# Clients keep what they earn (issue #4)

Build 14216215. Sources: host log `b4bcoop-552.log` (two-machine session 2026-09-23, client `offline.76561198975681908`
joined at 22:05, mission Evansburgh B ended 22:12:34), same-machine client log `b4bcoop-776.log`, the Windows
client's `docs/logs/2026-09-23-windows-client/PlayerProfileSettings.json`, the host's profile (read only), and static
analysis of `Back4Blood.exe`. Sections 1-4 were written from logs and static analysis; the fix was then tested live
(section 6).

## 1. Summary

The suspicion is half right. The host computes every player's rewards. Only some of them reach a remote client:

| Earned by a remote client | What happens today | Why |
|---|---|---|
| Supply points (mission success/failure) | **Lost** | Host executes the command on the client's profile component. That component is not local, so the command is dropped. |
| Skull totem points | **Lost** | Same path. |
| Duffel-bag rewards (unlocks, consumables) | **Lost** | Same path (`RewardDuffelBags`). |
| Burn cards | **Cannot be played at all** (static; see §6) | The host checks the remote player's quantity against a profile it does not have. The charge on leaving the saferoom would go through the same dropped path. |
| Stats (kills, missions completed, …) | **Kept** (native) | Host sends `ClientApplyStatDeltas`; the client reconciles into its own profile on EndPlay/OnLeavingMap. |
| Starting locations (map unlocks) | **Kept** (native, static evidence only) | Client runs the unlock itself from `GobiPlayerState::OnRep_UnlockedNewMap`. |
| Achievement rewards (`CompleteAchievement`) | Unknown, probably lost | Server-side tracker; no client RPC exists. |
| Things the client does in menus (decks, cosmetics, caravan purchases, recent character) | **Kept** (native) | Executed on the client's own, local, profile component. (Supply lines refreshing after a purchase: `supply-lines.md`, #48.) |
| Campaign run | **Host only** | The run belongs to `RunOwner` (the host). The client records nothing and cannot resume it alone. |

**Nothing is written to the host's profile on a client's behalf.** Offline writes go only to the `LocalPlayer` that
owns the controller, and a remote controller has none.

The fix (implemented, `native/src/rewards.c`): the game still ships client RPCs for exactly the lost commands
(`ClientExecute{AdjustSupplyPoints,UnlockProduct,AdjustConsumableQuantity,AdjustSkullTotemPoints}Command`). Their
client side runs the command through the normal offline path. Nothing in this build calls them any more. The host
agent now calls them.

## 2. Reward flow

### Command execution (`UGobiPlayerProfileComponent`, "PPC")
All profile changes are `FPlayerProfileCommand` subclasses (vtable: `[1]` GetType → `EPlayerProfileCommandType`, `[4]`
apply to `FPlayerProfileData`). They all go through:

`PPC::ExecuteCommand(PPC*, const FPlayerProfileCommand*, bool bPersist)` @ `0x141BC4970` (28 callers):
```
pc = PPC->OwnerPrivate (+0xD8), must be GobiPlayerControllerBase; GameInstance must have GobiPlayerProfileManager (+0xA30)
local = pc->IsLocalController()                      // vtable +0x6B8
if (local) ApplyCommandToOfflineData(PPC, cmd)       // 0x141BC4F70, logs "ApplyCommandToOfflineData:%s"
if (OnlineModeSubsystem(+0xA0) == 1 /*Online*/)      // skipped offline
    local ? 0x141BC4C10 (the "…bPersist:%d" Hydra path) : 0x140B92AF0 (`ret` — compiled out)
broadcast OnCommandExecuted (PPC +0x130, global list 0x146491760)
```
`ApplyCommandToOfflineData` @ `0x141BC4F70` gets its target from `0x141BC6930`: `pc->GetLocalPlayer()` must be a
`GobiLocalPlayer`, then `GobiLocalPlayer.PlayerProfileSettings` (+0x2F8) → `OfflineData` (+0xE0). It calls `cmd->Apply`
and then `SaveGame` (vtable +0x288). So **offline data is per LocalPlayer, gated on `IsLocalController`**. `EOnlineMode`
does not gate the offline apply. It only selects the (dead) online branch.

### Who issues which command
| Command (type) | Issuer | Runs on |
|---|---|---|
| AdjustSupplyPoints (5) | `0x141ECE2A0` "adjusting SP by %i" (also adds to replicated `GobiPlayerState.PostRoundBonusSP` +0x7A0) ← `RewardSurvivorsForSuccess` `0x141A0A010`, `…ForFailure` `0x141A0C860`, PvP, challenges `0x141B99C20` | server |
| AdjustSkullTotemPoints (20) | `RewardSurvivorsForSuccess` ("adjusting STP by %i") | server |
| UnlockProduct (6) / AdjustConsumableQuantity (19) | `0x141BD7610` ← `RewardDuffelBags` `0x141A0ADC0`; GCM burn-card charge `0x141768A30` ("Charging hydra ID %s for burn card %s") | server |
| AdjustStatValue (7) | `PlayerStatsComponent` reconcile `0x141C24D80` → `0x141BC5B90` | both (see below) |
| UnlockStartingLocation (8) | `0x141BCEFE0` "Unlocking up to map…" ← `OnMissionEnd` `0x141A08140` (server) **and** `OnRep_UnlockedNewMap` `0x142136240` (client) | both |
| CompleteAchievement (16) | `AchievementTrackerComponent` `0x141C5EF30` | server (client: unverified) |
| Decks, skins, customization, caravans, purchases, badge, spray, recent character | UI code | the client itself |

### What a remote client gets
- **Server-side commands on a remote PPC are dropped.** The non-local branch of `ExecuteCommand` is `ret`, and
  `ApplyCommandToOfflineData` is never reached (not local, and there is no LocalPlayer). Host log at mission end:
  ```
  [offline.76561198063588550]: adjusting SP by 73
  [offline.76561198063588550]: ApplyCommandToOfflineData:AdjustSupplyPoints      <- host: applied
  [offline.76561198975681908]: adjusting SP by 69                                <- client: nothing follows
  [offline.76561198975681908]: unlocking evans_b on Easy                         <- nothing follows
  [offline.76561198975681908]: adjusting stat RiddenKilled:base by 1 …           <- nothing follows (host copy)
  ```
- **Nothing is sent to the client for these commands.** The PPC has `ClientExecute*Command` RPCs (client-side
  `_Implementation`s `0x141BC5230/5330/5430/5570` log `"[CLIENT RPC] …"` and call `ExecuteCommand(…, true)`; `_Validate`s
  return true). But the `FName` globals for these RPCs (`0x14690CD70..CD90`) are referenced only by their static
  initialisers, so no code sends them. The same is true of the `ServerExecute*Command` RPCs.
- **Stats do arrive.** At mission end, `0x141C24370` logs "forcing network flush" for a remote owner
  (`Role==Authority && !IsLocalController`) and sends `PlayerStatsComponent::ClientApplyStatDeltas` (`0x141C24530`).
  The client adds the deltas to its own component. On EndPlay/OnLeavingMap it reconciles them into its own profile.
  Seen in the same-machine client log `b4bcoop-776.log` 21:22:48 (`adjusting stat …` + `ApplyCommandToOfflineData:AdjustStatValue`
  on the client, which left mid-mission). This also works when the client leaves early.
- **Starting locations**: the host sets the replicated `bUnlockedNewMap` for the client. The client's
  `OnRep_UnlockedNewMap` runs the same unlock against its own profile. This is static evidence only.
- The Windows client's profile after the session has `supplyPoints.acquired 4021`, `campaignRuns {}`, and
  `evans_b/evans_c` unlocked on easy. Without its log and a before-copy, it cannot show whether the 69 SP arrived.
  Static analysis says it did not.

### Host profile safety
Every `ApplyCommandToOfflineData` line in the host log carries the host's id. A remote PC has no `GobiLocalPlayer`,
so `0x141BC6930` returns null even if something bypassed the `IsLocalController` gate. On the host, the remote PPC's
`HydraPublicId` (+0x1A8) **is** set (`[offline.76561198975681908]: profile loaded` at join; the log prefix is read from
+0x1A8). That contradicts the static guess in `card-draft.md` §1 and may bear on why card ownership passed natively in
the two-machine session.

## 3. Campaign runs
- The mission URL carries `RunOwner=1`. `CampaignRunComponent.CampaignRunOwners` holds the host's id. On the host:
  `Initialized campaign run component for ResumedRun owner:offline.<host>` and at mission end
  `updating campaign run 1790208526 [OFFLINE]` (`0x141BC1780`), host id only.
- Client: `campaign run applied: 0`, `clearing campaign run data`. Its profile has `campaignRuns: {}`.
- So the run lives only in the host's profile. A client can continue it only by joining the host again. It cannot
  resume solo. This matches retail, where a campaign belonged to the party leader. **No change proposed.** Rewards
  earned during the run (SP, stats, unlocks) are what the client keeps.

## 4. Fix (implemented: `native/src/rewards.c`)
Hook `PPC::ExecuteCommand` `0x141BC4970` (32-byte signature check). The detour calls the original first, then, if the
component's owner is a `PlayerController` whose `Player` is not a `LocalPlayer` (a remote client on the listen host)
and the command type is one of:

| type | RPC sent to that client |
|---|---|
| 5 AdjustSupplyPoints | `ClientExecuteAdjustSupplyPointsCommand` |
| 6 UnlockProduct | `ClientExecuteUnlockProductCommand` |
| 19 AdjustConsumableQuantity | `ClientExecuteAdjustConsumableQuantityCommand` |
| 20 AdjustSkullTotemPoints | `ClientExecuteAdjustSkullTotemPointsCommand` |

it copies the command into the UFunction's parms (`Command` property, offset/size from reflection) and calls
`ProcessEvent` on the PPC. UE routes the NetClient RPC to the owning connection. On the client, the game's own
`_Implementation` runs `ExecuteCommand` on the local controller, so the native offline path writes the client's own
`PlayerProfileSettings.json`. The host's profile is untouched.

- Not forwarded: AdjustStatValue and UnlockStartingLocation (both already reach the client natively; forwarding
  would double-count). Other command types on a remote PPC are logged (`rewards: not forwarded: command type N`) so a
  live test shows what else the host generates (expect 16 CompleteAchievement).
- Idempotence: UnlockProduct is a set insert. SP, STP and consumables are deltas, so they must not also be applied
  natively on the client. Static analysis finds no client-side issuer for them, except challenge rewards
  (`0x141B99C20`, online-only feature). A live test must confirm there is no double count.
- Kill switch: env `B4BCOOP_NO_REWARDS=1`.
- Shared-file edits: `main.c` (`rewards_init()` after `cards_init()`), `cmds.h` (prototype).

Caveat in the client `_Implementation`s: they skip the command when the first local PC is a
`SocialSpacePlayerController` (Fort Hope). Mission rewards arrive in mission maps, so this is fine. A reward issued
in camp would still be lost.

## 5. Live test plan (needs 2 instances with **separate** profiles)
Same-machine tests with one Steam account share `offline.<steamid>` and one `PlayerProfileSettings.json`. Each
instance saves its in-memory copy over the other's, so profile diffs are meaningless there. Use a second account or
redirected save dirs. Before each run, copy both profiles aside and diff them after.

1. Load: host log `rewards: ExecuteCommand hooked`.
2. Mission success with the client alive: host `Rewarding <client> with N SupplyPoints` →
   `rewards: forwarding AdjustSupplyPoints (N) to remote player offline.<client>`. Client log
   `[CLIENT RPC] adjusting SP by N` + `ApplyCommandToOfflineData:AdjustSupplyPoints`. Client profile
   `supplyPoints.acquired` += N exactly once. Host profile changes only by the host's own reward.
3. Mission failure: same, via `RewardSurvivorsForFailure`.
4. Duffel bag picked up by the client: forwarded UnlockProduct/AdjustConsumableQuantity; new entry in client `unlocks`
   or `consumables`.
5. Client equips a burn card: on leaving the saferoom, host `Charging hydra ID … for burn card …` for the client →
   forwarded `AdjustConsumableQuantity (-1)`; client `consumables[...].spent` += 1.
6. Stats: client log `starting profile reconcile` + `ApplyCommandToOfflineData:AdjustStatValue` under the client's id;
   `missionsCompleted_Unsecured.<map>::<diff>` += 1 in the client profile. The host profile gets no client stats.
7. New map unlock for the client: client log `Unlocking up to map …` + `unlocking X on Y` +
   `ApplyCommandToOfflineData:UnlockStartingLocation`.
8. Client leaves mid-mission: stats applied on its EndPlay, no SP (expected). No errors on the host from the RPC to a
   closing connection.
9. Check which `rewards: not forwarded: command type N` lines appear, especially 16 (achievements). Decide whether
   CompleteAchievement needs forwarding. It has no RPC; it could be split into UnlockProduct + AdjustSupplyPoints,
   but only if the host would not re-award it every mission.
10. Post-round screen on the client shows the same N as its profile delta.

## 6. Live test results (2026-09-24)

Setup: `launch/multi.sh 2` (instance 1 hosts, instance 2 joins; each has its own prefix, so each has its own
`PlayerProfileSettings`). The two profiles were cloned from the same real profile, so they start almost identical,
and the host sees both players as `offline.76561198063588550`. Each run: `mission Easy` (new Evansburgh run), `ready`,
`endmission 1|0` (new agent commands in `testing.c`, below). The profile JSON was diffed after the game's deferred
save (about 30 s after the reward, see below).

| Run | Host log | Client log | Host SP | Client SP |
|---|---|---|---|---|
| Fix, success (Evansburgh B) | `Rewarding … 73` ×2, `rewards: forwarding AdjustSupplyPoints (73) to remote player …` | `[CLIENT RPC] adjusting SP by 73` + `ApplyCommandToOfflineData:AdjustSupplyPoints` | +73 | **+73** |
| Fix, failure (Evansburgh C) | `adjusting SP by 8` ×2, forwarded once | `[CLIENT RPC] adjusting SP by 8` + Apply | +8 | **+8** |
| Baseline `B4BCOOP_NO_REWARDS=1`, success | `Rewarding … 73` ×2, 2nd has no Apply | nothing | +73 | **+0** |
| Fix, success, 2 more runs | same as the first | same | +73, +73 | **+73, +73** |
| Fix, client disconnects mid-mission, host ends it | `Rewarding … 60` once (host only), no forward, no errors | stats applied on leave, no SP | +60 | +0 |

Totals across all runs: host `supplyPoints.acquired` +360 (73+8+73+73+73+60), client +227 (73+8+0+73+73). Every
forwarded amount landed once. The kill-switch baseline shows the fix is what makes the difference. The client's
post-round screen showed 73, the same as its profile delta. It shows the replicated `PostRoundBonusSP`, so it shows 73
even without the fix.

Also verified:
- **Stats** (native, unchanged): client `starting profile reconcile` on leaving the map → own
  `ApplyCommandToOfflineData:AdjustStatValue`; `missionsCompleted_Unsecured` +1 in each profile. The host ran a second
  reconcile for the remote player's component with no Apply. The host profile got no client stats.
- **Map unlock** (native): the client ran `Unlocking up to map 'Evansburgh_C'` itself (OnRep). The map was already
  unlocked in the cloned profile, so this was not a new write.
- **No other command types**: no `rewards: not forwarded` line in any run. Easy issues no skull totem points (not even
  to the host), and `[POSTROUND] … Achievements Reward Length 0`. So CompleteAchievement (16) never came up.
- **Same Steam id**: not a problem. The hook keys on the controller's `Player` (LocalPlayer vs NetConnection), not
  on the id. The id only shows in logs. The host's copy of the remote PPC's `HydraPublicId` was sometimes empty
  (`[]: adjusting SP by 73` in the baseline run), so the forward log can print `?` for the id. That is cosmetic.
- **Deferred save**: `ApplyCommandToOfflineData` updates memory at once, but the `.sav`/`.json` are written up to
  about 30 s later. Killing an instance (SIGKILL) inside that window loses the change. Wait before `multi-stop.sh`.

Not covered live in this run (both covered later, §6b):
- **Skull totem points** (type 20): no difficulty/run in this test awarded any.
- **Duffel bags** (UnlockProduct 6 / AdjustConsumableQuantity 19 via `RewardDuffelBags`): the director rolled
  `DuffelBag_None`, and `RewardDuffelBags found no collected duffel bags`. It walks a list of picked-up `DuffelBagItem`s,
  so testing it needs a bag in the map and a client picking it up. The RPC path is the same as SP, only the RPC name
  differs.
- **Burn cards**: remote players cannot play them, so there is nothing to charge. `ServerPlayBurnCard` →
  `GameplayCardManager` `0x141776EA0` needs the card's product quantity > 0 from `0x141BC2A00`
  (`PPC::GetConsumableQuantity`). Offline, that reads `0x141BC6930` (the LocalPlayer's profile), which is null for a
  remote PPC, so the play is rejected. This is the same class of bug as the card draft (`cards.c`). It needs its own
  hook (let remote players' burn-card quantity pass, or ask the client). The same unattended call on the host's own
  player also logged nothing, so the call itself is not proven. The handle must be a card row (`BurnCards_DT`
  `Burn_TeamLife`, see `burncard list`), not a product row.
  *Correction (issue #6, `burn-cards.md`):* `played burn card` is Log level and is captured. "Nothing logged" means the
  play was rejected, most likely because of the tool's guessed card table. The fix and the reworked `burncard` command
  are in `native/src/burncards.c` and `docs/investigations/burn-cards.md`.

Agent commands added for unattended runs (`testing.c`):
- `ready` / `ready vote`: host sets every player ready (`GobiPlayerState::ServerRequestPlayerReady(true)` /
  `ServerSetReadyForPostRoundVote`), so the match leaves `WaitingForReadyPlayers` without a click.
- `endmission [1|0]`: `MissionGameMode::OnMissionEnd(bSuccess, "b4bcoop")`, the normal success/failure path
  (rewards, stats sync, post-round). It only works while `MatchState == InProgress`, so run `ready` first.
- `burncard list | <card row> [card table]`: `GobiPlayerController::ServerPlayBurnCard` from this instance.
- `callp <Class> <Func> [args…]`: call with bool/int/byte/enum/float/string params (first live instance).

Recommendation: merge. SP forwarding works for success and failure, exactly once, and it is off with the kill
switch. The unverified paths (STP, duffel bags) use the same code with a different RPC name. Burn cards need a
separate fix (a new issue).

## 6b. Skull totem points and duffel bags, live (2026-09-24)

**Result: both forward exactly once to the client's own profile; the host's profile gets only the host's own
rewards.** Two instances (`launch/multi.sh 2`, `teamsize=5`, so 2 humans + 3 bots), Evansburgh B → C → D on Easy.

Why earlier runs never saw them (static, confirmed live):
- **Skull totem points do not depend on difficulty.** `RewardSurvivorsForSuccess` gets the amount from
  `AMissionGameMode::CountSkullTotemPoints` (`0x141A08FB0`, MissionGameMode vtable `+0xA30`): the sum of `PointsAwarded`
  of the skull-totem items (`TierableItemComponent.ItemType == SkullTotem`) that human pawns carry in the Carried slot
  (`EquipmentSlots[6]`) at mission end, and **every** eligible human gets that whole total. Totems come from the
  TU07 dungeon dynamic cards (`Dungeon_*`, objective "Search for Skull Totems"); nobody carried one out in our
  unattended runs, so the count was 0. `GiveSkullTotemPoints` / `GiveSupplyPoints` (Exec, `0x141FAEFD0`) are stubs.
- **Duffel bags are rewarded per `ADuffelBagItem` actor alive at a successful mission end** (the list is
  `GobiCollectionsSubsystem +0x698`, filled by the item's BeginPlay). The director rolls a `DuffelBag_*` dynamic
  card (Evansburgh C mostly rolls `DuffelBag_AnySingle`, B mostly `DuffelBag_None`), the loot spawner places a
  `Duffel_Utility_Pickup_BP`, and the item actor only exists once someone picks it up. Every non-bot hero then gets
  one reward per bag, whoever carries it. Per player the issuer `0x141BD7610(PlayerState, product, delta)` runs
  `UnlockProduct` or, for a consumable, `AdjustConsumableQuantity(delta)`. `DuffelBag.ChanceToPreserve` is a dead cvar.

Test commands added to `testing.c` (host):
- `stp <N>|off`: hooks `CountSkullTotemPoints` to return N, so `endmission 1` runs the real STP award.
- `items`: live `ItemPickup`s and their item rows; `giveitem <slot> <pickup#>`: the pickup's row into that hero's
  inventory (`InventoryComponent::ServerAddItemsOfHandle`, the server side of picking it up).
- `duffelreward <slot> <Products_DT row guid> [delta]`: the per-player duffel-bag issuer directly.

| Step | Host log | Client log | Profile diff (after save) |
|---|---|---|---|
| B: `stp 5`, `ready`, `endmission 1` | `CountSkullTotemPoints 0 -> 5`, `SkullTotem Points per safe survivor: 5`, `adjusting STP by 5` ×2 (host + Apply, client + `rewards: forwarding AdjustSkullTotemPoints (5)`) | `[CLIENT RPC] adjusting STP by 5` + `ApplyCommandToOfflineData:AdjustSkullTotemPoints` | host STP +5, client STP +5 |
| C: `items` → `Duffel_Utility_Pickup_BP`, `giveitem 1 <#>` (client's hero), `endmission 1` | `Duffel_Utility_Item_BP_C… BeginPlay successful`; `GiveDelayedDuffelBagRewards … Num Duffel Bags 1`; host: `Giving Reward WeaponSkin_Sni02_Hoffman` + Apply; client: `Giving Card Reward Card_Improvised_Insight` + `rewards: forwarding UnlockProduct` | `[CLIENT RPC] unlocking product Card_Improvised_Insight` + Apply | host: new unlock `WeaponSkin_Sni02_Hoffman`; client: none (it already owned that card, see below) |
| C post-round: `duffelreward 1 <Burn_TeamCurrency_250> 1` | `adjusting consumable Burn_TeamCurrency_250 by 1`, forwarded (1) | `[CLIENT RPC] adjusting consumable … by 1` + Apply | client `Burn_TeamCurrency_250.acquired` 114 → 115; host unchanged |
| D post-round: `duffelreward 1 <WeaponSkin_Sni02_Hoffman> 0` | forwarded `UnlockProduct` | `[CLIENT RPC] unlocking product WeaponSkin_Sni02_Hoffman` + Apply | client: new unlock `WeaponSkin_Sni02_Hoffman` |

Whole-session totals (SP B 77, C 66, D 67): host SP +210, STP +5, unlocks +1 (its own skin); client SP +210, STP +5,
`Burn_TeamCurrency_250.acquired` +1, unlocks +1. Nothing else changed in either profile, nothing was applied twice,
and no reward meant for the client landed in the host's profile.

Finding (not fixed): **a remote player's duffel reward ignores what they own.** The issuer's "already owned" filter
reads the host's copy of the player's profile, which does not exist for a remote player (`0x141BC6930` → null), so a
client can be given a product it already has. That happened here (`Card_Improvised_Insight`): the forwarded
`UnlockProduct` is then a no-op and the bag's reward is effectively lost for that client. Fixing it would need the
client's ownership on the host (the same missing channel as for burn cards); left as a known limitation.

## 7. Addresses
| What | VA |
|---|---|
| `PPC::ExecuteCommand` (hook) | `0x141BC4970` |
| `ApplyCommandToOfflineData` / `…bPersist` (online) | `0x141BC4F70` / `0x141BC4C10` |
| offline target (`GobiLocalPlayer.PlayerProfileSettings`) | `0x141BC6930` |
| AdjustSP helper | `0x141ECE2A0` |
| `RewardSurvivorsForSuccess` / `…ForFailure` / `RewardDuffelBags` | `0x141A0A010` / `0x141A0C860` / `0x141A0ADC0` |
| Client RPC impls (SP, STP, consumable, unlock) | `0x141BC5230`, `0x141BC5330`, `0x141BC5430`, `0x141BC5570` |
| PPC vtable (impl +0x450, validate +0x448) | `0x1454E66A8` |
| Stats: network flush / `ClientApplyStatDeltas` send / reconcile | `0x141C24370` / `0x141C24530` / `0x141C24D80` |
| Starting-location unlock / `OnRep_UnlockedNewMap` | `0x141BCEFE0` / `0x142136240` |
| `ServerPlayBurnCard_Implementation` / GCM play burn card / PPC consumable quantity | `0x141B9CA60` / `0x141776EA0` / `0x141BC2A00` |
| `MissionGameMode::OnMissionEnd` (exec thunk; virtual +0xA20) | `0x1421E2E10` |
| `CountSkullTotemPoints` (hooked by `stp`) / duffel issuer (PS, product, delta) | `0x141A08FB0` / `0x141BD7610` |
| `ServerAddItemsOfHandle` exec / collected duffel bags (`GobiCollectionsSubsystem`) | `0x142186AE0` / subsystem `+0x698` |
| Offsets | PPC owner +0xD8, PPC `HydraPublicId` +0x1A8, `PostRoundBonusSP` +0x7A0, AdjustSP `Delta` +0x8 |

## 8. Client-side validation (`native/src/rewardguard.c`, 2026-09-24)

The forwarding path lets the host write into a client's offline save, so a malicious or buggy host could send
anything through the four `ClientExecute*Command` RPCs (plus `ClientExecuteSetSecureLeaderboardMetadataCommand`,
which b4bcoop never sends). On a **network client** the agent hooks the five RPC handlers (`_Implementation`s, PPC
vtable `+0x410..+0x450`, 32-byte signatures) and applies a command only if it passes:

| Command | Accepted | Why this bound |
|---|---|---|
| AdjustSupplyPoints | delta 1..1000, ≤ 2000 per map | SP = CoreSp(map, difficulty) × (1 + bonuses). `MissionDifficulties` (DifficultyRow, `rewardguard difficulty`): CoreSp max 65 Easy / 114 Normal / 162 Hard / **195 VeryHard**; bonuses objective 0.5, glyph 0.15 each, group 0.1 per survivor (log: `Group = 0.300000` with 3), consecutive ≤ 0.3, party 0.05, quick play 0.25; failure pays 0.2×. Every bonus maxed stays under ×4 → < 800 per reward. |
| AdjustSkullTotemPoints | same limits | 5 points per carried totem, each survivor gets the team total (§6b). |
| AdjustConsumableQuantity −1 | only a burn card this client played on this map, once per play | the client records its own `ServerPlayBurnCard` (hook on `AActor::CallRemoteFunction` `0x143A013B0`, UObject vtable `+0x220`: every outgoing actor RPC, C++ and Blueprint alike; the card UI is Blueprint, so no C++ stub exists), card → product via `GameplayCardManager.CardNameToProductHandles` |
| AdjustConsumableQuantity +N | 1..5, ≤ 10 per map, duffel-bag product | a duffel bag gives +1 per bag (§6b) |
| UnlockProduct | duffel-bag product, ≤ 10 per map | set insert; a duffel bag gives one per bag |
| SetSecureLeaderboardMetadata | never | not sent by b4bcoop |

"Duffel-bag product": the handle's table is the game's own `Products_DT` (object identity with the profile component's
`ProductsTable`) and the row's `ProductRow.DuffelBagTags` is not empty. `Products_DT` has 1135 rows, 658 of them with
duffel-bag tags (every burn card, cards, skins); starter cards like `Health_01` have none. The row lookup walks
`UDataTable::RowMap` (+0x38, TMap sparse array with its allocation bits).

Rejections log `rewardguard: REJECTED <command> <detail> from the host: <reason>` and show one chat line per map.
On the host (listen server) or offline the handlers run unchanged: the host's own rewards are applied locally and
never pass through these RPCs.

Live (dev build, `multi.sh 2`, Evansburgh B Easy):
- client plays `Burn_RollGunSMG` → client log `rewardguard: you played burn card Burn_RollGunSMG (product AAC3D0CA…)`;
  host `rewardtest cons <Burn_RollGunLMG product> -1` → `REJECTED … you did not play this burn card`; `burncard charge`
  → real forward accepted + `[CLIENT RPC] adjusting consumable Burn_RollGunSMG by -1`; a second `-1` for the same card
  → rejected.
- `endmission 1` → forwarded SP 53 accepted and applied. `rewardtest sp 99999`, `sp -50`, `stp 50000`,
  `unlock <Health_01 product>`, `cons <duffel product> 50`, `slb` → all rejected with their reason; `rewardtest sp 120`
  → accepted.
- `tools/e2e.py --quick` on the merged dev build: 10/10 (burn cards charged once each, SP forwarded and applied, profiles
  diffed exactly).

Residual risk: a host can still grant up to the bounds (e.g. a legitimate-looking 1000 SP) or charge one burn card
per card the client actually played; other client RPCs (e.g. `ClientApplyStatDeltas`, stats only) are not validated.
