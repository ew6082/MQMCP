# MQMCPServer

A MacroQuest plugin that exposes EverQuest game state and character control via the [Model Context Protocol](https://modelcontextprotocol.io/) (MCP). Allows LLM agents and external tools to observe and control your character in real time.

## Overview

MQMCPServer runs a local HTTP server on `localhost:8284`. It speaks JSON-RPC 2.0 over the `/mcp` endpoint, making it compatible with any MCP client or custom agent.

Game state is read each server pulse (main thread) and cached for lock-free access by HTTP clients. Commands are queued from the HTTP thread and executed on the next pulse, keeping everything thread-safe and EQ-compatible.

## Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/mcp` | MCP JSON-RPC 2.0 endpoint |
| `GET`  | `/mcp` | SSE keepalive stream (for streaming MCP clients) |
| `GET`  | `/me` | Debug: player state JSON |
| `GET`  | `/zone` | Debug: zone info JSON |
| `GET`  | `/target` | Debug: current target JSON |
| `GET`  | `/spawns` | Debug: nearby spawns JSON |

## In-Game Command

Type `/mcp` in EverQuest to display server status: URL, uptime, character info, pending command queue depth, and chat buffer count.

## MCP Tools

### `get_state`
Returns a full game state snapshot. Call this after any action to observe the result.

**Returns:** JSON object with all state fields (see State Fields below).

---

### `execute_command`
Executes any MacroQuest or EverQuest slash command on the next game pulse.

| Parameter | Type | Description |
|-----------|------|-------------|
| `command` | string | Full command including leading slash, e.g. `/cast 1`, `/attack on` |

Commands are queued and executed in order on the next EQ pulse. The response includes a state snapshot captured before the command runs — call `get_state` again to observe the result.

---

### `navigate_to`
Navigates to world coordinates using MQ2Nav (must be loaded).

| Parameter | Type | Description |
|-----------|------|-------------|
| `y` | number | North/South coordinate |
| `x` | number | East/West coordinate |
| `z` | number | Up/Down coordinate (optional) |

Accepts both numeric and string values for coordinates.

---

### `say`
Says text in local `/say` chat (visible to nearby players).

| Parameter | Type | Description |
|-----------|------|-------------|
| `text` | string | Text to say |

## State Fields

The `get_state` tool (and the inline state returned with each command) returns:

```json
{
  "inGame": true,
  "name": "Yourtoon",
  "level": 70,
  "class": "Paladin",
  "hp_pct": 95,
  "mana_pct": 100,
  "endurance_pct": 88,
  "invisible": false,
  "moving": false,
  "position": { "x": 100.0, "y": 200.0, "z": 50.0 },
  "zone": "arcstone",
  "aggro_count": 1,
  "group_size": 2,

  "aggressors": [
    { "name": "a fallen spirit", "level": 67, "distance": 6.5 }
  ],

  "target": {
    "name": "a_fallen_spirit01",
    "type": "NPC",
    "distance": 6.5,
    "hp_pct": 45,
    "buffs": []
  },

  "mem_spells": [
    { "gem": 0, "id": 1234, "name": "Complete Heal", "ready": true,  "ms_remaining": 0 },
    { "gem": 1, "id": 5678, "name": "Yaulp IV",      "ready": false, "ms_remaining": 14000 }
  ],

  "abilities": [
    { "index": 0, "disc_cmd_index": 1, "name": "Mighty Strike", "id": 704,  "ready": true,  "secs_remaining": 0 },
    { "index": 1, "disc_cmd_index": 2, "name": "Defensive",     "id": 705,  "ready": false, "secs_remaining": 47 }
  ],

  "active_buffs": [
    { "id": 1534, "name": "Yaulp IV", "duration": 6, "song": false }
  ],

  "loot_available": [
    { "name": "Blade of Ravenglass' Victim", "noDrop": true, "personal": true }
  ],

  "recent_chat": [
    "You slash a fallen spirit for 1140 points of damage. (Slay Undead)",
    "You have slain a fallen spirit!"
  ]
}
```

### Field Reference

| Field | Description |
|-------|-------------|
| `inGame` | `false` if in character select or loading |
| `name` | Character name |
| `level` | Character level |
| `class` | Class name (e.g. `"Paladin"`, `"Warrior"`) |
| `hp_pct` / `mana_pct` | Health / mana as 0–100 integer |
| `endurance_pct` | Endurance as 0–100 integer |
| `invisible` | `true` if HideMode is active |
| `moving` | `true` if position changed since last snapshot |
| `position` | `{x, y, z}` world coordinates |
| `zone` | Zone short name |
| `aggro_count` | Number of NPCs in XTarget window |
| `group_size` | Total group members including self |
| `aggressors` | XTarget entries currently hostile to you |
| `target` | Current target with name, type, distance, hp_pct, and visible buffs |
| `mem_spells` | Memorized spell gems (0-indexed) with ready status and `ms_remaining` until ready |
| `abilities` | Known combat abilities/disciplines with ready status and `secs_remaining` until ready (seconds, not ms) |
| `active_buffs` | Self buffs and songs with remaining duration in ticks (`-1` = permanent, `-4` = song/disc) |
| `loot_available` | Items in the AdvLoot window (personal and shared) |
| `recent_chat` | Last 100 lines of game chat (combat, spells, system, say, group, etc.) |

## MCP Resources

In addition to tools, the server exposes MCP resources for granular reads:

| URI | Description |
|-----|-------------|
| `eq://me` | Full player stats (HP, mana, endurance, position, class, level) |
| `eq://zone` | Current zone name and ID |
| `eq://target` | Current target details |
| `eq://spawns` | All spawns within 300 units, sorted by distance |
| `eq://spells` | Memorized spell gems |
| `eq://cooldowns` | Spell gem and combat ability/disc recast timers (combined) |
| `eq://abilities` | Known combat abilities/disciplines with readiness and cooldowns |
| `eq://buffs` | Active buffs and songs on self |
| `eq://loot` | Items currently in the AdvLoot window |
| `eq://group` | Group members with HP%, mana%, level, and role |
| `eq://chat` | Last 100 lines of buffered chat |

## Chat Capture

MQMCPServer captures chat from two sources:
- **`OnWriteChatColor`** — MQ output window (plugin messages, MQ2 output)
- **`OnIncomingChat`** — All game chat channels (combat hits, spell results, XP messages, say, group, etc.)

MQ color codes (`0x07` + letter) and non-ASCII bytes are stripped so all strings are valid UTF-8.

## Combat Abilities / Disciplines

Combat abilities and disciplines are returned in `abilities[]` within the state snapshot, and also via `eq://abilities` and `eq://cooldowns`.

Each entry includes:

| Field | Description |
|-------|-------------|
| `index` | 0-based slot index in the combat abilities array |
| `disc_cmd_index` | 1-based index to pass to `/disc <n>` |
| `name` | Spell/disc name |
| `id` | Spell ID |
| `ready` | `true` if the ability is off cooldown |
| `secs_remaining` | Seconds until ready (0 if ready) — **seconds**, not ms |

To use a combat ability:

```
/disc <disc_cmd_index>
```

Always check `ready` or `secs_remaining == 0` before using a disc.

The `eq://cooldowns` resource returns **both** spell gems and combat abilities in one list. Each entry has a `type` field (`"spell_gem"` or `"ability"`) to distinguish them.

## Loot Commands

The AdvLoot window uses 1-based indices:

```
/advloot personal <index> loot   # loot personal item at index
/advloot shared <index> an       # take shared item at index
```

## Build Requirements

- MacroQuest (Test/Live client build)
- C++20
- Dependencies (pulled via vcpkg or MQ's bundled libs):
  - `nlohmann/json`
  - `cpp-httplib`
  - `fmtlib`

Add to your CMake plugin list and build with the `test` preset.

## Building in this fork (openvanilla)

> Fork note. This section records what it took to build against the `openvanilla`
> MacroQuest fork, where third-party plugins live in `plugins/` rather than upstream's
> `src/plugins/`. Several of the changes below are in the **openvanilla** tree, not this
> repo, so a `git pull` over there will silently undo them and the build will break
> again — hence writing them down here, in a repo we control.

### Changes in this repo (already committed)

| File | Change | Why |
|------|--------|-----|
| `vcpkg.json` | port `name` → `mq-mcp-server` | MQ's overlay generator derives the port name from the **directory** name (`PORT_LOWER` in `cmake/vcpkg_manifest.cmake`), while vcpkg reads it from the manifest's `name` field. When they disagree nothing provides the port and vcpkg falls through to the upstream baseline: `error: the baseline does not contain an entry for port mq-mcp-server`. |
| `CMakeLists.txt` | `../../Plugin.cmake` → `../../src/Plugin.cmake` | The upstream path is correct only at `src/plugins/<name>/`. From `plugins/<name>/` it needs the `src/` segment — the same include the vcxproj→CMake converter emits for the other `plugins/` dwellers (compare `plugins/MQ2EQBC/CMakeLists.txt`). |
| `portfile.cmake` | added | Empty-package stub. MQ regenerates it when absent, but vcpkg convention is that `vcpkg.json` + `portfile.cmake` are both port *source*, so a fresh clone has a complete port definition. |

`MQMCPServer.vcxproj` still carries an upstream-layout `ProjectReference` to
`..\..\main\MQ2Main.vcxproj`, which resolves outside the repo from `plugins/`. It is
unused when building through the generated `build/MacroQuest.sln` (CMake writes its own
project), so it was left alone. Fix it if you ever want a standalone `MQMCPServer.sln`.

### Prerequisites in the openvanilla tree (NOT in this repo)

These are the ones a pull will revert. Re-apply as needed.

1. **Enable custom plugins.** `MQ_BUILD_CUSTOM_PLUGINS` defaults `OFF`. Configure with
   `-DMQ_BUILD_CUSTOM_PLUGINS=ON`, and create `custom_plugins.cmake` at the repo root —
   `CMakePresets.json` points `MQ_CUSTOM_PLUGINS_FILE` at it, but it does not exist by
   default. Without it the build auto-globs every directory under `plugins/`, including
   scratch folders like `New folder`. One `add_subdirectory("plugins/<name>")` per line.

2. **`src/Common.cmake`: `/std:c++17` → `/std:c++20`** *(tracked file — will conflict)*.
   `Common.cmake` is the CMake port of `src/Common.props`, which specifies
   `<LanguageStandard>stdcpp20`; the port mistranslated it, and the hard-coded flag also
   silently overrode `CMAKE_CXX_STANDARD 20` from the root `CMakeLists.txt`. MQ core
   genuinely needs C++20 — `src/routing/Network.cpp:681` calls
   `std::unordered_set::contains`. Symptom: `error C2039: 'contains': is not a member of
   std::unordered_set`. Building via the `.sln` reads `Common.props` and works, so this
   only ever bites on the CMake path.

3. **Placeholder for `src/main/ImGuiAlphaMask.h`** *(untracked)*. `src/main/CMakeLists.txt`
   lists the header in `MQ2Main_HEADERS`, but only `ImGuiAlphaMask.cpp` was ever committed.
   CMake treats a listed-but-absent source as fatal at generate time; MSBuild only warns.
   Nothing `#include`s it, so an empty `#pragma once` is enough. Delete the placeholder
   before pulling if upstream ever commits the real header — an untracked file blocks the
   checkout. Symptom: `Cannot find source file: ImGuiAlphaMask.h`.

4. **Placeholder for `data/resources/ItemDB.txt`** *(untracked)*. Same cause, via
   `MQ2Main_OTHERFILES`. Nothing in the build copies `data/resources/`, so the contents do
   not matter; copying the real file from an MQ install is the easy route.

5. **Delete stale `src/routing/*.pb.{h,cc}`** *(untracked build artifacts)*.
   `protobuf_generate` has an empty `PROTOC_OUT_DIR` and emits into the build directory,
   but `src/routing/` sits earlier on the include path, so leftover generated files there
   shadow the fresh ones. If they came from a newer protoc than the current vcpkg baseline
   provides, the build demands a protobuf runtime that is not installed. Symptom:
   `Cannot open include file: 'google/protobuf/runtime_version.h'`. Delete the four files
   and let the build regenerate them.

6. **`include/mq/contrib/glm/glmUtils.h` needs `#include <array>`** *(tracked file — will
   conflict)*. It uses `std::array` in `GetVec3SetFromString` but never included `<array>`;
   it came in transitively until MSVC 14.44, whose `<utility>` only forward-declares
   `std::array`. Symptom: `error C2027: use of undefined type 'std::array<glm::vec3,2>'`.
   The same header also uses `std::string_view` without including it — still resolving
   transitively today, so expect the same failure there on a future toolset bump.

### Linking MQ2Main itself (not needed for this plugin)

Two further failures block a full `MQ2Main.dll` link. **Both are vcpkg port problems, not
MQ code**, and neither has to be solved to build this plugin — see the workaround below.

- **crashpad debug/release mixing.** `crashpadConfig.cmake` resolves its libraries with a
  bare `find_library(_LIB ${LIB_NAME})` and no debug/release distinction. vcpkg keeps
  `debug/lib` on the search path, so a Release link picks up `MTd_StaticDebug` objects:
  `LNK2038: mismatch detected for 'RuntimeLibrary'` (and `_ITERATOR_DEBUG_LEVEL`), repeated
  for every crashpad object.
- **`LNK1104: cannot open file 'absl_base.lib'`.** Nothing in MQ source or the generated
  vcxproj asks for abseil, and abseil is not installed; the reference most likely comes
  from a `/DEFAULTLIB` directive baked into `libprotobuf.lib`. Note the installed protobuf
  is 3.21.12 while older generated `.pb` files in this tree came from 6.33.4, so the
  dependency set has shifted at some point.

### Build

```
cmake --preset live -DMQ_BUILD_CUSTOM_PLUGINS=ON
cmake --build build --config Release --target MQMCPServer
```

**Workaround if MQ2Main will not link.** This plugin only needs `MQ2Main.lib` — an import
library, i.e. export names — not a freshly linked `MQ2Main.dll`. Given a previously built
`MQ2Main.lib` (and `routing.lib`, `pluginapi.lib`, `eqlib.lib`, which do build), place the
import lib at `build/lib/release/MQ2Main.lib` and build the plugin project alone:

```
MSBuild.exe build/plugins/mq-mcp-server/MQMCPServer.vcxproj \
    -p:Configuration=Release -p:Platform=x64 -p:BuildProjectReferences=false
```

`BuildProjectReferences=false` is what skips the broken MQ2Main link. Output lands at
`build/bin/release/plugins/MQMCPServer.dll`; copy it into your MQ install's `plugins/`.
Be aware that a *failed* MQ2Main link deletes any existing `build/bin/release/MQ2Main.dll`,
so keep a copy if you depend on that one.

The presets need CMake ≥ 3.30 (`"version": 9`). Visual Studio 2022 ships a new enough one
at `Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`; a mingw CMake on
`PATH` may be too old. The first configure builds `cpp-httplib` and `nlohmann-json` from
source and takes ~17 minutes; later ones are seconds.

## Usage with an MCP Client

Add the server to your MCP client config:

```json
{
  "mcpServers": {
    "everquest": {
      "type": "http",
      "url": "http://127.0.0.1:8284/mcp"
    }
  }
}
```

All tools (`get_state`, `execute_command`, `navigate_to`, `say`) will be available to any connected MCP client.

## Usage with a Python Agent

`eq_agent.py` is a standalone Python orchestrator that connects directly to MQMCPServer over HTTP and drives the character using an LLM agentic loop.

```bash
pip install anthropic httpx
export ANTHROPIC_API_KEY=sk-...
python eq_agent.py "Kill gnolls for XP, loot everything"
```

Goals can be chained with semicolons:

```bash
python eq_agent.py "Kill gnolls; loot everything; meditate when low mana"
```

## Configuration

The port defaults to `8284`. To change it, modify `k_defaultPort` in `MQMCPServer.cpp` and rebuild.

## Notes

- Commands are executed on the EQ main thread (game pulse) — never from the HTTP thread
- State snapshots update every pulse (~100ms)
- Nearby spawns are capped at 60, sorted by distance, within 300 units
- MQ2Nav must be loaded for `navigate_to` to work
- The plugin does not suppress any chat (all `OnIncomingChat` handlers return `FALSE`)
- Group member mana is read from the spawn data; accuracy depends on what the server sends to clients

## License

GPL-3.0 — see [LICENSE](LICENSE).
