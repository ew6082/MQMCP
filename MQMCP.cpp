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
#include <algorithm>
#include <array>
#include <deque>
#include <filesystem>
#include <functional>
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

// 'done' is set by run_command, which has to know when its command actually ran so it can
// collect the output that followed; fire-and-forget callers leave it null.
struct QueuedCommand
{
	std::string                          cmd;
	std::shared_ptr<std::promise<void>>  done;
};

static std::queue<QueuedCommand> s_commandQueue;
static std::mutex                s_queueMutex;

static void EnqueueCommand(std::string cmd, std::shared_ptr<std::promise<void>> done = nullptr)
{
	std::lock_guard lock(s_queueMutex);
	s_commandQueue.push({std::move(cmd), std::move(done)});
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

//----------------------------------------------------------------------------
// Chat channels and tabs
//
// OnIncomingChat hands us each line's color code (USERCOLOR_*), the same thing MQ2DPSAdv
// switches on. EQ routes a line to a window by its *channel* (eChatChannel), and keeps the
// channel -> window map plus window names in UI_<Name>_<server>.ini, [ChatManager]:
// ChannelMap<channel>=<window>, ChatWindow<window>_Name=<tab name>. EQ's own color ->
// channel lookup (CChatWindowManager::GetChannelFromColor) isn't exported by eqlib, so
// ChannelFromColor below is a hand-made equivalent; the entries marked "guess" have no
// one-to-one name match and should be checked against a live client.
//
// Reading the ini rather than CChatWindowManager::ChannelMap means a patch that moves the
// struct can't crash us. The cost: EQ writes the ini when you camp, so a tab change made in
// game shows up after the next login or /mcp chat reload.
//----------------------------------------------------------------------------

static constexpr int k_maxChatChannels = 128;   // eqlib knows 105; newer clients write ChannelMap up to 106
static constexpr int k_channelMQ       = -2;    // MQ's own output (OnWriteChatColor), not an EQ channel
static constexpr int k_channelUnknown  = -1;

static int ChannelFromColor(int color)
{
	if (color >= USERCOLOR_CHAT_1 && color <= USERCOLOR_CHAT_10)
		return CHAT_CHANNEL_CHAT1 + (color - USERCOLOR_CHAT_1);
	if (color >= USERCOLOR_ECHO_CHAT_1 && color <= USERCOLOR_ECHO_CHAT_10)
		return CHAT_CHANNEL_CHAT1 + (color - USERCOLOR_ECHO_CHAT_1);

	switch (color)
	{
	case USERCOLOR_SAY:              case USERCOLOR_ECHO_SAY:     return CHAT_CHANNEL_SAY;
	case USERCOLOR_TELL:             case USERCOLOR_ECHO_TELL:    return CHAT_CHANNEL_TELL;
	case USERCOLOR_GROUP:            case USERCOLOR_ECHO_GROUP:   return CHAT_CHANNEL_GROUP;
	case USERCOLOR_RAID:                                          return CHAT_CHANNEL_RAID;
	case USERCOLOR_GUILD:            case USERCOLOR_ECHO_GUILD:
	case USERCOLOR_GUILD_MSG:                                     return CHAT_CHANNEL_GUILD;
	case USERCOLOR_OOC:              case USERCOLOR_ECHO_OOC:     return CHAT_CHANNEL_OOC;
	case USERCOLOR_AUCTION:          case USERCOLOR_ECHO_AUCTION: return CHAT_CHANNEL_AUCTION;
	case USERCOLOR_SHOUT:            case USERCOLOR_ECHO_SHOUT:   return CHAT_CHANNEL_SHOUT;
	case USERCOLOR_EMOTE:            case USERCOLOR_ECHO_EMOTE:   return CHAT_CHANNEL_EMOTE;
	case USERCOLOR_CHAT_CHANNEL:                                  return CHAT_CHANNEL_CHAT_CHANNEL_INFO;

	case USERCOLOR_YOU_HIT_OTHER:          return CHAT_CHANNEL_MELEE_YOUR_HITS;
	case USERCOLOR_YOU_MISS_OTHER:         return CHAT_CHANNEL_MELEE_YOUR_MISSES;
	case USERCOLOR_OTHER_HIT_YOU:          return CHAT_CHANNEL_MELEE_YOU_BEING_HIT;
	case USERCOLOR_OTHER_MISS_YOU:         return CHAT_CHANNEL_MELEE_YOU_BEING_MISSED;
	case USERCOLOR_OTHER_HIT_OTHER:        return CHAT_CHANNEL_MELEE_OTHERS_HITS;
	case USERCOLOR_OTHER_MISS_OTHER:       return CHAT_CHANNEL_MELEE_OTHERS_MISSES;
	case USERCOLOR_YOUR_DEATH:             return CHAT_CHANNEL_MELEE_MY_DEATH;
	case USERCOLOR_OTHER_DEATH:            return CHAT_CHANNEL_MELEE_OTHER_PC_DEATH;
	case USERCOLOR_MELEE_CRIT:             return CHAT_CHANNEL_MELEE_CRITICAL_HITS;
	case USERCOLOR_DISCIPLINES:            return CHAT_CHANNEL_MELEE_DISCIPLINES;
	case USERCOLOR_TOO_FAR_AWAY:           return CHAT_CHANNEL_MELEE_WARNINGS;
	case USERCOLOR_NPC_RAMPAGE:            return CHAT_CHANNEL_MELEE_NPC_RAMPAGE;
	case USERCOLOR_NPC_FLURRY:             return CHAT_CHANNEL_MELEE_NPC_FLURRY;
	case USERCOLOR_NPC_ENRAGE:             return CHAT_CHANNEL_MELEE_NPC_ENRAGE;
	case USERCOLOR_FLURRY:                 return CHAT_CHANNEL_MELEE_YOUR_FLURRY;
	case USERCOLOR_NPC_DEATH:              return CHAT_CHANNEL_MELEE_NPC_DEATH;
	case USERCOLOR_MELEE_OTHERS_CRITICAL:  return CHAT_CHANNEL_MELEE_OTHERS_CRITICAL;
	case USERCOLOR_DISCIPLINES_OTHER:      return CHAT_CHANNEL_OTHER_DISCIPLINES;
	case USERCOLOR_TAUNT_MESSAGES:         return CHAT_CHANNEL_TAUNT_MESSAGES;

	case USERCOLOR_SPELLS:                 return CHAT_CHANNEL_SPELLS_MINE;
	case USERCOLOR_OTHERS_SPELLS:          return CHAT_CHANNEL_SPELLS_OTHERS;
	case USERCOLOR_SPELL_FAILURE:          return CHAT_CHANNEL_SPELLS_FAILURES;
	case USERCOLOR_SPELL_CRIT:             return CHAT_CHANNEL_SPELLS_CRITICALS;
	case USERCOLOR_SPELL_WORN_OFF:         return CHAT_CHANNEL_SPELLS_WORN_OFF;
	case USERCOLOR_NON_MELEE:              return CHAT_CHANNEL_SPELLS_DD_OTHERS;            // guess
	case USERCOLOR_DIRECT_DAMAGE_YOU:      return CHAT_CHANNEL_SPELLS_DD_YOURS;
	case USERCOLOR_DIRECT_DAMAGE_OTHERS:   return CHAT_CHANNEL_OTHER_DIRECT_DAMAGE;
	case USERCOLOR_DIRECT_DAMAGE_OTHERS_CRITICAL: return CHAT_CHANNEL_SPELLS_DD_OTHERS_CRITICAL;
	case USERCOLOR_DOTS_YOURS:             return CHAT_CHANNEL_YOUR_DOTS;
	case USERCOLOR_DOTS_OTHERS:            return CHAT_CHANNEL_OTHER_DOTS;
	case USERCOLOR_DOT_YOURS_CRITICAL:     return CHAT_CHANNEL_DOT_YOURS_CRITICAL;
	case USERCOLOR_DOT_OTHERS_CRITICAL:    return CHAT_CHANNEL_DOT_OTHERS_CRITICAL;
	case USERCOLOR_DOT_DAMAGE_TAKEN:       return CHAT_CHANNEL_DOT_YOU_BEING_HIT;
	case USERCOLOR_HEALS_YOU:              return CHAT_CHANNEL_YOUR_HEALS;
	case USERCOLOR_HEALS_OTHERS:           return CHAT_CHANNEL_OTHER_HEALS;
	case USERCOLOR_HEALS_RECEIVED:         return CHAT_CHANNEL_HEALS_RECEIVED;
	case USERCOLOR_HEALS_YOURS_CRITICAL:   return CHAT_CHANNEL_HEALS_YOUR_CRITICAL;
	case USERCOLOR_HEALS_OTHERS_CRITICAL:  return CHAT_CHANNEL_HEALS_OTHERS_CRITICAL;
	case USERCOLOR_AURAS_YOU:              return CHAT_CHANNEL_YOUR_AURAS;
	case USERCOLOR_AURAS_OTHER:            return CHAT_CHANNEL_OTHER_AURAS;
	case USERCOLOR_BARD_SONG_PETS:         return CHAT_CHANNEL_BARD_SONGS;                  // guess
	case USERCOLOR_SPELL_MESSAGES:         return CHAT_CHANNEL_SPELL_EMOTES;
	case USERCOLOR_FOCUS:                  return CHAT_CHANNEL_FOCUS_EFFECTS;
	case USERCOLOR_OVERWRITTEN_DET:        return CHAT_CHANNEL_OVERWRITTEN_DETRIMENTAL_SPELL_MESSAGES;
	case USERCOLOR_OVERWRITTEN_BENE:       return CHAT_CHANNEL_OVERWRITTEN_BENEFICIAL_SPELL_MESSAGES;
	case USERCOLOR_SPELL_FAILURES_OTHER:   return NUM_CHAT_CHANNELS;                        // guess: first channel past eqlib's list
	case USERCOLOR_LOCKED_INVENTORY_SLOTS: return NUM_CHAT_CHANNELS + 1;                    // guess

	case USERCOLOR_PET:                    return CHAT_CHANNEL_PET_MESSAGES;                // guess
	case USERCOLOR_PETRAMPFLURRY:          return CHAT_CHANNEL_PET_RAMPAGE_FLURRY;
	case USERCOLOR_PETCRITS:               return CHAT_CHANNEL_PET_CRITICALS;
	case USERCOLOR_PET_SPELLS:             return CHAT_CHANNEL_PET_SPELLS;
	case USERCOLOR_PET_RESPONSES:          return CHAT_CHANNEL_PET_RESPONSES;
	case USERCOLOR_DAMAGESHIELD:           return CHAT_CHANNEL_DAMAGE_SHIELDS_YOU_ATTACKING; // guess
	case USERCOLOR_DAMAGESHIELD_SELF:      return CHAT_CHANNEL_DAMAGE_SHIELDS_YOU_DEFENDING; // guess
	case USERCOLOR_DAMAGESHIELD_OTHER:     return CHAT_CHANNEL_DAMAGE_SHIELDS_OTHERS;
	case USERCOLOR_FALL_DAMAGE_SELF:       return CHAT_CHANNEL_ENVIRONMENTAL_DAMAGE_YOURS;
	case USERCOLOR_FALL_DAMAGE_OTHER:      return CHAT_CHANNEL_ENVIRONMENTAL_DAMAGE_OTHERS;

	case USERCOLOR_SKILLS:                 return CHAT_CHANNEL_SKILLS;
	case USERCOLOR_XP:                     return CHAT_CHANNEL_EXPERIENCE_MESSAGES;
	case USERCOLOR_SYSTEM:                 return CHAT_CHANNEL_SYSTEM_MESSAGES;
	case USERCOLOR_WHO:                    return CHAT_CHANNEL_WHO;
	case USERCOLOR_EVENT:                  return CHAT_CHANNEL_EVENT_MESSAGES;
	case USERCOLOR_NPC_SPEECH:
	case USERCOLOR_NPC_SPEECH_TO_YOU:      return CHAT_CHANNEL_NPC_EMOTES;                  // guess
	case USERCOLOR_ITEM_SPEECH:            return CHAT_CHANNEL_ITEM_SPEECH;
	case USERCOLOR_FELLOWSHIP:             return CHAT_CHANNEL_FELLOWSHIP_MESSAGES;
	case USERCOLOR_MERCENARY_GRP:          return CHAT_CHANNEL_MERCENARY_MESSAGES;
	case USERCOLOR_PVP:                    return CHAT_CHANNEL_PVP_MESSAGES;
	case USERCOLOR_DEBUG:                  return CHAT_CHANNEL_DEBUG;
	case USERCOLOR_RANDOM:                 return CHAT_CHANNEL_RANDOM_YOUR_ROLLS;
	case USERCOLOR_DICE_OTHER:             return CHAT_CHANNEL_RANDOM_OTHERS_ROLLS;
	case USERCOLOR_DICE_GROUP:             return CHAT_CHANNEL_RANDOM_GROUP_RAID_ROLLS;
	case USERCOLOR_LOOT:
	case USERCOLOR_MONEY_SPLIT:            return CHAT_CHANNEL_LOOT_MESSAGES;               // guess for money
	case USERCOLOR_FACTION:
	case USERCOLOR_FACTION_LINK:           return CHAT_CHANNEL_FACTION_MESSAGES;
	case USERCOLOR_CANT_USE_COMMAND:       return CHAT_CHANNEL_CANT_USE_COMMAND;
	case USERCOLOR_ABILITY_COOLDOWN:       return CHAT_CHANNEL_COMBAT_ABILITY_REUSE;
	case USERCOLOR_AA_REUSE_TIMER:         return CHAT_CHANNEL_AA_ABILITY_REUSE;
	case USERCOLOR_DESTROY_ITEM:           return CHAT_CHANNEL_DESTROY_ITEM;
	case USERCOLOR_ACHIEVEMENTS_YOU:       return CHAT_CHANNEL_YOUR_ACHIEVEMENTS;
	case USERCOLOR_ACHIEVEMENTS_OTHERS:    return CHAT_CHANNEL_OTHER_ACHIEVEMENTS;
	case USERCOLOR_FOOD_AND_DRINK:         return CHAT_CHANNEL_FOOD_AND_DRINK;
	case USERCOLOR_RAID_VICTORY:           return CHAT_CHANNEL_RAID_VICTORY;

	// Non-user colors (console/con colors) land in the colored "Other" channels.  guess
	case CONCOLOR_GREEN:                   return CHAT_CHANNEL_OTHER_GREEN;
	case CONCOLOR_BLUE:
	case CONCOLOR_LIGHTBLUE:               return CHAT_CHANNEL_OTHER_BLUE;
	case CONCOLOR_YELLOW:                  return CHAT_CHANNEL_OTHER_YELLOW;
	case CONCOLOR_RED:                     return CHAT_CHANNEL_OTHER_RED;

	default:                               return CHAT_CHANNEL_OTHER;                       // guess
	}
}

// Channel names for output and filtering: the eChatChannel name, lowercased, without its
// CHAT_CHANNEL_ prefix ("melee_your_hits"). Built once, from pairs so order can't drift.
static const std::array<std::string, k_maxChatChannels>& ChannelNames()
{
	static const auto names = [] {
		std::array<std::string, k_maxChatChannels> n;
		for (int i = 0; i < k_maxChatChannels; ++i)
			n[i] = fmt::format("channel_{}", i);
#define CH(x) { CHAT_CHANNEL_##x, #x }
		const std::pair<int, const char*> known[] = {
			CH(SAY), CH(TELL), CH(GROUP), CH(RAID), CH(GUILD), CH(OOC), CH(AUCTION), CH(SHOUT),
			CH(EMOTE), CH(MELEE_YOUR_HITS), CH(SPELLS_MINE), CH(SKILLS), CH(CHAT1), CH(CHAT2),
			CH(CHAT3), CH(CHAT4), CH(CHAT5), CH(CHAT6), CH(CHAT7), CH(CHAT8), CH(CHAT9), CH(CHAT10),
			CH(OTHER), CH(MELEE_YOUR_MISSES), CH(MELEE_YOU_BEING_HIT), CH(MELEE_YOU_BEING_MISSED),
			CH(MELEE_OTHERS_HITS), CH(MELEE_OTHERS_MISSES), CH(MELEE_MY_DEATH),
			CH(MELEE_OTHER_PC_DEATH), CH(MELEE_CRITICAL_HITS), CH(MELEE_DISCIPLINES),
			CH(MELEE_WARNINGS), CH(MELEE_NPC_RAMPAGE), CH(MELEE_NPC_FLURRY), CH(MELEE_NPC_ENRAGE),
			CH(SPELLS_OTHERS), CH(SPELLS_FAILURES), CH(SPELLS_CRITICALS), CH(SPELLS_WORN_OFF),
			CH(SPELLS_DD_OTHERS), CH(FOCUS_EFFECTS), CH(RANDOM_YOUR_ROLLS), CH(PET_MESSAGES),
			CH(PET_RAMPAGE_FLURRY), CH(PET_CRITICALS), CH(DAMAGE_SHIELDS_YOU_ATTACKING),
			CH(EXPERIENCE_MESSAGES), CH(NPC_EMOTES), CH(SYSTEM_MESSAGES), CH(WHO), CH(PET_SPELLS),
			CH(PET_RESPONSES), CH(ITEM_SPEECH), CH(FELLOWSHIP_MESSAGES), CH(MERCENARY_MESSAGES),
			CH(PVP_MESSAGES), CH(MELEE_YOUR_FLURRY), CH(DEBUG), CH(MELEE_NPC_DEATH),
			CH(RANDOM_OTHERS_ROLLS), CH(RANDOM_GROUP_RAID_ROLLS), CH(ENVIRONMENTAL_DAMAGE_YOURS),
			CH(ENVIRONMENTAL_DAMAGE_OTHERS), CH(DAMAGE_SHIELDS_YOU_DEFENDING),
			CH(DAMAGE_SHIELDS_OTHERS), CH(EVENT_MESSAGES),
			CH(OVERWRITTEN_DETRIMENTAL_SPELL_MESSAGES), CH(OVERWRITTEN_BENEFICIAL_SPELL_MESSAGES),
			CH(CANT_USE_COMMAND), CH(COMBAT_ABILITY_REUSE), CH(AA_ABILITY_REUSE), CH(DESTROY_ITEM),
			CH(YOUR_AURAS), CH(OTHER_AURAS), CH(YOUR_HEALS), CH(OTHER_HEALS), CH(YOUR_DOTS),
			CH(OTHER_DOTS), CH(BARD_SONGS), CH(OTHER_DIRECT_DAMAGE), CH(SPELL_EMOTES),
			CH(FACTION_MESSAGES), CH(LOOT_MESSAGES), CH(TAUNT_MESSAGES), CH(OTHER_DISCIPLINES),
			CH(YOUR_ACHIEVEMENTS), CH(OTHER_ACHIEVEMENTS), CH(FOOD_AND_DRINK), CH(RAID_VICTORY),
			CH(OTHER_GREEN), CH(OTHER_BLUE), CH(OTHER_YELLOW), CH(OTHER_RED),
			CH(CHAT_CHANNEL_INFO), CH(95), CH(SPELLS_DD_YOURS), CH(SPELLS_DD_OTHERS_CRITICAL),
			CH(DOT_YOURS_CRITICAL), CH(DOT_OTHERS_CRITICAL), CH(DOT_YOU_BEING_HIT),
			CH(HEALS_RECEIVED), CH(HEALS_YOUR_CRITICAL), CH(HEALS_OTHERS_CRITICAL),
			CH(MELEE_OTHERS_CRITICAL),
		};
#undef CH
		for (const auto& [id, name] : known)
		{
			std::string s = name;
			for (char& c : s)
				c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
			n[id] = std::move(s);
		}
		n[NUM_CHAT_CHANNELS]     = "spell_failures_other";      // guesses, see ChannelFromColor
		n[NUM_CHAT_CHANNELS + 1] = "locked_inventory_slots";
		return n;
	}();
	return names;
}

static std::string ChannelName(int channel)
{
	if (channel == k_channelMQ)
		return "mq";
	if (channel < 0 || channel >= k_maxChatChannels)
		return "unknown";
	return ChannelNames()[channel];
}

// This character's chat layout, from the UI ini. Written on the game thread (load/reload),
// read on the game thread (tagging) and the HTTP thread (chat_tabs), hence the mutex.
struct ChatLayout
{
	bool                                    loaded = false;
	std::string                             file;
	std::array<int, k_maxChatChannels>      windowOf{};     // channel -> window index, -1 none
	std::array<std::string, MAX_CHAT_WINDOWS> names;        // window index -> tab name
};

static ChatLayout s_chatLayout;
static std::mutex s_layoutMutex;

// With personas (one character, several classes) EQ keeps a layout per class:
// UI_<Name>_<server>_<CLS>.ini. Prefer the current class's file, then the plain
// UI_<Name>_<server>.ini, then the newest suffixed one.
static std::string FindUIIni(const std::string& name, const std::string& server, const std::string& classCode)
{
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::path dir(gPathEverQuest);
	if (!classCode.empty())
	{
		fs::path persona = dir / fmt::format("UI_{}_{}_{}.ini", name, server, classCode);
		if (fs::exists(persona, ec))
			return persona.string();
	}
	fs::path exact = dir / fmt::format("UI_{}_{}.ini", name, server);
	if (fs::exists(exact, ec))
		return exact.string();

	std::string prefix = fmt::format("UI_{}_{}_", name, server);
	fs::path best;
	fs::file_time_type bestTime{};
	for (const auto& entry : fs::directory_iterator(dir, ec))
	{
		std::string fn = entry.path().filename().string();
		if (fn.size() <= prefix.size() + 4 || !ci_starts_with(fn, prefix) || !ci_ends_with(fn, ".ini"))
			continue;
		auto t = entry.last_write_time(ec);
		if (best.empty() || t > bestTime)
			best = entry.path(), bestTime = t;
	}
	return best.empty() ? exact.string() : best.string();
}

static void LoadChatLayout()
{
	ChatLayout layout;
	layout.windowOf.fill(-1);
	if (GetGameState() == GAMESTATE_INGAME && pLocalPlayer)
	{
		const char* cls = pLocalPlayer->GetClassThreeLetterCode();
		layout.file = FindUIIni(pLocalPlayer->Name, GetServerShortName(), cls ? cls : "");

		// One read of the whole section; ~150 GetPrivateProfileString calls would reopen
		// a 6000-line file each time.
		std::vector<char> buf(64 * 1024);
		DWORD len = ::GetPrivateProfileSectionA("ChatManager", buf.data(), static_cast<DWORD>(buf.size()), layout.file.c_str());
		for (const char* p = buf.data(); p < buf.data() + len && *p; p += strlen(p) + 1)
		{
			std::string_view kv(p);
			size_t eq = kv.find('=');
			if (eq == std::string_view::npos)
				continue;
			std::string_view key = kv.substr(0, eq), val = kv.substr(eq + 1);

			int idx = 0;
			if (key.rfind("ChannelMap", 0) == 0)
			{
				idx = GetIntFromString(std::string(key.substr(10)), -1);
				if (idx >= 0 && idx < k_maxChatChannels)
					layout.windowOf[idx] = GetIntFromString(std::string(val), -1);
			}
			else if (key.rfind("ChatWindow", 0) == 0 && key.size() > 5 && key.substr(key.size() - 5) == "_Name")
			{
				idx = GetIntFromString(std::string(key.substr(10, key.size() - 15)), -1);
				if (idx >= 0 && idx < MAX_CHAT_WINDOWS)
				{
					// EQ pads default names ("Chat  3"); collapse runs of spaces for matching.
					std::string name;
					for (char c : val)
						if (c != ' ' || (!name.empty() && name.back() != ' '))
							name += c;
					layout.names[idx] = name;
				}
			}
		}
		layout.loaded = len > 0;
	}

	std::lock_guard lock(s_layoutMutex);
	s_chatLayout = std::move(layout);
}

// Tab a channel is shown in, "" if no layout is loaded or the channel isn't mapped.
static std::string TabForChannel(int channel)
{
	if (channel == k_channelMQ)
		return "MQ";
	std::lock_guard lock(s_layoutMutex);
	if (!s_chatLayout.loaded || channel < 0 || channel >= k_maxChatChannels)
		return {};
	int w = s_chatLayout.windowOf[channel];
	if (w < 0 || w >= MAX_CHAT_WINDOWS)
		return {};
	return s_chatLayout.names[w].empty() ? fmt::format("Window {}", w) : s_chatLayout.names[w];
}

// Every line gets a monotonic sequence number so a caller can ask for "everything after
// seq N" (chat_since, run_command) instead of guessing from the buffer length, which stops
// working once a chatty client has filled the ring. Numbering restarts at 1 on plugin load.
// channel/tab are resolved when the line arrives, so a later layout reload doesn't relabel it.
struct ChatLink
{
	std::string type;   // spell, item, player, achievement, ...
	int         id = 0; // spell or item id; 0 for other types
	std::string text;   // what the chat window shows
};

struct ChatLine
{
	uint64_t              seq;
	std::string           text;
	int                   color   = 0;
	int                   channel = k_channelUnknown;
	std::string           tab;
	std::vector<ChatLink> links;
};

// Item/spell/player links arrive as 0x12-delimited markup ("63^46875^38^'Vampiric Draw").
// The ids are worth keeping -- spell names repeat across ranks, the id doesn't.
static std::vector<ChatLink> ExtractChatLinks(const char* line)
{
	std::vector<ChatLink> out;
	if (!strchr(line, ITEM_TAG_CHAR))
		return out;

	TextTagInfo tags[MAX_EXTRACT_LINKS];
	size_t n = ExtractLinks(line, tags, MAX_EXTRACT_LINKS);
	for (size_t i = 0; i < n && i < MAX_EXTRACT_LINKS; ++i)
	{
		ChatLink link;
		link.text = std::string(tags[i].text);
		switch (tags[i].tagCode)
		{
		case ETAG_SPELL:
		{
			link.type = "spell";
			SpellLinkInfo info{};
			if (ParseSpellLink(tags[i].link, info))
				link.id = info.spellID;
			break;
		}
		case ETAG_ITEM:
		{
			link.type = "item";
			ItemLinkInfo info{};
			if (ParseItemLink(tags[i].link, info))
				link.id = info.itemID;
			break;
		}
		case ETAG_PLAYER:           link.type = "player";      break;
		case ETAG_ACHIEVEMENT:      link.type = "achievement"; break;
		case ETAG_DIALOG_RESPONSE:  link.type = "dialog";      break;
		case ETAG_FACTION:          link.type = "faction";     break;
		case ETAG_COMMAND:
		case ETAG_COMMAND2:         link.type = "command";     break;
		default:                    link.type = "other";       break;
		}
		out.push_back(std::move(link));
	}
	return out;
}

// Which lines a chat read returns. Lists match case-insensitively; empty means "any".
struct ChatFilter
{
	std::string              text;       // substring of the line
	std::vector<std::string> tabs;       // line's tab is one of these
	std::vector<std::string> channels;   // line's channel is one of these
	std::vector<std::string> exclude;    // drop if tab or channel is one of these

	bool Matches(const ChatLine& line) const
	{
		auto in = [](const std::vector<std::string>& list, const std::string& v) {
			return std::any_of(list.begin(), list.end(), [&](const std::string& s) { return ci_equals(s, v); });
		};
		std::string channel = ChannelName(line.channel);
		if (!text.empty() && ci_find_substr(line.text, text) < 0) return false;
		if (!tabs.empty() && !in(tabs, line.tab))                 return false;
		if (!channels.empty() && !in(channels, channel))          return false;
		if (in(exclude, line.tab) || in(exclude, channel))        return false;
		return true;
	}
};

static std::deque<ChatLine> s_chatLines;
static uint64_t             s_chatLastSeq = 0;   // seq of the newest line; guarded by s_chatMutex
static std::mutex           s_chatMutex;

// color is the USERCOLOR_* code for EQ chat; fromMQ marks MQ's own output (OnWriteChatColor),
// whose color is just a display color.
static void PushChatLine(const char* line, int color, bool fromMQ)
{
	int         channel = fromMQ ? k_channelMQ : ChannelFromColor(color);
	std::string tab     = TabForChannel(channel);

	// Keep the link ids, then reduce the markup to display text as MQ2DPSAdv does.
	// StripTextLinks works in place and only shrinks.
	std::vector<ChatLink> links = ExtractChatLinks(line);
	std::vector<char>     unlinked;
	if (strchr(line, ITEM_TAG_CHAR))
	{
		unlinked.assign(line, line + strlen(line) + 1);
		StripTextLinks(unlinked.data());
		line = unlinked.data();
	}

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
	s_chatLines.push_back({++s_chatLastSeq, std::move(clean), color, channel, std::move(tab), std::move(links)});
	if (s_chatLines.size() > k_chatBufferSize)
		s_chatLines.pop_front();
}

static uint64_t ChatLastSeq()
{
	std::lock_guard lock(s_chatMutex);
	return s_chatLastSeq;
}

// Lines with seq > since that pass filter, oldest first, at most maxLines. 'next' is the
// cursor for the following call: it advances past filtered-out lines too, so a caller
// polling with a filter doesn't rescan them.
static json ChatSince(uint64_t since, const ChatFilter& filter, size_t maxLines)
{
	std::lock_guard lock(s_chatMutex);

	// A cursor ahead of the newest line means the plugin was reloaded and numbering restarted.
	bool reset = since > s_chatLastSeq;
	if (reset)
		since = 0;

	uint64_t oldest = s_chatLines.empty() ? s_chatLastSeq + 1 : s_chatLines.front().seq;
	// Lines evicted before being read. since 0 means "whatever is buffered", so nothing is missed.
	uint64_t missed = since > 0 && since + 1 < oldest ? oldest - since - 1 : 0;
	size_t   start  = since >= oldest ? static_cast<size_t>(since - oldest + 1) : 0;

	json     lines = json::array();
	uint64_t next  = std::max(since, oldest - 1);
	bool     more  = false;
	for (size_t i = start; i < s_chatLines.size(); ++i)
	{
		const ChatLine& line = s_chatLines[i];
		if (filter.Matches(line))
		{
			if (lines.size() == maxLines)
			{
				more = true;
				break;
			}
			json l = {{"seq", line.seq}, {"channel", ChannelName(line.channel)}, {"text", line.text}};
			if (!line.tab.empty())
				l["tab"] = line.tab;
			if (line.channel != k_channelMQ)
				l["color"] = line.color;   // raw USERCOLOR_* code, for checking the channel mapping
			for (const ChatLink& link : line.links)
			{
				json j = {{"type", link.type}, {"text", link.text}};
				if (link.id)
					j["id"] = link.id;
				l["links"].push_back(std::move(j));
			}
			lines.push_back(std::move(l));
		}
		next = line.seq;
	}

	json out = {{"next", next}, {"more", more}, {"lines", std::move(lines)}};
	if (missed)
		out["missed"] = missed;
	if (reset)
		out["reset"] = true;
	return out;
}

//============================================================================
// Game state, read through MQ's TLOs
//
// Built on demand on the game thread from TLO expressions instead of being copied out of
// eqlib structs every pulse. The TLOs are maintained with eqlib by the MQ devs, so this
// carries no struct layouts of its own, and it costs nothing between requests.
//============================================================================

// Work for the game thread that hands back a JSON result -- the counterpart of TloQuery
// for callers that need several dependent reads (a count, then each entry) in one frame.
struct GameTask
{
	std::function<json()> build;
	std::promise<json>    result;
};

static std::queue<std::shared_ptr<GameTask>> s_taskQueue;
static std::mutex                            s_taskMutex;

// Runs `build` on the next pulse and waits for it, with the same 5s budget as a TLO query.
static bool RunOnGameThread(std::function<json()> build, json& out, std::string& err)
{
	auto task   = std::make_shared<GameTask>();
	task->build = std::move(build);
	auto fut    = task->result.get_future();
	{
		std::lock_guard lock(s_taskMutex);
		s_taskQueue.push(task);
	}
	if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
	{
		err = "Timed out waiting for the game thread (is the client pulsing?)";
		return false;
	}
	try { out = fut.get(); }
	catch (const std::exception& e) { err = std::string("Game-thread read failed: ") + e.what(); return false; }
	return true;
}

static bool InGame()
{
	return GetGameState() == GAMESTATE_INGAME && pLocalPlayer != nullptr;
}

// One TLO expression (no ${}), evaluated with parser 2 like query_tlo. Game thread only.
// Unresolved values come back empty rather than as "NULL".
static std::string Tlo(const std::string& expr)
{
	std::string wrapped = "${" + expr + "}";
	char buffer[MAX_STRING] = { 0 };
	strncpy_s(buffer, wrapped.c_str(), sizeof(buffer) - 1);
	auto oldParser = std::exchange(gParserVersion, 2);
	ParseMacroData(buffer, sizeof(buffer));
	gParserVersion = oldParser;
	std::string value = buffer;
	return value == "NULL" || value == wrapped ? std::string() : value;
}

static int64_t TloInt(const std::string& expr)   { return std::strtoll(Tlo(expr).c_str(), nullptr, 10); }
static float   TloFloat(const std::string& expr) { return GetFloatFromString(Tlo(expr), 0.f); }
static bool    TloBool(const std::string& expr)  { return ci_equals(Tlo(expr), "TRUE"); }

static json PositionJson(const std::string& spawn)
{
	return {{"x", TloFloat(spawn + ".X")}, {"y", TloFloat(spawn + ".Y")}, {"z", TloFloat(spawn + ".Z")}};
}

//----------------------------------------------------------------------------
// Builders -- game thread only. Each assumes InGame().
//----------------------------------------------------------------------------

static json BuildPlayerJson()
{
	return {
		{"name",      Tlo("Me.CleanName")},
		{"level",     TloInt("Me.Level")},
		{"class",     Tlo("Me.Class.Name")},
		{"classId",   TloInt("Me.Class.ID")},
		{"invisible", TloBool("Me.Invis")},
		{"position",  PositionJson("Me")},
		{"hp",        {{"current", TloInt("Me.CurrentHPs")},       {"max", TloInt("Me.MaxHPs")},       {"pct", TloInt("Me.PctHPs")}}},
		{"mana",      {{"current", TloInt("Me.CurrentMana")},      {"max", TloInt("Me.MaxMana")},      {"pct", TloInt("Me.PctMana")}}},
		{"endurance", {{"current", TloInt("Me.CurrentEndurance")}, {"max", TloInt("Me.MaxEndurance")}, {"pct", TloInt("Me.PctEndurance")}}}
	};
}

static json BuildZoneJson()
{
	return {
		{"longName",  Tlo("Zone.Name")},
		{"shortName", Tlo("Zone.ShortName")},
		{"id",        TloInt("Zone.ID")}
	};
}

// Spell buffs on a spawn TLO that has Buff[n] (Me, Target). `count` stops the scan once
// that many have been found, since slots can have gaps.
static json BuffsJson(const std::string& owner, int firstSlot, int lastSlot, int64_t count, bool song)
{
	json arr = json::array();
	for (int i = firstSlot; i <= lastSlot && static_cast<int64_t>(arr.size()) < count; ++i)
	{
		std::string buff = fmt::format("{}[{}]", owner, i);
		int64_t     id   = TloInt(buff + ".ID");
		if (id <= 0)
			continue;
		json b = {{"name", Tlo(buff + ".Name")}, {"id", id}, {"seconds", TloInt(buff + ".Duration.TotalSeconds")}};
		if (song)
			b["song"] = true;
		arr.push_back(std::move(b));
	}
	return arr;
}

static json BuildBuffsJson()
{
	json buffs = BuffsJson("Me.Buff", 1, static_cast<int>(TloInt("Me.MaxBuffSlots")), TloInt("Me.CountBuffs"), false);
	for (json& s : BuffsJson("Me.Song", 1, MAX_TOTAL_BUFFS - NUM_LONG_BUFFS, TloInt("Me.CountSongs"), true))
		buffs.push_back(std::move(s));
	return buffs;
}

static json BuildTargetJson()
{
	if (TloInt("Target.ID") <= 0)
		return nullptr;
	int64_t buffCount = TloInt("Target.BuffCount");
	return {
		{"name",     Tlo("Target.CleanName")},
		{"id",       TloInt("Target.ID")},
		{"type",     Tlo("Target.Type")},
		{"level",    TloInt("Target.Level")},
		{"distance", TloFloat("Target.Distance3D")},
		{"hp",       {{"current", TloInt("Target.CurrentHPs")}, {"max", TloInt("Target.MaxHPs")}, {"pct", TloInt("Target.PctHPs")}}},
		{"buffs",    BuffsJson("Target.Buff", 1, static_cast<int>(buffCount), buffCount, false)}
	};
}

// Gem numbers are 1-based, as /cast and Me.Gem[n] use them.
static json BuildSpellsJson()
{
	json arr = json::array();
	int  gems = static_cast<int>(TloInt("Me.NumGems"));
	for (int i = 1; i <= gems; ++i)
	{
		int64_t id = TloInt(fmt::format("Me.Gem[{}].ID", i));
		if (id <= 0)
			continue;
		arr.push_back({
			{"gem",          i},
			{"name",         Tlo(fmt::format("Me.Gem[{}].Name", i))},
			{"id",           id},
			{"ready",        TloBool(fmt::format("Me.SpellReady[{}]", i))},
			{"ms_remaining", TloInt(fmt::format("Me.GemTimer[{}].Raw", i))}
		});
	}
	return arr;
}

static json BuildLootJson()
{
	json arr = json::array();
	auto collect = [&](const char* list, const char* countMember, bool personal) {
		int count = static_cast<int>(TloInt(fmt::format("AdvLoot.{}", countMember)));
		for (int i = 1; i <= count; ++i)
		{
			std::string item = fmt::format("AdvLoot.{}[{}]", list, i);
			arr.push_back({
				{"name",      Tlo(item + ".Name")},
				{"id",        TloInt(item + ".ID")},
				{"noDrop",    TloBool(item + ".NoDrop")},
				{"stackSize", TloInt(item + ".StackSize")},
				{"personal",  personal}
			});
		}
	};
	collect("PList", "PCount", true);
	collect("SList", "SCount", false);
	return arr;
}

// Group.Member[0] is you; members 1-5 the rest. Spawn fields read 0 for offline members.
static json BuildGroupJson()
{
	json arr = json::array();
	if (TloInt("Me.GroupSize") == 0)
		return arr;
	for (int i = 0; i < MAX_GROUP_SIZE; ++i)
	{
		std::string m    = fmt::format("Group.Member[{}]", i);
		std::string name = Tlo(m + ".Name");
		if (name.empty())
			continue;
		arr.push_back({
			{"name",       name},
			{"level",      TloInt(m + ".Level")},
			{"leader",     TloBool(m + ".Leader")},
			{"mercenary",  TloBool(m + ".Mercenary")},
			{"offline",    TloBool(m + ".Offline")},
			{"other_zone", TloBool(m + ".OtherZone")},
			{"hp_pct",     TloInt(m + ".PctHPs")},
			{"mana_pct",   TloInt(m + ".PctMana")}
		});
	}
	return arr;
}

// NPCs on the extended target window. XTarget slots can also hold group members, so
// only NPC spawns in this zone count as aggressors.
static json BuildAggressorsJson()
{
	json arr = json::array();
	int  slots = static_cast<int>(TloInt("Me.XTargetSlots"));
	for (int i = 1; i <= slots; ++i)
	{
		int64_t id = TloInt(fmt::format("Me.XTarget[{}].ID", i));
		if (id <= 0)
			continue;
		std::string spawn = fmt::format("Spawn[id {}]", id);
		if (Tlo(spawn + ".Type") != "NPC")
			continue;
		arr.push_back({
			{"name",     Tlo(spawn + ".CleanName")},
			{"id",       id},
			{"level",    TloInt(spawn + ".Level")},
			{"distance", TloFloat(spawn + ".Distance3D")}
		});
	}
	return arr;
}

// Nearest spawns within 300 units, closest first, at most 60.
static json BuildSpawnsJson()
{
	json arr   = json::array();
	int  count = std::min(60, static_cast<int>(TloInt("SpawnCount[radius 300]")));
	for (int i = 1; i <= count; ++i)
	{
		int64_t id = TloInt(fmt::format("NearestSpawn[{},radius 300].ID", i));
		if (id <= 0)
			continue;
		std::string spawn = fmt::format("Spawn[id {}]", id);
		arr.push_back({
			{"id",       id},
			{"name",     Tlo(spawn + ".CleanName")},
			{"type",     Tlo(spawn + ".Type")},
			{"level",    TloInt(spawn + ".Level")},
			{"distance", TloFloat(spawn + ".Distance3D")},
			{"position", PositionJson(spawn)}
		});
	}
	return arr;
}

// get_state, and the state attached to action tool replies. Chat is left out unless
// maxChat > 0 (chat_since is the way to read it); chat_seq is always there.
static json BuildStateJson(size_t maxChat)
{
	uint64_t chatSeq = ChatLastSeq();
	if (!InGame())
		return {{"inGame", false}, {"chat_seq", chatSeq}};

	json state = {
		{"inGame",         true},
		{"name",           Tlo("Me.CleanName")},
		{"level",          TloInt("Me.Level")},
		{"class",          Tlo("Me.Class.Name")},
		{"hp_pct",         TloInt("Me.PctHPs")},
		{"mana_pct",       TloInt("Me.PctMana")},
		{"endurance_pct",  TloInt("Me.PctEndurance")},
		{"moving",         TloBool("Me.Moving")},
		{"invisible",      TloBool("Me.Invis")},
		{"position",       PositionJson("Me")},
		{"zone",           Tlo("Zone.ShortName")},
		{"group_size",     TloInt("Me.GroupSize")},
		{"mem_spells",     BuildSpellsJson()},
		{"active_buffs",   BuildBuffsJson()},
		{"loot_available", BuildLootJson()},
		{"chat_seq",       chatSeq}   // pass to chat_since to read only what comes after
	};
	json aggressors = BuildAggressorsJson();
	state["aggro_count"] = aggressors.size();
	state["aggressors"]  = std::move(aggressors);

	json target = BuildTargetJson();
	if (!target.is_null())
		state["target"] = {
			{"name",     target["name"]},
			{"type",     target["type"]},
			{"distance", target["distance"]},
			{"hp_pct",   target["hp"]["pct"]},
			{"buffs",    target["buffs"]}
		};

	if (maxChat > 0)
	{
		json chat = json::array();
		std::lock_guard lock(s_chatMutex);
		size_t skip = s_chatLines.size() > maxChat ? s_chatLines.size() - maxChat : 0;
		for (auto it = s_chatLines.begin() + skip; it != s_chatLines.end(); ++it)
			chat.push_back(it->text);
		state["recent_chat"] = std::move(chat);
	}
	return state;
}

// One builder's result, or err = "Not in game". The task captures nothing by reference:
// after a timeout it can still run on a later pulse, when this frame is gone.
static bool ReadGameState(json (*build)(), json& out, std::string& err)
{
	json wrapped;
	if (!RunOnGameThread([build] { return InGame() ? json{{"data", build()}} : json(); }, wrapped, err))
		return false;
	if (wrapped.is_null())
	{
		err = "Not in game";
		return false;
	}
	out = std::move(wrapped["data"]);
	return true;
}

//============================================================================
// Client registry -- which character is on which port
//
// Each client records itself in MQMCP_Clients.ini as [Clients] <port>=<name>,<server>,<pid>
// (name and server are empty at char select). It is a separate file from MQMCP.ini on
// purpose: the profile API rewrites the whole file on every write, so clients writing at
// the same moment can drop each other's change. That is harmless here -- every client
// rechecks its entry every few seconds and rewrites it -- but not for pins and settings.
// An entry can outlive a crashed client, so readers confirm each one with GET /whoami.
//============================================================================

struct ClientIdentity
{
	std::string name;     // empty at char select
	std::string server;
};

static ClientIdentity s_identity;   // set on the game thread (OnPulse), read by HTTP workers
static std::mutex     s_identityMutex;

static const std::string& ClientsIniPath()
{
	static const std::string path = fmt::format("{}\\MQMCP_Clients.ini", gPathConfig);
	return path;
}

static std::string ClientEntry()
{
	std::lock_guard lock(s_identityMutex);
	return fmt::format("{},{},{}", s_identity.name, s_identity.server, ::GetCurrentProcessId());
}

static std::string ReadClientEntry(int port)
{
	char buf[256] = { 0 };
	::GetPrivateProfileStringA("Clients", std::to_string(port).c_str(), "", buf, sizeof(buf), ClientsIniPath().c_str());
	return buf;
}

// Write our entry if it is missing or differs: out of date, or lost to another client's write.
static void PublishClient()
{
	int port = s_port;
	if (!port)
		return;
	std::string entry = ClientEntry();
	if (ReadClientEntry(port) != entry)
		::WritePrivateProfileStringA("Clients", std::to_string(port).c_str(), entry.c_str(), ClientsIniPath().c_str());
}

// Remove the entry for a port we are leaving, unless another process has taken it over since.
static void UnpublishClient(int port)
{
	if (!port)
		return;
	std::string entry = ReadClientEntry(port);
	if (entry.ends_with(fmt::format(",{}", ::GetCurrentProcessId())))
		::WritePrivateProfileStringA("Clients", std::to_string(port).c_str(), nullptr, ClientsIniPath().c_str());
}

static json WhoamiJson()
{
	std::lock_guard lock(s_identityMutex);
	return {
		{"name",    s_identity.name},
		{"server",  s_identity.server},
		{"in_game", !s_identity.name.empty()},
		{"port",    s_port.load()},
		{"pid",     ::GetCurrentProcessId()}
	};
}

// Every registered client, confirmed live with GET /whoami. The checks run in parallel,
// since a dead port costs the whole connect timeout. Entries that don't answer are removed.
static json ListClients()
{
	std::vector<char> buf(16 * 1024);
	DWORD len = ::GetPrivateProfileSectionA("Clients", buf.data(), static_cast<DWORD>(buf.size()), ClientsIniPath().c_str());

	struct Probe { int port; std::string entry; std::future<json> answer; };
	std::vector<Probe> probes;
	for (const char* p = buf.data(); p < buf.data() + len && *p; p += strlen(p) + 1)
	{
		std::string_view kv(p);
		size_t eq = kv.find('=');
		int port = eq == std::string_view::npos ? 0 : GetIntFromString(std::string(kv.substr(0, eq)), 0);
		if (port <= 0)
			continue;
		if (port == s_port)
		{
			std::promise<json> self;
			self.set_value(WhoamiJson());
			probes.push_back({port, std::string(kv.substr(eq + 1)), self.get_future()});
			continue;
		}
		probes.push_back({port, std::string(kv.substr(eq + 1)), std::async(std::launch::async, [port] {
			httplib::Client cli("127.0.0.1", port);
			cli.set_connection_timeout(0, 300000);
			cli.set_read_timeout(2, 0);
			auto res = cli.Get("/whoami");
			if (!res || res->status != 200)
				return json();
			json j = json::parse(res->body, nullptr, false);
			return j.is_discarded() ? json() : j;
		})});
	}

	json clients = json::array();
	int  removed = 0;
	for (Probe& probe : probes)
	{
		json who = probe.answer.get();
		if (who.is_object())
		{
			who["self"] = probe.port == s_port;
			clients.push_back(std::move(who));
		}
		else if (ReadClientEntry(probe.port) == probe.entry)
		{
			::WritePrivateProfileStringA("Clients", std::to_string(probe.port).c_str(), nullptr, ClientsIniPath().c_str());
			++removed;
		}
	}
	return {{"clients", std::move(clients)}, {"removed_stale", removed}};
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

		json content;
		if (uri == "eq://chat")
		{
			content = json::array();
			std::lock_guard chatLock(s_chatMutex);
			for (const ChatLine& line : s_chatLines)
				content.push_back(line.text);
		}
		else
		{
			// eq://spells and eq://cooldowns are the same list: gems with their recast state.
			json (*build)() = nullptr;
			if      (uri == "eq://me")        build = BuildPlayerJson;
			else if (uri == "eq://zone")      build = BuildZoneJson;
			else if (uri == "eq://target")    build = BuildTargetJson;
			else if (uri == "eq://spawns")    build = BuildSpawnsJson;
			else if (uri == "eq://spells")    build = BuildSpellsJson;
			else if (uri == "eq://cooldowns") build = BuildSpellsJson;
			else if (uri == "eq://loot")      build = BuildLootJson;
			else if (uri == "eq://group")     build = BuildGroupJson;
			else if (uri == "eq://buffs")     build = BuildBuffsJson;
			else return MakeError(id, -32602, "Unknown resource URI: " + uri);

			std::string err;
			if (!ReadGameState(build, content, err))
				return MakeError(id, -32000, err);
		}

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
				{"description", "Get current game state: name, level, class, HP%, mana%, endurance%, movement, zone, "
				                "target (with buffs), XTarget aggressors, memorized spells with recast state, buffs, "
				                "loot and group size. Read live through MQ's TLOs on one game frame. Chat is not "
				                "included by default: use chat_since with the returned chat_seq, or max_chat."},
				{"inputSchema", {
					{"type",       "object"},
					{"properties", {{"max_chat", {{"type","integer"},{"description","Also return this many of the newest chat lines (default 0)"}}}}}
				}}
			},
			{
				{"name",        "execute_command"},
				{"description", "Queue any MacroQuest slash command (e.g. /say hello, /cast \"Complete Heal\") and "
				                "return at once, before it runs. To see its output use run_command, or pass the "
				                "returned chat_seq to chat_since."},
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
				                "Note TLO index syntax uses [brackets], not (parens): "
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
			},
			{
				{"name",        "chat_since"},
				{"description", "Return chat lines newer than a sequence number, oldest first. Every buffered "
				                "line has a monotonic 'seq'; get_state reports the newest as 'chat_seq'. Pass "
				                "the 'next' value from the previous call as 'since' to read only new lines. "
				                "'missed' counts lines that were evicted from the 4000-line buffer before you "
				                "read them; 'reset' means the plugin reloaded and numbering restarted. "
				                "'more' means max_lines was hit: call again with 'next'. Each line carries "
				                "its EQ chat 'channel' (e.g. raid, guild, melee_others_hits, your_dots; MQ "
				                "output is 'mq') and the 'tab' it shows in on this client (MQ output is "
				                "tab 'MQ'); filter on either. Use chat_tabs to see the tab names."},
				{"inputSchema", {
					{"type",       "object"},
					{"properties", {
						{"since",     {{"type","integer"},{"description","Return lines with seq greater than this (default 0 = all buffered)"}}},
						{"filter",    {{"type","string"}, {"description","Only lines containing this text (case-insensitive)"}}},
						{"tabs",      {{"type","array"},  {"items",{{"type","string"}}}, {"description","Only lines shown in these chat tabs"}}},
						{"channels",  {{"type","array"},  {"items",{{"type","string"}}}, {"description","Only lines on these channels"}}},
						{"exclude",   {{"type","array"},  {"items",{{"type","string"}}}, {"description","Drop lines whose tab or channel is listed"}}},
						{"max_lines", {{"type","integer"},{"description","Cap on lines returned (default 200)"}}}
					}}
				}}
			},
			{
				{"name",        "chat_tabs"},
				{"description", "List this client's chat tabs and the channels routed to each, as read from "
				                "the character's UI ini. Use the tab and channel names with chat_since / "
				                "run_command filters."},
				{"inputSchema", {{"type","object"},{"properties",json::object()}}}
			},
			{
				{"name",        "run_command"},
				{"description", "Run a slash command and return the chat output that follows it. Waits until the "
				                "command has run on the game thread, then for wait_ms more (default 500) so game "
				                "replies such as spell results arrive, then returns every chat line since the "
				                "command was queued (optionally filtered). Use this instead of execute_command "
				                "whenever you need to see what the command did. Output includes 'next' for a "
				                "follow-up chat_since."},
				{"inputSchema", {
					{"type",       "object"},
					{"properties", {
						{"command",   {{"type","string"}, {"description","The full command including the leading slash"}}},
						{"wait_ms",   {{"type","integer"},{"description","Extra time to collect output after the command runs, 0-10000 (default 500)"}}},
						{"filter",    {{"type","string"}, {"description","Only lines containing this text (case-insensitive)"}}},
						{"tabs",      {{"type","array"},  {"items",{{"type","string"}}}, {"description","Only lines shown in these chat tabs"}}},
						{"channels",  {{"type","array"},  {"items",{{"type","string"}}}, {"description","Only lines on these channels"}}},
						{"exclude",   {{"type","array"},  {"items",{{"type","string"}}}, {"description","Drop lines whose tab or channel is listed"}}},
						{"max_lines", {{"type","integer"},{"description","Cap on lines returned (default 200)"}}}
					}},
					{"required",   {"command"}}
				}}
			},
			{
				{"name",        "wait_for"},
				{"description", "Wait until a TLO expression becomes true, checking it on every game pulse, "
				                "and return as soon as it does or when timeout_ms passes. Without 'equals', "
				                "the condition is true when the value is TRUE or a non-zero number; NULL, FALSE, "
				                "0 and unresolved count as false. With 'equals', it is true when the value equals "
				                "that text (case-insensitive), e.g. expr Navigation.Active equals FALSE, or "
				                "Me.Casting equals NULL. For comparisons use a TLO that evaluates them, e.g. "
				                "\"${Math.Calc[${Me.PctHPs} > 80]}\". Returns met, the last value and elapsed_ms."},
				{"inputSchema", {
					{"type",       "object"},
					{"properties", {
						{"expr",       {{"type","string"}, {"description","TLO expression, with or without surrounding ${}"}}},
						{"equals",     {{"type","string"}, {"description","Wait for this exact value instead of a truthy one"}}},
						{"timeout_ms", {{"type","integer"},{"description","Give up after this long, 0-60000 (default 5000)"}}}
					}},
					{"required",   {"expr"}}
				}}
			},
			{
				{"name",        "list_clients"},
				{"description", "List the EQ clients running this plugin on this machine: character name "
				                "(empty at char select), server, MCP port and process id, each confirmed live. "
				                "'self' marks this client."},
				{"inputSchema", {{"type","object"},{"properties",json::object()}}}
			},
			{
				{"name",        "query_peer"},
				{"description", "Evaluate TLO expressions on another character's client (by name), all on one "
				                "of its game frames -- query_tlo_batch run on that client. Use instead of "
				                "DanNet /dquery or /dex round trips."},
				{"inputSchema", {
					{"type",       "object"},
					{"properties", {
						{"name",  {{"type","string"}, {"description","Character name, case-insensitive"}}},
						{"exprs", {{"type","array"},  {"items",{{"type","string"}}}, {"description","TLO expressions, each with or without surrounding ${}"}}}
					}},
					{"required",   {"name", "exprs"}}
				}}
			}
		}}});
	}

	if (method == "tools/call")
	{
		const std::string tool = req.value("/params/name"_json_pointer, "");
		const json        args = req.value("/params/arguments"_json_pointer, json::object());

		// Helper: build a response that includes current game state, built on the pulse that
		// runs the queued command (commands drain first), so it reflects the command having
		// been issued; a short chat tail rides along. chatSeq, taken *before* queueing,
		// replaces the state's own: the command's output must land after the cursor.
		auto respondWithState = [&](std::string msg, uint64_t chatSeq) {
			json        state;
			std::string err;
			if (!RunOnGameThread([] { return BuildStateJson(k_actionChatLines); }, state, err))
				state = {{"error", err}};
			state["chat_seq"] = chatSeq;
			std::string text = msg + "\n\nCurrent state:\n" + state.dump(2);
			return MakeOk(id, {{"content", {{{"type","text"},{"text", std::move(text)}}}}});
		};

		if (tool == "get_state")
		{
			size_t maxChat = 0;
			if (args.contains("max_chat") && args["max_chat"].is_number_integer())
				maxChat = static_cast<size_t>(std::clamp<int64_t>(args["max_chat"].get<int64_t>(), 0, k_chatBufferSize));
			json        state;
			std::string err;
			if (!RunOnGameThread([maxChat] { return BuildStateJson(maxChat); }, state, err))
				return MakeError(id, -32000, err);
			return MakeOk(id, {{"content", {{{"type","text"},{"text", state.dump(2)}}}}});
		}

		if (tool == "execute_command")
		{
			std::string cmd = args.value("command", "");
			if (cmd.empty())
				return MakeError(id, -32602, "command is required");
			uint64_t seq = ChatLastSeq();
			EnqueueCommand(cmd);
			return respondWithState("Queued: " + cmd, seq);
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
			uint64_t seq = ChatLastSeq();
			EnqueueCommand(cmd);
			return respondWithState("Navigating: " + cmd, seq);
		}

		if (tool == "say")
		{
			std::string text = args.value("text", "");
			if (text.empty())
				return MakeError(id, -32602, "text is required");
			uint64_t seq = ChatLastSeq();
			EnqueueCommand("/say " + text);
			return respondWithState("Said: " + text, seq);
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

		// Integer argument that tolerates a numeric string and clamps to [lo, hi].
		auto intArg = [&](const char* key, int64_t def, int64_t lo, int64_t hi) -> int64_t {
			int64_t v = def;
			if (args.contains(key))
			{
				const json& a = args[key];
				if (a.is_number())      v = a.get<int64_t>();
				else if (a.is_string()) { try { v = std::stoll(a.get<std::string>()); } catch (...) {} }
			}
			return std::clamp(v, lo, hi);
		};

		// Waits are sliced so a server stop -- which joins this worker from the game thread,
		// where no pulse can run -- is never held up by a sleeping request.
		auto sleepUnlessStopping = [](int64_t ms) {
			auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
			while (!s_stopping && std::chrono::steady_clock::now() < until)
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
		};

		// filter / tabs / channels / exclude. A list may also be given as one string.
		auto chatFilterArg = [&]() {
			auto list = [&](const char* key) {
				std::vector<std::string> out;
				if (!args.contains(key))
					return out;
				const json& a = args[key];
				if (a.is_string())
					out.push_back(a.get<std::string>());
				else if (a.is_array())
					for (const json& e : a)
						if (e.is_string())
							out.push_back(e.get<std::string>());
				return out;
			};
			ChatFilter f;
			f.text     = args.contains("filter") && args["filter"].is_string() ? args["filter"].get<std::string>() : "";
			f.tabs     = list("tabs");
			f.channels = list("channels");
			f.exclude  = list("exclude");
			return f;
		};

		if (tool == "chat_since")
		{
			uint64_t    since    = static_cast<uint64_t>(intArg("since", 0, 0, INT64_MAX));
			size_t      maxLines = static_cast<size_t>(intArg("max_lines", 200, 1, k_chatBufferSize));
			json out = ChatSince(since, chatFilterArg(), maxLines);
			return MakeOk(id, {{"content", {{{"type","text"},{"text", out.dump(2)}}}}});
		}

		if (tool == "run_command")
		{
			std::string cmd = args.value("command", "");
			if (cmd.empty())
				return MakeError(id, -32602, "command is required");
			int64_t     waitMs   = intArg("wait_ms", 500, 0, 10000);
			size_t      maxLines = static_cast<size_t>(intArg("max_lines", 200, 1, k_chatBufferSize));
			ChatFilter  filter   = chatFilterArg();

			uint64_t since = ChatLastSeq();
			auto     done  = std::make_shared<std::promise<void>>();
			auto     fut   = done->get_future();
			EnqueueCommand(cmd, done);

			// Same 5s budget as a TLO query: a pulse that hasn't come by then isn't coming soon.
			auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (fut.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready)
			{
				if (s_stopping)
					return MakeError(id, -32000, "Server stopping; the command may not have run");
				if (std::chrono::steady_clock::now() >= deadline)
					return MakeError(id, -32000, "Command not run within 5s (is the client pulsing?). "
					                             "It is still queued and will run on the next pulse.");
			}
			sleepUnlessStopping(waitMs);

			json out = ChatSince(since, filter, maxLines);
			out["command"] = cmd;
			return MakeOk(id, {{"content", {{{"type","text"},{"text", out.dump(2)}}}}});
		}

		if (tool == "chat_tabs")
		{
			json tabs = json::object();
			json out;
			{
				std::lock_guard lock(s_layoutMutex);
				for (int ch = 0; ch < k_maxChatChannels; ++ch)
				{
					int w = s_chatLayout.windowOf[ch];
					if (w < 0 || w >= MAX_CHAT_WINDOWS)
						continue;
					std::string tab = s_chatLayout.names[w].empty() ? fmt::format("Window {}", w) : s_chatLayout.names[w];
					tabs[tab].push_back(ChannelName(ch));
				}
				out = {{"loaded", s_chatLayout.loaded}, {"file", s_chatLayout.file}, {"tabs", tabs}};
			}
			out["note"] = "MQ output is tab \"MQ\", channel \"mq\". Layout is read at login and by /mcp chat reload.";
			return MakeOk(id, {{"content", {{{"type","text"},{"text", out.dump(2)}}}}});
		}

		if (tool == "wait_for")
		{
			const std::string expr = args.value("expr", "");
			if (expr.empty())
				return MakeError(id, -32602, "expr is required");
			const bool        hasEquals = args.contains("equals") && args["equals"].is_string();
			const std::string equals    = hasEquals ? args["equals"].get<std::string>() : "";
			const int64_t     timeoutMs = intArg("timeout_ms", 5000, 0, 60000);
			const std::string parsed    = wrapExpr(expr);

			// MQ renders booleans as TRUE/FALSE and numbers as text ("0", "0.00").
			auto isTruthy = [&](const std::string& v) {
				if (!isResolved(v, parsed) || v.empty() || ci_equals(v, "FALSE"))
					return false;
				char* end = nullptr;
				double d = std::strtod(v.c_str(), &end);
				if (end != v.c_str() && *end == '\0')
					return d != 0.0;
				return true;
			};

			// Each evaluation waits for its own pulse, so this loop runs at the pulse rate.
			auto        start = std::chrono::steady_clock::now();
			std::string value;
			bool        met   = false;
			int         polls = 0;
			for (;;)
			{
				std::vector<std::string> values;
				std::string err;
				if (!evaluateOnGameThread({parsed}, values, err) || values.empty())
					return MakeError(id, -32000, err.empty() ? "No result returned" : err);
				value = values[0];
				++polls;
				met = hasEquals ? ci_equals(value, equals) : isTruthy(value);

				auto elapsed = std::chrono::steady_clock::now() - start;
				if (met || s_stopping || elapsed >= std::chrono::milliseconds(timeoutMs))
					break;
			}

			auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start).count();
			json out = {
				{"expr",       parsed},
				{"met",        met},
				{"value",      value},
				{"resolved",   isResolved(value, parsed)},
				{"elapsed_ms", elapsedMs},
				{"polls",      polls}
			};
			return MakeOk(id, {{"content", {{{"type","text"},{"text", out.dump(2)}}}}});
		}

		if (tool == "list_clients")
		{
			json out = ListClients();
			return MakeOk(id, {{"content", {{{"type","text"},{"text", out.dump(2)}}}}});
		}

		if (tool == "query_peer")
		{
			const std::string name = args.value("name", "");
			if (name.empty())
				return MakeError(id, -32602, "name is required");
			if (!args.contains("exprs") || !args["exprs"].is_array())
				return MakeError(id, -32602, "exprs must be an array of strings");

			int  port    = 0;
			json clients = ListClients();   // named: looping over ListClients()["clients"] would outlive the temporary
			for (const json& c : clients["clients"])
				if (ci_equals(c.value("name", ""), name))
					port = c.value("port", 0);
			if (!port)
				return MakeError(id, -32000, fmt::format("{} is not on any running client (see list_clients)", name));

			// The peer answers on its next pulse, within its own 5s TLO timeout.
			httplib::Client cli("127.0.0.1", port);
			cli.set_connection_timeout(1, 0);
			cli.set_read_timeout(8, 0);
			json call = {{"jsonrpc","2.0"}, {"id",1}, {"method","tools/call"},
			             {"params", {{"name","query_tlo_batch"}, {"arguments", {{"exprs", args["exprs"]}}}}}};
			auto res = cli.Post("/mcp", call.dump(), "application/json");
			if (!res || res->status != 200)
				return MakeError(id, -32000, fmt::format("{} on port {} did not answer", name, port));
			json reply = json::parse(res->body, nullptr, false);
			if (reply.is_discarded())
				return MakeError(id, -32000, "Unreadable reply from " + name);
			if (reply.contains("error"))
				return MakeError(id, reply["error"].value("code", -32000), name + ": " + reply["error"].value("message", ""));

			json out = json::parse(reply.value("/result/content/0/text"_json_pointer, std::string("{}")), nullptr, false);
			if (out.is_discarded())
				return MakeError(id, -32000, "Unreadable reply from " + name);
			out["name"] = name;
			out["port"] = port;
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

	// Which character this port belongs to. Cheap, and answers at char select too.
	s_server->Get("/whoami", [](const httplib::Request&, httplib::Response& res) {
		res.set_content(WhoamiJson().dump(), "application/json");
	});

	s_server->Get("/.well-known/oauth-authorization-server", [](const httplib::Request&, httplib::Response& res) {
		res.status = 404;
		res.set_content(R"({"error":"not_found"})", "application/json");
	});

	// -----------------------------------------------------------------------
	// Convenience read-only REST endpoints (useful for debugging)
	// -----------------------------------------------------------------------
	auto gameGet = [](httplib::Response& res, json (*build)()) {
		json        out;
		std::string err;
		if (!ReadGameState(build, out, err))
		{
			res.status = 503;
			res.set_content(json{{"error", err}}.dump(), "application/json");
			return;
		}
		res.set_content(out.dump(2), "application/json");
	};

	s_server->Get("/me",     [gameGet](const httplib::Request&, httplib::Response& res) { gameGet(res, BuildPlayerJson); });
	s_server->Get("/zone",   [gameGet](const httplib::Request&, httplib::Response& res) { gameGet(res, BuildZoneJson);   });
	s_server->Get("/target", [gameGet](const httplib::Request&, httplib::Response& res) { gameGet(res, BuildTargetJson); });
	s_server->Get("/spawns", [gameGet](const httplib::Request&, httplib::Response& res) { gameGet(res, BuildSpawnsJson); });
}

//============================================================================
// Server lifecycle / port selection
//============================================================================

// httplib's default socket options leave a REUSEADDR socket on Windows: 0.15.3 set
// SO_REUSEADDR then SO_EXCLUSIVEADDRUSE (Winsock rejects the second, unchecked), and
// 0.41 sets SO_REUSEADDR alone. Either way every boxed client bound 8284 and the newest
// silently took over connections. Exclusive only, so a port already in use fails to
// bind and we move on to the next.
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
		{
			std::lock_guard lock(s_taskMutex);
			std::queue<std::shared_ptr<GameTask>>().swap(s_taskQueue);
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
	UnpublishClient(s_port);
	s_port = 0;
}

// Binds `preferred` if given and free, else the first free port in the scan range.
// Returns the bound port, or 0 if nothing in the range was free.
static int StartServer(int preferred)
{
	StopServer();

	// A fresh Server per attempt: in httplib 0.41 a failed bind_to_port decommissions
	// the Server, and every later bind on it fails without trying -- so one taken port
	// (a pin, or 8284 held by another client) used to end the scan with "no free port".
	auto tryBind = [](int p) {
		s_server = std::make_unique<httplib::Server>();
		s_server->set_socket_options(ExclusiveSocketOptions);
		SetupRoutes();
		if (s_server->bind_to_port("127.0.0.1", p))
			return true;
		s_server.reset();
		return false;
	};

	int port = 0;
	if (preferred > 0 && tryBind(preferred))
		port = preferred;
	for (int p = k_defaultPort; !port && p < k_defaultPort + k_portSpan; ++p)
		if (tryBind(p))
			port = p;

	if (!port)
		return 0;

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
	PublishClient();
	return port;
}

static std::string CurrentCharName()
{
	if (GetGameState() != GAMESTATE_INGAME || !pLocalPlayer)
		return {};
	return pLocalPlayer->Name;
}

static void SetIdentity(ClientIdentity id)
{
	{
		std::lock_guard lock(s_identityMutex);
		s_identity = std::move(id);
	}
	PublishClient();
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

	// /mcp chat reload   re-read this character's chat tab layout from the UI ini
	if (ci_equals(arg1, "chat"))
	{
		if (ci_equals(arg2, "reload"))
			LoadChatLayout();
		std::lock_guard lock(s_layoutMutex);
		if (!s_chatLayout.loaded)
			WriteChatf("\ag[MQMCP]\ax Chat layout not loaded%s%s   (/mcp chat reload)",
				s_chatLayout.file.empty() ? "" : " from ", s_chatLayout.file.c_str());
		else
		{
			int tabs = static_cast<int>(std::count_if(s_chatLayout.names.begin(), s_chatLayout.names.end(),
				[](const std::string& n) { return !n.empty(); }));
			WriteChatf("\ag[MQMCP]\ax Chat layout: \aw%d\ax tabs from \ay%s\ax   (/mcp chat reload)",
				tabs, s_chatLayout.file.c_str());
		}
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

	// Game state summary (commands run on the game thread, so the TLOs can be read directly)
	bool inGame = InGame();
	std::string charInfo;
	if (inGame)
		charInfo = fmt::format("{} ({}), zone: {}, HP: {}%, mana: {}%", Tlo("Me.CleanName"),
			Tlo("Me.Level"), Tlo("Zone.ShortName"), Tlo("Me.PctHPs"), Tlo("Me.PctMana"));

	WriteChatf("\ag[MQMCP]\ax ─────────────────────────────────");
	int pin = PinnedPort(CurrentCharName());
	WriteChatf("\ag[MQMCP]\ax  URL    : \ayhttp://127.0.0.1:%d/mcp\ax  (%s)", s_port.load(),
		pin > 0 ? fmt::format("pinned {}", pin).c_str() : "auto");
	WriteChatf("\ag[MQMCP]\ax  Uptime : \aw%02d:%02d:%02d\ax", hours, minutes, secs);
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
		PushChatLine(Line, static_cast<int>(Color), true);
	return 0;
}

// Captures all incoming EQ chat: combat messages, spells, skills, say, tells, etc.
PLUGIN_API BOOL OnIncomingChat(const char* Line, DWORD Color)
{
	if (Line && *Line)
		PushChatLine(Line, static_cast<int>(Color), false);
	return FALSE;  // don't suppress — let EQ display it normally
}

PLUGIN_API void InitializePlugin()
{
	s_startTime = std::chrono::steady_clock::now();

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
	// Drain and execute queued commands on the main thread
	std::queue<QueuedCommand> toRun;
	{
		std::lock_guard lock(s_queueMutex);
		std::swap(toRun, s_commandQueue);
	}

	while (!toRun.empty())
	{
		QueuedCommand& qc = toRun.front();
		DoCommand(qc.cmd.c_str(), false);
		// Output DoCommand writes synchronously is already in the chat buffer by now.
		if (qc.done)
			try { qc.done->set_value(); } catch (...) {}
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

	// Drain game-state builds (get_state, eq:// resources, REST). After the commands, so a
	// state requested alongside a command already reflects it.
	std::queue<std::shared_ptr<GameTask>> tasks;
	{
		std::lock_guard lock(s_taskMutex);
		std::swap(tasks, s_taskQueue);
	}
	while (!tasks.empty())
	{
		auto task = std::move(tasks.front());
		tasks.pop();
		try { task->result.set_value(task->build()); }
		catch (...) { try { task->result.set_exception(std::current_exception()); } catch (...) {} }
	}

	// Apply the character's pinned port and load its chat layout once per entry into the
	// game. Polled here rather than in SetGameState because pLocalPlayer is not guaranteed
	// to be set on the INGAME transition. Runs after the TLO drain so a rebind has no
	// queries left to strand.
	static bool s_pinChecked = false;
	if (CurrentCharName().empty())
	{
		if (s_pinChecked)
		{
			LoadChatLayout();   // not in game: clears it, so char-select lines aren't tagged with old tabs
			SetIdentity({});
		}
		s_pinChecked = false;
	}
	else if (!s_pinChecked)
	{
		s_pinChecked = true;
		LoadChatLayout();
		SetIdentity({CurrentCharName(), GetServerShortName()});
		ApplyPinnedPort();
	}

	// Re-assert our registry entry now and then, in case another client's write dropped it.
	static auto s_nextPublish = std::chrono::steady_clock::now();
	if (std::chrono::steady_clock::now() >= s_nextPublish)
	{
		s_nextPublish = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		PublishClient();
	}
}
