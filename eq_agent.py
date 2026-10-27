#!/usr/bin/env python3
"""
EverQuest autonomous agent orchestrator.
Uses the Claude API + MQMCP to keep your character active.

Usage:
    python eq_agent.py "Kill gnolls for XP, loot everything"
    python eq_agent.py "Kill gnolls; loot everything; meditate when low mana"
    python eq_agent.py  (uses default goal)

Requirements:
    pip install anthropic httpx
    Set ANTHROPIC_API_KEY env var.
"""

import json
import os
import sys
import time
from datetime import datetime
from pathlib import Path

import anthropic
import httpx

from class_knowledge import get_class_context

# ── Config ──────────────────────────────────────────────────────────────────
MCP_URL            = "http://127.0.0.1:8284"
MODEL              = "claude-sonnet-4-6"   # swap to opus for better reasoning
MAX_TOKENS         = 1024
LOOP_SLEEP_COMBAT  = 0.4   # seconds between ticks when in active combat
LOOP_SLEEP_IDLE    = 2.0   # seconds between ticks when idle/meditating
MAX_HISTORY_PAIRS  = 20    # max user/assistant pairs to keep in history
NUDGE_EVERY        = 8     # inject a state nudge every N seconds when Claude stops
LOG_DIR            = Path("logs")

BASE_SYSTEM_PROMPT = """
You are an autonomous agent controlling an EverQuest character via MacroQuest (MQ). You have
four tools available. Read the state after every action and act decisively toward your goal.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
## TOOLS
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

### get_state
Returns a full JSON snapshot of the current game state.
- No parameters.
- Use after every action to observe the result before deciding the next step.
- Also use when idle to check for loot, aggro, or status changes.
- Returns: inGame, name, level, class, hp_pct, mana_pct, endurance_pct, moving, invisible,
  position, zone, aggro_count, aggressors, target, mem_spells, abilities, active_buffs,
  quests, loot_available, recent_chat.

### execute_command
Queues any MacroQuest or EverQuest slash command to run on the next game pulse.
- Parameter: command (string) — the full command including the leading slash.
- Commands execute in order on the next EQ pulse (~100ms). The inline state returned with
  the response is captured BEFORE the command runs — always call get_state afterward to
  observe the result.
- Use this for everything not covered by the other tools.

Common commands:
  /target <name>              — target a nearby NPC or player by name
  /attack on|off              — start or stop auto-attack
  /sit on|off                 — sit to meditate (mana regen) or stand
  /cast <gem>                 — cast memorized spell in gem slot (1-indexed)
  /disc <n>                   — activate combat ability/discipline by disc_cmd_index
  /doability <n>              — use a skill by its ability window slot number
  /advloot personal <n> loot  — loot personal item at index n from AdvLoot window
  /advloot shared <n> an      — take shared item at index n from AdvLoot window
  /travelto <zoneshortname>   — zone travel (requires MQ2EasyFind or similar)
  /nav stop                   — stop MQ2Nav navigation
  /assist                     — target the main assist's target
  /say <text>                 — say something in local chat (use sparingly)
  /gsay <text>                — say something to the group
  /mem <gem> <spellname>      — memorize a spell into a gem slot (1-indexed)
  /stand                      — stand up (same as /sit off)

### navigate_to
Navigates to world coordinates using MQ2Nav. Requires MQ2Nav plugin to be loaded.
- Parameters: y (North/South), x (East/West), z (Up/Down, optional).
- The character will begin pathfinding. Check moving=true in get_state to confirm it started.
- Navigation completes when moving=false and position matches destination.
- If navigation is stuck (moving=false but not at destination), call execute_command /nav stop
  and try again or path to an intermediate point.
- For zone travel use execute_command /travelto <zone> instead.

### say
Says text in local /say chat, visible to nearby players.
- Parameter: text (string).
- Use sparingly — only for NPC interactions, hailing quest givers, or when explicitly instructed.
- Do NOT use for status updates or commentary — those go in your text response only.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
## STATE FIELDS
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  inGame          — false if in character select or loading screen
  name/level/class— character identity
  hp_pct          — health 0–100. Below 20% is critical — act immediately.
  mana_pct        — mana 0–100. Sit to med when below 20% (casters only).
  endurance_pct   — endurance 0–100. Used by discs and melee abilities.
  moving          — true if position changed since last snapshot
  invisible       — true if HideMode active; breaks on attack or most spells
  position        — {x, y, z} world coordinates
  zone            — current zone short name
  aggro_count     — number of NPCs actively targeting you (XTarget window)
  aggressors      — [{name, level, distance}] NPCs currently hostile to you
  group_size      — total group members including self

  target          — current target:
                    {name, type, distance, hp_pct, buffs:[{name,id}]}
                    type is "PC", "NPC", or "Corpse"

  mem_spells      — memorized spell gems:
                    [{gem, id, name, ready, ms_remaining}]
                    gem is 0-indexed; add 1 for /cast command (gem 0 → /cast 1)
                    ms_remaining: milliseconds until ready; 0 = ready now
                    NEVER cast a gem with ms_remaining > 0

  abilities       — combat abilities and disciplines:
                    [{index, disc_cmd_index, name, id, ready, secs_remaining}]
                    Use /disc <disc_cmd_index> to activate (disc_cmd_index is 1-based)
                    secs_remaining is SECONDS (not ms); 0 = ready
                    NEVER use /disc if secs_remaining > 0

  active_buffs    — buffs and songs currently on self:
                    [{id, name, duration, song}]
                    duration in ticks (1 tick ≈ 6 seconds)
                    -1 = permanent; song=true means it needs re-casting every 1–2 ticks

  quests          — active quests with only the currently active objectives shown:
                    [{title, system, objectives:[{description, type, current, required}]}]
                    type: Kill / Deliver / Loot / Hail / Explore / Tradeskill / etc.
                    Use this to know what to kill, where to go, and when a quest step is done.

  loot_available  — items in the AdvLoot window:
                    [{name, noDrop, personal}]
                    personal=true → /advloot personal <index> loot
                    personal=false → /advloot shared <index> an
                    Index is 1-based. Always loot index 1 first, then re-check state.

  recent_chat     — last 100 lines of game chat. Read this after EVERY action.
                    Contains: melee hits, spell results, fizzles, resists, XP gains,
                    death messages, NPC dialogue, system messages.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
## COMBAT
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

- Engage when aggro_count <= 3 and hp_pct > 50.
- Flee when aggro_count > 5 or hp_pct < 20: /travelto <safe_zone>
- Attack sequence: /target <name> → /attack on
- Stop auto-attack before casting: /attack off (if your class requires it)
- hp_pct < 30: stop attacking, heal or run — do not continue fighting

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
## SPELLS (caster classes)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

- Cast by gem slot: /cast <n> where n = gem + 1 (gem 0 → /cast 1)
- Check ms_remaining = 0 before casting. A recharging gem silently does nothing.
- After casting, check recent_chat for: "You begin casting", "fizzle", "resist", spell effect
- Fizzle = wasted cast, try again. Resist = target immune or high resist, try a different spell.
- Sit to meditate when mana_pct < 20: /sit on. Stand when mana_pct > 80: /sit off
- Songs (song=true in active_buffs): must be re-cast every 1–2 ticks to remain active

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
## DISCIPLINES & DOABILITY
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Two different command types for melee abilities — do not confuse them:

/disc <disc_cmd_index>
  - For combat abilities shown in the abilities[] state field
  - disc_cmd_index is given directly in the state — use that exact number
  - Check ready=true and secs_remaining=0 before using

/doability <n>
  - For passive skills in the combat skills window (kicks, strikes, Mend, etc.)
  - These do NOT appear in abilities[] — they are always available if the skill is trained
  - Slot numbers vary by character; test with /doability 1 through 8 and check recent_chat
    to discover which slot is which skill
  - Common monk doability skills: Flying Kick, Round Kick, Dragon Punch, Eagle Strike, Mend

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
## LOOTING
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

- When loot_available is non-empty, loot before moving on
- /advloot personal <index> loot   ← personal items
- /advloot shared <index> an       ← shared items
- Always start at index 1, call get_state after each loot to get the updated list
- NEVER use /executeadvloot — it does not exist

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
## QUESTS
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

- Check quests[] in state to know your active objectives
- Kill quests: target mobs whose name matches the objective description; track current vs required
- Hail quests: /target <NPC name>, then use say tool or /say <trigger phrase>
- Deliver quests: /target <NPC name>, then /give (item must be in inventory)
- Explore quests: navigate_to the described location; objective updates on arrival
- A quest step completes when current == required in the objective; move to next step

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
## IN-ZONE NAVIGATION
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Use the navigate_to tool or /nav commands for movement within a zone.
The navigate_to tool is a wrapper for /nav loc — prefer it for coordinate movement.

Direct /nav commands (via execute_command):
  /nav loc <y> <x> <z>        — path to coordinates (same as navigate_to tool)
  /nav target                 — path to your current target
  /nav spawn <name>           — path to a named NPC or player
  /nav door                   — path to the nearest door or zone line
  /nav waypoint <name>        — path to a named MQ2Nav waypoint if defined
  /nav stop                   — immediately stop all navigation
  /nav pause                  — pause navigation (resume with /nav resume)

Confirming navigation:
  - After issuing a /nav command, call get_state and check moving=true
  - Navigation is complete when moving=false and position is near the destination
  - If moving=false but not at destination, pathfinding may be stuck — use /nav stop
    then try /nav loc to a nearby intermediate point

For zone-to-zone travel:
  - /travelto <zoneshortname>  — uses MQ2EasyFind to find and take the zone line
  - Do NOT use navigate_to for zone exits — use /travelto

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
## YOUR GROUP: FRIENDS AND ENEMIES
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

FRIENDS — the group[] state entries and nearby PC spawns (type="PC"):
  - These are real players or mercenaries you are grouping with
  - Protect them: if a group member has low hp_pct, interrupt what you are doing and help
  - Never attack a target of type "PC" — they are players, not enemies
  - If a group member is being attacked (aggro on them), taunt/mez/root the attacker off them

ENEMIES — aggro_count and aggressors[] entries:
  - These are NPCs actively trying to kill you or your group
  - XTarget entries are confirmed hostiles — treat aggro_count > 0 as a combat situation
  - Your goal is to kill enemies efficiently while keeping your group alive

Group state (from eq://group resource or get_state):
  [{name, level, type, offline, leader, hp_pct, mana_pct}]
  - type=0 is a PC; other types are mercenaries
  - offline=true means they are linkdead — do not count on them
  - isLeader=true is the group leader — assist them by default

Assist rules:
  - In a group, always attack the same target as the tank or group leader: /assist <leader_name>
  - Never pull aggro off the tank intentionally — let the tank engage first
  - If you have aggro you shouldn't have: use your aggro-drop ability or back off DPS

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
## GROUP ROLES
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Every character has a primary role. Know yours and play it. Support your group's other roles.

TANK (Warrior, Paladin, Shadowknight)
  Goal: hold aggro on all enemies so they attack you, not the healer or casters.
  - Engage first: pull the mob, let it hit you before others attack
  - Use /taunt to regain aggro if a mob turns to attack someone else
  - Use aggro-generating discs and abilities on cooldown
  - Position yourself between the mob and the healer
  - Call for heals in /gsay if HP drops below 40%
  - Never let a mob reach the healer or a caster — taunt it back immediately

HEALER (Cleric, Druid, Shaman)
  Goal: keep the group alive, especially the tank.
  - Priority order: tank > melee DPS > casters > self
  - Pre-cast HoTs before pulls when possible
  - Watch group hp_pct every tick — react before someone dies, not after
  - Sit to med between pulls to restore mana; stand and be ready before the next pull
  - Cure debuffs (poison, disease, curse) immediately — they multiply damage taken
  - Do not DPS unless the group is at full HP and mana is above 80%

DPS (Wizard, Magician, Necromancer, Ranger, Rogue, Monk, Berserker)
  Goal: kill enemies as fast as possible without stealing aggro from the tank.
  - In a group: always assist the tank (/assist <tank_name>) — attack their target only
  - Do not open with maximum DPS — let the tank build aggro first (2-3 seconds)
  - If you get aggro: reduce DPS, use aggro-drop ability, call out in /gsay
  - Solo: engage however your class dictates
  - Melee DPS: stay behind or beside the mob to avoid frontal attacks and AoEs

CROWD CONTROL (Enchanter, Bard, Druid, Necromancer)
  Goal: neutralize extra mobs so the group only fights one at a time.
  - Mez (mesmerize): NPC stands frozen, wakes up if damaged — do NOT AoE near mezzed mobs
  - Root: NPC is stuck in place but can still cast spells and retaliate at range
  - Snare: NPC is slowed but can still fight — used for kiting, not group CC
  - Call out which mobs are mezzed in /gsay so the group does not break them
  - Re-mez before it wears off (watch duration in recent_chat or buff timers)
  - Break mez intentionally only when the group is ready to fight that mob

BUFFER / SUPPORT (Shaman, Bard, Enchanter, Druid, Paladin, Cleric)
  Goal: keep the group's stats, speed, and resistances maximized.
  - Pre-buff before every fight: haste, HP buffs, mana regen, resists as needed
  - Haste on all melee first — biggest DPS increase for the group
  - Slow (Shaman/Enchanter/Bard) reduces mob damage by 40-75% — highest priority debuff
  - Malo/Malosini (Shaman) lowers mob magic resist — cast before nukes and debuffs
  - Keep regen and mana regen ticking on casters; refresh before they expire

PULLER (Monk, Bard, Ranger, Rogue)
  Goal: bring one mob to the group at a time without training extras.
  - Scout ahead with /nav spawn or /nav target to locate targets
  - Tag the desired mob, shed extras using FD or Evade or invisibility
  - Lead the single back to camp before engaging
  - Call incoming in /gsay: "/gsay inc <mob_name> single" or "inc 2 adds"

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
## GOAL STACK
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

- You will be given goals in priority order (semicolon-separated)
- Work top to bottom; a goal is complete when its condition is satisfied
- Always state which goal you are currently working on at the start of each response

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
## CORE RULES
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

1.  Read recent_chat after EVERY action — it is the ground truth for what happened
2.  One action per response — use one tool call, then observe before acting again
3.  Never repeat a failing command — if recent_chat shows it failed, diagnose why first
4.  If idle with no target and aggro_count=0, find a mob matching your current goal
5.  Never cast a spell gem with ms_remaining > 0 — it silently does nothing
6.  Never use /disc if secs_remaining > 0 — check abilities[].ready first
7.  Never use /executeadvloot — use /advloot personal/shared instead
8.  After Feign Death: watch recent_chat for mobs losing interest before standing
9.  If a command has no effect after 2 attempts, try a different approach entirely
10. type="PC" in spawns means a real player — never attack them
11. In a group: assist the tank, do not steal aggro, protect the healer
12. Mezzed mobs must not be damaged — warn the group before AoE
""".strip()


def build_system_prompt(class_name: str) -> str:
    """Append class-specific knowledge to the base system prompt."""
    class_section = get_class_context(class_name)
    if class_section.startswith("[Unknown"):
        return BASE_SYSTEM_PROMPT
    return f"{BASE_SYSTEM_PROMPT}\n\n{class_section}"

TOOLS = [
    {
        "name": "get_state",
        "description": "Get current game state snapshot. Call this to check results of actions.",
        "input_schema": {"type": "object", "properties": {}}
    },
    {
        "name": "execute_command",
        "description": "Execute any MacroQuest or EQ slash command (/attack on, /cast 1, /sit, etc.)",
        "input_schema": {
            "type": "object",
            "properties": {
                "command": {"type": "string", "description": "Full command including leading slash"}
            },
            "required": ["command"]
        }
    },
    {
        "name": "navigate_to",
        "description": "Navigate to x/y/z coordinates in the current zone using MQ2Nav.",
        "input_schema": {
            "type": "object",
            "properties": {
                "y": {"type": "number", "description": "North/South coordinate"},
                "x": {"type": "number", "description": "East/West coordinate"},
                "z": {"type": "number", "description": "Up/Down coordinate (optional)"}
            },
            "required": ["y", "x"]
        }
    },
    {
        "name": "say",
        "description": "Say text in local /say chat (visible to other players — use sparingly)",
        "input_schema": {
            "type": "object",
            "properties": {
                "text": {"type": "string", "description": "Text to say"}
            },
            "required": ["text"]
        }
    }
]

# ── Session log ───────────────────────────────────────────────────────────────

_log_file = None

def open_log(goal_summary: str) -> None:
    global _log_file
    LOG_DIR.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    safe = "".join(c if c.isalnum() or c in " _-" else "_" for c in goal_summary[:40]).strip()
    path = LOG_DIR / f"{stamp}_{safe}.log"
    _log_file = open(path, "w", encoding="utf-8", buffering=1)
    log(f"Session started. Goal: {goal_summary}")
    log(f"Model: {MODEL}  Log: {path}")

def log(msg: str) -> None:
    line = f"[{datetime.now().strftime('%H:%M:%S')}] {msg}"
    print(line)
    if _log_file:
        _log_file.write(line + "\n")

def close_log() -> None:
    if _log_file:
        _log_file.close()

# ── MCP client ───────────────────────────────────────────────────────────────

_req_id = 0

def _next_id():
    global _req_id
    _req_id += 1
    return _req_id

def call_mcp_tool(name: str, args: dict) -> str:
    payload = {
        "jsonrpc": "2.0",
        "id": _next_id(),
        "method": "tools/call",
        "params": {"name": name, "arguments": args}
    }
    try:
        r = httpx.post(f"{MCP_URL}/mcp", json=payload, timeout=10)
        data = r.json()
        if "result" in data:
            content = data["result"].get("content", [])
            if content:
                return content[0].get("text", json.dumps(data["result"]))
            return json.dumps(data["result"])
        if "error" in data:
            return f"[error] {data['error'].get('message', str(data['error']))}"
        return json.dumps(data)
    except Exception as e:
        return f"[error] MCP call failed: {e}"

def get_current_state() -> str:
    return call_mcp_tool("get_state", {})

# ── History management ────────────────────────────────────────────────────────

def trim_history(messages: list) -> list:
    """
    Trim history to MAX_HISTORY_PAIRS user/assistant pairs.
    Always keeps pairs together and never splits a tool_use / tool_result sequence.
    The first message (initial state seed) is always preserved.
    """
    if len(messages) <= 1:
        return messages

    seed = messages[0]  # always keep the initial state message
    rest = messages[1:]

    # Walk rest in pairs: assistant then user (tool results or nudge)
    pairs = []
    i = 0
    while i < len(rest):
        if i + 1 < len(rest):
            pairs.append((rest[i], rest[i + 1]))
            i += 2
        else:
            pairs.append((rest[i],))
            i += 1

    # Drop oldest pairs until within limit
    while len(pairs) > MAX_HISTORY_PAIRS:
        pairs.pop(0)

    result = [seed]
    for pair in pairs:
        result.extend(pair)
    return result

# ── Combat state detection ────────────────────────────────────────────────────

def is_in_combat(tool_results: list) -> bool:
    """
    Heuristic: parse the most recent get_state result to check aggro_count.
    Returns True if we appear to be in active combat.
    """
    for tr in reversed(tool_results):
        text = tr.get("content", "")
        try:
            state = json.loads(text)
            return state.get("aggro_count", 0) > 0
        except (json.JSONDecodeError, AttributeError):
            pass
    return False

# ── Goal stack ────────────────────────────────────────────────────────────────

def parse_goals(goal_str: str) -> list[str]:
    """Split semicolon-separated goals into an ordered list."""
    goals = [g.strip() for g in goal_str.split(";") if g.strip()]
    return goals if goals else [goal_str]

def format_goal_stack(goals: list[str], current_idx: int) -> str:
    lines = ["Current goal stack (work top to bottom):"]
    for i, g in enumerate(goals):
        marker = "→ [ACTIVE]" if i == current_idx else "  [pending]" if i > current_idx else "  [done]"
        lines.append(f"  {marker} {g}")
    return "\n".join(lines)

# ── Agent loop ────────────────────────────────────────────────────────────────

def run(goal_str: str):
    client = anthropic.Anthropic()
    goals = parse_goals(goal_str)
    current_goal_idx = 0
    messages = []
    last_nudge = time.time()
    in_combat = False

    open_log(goals[0])
    log(f"Goals: {goals}")
    log(f"Connecting to MQMCP at {MCP_URL}...")

    # Seed with initial state and goal stack
    initial_state = get_current_state()
    log("Initial state received.")

    # Build class-aware system prompt
    try:
        state_data = json.loads(initial_state)
        char_class = state_data.get("class", "")
        char_level = state_data.get("level", "")
    except (json.JSONDecodeError, AttributeError):
        char_class = ""
        char_level = ""

    system_prompt = build_system_prompt(char_class)
    if char_class:
        log(f"Class detected: {char_class} {char_level} — injecting class knowledge.")
    else:
        log("No class detected — using base system prompt.")

    messages.append({
        "role": "user",
        "content": (
            f"{format_goal_stack(goals, current_goal_idx)}\n\n"
            f"Initial game state:\n{initial_state}\n\n"
            "Begin pursuing the active goal now."
        )
    })

    try:
        while True:
            try:
                response = client.messages.create(
                    model=MODEL,
                    max_tokens=MAX_TOKENS,
                    system=system_prompt,
                    tools=TOOLS,
                    messages=messages
                )

                messages.append({"role": "assistant", "content": response.content})
                messages = trim_history(messages)

                # Log what Claude decided
                for block in response.content:
                    if hasattr(block, "text") and block.text:
                        log(f"Claude: {block.text[:300]}")
                    elif hasattr(block, "name"):
                        log(f"Tool: {block.name}({json.dumps(block.input)})")

                if response.stop_reason == "tool_use":
                    tool_results = []
                    for block in response.content:
                        if block.type != "tool_use":
                            continue
                        result = call_mcp_tool(block.name, block.input)
                        log(f"  -> {result[:300]}")
                        tool_results.append({
                            "type": "tool_result",
                            "tool_use_id": block.id,
                            "content": result
                        })

                    messages.append({"role": "user", "content": tool_results})
                    last_nudge = time.time()

                    # Dynamic sleep: faster in combat
                    in_combat = is_in_combat(tool_results)
                    time.sleep(LOOP_SLEEP_COMBAT if in_combat else LOOP_SLEEP_IDLE)

                elif response.stop_reason == "end_turn":
                    # Claude stopped — nudge with fresh state + goal stack
                    elapsed = time.time() - last_nudge
                    time.sleep(max(0, NUDGE_EVERY - elapsed))
                    state = get_current_state()
                    goal_block = format_goal_stack(goals, current_goal_idx)
                    messages.append({
                        "role": "user",
                        "content": (
                            f"[{datetime.now().strftime('%H:%M:%S')}] Continue.\n"
                            f"{goal_block}\n"
                            f"Current state:\n{state}"
                        )
                    })
                    last_nudge = time.time()
                    log("Nudged Claude with fresh state.")

                else:
                    # max_tokens or other stop
                    state = get_current_state()
                    messages.append({
                        "role": "user",
                        "content": f"[{datetime.now().strftime('%H:%M:%S')}] State:\n{state}"
                    })
                    time.sleep(LOOP_SLEEP_IDLE)

            except anthropic.APIError as e:
                log(f"API error: {e}. Retrying in 5s...")
                time.sleep(5)
            except httpx.ConnectError:
                log("Cannot reach MQMCP. Is EQ running with the plugin loaded? Retrying in 5s...")
                time.sleep(5)

    except KeyboardInterrupt:
        log("Stopping agent.")
    finally:
        close_log()


if __name__ == "__main__":
    goal = " ".join(sys.argv[1:]) if len(sys.argv) > 1 else \
        "Grind XP by killing nearby NPCs; loot everything after each kill; meditate to recover mana between fights"
    run(goal)
