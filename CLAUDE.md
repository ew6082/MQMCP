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

MQ does not lock plugin DLLs: copy the build straight over
`C:\Users\ew608\Desktop\MQ\plugins\MQMCP.dll`, then on each client
`/plugin mqmcp unload` and `/plugin mqmcp`. If you trigger that *through*
the MCP itself, route the unload via `/timed` — commands run inside the plugin's own
OnPulse, and unloading from within its own callback is unsafe.

## Ports (one per client)

- `/mcp port <n>` pins this character (stored in `MQMCP.ini`, `[Ports] <Name>=<n>`)
  and rebinds immediately; `/mcp port auto` clears the pin. `/mcp` shows port + pin.
- Unpinned clients take the first free port in 8284–8315. A plugin loaded at char select
  starts on auto, and OnPulse moves it to the pin once the character is known.
- Binding is `SO_EXCLUSIVEADDRUSE` only. httplib 0.15.3's default sets `SO_REUSEADDR`
  then `SO_EXCLUSIVEADDRUSE`, which Winsock rejects (unchecked), leaving a REUSEADDR
  socket — every client bound 8284 and the newest silently took over. Don't revert it.
- Current pins: Terminaar 8284, Vtan 8285, Davran 8586 (outside the auto range on purpose).
  The rip `.mcp.json` (gitignored, local only) registers them as `everquest`,
  `everquest-vtan` and `everquest-davran`.

## To-do

1. **Reading new chat is awkward.** `recent_chat` is a 4000-line ring buffer with no
   sequence numbers, so "lines since my command" can't be found by length once a chatty
   client (bards) has filled it; callers had to `/echo` a unique marker and search for
   it. A `chat_since` tool (monotonic line ids, or "since marker") would fix that.
2. **`get_state` is heavy** — it returns the full chat buffer every call, slow on chatty
   clients. Consider a `max_chat` argument, or dropping chat from get_state in favour of (1).
3. **Port discovery** — no way to list which character is on which port without probing
   8284–8315. Options: write a `[Clients]` table to a shared ini on bind, or a
   `GET /whoami` that returns name/server cheaply (`/me` returns zeros at char select).

## Gotchas learned using it

- TLO index syntax uses `[brackets]`; nested brackets inside `DanNet[x].Q[...]` do not
  resolve (`Me.Gem[1].Name` came back NULL). `DanNet.Q` also only returns values already
  fetched by `/dquery`. To read a peer, `/noparse /dex <peer> /dt <me> ${...}` and read the
  tell back from chat — or just query that peer's own MCP port.
- From Git Bash, a command string starting with `/` is mangled into a Windows path
  (`/target` → `C:/Program Files/Git/target`). Send commands from Python or PowerShell.
- Bard song/resist messages only reach `recent_chat` if the client's chat filters show them.
