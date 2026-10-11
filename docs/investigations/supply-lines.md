# Supply lines on a client (issue #48)

Build 14216215, 2026-10-11, lane 1 local copies (`multi.sh 2`, instance 2 = client), dev build. Fix:
`native/src/supplylines.c`.

Report: a non-host completes a supply line and nothing takes its place; the host gets a new one.

## 1. Result

**On a network client the game's supply-line manager is often never turned on**, so nothing on the client ever
updates the supply lines while it is in someone's Fort Hope: a bought-out line is not replaced, expired lines stay,
no dynamic line is generated, the next line of a static chain never appears. Not host data: everything here runs on
the client's own offline profile; the host plays no part. Same race on every camp visit (join, camp to camp travel,
back from a mission), lost about two times out of three in these runs. The host and an offline camp are never
affected.

Fix (client only, no protocol change, works with an old host): if the local manager has stayed off for 3 s while
this machine is a network client and its profile is loaded, the agent makes the call the game's "profile loaded"
event would have made. Everything after that is the game's own code.

## 2. How supply lines work

- Kinds: the tutorial line and the **static chains** (`StaticCaravans` data asset: `TutorialCaravan` + 3
  `CaravanChains`, one line per chain at a time; a line is done when every product in it is unlocked, so the next
  one is worked out from the profile's unlocks and never saved), and **dynamic / skull-totem lines** (generated with
  a merchant, a schema and an expiry, saved as `PlayerProfileData.Caravans`, `FCaravanInstance` 0x68). Max 4 supply
  point lines + 1 skull totem line (`UCaravanManager.MaxCaravans` / `MaxSkullTotemCaravans`).
- Each local player's `UIScreenManager` (a `MainMenuScreenManager` in Fort Hope) owns a `UCaravanManager`. Its
  `ProfileCaravanData` is **what the supply-line screen shows** (`UCaravanScreen` 0x141CF73E0 copies it from
  `screen manager +0x330`).
- `UCaravanManager::UpdateCaravans` (0x141CB6740) rebuilds that list: "caravan %i has expired/been completed,
  removing" (RemoveCaravan), the static lines (`AddStaticCaravans` 0x141CB9950: first incomplete line per chain,
  0x141CBA0F0), "not enough Supply Point caravans, generating %d more" (UpsertCaravan). Commands go through the
  player's own profile component (`ExecuteCommand`, offline: `ApplyCommandToOfflineData`, saved ~30 s later).
- It runs from the manager's `OnCommandExecuted` handler (0x141CB66F0: types UnlockProduct, UpsertCaravan,
  RemoveAllCaravans, PurchaseCaravanItem on its own player's component) and from a 5 s timer. It returns at once
  while the manager is off (`+0x78 == 0`), while it is already running (`+0x79`) or while a Matchmaking task runs
  (0x141AFE420 on `GobiGameInstance.Matchmaking`).
- A purchase from the screen: `UCaravanProgressScreen::MakePurchaseRequest` (exec 0x141FED0C0 → 0x141CF5CF0) →
  `PPC::PurchaseCaravanItem` (0x141BC1F40: "purchasing item %i from caravan %i for %i SP %i STP", command
  `FPurchaseCaravanItemCommand` 0x38, vtable 0x1454E8A60). The screen marks the item bought in its own copy, so a
  purchase looks right even when nothing else updates.
- Generating a dynamic line picks a merchant at random and quietly gives up on that pass when it can't use it; the
  5 s timer retries. A replacement therefore takes from 0 to ~40 s on the host and on a client alike (seen: 2 to 8
  passes). Retail behaviour, not changed.

## 3. Root cause

- The manager is turned on only by its profile-loaded handler (0x141CB65F0): bound in the manager's init
  (0x141CB5500) to the global "profile loaded" event (0x146491778, broadcast by `PPC::SetProfileLoaded(true)`
  0x141BBF2E0 → 0x141BBF3D0, which also sets PPC `+0x1B8`), it acts only if the loaded component is the manager's own
  player's (0x141CB6590: screen manager → `PlayerState.PlayerProfileComponent`, else the controller's), then sets
  `+0x78`, clears the list and runs UpdateCaravans. Nothing else ever turns it on (the unload handler 0x141CB66C0
  turns it off).
- On a client the order of the two is not fixed. The client's profile component is local and loads right after the
  map; the screen manager and its caravan manager are created a little later. A manager created after the event
  never hears it and stays off for the whole visit. Client log, joining (dev build line from `supplylines.c`):
  ```
  03:15:40.177 UE LogPPC Log: [offline.76561198063588550]: profile loaded
  03:15:42.006 supplylines: client manager off 0s after it appeared (0 line(s) cached; its profile component
               00000000BD37CA00, ours 00000000BD37CA00 loaded, PlayerState 00000000BB533620)
  ```
  The host and an offline camp, every run: manager first, then the event:
  ```
  03:35:11.754 supplylines: host/offline manager off 0s after it appeared (... not loaded ...)
  03:35:12.273 UE LogPPC Log: [offline.76561198063588550]: profile loaded
  03:35:12.747 supplylines: host/offline manager on 1s after it appeared
  ```
- Client camp visits in these runs: manager off 7 times (joins, a camp to camp `servertravel`, blank profile), on by
  itself 3 times (a join, back from a mission, a join after restart). With the manager off the screen's list stays
  as the new manager started (empty; from the code, the screen was not looked at), so on a fresh visit the client may
  see no lines at all; with a list on screen
  (it was on once, or the screen's own copy) a bought-out line stays and nothing replaces it. The client's log then
  has no `LogCaravanManager` line at all, where the host's shows the removal and the 5 s "generating" retries.
- Not the cause: the PlayerState (present and matching when the manager appears), Matchmaking (idle on the client,
  `supplylines` prints it), the client's save (it holds the lines; its own camp updates them normally), the host.

## 4. Fix (`native/src/supplylines.c`)

Every second, on a network client only: the local manager (`PlayerController.UIScreenManager.CaravanManager`); if it
has been off for 3 s, its own profile component is this player's and that profile is loaded (PPC `+0x1B8` and the
LocalPlayer's `PlayerProfileSettings`), call the handler (0x141CB65F0) with it, once per manager. Signatures: the
handler (+ its `bEnabled = true` store), 0x141CB6590, UpdateCaravans, `SetProfileLoaded`; on a mismatch the fix is
off ("supplylines: signature mismatch"). Log: `supplylines: the supply-line manager missed this client's profile load;
turned it on (N line(s) now)`. Kill switch: env `B4BCOOP_NO_SUPPLYLINES=1`.

Nothing comes from the host: the handler and UpdateCaravans act on this player's own profile with the game's own
rules (rewardguard-style validation does not apply, there is no host input). Host and offline camps are untouched
(the check stops at "network client"). No protocol change.

Dev commands (`supplylines`): status (role, manager on/off, its component vs ours, Matchmaking busy, first open line
per static chain, saved lines with expiry, the lines on screen), `fix`, `off` (the state a missed load leaves),
`update`, `auto on|off`, `buy <line#> [item#|all]` (the screen's purchase, priced from `ProductRow.Price`/
`SkullTotemPrice`), `sp <N>` (supply points on our own test profile). `exec CaravanManager.DebugEnabled 1` +
`exec log LogCaravanManager VeryVerbose` turn on the game's verbose caravan log (UpdateCaravans passes, static chains).

## 5. Live test (2026-10-11, lane 1, `B4B_GPU=4090`)

| Step | Evidence |
|---|---|
| Repro, fix off (`B4BCOOP_NO_SUPPLYLINES=1`), client joined | `supplylines`: `manager ...: OFF, 0 line(s) cached` with 5 saved lines (an earlier run with the old build: OFF too, its 5 saved lines expired weeks ago and never removed) |
| Client buys out line 1791702699 (`buy 3 all`, 4 items, 200 SP) | 4× `purchasing item … ApplyCommandToOfflineData:PurchaseCaravanItem`, then nothing: line `4/4 bought (COMPLETE)`, no `LogCaravanManager` line with verbose logging on |
| `supplylines auto on` | `03:16:51.042 caravan 1791702699 has been completed, removing` → `RemoveCaravan` → `generating 1 more` → `supplylines: the supply-line manager missed this client's profile load; turned it on`; `03:17:11.130 adding caravan 1791703031` (UpsertCaravan) |
| Saved | client `.json` 03:17:28: 1791702699 gone, 1791703031 present, SP spent +200 |
| Restart (fix on) | client profile still has 1791703031; on the join `supplylines: ... turned it on` at 03:18:57, 3 s after the manager appeared |
| Static chain (blank client profile, `B4B_BLANK=2`) | fix on: tutorial line (`id -1`) → `sp 100000`, `buy 0 all` → first line of all 3 chains + a dynamic + a skull line. Then `auto off`, `off`, buy out a chain line → first open line of chain 2 becomes 1, the screen's list keeps the finished line, no update. `auto on` → `First incomplete caravan in chain 2: 1`, `AddStaticCaravanFromChain: ChainIndex 2 CaravanIndex 1`, new line on screen. After restart: `first open line per chain: 0 0 1`, SP spent 445 kept |
| Final build, golden profile, fix on from the start | join: manager off → `turned it on` (03:35:19); 03:35:31 five expired lines removed, 4 generated; `buy 1 all` → `03:35:47 caravan 1791704132 has been completed, removing` → `03:36:26 adding caravan 1791704186`; `.json` 03:36:32 has it, SP spent +80 |
| Host | own completion (`buy 0 all` on line 1791702279): `has been completed, removing` → `adding caravan 1791703169` 7 s later; the host never logs `turned it on`; its manager always on 1 s after it appeared |

Not covered: a real two-machine session (the race there is untested, the fix does not depend on which way it goes),
Windows, the player build live (same code without the dev commands; built), the supply-line screen itself (driven
through the same functions the screen calls; no screenshot).
