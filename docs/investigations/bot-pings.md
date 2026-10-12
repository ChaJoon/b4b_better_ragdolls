# Bots and pinged toolkit doors

Build 14216215. Static analysis of `Back4Blood.exe` plus live runs on lane 2 (Flatpak Steam, `launch/multi.sh 1` and
`2`, Evansburgh A "Resurgence", Easy; 2026-10-12). Implementation: `native/src/bots.c` (host only, ini
`bot_toolkit_doors=0` = retail). Scope: supply-stash doors that need a toolkit; no other pings, no other doors.

Dan's report: a bot holds a toolkit it never uses on its own; pinging a supply-stash door does nothing.

## Short answer

Retail bots do open a pinged toolkit door, but only in a narrow case: the ping must be an **actor ping of the door**
and the door must lie **inside the bot's tether** at the moment of the ping (about 12 m, 7 m in combat, around the
bot's follow target, the teammate closest to the objective). The ping is looked at once. Two common situations break
it, both retail behaviour (not caused by the mod):

1. **The door is further away** (pinged from down the corridor, or the follow target is elsewhere): every bot drops
   the ping in its tether check, nothing happens, and nothing happens later either.
2. **A teammate stands between the pinger and the door**: the game's ping trace hits the teammate (not pingable), so
   the ping becomes a plain location ping (generic marker, "Look, over there!") on the teammate or on the door's
   surface. Bots ignore location pings (`PingConfig.bPingAllowsMoves = 0`). Bots crowd around the players, so this
   happens a lot, to the host and to clients alike.

The fix (host): a location ping aimed at a closed toolkit door is turned into that door's actor ping (the door's
stash icon and "Stash over here!" for everyone), and when no bot took the door, the nearest bot holding the door's
required item (40 m at most) gets it as its interact target, with the tether checks that would stop it lifted for
that bot and that door until the door opens (30 s at most).

## Retail mechanics

### Bot configuration
- `BotController_BP` uses `BotSharedConfigRowHandle` = `/Game/AITemplates/Bot/Playstyles/BotSharedConfigs` row
  `PlayerBot`; the active `FBotPlaystyleConfig` (0xC60) is copied to `BotController+0x3000`. Live (every hero bot):
  `AwarenessConfig.UsableActorFromPingTagQuery` at `+0x3468` has 3 dictionary tags `Usable.Healing Usable.Door
  Door.Locked`, tokens `0 1 4 2 2 1 0 2 2 1 2` = `ANY(ALL(Usable.Healing), ALL(Usable.Door, Door.Locked))`. (The
  `DefaultBotPlaystyleConfigSet` with an empty query is not what runs; NPC bots such as Emmett have the empty one.)
- The ping system copies `PingConfig` to `BotController+0x15C8`: `bListenToBots 1, bListenToBotsIfNoPlayers 1,
  bListenToPlayers 1, bListenOnlyToPrimaryTeammate 0, bPingAllowsMoves 0`; every one of the 34 `PingTypeInfo`
  entries listens to players, `PingLocConstraintDist` 500.
- `TetherCfg` (row PlayerBot): `TgtTetherDistNormal 1200`, `TgtTetherDistTight 700`, `FollowTarget
  ClosestToObjective`, `TeammateFollowDist 500`, `TeamTeleportNormalDist 3500`; `UsableSearchDist 1500`.
- `Door_Treasure_BP` (the stash door, spawned by `PassageSpawner_Door_TreasureRoom`, replicated): tags `Door.Locked
  Door.Treasure Omen.Door.Treasure`; `PingableComponent.Type = TreasureDoor (10)`, `OwningTeam Unknown`;
  `DoorUsable_BP` with `bRequireQuickUseItem 1` and one `InventoryConditions` entry `Accessories_DT /
  622285CB4E369488C40316AD4441BDDC` (LockPick_1) x1. Bots' loadouts sometimes carry `LockPick_1`.

### The ping path
- Client side, `UPlayerWaypointsComponent::SpawnPing` (`0x141E87A60`): `0x141E87D30` traces on the collision channel
  named **"Pingable"** (`0x141E88200`, `LineTraceSingleByChannel 0x143B25050`), takes the hit actor's
  `PingableComponent` (enabled, team Unknown or own team) and sends `ServerSpawnPing(PC, transform, PingActor (1),
  component)`. No pingable hit (e.g. a teammate's body first) -> `0x141E87FA0`, a location trace, and
  `ServerSpawnPing(..., PingLocation (2), null)`.
- Server: `ServerSpawnPing_Implementation` (vtable `+0x430`, `0x141E887C0`) -> waypoint manager `0x141E8D510`
  -> every bot's `FBotPingSystem` handler `0x1415E1A60` (synchronously):
  pinger checks (player/bot, listen flags), waypoint `Visibility`/team checks, then by type: `PingActor` ->
  `PingTarget->Owner` -> `0x1415DE7B0` -> `0x1415DB8D0` = **tether check `0x1415E9AB0`** (`FTetherSystem` at
  `BotController+0x1488`, `IsActorInside(actor, extra radius)` -> `0x1415E96E0`); `PingLocation` only acts with
  `bPingAllowsMoves`. A bot that accepts: `0x1415DE950` -> usable score `0x1415D86F0` > 0 (the usable must be
  inside the tether too, `0x1415D8D77`) -> `SetInteractTarget` (`0x1415B5E10`, score 1.0) -> ping-system count
  `0x1415E0A10` (+1 for the actor in a map at `PingSystem+0x398`, keyed by object index).
- The bot then walks there (`Bot_BT` InteractTarget branch, `Bot_UsableUse_GA`), quick-uses the toolkit
  (`LogUse BeginUse`, `ADoor::SetDoorState Closed -> Opening`, `EndUse Reason=SuccessfulUse`).
- What clears an interact target (dev `botdoor watch` logs every `ClearInteractTarget` with reason and caller):
  the bot's tether update `0x1415DDC10` calls `ClearAllTargets` (`0x1415B5D50`, at `0x1415DDD80`) when the bot and
  its target are both outside the tether (a bot sent further turns back halfway); the environment sensor clears it
  at the door when the usable scores 0 (reason `EnvironSensor:UsableScore && bUsesInteractTarget`, `0x1415FB722`);
  `GoToLocation` clears all targets; the door's usable switching off clears it (`OnInteractUsableEnabledChanged`).

### Live results, retail (fix off)
| Case | Result |
|---|---|
| Host alone, unobstructed actor ping, toolkit bot 2-7 m away | Bot takes it (`SetInteractTarget` during the ping), opens the door within ~3 s |
| 2 humans, host unobstructed, toolkit bot near | Same, opened |
| Client unobstructed actor ping, bots near | Opened |
| Team in the start saferoom, door 45 m | All bots reject (`0x1415DE950` returns 0), nothing |
| Team 21-28 m from the door (`botdoor far 0 18 25`), host actor ping | Both bots reject, nothing |
| Client pings with the host's hero in between | Location ping on the host or the door surface, nothing (shot `retail-client-ping-blocked-by-host.png`) |
| Bot sent by hand (`SetInteractTarget`) from 25 m | Walks off, target cleared by the tether update, turns back |
| Same plus `AddTether`/`SetScriptTarget`/`GoToLocation` | Tether: still cleared; script target: refused; `GoToLocation` teleports the bot, clears all targets |

Screenshots (`~/.local/share/b4b-coop/bot-pings/shots/`): `retail-host-door-before.png`,
`retail-host-ping-bot-opened.png`, `retail-client-ping-blocked-by-host.png`,
`retail-client-ping-blocked-location-marker.png`.

## The fix (`native/src/bots.c`)

Hooks (all signature-checked; any mismatch = nothing hooked, retail):
- `ServerSpawnPing_Implementation` `0x141E887C0`:
  - before the game: a `PingLocation` without target whose ray (pinger's eyes -> ping point) crosses a closed toolkit
    door's bounds (+20 cm) before the point, up to 6 m past it, or up to 25 m from the eyes with nothing but heroes in
    the way (`KismetSystemLibrary::LineTraceSingle`, Visibility) is passed on as `PingActor` with the door's
    `PingableComponent`;
  - after the game: if the pinged actor is a closed toolkit door and no bot has it as interact target, the nearest
    living bot holding the door's required item (`ItemCountByRowHandle`, 40 m at most) gets the ping-system count
    (`0x1415E0A10`) and `SetInteractTarget(door, 1.0)`, as retail does when a bot accepts.
- `FTetherSystem::IsActorInside` `0x1415E9AB0`: true for (that bot, that door) while the job is open, and
  `FTetherSystem::IsLocationInside` `0x1415E96E0`: true for that bot and any point within 4 m of the door. The
  location check also guards the bots' teleport back to the team (`0x1415E9200`..: bot and its target outside the
  tether -> `ClearAllTargets` from `0x1415E957C` and a teleport); without it a bot with a longer way (from the
  east side of the floor) walked half-way, was teleported back, and again on every ping.
- `ClearAllTargets` `0x1415B5D50`: the call from the tether update (return address `0x1415DDD85`) is skipped for that
  bot while its target is the door or the door's usable. Everything else still clears it.
- A job ends when the door's `DesiredDoorState` leaves Closed or its usable is disabled, or after 30 s.

Live results (fix on):
| Case | Result |
|---|---|
| Host actor ping, toolkit bot 25-30 m away (team 22 m from the door) | Bot walks there, opens, toolkit used (1 -> 0), returns |
| Client actor ping (RPC), toolkit bot 25 m away | Same |
| Client ping blocked by the host, near the door | Converted, bot sent (5 m), opened; the client sees the stash icon, "Stash over here!" (`fix-client-ping-blocked-converted.png`) |
| Client real ping from 15 m through the host's hero | Converted, bot sent (16 m), opened 6 s later |
| Client ping on a wall with the door behind it | Not converted (no line of sight) |
| Host ping when no bot has a toolkit | Log `no bot with the item within 40 m (3 bot(s), 0 holder(s))`, nothing |
| Host also holds a toolkit | The bot still goes (humans are never sent) |
| Host actor ping from the east side of the floor (18 m, longer way) | Before the location hook: teleported back twice; with it: opened after 6 s, the bot rejoined the team when the host moved |
| Map change (`/restart`) while a bot is on its way | No crash (sent entries checked by object index) |
| `e2e.py --quick` (lane 2), final build | 14/14 |

Screenshots: `fix-client-ping-blocked-converted.png`, `fix-far-ping-25m-opened.png`,
`fix-client-ping-15m-through-host.png`; `final-client-ping-15m.png` is an earlier build's miss (the location hit
something 4.6 m before the door; the rule then only looked 3 m past the point).

Other observations:
- A converted ping is often not accepted by the retail bot logic even with a bot next to the door (and a toolkit given
  with `botdoor give` is never accepted: probably the bot's own inventory bookkeeping); the fallback covers both.
- The toolkit count after opening: a toolkit given with `botdoor give` went 1 -> 0, a bot's own loadout toolkit
  stayed at 1 (retail and fix alike); not looked into.
- Only bots driving a survivor slot are sent (`PlayerSlotManager.TeamSlots[0]`); NPCs such as Emmett (an
  `NPCBotController_BP_Child`, a `BotController` too) are not.
- Every third `/restart` in a row sends the party back to Fort Hope (campaign run lost).

## Dev commands (dev builds)

`botdoor` (bots: ping-usable query, interact target, toolkits), `botdoor doors [all]`, `tags`, `ping <door#>`
(ServerSpawnPing with the door's pingable, a real RPC on a client), `look`/`lookat` + `face ping` (a real ping),
`tp <door#> [+|-] [me|bots]`, `far <door#> <min> <max>`, `give <bot#|me>`, `target`, `move ... goto|walk|tether|
untether|clear|script`, `send <door#>`, `fix on|off`, `watch`, `call <hex> <Func>`. Every ServerSpawnPing is logged
(`bots: ServerSpawnPing by ... type N target ...`).

## Open questions
- Other usables in the same query (`Usable.Healing`: trauma stations) have the same tether and blocked-ping limits;
  not changed (scope).
- Should the fix also send a bot that is in combat? It does not check; the bot's combat behaviour may override it.
- A `~` window toggle for `bot_toolkit_doors` (only the ini key so far).
- Not run on native Windows; not run on other maps' stash doors (other `Door_Treasure*` variants share the class).
