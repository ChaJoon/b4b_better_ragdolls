# b4b-coop

Unofficial private co-op for **Back 4 Blood**: play the game's offline mode together with your Steam friends, up to
4 players, with no Warner Bros / Turtle Rock servers. Everyone keeps their own offline progress.

> Unofficial and unaffiliated. For playing a game you own with friends in offline mode. It never touches the
> official online services. Use at your own risk. See [Safety & disclaimer](#safety--disclaimer).

> This mod was made quickly for my group of friends, it is not something I intend to support with any degree of
> dedication. Pull requests for features or bug fixes are welcome. If you find it useful and fun, consider starring
> the project and sharing it with friends. Eventually running your own server might be the only way to play this game
> cooperatively.


## Install
> Already have b4bcoop 0.7.0 or newer? You don't need to install again: update from inside the game, see
> [Update](#update). Upgrading from something older? Do step 3 of [Uninstall](#uninstall) first.

**Everyone who plays together installs it, and everyone needs the same
version.**

1. Download the latest version of the .zip file from [Releases](../../releases).
    - Should appear as `b4bcoop-<version>.zip`
2. Open the game folder: 
    1. in Steam, right-click **Back 4 Blood** 
    1. Click **Manage**
    1. Click **Browse local files**. 
    1. A folder should open that contains `Back4Blood.exe` and a folder named `Gobi`. 
        1. On Windows this is usually `C:\Program Files (x86)\Steam\steamapps\common\Back 4 Blood` 
        1. On Linux/Steam Deck: `~/.local/share/Steam/steamapps/common/Back 4 Blood`

3. Extract **everything** from the zip into that folder. 
    1. The zip has the same layout as the game folder, so its `Gobi`
   folder merges into the game's `Gobi` folder. No game file is replaced. You get:
   ```
   Back 4 Blood\xinput1_3.dll                          (next to Back4Blood.exe)
   Back 4 Blood\b4bcoop-README.txt, b4bcoop-COMMANDS.txt, b4bcoop-LICENSE.txt
   Back 4 Blood\Gobi\Binaries\Win64\X3DAudio1_7.dll     (the mod)
   Back 4 Blood\Gobi\Binaries\Win64\b4bcoop.ini         (settings, optional)
   ```
4. say **Yes** if asked to merge   

*That's all: no launch options, no scripts to run manually.*

## Play
1. Press **Play** in Steam. Steam itself must be online: joins go through Steam.
2. b4bcoop asks how you want to play: pick **b4bcoop co-op** (it is picked for you after 15 seconds without input).
   **Online** is the official online game, see [Play online](#play-online). On Linux/Steam Deck the question appears
   from the second start on.
3. The game signs in Offline by itself: no Sign in button, no Online/Offline question.
4. In Fort Hope you are **hosting automatically**: your Steam friends see **Join Game** on you in their Steam friends
   list.
5. Start missions from the war table as usual. Everyone in your game follows you in.

**Join a friend:** in the Steam friends list, right-click your friend while they are in Back 4 Blood → **Join Game**,
or accept their Steam invite. It works with your game closed or running: the game starts if needed, signs in
Offline and joins them.

Only the host's **Steam friends** can join. Press **`~`** in game for the b4bcoop window: players (kick, ban, lock),
join/leave, camera, flashlight, keys, cheats, looks and add-ons, updates. The same things work as chat commands
(`/help` in the game's chat).

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
All optional. Open `Gobi\Binaries\Win64\b4bcoop.ini` in a text editor and remove the `;` in front of a line to turn
it on. Saved changes apply within a couple of seconds, also while you play (`host`, `join`, `host_ip` and the network
options need a game restart). Every option, with defaults and examples:
[docs/COMMANDS.md](docs/COMMANDS.md#b4bcoopini-options).
- `host=0`: don't host; your offline game stays private.
- `teamsize=5`: (host) 5 survivors instead of 4.
- `auto_signin=0`: show the game's own Sign in step (Offline is still the only choice that works with the mod).
- `launch=coop` or `launch=online`: no question at the game start (what "Remember my choice" writes); `launch=ask`
  asks again.
- `flashlight_key=L`: the key for the manual flashlight toggle (`off` turns it off).
- `allow_joins=anyone`: (host) let in people who aren't your Steam friends; or `allow_steamids=<17-digit Steam ID>`
  for one person.
- `host_ip=1`: **advanced**, only if you know you need it: host and join by IP address instead of through Steam. Needs
  port forwarding (UDP 7777) and triggers the Windows Firewall prompt. Everyone in the game needs it.

Add-ons (textures, models): press `~`, tab **Looks & add-ons**: **Get more** lists free ones (**Add**, then **Wear**),
**Installed** manages yours. Or put an add-on's `.pak` in a `b4bcoop-addons` folder next to `Back4Blood.exe` and
restart; `/addons` lists them. Only you see your add-ons, and nobody learns which ones you have. By default hosts
let in players with any add-ons; a host can allow cosmetic ones only, or none (`addons_policy=`; your game checks
yours before joining).
Details: [docs/COMMANDS.md](docs/COMMANDS.md#add-ons).

All chat commands and options: [docs/COMMANDS.md](docs/COMMANDS.md).

## Update
In game: press **`~`**, tab **Updates** → **Check for updates** → **Download and install on next start**, then
restart the game. Nothing is downloaded or changed until you click; the download must carry the b4bcoop release
signature, and your `b4bcoop.ini`, add-ons and bans stay as they are. **Go back to ...** in the same tab returns to
the version you had. Details: [docs/COMMANDS.md](docs/COMMANDS.md#updates).

By hand: download the new zip from [Releases](../../releases) and extract it over the old files (Install, step 2).

## Uninstall
Delete these files from the game folder (Steam → right-click Back 4 Blood → Manage → Browse local files):
1. Next to `Back4Blood.exe`: 
    - `xinput1_3.dll`
    - `b4bcoop-README.txt`
    - `b4bcoop-COMMANDS.txt`
    - `b4bcoop-LICENSE.txt`
    - the `b4bcoop-addons` folder (only there if you installed add-ons).
    - the `b4bcoop-update` folder (only there if you updated from the game).
    - the `b4bcoop-online` folder (only there while b4bcoop is switched off for online play).
2. In `Gobi\Binaries\Win64`:
    - `X3DAudio1_7.dll`
    - `b4bcoop.ini`
    - all `b4bcoop-*.log` files
    - and `b4bcoop-bans.txt`
   (only there if you banned someone).
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
- **The log**: `Gobi\Binaries\Win64\b4bcoop-<date>-<time>-<number>.log` (the newest one; the last 20 are kept). Its first lines show the version, e.g.
  `b4bcoop 0.3.0 (protocol 1)`. No new log after starting the game = the mod didn't load.
- **Windows**: this install layout hasn't been tested on a Windows PC yet (it has on Linux and Steam Deck's Proton). If
  the mod doesn't load there, please [open an issue](../../issues) and attach `Gobi\Binaries\Win64\b4bcoop-launcher.log`
  if that file exists.
- **Join Game missing or not working**: both of you need the mod (same version), Steam online, and the host in Fort
  Hope or a mission. Fallback: the host looks up their 17-digit Steam ID (Steam → click your account name at the top
  right → Account details), and you type `/join steam:<that number>` in the game's chat while in your own Fort Hope.

## Features

**Co-op**
- Your offline Fort Hope is a co-op game: you host automatically, and Steam friends join with **Join Game** in their
  Steam friends list or through a Steam invite. Steam's peer-to-peer networking, so no port forwarding.
- Everyone follows the host into missions and across chapters, and joiners take over bot slots.
- Everyone plays with their **own deck**, keeps their **own rewards** (supply points, skull totems, unlocks,
  consumables land in their own profile) and can play **burn cards**, charged to their own profile.
- **5+ players** (`teamsize=5` on the host); a clean "Server full." when nobody else fits.
- Who may join: Steam friends by default, or anyone, or a list of Steam IDs. Kick, ban and lock.
- Everyone must run the same version; a mismatch is refused with a message that points to the Updates tab.

**In game**
- The **`~` window**: players, session, camera, flashlight, cheats, looks and add-ons, and updates, with key bindings
  and settings saved for you. The same things also work as chat commands (`/help`).
- **Manual flashlight** toggle (L), plus a wider, longer or brighter beam for your own view.
- **Third-person camera** for your own hero (`/thirdperson`, N), with free look while you stand still.
- **Change your look** with `/model`: other survivors' outfits and pieces, and add-on outfits. Others see it too if
  they have the add-on; otherwise they see your survivor.
- **Cheats** (host, opt-in, `/cheats on`): god mode, fly, noclip, spawn, horde control, slow motion and more, as a
  sandbox. Rewards still work. While they're on, every player can add supply points, skull totems and burn cards to
  their own save (never anyone else's).

**Add-ons**
- Drop `.pak` add-ons into `b4bcoop-addons`: textures, custom outfits and weapon looks (up to 512 add-ons and 512
  outfits). Added outfits are also in the game's customization screen, for every survivor. Nobody learns which
  add-ons you have; the host decides whether joiners may bring gameplay-changing ones.
- One **Looks & add-ons** tab in the `~` window: **Get more** finds free add-ons in a signed catalog and adds them
  (most are ready at once), **Wear** puts on an outfit or weapon look (yours, other survivors', NPC bodies),
  **Installed** switches add-ons on/off, sets the load order, updates and removes them.
- **Modkit** (separate download, `b4bcoop-modkit-<version>.zip`): turn a downloaded character (FBX, glTF/glb, VRM, OBJ,
  DAE, .blend) or a gun model into an outfit or weapon look, with hair physics, cloth for skirts, coats and capes,
  talking and blinking faces, and first-person arms. See [modkit/README.md](modkit/README.md).

**Updates and safety**
- **In-game updater** (`~` → Updates, since 0.7.0): checks GitHub, verifies the release signature, installs on the
  next start, and can go back; an update that doesn't start properly is reverted automatically.
- No third-party network traffic: the game's online services are blocked while the mod runs, and the game's own port
  only listens on your PC.

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
  that hooks game functions; `xinput1_3.dll` changes which program Steam's launcher starts (Windows). That is also
  what some malware does, so heuristic scanners sometimes flag them. There is no installer, nothing is downloaded,
  and the only network traffic is the co-op session over Steam. If you don't trust the zip, build it yourself from the
  source.
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
