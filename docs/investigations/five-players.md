# 5-player co-op (issue #1)

Build 14216215. Static analysis of `Back4Blood.exe`, the SDK dump, and host/client logs from 2026-09-23
(`b4bcoop-2972.log` host, `b4bcoop-776.log` client). Sections 1–4 are the phase-1 static work; **section 5 has the
live results (2026-09-24): 5 humans play a mission with `teamsize=5`.**

## 1. Where the 4 comes from

**There is exactly one source: the `APlayerSlotManager` class default `Config.TeamSize = 4`.** Retail has no fixed slot array.

| What | Where | Notes |
|---|---|---|
| `APlayerSlotManager` ctor | `0x14224C850` | `NumTeams=0` (OneTeam), `mov dword [rbx+0x2a4], 4` (TeamSize), `bSupportsBots=1`, `CharacterSelectClass=null` |
| `FSlotManagerConfig` | PSM `+0x2a0` (size 0x10) | `NumTeams` i32 `+0` (ETeamCounts: 0=One, 1=Two), `TeamSize` i32 `+4`, `CharacterSelectClass` `+8` |
| `APlayerSlotManager::InitSlots` | `0x141A14860` | Reads `+0x2a0`/`+0x2a4`, appends `NumTeams` × `FTeamSlots` (0x20: Team `+0`, MatchmakingTeam `+1`, Slots TArray `+8`, CharacterSelect `+0x18`), spawns `TeamSize` `PlayerSlot` actors per team, sets `SlotIdx` (`+0x2a0` on the slot). Logs `PlayerSlotManager::InitSlots with %d team(s) and %d slots`. |
| `APlayerSlotManager::Init(PlayerSlotClass)` | `0x141A13CB0` | Only caller of InitSlots. Creates one `CharacterSelect` per team afterwards. |
| `GobiGameStateBase` spawn of the PSM | `0x1419913C0` (virtual, 7 vtables) | `SpawnActor(GS.PlayerSlotManagerClass +0x368)`, stores GS `+0x390`, then `Init(GS.PlayerSlotClass +0x370)`. Nothing writes `Config` between spawn and `InitSlots`, so the value is the spawned class's default (a BP subclass may override it. Fort Hope's gives `NumTeams=Two`). |

- **InitSlots runs on the authority only.** The client log has `InitSlots` only for its own standalone Legal/Fort Hope
  loads, never after `Welcomed by server`. Clients get the slots through the replicated `TeamSlots`.
- **`Config.TeamSize` is only read in two places:** InitSlots, and `GetTeamSizeData`. `GetTeamSizeData` is the
  `UPartyPlayerBlueprintFunctionLibrary` thunk `0x1422FD8D0` and its inline copy `0x141D61AF0`, which is used by 6
  UI callers. When matchmaking state > 1, `GetTeamSizeData` returns `MatchmakingPoolConfig.MaxTeamA` (`+0x1c`).
  Otherwise it returns `GS->PlayerSlotManager->Config`. Everything else counts `TeamSlots[i].Slots.Num()`. No other
  write to `+0x2a4` exists in the PSM code region (`0x141A00000–0x141A2C000`). The rest of the `+0x2a4` hits there
  are `PlayerSlot.SlotIdx.Team` byte compares.
- **Matchmaking pools do not size the slots.** `MatchmakingPoolConfig` (0x48: Name, Min/MaxUnknowns, Min/MaxTeamA
  `+0x18/+0x1c`, Min/MaxTeamB, bBackfill, GameMode, bTeamSwap `+0x40`, …) lives in `MatchmakingPoolManager.PoolConfigs`
  (`+0x30`). Lookup by name: `0x141B1DEF0`. `?PoolConfig=Coop` becomes `GS.PoolConfig` (`+0x348`), which is only
  looked up for logging (`PoolConfig: %s` in `0x141991BD0`), bTeamSwap (`0x141A1D1E0`) and the UI team-size query.
  `EMatchmakingPool`/`EGobiMapGameModesType` include a **`Coop8P`** entry (and `MainMenuDevOptionsUserWidget::IsSelectedMapCoop8P`).
  This dev mode means the slot/UI code was at least meant to handle more than 4 heroes.
- **Hard-coded 4s on the slot and character-select paths:** none found apart from the ctor. The immediate-4 compares in
  the PSM region are container internals (`0x141A09916`, `0x141A09C66`). The one near CharacterSelect (`0x1419F736C`)
  is a `Scalability %d` debug cycler. Seamless-travel restore (`HandleSeamlessTravelPlayer`, `0x141A14560`) and the
  lineup layout manager (`0x141CBD60C`) bounds-check slot indices.
- **Join capacity:** the engine `AGameSession::ApproveLogin` (`0x143CC0625`) returns `"Server full."` when
  `GameMode->GetNumPlayers() >= (net.MaxPlayersOverride > 0 ? cvar : GameSession.MaxPlayers)`. The value of
  `GameSession.MaxPlayers` is unknown (it is in ini/assets). The cvar's `int*` is at `0x146967C10`, registered in
  `0x140A3F140` with default flags. There is no Gobi-side "team full" reject for humans. The only "full" string is
  for bots: `RequestSlot(Bot) … team is currently full`.

## 2. Implementation (`native/src/teamsize.c`)

- MinHook on `InitSlots` `0x141A14860` (32-byte signature, unique). When `teamsize` is set to N > 0 and
  `Config.TeamSize < N`, the hook writes N before the original runs. It applies to every team: Fort Hope gets 5+5,
  and the zombie team is unused in PvE. It only raises the value, never lowers it.
- Sets `net.MaxPlayersOverride = N + 2`. It writes the cvar storage directly. The pointer is decoded from the
  `ApproveLogin` instruction at `0x143CC0643`, which is checked by a signature. The +2 is headroom for a stale
  connection during the follow/rejoin retry.
- Client side: once a second, if the replicated hero team has more slots than the local (non-replicated)
  `Config.TeamSize`, the tick raises the local value so that `GetTeamSizeData` UI agrees. Runs on every machine,
  whatever its own `teamsize` (until #24 it only ran with `teamsize` set locally; §7).
- Opt-in: `teamsize=5` in `b4bcoop.ini`, or the agent command `teamsize 5`. It applies from the next map load. The default
  is 0, which leaves the game alone. Solo play is unchanged unless enabled; with it on, solo gets 4 bots.
- Commands: `teamsize [N]` (query/set, 0–8), and `slots`, which dumps the slot layout:
  `Config`, `bSupportsBots`, cvar, `GameSession.MaxPlayers`, then per team each slot's hero row, owning and controlling
  PlayerState (name + controller class), pawn class and location, and reserved flag.
- Shared-file edits: `main.c` gets one init call, and the agent port range goes from 4 to 8 instances
  (`47112..47119`), because a 5th local instance could not bind before. `cmds.c` gets one tick call and one
  dispatch fallback. `cmds.h` gets the declarations.

Expected host log: `teamsize: InitSlots 1 team(s): TeamSize 4 -> 5`, then the engine's
`PlayerSlotManager::InitSlots with 1 team(s) and 5 slots`.

## 3. What will likely break with 5

Ordered by risk. "Evidence" is static unless stated.

1. **Safe-room spawn (medium).** `AHeroGameMode::ChoosePlayerStart_Implementation` (`0x141A05210`) asks the spawn
   manager (GM `+0x3b8`, `0x141C18750`) for a free location from a candidate list (`+0x368`, 12-byte vectors). If none
   passes, it sets `SetWaitingForSpawnLocation` (PS `+0x71a`) and returns null. Most safe rooms probably have 4
   PlayerStarts. The 5th hero may wait for an EQS retry, spawn at the `Falling back to default PlayerStart` path
   (`0x141C1868D`), or overlap another hero. Watch for these log lines.
2. **Party HUD (high likelihood, cosmetic).** `PartyStatusUserWidget::OnSlotsUpdated` (`0x141E2BE40`) loops over
   all of its team's occupied slots except the local one and calls the BP event `SetPlayerAt(Index, Slot, …)`. It then
   hides entries up to the panel's child count. With 5 heroes it asks for index 3 (the 4th teammate). The BP most
   likely has 3 entry widgets, so one teammate is missing from the HUD (and there may be an `Accessed None` warning). This
   is not a crash.
3. **Lineups / post-round / character-select mannequins (likely cosmetic).** `CharacterLineupLayoutManager` has
   `Mannequins`, `PreRoundLockInTargetPoints` and `PostRoundTargetPoints` arrays placed in a lineup level, probably 4 each.
   Access is bounds-checked (`0x141CBD60C`), so the 5th hero is simply not shown.
4. **Join cap (handled).** "Server full." if `GameSession.MaxPlayers` < 5. It is covered by the cvar. `slots` prints
   the real `GameSession.MaxPlayers`.
5. **Same-account single-machine tests (test artefact).** All local instances share `offline.<steamid64>`. Campaign-run
   slot reservation (`Reserved: offline.765…` on slot 0) and the card-ownership override both key on it. A client
   could claim the host's reserved slot. This did not happen with 2 instances, but watch `claimed reserved slot`.
6. **Bots (low).** With `bSupportsBots`, every empty slot gets a bot, so solo with `teamsize=5` has 4 bots. Bot heroes
   are claimed by name through `CharacterSelect`, which is mapped from the `HeroDefinitions` table (12 heroes), so a 5th
   unique hero should exist. Bot formation/tether logic is unverified with 4 teammates.
7. **Card/deck per-slot data (low).** GCM `PlayerActiveGameplayCardDataArray`, `CampaignRunData.CampaignPlayerSlotData`
   (JSON list keyed by `slotIndex`) and PSM seamless-travel data are TArrays keyed by `SlotIndex` or player.
   Nothing is sized 4. The card-draft override in `cards.c` covers remote humans in any slot.
8. **Campaign-run save (low).** A run saved with 5 slots has a `slotId: 4` entry. Resuming it with `teamsize=0`
   should drop that entry (the travel/restore lookups bounds-check). This is unverified.
9. **Changing `teamsize` mid-run (low).** Seamless travel restores by old slot index into the new `TeamSlots` with
   bounds checks. A player whose old index no longer exists logs `did NOT find a previous slot` and gets a fresh slot.
10. **Director/balance.** There are no per-player-count native tables (no `[4]` arrays indexed by hero count). The
    reflected `PassageSpawnerMod[4]` arrays are door types. Difficulty is out of scope.
11. **Not verified at all:** voice routing, ready-status UI with 5 entries, the 5-card-draw screen, vote-kick
    thresholds, and `DungeonTransitionArea` required counts (these look dynamic).

## 4. Phase-2 test plan (`launch/multi.sh N`)

Setup: build and install once. Put `teamsize=5` in `b4bcoop.ini` next to the DLL (shared by all instances; only the
host's value matters for slots). Agents answer on `47112 + i` (`B4B_AGENT=i tools/b4b.py …`). After each step, grep the
host log for `teamsize:`, `InitSlots with`, `Server full`, `team is currently full`, `SetWaitingForSpawnLocation`,
`Falling back to default PlayerStart`, `claimed reserved slot`, `Accessed None`, and `don't own it`.

1. **Solo smoke (1 instance).** `teamsize` returns 5. In Fort Hope, `slots` shows `TeamSize=5` and 2 teams × 5. Start a
   mission solo: 5 slots, 4 bots, 5 pawns in the safe room, no crash. Take a screenshot of the HUD.
2. **Regression (2 instances).** The client follows into the mission and takes over a bot (existing path). On the
   client, `slots` shows 5 replicated slots and `Config TeamSize=5` after the sync tick.
3. **Main test (5 instances).** Start `multi.sh 5` and wait until all 4 clients are in the camp. Host `players` = 5 and
   `status` = `client_conns=4`. Start the mission (unattended command if available). Within about 30 s, host `slots`
   shows all 5 slots with human owners and no bot. Each client's `slots` agrees. Check that every pawn spawned and
   where (item 1 in section 3), and take a HUD screenshot per window (item 2). Check that each remote gets 15
   `applying loadout card` lines and no draft.
4. **Play through.** Leave the safe room and do one chapter transition (seamless). `slots` should still show 5 with
   the same heroes and owners. Finish or fail the map; check the post-round lineup and the return to camp.
5. **Hot-join / leave.** Start with 4 humans + 1 bot, then have the 5th join mid-mission: TakeOverBot on slot 4. Kill a
   client process: ReplaceWithBot. Rejoin: it takes the slot back.
6. **Baseline (`teamsize=0`, 5 instances).** Record what the 5th client gets in vanilla (spectator / rejected /
   nothing). This confirms the hook is what makes the difference.
7. **Save.** Check that the host `PlayerProfileSettings.json` `campaignRuns[*].data.campaignPlayerSlotData` has 5
   entries (read-only). Resume that run with `teamsize=0`: expect no crash.

If step 3 fails on spawn, the next thing to try is hooking `0x141C18750` / the ChoosePlayerStart fallback and cloning
a PlayerStart with an offset for slot ≥ 4. If the HUD misses a teammate, that is BP-side, so live with it or look at `SetPlayerAt`.

## 5. Live results (2026-09-24, 5 local instances, one Steam account)

**Verdict: with `teamsize=5` five humans play a campaign run.** The Fort Hope host crash is gone, all 5 follow into
the mission, each gets a hero, 15 loadout cards, a HUD entry, and the party survives two chapter transitions. Without
`teamsize` the host still crashes (reproduced; cause below). Nothing needed fixing beyond the existing hook.

| Step | Result |
|---|---|
| Solo, `teamsize=5` | `slots`: `Config TeamSize=5`, Fort Hope 2×5 (`InitSlots 2 team(s): TeamSize 4 -> 5`). Mission: 5 slots, host + **4 bots**, all spawned, HUD shows 4 bot teammates + self (`solo-4-bots.jpg`). |
| `multi.sh 5`, `teamsize=5`, Fort Hope | All 4 clients in (`client_conns=4`), 5 heroes at 5 distinct spots, **no host crash**. `GameSession.MaxPlayers` is 16, so "Server full" was never a factor. |
| `mission Easy` | All 5 in `Evansburgh_B` about 40 s after the command. Host `slots`: 5 human owners, no bot, 5 distinct pawn positions. Clients' `slots` agree (5 replicated slots, same heroes). |
| Spawns | No `SetWaitingForSpawnLocation … true` and no `Falling back to default PlayerStart` in any map (B, C, D). The safe rooms fit 5; risk item 1 did not happen. The spawn-picker hook (`0x141C18750`) is not needed. |
| HUD | Party panel in every window: 4 teammates + self (`hud-5-windows.jpg`). Risk item 2 did not happen. Character select also lists all 4 others (`character-select-host.jpg`). |
| Loadout cards | Host: `GrantLoadoutCardsForSlot` ×5, `granting 15 cards` ×5, no draft. (Same account everywhere, so `cards.c` never had to override ownership.) |
| Chapter transition | `ready`, `endmission 1`: post-round screen, then seamless travel to `Evansburgh_C` and, after a second round, `Evansburgh_D`. `HandleSeamlessTravelPlayer found previous slot` for all 5. Same heroes in the same slots each time (`reserved=1`). |
| Post-round lineup | **Shows 4 of 5 heroes** (`postround-lineup.jpg`): the lineup level has 4 target points (risk item 3). Cosmetic, no error. Fixed later (§6, #8). |
| Leave | SIGKILL of one client in the pre-round: its slot sits unowned until the round starts, then a bot takes the hero over (`BotController`). Host fine. |
| Baseline, no `teamsize`, `multi.sh 5` | **Host crashes in Fort Hope** as the 5th hero spawns (reproduced). |

**Baseline crash cause.** The 5th player's `RequestSlot` finds no free slot (no "claimed" line follows it). The host
still spawns a hero for it, and the possession handler at `0x1419FE7D0` reads the player's slot
(weak ptr at `+0x554` of the object RequestSlot logged, via `0x1426F7D20`) and does `cmp byte [slot+0x2ba], 1` on null: `EXCEPTION_ACCESS_VIOLATION`
reading `0x2ba` at `0x1419FE875` (minidump; caller chain `0x141C1B1A0` ← `0x141C19A34` ← `0x141C173D0`, the spawn
manager). Last log lines:

```
LogGameMode Verbose: RestartPlayerAtPlayerStart <Redacted>
LogGobiPlayerController Verbose: SetViewTarget for <Redacted> to Hero_BP_C_2147479040 on Server
LogCrashHandler Log: CrashHandler ReportCrash
```

So any host crashes when more humans join than it has hero slots, since `GameSession.MaxPlayers` (16) lets them in.
With `teamsize=5`, a 6th joiner would crash the host the same way. `net.MaxPlayersOverride = N+2` caps joins at 7,
which is lower than 16 but still above N.

**Not tested:** hot-join into a running mission (5th takes over a bot), rejoin after a leave, a real multi-machine
session, a failed mission and the return to camp, resuming a 5-slot save with `teamsize=0`, voice, vote-kick.

**Next steps**
1. ~~Crash guard~~ Done (#7): `native/src/slotguard.c` refuses the login with "Server full." and never spawns a
   slotless hero. The "possession handler" above is actually `AHeroGameMode::RestartPlayerAtPlayerStart`. See
   `slot-guard.md`.
2. ~~Post-round lineup~~ Done (#8, §6): `native/src/lineup.c` adds a 5th mannequin.
3. Test hot-join into a running mission, and a real two-machine session with `teamsize=5`.

## 6. Lineup with 5 heroes (issue #8, `native/src/lineup.c`)

**How the lineup places heroes.** It never shows the real pawns. The lineup sublevel (`MAP_CharacterPreRound`,
`GobiWorldSettings.CharacterLineupLevel`) is streamed in locally on every machine by the post-round / pre-round /
character-select UI. It holds an `ACharacterLineupLayoutManager` whose level-placed arrays are fixed: `Mannequins`
(4 `CustomizationMannequin_BP`), `PreRoundLockInTargetPoints` and `PostRoundTargetPoints` (both the same 4
`PreRoundLineupSpot_1..4` `ATargetPoint`s), and a `CameraActor` (FOV 30). `SetLayoutType` (`0x141CBD120`; type
1 character select, 2 pre-round lock-in, 3 post-round at `0x141CBD900`) walks the hero team's slots: slot index i →
`Mannequins[i]` dressed as that slot's hero (bots included) → moved to `TargetPoints[i]`; `i >= Mannequins.Num` is
skipped silently. The name plates are a Blueprint widget with 4 fixed columns.

Live dump (`lineup` command, post-round): camera at (9758, 10000, 10134) looking +x; spots 1-4 at
(10151, 9974), (10230, 9893), (10264, 10008), (10119, 10069), z 10000 — a staggered group, not a row.

**Fix.** Hook `SetLayoutType`. Only when the hero team has more slots than there are mannequins (so never with 4):
before the original, spawn a copy of the last mannequin (`BeginDeferredActorSpawnFromClass` / `FinishSpawningActor`,
owner = the manager, so it lives in the lineup sublevel and goes away with it; not replicated) and append it to
`Mannequins` (array grown with `GMalloc->Realloc`). The game then dresses and shows it like the others. After the
original, the heroes without a target point are moved (`K2_SetActorTransform`) to the back row: last spot +
(211, -11), i.e. about (10330, 10058), in the gap between the 3rd and 4th hero as seen from the camera. Kill switch
`B4BCOOP_NO_LINEUP=1`. `lineup` dumps managers/mannequins/points/camera; `lineup off <dx> <dy>`, `lineup fov <deg>`,
`lineup apply` were used to tune the placement live.

**Result** (`B4B_INI_EXTRA="teamsize=5" launch/multi.sh 5`, Evansburgh C post-round): all 5 instances show 5
heroes (`five-players/postround-lineup-5.jpg`, instance 5's own view): `lineup: 5 hero slots, 4 mannequins: spawned
…` and `layout 3, placed 1 hero(es) beyond 4 target points` on each. With 2 humans + 3 bots the bot in slot 4 shows
the same way. Placement options tried: continuing the row past spot 4 (hero cut off at the right edge, overlapping
spot 4), just behind spot 3 (hidden behind that hero). The back-row gap is the only spot that is fully in frame
without moving the 4 lit heroes or the camera.

**Limits (cosmetic).** The 5th hero stands in the back and is dimmer (the level's lights aim at the 4 spots) and has
no name plate (the plate widget has 4 columns). Widening the FOV or re-spacing all 5 would misalign the 4 plates,
so it was not done. The character-select (type 1) and pre-round (type 2) layouts get the same extra mannequin, so a
5th player sees their own hero on character select; that path was not looked at separately. 6+ heroes would stack
further back (untested).

## 7. Clients without `teamsize` (issue #24)

Question: does a client show 5 survivors when only the host sets `teamsize=5`? Test (2026-09-25, lane 2, 4090
headless): `B4B_INI_EXTRA1="teamsize=5" launch/multi.sh 5`, Fort Hope, `mission Easy`, `ready`, `endmission 1`,
seamless travel to `Evansburgh_C`; presented-frame screenshots of every instance.

**Verdict: nothing a player sees depended on the client's `teamsize`.** Everything in the offline session counts the
replicated `TeamSlots`, not `Config.TeamSize`:

| What (clients with no `teamsize`) | Result |
|---|---|
| `slots` | 5 replicated slots; `Config TeamSize=4` (before the fix below) |
| HUD party panel, Fort Hope and mission | 4 teammates + self on all 5 instances |
| Post-round lineup (lineup.c) | every client logs `lineup: 5 hero slots, 4 mannequins: spawned …` (character select and post-round) and `layout 3, placed 1 hero(es) beyond 4 target points`; host frame shows the 5th hero in the back row |
| Rewards | `rewards: forwarding AdjustSupplyPoints (63)` ×4, 63 SP on every post-round screen |
| Chapter transition | all 5 in `Evansburgh_C`, same heroes/slots, `reserved=1` |
| World name plates over teammates | shown |

**`GetTeamSizeData` callers** (the only reader of `Config.TeamSize` besides InitSlots; the 6 calls of `0x141D61AF0`):
`UFTUETutorialScreen::OnCategoryPlayersChanged` (`0x141D08230`), `UMatchmakingScreen` (`0x141D16F40`), a quickplay
check that shows `TEXT_Matchmaking_Error_QuickplayFull` (`0x141D1CD20`), `UPartyScreen::OnTeamSizeChanged`
(`0x141D21F77`), `UMatchmakingPartyPromptUserWidget` (`0x141DF6BA0`), and a party team-size cache that broadcasts a
delegate when it changes (`0x141B2B62D`). All online party / matchmaking UI, unused offline. The client sync now runs
regardless of the local `teamsize` anyway (log `teamsize: hero team has 5 slots, local Config.TeamSize 4 -> 5`,
client `slots` then `Config TeamSize=5`), so the doc statement holds.

**Character-select list (all machines, host included, not #24; fixed in #28):** the player list at the left of the
mission-start character select / loadout screen had 4 rows (hero-team slots 0-3); the player in slot 4 was not
listed, also not on its own screen. It is `MissionLoadoutScreen_WBP.PlayerLoadouts` (`PlayerLoadouts_WBP_C :
UPlayerLoadoutsUserWidget`): a VerticalBox (`+0x468`) with 4 designer-placed `PlayerLoadoutsEntry_WBP_C` rows.
`OnSlotsUpdated` (body `0x141E3D670`, thunk `0x142238DA0`) gives hero slot i (PlayerSlot `+0x2a0`) row i and skips a
slot without a row (the matchmaking party list `MatchmakingPlayerList_WBP`, `0x141DF7900`, works the same way; it is
not shown offline). Fix (lineup.c): before it runs, when the hero team has more slots than rows, rows of the same class
are added (`WidgetBlueprintLibrary::Create`, `PanelWidget::AddChild`, the new slot gets the last row's size, padding
and alignment through the slot's setters); the game fills them like the others. Verified 2026-10-04 (lane 1,
`B4B_INI_EXTRA="teamsize=5" launch/multi.sh 5`, `mission Easy`): host and instance 5 show 5 rows ("Hergmgurk /
PICKING CHARACTER", the local player's row in red), log `lineup: 5 hero slots: player list row 5 added
(PlayerLoadoutsEntry_WBP_C_...)`; `five-players/charselect-list-5.jpg`. The voice overlay (`VoipOverlay_WBP`) already
has 5 entries.

**Reverse** (`B4B_INI_EXTRA2/3="teamsize=5" launch/multi.sh 3`, host without): 4 slots everywhere (2 humans + a
bot on the host's side, clients' `Config TeamSize=4`, `net.MaxPlayersOverride=7` locally, unused on a client), HUD 3
teammates + self, post-round 4 heroes and no lineup spawn, SP forwarded, no errors.

**Not checked:** pings on the 5th hero specifically (ping markers are per-player replicated actors, not sized by
team); a 5th hero's outline through walls (not visible from the safe room shots). One instance hit `LogRendererCore
Fatal: GameThread timed out waiting for RenderThread after 120.00 secs` while loading Fort Hope during the 5-instance
start (headless gamescope on one GPU); relaunched with `launch/instance.sh 3`, it rejoined and took slot 3.

## 8. Teammate lists with 6-8 heroes (`native/src/hud.c`)

Test (2026-10-13, lane 1, 4090 headless): `teamsize=8` on one instance (7 bots) in the Evansburgh_B saferoom and in
the tunnel (`tp 0 volume 2`), at 960x540 and 1920x1080 (`B4B_RES` + the prefix's GameUserSettings.ini, put back
after); `teamsize 6` / `teamsize 5` + `mission Easy` for 6 and 5; `multi.sh 2` (host `teamsize=8`, client without):
leave, rejoin mid-mission, `takeover`; `multi.sh 5` (5 humans + 3 bots): character select, two post-rounds and the
seamless travel to Evansburgh_C and _D. Widget trees with `hudtree <class|path suffix> [depth]` (pixel rects from the
cached geometry, slot layout, render transforms), live tuning with `hudxf` / `hud gap`. Screenshots:
`~/.local/share/b4b-coop/hud8-shots/` (not committed).

**What broke (all machines, host and clients):**

| Place | 5 heroes | 6-8 heroes |
|---|---|---|
| HUD teammate list, bottom left (`GameHUDScreen_WBP.PartyStatus`) | 4th teammate over the top of your own panel | 5-7 teammates run down over your own panel and off the bottom of the screen (`before-8-540.png`) |
| Character select player list (`MissionLoadoutScreen_WBP.PlayerLoadouts`, rows added by lineup.c #28) | fine | rows 7-8 off the bottom; rows of bots past the 4th showed a stale "PREROUND STEP" row with a portrait (`before-charselect-2humans-8slots-540.png`) |
| Esc menu scoreboard (`SystemSettingsScreen_WBP.Scoreboard`) | fine | fine: 8 rows (`esc-8-540.png`) |
| World name plates, VOIP overlay (8 entries) | fine | fine |
| Post-round lineup (lineup.c §6) | 5th in the back row, no plate | 6th-8th stack further back behind the 5th, mostly hidden; still 4 name plates. Not changed |

**HUD layout.** `PartyStatus` (`PartyStatus_WBP_C : UPartyStatusUserWidget`) is a CanvasPanel slot anchored bottom
left: offset (128, -941), size 673x588 design units (the HUD is laid out for 2160p: 0.25 px/unit at 540p, 0.5 at
1080p), so the box is y 305-452 px at 540p and your own panel (`HorizontalBox_0`) starts at 468. Inside:
`VerticalBox_119` = {`PushAllToBottomSpacer` (fill), `PlayerList` (VerticalBox, auto)}; one `PartyStatusEntry_WBP_C`
per teammate (`SetPlayerAt` index = position in `PlayerList`), 524x116 units each but 176 apart: the 60-unit gap is
not in the UMG slots (VerticalBoxSlot padding 0, desired size 116), so positions must come from the layout geometry.
3 entries (528) fit the box bottom-aligned; 7 entries (1232) push the spacer to 0 and hang down from the box's top.

**Fix.** Render transforms only, so the game's indexing, layout and slots are untouched. A first version made the
HUD list two columns; Dan rejected it (it covers too much of the view), so the HUD keeps the game's single column:
with 4+ teammates (hero team > 4) `PlayerList` gets a uniform scale about its top-left corner (pivot 0,0, so the
game's x stays) and a vertical move, so that its bottom stays where the game's own bottom-aligned list ends (the box's
bottom, above your own panel, which is not touched) and its top sits `fit_margin` (48 units, 12 px at 540p) under the
objectives' real bottom: `HUDMissions` (GameHUDScreen_WBP, `VerticalBox_2` at (128, 190), one `HUDMission_WBP` per
mission block, so 1-3 blocks change its height), read every frame. Scale = room / list height, at most 1 (then it is
only moved up), at least `fit_floor` 0.6 (then the top entries overlap the objectives, never your panel). With 2-3
objective blocks 7 teammates (293 px at 540p) fit at scale 1 in the 315-327 px of room, so the column mostly just
moves up. Positions from `Widget.GetCachedGeometry` (FGeometry Size +0, Scale +8, AbsolutePosition +0xC: layout
only, render transforms excluded), applied every frame (no write when unchanged), widgets looked up once a second
while the hero team has more than 4 slots (class check: the Blueprint class or its direct super; only
`/Engine/Transient` instances). Character select keeps two columns: `PlayerLoadouts`' `PlayerList` (top-anchored,
288 units apart) keeps the designer's 4 rows and rows 5-8 go to a second column beside them, because under row 4 sit
the hero portraits (y 420 at 540p; one column would need scale 0.59 for 8 rows above them) and the second column only
shows rows of humans 5-8 (bot rows are Hidden) (5 humans: `charselect-5humans-8slots-host-540.png`). lineup.c's added
rows now start Hidden like the designer's rows, so a bot's row stays hidden and a human's is shown by the game. Hero
team of 4 or less: nothing runs (e2e `--quick` logs have no `hud:` line). No ini switch.

**Crash found on the way (not hud.c).** The first `multi.sh 5` run crashed all 5 instances in the agent about a
second after the GC that follows the post-round (13:55, `UE4Minidump.dmp` in each prefix's `Saved/Crashes`). Mapped
on a rebuild of that exact source (same SizeOfImage 0x957000): the exception (read of -1) is `ue_process_event`'s
`jmp [vtable+0x210]` called from `cmds_hotkey_down` (`IsInputKeyDown` on `ue_local_pc()`, the flashlight /
third-person / bot-heal hotkeys), i.e. the local PlayerController it got was already freed. Not seen before in any
e2e run; not reproduced in two more post-rounds + transitions with 5 instances and 8 slots. Guards added anyway:
`ue_object_usable()` (ue.c: still at its GUObjectArray index, FUObjectItem.Flags without Unreachable/PendingKill
(1<<28, 1<<29), no RF_BeginDestroyed/RF_FinishDestroyed) on that PlayerController (falls back to GetAsyncKeyState)
and on every widget hud.c touches. Watch for it.

**Results** (single column, `fit-*.png`). 8 heroes: the 7 teammates fit at scale 1 under 2-3 objective lines in the
saferoom and in the tunnel, 540p and 1080p (`fit-8-540-saferoom.png`, `fit-8heroes-540-VeryHard-2missions-1..4.png`
(4 runs: "Blood Samples" with 2 lines = room 315 px, others 327 px), `fit-8heroes-1080-Easy-saferoom.png`,
`fit-8heroes-1080-tunnel.png`); 6 and 5 heroes (`fit-6heroes-*`, `fit-5heroes-*`, both resolutions). Shrinking was
never needed with real objectives in Evansburgh_B (4 VeryHard + Easy runs never showed a 3rd block), so it was
checked by raising `hud margin` (taller objectives simulated): 600 units = scale 0.65 / 0.63 at 540p / 1080p, 1100 =
the 0.6 floor with the top entries over the objectives (`fit-8heroes-*-simulated-margin*.png`); bottom and x stay.
A client without `teamsize` gets the same (`fit-8heroes-540-client.png`); each new mission rebuilds the HUD
and it is fitted again; `e2e.py --quick` 14/14. Earlier (two-column version, same mechanics): leave, mid-mission rejoin +
take-over and two chapter transitions with 5 instances.
