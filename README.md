# b4b_better_ragdolls

A fork of [b4b-coop](https://github.com/actuallydan/b4b-coop) that makes the Ridden die properly: physics ragdolls,
bodies thrown by shots and blasts, and dismemberment. Everything else is b4b-coop as released (the base version is in
`VERSION`); its own README follows below this section.

> Unofficial and experimental. Built with heavy AI help (Claude); the commits say so. Report problems with the
> ragdolls in **this repo's Issues**, not to b4b-coop.

## What it changes

- **Bodies fall all the way.** The game already ragdolls Ridden at death, but its death animation steers the body,
  and its "settled" check freezes bodies still moving at up to ~5 m/s, so most freeze mid-fall. Here the animation is
  cut and the freeze waits until the body has really come to rest.
- **The killing hit throws the body**, with a strength per weapon type (pistol, SMG, rifle, LMG, shotgun, sniper).
- **Blasts** (frags, pipe bombs, barrels, propane, and the Volatile Ridden and Broadside card explosions) throw bodies
  from the explosion instead of turning nearby ones into blood bags, and take 2-3 limbs (and sometimes the head) off
  through the game's own dismemberment.
- **Shooting a body that is already down pushes it.**
- **Ridden that die running keep their speed** and tumble on, instead of dropping on the spot.
- **Wounds where the shot lands**: shotguns, snipers, LMGs and rifles take off the arm, leg or head they hit, or open
  the chest or stomach; pistols and SMGs only take heads; the bow takes nothing (not even the game's own head pop).
  Blast victims get open wounds too. The game's own extra open wounds on a gun kill (the chest and an arm opened with
  the head) are stopped on Commons; melee, blasts and fire keep the game's own (`/ragdoll wounds game on` brings back
  all of the game's own wounds).
- **Survivor barks muted** (on by default, each player's own setting): no "She shoots, she scores!" after headshots,
  melee kills, shoves, shooting explosive items, picking up copper and Tala's bleed quips ("Keep bleeding!").
  `/dialogue barks off` brings them back.
- **`/give <weapon> [tier]`** (host, after `/cheats on`): a weapon into a hero's inventory, for testing; `/give list`
  numbers every weapon with its tiers.
- Reeker and Exploder pops, and the gameplay of explosions, are left as the game does them.

## Status

- Tested in co-op (17 Oct 2026): host on Linux (Proton), two friends on native Windows, all running this build.
  Protocol unchanged, so it also plays with players on stock b4b-coop of the same protocol.
- Performance lightly tested: no stutters in hordes on a fairly strong PC. Not tried on weak machines.
- The ragdolls run on each PC that has this build. Two parts come from the host and reach every player: refused blood
  bags and lost limbs.
- One freeze seen in testing (25 Oct 2026, build 42, cause not found). If the game ever stops for 10 s, the log
  (`Gobi\Binaries\Win64\b4bcoop-*.log`) records where the game's main thread is stuck (`ragdoll freeze` lines):
  please send it.

## Install

1. Install b4b-coop as its README explains (below), using the release that matches this fork's `VERSION`.
2. With the game closed, replace both files with this fork's build (Releases):
   - `xinput1_3.dll` next to `Back4Blood.exe`
   - `X3DAudio1_7.dll` in `Gobi\Binaries\Win64\`
3. Start a co-op game and type `/ragdoll on` in chat once. It stays on, and every setting is saved in `b4bcoop.ini`.

Keep both files from the same build. Automatic updates are **off by default** in this fork: an update would install
the official b4b-coop and remove the ragdolls (`auto_update=1` in `b4bcoop.ini` turns them back on). Don't take mods
into online play.

## Commands

| Command | Does |
|---|---|
| `/ragdoll on` / `off` | turn the ragdolls on or off (remembered) |
| `/ragdoll` | status |
| `/ragdoll push [x]`, `/ragdoll push <kind> <n>` | how hard the killing hit throws, overall or per weapon kind; no value shows the table |
| `/ragdoll shot [x]` | how hard shots push bodies that are already down |
| `/ragdoll gravity [x]` | extra gravity on falling bodies (default 1.3) |
| `/ragdoll dismember on\|off\|limbs <min> [max]\|head <0-100>` | dismemberment of blast victims |
| `/ragdoll blood on\|off` | paint spared blast victims with blood like the others |
| `/ragdoll carry [x]` | how much of its running speed a dying Ridden keeps (default 1, off = 0) |
| `/ragdoll carry keep <s>` | how long a run's speed survives a stagger before the kill (default 1 s) |
| `/ragdoll wounds on\|off\|living off\|arms\|all\|blast on\|off\|kinds\|small\|heads\|none <K,K>` | wounds by weapon and hit location (`none`: kinds that take nothing, default the bow) |
| `/ragdoll wounds game on\|off` | the game's own wounds on Commons: `off` (default; host) stops its open wounds from those weapons and every wound from the bow |
| `/dialogue barks on\|off\|mute <Name,Name>\|log` | survivors' barks muted (default on: `Headshot,Kill_Bat,Shoot_Explosive,Bash,PickUp_Copper,Trigger_Bleed`); `log` names the next 40 lines heard |
| `/give <name\|code\|#n> [tier] [player]`, `/give list [all] [filter]` | a weapon (or any item) into a hero's inventory, its best tier unless one is named (host, cheats on) |

## Building

`tools/fetch-deps.sh --build`, then `native/build.sh --release` (the DLLs land in `native/out/release/`). The build is
reproducible. All ragdoll code is in `native/src/ragdoll.c`.

---

# b4b-coop

Unofficial private co-op for **Back 4 Blood**: play the game's offline mode together with your Steam friends, up to
4 players (5 if the host wants), with no Warner Bros / Turtle Rock servers. Everyone keeps their own offline progress.

> Unofficial and unaffiliated. For playing a game you own with friends in offline mode. It never touches the
> official online services. Use at your own risk. See [Safety & disclaimer](#safety--disclaimer).

> This mod was made quickly for my group of friends, it is not something I intend to support with any degree of
> dedication. Pull requests for features or bug fixes are welcome. If you find it useful and fun, consider starring
> the project and sharing it with friends. Eventually running your own server might be the only way to play this game
> cooperatively.

Two words used below: the **host** is the player whose game the others join (by default, whoever is in their own
Fort Hope); the **`~` window** is b4bcoop's own menu in the game: press the **`~`** key (left of `1`).

## Install
> Already have b4bcoop 0.7.0 or newer? You don't need to install again: update from inside the game, see
> [Update](#update). Upgrading from something older? Do step 3 of [Uninstall](#uninstall) first.

**Everyone who plays together installs it, and everyone needs the same version.**

1. Download the latest `b4bcoop-<version>.zip` from [Releases](../../releases).
2. Open the game folder: in Steam, right-click **Back 4 Blood** → **Manage** → **Browse local files**. It is the
   folder with `Back4Blood.exe` and a folder named `Gobi` in it, usually:
    - Windows: `C:\Program Files (x86)\Steam\steamapps\common\Back 4 Blood`
    - Linux / Steam Deck (in Desktop Mode): `~/.local/share/Steam/steamapps/common/Back 4 Blood`
3. Extract **everything** from the zip into that folder, and say **Yes** if asked to merge. The zip has the same
   layout as the game folder (its `Gobi` folder merges into the game's), and no game file is replaced. You get:
   ```
   Back 4 Blood\xinput1_3.dll                          (next to Back4Blood.exe)
   Back 4 Blood\b4bcoop-README.txt, b4bcoop-COMMANDS.txt, b4bcoop-LICENSE.txt
   Back 4 Blood\Gobi\Binaries\Win64\X3DAudio1_7.dll     (the mod)
   Back 4 Blood\Gobi\Binaries\Win64\b4bcoop.ini         (settings, optional)
   ```

*That's all: no launch options, no scripts to run.*

## Play
1. Press **Play** in Steam. Steam itself must be online: friends connect through Steam.
2. b4bcoop asks how you want to play: pick **b4bcoop co-op** (it is picked for you after 15 seconds without input).
   **Online** is the official online game, see [Play online](#play-online). On Linux/Steam Deck the question appears
   from the second start on.
3. The game signs in Offline by itself and takes you to Fort Hope: no Sign in button, no Online/Offline question.
4. In Fort Hope you are **hosting**: your Steam friends can join you. Nothing to set up, no ports to open.
5. Start missions from the war table as usual. Everyone in your game follows you in.

**Invite a friend:** press `~`, tab **Session**, and click **Invite** next to their name (or use Steam's own invite).
Only the host's **Steam friends** can join ([Options](#options) changes that).

**Join a friend:** in the Steam friends list, right-click your friend while they are in Back 4 Blood → **Join Game**,
or accept their invite, or press `~` → **Session** → **Join** next to their name. If your game isn't running, Steam
starts it and it joins them once it has signed in.

The **`~` window** has everything else: players (kick, ban, lock), joining and leaving, third-person camera,
flashlight, cheats, add-ons, settings and updates. The same things work as chat commands (`/help` in the game's chat).

## Play online
Keep the mod installed and play the official online game: press **Play**, pick **Online**. b4bcoop then moves its
game file (`X3DAudio1_7.dll`) into a `b4bcoop-online` folder next to `Back4Blood.exe` before Easy Anti-Cheat starts
the game, so the online game runs without any b4bcoop code. The next time you pick **b4bcoop co-op**, the file moves
back. Before each online start your offline save is copied to
`PlayerProfileSettings-b4bcoop-before-online-<date>-<time>.sav` next to it (the 5 newest are kept), in case the
online sign-in touches it.
- **Remember my choice** skips the question from then on. To get it back: hold **Shift** while the game starts, or
  `~` window → **Settings** → "Game start" (in co-op), or set `launch=ask` in `b4bcoop.ini`.
- Controller (Steam Deck): **A** co-op, **Y** online, **X** remember.
- Steam **Join Game** on a b4bcoop friend always starts co-op.
- In co-op the game's own Sign in step is skipped (b4bcoop signs in Offline by itself). With `auto_signin=0` in
  `b4bcoop.ini` you see it again; picking Online there still signs you in Offline (b4bcoop never goes online).

## Options
All optional. Most are in the `~` window (they're saved for you). Or open `Gobi\Binaries\Win64\b4bcoop.ini` in a
text editor and remove the `;` in front of a line to turn it on. Saved changes apply within a couple of seconds, also
while you play (`host`, `join`, `host_ip` and the network options need a game restart). Every option, with defaults
and examples: [docs/COMMANDS.md](docs/COMMANDS.md#b4bcoopini-options).
- `host=0`: don't host; your offline game stays private.
- `teamsize=5`: (host) 5 survivors instead of 4.
- `auto_signin=0`: show the game's own Sign in step (Offline is still the only choice that works with the mod).
- `launch=coop` or `launch=online`: no question at the game start (what "Remember my choice" writes); `launch=ask`
  asks again.
- `flashlight_key=L`, `thirdperson_key=N`, `bot_heal_key=H`: the keys for the flashlight, the third-person camera
  and asking the bots to heal you (`off` turns a key off).
- `allow_joins=anyone`: (host) let in people who aren't your Steam friends; or `allow_steamids=<17-digit Steam ID>`
  for one person.
- `host_ip=1`: **advanced**, only if you know you need it: host and join by IP address instead of through Steam. Needs
  port forwarding (UDP 7777) and triggers the Windows Firewall prompt. Everyone in the game needs it.

**Add-ons** (new outfits, weapon looks, textures): press `~`, tab **Add-ons**. It lists the free add-ons from the
add-on shop and the ones you have: **Add** downloads one (most are ready at once), **Wear** / **Take off** an outfit
(on your survivor at once, kept for that survivor), **Use** / **Stop** a weapon look, **Update**, **Remove**. Or put
an add-on's `.pak` file in a `b4bcoop-addons` folder next to `Back4Blood.exe` and restart the game. Only you see your
add-ons, and nobody learns which ones you have; friends with the same add-on see your outfit, the others your normal
survivor. By default hosts let in players with any add-ons; a host can allow cosmetic ones only, or none
(`addons_policy=`; your game checks yours before joining). Details: [docs/COMMANDS.md](docs/COMMANDS.md#add-ons).

All chat commands and options: [docs/COMMANDS.md](docs/COMMANDS.md).

## Update
By itself: b4bcoop checks GitHub for an update at each start and installs it before the game opens (a small window
shows it; **Skip update** plays the version you have; `auto_update=0` turns it off). Joining a friend through Steam
installs your friend's version, so the join works.

In game: press **`~`**, tab **Updates** → **Check for updates** → **Download and install on next start**, then
restart the game. Every download must carry the b4bcoop release signature, and your `b4bcoop.ini`, add-ons and bans
stay as they are. **Go back to ...** in the same tab returns to the version you had. Details:
[docs/COMMANDS.md](docs/COMMANDS.md#updates).

By hand: download the new zip from [Releases](../../releases) and extract it over the old files ([Install](#install),
step 3).

## Uninstall
Delete these files from the game folder (Steam → right-click Back 4 Blood → Manage → Browse local files):
1. Next to `Back4Blood.exe`:
    - `xinput1_3.dll`
    - `b4bcoop-README.txt`
    - `b4bcoop-COMMANDS.txt`
    - `b4bcoop-LICENSE.txt`
    - the `b4bcoop-addons` folder (only there if you installed add-ons)
    - the `b4bcoop-update` folder (only there after an update)
    - the `b4bcoop-online` folder (only there while b4bcoop is switched off for online play)
2. In `Gobi\Binaries\Win64`:
    - `X3DAudio1_7.dll`
    - `b4bcoop.ini`
    - `b4bcoop-outfits.txt` (only there if you wore an add-on outfit)
    - all `b4bcoop-*.log` files
    - `b4bcoop-bans.txt` (only there if you banned someone)
3. Left over from older versions, if present, in `Gobi\Binaries\Win64`: `dwmapi.dll`, `Play B4B co-op.cmd`,
   `steam_appid.txt`. On Linux also remove the launch option `WINEDLLOVERRIDES="dwmapi=n,b" %command%`
   (Steam → right-click Back 4 Blood → Properties → Launch Options).

Steam's "Verify integrity of game files" does **not** remove these: they are extra files, not game files. Your offline
progress stays either way.

Making your own add-ons: see the modkit ([modkit/README.md](modkit/README.md), `b4bcoop-modkit-<version>.zip` on [Releases](../../releases)).

## Troubleshooting
- **"Everyone needs the same version"**: someone has another b4bcoop version. Press `~`, tab **Updates**: it offers
  the host's version (or the latest). Or everyone downloads the latest release and extracts it again (Install).
- **Play online / with Easy Anti-Cheat** without removing the mod: pick **Online** when the game starts (see
  [Play online](#play-online)). The launch option `-b4bcoop=off` does the same without asking.
- **b4bcoop is gone after playing online**: pick **b4bcoop co-op** at the next start (hold Shift if you made Online
  your remembered choice). By hand: move the `Gobi` folder from `b4bcoop-online` back into the game folder, or extract
  the zip again.
- **The log**: `Gobi\Binaries\Win64\b4bcoop-<date>-<time>-<number>.log` (the newest one; the last 20 are kept). Its
  first lines show the version, e.g. `b4bcoop 0.9.7 (protocol 2)`. No new log after starting the game = the mod didn't
  load.
- **Windows**: this install layout hasn't been tested on a Windows PC yet (it has on Linux and Steam Deck's Proton). If
  the mod doesn't load there, please [open an issue](../../issues) and attach `Gobi\Binaries\Win64\b4bcoop-launcher.log`
  if that file exists.
- **Join Game missing or not working**: both of you need the mod (same version), Steam online, and the host in Fort
  Hope or a mission. Fallback: the host looks up their
  17-digit Steam ID (Steam → click your account name at the top right → Account details, or `~` → **Session** →
  **Copy**), and you type `/join steam:<that number>` in the game's chat while in your own Fort Hope.

## Features

**Co-op**
- Your offline Fort Hope is a co-op game: you host automatically, and Steam friends join with **Join Game** in their
  Steam friends list or through an invite. Steam's peer-to-peer networking, so no port forwarding.
- Everyone follows the host into missions and across chapters, and joiners take over bot slots.
- Everyone plays with their **own deck**, keeps their **own rewards** (supply points, skull totems, unlocks,
  consumables land in their own profile) and can play **burn cards**, charged to their own profile. Joined players get a
  new supply line after finishing one, like the host.
- **5 players** (`teamsize=5` on the host); a clean "Server full." when nobody else fits.
- **The host can leave**: the next player takes over as host and everyone moves to them (or the host picks one: `~` →
  **Players** → **Make host**). In a mission the run goes on from the start of the current chapter.
- Who may join: Steam friends by default, or anyone, or a list of Steam IDs. Kick, ban and lock.
- Everyone must run the same version; a mismatch is refused with a message that points to the Updates tab.
- **Co-op or online** at every start: online runs the official game with Easy Anti-Cheat and no b4bcoop code.

**In game**
- The **`~` window**: players, session, camera, flashlight, cheats, add-ons, settings and updates, with key bindings
  and settings saved for you. The same things also work as chat commands (`/help`).
- **Manual flashlight** toggle (L), plus a wider, longer or brighter beam for your own view.
- **Third-person camera** for your own hero (N key, or `~` → **Camera**), with free look while you stand still.
- **Smarter bots** (the host decides, `~` → **Cheats**): a bot carrying a toolkit opens a supply-stash door you ping
  (on by default), and **H** asks the nearest bot with a healing item to come and heal you (off by
  default).
- **Change your look**: add-on outfits and weapon looks from the `~` window's Add-ons tab; other survivors' outfits
  and Fort Hope NPC bodies with `/model`. Others see it too if they have the add-on; otherwise they see your survivor.
- **Cheats** (host, opt-in: `~` → **Cheats** → **Cheats on**, or `/cheats on`): god mode, fly, noclip, spawn, horde
  control, slow motion and more, as a sandbox. Rewards still work. While they're on, every player can add supply
  points, skull totems and burn cards to their own save, or unlock everything in the supply lines (never anyone
  else's save; a backup is made first).

**Add-ons**
- One **Add-ons** tab in the `~` window: the free add-ons of the signed add-on shop and the ones you have, in one
  list. **Add** (most are ready at once, no restart), **Wear** (an outfit, at once, kept for that survivor),
  **Use** (a weapon look), **Update**, **Remove**; search, filter and sort. The shop's list loads by itself when you
  reach Fort Hope.
- Or drop `.pak` add-ons into `b4bcoop-addons`: textures, custom outfits and weapon looks (up to 512 add-ons and 512
  outfits). Added outfits are in the game's customization screen, for every survivor. Nobody learns which add-ons you
  have; the host decides whether joiners may bring gameplay-changing ones.
- **Modkit** (separate download, `b4bcoop-modkit-<version>.zip`): turn a downloaded character (FBX, glTF/glb, VRM, OBJ,
  DAE, .blend) or a gun model into an outfit or weapon look, with hair physics, cloth for skirts, coats and capes,
  talking and blinking faces, and first-person arms. See [modkit/README.md](modkit/README.md).

**Updates and safety**
- **Updates** before the game starts (checked at each start, `auto_update=0` turns it off) and in game (`~` →
  Updates, since 0.7.0): from GitHub, the release signature verified, and it can go back; an update that doesn't
  start properly is reverted automatically.
- No traffic to the game's online services: they are blocked while the mod runs, and the game's own port only
  listens on your PC. b4bcoop itself only talks to Steam (co-op) and GitHub (the update check at each start, the add-on shop's list,
  updates).

**Limits**
- Tested on Linux (Proton) and Steam Deck; Windows is lightly tested (see Troubleshooting).
- Supports the current Steam build of Back 4 Blood only; on another build the mod checks and does nothing.

## Safety & disclaimer
- **Unofficial.** Not made, endorsed or supported by Turtle Rock Studios or Warner Bros. Games.
- **Offline mode only.** It uses the game's offline mode and, while it runs, blocks the game's online services
  (Epic/WB/Turtle Rock). Your offline progress is saved on your PC as usual.
- **Never in the online game.** Online play goes through Easy Anti-Cheat with b4bcoop's game file moved out of the
  game's folders first ([Play online](#play-online)); a game that has b4bcoop loaded signs in Offline only. Playing
  online with a modified game or without anti-cheat can break the game's terms.
- **Nothing exposed.** By default the mod opens nothing to the network (no Windows Firewall prompt): players connect
  through Steam's peer-to-peer networking, and the game's own network port only listens on your PC itself. Only
  people you allow (Steam friends by default) can join your game.
- **No game files.** The zip holds only this project's own two DLLs, a config file and text files; it replaces no
  game file.
- **Open source, built in public.** Every release is built by GitHub Actions from this repository
  (`.github/workflows/release.yml`), with a `SHA256SUMS` file and a signed build provenance attestation. To check a
  download: `sha256sum -c --ignore-missing SHA256SUMS` (Windows: `Get-FileHash b4bcoop-<version>.zip`), and
  `gh attestation verify b4bcoop-<version>.zip -R actuallydan/b4b-coop`. The build is reproducible:
  `tools/fetch-deps.sh --build` then `launch/package.sh` gives the same DLL hashes (`SHA256SUMS` lists them).
- **Why antivirus may warn.** `X3DAudio1_7.dll` is an unsigned DLL that the game loads in place of a DirectX DLL and
  that hooks game functions; `xinput1_3.dll` changes which program Steam's launcher starts and downloads b4bcoop updates. That is also
  what some malware does, so heuristic scanners sometimes flag them. There is no installer. Network traffic: the
  co-op session over Steam, and GitHub only for the update check at each start (before the game opens; it installs a
  newer release; `auto_update=0` turns it off), the add-on shop's list (once per game start, in Fort Hope; `shop=0`
  turns it off) and for updates and add-ons you click. If you don't trust the zip, build it yourself from the source.
- **How it loads.** The game looks for `X3DAudio1_7.dll` (DirectX audio) in its own folder first, on Windows and under
  Proton; ours forwards to the real one. On Windows, Steam's Play button starts a small launcher that would start Easy
  Anti-Cheat, which keeps mods out; `xinput1_3.dll` is loaded by that launcher, asks co-op or online, and for co-op
  starts the game directly instead (on Linux/Steam Deck the mod sets a Wine setting for `Back4Blood.exe` in the game's
  Proton prefix so that this file loads there too). Details: `docs/investigations/launch.md`,
  `docs/investigations/online-mode.md`.
- **Your save.** The mod checks every reward a host sends before it touches your save, and ignores anything outside
  normal mission limits.
- **No warranty.** MIT licensed ([LICENSE](LICENSE)), provided as is. Back up
  `%LOCALAPPDATA%\Back4Blood\Steam\Saved\SaveGames` if your progress matters to you.

## Development
See `CLAUDE.md` (layout, setup, versioning, progress) and `docs/NOTES.md` (engine findings).
`tools/fetch-deps.sh` then `native/build.sh` (cross-compiles on Linux with zig; dev build with the local command
server and test commands) → `launch/package.sh` (player build, `native/build.sh --release`) →
`dist/b4bcoop-<version>.zip`. The version is in `VERSION`.
