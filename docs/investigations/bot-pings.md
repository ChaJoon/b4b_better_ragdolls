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
- A `~` window toggle for `bot_toolkit_doors`: added (Cheats tab).
- Not run on native Windows; not run on other maps' stash doors (other `Door_Treasure*` variants share the class).

## Heal requests (bots heal a player who asks)

Static analysis plus live runs on lane 2 (Flatpak Steam, `multi.sh 1` and `2`, Evansburgh B "Tunnel of Blood", Easy;
2026-10-12). Implementation: `native/src/bots.c` "heal requests" (host decides; key on every machine; ini
`bot_heal_requests=1` = on, off by default (= retail), `bot_heal_key=` (default H)).

Dan's wish: a key that means "bots, I'm low, heal me", instead of hoping a bot decides to.

### Short answer

Retail bots do heal teammates on their own, through their generic "item use" logic, but rarely when a player wants it:

1. **Threshold**: a teammate counts only below **76%** of their full max health out of combat, **66%** with an
   enemy set (`PlayerBot` `CombatCfg.HealthInfo`; max = `HealthComponent.HealthTuning.InitialMaxHealth.Value`, the
   full max including card bonuses, not the trauma-reduced `CurrentMaxHealth`).
2. **120 s item cooldown**: after **any** successful item use (a heal, a thrown grenade) `Bot_ItemUse_GA` applies
   `Bot_ItemUseSuccess_Cooldown_GE` (120 s, tag `Bot.Actions.Item.Use.Cooldown.Success`); while it lasts the bot uses no
   Aid item (medkit, bandage, pills, defib) at all. A failed use: `Bot_ItemUse_Cooldown_GE`, 2 s.
3. **Never in a saferoom**: `InventoryCfg.ItemUseTypesSaferoomNotAllowed = 0x1FE` contains Aid_Heal.
4. **One item use at a time**: in a fight a bot often picks a grenade on a common instead (and keeps retrying it).
5. The tether update clears the item use with every other target when bot and teammate are outside the bot's tether.
6. **No way to ask**: no comm-wheel action for it (Approve, Deny, Go here, Group up, Spray, Warning, Ready, Wait,
   Thank), no ping (a ping's `Usable.Healing` query is for trauma stations / medical cabinets), no voice line that
   triggers it.

When several bots pass all of that they all go at once (seen: two medkits and pills on one player at 36%).

### Retail mechanics

- Bot_BT root selector (priority order): dead / death anim / rescue idle / hit reaction / High Priority Smart Actor /
  team teleport / GoToLocation / **ItemUse** (blackboard `ItemUse` set, no `Bot.Actions.Item.Use.Cooldown`
  tag: NavigateTo `ItemUseNavActor` or `ItemUseNavLoc`, radius 150, then TriggerAction `Bot_ItemUse_GA`) / Interact /
  Combat / alert location / follow teammate / low-priority smart actor. Item use outranks interacting and fighting.
- The item-use update `0x1415D5210` (BotController, every tick on the host; skipped while `ctl+0x494C` > 0, an item
  use in progress) scores each carried item, equipment slots 3..11 (and the weapons), with `0x1415D6030`
  (ScoreItemUse(ctl, FBotItemUseInfo*); info: +0x20 item, +0x28 enemy, +0x30 target/nav actor, +0x38 location,
  +0x44 slot, +0x45 needs nav = ItemUseConfig.TargetRange == Close). The best score > 0 becomes the item use
  (`UBotAIScriptingComponent::SetItemUseItem` `0x1415B7BE0`: blackboard ItemUse, ItemUseNavActor/NavLoc).
- ScoreItemUse: the item row (`Item.ItemRow` +0x360, `ItemUseConfig` +0xE0: UseType +0x0, TargetType +0x50,
  TargetRange +0xC8) must pass `ItemUseUseTypesAllowed` (0x1BB: not Buff, not Quest), `ItemUseTargetTypesAllowed`
  (0x1F), the saferoom mask (if the bot tracks saferooms and its hero is in one), the horde-only mask (0x30); then one
  target eval per allowed target type: self, each teammate (team list, not the bot), enemies, environment.
- Target eval `0x1415D67D0` (ctl, info, target, bSelf, outLoc) for Aid_Heal (UseType bit 2): Aid (bit 1) items return 0
  while the bot has `BotItemUseCooldownTag`; ratio = Health / InitialMaxHealth; threshold Self/Team OOC or InCombat
  (`ctl+0x3B20..0x3B2C`, i.e. config +0xB20); ratio above it = 0; then the item's ApplyEffect component must accept the
  target (vtable +0x4F8, e.g. temporary health already full with pills, trauma cap) and the target must not match
  `AssistGrappledTagQuery`; score = (1 - ratio) * EquipmentSlotPriority[slot]. **No distance check.**
- Live config (every hero bot, row `PlayerBot`): HealthInfo self 0.75 / 0.65, team 0.76 / 0.66; Emmett (NPC config)
  0.65 / 0.50, 0.60 / 0.30. Items: Medkit, Bandage, PainKillers use 3 (Aid + Aid_Heal) target 3 (self + teammate),
  range Close, support slot (4); Doc's `ActiveHealAbility_BP` use 3 target 2; Defib use 1 target 2 (revive).
- `Bot_ItemUse_GA`: AttemptUse `0x1415FEB00` re-scores the use (0 = ClearItemUseItem), selects the slot, waits for
  the item to apply; `UseTimeout` (log `UGameplayAction_BotItemUse::UseTimeout`) after about 5 s if it never applies.
  Cooldowns on success / failure as above.
- Revive is separate: `Assist*` scores in the awareness config (incapped 0.75, dead 0.95 with a defib); a downed player
  is revived by the bots on their own (seen live).
- The requester's own "I need healing" line can't be played from the agent: `DialogueComponent.SayLine` builds the
  event `DX_<Voice>_<Response>` (`DX_Evangelo_Low_Health`), the real events are `Dx_B_<Voice>_Low_Health_NN`
  (mesh-mods.md). The bot's own healing barks (`Healing_Other`) do play when it heals.

### Live, retail

| Case | Result |
|---|---|
| Host at 36% (60/165), everyone in the start saferoom, 60 s | Nothing (saferoom) |
| Same, everyone moved outside | All three bots took the host as item-use target at once, healed to 165 + 41 temp in 2 s |
| Host 83% with temp health, bots fighting commons | Bots keep (re)trying grenades on commons, nobody heals |
| Client 50%, outside, bots in a fight | Not healed (grenade item uses, cooldowns) |
| Host downed (health 0) | Bots revived it (112 health after) |

### The feature (`native/src/bots.c`)

- **Key** (`bot_heal_key`, default H; unbound in play: H only maps to the UI action `UICustomizeToggleUI`): through
  `cmds_hotkey_down` like the flashlight / third-person keys, 1.5 s between presses. Host: the request runs at once.
  Client: `PlayerWaypointsComponent::ServerSpawnCommWheelPing(PC, Transform, GroupUp)` at its hero with Scale3D
  `(1, 1, 0.875)`. The host hooks `ServerSpawnCommWheelPing_Implementation` (`0x141E88840`, vtable +0x420; `_Validate`
  is `return true`), recognises the mark (the Scale3D arrives intact) and runs the request for the RPC component's
  owner, no comm ping. A host without this shows a "Group up" ping and line there. **No protocol bump.**
- **Request** (host): the requester down → "You are down: bots revive downed players on their own."; not hurt →
  "You are not hurt."; else the nearest living survivor bot (PlayerSlotManager team slots) carrying an item with
  UseType Aid_Heal and TargetType Actor_Teammate, 60 m at most, gets a job (40 s at most):
  - its item-use cooldown effects are removed (`GameplayEffectsComponent.Remove(effect, RemoveAllStacks)` for its
    `Bot_ItemUse*Cooldown_GE` effects);
  - hook on ScoreItemUse `0x1415D6030`: for that bot the saferoom mask loses Aid_Heal during the call;
  - hook on the target eval `0x1415D67D0`: for that bot and the requester the team thresholds are 1.0 during the call
    (any missing health), and a healing result gets +10 (the requester beats other teammates, grenades, ...). The
    game's own checks still apply (the item must accept the target);
  - tether hooks (shared with the doors): `IsActorInside(requester)` and `IsLocationInside(any)` are true for that bot,
    and the tether update's ClearAllTargets is skipped (it also clears the item use). Without the location part the
    group-up / teleport-back check (`0x1415E9000`: ClearAllTargets at `0x1415E957C`, `0x1415E964C`) kept pulling a bot
    on its way back to the team.
  - The bot "took it" when its `GetCurItemUseTargetActor` is the requester: chat notice to everyone
    "<player> needs healing: <bot> is on the way."; healed (health or temporary health went up): "<bot> healed you."
    to the requester. Not taken within 4 s, or taken and not healed within 8 s + 0.5 s per metre, or the bot went down:
    the next candidate; none left: "No bot can heal you right now." ("No bot carries a healing item." when none has
    one). 40 s: "No bot reached you in time."
  - Jobs are keyed by object index (map change safe), at most 4 at once, one per requester and per bot.
- `bot_heal_requests` is off by default (2026-10-13, Dan; host, also the `~` Cheats tab checkbox "Bots heal you on
  request"): requests ignored, a client gets "The host has heal requests off.", the host's own key tells it where to turn
  it on. A client's key always sends the request (its own setting doesn't matter: the host decides).

### Live results (fix on)

| Case | Result |
|---|---|
| Host 85% in the start saferoom (`botheal ask`) | Doc took it at once (his ActiveHealAbility), full health 2 s later |
| Host again right after (Doc on the 120 s cooldown) | Cooldown removed, Doc healed again in 1.8 s |
| Client 60% in the saferoom (marked comm ping) | Dan (pills) took it 0.06 s after, healed (+51 temp) in 0.7 s; the client got both chat lines |
| Client 60%, real H key (`botheal key` on the client) | Walker healed it in 2.3 s |
| Host 85%, real H key | Hoffman healed in 0.7 s |
| Host 80%, nearest bot 16 m away outside | Jim walked over, healed 6.7 s after the request |
| Host 80%, bots 20 m away | Tala healed after 9.3 s |
| Client 70%, bots 8 m away | Jim healed in 3.9 s |
| Bots in a fight (item-use target a common) | Jim switched to the requester and healed |
| Host full health | "You are not hurt." |
| Host downed | "You are down: ..."; the bots revived it |
| Host with pills' temporary health on top, 83% | No bot took it (the item's apply check refuses), "No bot can heal you right now." |
| Requester teleported 1.6 m next to another player | Both bots reached 1.7 m and `UseTimeout`ed twice each, then "No bot can heal you right now." |
| Host with `bot_heal_requests` off, client presses H | Client: "The host has heal requests off." |
| `e2e.py --quick` (lane 2), final build | 14/14 |

Screenshot: `~/.local/share/b4b-coop/bot-pings/shots/heal-client-request-notice.png` (client's chat lines, Jim's
healing bark subtitle).

### Open questions

- `UseTimeout` at 1.5-2 m happens (seen when the requester stood next to another player, and with heroes teleported by
  the test commands): the bot's use never applies; the fallback moves to the next bot. Why exactly (aim trace blocked
  by the other hero? use range?) not found; `Bot_ItemUse_GA`'s OnSelectedItemChanged `0x1415FF140` is the place.
- Each request uses an item even when a teammate (human) could heal; spamming the key empties the bots' medkits.
- Bots heal on request inside saferooms, where retail never does (the medical cabinet is usually there).
- Not run on native Windows; not run with 5+ players.
