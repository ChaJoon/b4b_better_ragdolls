# Joining friends through Steam (Join Game, invites, launch)

Goal: a friend clicks **Join Game** (or accepts an invite) in Steam, before or after starting B4B, and lands in the
host's b4bcoop session. No ini edits, no IP typing. Code: `native/src/presence.c`, small hooks in `cmds.c`
(session join target, `coop_join`), `testing.c` (auto sign-in for a Steam join), `travel.c` (travel counter).

## How it works

| Step | Mechanism |
|---|---|
| Advertise | While the local world is a listen server (offline Fort Hope or a mission), the host sets Steam rich presence: `connect` = `+b4bcoop_join steam:<host id64> proto:<n> ver:<x.y.z>` (plus ` addr:<ip:port>` only with `host_ip=1`), plus `status` ("b4bcoop: hosting Fort Hope (2 players)"), `steam_player_group` / `steam_player_group_size` (Steam groups the party in the friends list). Not while the sign-in screen is up (hosting, the default, makes even the title's Fort Hope a listen server) and not while a Steam join target is pending. Keys are cleared 15 s after hosting stops (the delay covers server travel, where the world briefly has no NetDriver). The game's own presence push replaces the whole key set on every state change (see below), so every 2 s the agent reads its `connect` back (a local cache call) and re-applies its keys if they're gone. It never calls `ClearRichPresence`, so the game's keys stay alongside ours. |
| Join while running | Steam posts `GameRichPresenceJoinRequested_t` (id 337: `CSteamID friend; char connect[256]`) to the running game. Our callback copies the string and the game thread handles it. |
| Join at launch | Steam starts the game with the connect string on the command line. `presence_init` scans `GetCommandLineW()` for `+b4bcoop_join` (and, once Steam is up, `ISteamApps::GetLaunchCommandLine` as a fallback for `steam://run/924970//…` launches). |
| Version check | `proto:` must equal our protocol (`VERSION`), else the join stops right there with "Could not join: Host runs b4bcoop 0.4.0 (protocol 2); you have 0.3.0 (protocol 1). Everyone needs the same version." (a connect string without `proto:` = an older b4bcoop). The host's login gate (admin.c) checks the same for every join. |
| Join target | The connect string becomes a session join target `steam:<id64>,<ip:port>` (`cmds_set_session_join`). It overrides `host=` and `join=` from the ini for this session, arms the Offline auto sign-in, and the existing auto-join loop joins once the player is signed in and standalone in offline Fort Hope. A target list is tried in turn: an alternative that starts no travel (no `SetClientTravel` call, e.g. `steam:` without the P2P transport) is skipped immediately. With `host_ip=0` (default) an `addr:` on another machine is dropped at parse time. If the player is hosting or already in a session when the request arrives, the agent leaves and joins immediately (they clicked Join). After 3 minutes without a connection the target is dropped and the ini applies again (since #41: one retry loop for every join, docs/investigations/join-timing.md). |
| Sign-in | `testing_arm_signin()`: the `offline=1` machinery (press Sign in, answer the Online/Offline popup with Offline) runs for 10 minutes from the join request. If the player is already past the title screen (Fort Hope, no `SignInScreen`) it is a no-op. Join attempts wait until sign-in finishes. |
| Invites | Host: the Steam overlay / friends list "Invite to Game" uses the rich-presence `connect` string, so it needs nothing extra. Agent command `invite <id64 or name>` calls `ISteamFriends::InviteUserToGame(friend, connect)`; the name is a case-insensitive substring of persona name or nickname and must match exactly one friend. `friends [all]` lists online friends playing B4B (app 924970) with their rich presence. |

Connect string parsing is strict, because it comes from another player and ends up in a console `open` command:
`steam:` needs 5-20 digits, `addr:` accepts only `[A-Za-z0-9.-]` plus an optional `:digits` port. Anything else
is logged and ignored (`?listen`, `;exec`, `|`, spaces, and extra colons are all rejected). A bare `host:port`
token with a dot is taken as an address, so `+b4bcoop_join 1.2.3.4:7777` works too.

### Address fallback (host_ip=1 only)
Since 0.3.0 hosts advertise no address by default: joins are Steam-only and the game's socket is bound to 127.0.0.1
(steam-p2p.md, "Loopback binding"); `presence_addr` is ignored without `host_ip=1`. With `host_ip=1` (advanced):
the host advertises `addr:` from `presence_addr=host[:port]` in `b4bcoop.ini` if that key is set, for example a public IP or DNS
name with UDP forwarded, or a Tailscale IP. Otherwise it advertises the first up, non-loopback LAN IPv4 (`GetAdaptersAddresses`,
no traffic) and the listen port (`-Port=` on the command line, else 7777). Steam 1.47 has no client-side
"my public IP" call (`ISteamUser::GetPublicIP` doesn't exist yet; only the game-server interface has one), so a
LAN address is the only automatic choice. It works on a LAN or VPN; over the internet it needs `presence_addr` and a
forwarded port. `presence=0` turns advertising off.

### Steam P2P hand-off
`void coop_join(const char *target)` in `cmds.c` is the entry point that the P2P branch replaces. Here it runs the
existing UDP join for `ip[:port]` and logs `steam: transport not available (steam:…)` for `steam:` targets. Once
the P2P version starts a travel for `steam:` (any `open …` goes through `SetClientTravel`, which `travel.c` counts),
the target list order makes it the first choice, and the address becomes the fallback on the next attempt. No other
change is needed after the merge.
Landed in `steamnet.c` (docs/investigations/steam-p2p.md): `steam:` targets now start a travel over Steam P2P, and
`connect` carries `steam:` only while Steam P2P is on (`steam_p2p=0` drops it); the host takes both at once.

## Decisions

**steam_api binding.** The game ships steam_api64 **v1.47**, delay-loaded by OnlineSubsystemSteam from
`Engine/Binaries/ThirdParty/Steamworks/Steamv147/Win64/`. Its flat API predates the `SteamAPI_SteamFriends_v017()`
accessors (1.48+) and `SteamAPI_ManualDispatch_*`. What exists: `SteamInternal_FindOrCreateUserInterface(hUser,
"SteamFriends017")` (the same call the game uses; `SteamFriends017` / `SteamUser020` are the version strings in
the exe), `SteamAPI_ISteamFriends_*(ISteamFriends*, …)`, `SteamAPI_RegisterCallback`. Disassembly of the flat
wrappers confirms that `CSteamID` is passed and returned as a plain uint64 (`GetFriendByIndex` / `GetSteamID` return `[rsp+..]`
in rax). The agent only uses `GetModuleHandle` and never loads the DLL itself, so it binds once the OSS has loaded it and
`SteamAPI_GetHSteamUser()` is non-zero.

**Callback registration.** Manual dispatch doesn't exist in 1.47, so the agent registers a C-built `CCallbackBase`
(`{vtbl, uint8 flags, int32 id}`). MSVC orders the overloaded virtuals as `Run(p, bIOFailure, hCall)` then `Run(p)`,
then `GetCallbackSizeBytes`. Both Run slots point at one function that reads the payload from the second argument, so the
order can't be wrong. `SteamAPI_RegisterCallback` in this steam_api is an unlocked map insert, and UE's OSS pumps
`SteamAPI_RunCallbacks` on its online thread. Registering from the game thread could race that dispatch loop, so
the agent hooks `SteamAPI_RunCallbacks` (MinHook) and registers from inside the first call, on the pump thread,
before the original dispatch. Steam keeps a multimap per callback id, so the OSS's own 337 handler still runs.
Its `FOnlineAsyncEventSteamInviteAccepted` looks for `SteamConnectIP=` and logs `Failed to parse connection URL` for
ours. The known side effect: it leaves its `CurrentSessionSearch` set, which blocks later **Steam OSS** session searches in that
process. Offline B4B never uses those (sessions are EOS/Hydra online, and our own join offline).

**Why not hook the game's own invite handling.** The strings in question (`Ignoring session invite …`,
`ClientOnGroupInviteFrom`, `OnSessionUserInviteAccepted failed to parse compact presence`) belong to Gobi's online
party flow. It runs on EOS/Hydra sessions with "compact presence" custom data, needs Online mode and WB services, and
filters on Hydra relationships. Offline none of that exists. The Steam OSS path only knows `+connect <ip>` /
`SteamConnectIP=` for **registered Steam game servers**: it would run a Steam server query for the IP and fail, and B4B
would then show its own invite-failed flow. So the agent uses its own token (`+b4bcoop_join`, deliberately not `+connect`,
which `FOnlineSessionSteam::CheckPendingSessionInvite` would grab at launch) and its own callback, and doesn't touch the
game's paths.

## Commands
- `presence [on|off]`: shows the binding, callback state (pump thread, RunCallbacks count, join requests), the keys set, our own rich
  presence read back from Steam, and the session and launch join targets.
- `steamjoin <connect string>`: simulates a join request through the same queue the Steam callback uses.
- `invite <steamid64|friend name>`: sends a Steam game invite with the current `connect` (host only).
- `friends [all]`: lists online friends playing B4B with their rich presence (`all`: every online friend).
- `join steam:<id64>`: goes through `coop_join`.

## Local checks (one account, Proton)
Two local copies on a single account, Proton, 2026-09-24. They ran from a private game-dir shadow (symlinks plus own exe and DLL) and
separate test prefixes and ports, because another agent was testing in the shared game dir at the same time.

| Check | Result |
|---|---|
| Binding | `presence: steam bound, user 76561198063588550` about 10 s after DLL load (steam_api64 is loaded by then). The callback registered on the OSS pump thread (Wine tid 888, not the game thread), and `RunCallbacks` is pumped continuously (~50/s). |
| Advertise | After sign-in (not on the title screen, after the fix) `connect = +b4bcoop_join steam:<id> addr:192.168.1.174:7797`, `status`, and the group keys were **read back from Steam** (`presence`). `status` tracked the player count (1 → 2) and switched to "hosting a mission (2 players)" across `mission Easy`. `connect` was never cleared during the server travel. |
| Game's own presence | **Finding:** the game pushes Steam rich presence itself even offline: `AppId=Gobi`, `Platform=Win64`, `CustomData=/…`, `steam_display=Solo in Fort Hope` (`LogPresence: SetPresence … TEXT_RP_FortHope_Solo`, and at mission start `Playing Campaign: Tunnel of Blood on Recruit`). Each push dropped our keys. With the first version (30 s check), `connect` stayed missing for up to 30 s. With the 2 s check, both sets coexist (read back: the game's 4 keys plus our 4). Friends therefore see the game's localized `steam_display` text plus Join Game. |
| Launch path (step 3) | Copy 2 started with `+b4bcoop_join steam:<id> addr:127.0.0.1:7797` on the command line and `host=1` (the real game-dir default), no `offline=1`. Log: `command line carries a join` → `session join target steam:<id>,127.0.0.1:7797` → `auto sign-in (Offline) armed` → StartSignIn → popup answered Offline → `signed in` → `joining steam:<id>` → `steam: transport not available` → `started no connection, trying the next target` → `joining 127.0.0.1:7797` → `Welcomed by server` 2 s later. host=1 was overridden: no auto-host. |
| Running, title screen (step 4, simulated) | Copy 2 idle on the title (Online/Offline popup open, listen camp from host=1), then `steamjoin <string>`: armed → signed in Offline → joined 14 s after the request. The first version joined immediately during sign-in (it treated the title's listen camp as "in a session"). Fixed: the title counts as "wait", and an empty own listen camp may be left for a Steam join. |
| In a session (step 5, simulated) | Connected client, `steamjoin`: `leaving the current session to join` → re-joined at once. |
| Hostile string | `addr:127.0.0.1:7797;exec quit` → token rejected, only the `steam:` target kept (no connection, nothing executed). Parser unit-checked on the host with 10 strings (`?listen`, a pipe, `a:b:c`, `:7777`, trailing garbage are all rejected). |
| `friends` / `invite` | `friends` → `0 of 12 friend(s) shown`, `friends all` → 4 online. `invite zzqqxx` → no match. `invite <own id>` → `InviteUserToGame` returned true, and no 337 came back to the sender (expected; nothing was sent to real friends). |
| Regression | `multi.sh 2` with the standard ini `host=1` / `join=127.0.0.1:7797` + `offline=1`: host sees 2 players as before. The joining copy doesn't advertise. |

Not testable on one account: a real `GameRichPresenceJoinRequested_t` delivery (the queue behind it is the tested
`steamjoin` path), Steam's Join Game menu itself, and a Steam-initiated launch with the connect string.

## Two-account test plan
Host A and joiner B, two Steam accounts that are friends, both owning B4B, both with this build installed (the
player zip: `X3DAudio1_7.dll`, no launch options; on Windows the root `xinput1_3.dll` redirect makes a Steam-initiated
launch load the agent too, unverified). No ini needed on either side (0.3.0: hosting is the default, joins go over
Steam P2P; a Steam join overrides the joiner's ini). Steps 6 and 11 below predate that and are kept for the record. Evidence: `b4bcoop-<pid>.log` lines prefixed `presence:` / `auto:` / `testing:`, and
`tools/b4b.py presence`.

| # | Step | Expected | Confidence |
|---|---|---|---|
| 1 | A starts, signs in Offline, reaches Fort Hope (auto-host). | A's log: `presence: steam bound`, `presence: advertising connect="+b4bcoop_join steam:<A> addr:…"`. `presence` on A shows the keys read back. | High (verified locally) |
| 2 | B (game closed) opens Steam friends: A shows "Back 4 Blood", right-click shows **Join Game**; with the chat window, the arrow next to A's name also shows Join Game. | Join Game is present. The line under A's name is the game's own `steam_display` ("Solo in Fort Hope"; Steam's new UI doesn't render our `status`). | Medium-high: `connect` is what Steam keys Join Game on; not seen from a second account yet |
| 3 | B clicks Join Game with the game **closed**. | Steam launches B4B with `+b4bcoop_join …` appended. B's log: `presence: command line carries a join`, `auto: session join target steam:<A>,<addr>`, `testing: auto sign-in (Offline) armed`, `testing: StartSignIn`, `answering online/offline popup with Offline`, then `auto: joining steam:<A>` → `steam: transport not available` → `auto: joining <addr>`, and B appears in A's camp. | Medium on Linux (the arguments must survive the launch chain and the bootstrapper); low on Windows (see step 6) |
| 4 | B quits to desktop, starts the game normally (not signed in yet), and at the title screen clicks Join Game in Steam. | `presence: steam join request from <A>: …` arrives from the callback. After that it matches step 3 from "armed". | Medium-high: callback id/layout are standard SDK and registration is verified locally, but the callback itself is only simulated so far |
| 5 | B in their own offline Fort Hope (hosting, host=1) clicks Join Game on A. | `presence: leaving the current session to join`, joins A. B stops advertising at once (`presence: cleared`: a pending Steam target means "not hosting"). | Medium-high |
| 6 | Windows B: a normal Steam launch goes through EAC and the DLL doesn't load, so step 3 can't work. Start with `Play B4B co-op.cmd` first, then use step 4/5 (callback path). Optional experiment: Steam launch options `"<game>\Gobi\Binaries\Win64\Back4Blood.exe" %command%` so Steam starts the Gobi exe directly. | Step 4/5 work. The launch-option trick is untested. | Medium (step 4/5 path); low (trick) |
| 7 | A: `tools/b4b.py invite <B's name>` (or Steam overlay → friends → Invite to Game). | A: `invite <B>: sent`. B gets a Steam game invite; accepting behaves like step 3 (closed) or step 4 (running). | Medium: `InviteUserToGame` with our string is standard; overlay invite uses the same `connect` |
| 8 | A: `tools/b4b.py friends` while B is in game. | B listed with `[Back 4 Blood]` and B's rich presence (B hosting → B's `connect`). | High |
| 9 | A starts a mission (servertravel). Watch A's `presence` during the load. | `connect` stays set (15 s hysteresis) and status switches to "a mission". B joining mid-mission via Join Game hot-joins through the usual path. | Medium-high |
| 10 | A quits. B, still connected, drops to its own camp. | B retries A for 3 minutes, then `auto: … giving up`; the ini (host=1) applies again. A's rich presence disappears with A's process. | High |
| 11 | After the P2P merge: repeat 3/4 without `presence_addr`. | `auto: joining steam:<A>` starts a travel, and the address isn't needed. | Depends on the P2P branch |

## Two-account result (2026-10-05)
Steps 4/5/7/9 with the real callback: the client's Steam (dreamsofants, Flatpak) opened the friends-list Join Game
URL (`steam://rungame/924970/<host id>/<connect>`) and the running game got `GameRichPresenceJoinRequested_t` from
the host (Hergmgurk, native) every time: 21/21 joins (host in camp, loading a mission, in the lobby, mid-mission,
and the session runs' rejoins) and 2/2 after the host's `invite <id>` (`sent`). Details and timings: join-timing.md "Two-account check". Not clicked in
Steam's UI (the URL is what the click opens). Since then (2026-10) Dan has used Join Game from Steam's friends list
for weeks, also with the game closed (Steam starts it and it joins): both work.

## Limits / follow-ups
- Clients don't advertise. A friend of B (not of A) can't Join via B. Possible later: B mirrors A's `connect`.
- Join from **Online** mode isn't handled specially. The auto sign-in only acts on the title screen, so a player who
  is already signed in Online and clicks Join will attempt the join from the online camp. That's untested; return to the title
  first.
- A second join request replaces the first target. Steam groups (`steam_player_group`) only use the host's value.
