# MQMCP

A MacroQuest plugin that exposes EverQuest game state and character control via the [Model Context Protocol](https://modelcontextprotocol.io/) (MCP). Allows LLM agents and external tools to observe and control your character in real time.

## Overview

MQMCP runs a local HTTP server on `localhost`, one port per EverQuest client: the character's pinned port if it has one, otherwise the first free port from `8284` (see [Ports](#ports)). It speaks JSON-RPC 2.0 over the `/mcp` endpoint, making it compatible with any MCP client or custom agent.

Game state is read each server pulse (main thread) and cached for lock-free access by HTTP clients. Commands are queued from the HTTP thread and executed on the next pulse, keeping everything thread-safe and EQ-compatible.

## Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/mcp` | MCP JSON-RPC 2.0 endpoint |
| `GET`  | `/mcp` | SSE keepalive stream (for streaming MCP clients) |
| `GET`  | `/whoami` | `{name, server, in_game, port, pid}` for the client on this port (name is empty at char select) |
| `GET`  | `/me` | Debug: player state JSON |
| `GET`  | `/zone` | Debug: zone info JSON |
| `GET`  | `/target` | Debug: current target JSON |
| `GET`  | `/spawns` | Debug: nearby spawns JSON |

## In-Game Command

Type `/mcp` in EverQuest to display server status: URL (with whether the port is pinned or auto), uptime, character info, pending command queue depth, and chat buffer count.

| Command | Effect |
|---------|--------|
| `/mcp` | Show status |
| `/mcp port <n>` | Pin this character to port `<n>` (1024–65535) and rebind now |
| `/mcp port auto` | Remove this character's pin and rebind to the first free port |
| `/mcp chat` / `/mcp chat reload` | Show, or re-read, this character's chat tab layout |

`get_state`, the `eq://` resources and the debug REST endpoints read the game through MQ's TLOs, on the game thread, when they are asked. Nothing is read between requests. (Earlier versions copied EverQuest structs every pulse behind a `/mcp snapshot` toggle; that and the `[Settings] Snapshot` ini key are gone.)

You have to be logged in to a character to use `/mcp port`, because pins are stored per character.

## MCP Tools

### `get_state`
Returns the current game state, read from one game frame. Call this after any action to observe the result.

| Parameter | Type | Description |
|-----------|------|-------------|
| `max_chat` | integer | Also return this many of the newest chat lines as `recent_chat` (default 0) |

**Returns:** JSON object with all state fields (see State Fields below). Chat is left out by default. Read it with `chat_since`, starting from the returned `chat_seq`.

---

### `execute_command`
Executes any MacroQuest or EverQuest slash command on the next game pulse.

| Parameter | Type | Description |
|-----------|------|-------------|
| `command` | string | Full command including leading slash, e.g. `/cast 1`, `/attack on` |

Commands are queued and executed in order on the next EQ pulse. The response includes the game state read on that same pulse, just after the command was issued, with the last 20 chat lines. Its `chat_seq` is taken before the command was queued, so `chat_since` with that value returns the command's output. To get the output in one call, use `run_command`.

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

---

### `run_command`
Runs a slash command and returns the chat output that follows it.

| Parameter | Type | Description |
|-----------|------|-------------|
| `command` | string | Full command including leading slash |
| `wait_ms` | integer | How long to keep collecting output after the command runs, 0–10000 (default 500) |
| `filter` | string | Only return lines containing this text (case-insensitive) |
| `max_lines` | integer | Cap on lines returned (default 200) |

The tool waits until the command has run on the game thread, which takes up to 5 s before it gives up. It then waits `wait_ms` longer so that replies from the server, such as spell landed or resisted messages, can arrive. It returns every chat line since the command was queued, in the same format as `chat_since`. Lines from other sources that arrived in that window, such as combat spam, are included too, so use `filter` to narrow them.

---

### `chat_since`
Returns chat lines newer than a sequence number, oldest first.

| Parameter | Type | Description |
|-----------|------|-------------|
| `since` | integer | Return lines with `seq` greater than this (default 0 = everything buffered) |
| `filter` | string | Only lines containing this text (case-insensitive) |
| `tabs` | string[] | Only lines shown in these chat tabs (see `chat_tabs`) |
| `channels` | string[] | Only lines on these channels |
| `exclude` | string[] | Drop lines whose tab or channel is listed |
| `max_lines` | integer | Cap on lines returned (default 200) |

`run_command` takes the same `tabs`, `channels` and `exclude` arguments.

Each line in the buffer has a sequence number that only increases. `get_state` reports the newest one as `chat_seq`. The result looks like this:

```json
{ "next": 1234, "more": false,
  "lines": [{ "seq": 1233, "channel": "melee_your_hits", "tab": "Combat", "color": 265, "text": "..." }, ...] }
```

Each line has these fields:

- `channel` is EQ's chat channel, named after eqlib's `eChatChannel` (`raid`, `guild`, `melee_others_hits`, `your_dots`, ...). MQ's own output, such as `/echo` and plugin messages, has the channel `mq`.
- `tab` is the chat tab where the line is shown on this client. MQ output is in tab `MQ`.
- `color` is the raw `USERCOLOR_*` code. It's included so that you can check the mapping from colors to channels.

Tags are assigned when a line arrives. In `text`, item, spell and player links appear as their display text. When a line has links, `links` lists each one as `{type, id, text}`, where `id` is the spell or item ID. Spell names repeat across ranks, but the ID identifies the exact spell, for use with `Spell[<id>]`.

Pass `next` as `since` on the next call. `next` moves past filtered-out lines too, so polling with a filter never rescans them. When `more` is `true`, `max_lines` was reached, so call again with `next`. `missed` (present only when non-zero) counts lines that were evicted from the 4000-line buffer before you read them. `reset: true` means the plugin was reloaded and numbering restarted from 1.

---

### `chat_tabs`
Lists this client's chat tabs and the channels that are routed to each one.

The layout comes from the character's UI ini in the EverQuest folder. For a character with personas, that's the file for the current class, `UI_<Name>_<server>_<CLS>.ini`. Otherwise it's `UI_<Name>_<server>.ini`. The plugin reads the file when you enter the game and when you run `/mcp chat reload`. EQ only writes tab changes to the file when you camp, so a change you make in game shows up after you log in again.

---

### `wait_for`
Waits until a TLO expression becomes true. It checks the expression on every game pulse and returns as soon as the condition is met, or when the timeout runs out.

| Parameter | Type | Description |
|-----------|------|-------------|
| `expr` | string | TLO expression, with or without `${}` |
| `equals` | string | Wait for this exact value (case-insensitive) instead of a truthy one |
| `timeout_ms` | integer | Give up after this long, 0–60000 (default 5000) |

When `equals` is not given, the condition is true if the value is `TRUE` or a non-zero number. `NULL`, `FALSE`, `0` and unresolved expressions count as false. For example, `{"expr": "Navigation.Active", "equals": "FALSE"}` waits for nav to finish, and `{"expr": "Me.Casting", "equals": "NULL"}` waits for a cast to end. For comparisons, use a TLO that evaluates them: `"${Math.Calc[${Me.PctHPs} > 80]}"`. The result is `{expr, met, value, resolved, elapsed_ms, polls}`, and `met: false` means it timed out.

---

### `list_clients`
Lists the EQ clients on this machine that run the plugin. Each entry is that client's `/whoami` answer: `name` (empty at char select), `server`, `in_game`, `port` and `pid`, plus `self: true` for the client you asked. Every entry is confirmed live. Entries for clients that no longer answer are removed from the registry, and `removed_stale` counts them. See [Client registry](#client-registry).

---

### `query_peer`
Runs `query_tlo_batch` on another character's client, found by name through the registry, and returns its results with that character's `name` and `port` added.

| Parameter | Type | Description |
|-----------|------|-------------|
| `name` | string | Character name, case-insensitive |
| `exprs` | string[] | TLO expressions, as for `query_tlo_batch` |

This replaces DanNet round trips such as `/dquery` or `/dex ... /dt` for reading another box's state.

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
    { "name": "a fallen spirit", "id": 4411, "level": 67, "distance": 6.5 }
  ],

  "target": {
    "name": "a fallen spirit",
    "type": "NPC",
    "distance": 6.5,
    "hp_pct": 45,
    "buffs": [ { "name": "Tashan's Lingering Cry", "id": 2884, "seconds": 54 } ]
  },

  "mem_spells": [
    { "gem": 1, "id": 1234, "name": "Complete Heal", "ready": true,  "ms_remaining": 0 },
    { "gem": 2, "id": 5678, "name": "Yaulp IV",      "ready": false, "ms_remaining": 14000 }
  ],

  "active_buffs": [
    { "id": 1534, "name": "Yaulp IV", "seconds": 36 },
    { "id": 8001, "name": "Selo's Accelerating Chorus", "seconds": 18, "song": true }
  ],

  "loot_available": [
    { "name": "Blade of Ravenglass' Victim", "id": 99001, "noDrop": true, "stackSize": 1, "personal": true }
  ],

  "chat_seq": 10422
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
| `invisible` | `true` if you have any invisibility (`Me.Invis`) |
| `moving` | `true` if you are moving (`Me.Moving`) |
| `position` | `{x, y, z}` world coordinates |
| `zone` | Zone short name |
| `aggro_count` | Number of `aggressors` |
| `group_size` | Total group members including self, 0 when ungrouped |
| `aggressors` | NPCs in this zone on your XTarget window (slots holding group members are skipped) |
| `target` | Current target with name, type, distance, hp_pct, and buffs (from MQ's buff cache, so NPC targets have them too) |
| `mem_spells` | Memorized spell gems, numbered from 1 as `/cast` uses them, with `ready` (`Me.SpellReady`) and `ms_remaining` until the gem's recast ends |
| `active_buffs` | Self buffs, then songs (marked `song: true`), with `seconds` remaining |
| `loot_available` | Items in the AdvLoot window (personal and shared) |
| `recent_chat` | Only with `max_chat`: the newest chat lines. Action tool replies include the last 20. |
| `chat_seq` | Sequence number of the newest chat line, to pass to `chat_since`. In action tool responses, it is taken before the command was queued. |

## MCP Resources

In addition to tools, the server exposes MCP resources for granular reads:

| URI | Description |
|-----|-------------|
| `eq://me` | Full player stats (HP, mana, endurance, position, class, level) |
| `eq://zone` | Current zone name and ID |
| `eq://target` | Current target details |
| `eq://spawns` | All spawns within 300 units, sorted by distance |
| `eq://spells` | Memorized spell gems with recast state |
| `eq://cooldowns` | Same as `eq://spells` |
| `eq://buffs` | Active buffs and songs on self |
| `eq://loot` | Items currently in the AdvLoot window |
| `eq://group` | Group members with HP%, mana%, level, and role |
| `eq://chat` | Buffered chat, up to 4000 lines |

## Chat Capture

MQMCP captures chat from two sources:
- **`OnWriteChatColor`** — MQ output window (plugin messages, MQ2 output)
- **`OnIncomingChat`** — All game chat channels (combat hits, spell results, XP messages, say, group, etc.)

MQ color codes (`0x07` + letter) and non-ASCII bytes are stripped so all strings are valid UTF-8.

## Combat Abilities / Disciplines

The state doesn't list combat abilities. (The upstream README described an `abilities` list and `eq://abilities`, but the plugin never had them.) Read them with `query_tlo_batch`, e.g. `Me.CombatAbilityReady[Mighty Strike]` and `Me.CombatAbilityTimer[Mighty Strike].TotalSeconds`.

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
| `vcpkg.json` | port `name` → `mqmcp` | MQ's overlay generator derives the port name from the **directory** name (`PORT_LOWER` in `cmake/vcpkg_manifest.cmake`), while vcpkg reads it from the manifest's `name` field. When they disagree nothing provides the port and vcpkg falls through to the upstream baseline: `error: the baseline does not contain an entry for port mqmcp`. |
| `CMakeLists.txt` | `../../Plugin.cmake` → `../../src/Plugin.cmake` | The upstream path is correct only at `src/plugins/<name>/`. From `plugins/<name>/` it needs the `src/` segment — the same include the vcxproj→CMake converter emits for the other `plugins/` dwellers (compare `plugins/MQ2EQBC/CMakeLists.txt`). |
| `portfile.cmake` | added | Empty-package stub. MQ regenerates it when absent, but vcpkg convention is that `vcpkg.json` + `portfile.cmake` are both port *source*, so a fresh clone has a complete port definition. |

`MQMCP.vcxproj` still carries an upstream-layout `ProjectReference` to
`..\..\main\MQ2Main.vcxproj`, which resolves outside the repo from `plugins/`. It is
unused when building through the generated `build/MacroQuest.sln` (CMake writes its own
project), so it was left alone. Fix it if you ever want a standalone `MQMCP.sln`.

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

7. **`local_build_overrides.cmake`** *(untracked, repo root)*, passed with
   `-DCMAKE_PROJECT_INCLUDE_BEFORE`. It runs at the start of `project()`, after the root
   `CMakeLists.txt` writes `build/vcpkg-configuration.json` and before the vcpkg toolchain
   reads it. It fixes three gaps in the CMake conversion without editing tracked files:
   - **vcpkg baseline.** `cmake/vcpkg-configuration.json.in` pins `d612d36b` (mid-2024:
     protobuf 3.21.12, old crashpad). Upstream `c6e344db` moved the tree to `81e3c888`
     (protobuf 6.33.4 + abseil) but updated only the MSBuild path. The override rewrites
     the generated config's baseline. Symptoms on the old baseline: `LNK1104: cannot open
     file 'absl_base.lib'`, and crashpad debug libs in a Release link (`LNK2038: mismatch
     detected for 'RuntimeLibrary'`).
   - **imanim sources.** `src/imgui/imgui.vcxproj` compiles `imanim/*.cpp`, but
     `src/imgui/CMakeLists.txt` doesn't. The override adds them to the `imgui` target.
     Symptom: `LNK2019: unresolved external symbol ... iam_update_begin_frame` (12 of them).
   - **vcpkg lib directory.** `ProtobufLibs.h` names abseil libs with bare
     `#pragma comment(lib, ...)`, but nothing puts the vcpkg lib directory on MQ2Main's
     search path. The override adds it. Symptom:
     `LNK1104: cannot open file 'absl_cordz_sample_token.lib'`.

### Build

Configure once (the first run builds protobuf and abseil from source, which takes a while):

```
cmake --preset live -DMQ_BUILD_CUSTOM_PLUGINS=ON \
    -DMQ_CUSTOM_PLUGINS_FILE=C:/repos/openvanilla/custom_plugins_scope.cmake \
    -DCMAKE_PROJECT_INCLUDE_BEFORE=C:/repos/openvanilla/local_build_overrides.cmake
```

`custom_plugins_scope.cmake` (untracked) lists just the plugins to build, currently
`MQ2MyButtons2` and `mqmcp`. Then build MQ2Main and both plugins in Release:

```
cmake --build build --config Release --target MQ2Main MQMCP MQ2MyButtons2
```

The output lands in `build/bin/release/` (MQ2Main) and `build/bin/release/plugins/`.
`build/MacroQuest.sln` is generated, untracked, and can be opened in Visual Studio to
build the same targets.

The presets need CMake ≥ 3.30 (`"version": 9`). Visual Studio 2022 ships a new enough one
at `Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`; a mingw CMake on
`PATH` may be too old. The first configure builds `cpp-httplib` and `nlohmann-json` from
source and takes ~17 minutes; later ones are seconds.

## Usage with an MCP Client

Add one server entry per character to your MCP client config, each pointing at that character's port:

```json
{
  "mcpServers": {
    "everquest": {
      "type": "http",
      "url": "http://127.0.0.1:8280/mcp"
    },
    "everquest-vtan": {
      "type": "http",
      "url": "http://127.0.0.1:8281/mcp"
    }
  }
}
```

Pin these ports below the auto range (8284–8315). Otherwise, an unpinned client that starts first can take a pinned character's port.

Pin each character's port (`/mcp port <n>`) so the entries stay correct no matter which order the clients start in.

All tools (`get_state`, `execute_command`, `navigate_to`, `say`) will be available to any connected MCP client.

## Usage with a Python Agent

`eq_agent.py` is a standalone Python orchestrator that connects directly to MQMCP over HTTP and drives the character using an LLM agentic loop.

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

### Ports

Every boxed client loads its own copy of the plugin, so each one needs a different port.

- **Pinned:** `/mcp port <n>` saves the pin in `MQMCP.ini` as `[Ports] <CharName>=<n>`. You can also edit that section by hand. If the pinned port is already taken, the server falls back to auto and says so in chat.
- **Auto:** An unpinned client binds the first free port in `8284`–`8315`. The client that starts first gets the lowest port, so auto ports can change between sessions.
- **Loaded at char select:** The character isn't known yet, so the server starts on auto. Once you enter the game, it moves to that character's pinned port.

The scan range is set by `k_defaultPort` and `k_portSpan` in `MQMCP.cpp`.

### Client registry

Each client records which port it is on in `config/MQMCP_Clients.ini`, as `[Clients] <port>=<name>,<server>,<pid>`. The name and server are empty at char select. A client writes its entry when it binds a port and when it enters or leaves the game, and removes the entry when it stops. Scripts can read this file to find a character's port, including an unpinned one. Use `list_clients` from an MCP client.

The registry is its own file rather than a section of `MQMCP.ini`. The Windows profile API rewrites the whole file on each write, so two clients writing at once can lose one of the changes. In the registry that fixes itself, because every client checks its entry every 5 seconds and rewrites it if it's missing. In `MQMCP.ini` it could drop a port pin. An entry can outlive a client that crashed, so confirm it with `GET /whoami` before trusting it. `list_clients` does this and removes the dead entries.

**Why the server binds with `SO_EXCLUSIVEADDRUSE` only:** cpp-httplib's default socket options leave a plain `SO_REUSEADDR` socket on Windows. Version 0.41 sets `SO_REUSEADDR` alone. Version 0.15.3 set `SO_REUSEADDR` and then `SO_EXCLUSIVEADDRUSE`, but Winsock rejects that combination, the second call fails, and httplib doesn't check the result. As a result, every client successfully bound `8284`, and the newest one silently took over its connections. With exclusive-only binding, a port that is already in use fails to bind and the server moves on to the next one. Don't go back to httplib's defaults.

`eq_agent.py` connects to `MCP_URL`, which is set near the top of the script to `http://127.0.0.1:8284`. Change it to drive a character on another port.

## Notes

- Commands are executed on the EQ main thread (game pulse) — never from the HTTP thread
- Requests that carry an `Origin` header are refused with 403, and no CORS headers are sent. Browsers add that header and MCP clients don't, so a web page can't drive the client through `127.0.0.1`.
- Game state is read on request, on the next pulse after it is asked for (~100ms at most)
- Nearby spawns are capped at 60, sorted by distance, within 300 units
- MQ2Nav must be loaded for `navigate_to` to work
- The plugin does not suppress any chat (all `OnIncomingChat` handlers return `FALSE`)
- Group member mana is read from the spawn data; accuracy depends on what the server sends to clients

## License

GPL-3.0 — see [LICENSE](LICENSE).
