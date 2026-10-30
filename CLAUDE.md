# MQMCP

MacroQuest plugin exposing EverQuest game state and control over MCP (JSON-RPC 2.0 over
HTTP, `POST /mcp`). One instance per EQ client. See README.md for the tool list.

Lives in the openvanilla MacroQuest fork at `plugins/mqmcp` (its own git repo,
`ew6082/mqmcp`). Renamed from MQMCPServer / `mq-mcp-server` on 2026-10-27; the upstream
remote (`niccellular/mq-mcp-server`) still uses the old names, so expect conflicts on
those when merging from it.
Main consumer: the rip raid-script repo at `C:\Users\ew608\Desktop\MQ\lua\rip`, whose
`.mcp.json` registers one server entry per character.

## Build

README.md "Building in this fork (openvanilla)" is authoritative — read it before
building; several prerequisites live in the openvanilla tree and a `git pull` there
reverts them. What works today:

- The full build works. From `C:\repos\openvanilla`, with VS's CMake (`Common7/IDE/
  CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`; a mingw one is too old):
  `cmake --build build --config Release --target MQ2Main MQMCP MQ2MyButtons2`.
  Output: `build/bin/release/MQ2Main.dll`, `build/bin/release/plugins/*.dll`.
- It depends on two untracked files at the openvanilla root, both already in the
  `build/` cache: `custom_plugins_scope.cmake` (`MQ_CUSTOM_PLUGINS_FILE`) and
  `local_build_overrides.cmake` (`CMAKE_PROJECT_INCLUDE_BEFORE`: vcpkg baseline, imanim
  sources, vcpkg lib path). If `build/` is wiped, pass both again at configure time
  (README "Build"). Don't "fix" the build by editing tracked openvanilla files or
  `contrib/vcpkg` ports. Under the old baseline those edits made no difference.
- The plugin's `EverQuestVersion` must match the installed MQ, or MQ refuses to load it.
  Check with `strings MQMCP.dll | grep "^[A-Z][a-z][a-z] [ 0-9][0-9] 20"` against
  the same on `C:\Users\ew608\Desktop\MQ\MQ2Main.dll` (currently `Oct 19 2026 13:46:43`).
  A stale date is the usual reason the MCP "won't connect" after an EQ patch.

## Deploy

The loaded DLL can be locked ("Device or resource busy"), but Windows allows renaming it:
move `C:\Users\ew608\Desktop\MQ\plugins\MQMCP.dll` aside (`MQMCP.dll.old`), copy the
build in, then on each client
`/plugin mqmcp unload` and `/plugin mqmcp`. If you trigger that *through*
the MCP itself, route the unload via `/timed` — commands run inside the plugin's own
OnPulse, and unloading from within its own callback is unsafe.

## Ports (one per client)

- `/mcp port <n>` pins this character (stored in `MQMCP.ini`, `[Ports] <Name>=<n>`)
  and rebinds immediately; `/mcp port auto` clears the pin. `/mcp` shows port + pin.
- Unpinned clients take the first free port in 8284–8315. A plugin loaded at char select
  starts on auto, and OnPulse moves it to the pin once the character is known.
- Binding is `SO_EXCLUSIVEADDRUSE` only. httplib's defaults leave a REUSEADDR socket on
  Windows (0.15.3: REUSEADDR then EXCLUSIVEADDRUSE, which Winsock rejects unchecked;
  0.41: REUSEADDR alone) — every client bound 8284 and the newest silently took over.
  Don't revert it.
- vcpkg now installs httplib **0.41.0**, where a failed `bind_to_port` decommissions the
  `Server` and every later bind on it fails. `StartServer` therefore makes a fresh
  `Server` per port tried; reusing one ended the scan at the first taken port with "No
  free port in 8284-8315" (2026-10-29: every unpinned client after Domolite on 8284).
- Pins (2026-10-29): Terminaar 8280, Vtan 8281, Davran 8282, Sundarr 8270, Grookka 8271,
  Warzan 8278, Tarkas 8279 — all below the auto range on purpose, so an unpinned client
  can never take a pinned character's port. Put new pins below 8284, not inside 8284–8315.
- `config/MQMCP_Clients.ini` (`[Clients] <port>=<name>,<server>,<pid>`) records which
  client is on which port; `list_clients` / `GET /whoami` / `query_peer` read it.
  The rip `.mcp.json` (gitignored, local only) registers them as `everquest`,
  `everquest-vtan` and `everquest-davran`.

## Security

Requests with an `Origin` header get a 403, and no CORS headers are sent (pre-routing
handler in `SetupRoutes`). Before this, `Access-Control-Allow-Origin: *` with no auth
let any web page POST `execute_command` to 127.0.0.1. Don't add CORS back.

## Game state

`get_state`, the `eq://` resources and the REST endpoints are built on demand from TLOs
(`Build*Json` + `RunOnGameThread`, a `GameTask` queue drained in OnPulse), not from eqlib
structs every pulse. The old struct snapshot and `/mcp snapshot` toggle are gone; a stale
`[Settings] Snapshot` key in the ini is ignored. Chat is out of `get_state` unless
`max_chat` is given. Gems are 1-based, buffs report `seconds`. **Not yet tested live**
(committed 2026-10-29 before deploy): run the scratchpad `test_state.py`-style checks
(timings, fields vs `query_tlo_batch`, every `eq://` URI) on the first load.

## To-do

In order of payoff. Chat sequence numbers, `chat_since`, `run_command` and `wait_for` are
done and confirmed live (2026-10-27). The rip scripts keep their `/echo` markers: they
also tell the user what's happening in the MQ chat window. Read them from the MCP with
`chat_since` (`tabs: ["MQ"]` + `filter`); any extra instrumentation for that is separate work.
Port discovery (`MQMCP_Clients.ini`, `GET /whoami`, `list_clients`, `query_peer`) is done
and confirmed live on all 12 clients (2026-10-29).

1. **Verify the TLO-built `get_state` live** (see Game state), then mark it done here.
2. **`navigate_to` by spawn/target** (`/nav id`, `/nav target`), optionally waiting until
   nav ends (via `wait_for`).
3. **Combat abilities on demand** (`eq://abilities` or a tool): known discs with ready /
   seconds remaining, optionally only those on cooldown. Not in `get_state` — a 130 knows
   hundreds. The upstream README documented an `abilities` list that was never implemented.
4. **Protocol version.** It reports `2024-11-05`. Moving to Streamable HTTP would let the
   SSE keepalive stub go. Not urgent while Claude Code accepts it.

## Chat tabs

Lines are tagged with a channel and the tab that shows them (see the comment block above
`ChannelFromColor`). The color -> channel table is hand-made; entries marked `guess` there
are unverified. Verified live on Terminaar (2026-10-27, combat dummy): 265, 273, 301, 327,
349, 356, 380, 397, 402, 13, 302 (your spell crits; the user routes them to Chat 3); and 365 (event, green raid/mission text) on a spite mission,
which rip's `utils/eventlog.lua` records via `chat_since` channels. The UI ini has a persona class suffix
(`UI_Terminaar_bristle_SHD.ini`); `FindUIIni` picks the current class's file. When the user
does a deploy, let them do it unless they ask; they often copy and reload themselves.

## Gotchas learned using it

- `run_command` / `wait_for` block an httplib worker (pool of max(8, cores-1); each SSE
  stream holds one permanently). Fine for a few concurrent callers. Long waits are why every wait
  checks `s_stopping`: StopServer joins workers from the game thread, where no pulse runs.

- TLO index syntax uses `[brackets]`; nested brackets inside `DanNet[x].Q[...]` do not
  resolve (`Me.Gem[1].Name` came back NULL). `DanNet.Q` also only returns values already
  fetched by `/dquery`. To read a peer, `/noparse /dex <peer> /dt <me> ${...}` and read the
  tell back from chat — or just use `query_peer(name, exprs)`.
- From Git Bash, a command string starting with `/` is mangled into a Windows path
  (`/target` → `C:/Program Files/Git/target`). Send commands from Python or PowerShell.
- Bard song/resist messages only reach `recent_chat` if the client's chat filters show them.
