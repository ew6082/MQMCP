/*
 * MQMCP - Model Context Protocol server for MacroQuest
 *
 * Exposes EverQuest game state as MCP resources and accepts tool calls
 * to control the character. Runs an HTTP server on localhost, one port per client:
 * the character's pinned port (/mcp port <n>), else the first free one from 8284.
 *
 * MCP endpoint:  POST http://localhost:<port>/mcp  (JSON-RPC 2.0)
 * Debug REST:    GET  http://localhost:<port>/me|zone|target|spawns
 */

// cpp-httplib requires a Windows 10 API level (CreateFile2, etc.), but MQ's
// Common.props pins _WIN32_WINNT=0x0601 (Win7). Override it BEFORE any include
// (windows.h must see 0x0A00 to declare the newer APIs). #undef first so there
// is no macro-redefinition warning against the command-line define.
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0A00
#ifdef WINVER
#undef WINVER
#endif
#define WINVER 0x0A00

// winsock2.h must be included before windows.h (which mq/Plugin.h pulls in)
#include <winsock2.h>
#include <ws2tcpip.h>

// Include cpp-httplib BEFORE mq/Plugin.h. Plugin.h's pch.h forces
// _WIN32_WINNT=0x0601 and pulls in windows.h, which hides the Win10 APIs
// (CreateFile2, CreateFileMappingFromApp, ...) that httplib needs. Including it
// first processes its windows.h at 0x0A00 (set above); windows.h's include
// guard then makes MQ's later include a no-op.
#define CPPHTTPLIB_NO_EXCEPTIONS
#include <httplib.h>

#include <mq/Plugin.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

PreSetup("MQMCP");
PLUGIN_VERSION(1.0);

using json = nlohmann::json;

// Every boxed client loads this plugin, so each needs its own port. A client takes its
// pinned port from [Ports] <CharName>=<port> in the plugin ini when it has one (set with
// /mcp port <n>), otherwise the first free port in k_defaultPort..+k_portSpan-1.
static constexpr int k_defaultPort = 8284;
static constexpr int k_portSpan    = 32;
static std::atomic<int>  s_port{0};          // 0 = not listening
static std::atomic<bool> s_stopping{false};  // tells long-lived handlers (SSE) to bail
static std::chrono::steady_clock::time_point s_startTime;

//============================================================================
// Thread-safe command queue — written by HTTP thread, drained on game pulse
//============================================================================

static std::queue<std::string> s_commandQueue;
static std::mutex              s_queueMutex;

static void EnqueueCommand(std::string cmd)
{
	std::lock_guard lock(s_queueMutex);
	s_commandQueue.push(std::move(cmd));
}

//============================================================================
// TLO query queue — HTTP thread -> game thread -> HTTP thread (synchronous)
//
// Unlike commands, a TLO query has to hand a value *back*, so this cannot be
// fire-and-forget. The HTTP thread pushes a request holding a promise and then
// blocks on its future; OnPulse evaluates on the game thread (the only place
// touching MQ's parser is safe) and fulfills it.
//
// ParseMacroData must not be called from the HTTP thread: it walks live client
// structures that the game thread mutates every frame.
//============================================================================

// A request carries a *list* of expressions so a caller can read many values
// from one consistent game frame. Evaluating them one per request would cost a
// round trip and a pulse each, and worse, would sample different frames -- so
// position, target and distance could disagree with one another.
struct TloQuery
{
	std::vector<std::string>               exprs;
	std::promise<std::vector<std::string>> result;
};

static std::queue<std::shared_ptr<TloQuery>> s_tloQueue;
static std::mutex                            s_tloMutex;

// Guards against one request monopolising a frame; MQ's parser is not free and
// OnPulse blocks rendering.
static constexpr size_t k_maxTloBatch = 64;

//============================================================================
// Chat ring buffer — fed by OnWriteChatColor, read by HTTP thread
//============================================================================

static constexpr size_t k_chatBufferSize  = 4000;
static constexpr size_t k_actionChatLines = 20;   // chat tail sent back with action tools
static std::deque<std::string> s_chatLines;
static std::mutex              s_chatMutex;

static void PushChatLine(const char* line)
{
	// Strip MQ color codes (0x07 + letter) and non-ASCII bytes so the
	// result is valid UTF-8 and safe to embed in JSON.
	std::string clean;
	for (const char* p = line; *p; ++p)
	{
		unsigned char c = static_cast<unsigned char>(*p);
		if (c == 0x07 && *(p + 1)) { ++p; continue; }  // MQ color: skip 2 bytes
		if (c >= 0x80) continue;                        // non-ASCII: skip
		clean += *p;
	}

	std::lock_guard lock(s_chatMutex);
	s_chatLines.push_back(std::move(clean));
	if (s_chatLines.size() > k_chatBufferSize)
		s_chatLines.pop_front();
}

//============================================================================
// Cached game state — written on pulse (main thread), read by HTTP thread
//============================================================================

struct SpawnSnapshot
{
	int         id;
	std::string name;
	std::string type;
	int         level;
	float       x, y, z;
	float       distance;
};

struct GameSnapshot
{
	bool inGame = false;

	// Player
	std::string playerName;
	std::string playerClass;
	int         playerLevel   = 0;
	int         playerClassId = 0;
	float       playerX = 0, playerY = 0, playerZ = 0;
	bool        isMoving      = false;   // true if position changed since last snapshot
	bool        isInvisible   = false;
	int64_t     hp = 0,  hpMax = 0;
	int         mana = 0, manaMax = 0;
	int         endurance = 0, enduranceMax = 0;

	// Zone
	std::string zoneLongName;
	std::string zoneShortName;
	int         zoneId = 0;

	// Target
	bool        hasTarget   = false;
	std::string targetName;
	int         targetId    = 0;
	std::string targetType;
	float       targetDist  = 0.f;
	int64_t     targetHp    = 0;
	int64_t     targetHpMax = 0;
	int         targetLevel = 0;

	// Nearby spawns (within 300 units, capped at 60, sorted by distance)
	std::vector<SpawnSnapshot> spawns;

	// AdvLoot items available to loot
	struct LootEntry {
		std::string name;
		bool        noDrop    = false;
		bool        stackable = false;
		int         stackSize = 1;
		bool        personal  = false;  // true = personal loot, false = shared
	};
	std::vector<LootEntry> lootItems;

	// Memorized spells (gem slots 0-based)
	struct SpellGem {
		int         gem         = 0;
		int         spellId     = -1;
		std::string name;
		uint32_t    msRemaining = 0;   // recast timer; read here so the HTTP thread never does
	};
	std::vector<SpellGem> memSpells;

	// Extended targets (XTarget window) — active aggressors/haters
	struct XTargetEntry {
		std::string name;
		int         spawnId  = 0;
		float       distance = 0.f;
		int         level    = 0;
	};
	std::vector<XTargetEntry> xTargets;   // only occupied, in-zone slots
	int xTargetCount = 0;                 // total occupied slots

	// Group members
	struct GroupMember {
		std::string name;
		std::string ownerName;   // mercenary owner
		int         level    = 0;
		int         type     = 0;  // EQP_PC=0, mercenary etc
		bool        offline  = false;
		bool        isLeader = false;
		int         hpPct    = 0;  // 0 if no spawn data
		int         manaPct  = 0;
	};
	std::vector<GroupMember> groupMembers;

	// Active buffs on self (long buffs + songs/disciplines)
	struct BuffEntry {
		int         spellId  = 0;
		std::string name;
		int         duration = 0;   // remaining ticks
		bool        song     = false;
	};
	std::vector<BuffEntry> buffs;

	// Visible buffs/debuffs on current target
	std::vector<BuffEntry> targetBuffs;
};

static GameSnapshot s_snapshot;
static std::mutex   s_snapshotMutex;
// OFF by default: the snapshot reads PlayerClient/spawn fields directly, which crash
// when eqlib's struct offsets lag an EQ patch. The server still serves execute_command,
// query_tlo and chat. /mcp snapshot on|off toggles it ([Settings] Snapshot in the ini).
static bool         s_snapshotEnabled = false;

static const char* SpawnTypeName(int type)
{
	switch (type)
	{
	case SPAWN_PLAYER: return "PC";
	case SPAWN_NPC:    return "NPC";
	case SPAWN_CORPSE: return "Corpse";
	default:           return "Unknown";
	}
}

static void UpdateSnapshot()
{
	// Struct field reads below are skipped unless s_snapshotEnabled (see its comment).
	GameSnapshot snap;
	// GetGameState()/pLocalPlayer-null are safe (not struct field reads), so always set
	// inGame accurately — the command queue, chat reader and resources gate on it.
	snap.inGame = (GetGameState() == GAMESTATE_INGAME && pLocalPlayer != nullptr);

	if (!s_snapshotEnabled || !snap.inGame)
	{
		std::lock_guard lock(s_snapshotMutex);
		s_snapshot = snap;   // inGame preserved; struct field reads skipped (avoids crash)
		return;
	}

	// ---- Player ----
	snap.playerName    = pLocalPlayer->Name;
	snap.playerLevel   = pLocalPlayer->Level;
	snap.playerClassId = static_cast<int>(pLocalPlayer->GetClass());
	if (const char* cls = pLocalPlayer->GetClassString())
		snap.playerClass = cls;
	snap.playerX       = pLocalPlayer->X;
	snap.playerY       = pLocalPlayer->Y;
	snap.playerZ       = pLocalPlayer->Z;
	snap.isMoving      = (snap.playerX != s_snapshot.playerX ||
	                      snap.playerY != s_snapshot.playerY ||
	                      snap.playerZ != s_snapshot.playerZ);
	snap.isInvisible   = (pLocalPlayer->HideMode != 0);
	snap.hp            = pLocalPlayer->HPCurrent;
	snap.hpMax         = pLocalPlayer->HPMax;
	snap.mana          = GetCurMana();
	snap.manaMax       = GetMaxMana();
	snap.endurance     = GetCurEndurance();
	snap.enduranceMax  = GetMaxEndurance();

	// ---- Zone ----
	if (pZoneInfo)
	{
		snap.zoneLongName  = pZoneInfo->LongName;
		snap.zoneShortName = pZoneInfo->ShortName;
		snap.zoneId        = pZoneInfo->ZoneID & 0x7FFF;
	}

	// ---- Target ----
	if (pTarget)
	{
		snap.hasTarget   = true;
		snap.targetName  = pTarget->Name;
		snap.targetId    = pTarget->SpawnID;
		snap.targetLevel = pTarget->Level;
		snap.targetHp    = pTarget->HPCurrent;
		snap.targetHpMax = pTarget->HPMax;
		snap.targetType  = SpawnTypeName(pTarget->Type);
		snap.targetDist  = Distance3DToSpawn(pLocalPlayer, pTarget);
	}

	// ---- Nearby spawns ----
	for (SPAWNINFO* pSpawn = (SPAWNINFO*)pSpawnList; pSpawn; pSpawn = pSpawn->pNext)
	{
		if (pSpawn == pLocalPlayer)
			continue;

		float dist = Distance3DToSpawn(pLocalPlayer, pSpawn);
		if (dist > 300.f)
			continue;

		SpawnSnapshot ss;
		ss.id       = pSpawn->SpawnID;
		ss.name     = pSpawn->Name;
		ss.level    = pSpawn->Level;
		ss.type     = SpawnTypeName(pSpawn->Type);
		ss.x        = pSpawn->X;
		ss.y        = pSpawn->Y;
		ss.z        = pSpawn->Z;
		ss.distance = dist;

		snap.spawns.push_back(ss);
	}

	std::sort(snap.spawns.begin(), snap.spawns.end(),
	          [](const SpawnSnapshot& a, const SpawnSnapshot& b) { return a.distance < b.distance; });

	if (snap.spawns.size() > 60)
		snap.spawns.resize(60);

	// ---- AdvLoot items ----
	if (pAdvancedLootWnd)
	{
		auto collectItems = [&](AdvancedLootItemList* list, bool personal) {
			if (!list) return;
			int count = list->Items.GetSize();
			for (int i = 0; i < count; ++i)
			{
				const AdvancedLootItem& li = list->Items[i];
				if (li.Name[0] == '\0') continue;

				GameSnapshot::LootEntry entry;
				entry.name      = li.Name;
				entry.noDrop    = li.NoDrop;
				entry.stackable = li.bStackable;
				entry.stackSize = li.bStackable ? li.MaxStack : 1;
				entry.personal  = personal;
				snap.lootItems.push_back(entry);
			}
		};

		collectItems(pAdvancedLootWnd->pPLootList, true);
		collectItems(pAdvancedLootWnd->pCLootList, false);
	}

	// ---- Memorized spells ----
	if (PcProfile* pProfile = GetPcProfile())
	{
		for (int gem = 0; gem < MAX_MEMORIZED_SPELLS; ++gem)
		{
			int spellId = pProfile->GetMemorizedSpell(gem);
			if (spellId <= 0)
				continue;

			GameSnapshot::SpellGem sg;
			sg.gem         = gem;
			sg.spellId     = spellId;
			sg.msRemaining = GetSpellGemTimer(gem);

			if (EQ_Spell* pSpell = GetSpellByID(spellId))
				sg.name = pSpell->Name;

			snap.memSpells.push_back(sg);
		}
	}

	// ---- Group members ----
	if (pLocalPC && pLocalPC->Group)
	{
		CGroup* pGrp = pLocalPC->Group;
		for (int i = 0; i < (int)pGrp->GetMaxGroupSize(); ++i)
		{
			CGroupMember* pMem = pGrp->GetGroupMember(i);
			if (!pMem || pMem->Name.empty()) continue;

			GameSnapshot::GroupMember gm;
			gm.name      = pMem->Name.c_str();
			gm.ownerName = pMem->OwnerName.c_str();
			gm.level     = pMem->Level;
			gm.type      = pMem->Type;
			gm.offline   = pMem->bIsOffline;
			gm.isLeader  = (pMem == pGrp->GetGroupLeader());

			if (PlayerClient* pSpawn = pMem->pPlayer)
			{
				int64_t hp  = pSpawn->HPCurrent, hpMax = pSpawn->HPMax;
				gm.hpPct   = hpMax > 0 ? static_cast<int>(hp * 100 / hpMax) : 0;
				int mana = 0, manaMax = 0;
				if (pMem->pPlayer == pLocalPlayer)
				{
					mana    = GetCurMana();
					manaMax = GetMaxMana();
				}
				else
				{
					mana    = pSpawn->GetCurrentMana();
					manaMax = pSpawn->GetMaxMana();
				}
				gm.manaPct = manaMax > 0 ? mana * 100 / manaMax : 0;
			}

			snap.groupMembers.push_back(gm);
		}
	}

	// ---- Self buffs (long + songs/disciplines) ----
	if (pLocalPC)
	{
		auto collectBuffs = [&](auto&& getEffect, int count, bool isSong) {
			for (int i = 0; i < count; ++i)
			{
				const EQ_Affect& affect = getEffect(i);
				if (affect.SpellID <= 0) continue;

				GameSnapshot::BuffEntry entry;
				entry.spellId  = affect.SpellID;
				entry.duration = affect.Duration;
				entry.song     = isSong;

				if (EQ_Spell* pSpell = GetSpellByID(affect.SpellID))
					entry.name = pSpell->Name;

				snap.buffs.push_back(entry);
			}
		};

		collectBuffs([&](int i) -> const EQ_Affect& { return pLocalPC->GetEffect(i); },    NUM_LONG_BUFFS, false);
		collectBuffs([&](int i) -> const EQ_Affect& { return pLocalPC->GetTempEffect(i); }, NUM_TEMP_BUFFS, true);
	}

	// ---- Target buffs (visible effects on target, PC targets only) ----
	if (pTarget && pTarget->Type == SPAWN_PLAYER)
	{
		// Only PC targets have a PcClient buff list accessible this way
		if (PcClient* pTargetPC = pTarget->GetPcClient())
		{
			for (int i = 0; i < NUM_LONG_BUFFS; ++i)
			{
				const EQ_Affect& affect = pTargetPC->GetEffect(i);
				if (affect.SpellID <= 0) continue;

				GameSnapshot::BuffEntry entry;
				entry.spellId  = affect.SpellID;
				entry.duration = affect.Duration;
				entry.song     = false;

				if (EQ_Spell* pSpell = GetSpellByID(affect.SpellID))
					entry.name = pSpell->Name;

				snap.targetBuffs.push_back(entry);
			}
		}
	}

	// ---- Extended targets (XTarget window) ----
	if (pLocalPC && pLocalPC->pXTargetMgr)
	{
		ExtendedTargetList* xtm = pLocalPC->pXTargetMgr;
		int numSlots = xtm->GetNumSlots();
		for (int i = 0; i < numSlots; ++i)
		{
			ExtendedTargetSlot* xts = xtm->GetSlot(i);
			if (!xts || xts->XTargetSlotStatus != eXTSlotCurrentZone || xts->SpawnID == 0)
				continue;

			++snap.xTargetCount;

			GameSnapshot::XTargetEntry entry;
			entry.spawnId = xts->SpawnID;
			entry.name    = xts->Name;

			if (SPAWNINFO* pSpawn = GetSpawnByID(xts->SpawnID))
			{
				entry.level    = pSpawn->Level;
				entry.distance = Distance3DToSpawn(pLocalPlayer, pSpawn);
			}

			snap.xTargets.push_back(entry);
		}
	}

	std::lock_guard lock(s_snapshotMutex);
	s_snapshot = std::move(snap);
}

//============================================================================
// JSON builders
//============================================================================

static json BuildPlayerJson(const GameSnapshot& s)
{
	int hpPct = s.hpMax  > 0 ? static_cast<int>(s.hp  * 100 / s.hpMax)  : 0;
	int mnPct = s.manaMax > 0 ? s.mana * 100 / s.manaMax : 0;

	return {
		{"name",        s.playerName},
		{"level",       s.playerLevel},
		{"classId",     s.playerClassId},
		{"invisible",   s.isInvisible},
		{"position",    {{"x", s.playerX}, {"y", s.playerY}, {"z", s.playerZ}}},
		{"hp",          {{"current", s.hp},        {"max", s.hpMax},       {"pct", hpPct}}},
		{"mana",        {{"current", s.mana},       {"max", s.manaMax},     {"pct", mnPct}}},
		{"endurance",   {{"current", s.endurance},  {"max", s.enduranceMax}}}
	};
}

static json BuildZoneJson(const GameSnapshot& s)
{
	return {
		{"longName",  s.zoneLongName},
		{"shortName", s.zoneShortName},
		{"id",        s.zoneId}
	};
}

static json BuildTargetJson(const GameSnapshot& s)
{
	if (!s.hasTarget)
		return nullptr;

	json tBuffs = json::array();
	for (const GameSnapshot::BuffEntry& b : s.targetBuffs)
		tBuffs.push_back({{"name", b.name}, {"id", b.spellId}, {"duration", b.duration}});

	return {
		{"name",     s.targetName},
		{"id",       s.targetId},
		{"type",     s.targetType},
		{"level",    s.targetLevel},
		{"distance", s.targetDist},
		{"hp",       {{"current", s.targetHp}, {"max", s.targetHpMax}}},
		{"buffs",    tBuffs}
	};
}

// Compact state summary returned alongside every tool response so the LLM
// always has situational awareness after acting. maxChat caps recent_chat to the
// newest lines: action responses must not drag the whole 4000-line buffer along.
static json BuildStateJson(const GameSnapshot& s, size_t maxChat = k_chatBufferSize)
{
	if (!s.inGame)
		return {{"inGame", false}};

	int hpPct  = s.hpMax  > 0 ? static_cast<int>(s.hp  * 100 / s.hpMax)  : 0;
	int mnPct  = s.manaMax > 0 ? s.mana * 100 / s.manaMax : 0;
	int endPct = s.enduranceMax > 0 ? s.endurance * 100 / s.enduranceMax : 0;

	// Extended targets (confirmed aggressors in XTarget window)
	json xtargets = json::array();
	for (const GameSnapshot::XTargetEntry& xt : s.xTargets)
		xtargets.push_back({{"name", xt.name}, {"level", xt.level}, {"distance", xt.distance}});

	// Recent chat (the newest maxChat buffered lines)
	json chat = json::array();
	{
		std::lock_guard chatLock(s_chatMutex);
		size_t skip = s_chatLines.size() > maxChat ? s_chatLines.size() - maxChat : 0;
		for (auto it = s_chatLines.begin() + skip; it != s_chatLines.end(); ++it)
			chat.push_back(*it);
	}

	// Memorized spells + recast readiness
	json spells = json::array();
	for (const GameSnapshot::SpellGem& sg : s.memSpells)
		spells.push_back({{"gem", sg.gem}, {"name", sg.name}, {"id", sg.spellId},
		                  {"ready", sg.msRemaining == 0}, {"ms_remaining", sg.msRemaining}});

	// Loot available
	json loot = json::array();
	for (const GameSnapshot::LootEntry& le : s.lootItems)
		loot.push_back({{"name", le.name}, {"noDrop", le.noDrop}, {"personal", le.personal}});

	// Active buffs on self
	json buffs = json::array();
	for (const GameSnapshot::BuffEntry& b : s.buffs)
		buffs.push_back({{"name", b.name}, {"id", b.spellId}, {"duration", b.duration}, {"song", b.song}});

	json state = {
		{"inGame",         true},
		{"name",           s.playerName},
		{"level",          s.playerLevel},
		{"class",          s.playerClass},
		{"hp_pct",         hpPct},
		{"mana_pct",       mnPct},
		{"endurance_pct",  endPct},
		{"moving",         s.isMoving},
		{"invisible",      s.isInvisible},
		{"position",       {{"x", s.playerX}, {"y", s.playerY}, {"z", s.playerZ}}},
		{"zone",           s.zoneShortName},
		{"group_size",     static_cast<int>(s.groupMembers.size())},
		{"aggro_count",    s.xTargetCount},
		{"aggressors",     xtargets},
		{"mem_spells",     spells},
		{"active_buffs",   buffs},
		{"loot_available", loot},
		{"recent_chat",    chat}
	};

	if (s.hasTarget)
	{
		int tHpPct = s.targetHpMax > 0 ? static_cast<int>(s.targetHp * 100 / s.targetHpMax) : 0;
		json tBuffs = json::array();
		for (const GameSnapshot::BuffEntry& b : s.targetBuffs)
			tBuffs.push_back({{"name", b.name}, {"id", b.spellId}});
		state["target"] = {
			{"name",     s.targetName},
			{"type",     s.targetType},
			{"distance", s.targetDist},
			{"hp_pct",   tHpPct},
			{"buffs",    tBuffs}
		};
	}

	return state;
}

static json BuildSpawnsJson(const GameSnapshot& s)
{
	json arr = json::array();
	for (const SpawnSnapshot& sp : s.spawns)
	{
		arr.push_back({
			{"id",       sp.id},
			{"name",     sp.name},
			{"type",     sp.type},
			{"level",    sp.level},
			{"distance", sp.distance},
			{"position", {{"x", sp.x}, {"y", sp.y}, {"z", sp.z}}}
		});
	}
	return arr;
}

//============================================================================
// MCP JSON-RPC 2.0 handler
//============================================================================

static json MakeOk(const json& id, json result)
{
	return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

static json MakeError(const json& id, int code, std::string msg)
{
	return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", std::move(msg)}}}};
}

static json HandleMCP(const json& req)
{
	const std::string method = req.value("method", "");
	const json        id     = req.contains("id") ? req.at("id") : json(nullptr);

	// Notifications have no id and require no response
	if (id.is_null() && method.find("notifications/") == 0)
		return {};

	if (method == "initialize")
	{
		return MakeOk(id, {
			{"protocolVersion", "2024-11-05"},
			{"capabilities",    {{"resources", json::object()}, {"tools", json::object()}}},
			{"serverInfo",      {{"name", "MQMCP"}, {"version", "1.0"}}}
		});
	}

	if (method == "resources/list")
	{
		return MakeOk(id, {{"resources", {
			{{"uri","eq://me"},     {"name","Player State"},    {"description","Your character's current state"},          {"mimeType","application/json"}},
			{{"uri","eq://zone"},   {"name","Zone Info"},       {"description","Current zone name and ID"},                {"mimeType","application/json"}},
			{{"uri","eq://target"}, {"name","Current Target"},  {"description","Your currently selected target"},          {"mimeType","application/json"}},
			{{"uri","eq://spawns"}, {"name","Nearby Spawns"},   {"description","All spawns within 300 units"},             {"mimeType","application/json"}},
			{{"uri","eq://spells"}, {"name","Memorized Spells"},{"description","Spells currently memorized in gem slots"},    {"mimeType","application/json"}},
			{{"uri","eq://chat"},   {"name","Recent Chat"},     {"description","Buffered chat and MQ output (up to 4000 lines)"},    {"mimeType","application/json"}},
			{{"uri","eq://loot"},   {"name","Available Loot"},  {"description","Items currently in the AdvLoot window"},          {"mimeType","application/json"}},
			{{"uri","eq://group"},     {"name","Group Members"},   {"description","Current group members with HP, level, and role"},             {"mimeType","application/json"}},
			{{"uri","eq://buffs"},     {"name","Active Buffs"},    {"description","Buffs and songs currently active on your character"},         {"mimeType","application/json"}},
			{{"uri","eq://cooldowns"}, {"name","Spell Cooldowns"}, {"description","Spell gem recast timers — which gems are ready vs on cooldown"}, {"mimeType","application/json"}}
		}}});
	}

	if (method == "resources/read")
	{
		const std::string uri = req.value("/params/uri"_json_pointer, "");

		std::lock_guard lock(s_snapshotMutex);
		if (!s_snapshot.inGame)
			return MakeError(id, -32000, "Not in game");

		json content;
		if      (uri == "eq://me")     content = BuildPlayerJson(s_snapshot);
		else if (uri == "eq://zone")   content = BuildZoneJson(s_snapshot);
		else if (uri == "eq://target") content = BuildTargetJson(s_snapshot);
		else if (uri == "eq://spawns") content = BuildSpawnsJson(s_snapshot);
		else if (uri == "eq://spells")
		{
			json arr = json::array();
			for (const GameSnapshot::SpellGem& sg : s_snapshot.memSpells)
				arr.push_back({{"gem", sg.gem}, {"name", sg.name}, {"id", sg.spellId}});
			content = arr;
		}
		else if (uri == "eq://chat")
		{
			json arr = json::array();
			std::lock_guard chatLock(s_chatMutex);
			for (const std::string& line : s_chatLines)
				arr.push_back(line);
			content = arr;
		}
		else if (uri == "eq://loot")
		{
			json arr = json::array();
			for (const GameSnapshot::LootEntry& le : s_snapshot.lootItems)
				arr.push_back({
					{"name",      le.name},
					{"noDrop",    le.noDrop},
					{"stackable", le.stackable},
					{"stackSize", le.stackSize},
					{"personal",  le.personal}
				});
			content = arr;
		}
		else if (uri == "eq://group")
		{
			json arr = json::array();
			for (const GameSnapshot::GroupMember& gm : s_snapshot.groupMembers)
				arr.push_back({
					{"name",     gm.name},
					{"owner",    gm.ownerName},
					{"level",    gm.level},
					{"type",     gm.type},
					{"offline",  gm.offline},
					{"leader",   gm.isLeader},
					{"hp_pct",   gm.hpPct},
					{"mana_pct", gm.manaPct}
				});
			content = arr;
		}
		else if (uri == "eq://buffs")
		{
			json arr = json::array();
			for (const GameSnapshot::BuffEntry& b : s_snapshot.buffs)
				arr.push_back({
					{"name",     b.name},
					{"id",       b.spellId},
					{"duration", b.duration},
					{"song",     b.song}
				});
			content = arr;
		}
		else if (uri == "eq://cooldowns")
		{
			json arr = json::array();
			for (const GameSnapshot::SpellGem& sg : s_snapshot.memSpells)
				arr.push_back({
					{"gem",          sg.gem},
					{"name",         sg.name},
					{"id",           sg.spellId},
					{"ready",        sg.msRemaining == 0},
					{"ms_remaining", sg.msRemaining}
				});
			content = arr;
		}
		else return MakeError(id, -32602, "Unknown resource URI: " + uri);

		return MakeOk(id, {{"contents", {{
			{"uri",      uri},
			{"mimeType", "application/json"},
			{"text",     content.dump()}
		}}}});
	}

	if (method == "tools/list")
	{
		return MakeOk(id, {{"tools", {
			{
				{"name",        "get_state"},
				{"description", "Get current game state: name, level, class, HP%, mana%, endurance%, movement, zone, target, buffs, spells, loot, and recent chat. Call this after any action to check the result."},
				{"inputSchema", {{"type","object"},{"properties",json::object()}}}
			},
			{
				{"name",        "execute_command"},
				{"description", "Execute any MacroQuest slash command (e.g. /say hello, /cast \"Complete Heal\")"},
				{"inputSchema", {
					{"type",       "object"},
					{"properties", {{"command", {{"type","string"},{"description","The full command including the leading slash"}}}}},
					{"required",   {"command"}}
				}}
			},
			{
				{"name",        "navigate_to"},
				{"description", "Navigate to a world location using MQ2Nav. Requires MQ2Nav to be loaded."},
				{"inputSchema", {
					{"type",       "object"},
					{"properties", {
						{"y", {{"type","number"},{"description","North/South (Y) coordinate"}}},
						{"x", {{"type","number"},{"description","East/West (X) coordinate"}}},
						{"z", {{"type","number"},{"description","Up/Down (Z) coordinate, optional"}}}
					}},
					{"required",   {"y", "x"}}
				}}
			},
			{
				{"name",        "say"},
				{"description", "Say text in local /say chat"},
				{"inputSchema", {
					{"type",       "object"},
					{"properties", {{"text", {{"type","string"},{"description","Text to say"}}}}},
					{"required",   {"text"}}
				}}
			},
			{
				{"name",        "query_tlo"},
				{"description", "Evaluate a MacroQuest TLO expression and return its value as a string. "
				                "Accepts either a bare expression (Me.Name, Navigation.Setting[OpenDoors]) "
				                "or a full macro string with ${} (e.g. \"${Me.Name} is level ${Me.Level}\"). "
				                "Unlike get_state this reads live via MQ's parser, so it works even when the "
				                "state snapshot is disabled. Note TLO index syntax uses [brackets], not (parens): "
				                "Navigation.Setting[OpenDoors], not Navigation.Setting(OpenDoors). "
				                "Returns JSON with the value and a 'resolved' flag that is false when the TLO "
				                "did not evaluate (MQ yields NULL or echoes the expression back)."},
				{"inputSchema", {
					{"type",       "object"},
					{"properties", {{"expr", {{"type","string"},{"description","TLO expression, with or without surrounding ${}"}}}}},
					{"required",   {"expr"}}
				}}
			},
			{
				{"name",        "query_tlo_batch"},
				{"description", "Evaluate several TLO expressions at once, all on the SAME game frame. "
				                "Prefer this over repeated query_tlo calls: each single call costs a network "
				                "round trip plus a game pulse, and values read from different frames can "
				                "disagree (position vs target vs distance). Same syntax and 'resolved' "
				                "semantics as query_tlo, one result object per expression in request order. "
				                "Example: [\"Me.Y\",\"Me.X\",\"Target.CleanName\",\"Target.Distance\"]. "
				                "Limit 64 expressions per call."},
				{"inputSchema", {
					{"type",       "object"},
					{"properties", {{"exprs", {
						{"type","array"},
						{"items", {{"type","string"}}},
						{"description","TLO expressions, each with or without surrounding ${}"}
					}}}},
					{"required",   {"exprs"}}
				}}
			}
		}}});
	}

	if (method == "tools/call")
	{
		const std::string tool = req.value("/params/name"_json_pointer, "");
		const json        args = req.value("/params/arguments"_json_pointer, json::object());

		// Helper: build a response that includes current game state. The command has
		// only been queued, so its own output is not in chat yet; a short tail is enough.
		auto respondWithState = [&](std::string msg) {
			std::lock_guard lock(s_snapshotMutex);
			json state = BuildStateJson(s_snapshot, k_actionChatLines);
			std::string text = msg + "\n\nCurrent state:\n" + state.dump(2);
			return MakeOk(id, {{"content", {{{"type","text"},{"text", std::move(text)}}}}});
		};

		if (tool == "get_state")
		{
			std::lock_guard lock(s_snapshotMutex);
			json state = BuildStateJson(s_snapshot);
			return MakeOk(id, {{"content", {{{"type","text"},{"text", state.dump(2)}}}}});
		}

		if (tool == "execute_command")
		{
			std::string cmd = args.value("command", "");
			if (cmd.empty())
				return MakeError(id, -32602, "command is required");
			EnqueueCommand(cmd);
			return respondWithState("Queued: " + cmd);
		}

		if (tool == "navigate_to")
		{
			auto toFloat = [](const json& v, float def = 0.f) -> float {
				if (v.is_number()) return v.get<float>();
				if (v.is_string()) { try { return std::stof(v.get<std::string>()); } catch (...) {} }
				return def;
			};
			float y = args.contains("y") ? toFloat(args["y"]) : 0.f;
			float x = args.contains("x") ? toFloat(args["x"]) : 0.f;
			float z = args.contains("z") ? toFloat(args["z"]) : 0.f;
			std::string cmd = fmt::format("/nav loc {:.2f} {:.2f} {:.2f}", y, x, z);
			EnqueueCommand(cmd);
			return respondWithState("Navigating: " + cmd);
		}

		if (tool == "say")
		{
			std::string text = args.value("text", "");
			if (text.empty())
				return MakeError(id, -32602, "text is required");
			EnqueueCommand("/say " + text);
			return respondWithState("Said: " + text);
		}

		// Accept a bare TLO for convenience; wrap it so the parser sees a macro
		// expression. Anything already containing ${ passes through untouched,
		// which is what allows composite strings like "${Me.Name} ${Me.Level}".
		auto wrapExpr = [](const std::string& e) {
			return (e.find("${") == std::string::npos) ? "${" + e + "}" : e;
		};

		// MQ reports a failed lookup either as the literal "NULL" or by leaving the
		// expression untouched. Surfacing that beats returning "NULL" as if it were
		// a real value -- a caller checking a boolean TLO would otherwise read a
		// non-empty string and conclude true.
		auto isResolved = [](const std::string& value, const std::string& expr) {
			return value != "NULL" && value != expr;
		};

		// Submit expressions to the game thread and wait for one frame's worth of
		// answers. Returns false and fills 'err' on timeout or abandonment.
		auto evaluateOnGameThread = [&](const std::vector<std::string>& exprs,
		                                std::vector<std::string>& out,
		                                std::string& err) -> bool {
			auto q   = std::make_shared<TloQuery>();
			q->exprs = exprs;
			auto fut = q->result.get_future();
			{
				std::lock_guard lock(s_tloMutex);
				s_tloQueue.push(q);
			}

			// The game thread fulfills this on the next pulse (~100ms). A timeout
			// means pulses are not running: char select, frozen client, or shutdown.
			// Far better to say so than to block the HTTP thread indefinitely.
			if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
			{
				err = "Timed out waiting for the game thread (is the client pulsing?)";
				return false;
			}
			try { out = fut.get(); }
			catch (...) { err = "TLO evaluation was abandoned (plugin unloading?)"; return false; }
			return true;
		};

		if (tool == "query_tlo")
		{
			const std::string expr = args.value("expr", "");
			if (expr.empty())
				return MakeError(id, -32602, "expr is required");

			const std::string parsed = wrapExpr(expr);
			std::vector<std::string> values;
			std::string err;
			if (!evaluateOnGameThread({parsed}, values, err) || values.empty())
				return MakeError(id, -32000, err.empty() ? "No result returned" : err);

			json out = {
				{"expr",     parsed},
				{"value",    values[0]},
				{"resolved", isResolved(values[0], parsed)}
			};
			return MakeOk(id, {{"content", {{{"type","text"},{"text", out.dump(2)}}}}});
		}

		if (tool == "query_tlo_batch")
		{
			if (!args.contains("exprs") || !args["exprs"].is_array())
				return MakeError(id, -32602, "exprs must be an array of strings");

			std::vector<std::string> raw, wrapped;
			for (const auto& e : args["exprs"])
			{
				if (!e.is_string())
					return MakeError(id, -32602, "every entry in exprs must be a string");
				raw.push_back(e.get<std::string>());
				wrapped.push_back(wrapExpr(raw.back()));
			}

			if (raw.empty())
				return MakeError(id, -32602, "exprs must not be empty");
			if (raw.size() > k_maxTloBatch)
				return MakeError(id, -32602,
					fmt::format("too many expressions ({}); the limit is {} per call",
					            raw.size(), k_maxTloBatch));

			std::vector<std::string> values;
			std::string err;
			if (!evaluateOnGameThread(wrapped, values, err))
				return MakeError(id, -32000, err);
			if (values.size() != wrapped.size())
				return MakeError(id, -32000, "Result count did not match request count");

			json results = json::array();
			for (size_t i = 0; i < values.size(); ++i)
			{
				results.push_back({
					{"expr",     raw[i]},
					{"value",    values[i]},
					{"resolved", isResolved(values[i], wrapped[i])}
				});
			}

			json out = {{"count", results.size()}, {"results", results}};
			return MakeOk(id, {{"content", {{{"type","text"},{"text", out.dump(2)}}}}});
		}

		return MakeError(id, -32601, "Unknown tool: " + tool);
	}

	return MakeError(id, -32601, "Method not found: " + method);
}

//============================================================================
// HTTP server
//============================================================================

static std::unique_ptr<httplib::Server> s_server;
// Raw pointer (not a global std::thread) on purpose: a global std::thread has a
// destructor that calls std::terminate() if still joinable. On an abnormal process
// exit MQ may not call ShutdownPlugin, yet DLL_PROCESS_DETACH still runs the CRT
// atexit destructors — a joinable global thread there => FAST_FAIL abort that masks
// the real exit. A raw pointer has a trivial destructor, so teardown can never abort.
static std::thread*                      s_serverThread = nullptr;

static void SetupRoutes()
{
	// Browsers attach Origin to cross-site requests; MCP clients (Claude Code, Python
	// scripts) do not. Refusing anything with an Origin stops a web page -- or a DNS
	// rebinding attack -- from driving the client through 127.0.0.1. No CORS headers
	// are sent either, so browsers also block preflighted requests on their own.
	s_server->set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
		if (!req.has_header("Origin"))
			return httplib::Server::HandlerResponse::Unhandled;
		res.status = 403;
		res.set_content(R"({"error":"browser requests are not allowed"})", "application/json");
		return httplib::Server::HandlerResponse::Handled;
	});

	// -----------------------------------------------------------------------
	// MCP endpoint — POST for JSON-RPC requests
	// -----------------------------------------------------------------------
	s_server->Post("/mcp", [](const httplib::Request& req, httplib::Response& res) {
		try
		{
			json body     = json::parse(req.body);
			json response = HandleMCP(body);

			if (response.is_null() || response.empty())
			{
				res.status = 204;
				return;
			}
			res.set_content(response.dump(), "application/json");
		}
		catch (const std::exception& e)
		{
			res.status = 400;
			res.set_content(
				json{{"jsonrpc","2.0"},{"id",nullptr},{"error",{{"code",-32700},{"message",std::string("Parse error: ") + e.what()}}}}.dump(),
				"application/json");
		}
	});

	// -----------------------------------------------------------------------
	// MCP endpoint — GET for SSE (server-sent events / streaming transport)
	// We don't push server-initiated messages, so we open the stream and keep
	// it alive with periodic comments. Claude Code requires this to exist.
	// -----------------------------------------------------------------------
	s_server->Get("/mcp", [](const httplib::Request&, httplib::Response& res) {
		res.set_header("Cache-Control",  "no-cache");
		res.set_header("X-Accel-Buffering", "no");

		res.set_chunked_content_provider(
			"text/event-stream",
			[](size_t /*offset*/, httplib::DataSink& sink) {
				// Send a keepalive comment every 15 seconds.
				// The loop exits when the client disconnects (sink.is_writable() == false)
				// or the server is stopping. The 15s wait is sliced so a stop -- which joins
				// this worker from the game thread -- is not held up by a sleeping stream.
				const std::string keepalive = ": keepalive\n\n";
				for (;;)
				{
					if (s_stopping || !sink.is_writable())
						return false;
					sink.write(keepalive.data(), keepalive.size());
					for (int i = 0; i < 60 && !s_stopping; ++i)
						std::this_thread::sleep_for(std::chrono::milliseconds(250));
				}
			}
		);
	});

	// -----------------------------------------------------------------------
	// OAuth discovery — return empty metadata so Claude Code skips OAuth
	// -----------------------------------------------------------------------
	s_server->Get("/.well-known/oauth-protected-resource", [](const httplib::Request&, httplib::Response& res) {
		// No auth required — return minimal valid metadata
		res.set_content(json{{"resource", fmt::format("http://127.0.0.1:{}", s_port.load())}}.dump(), "application/json");
	});

	s_server->Get("/.well-known/oauth-authorization-server", [](const httplib::Request&, httplib::Response& res) {
		res.status = 404;
		res.set_content(R"({"error":"not_found"})", "application/json");
	});

	// -----------------------------------------------------------------------
	// Convenience read-only REST endpoints (useful for debugging)
	// -----------------------------------------------------------------------
	auto gameGet = [](httplib::Response& res, auto buildFn) {
		std::lock_guard lock(s_snapshotMutex);
		if (!s_snapshot.inGame)
		{
			res.status = 503;
			res.set_content(R"({"error":"not in game"})", "application/json");
			return;
		}
		res.set_content(buildFn(s_snapshot).dump(2), "application/json");
	};

	s_server->Get("/me",     [gameGet](const httplib::Request&, httplib::Response& res) { gameGet(res, BuildPlayerJson); });
	s_server->Get("/zone",   [gameGet](const httplib::Request&, httplib::Response& res) { gameGet(res, BuildZoneJson);   });
	s_server->Get("/target", [gameGet](const httplib::Request&, httplib::Response& res) { gameGet(res, BuildTargetJson); });
	s_server->Get("/spawns", [gameGet](const httplib::Request&, httplib::Response& res) { gameGet(res, BuildSpawnsJson); });
}

//============================================================================
// Server lifecycle / port selection
//============================================================================

// httplib 0.15.3's default_socket_options sets SO_REUSEADDR and *then*
// SO_EXCLUSIVEADDRUSE on Windows. Winsock rejects that combination (WSAEINVAL on the
// second call, unchecked), leaving a plain REUSEADDR socket -- so every boxed client
// bound 8284 and the newest silently took over connections. Exclusive only, so a port
// already in use fails to bind and we move on to the next.
static void ExclusiveSocketOptions(socket_t sock)
{
	int yes = 1;
	setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof(yes));
}

static std::atomic<bool> s_listenDone{true};

static void StopServer()
{
	if (!s_server)
		return;

	s_stopping = true;
	s_server->stop();

	// Workers blocked in a TLO wait need OnPulse to answer them, but we are usually
	// *on* the game thread here (OnPulse, /mcp, ShutdownPlugin), so they would sit out
	// their 5s timeout and stall the client. Dropping the queued promises breaks them
	// immediately instead; keep doing it until the listener has drained its workers,
	// since a keep-alive connection can still enqueue between our sweeps.
	while (!s_listenDone)
	{
		{
			std::lock_guard lock(s_tloMutex);
			std::queue<std::shared_ptr<TloQuery>>().swap(s_tloQueue);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	if (s_serverThread)
	{
		if (s_serverThread->joinable())
			s_serverThread->join();
		delete s_serverThread;
		s_serverThread = nullptr;
	}
	s_server.reset();
	s_port = 0;
}

// Binds `preferred` if given and free, else the first free port in the scan range.
// Returns the bound port, or 0 if nothing in the range was free.
static int StartServer(int preferred)
{
	StopServer();

	s_server = std::make_unique<httplib::Server>();
	s_server->set_socket_options(ExclusiveSocketOptions);
	SetupRoutes();

	int port = 0;
	if (preferred > 0 && s_server->bind_to_port("127.0.0.1", preferred))
		port = preferred;
	for (int p = k_defaultPort; !port && p < k_defaultPort + k_portSpan; ++p)
		if (s_server->bind_to_port("127.0.0.1", p))
			port = p;

	if (!port)
	{
		s_server.reset();
		return 0;
	}

	s_stopping   = false;
	s_listenDone = false;
	s_port       = port;
	s_serverThread = new std::thread([] {
		s_server->listen_after_bind();
		s_listenDone = true;
	});
	// stop() is a no-op until the listener is running, so a stop racing a fresh start
	// would leave join() waiting forever. Don't return until it is up.
	s_server->wait_until_ready();
	return port;
}

static std::string CurrentCharName()
{
	if (GetGameState() != GAMESTATE_INGAME || !pLocalPlayer)
		return {};
	return pLocalPlayer->Name;
}

static int PinnedPort(const std::string& name)
{
	if (name.empty())
		return 0;
	return ::GetPrivateProfileIntA("Ports", name.c_str(), 0, INIFileName);
}

static void SavePinnedPort(const std::string& name, int port)
{
	// port 0 deletes the key (back to auto)
	::WritePrivateProfileStringA("Ports", name.c_str(),
		port > 0 ? std::to_string(port).c_str() : nullptr, INIFileName);
}

static void AnnouncePort(int wanted)
{
	int port = s_port;
	if (!port)
		WriteChatf("\ag[MQMCP]\ax \arNo free port in %d-%d -- server not running.\ax",
			k_defaultPort, k_defaultPort + k_portSpan - 1);
	else if (wanted > 0 && port != wanted)
		WriteChatf("\ag[MQMCP]\ax \arPinned port %d is in use\ax -- listening on \ayhttp://127.0.0.1:%d/mcp\ax instead", wanted, port);
	else
		WriteChatf("\ag[MQMCP]\ax MCP server listening on \ayhttp://127.0.0.1:%d/mcp\ax -- type \aw/mcp\ax for status", port);
}

// Move onto this character's pinned port, if it has one and we are not already on it.
static void ApplyPinnedPort()
{
	int pin = PinnedPort(CurrentCharName());
	if (pin <= 0 || pin == s_port)
		return;
	StartServer(pin);
	AnnouncePort(pin);
}

//============================================================================
// /mcp command — display server status
//============================================================================

static void Cmd_MCPStatus(PlayerClient* /*pChar*/, const char* szLine)
{
	// /mcp port <n>   pin this character to <n> and rebind now
	// /mcp port auto  drop the pin and rebind to the first free port
	char arg1[MAX_STRING] = { 0 };
	char arg2[MAX_STRING] = { 0 };
	GetArg(arg1, szLine, 1);
	GetArg(arg2, szLine, 2);

	// /mcp snapshot on|off   enable the struct-reading state snapshot (saved in the ini)
	if (ci_equals(arg1, "snapshot"))
	{
		if (ci_equals(arg2, "on") || ci_equals(arg2, "off"))
		{
			s_snapshotEnabled = ci_equals(arg2, "on");
			::WritePrivateProfileStringA("Settings", "Snapshot", s_snapshotEnabled ? "1" : "0", INIFileName);
		}
		WriteChatf("\ag[MQMCP]\ax Snapshot is \ay%s\ax   (/mcp snapshot on|off)", s_snapshotEnabled ? "on" : "off");
		return;
	}

	if (ci_equals(arg1, "port"))
	{
		std::string name = CurrentCharName();
		if (name.empty())
		{
			WriteChatf("\ag[MQMCP]\ax Ports are pinned per character -- log in first.");
			return;
		}
		if (ci_equals(arg2, "auto"))
		{
			SavePinnedPort(name, 0);
			StartServer(0);
			AnnouncePort(0);
			return;
		}
		int want = GetIntFromString(arg2, 0);
		if (want < 1024 || want > 65535)
		{
			WriteChatf("\ag[MQMCP]\ax Usage: /mcp port <1024-65535|auto>   (now: %d)", s_port.load());
			return;
		}
		SavePinnedPort(name, want);
		if (want != s_port)
			StartServer(want);
		AnnouncePort(want);
		return;
	}

	// Uptime
	auto elapsed = std::chrono::steady_clock::now() - s_startTime;
	int totalSec = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
	int hours    = totalSec / 3600;
	int minutes  = (totalSec % 3600) / 60;
	int secs     = totalSec % 60;

	// Queue depth
	int queueDepth = 0;
	{
		std::lock_guard lock(s_queueMutex);
		queueDepth = static_cast<int>(s_commandQueue.size());
	}

	// Chat buffer depth
	int chatDepth = 0;
	{
		std::lock_guard lock(s_chatMutex);
		chatDepth = static_cast<int>(s_chatLines.size());
	}

	// Game state summary
	bool inGame = false;
	std::string charInfo;
	{
		std::lock_guard lock(s_snapshotMutex);
		inGame = s_snapshot.inGame;
		if (inGame)
		{
			int hpPct = s_snapshot.hpMax > 0
				? static_cast<int>(s_snapshot.hp * 100 / s_snapshot.hpMax) : 0;
			int mnPct = s_snapshot.manaMax > 0
				? s_snapshot.mana * 100 / s_snapshot.manaMax : 0;
			charInfo = fmt::format("{} ({}), zone: {}, HP: {}%, mana: {}%",
				s_snapshot.playerName, s_snapshot.playerLevel,
				s_snapshot.zoneShortName, hpPct, mnPct);
		}
	}

	WriteChatf("\ag[MQMCP]\ax ─────────────────────────────────");
	int pin = PinnedPort(CurrentCharName());
	WriteChatf("\ag[MQMCP]\ax  URL    : \ayhttp://127.0.0.1:%d/mcp\ax  (%s)", s_port.load(),
		pin > 0 ? fmt::format("pinned {}", pin).c_str() : "auto");
	WriteChatf("\ag[MQMCP]\ax  Uptime : \aw%02d:%02d:%02d\ax", hours, minutes, secs);
	WriteChatf("\ag[MQMCP]\ax  Snap   : \aw%s\ax  (/mcp snapshot on|off)", s_snapshotEnabled ? "on" : "off");
	WriteChatf("\ag[MQMCP]\ax  Game   : %s", inGame ? fmt::format("\aw{}\ax", charInfo).c_str() : "\arNot in game\ax");
	WriteChatf("\ag[MQMCP]\ax  Queue  : \aw%d\ax pending command(s)", queueDepth);
	WriteChatf("\ag[MQMCP]\ax  Chat   : \aw%d\ax / %d lines buffered", chatDepth, (int)k_chatBufferSize);
	WriteChatf("\ag[MQMCP]\ax ─────────────────────────────────");
}

//============================================================================
// Plugin callbacks
//============================================================================

// Captures MQ window output (plugin messages, /echo, etc.)
PLUGIN_API DWORD OnWriteChatColor(const char* Line, DWORD Color, DWORD Filter)
{
	if (Line && *Line)
		PushChatLine(Line);
	return 0;
}

// Captures all incoming EQ chat: combat messages, spells, skills, say, tells, etc.
PLUGIN_API BOOL OnIncomingChat(const char* Line, DWORD Color)
{
	if (Line && *Line)
		PushChatLine(Line);
	return FALSE;  // don't suppress — let EQ display it normally
}

PLUGIN_API void InitializePlugin()
{
	s_startTime = std::chrono::steady_clock::now();
	s_snapshotEnabled = ::GetPrivateProfileIntA("Settings", "Snapshot", 0, INIFileName) != 0;

	// Loaded in game: go straight to the pinned port. Loaded at char select: auto for
	// now, and OnPulse moves us to the pin once the character is known.
	int pin = PinnedPort(CurrentCharName());
	StartServer(pin);

	AddCommand("/mcp", Cmd_MCPStatus);

	AnnouncePort(pin);
}

PLUGIN_API void ShutdownPlugin()
{
	RemoveCommand("/mcp");

	StopServer();

	WriteChatf("\ag[MQMCP]\ax Stopped.");
}

PLUGIN_API void OnPulse()
{
	// Refresh game state snapshot
	UpdateSnapshot();

	// Drain and execute queued commands on the main thread
	std::queue<std::string> toRun;
	{
		std::lock_guard lock(s_queueMutex);
		std::swap(toRun, s_commandQueue);
	}

	while (!toRun.empty())
	{
		DoCommand(toRun.front().c_str(), false);
		toRun.pop();
	}

	// Drain TLO queries. Same thread-safety reasoning as the commands above:
	// ParseMacroData reads live client state, so it only runs here.
	std::queue<std::shared_ptr<TloQuery>> tloToRun;
	{
		std::lock_guard lock(s_tloMutex);
		std::swap(tloToRun, s_tloQueue);
	}

	while (!tloToRun.empty())
	{
		auto q = std::move(tloToRun.front());
		tloToRun.pop();

		std::vector<std::string> values;
		values.reserve(q->exprs.size());

		// Parser version 2 to match what MQ's own Lua binding uses, so results
		// agree with what a script would see (see lua_MQBindings.cpp). Swapped
		// once around the whole batch rather than per expression.
		auto oldParser = std::exchange(gParserVersion, 2);
		for (const std::string& expr : q->exprs)
		{
			char buffer[MAX_STRING] = { 0 };
			strncpy_s(buffer, expr.c_str(), sizeof(buffer) - 1);
			ParseMacroData(buffer, sizeof(buffer));
			values.emplace_back(buffer);
		}
		gParserVersion = oldParser;

		// set_value throws if the promise was already satisfied or the future
		// was abandoned; a throw escaping OnPulse would take the client down.
		try { q->result.set_value(std::move(values)); } catch (...) {}
	}

	// Apply the character's pinned port once per entry into the game. Polled here rather
	// than in SetGameState because pLocalPlayer is not guaranteed to be set on the INGAME
	// transition. Runs after the TLO drain so a rebind has no queries left to strand.
	static bool s_pinChecked = false;
	if (CurrentCharName().empty())
		s_pinChecked = false;
	else if (!s_pinChecked)
	{
		s_pinChecked = true;
		ApplyPinnedPort();
	}
}
