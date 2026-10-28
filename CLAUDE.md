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
- Binding is `SO_EXCLUSIVEADDRUSE` only. httplib 0.15.3's default sets `SO_REUSEADDR`
  then `SO_EXCLUSIVEADDRUSE`, which Winsock rejects (unchecked), leaving a REUSEADDR
  socket — every client bound 8284 and the newest silently took over. Don't revert it.
- Current pins: Terminaar 8280, Vtan 8281, Davran 8282 — all below the auto range on
  purpose, so an unpinned client can never take a pinned character's port. Put new
  pins at 8283 and below, not inside 8284–8315.
  The rip `.mcp.json` (gitignored, local only) registers them as `everquest`,
  `everquest-vtan` and `everquest-davran`.

## Security

Requests with an `Origin` header get a 403, and no CORS headers are sent (pre-routing
handler in `SetupRoutes`). Before this, `Access-Control-Allow-Origin: *` with no auth
let any web page POST `execute_command` to 127.0.0.1. Don't add CORS back.

## Snapshot

The struct-reading snapshot (`get_state`, `eq://` resources, REST) is off by default.
Turn it on with `/mcp snapshot on|off` (`[Settings] Snapshot` in the ini, shared by all
clients). Confirmed working on 2026-10-27 against the `Oct 19 2026` MQ build, and on
in the ini since then. After an EQ patch, if a client crashes when the plugin loads,
set `Snapshot=0` first. Off, `get_state` returns only `inGame` + chat; use `query_tlo_batch`.

## To-do

In order of payoff. Chat sequence numbers, `chat_since`, `run_command` and `wait_for` are
done (2026-10-27). Once they're confirmed on a live client, update the rip scripts to use
them instead of `/echo` markers and client-side poll loops.

1. **Port discovery.** There's no way to list which character is on which port without
   probing 8284–8315. Write a `[Clients]` table to the shared ini on bind and/or add
   `GET /whoami` (name/server, cheap; `/me` returns zeros at char select). A
   `query_peer(name, expr)` could then remove the DanNet `/dex` + tell workaround.
2. **Slimmer `get_state`.** It still returns the full chat buffer (action tools now send
   only the last 20 lines). With `chat_since` available, chat could go from get_state
   entirely (keep `chat_seq`), or become opt-in (`max_chat`).
3. **Build `get_state` from TLOs** (one `query_tlo_batch`-style evaluation) instead of
   struct reads, so an EQ patch can't crash it and the snapshot toggle can go.
4. **`navigate_to` by spawn/target** (`/nav id`, `/nav target`), optionally waiting until
   nav ends (via `wait_for`).
5. **Protocol version.** It reports `2024-11-05`. Moving to Streamable HTTP would let the
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
  tell back from chat — or just query that peer's own MCP port.
- From Git Bash, a command string starting with `/` is mangled into a Windows path
  (`/target` → `C:/Program Files/Git/target`). Send commands from Python or PowerShell.
- Bard song/resist messages only reach `recent_chat` if the client's chat filters show them.
