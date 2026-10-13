# Host hand-off

Goal (Dan): when the host leaves, another player hosts instead of the session ending for everyone (players lose the
multiplayer run today); as a start, the host names someone else as host mid-match.

Result: a prototype in `native/src/handoff.c` hands the session over in Fort Hope and in a mission, on `/handoff`
(the `~` Players tab's **Make host**) and automatically when the host quits or drops. In a mission the campaign run
continues on the new host from the start of the current chapter, with every player's run state (hero, cards, copper,
loadout) as the run saved it. Verified live with three local copies (lane 2, loopback); not yet across two Steam
accounts (below).

## 1. What lives where

| State | Where | Notes |
|---|---|---|
| The campaign run (map to resume, next map, difficulty, retries, state) | **host's profile only** | `FPlayerProfileData.CampaignRuns` (TMap<int32, `FPlayerProfileCampaignRun`>, 0x60 each), offline data at `UPlayerProfileSettings` +0xE0, map at +0x178. Clients log `campaign run applied: 0` / `clearing campaign run data`; their profile has no copy (client-rewards.md §3). |
| Per-slot run state: owner (`offline.<steamid64>`), hero row, customization, copper, `bHasChosenLoadout/Cards`, consecutive maps, `HeroRespawnSnapshot` (health, loadout items + attachments + ammo, currency, active gameplay cards, burn cards played this map, bonus draws, ...) | inside the run record's `Data` | `FPlayerProfileCampaignRun.Data` is a JSON FString of `FCampaignRunData` (≈25-50 KB) with a `rowHandleNameCache`: self-contained, nothing points into the host's profile. |
| Game-level cards (corruption/difficulty cards, card random stream) | `Data.gameplayCardTravelData` | Same JSON. |
| Run progress counters (`runPersistentFloats/Ints`, dungeons entered, threat at map end, `NumTimesMissionRestarted`) | `Data` | Same JSON. |
| Mid-chapter state: positions, enemies, objectives, items picked up and copper earned *in this chapter*, corruption-card progress | host's world only | Not saved anywhere; lost with the host. |
| Each player's own profile (supply points, decks, unlocks, burn-card stock, cosmetics) | each player's machine | Unaffected by who hosts (rewards.c/burncards.c already route a remote player's gains to their own profile). |
| Which run is being played | `UCampaignRunManager` (GameInstance.CampaignRunManager) +0x160 run id, +0x168 owner | `JoinRun` sets it: the resumed run's id, **0 for a new run** until the run is first saved. |

When the record is written: `creating campaign run %i [OFFLINE]` at the end of the first chapter (`mission completed,
saving run`), `updating ...` at every later chapter end. So during chapter N (N > 1) the record says "resume at chapter
N" with everyone's state from the start of chapter N; during chapter 1 of a new run there is no record yet.

### Game functions used (build 14216215)
- `0x141BC1780` `PPC::UpsertOfflineCampaignRun(PPC*, int32 Id, const FString& Map, const FString& Next, uint8 Difficulty,
  int32 Retries, uint8 State, const FString& Data, int32 DataVersion)`: finds or adds the run in the *local* player's
  offline data (`0x141BC6930` = the PPC's `UPlayerProfileSettings`), copies the fields, stamps the time and saves.
  Refuses a new run when the profile holds `PlayerProfileManager.MaxCampaignRuns` (cvar int at `0x1463FA838`, 16) runs.
- `0x141AE9240` `UMatchmaking::JoinRun(this, const FString& Map, int32 RunId, FString OwnerId /*by value, callee frees*/,
  uint8 Difficulty, uint8 Pool, uint8, uint8 bTraining, uint8* Voice)`: the war table's start and Continue (caller
  `0x141D1ACF0` logs "Joining campaign run Map: %s Difficulty: %s Run: %d"). Offline: `set local campaign run id %d %s`,
  then the `?RunOwner=1` travel that travel.c turns into a listen servertravel; on the mission the run component logs
  `Initialized campaign run component for ResumedRun ... loading campaign run %d from profile ... campaign run applied: 1`.
  A full map path works (`/Game/Maps/Missions/Evansburgh/MAP_PERS_Evansburgh_C`).
- `0x146972E60` `FUrlConfig::DefaultPort` (`-Port=` / `[URL] Port`): the port a map opened with `?listen` binds.
- `0x1424CBAE0` `FWindowsPlatformMisc::RequestExit(bool)`: `quit`, the game's Quit, closing the window.

## 2. Options

| Option | Feasible | Work | Notes |
|---|---|---|---|
| a. Hand off in Fort Hope | yes, done | small | Coordination only: who hosts, everyone joins them. |
| b. Hand off in a mission, restart the current chapter, run kept | **yes, done** | medium | The run record is portable: copy it into the new host's profile with the game's own upsert and Continue it. Players keep what the run saved for their slot. |
| b'. First chapter of a new run (no record yet) | yes, done | small | New run on the same chapter and difficulty; cards drafted this chapter are lost (nothing saved them). |
| b''. Automatic when the host quits / crashes | yes, done | medium | Plan and run are pushed to every client ahead of time, so nothing is needed from the host at the moment it goes. |
| c. True mid-match migration (positions, AI, objectives, pickups) | no | very large | UE4 has no host migration: every replicated actor's authority, the AI director's state, spawners, objective/encounter state machines, pickups and the PlayerSlotManager live as server-only objects; the client copies are partial replicas (no AI controllers, no director, no game mode). Rebuilding a world from replicas would need per-class serialization of Gobi's game state (hundreds of classes, obfuscated build). A "chapter restart" is the game's own checkpoint model (a wipe does the same), so (b) is the practical ceiling. |

Why (b) works so well: the offline game already supports resuming a run from the profile (war table Continue), and the
saved slot data is keyed by owner id (`offline.<steamid64>`), which is the same on every machine. The new host finds
its own slot by its id; returning players should claim theirs (`claimed reserved slot N`, retail resume behaviour;
with one shared Steam id locally this could not be told apart, see §5).

## 3. The prototype (`native/src/handoff.c`)

**Host side** (every second while it hosts players): each client gets a plan as silent data lines (`b4bcoopdata`,
admin.c `admin_data_to`): `handoff plan host <port> <n> <name>` for the next host, `handoff plan join <addr> <n> <name>`
for the others. Next host = the player named with `/handoff`, else the first other player in the list. Address:
`steam:<id64>` for a Steam P2P peer (steamnet.c knows it sent us packets), else `127.0.0.1:<our port + 1>` (local copies:
the next host is told to listen there; host_ip=0 allows only loopback anyway). With `host_ip=1` to another machine there
is no address we could know (no hand-off; logged).
In a mission it sends the run: `handoff run <seq> <id> <diff> <retries> <state> <dver> <len> <map> <next> <path>`, the
JSON as wide-text chunks of 3000 characters (`handoff chunk`, `admin_data_to_w`: the data line is not cut to a chat line;
chat.c passes `handoff ...` lines to handoff.c whole), `handoff end <seq> <fnv32>`; again whenever the record changes
(chapter end). The current run: the run manager's id, else (a new run) the newest in-progress record written since the
host left Fort Hope. First chapter of a new run: `handoff newrun <diff> <path>`. In Fort Hope: `handoff run none`.
`<path>` = the current mission if the record resumes there, else the same folder's `MAP_PERS_<map>` (post-round: the
next chapter).

**Triggers**
- `/handoff <name|#>` (CMD_HOST inside handoff.c; `/handoff` alone shows the plan to anyone), overlay **Make host**:
  plans are sent, then `handoff go`; 3 s later the old host sets the next host as its join target and disconnects
  (it joins from its own camp, where the join loop retries).
- Host quits: hook on `RequestExit`: `handoff go quit` to everyone who has a plan, the real exit 1 s later (without
  it the clients only noticed after the 90 s connection timeout: the host's close bunches never arrived).
- Host crashes / connection lost: uelog.c sees `BroadcastNetworkFailure: FailureType = ConnectionLost|ConnectionTimeout`
  (joins and follows only ever fail as `PendingConnectionFailure`: every older client log has only those), travel.c a
  `?closed` with the host still set (not our /leave, not a kick: both clear it first). Either requests the plan.
  `host_handoff=0` (ini, live) turns the automatic take-over off.
- Requests run on the next tick (not inside the network receive path or a log call).

**Client side**
- Next host: listen port set (loopback only), `cmds_become_host()` (no join targets, auto-host on), no rejoin of the
  old host, disconnect → own Fort Hope → auto-host opens it with `?listen`. In a mission: waits until the other players
  are back in its camp (all of them, or 60 s), 5 s after the last one, then upserts the run into its own profile (raising
  the 16-run limit by one for that call if the profile is full) and calls `JoinRun` with the run id and its own
  HydraPublicId; first chapter of a new run: `JoinRun` with run 0. Everyone follows the servertravel.
- Others: the next host becomes the session join target; they join it from their camp once the old host is gone
  (retries 3/8/15 s while it is still loading, cmds.c).

**Protocol**: bumped 2 → 3 (`VERSION`): clients must understand the new data lines; a mixed session would leave older
clients behind on a hand-off. No new RPCs: data lines over the existing `ClientTeamMessage` path.

**Commands**: chat `/handoff [<name|#>]`; dev `handoff` (status), `handoff runs [id]` (the profile's runs, the run
manager's id), `handoff resume <id> [path]` (Continue one of our own runs through `JoinRun`, as a take-over does),
`handoff go` (client: act on the plan now), `handoff <player>`. Ini `host_handoff`. docs/COMMANDS.md updated.

## 4. Live results (2026-10-13, lane 2, Proton, dev build, three local copies on one Steam account, 4090 headless)

All on loopback (one Steam account: every copy is `offline.76561198063588550`, so the slot-owner matching is
degenerate: the first player back claims "reserved" slot 0/1).

| Test | Result |
|---|---|
| `handoff resume <id>` on a host with 2 clients (Continue an existing run, Evansburgh_C) | `ResumedRun`, `campaign run applied: 1`, heroes and loadouts from the record, both clients followed. |
| Fort Hope, `/handoff #1` | Everyone in the new host's camp 25 s after the command (old host and the third player joined over 127.0.0.1:7888). |
| Chapter 1 of a new run, host `quit` (before the RequestExit hook) | Clients noticed only after the 90 s timeout; then the next host started a new run on Evansburgh_B, the other followed. Led to the RequestExit hook. |
| Chapter 2 (Evansburgh_C, record from the end of B), `/handoff #1` mid-chapter | 28 s to `JoinRun` on the new host, everyone `Welcomed` in C 45 s after the command; `Initialized campaign run component for ResumedRun ... run:1791911751`, `campaign run applied: 1`; heroes, cards and copper from the record (HUD: 1,468 copper); screenshot of the new host playing. |
| Same run, the new host `quit`s (chained hand-off) | `handoff go quit` received at once; third host continued run 1791911751 30 s later, the remaining player in the mission 46 s after the quit. |
| Chapter end on the third host | `updating campaign run 1791911751 [OFFLINE]` (resume at Evansburgh_D), SP forwarded to the remote player (`rewards: forwarding AdjustSupplyPoints (84)`), new record pushed to the client. |
| Chapter 2, host `quit` with the hook | `go quit` → next host continued (30 s to `JoinRun`). |
| Final build (after a chunk-parsing fix), chapter 2, `/handoff #2` | `JoinRun` 29 s after the command, `ResumedRun` + `campaign run applied: 1` on the new host, both others `Welcomed` in C 46 s after it. |

Found and fixed during the runs: Fort Hope's path is under `/Missions/` (was taken for a mission); a new run's record
was taken from an older in-progress run (now: only records written since leaving Fort Hope, kept over the chapter
transition map); the old host joining straight from its mission stayed alone in it after a failed first attempt (now it
disconnects and joins from its camp); pending connections counted as players back (now player states); plans the old
host sent after `go` overwrote the clients' plan (now ignored once a hand-off started).

Local-copy artefact: when the next host disconnected from the old host, the old host logged `offline.<id> has
disconnected ... processing abandonment w/o penalty`, `campaign run abandoned`: the departing client had the run owner's
id (all copies share it). With real accounts the departing player is not the owner.

`tools/e2e.py --quick` on the final build, lane 2: 14/14 (protocol 3 in presence and login).

## 5. Not verified / open
- **Two Steam accounts** (Steam P2P addresses, `steam:<id64>` plans, slot ownership with distinct ids): not run. The
  Flatpak Steam (dreamsofants) was running with a shared IPC namespace; `launch/flatpak-steam.sh start` restarted it
  isolated but it did not log on by itself (login prompt), so lane 2's two-account setup was unavailable, and lane 1
  was held by another session. Next: `tools/jointest2acct.py`-style run, host native, client Flatpak, `/handoff` both
  ways and a host `quit` in a mission; check `claimed reserved slot` per owner and that the old host's slot keeps its
  cards when it rejoins.
- Real multi-machine: a crashed host is noticed after `ConnectionTimeout` (90 s, raised in cmds.c for slow loads).
- `host_ip=1` sessions to other machines: no hand-off (address unknown); would need the client to report its public
  address and forwarded port.
- The old host's own profile keeps its copy of the run (still in progress); both copies can be continued separately.
  The new host's profile may hold one run more than the game's limit of 16 (the war table then needs one deleted before a
  new run).
- The next host waits for *all* other players (60 s cap) before continuing; a player whose game is slow to load joins
  the mission late (the retail hot-join path, taking over its bot).
- Not checked: the host leaving a mission through the game's own menu (to its Fort Hope or the main menu) instead of
  quitting; whatever the clients see then (a lost connection or a travel) decides whether the take-over starts.
- Mid-chapter progress is lost by design (option c).
