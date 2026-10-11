# b4b-coop: commands & options

Everything b4b-coop lets you change, for the current release: the **`~` window** (the easy way), the same things as
chat commands, and `b4bcoop.ini`. You don't need any of it to play: install, press Play, and your Steam friends can
**Join Game** on you (see the README).

Contents: [The ~ window](#the--window) · [Chat commands](#how-to-use-chat-commands) ·
[Commands for everyone](#commands-for-everyone) · [Host-only commands](#host-only-commands) ·
[Cheats](#cheats-sandbox) · [Add-ons](#add-ons) · [b4bcoop.ini options](#b4bcoopini-options) ·
[Launch options & troubleshooting](#launch-options--troubleshooting)

## The ~ window

Press **`~`** (the key left of `1`) in game: a see-through window opens over the game with a tab for each topic.
Every chat command below has a button, box or slider there, with the same rules (a control the host alone may use is
greyed out on a client: hover it to see why; cheats need **Cheats on** first). Replies show in the log at the bottom
of the window, and also in your chat.

- While it is open, the mouse moves the window's own cursor and your hero doesn't move, shoot or react to keys
  (your flashlight and third-person keys too). `~` or `Esc` closes it.
- Settings you change here (camera, keys, flashlight, who may join, add-ons policy, the window's text size) apply at
  once and are saved to `b4bcoop.ini`: only the settings you touched. Editing `b4bcoop.ini` while the game runs updates the window.
- A key binding: click the key's button, then press the new key (`Esc` cancels, `Backspace` = no key). Mouse buttons
  4/5 and the middle button work too.

| Tab | What's in it | Chat equivalent |
|---|---|---|
| **Session** | Version and protocol, your Steam ID (Copy), Join a host by Steam ID, Host, Leave; your Steam friends in Back 4 Blood with **Join** (like Steam's Join Game) and **Invite** (while you host); "Show Join Game to Steam friends" (`presence`); host: who may join (Steam friends / anyone, always-allowed Steam IDs) | `/join`, `/host`, `/leave`; ini `presence`, `allow_joins`, `allow_steamids` |
| **Players** | Everyone in the game with ping and Steam ID; host: **Kick**, **Ban** per player, a message to everyone (**Say**), **Locked**, bots (game default/on/off), team size, **Ready everyone**, **Ready post-round vote**, **Restart mission**, the ban list with **Unban** / **Unban all** | `/players`, `/ping`, `/kick`, `/ban`, `/say`, `/lock`, `/unlock`, `/bots`, `/teamsize`, `/ready`, `/ready vote`, `/restart`, `/bans`, `/unban` |
| **Camera** | Third person on/off, start the game in third person, its key, sliders for distance, side, height and FOV (Ctrl+click a slider to type a number), Swap shoulder, Reset camera, aim correction, free look on/off | `/thirdperson ...`; ini `thirdperson*` |
| **Flashlight** | Your light (Toggle, On, Off; host: Automatic), its key, sticky mode; **Beam (your view)**: sliders for width, range and brightness, Reset beam | `/flashlight ...`; ini `flashlight_key`, `flashlight_sticky`, `flashlight_width`, `flashlight_range`, `flashlight_brightness` |
| **Cheats** | Host: **Cheats on**, a player picker (`me`, everyone, a player) for the buttons marked `*`, then every cheat: god, heal, revive, infinite ammo, copper, cards, fly, noclip, walk, teleport, free camera, size, horde, kill all ridden, freeze, director phases, spawn, game speed, win, lose, and your own save's supply points / skull totems / burn cards / unlock all (everyone, while the host has cheats on) | every `/cheats` command |
| **Models** | Your look now and your pick, the host's refusal when it said no, **Reset my look**; lists to wear with a search box (click a name): **Survivors** (per survivor: whole survivor, outfits, heads, torsos, legs), **NPC bodies** (Fort Hope NPCs, other survivors, cultists), **Add-on outfits** (by add-on); **Weapon looks** per weapon type (**Use**, which add-on it comes from, **Reset** for that weapon type); **Everyone's look** (every player and bot); host: **Change the look of** a player or bot (then click a look), their **Reset**, **Model swaps allowed** (greyed on a client, with the host's last announced state) | `/model`, `/model list ...`, `/model <name>`, `/model reset`, `/model <player> <name>\|reset`, `/models on\|off` |
| **Add-ons** | Your add-ons in load order: on/off, **Up**/**Down** (load order), cosmetic or gameplay (and what kind), "not loaded" with the reason, conflicts (who wins); click one for its details (author, description, id with Copy, outfits it adds); a banner when `addonlist.txt` differs from what is loaded (restart to apply); the folder path (Copy); "Load add-ons" (`addons`); host: who may join with add-ons (`addons_policy`; nobody sees other players' add-ons) | `/addons`, `/addons on\|off`, `/addons info`, `/addons policy`; ini `addons`, `addons_policy` |
| **Browse** | The add-on shop: **Get the add-on list** (free add-ons with a public license, with picture, author, license, size, what each adds or replaces), search and filter, **Add**, **Remove**, **Update**, **Undo**; click one for its details (license link, description). See [Browse (add-on shop)](#browse-add-on-shop) | ini `shop` (no chat command) |
| **Settings** | The window's text size and key, the flashlight and third-person keys; **Game start**: ask co-op or online every time, always b4bcoop co-op, always online (see [Co-op or online](#co-op-or-online-at-the-game-start)) | ini `overlay_scale`, `overlay_key`, `flashlight_key`, `thirdperson_key`, `launch` |
| **Updates** | Your version; **Check for updates** (the latest release: version, protocol, short notes); **Download and install on next start**; the host's version after a "same version" refusal (**Check** it); **Go back to** the previous version. See [Updates](#updates) | ini `updates` (no chat command) |
| **Help** | Your version, a box to run any chat command (without the `/`) | `/help` |

Buttons that change something for everyone or permanently (Ban, Restart, Leave, Win, Lose, Unlock all, Add supply
points, Unban all) need a second click within 3 seconds ("Sure?"). `overlay=0` in `b4bcoop.ini` turns the window off.

## How to use chat commands

1. Press **Enter** to open the chat box (Fort Hope and missions; loading and post-round screens take no input).
   On Steam Deck, open the on-screen keyboard with **STEAM + X**.
2. Type the command, starting with `/`, for example `/players`.
3. Press **Enter**.

- Commands are **never sent to other players**. They run on your own game only.
- Replies appear only in **your** chat, as lines from `coop`, like `[ALL] coop: kicked Alex`.
- Upper or lower case doesn't matter: `/Players` works.
- `//text` sends `/text` as a normal chat message. Note: normal chat messages don't reach other players in offline
  mode (the game's chat needs its online service), so this rarely matters.
- `/help` lists the commands. An unknown command replies `unknown command /xyz (/help)`.

## Commands for everyone

| Command | What it does | Example |
|---|---|---|
| `/help` | Shows your b4bcoop version and the commands | `/help` |
| `/players` | Lists everyone in the game, with numbers | `/players` |
| `/ping` | Your ping to the host | `/ping` |
| `/flashlight` | Turns your flashlight on or off | `/flashlight`, `/flashlight on` |
| `/thirdperson` | Over-the-shoulder camera for your own hero | `/thirdperson`, `/thirdperson off`, `/thirdperson distance 150` |
| `/join steam:<id>` | Joins a friend by their Steam ID | `/join steam:7656119XXXXXXXXXX` |
| `/leave` | Leaves the host's game, back to your own Fort Hope | `/leave` |
| `/host` | Starts hosting your Fort Hope (only needed with `host=0`) | `/host` |
| `/model <name>` | Changes how your survivor looks, this game session only | `/model karlee_elite_03`, `/model holly` |
| `/model list [survivor\|npc\|outfits\|weapons]` | The looks you can use (`outfits`, `weapons`: those your add-ons add) | `/model list`, `/model list walker` |
| `/model reset` | Back to your own look | `/model reset` |
| `/addons` | Lists your add-ons, on/off, and conflicts | `/addons` |
| `/addons on\|off <#>` | Switches an add-on on or off from the next game start | `/addons off 2` |
| `/addons info <#>` | Title, author, version, description, cosmetic or gameplay, id of an add-on | `/addons info 1` |

**`/help`**: the first line is your version, e.g. `b4bcoop 0.3.0 (protocol 1)`. The host also sees the host-only
commands; a client sees `(/kick /ban /lock ... are for the host)`.

**`/players`**: one line per player: `#0 Sam (you)`, `#1 Alex`, `#2 Holly [bot]`. The number is what host commands
like `/kick 1` use. The host also sees each player's ping and Steam ID (`#1 Alex 48ms steam:7656119...`).

**`/ping`**: `ping to host: 48 ms`. On the host: `you are the host (no round trip)`.

**`/flashlight`**: no word = toggle. Also `on`, `off`, `auto`, `status`.
- The same as the flashlight key (**L** by default, see `flashlight_key`).
- Your manual choice sticks: walking into a dark or bright area no longer switches it for you, until the next map.
- `/flashlight auto` (host only) hands your light back to the game's automatic switching right away. On a client
  it replies `auto: host only (a client's override lasts until the next map)`.

**`/thirdperson`**: no word = toggle. Also `on`, `off`, `status`, and the camera settings below.
- A third-person camera behind your own hero. **Aiming** (right mouse) switches to first person while you hold it,
  and back when you let go.
- Only your own view changes: the other players see nothing different, and nothing is sent to them. Host and clients
  can each use it; cheats aren't needed.
- It stays on until you type `/thirdperson` again: into the next chapter, back in Fort Hope, and in someone else's
  game. It isn't saved: after restarting the game you start in first person, unless `thirdperson=1` is in
  `b4bcoop.ini`.
- The **N** key toggles it too (see `thirdperson_key`).
- Moments where the game itself switches to a view from behind (healing, being grabbed or pounced, ...) are left to
  the game.
- There is no third-person crosshair: the normal centre-of-screen one is used, and shots land under it (see **Aim**
  below). Aim with right mouse for precise shots.
- **Free look**: standing still, move the mouse to swing the camera around your hero, all the way round to see its
  front (your outfit, a custom character). Your hero doesn't turn, and the other players don't see it move. As soon as
  you move, shoot, aim, reload, melee, use something, jump, crouch, sprint or switch weapons, your hero turns to face
  where the camera looks, and the camera stays where it is (it never swings around on its own). W then walks away from
  the camera, the first shot lands under the crosshair and aiming (right mouse) looks where the camera looked. The
  other players see your hero turn. From then on the mouse turns your hero again. Free look starts after a quarter of
  a second of standing still, so turning while you stop still turns your hero.
  Off with the **Camera** tab's checkbox or `thirdperson_freelook=0` (then standing still, the mouse turns your hero
  as before).
- **Picking things up** works as in first person: put the crosshair on a weapon, ammo, item or a card at a card
  shrine to see its prompt and card and press (or hold) F. (The game hides these in its own views from behind;
  b4bcoop turns that off in yours.) Doors, vendors, the war table and reviving work the same in both views.
- Camera settings (only your view, applied at once, kept for every map until the game quits; put them in
  `b4bcoop.ini` to keep them):

| Setting | Default | Values | What it does |
|---|---|---|---|
| `/thirdperson distance <n>` | `180` | `50`-`600` | How far behind your hero the camera is (the game's own is 300) |
| `/thirdperson side <n>` | `40` (right shoulder) | `-150`-`150`, `left`, `right`, `swap` | Over the shoulder: positive = right, negative = left. `left`/`right` pick a shoulder (40 if it was 0), `swap` switches shoulders, `0` = centred behind your hero |
| `/thirdperson height <n>` | `0` | `-100`-`150` | Raises (or lowers) the camera |
| `/thirdperson fov <n>` | the game's | `60`-`130`, `0` = the game's | Field of view in third person |
| `/thirdperson reset` | | | Back to these defaults |

- **Aim**: your shots come from your hero's eyes, not from the camera. With a side or height offset b4bcoop turns
  your hero's aim towards whatever is under the crosshair, so shots land there (also as a client: the host takes your
  hits as your game saw them). Something right beside your hero that only the camera sees past can still stop a
  shot. `thirdperson_aimfix=0` turns this off: shots then land `side`/`height` units beside the crosshair point, at
  every range, and pick-ups react to what is that far beside it. Aiming with right mouse is always exact.

**`/join steam:<id>`**: the fallback when **Join Game** in Steam doesn't work. Use it from your own Fort Hope. Like
Join Game, it keeps trying for 3 minutes if the host doesn't answer yet (for example while it loads a mission).
- `<id>` is the host's 17-digit Steam ID. The host finds it in Steam: click your account name at the top right →
  **Account details**.
- Joining by IP address (`/join 1.2.3.4`) works only with `host_ip=1` (advanced, see below). Without it the reply
  is `Joining by IP address is off. Join through Steam instead: ...`.

**`/model`**: wear another survivor's outfit (`/model walker_elite_07`), a whole survivor's look (`/model holly`),
single pieces (`/model holly_head_03`), a Fort Hope NPC (`/model vanessa`) or an outfit one of your add-ons adds
(`/model list outfits`), or put an add-on's model on your weapon (`/model list weapons`, e.g. `/model ak47` for your
AR02). The `~` window's **Models** tab has all of it as lists (click to wear). Everyone sees survivor outfits; NPC looks only players with b4bcoop; add-on
outfits and weapon looks only players with the same add-on (the others see your survivor, the normal weapon). Kept through map changes, death and respawn; nothing is saved. `/model` alone shows
what you wear. All names, examples and limits: `docs/commands-models.md`.

**`/leave`**: only while in someone else's game (else `you are not in someone else's session`).

**`/host`**: you host automatically by default, so you only need this after setting `host=0`. Works only from your
own offline Fort Hope while playing alone. Replies:
- `hosting: others can now /join you`: done, friends can join until you quit the game.
- `already hosting (N player(s) connected)`
- `you are in someone's session: /leave first`
- `go back to Fort Hope first`

**Every command here is also in the `~` window** ([above](#the--window)).

## Host-only commands

**Who is the host?** The player whose Fort Hope (and missions) everyone else is in. Everyone who joined them is a
client. If you're playing without having joined anyone, you count as the host too, so you can set things like
`/teamsize` before your friends arrive.

**On a client**, a host-only command does nothing and replies `/kick: host only (you are a client)`. Nothing is
sent to the host.

| Command | What it does | Example |
|---|---|---|
| `/kick <player>` | Removes a player from the game | `/kick 2`, `/kick Alex` |
| `/ban <player>` | Removes a player and keeps them out, also after restarts | `/ban 2` |
| `/unban <ban>` | Lifts a ban | `/unban Alex`, `/unban #0`, `/unban all` |
| `/bans` | Lists the bans, with numbers | `/bans` |
| `/lock` | No new players; those in the game now can still come back | `/lock` |
| `/unlock` | Anyone allowed may join again | `/unlock` |
| `/teamsize <N>` | Number of survivors, from the next map | `/teamsize 5` |
| `/bots on\|off\|default` | Bots in empty survivor slots or not, from the next map | `/bots off` |
| `/ready` | Readies everyone on the pre-mission screen | `/ready` |
| `/ready vote` | Readies everyone for the post-round vote | `/ready vote` |
| `/restart` | Restarts the current chapter (counts as a wipe) | `/restart` |
| `/say <message>` | A message every player sees in their chat | `/say back in 5 min` |
| `/model <player> <name\|reset>` | Changes a player's or a bot's look; everyone is told | `/model 2 doc_elite_03` |
| `/models on\|off` | Allows or stops model swaps for everyone (on by default) | `/models off` |
| `/addons policy [any\|cosmetic\|none\|match]` | Shows or sets (this session) which add-ons joiners may have | `/addons policy none` |

**`<player>`** is the number from `/players` (`2` or `#2`), or a name: exact (any case) or the start of a name, if
only one player matches. Bots and yourself can't be kicked or banned (`Holly is a bot`, `that's you`).

**`/kick`**: the player sees `You were kicked by the host.` and goes back to their own Fort Hope. They can join
again; use `/ban` or `/lock` to keep them out. `/kick` and `/ban` need you to be hosting (`not hosting a session`
otherwise).

**`/ban`**: the player sees `You were banned by the host.`. When they try again: `You are banned from this
session.`. Bans are by Steam ID and saved in `Gobi\Binaries\Win64\b4bcoop-bans.txt`, so they last until you
`/unban`. Deleting that file removes all bans.

**`/unban`**: takes a name, a `steam:<id>`, a number from `/bans` (`#0`), or `all`.

**`/lock`**: replies `session locked: no new players (current players can still rejoin)`. Anyone else trying to join
sees `The host locked the session.`. A player you kick while locked can't come back. The lock lasts until `/unlock`
or until you close the game.

**`/teamsize <N>`**: survivors per team, from the next map load (next mission, chapter or Fort Hope). The game's
normal size is 4. `5` is tested and works (5 players, or 4 players + a bot); up to `8` is accepted but untested.
Only the host's setting decides the team size (friends don't need it). Same as the `teamsize` ini option, for this game session only.
`/teamsize 4` goes back to normal. Numbers below 4 change nothing.

**`/bots`**: `off` = empty survivor slots stay empty (e.g. play a mission with just 2 heroes). `on` = fill them with
bots. `default` = the game decides. `/bots` alone shows the setting. Applies from the next map, for this game
session only.

**`/ready`**: presses Ready for every player on the pre-mission (loadout) screen, so nobody has to click.
`/ready vote` does the same for the post-round screen's vote. Needs you to be hosting.

**`/restart`**: only in a mission. Fails the mission on purpose, like a team wipe, and the game restarts the chapter
(or goes back to the last checkpoint) for everyone. It counts as a failed attempt, like a real wipe. If a wipe would
end your run, it refuses: `restart refused: failing now would end the run (GameOver)`.

**`/models off`**: resets every look that uses another survivor's outfit or an NPC, and refuses new ones (the player
sees `The host turned model swaps off (/models).`). Players can still wear their own survivor's outfits. `/models on`
allows swaps again. `/model <player> reset` puts one player or bot back to their own look.

**`/say`**: every player with b4bcoop sees `<your name>: [host] back in 5 min` in their chat, you too.

## Cheats (sandbox)

An opt-in sandbox for the **host**: god mode, flying, spawning ridden, forcing a horde, and so on. While the host has
cheats on, **every player** can also add supply points, skull totem points and burn cards to **their own save**
([Your own progression](#your-own-progression-permanent)). Everything is a chat command (type it in the game's chat
box; a message starting with `/` is never sent to the other players) and a control in the `~` window's Cheats tab.

**The rules:**
- **Off until the host turns them on** with `/cheats on`. `/cheats off` turns them off again, and they switch off by
  themselves when the game goes back to the menus.
- **Every map change clears the sandbox.** Cheats stay on between Fort Hope (camp) and missions, both ways, and on
  into the next chapter, but everything they were doing stops at the map change: god mode, fly, noclip, infinite ammo, game
  speed, freeze, size and the free camera (spawned ridden and director changes go with the old map). Everyone sees
  `[b4bcoop] cheats still on; effects reset`. Copper and cards already given stay, like any copper or card.
- **Host only**, except your own save. Every cheat command works only on the host (the player whose game the others
  joined, or your own offline game). Typed by someone who joined, it only replies `host only` and does nothing. The
  [own progression](#your-own-progression-permanent) commands work for everyone while the host has cheats on, and
  only ever change the save of the player who typed them.
- **Everyone is told.** Turning cheats on or off, and every cheat that affects another player or the whole game, shows
  a `[b4bcoop] host ...` line in everyone's chat (for example `[b4bcoop] host gave god mode to Mellon`).
- **Rewards and stats are never withheld.** Cheats change the running game, or your own save (`/supply`, `/skulls`,
  `/burncards`, `/unlockall`), never another player's save. A map played with cheats still gives every player their
  supply points, skull totem points, unlocks, stats and achievements as usual, so one stray cheat can't cost anyone
  their run's rewards.
- Cheats that change a hero (god, fly, infinite ammo, speed, freeze) last until the map changes; `/cheats off` undoes
  them.

**Naming a player:** `[player]` is a name from `/players`, its number (`#2`), `me` (the host, the default when left
out) or `all` (every hero, players and bots). All copies of the game on one Steam account share a name; use the
number then.

### Turning cheats on and off

| Command | What it does | Example |
|---|---|---|
| `/cheats on` | Turns cheats on for this game (they stay on into the mission and the next chapter; each map change resets their effects). Everyone sees `host enabled cheats`. | `/cheats on` |
| `/cheats off` | Turns them off and undoes god mode, infinite ammo, game speed, freeze, size and the free camera. | `/cheats off` |
| `/cheats` or `/cheats help` | Lists the cheat commands and what is on right now. Someone who joined sees the own-save commands and whether the host has cheats on. | `/cheats` |

### Survival

| Command | What it does | Example |
|---|---|---|
| `/god [player\|all] [on\|off]` | God mode: no damage and no death. Without `on`/`off` it toggles (`all` turns it on for everyone). Follows the player onto a new hero (a rescue, a bot take-over) until the map changes. | `/god all`, `/god Mellon off` |
| `/heal [player\|all]` | Full health, trauma included (the lost part of the health bar). Downed heroes need `/revive`. | `/heal all` |
| `/revive [player\|all]` | Gets downed (incapacitated) heroes back up with half their health. Default: everyone. Dead heroes still come back through the rescue closets as usual. | `/revive` |
| `/ammo infinite\|off` | Infinite reserve ammo on every weapon, for everyone (weapons picked up later too). You still reload. | `/ammo infinite` |
| `/copper <+N\|-N> [player\|all]` | Gives (or takes) copper. Default: every hero. Up to 100000 at once. | `/copper +500`, `/copper -100 #2` |
| `/card <card name> [player\|all]` | Adds a gameplay card to a hero's active cards during a mission, as if drawn: it stays for the rest of the run. Name as shown in the game (`amped up`) or its internal row name; spaces and case don't matter. | `/card amped up`, `/card steady aim all` |
| `/card list [filter]` | Lists card names matching the filter (up to 40). | `/card list aim` |

### Movement and camera

| Command | What it does | Example |
|---|---|---|
| `/fly [player\|all]` | Fly mode (move freely up and down with the camera). Players only, not bots. | `/fly`, `/fly #2` |
| `/noclip` | Fly through walls. Your own hero only: another player's game would still stop them at walls. | `/noclip` |
| `/walk [player\|all]` | Back to normal (ends fly and noclip). | `/walk all` |
| `/tp <player\|saferoom\|start>` | Teleports you to a player, to this map's end saferoom, or back to its start saferoom. Entering the end saferoom with the whole team can end the chapter, as walking in would. | `/tp saferoom`, `/tp Mellon` |
| `/tp <player\|all> <player\|me\|saferoom\|start>` | Teleports a player (or everyone) somewhere; each gets their own spot. | `/tp all me`, `/tp #2 start` |
| `/freecam` | A free-flying camera for your own view only; your hero stays where it is. **Press F8 to come back** (the chat doesn't work while it's on). | `/freecam` |
| `/size <0.25-4>` | Your own hero's size (1 = normal). Only you see it. | `/size 2` |

### World and director

These act on the whole mission (only during a mission).

| Command | What it does | Example |
|---|---|---|
| `/horde` | Triggers a horde now. | `/horde` |
| `/director calm\|build\|peak\|fade\|recover` | Forces the AI director's pacing phase: calm (no pressure), build (build-up), peak, peak fade, recover. | `/director peak` |
| `/spawn <type> [1-10]` | Spawns ridden in front of you. Types: `tallboy crusher bruiser reeker exploder retch stinger stalker hocker hag snitcher breaker ogre common`. | `/spawn crusher 2` |
| `/killall` | Kills every ridden in the map (common ridden, specials, mutations). Never players, bots or allies. | `/killall` |
| `/freeze` | Freezes the world except the players (ridden and bots stop). Again to resume. | `/freeze` |
| `/slomo <0.1-5>` | Game speed for everyone (1 = normal). | `/slomo 0.5` |
| `/win` | Ends the mission as a success (the normal end-of-mission flow). | `/win` |
| `/lose` | Ends the mission as a failure (on most difficulties the mission restarts). | `/lose` |

### Your own progression (permanent)

These change **your own save, permanently**. They never touch another player's save, and nothing is sent to the host
or the other players.

**Who:** everyone, while the game they are in has cheats on: the host after `/cheats on`, and a player who joined
while that host has cheats on (`/cheats off` stops them again). In your own Fort Hope you are the host, so it is your
own `/cheats on`. Joining someone else's game turns your own cheats off: their host decides there. A host on an older
b4bcoop never tells the others that cheats are on, so for them these stay off.

| Command | What it does | Example |
|---|---|---|
| `/supply <+N>` | Adds supply points to your save (1-100000). | `/supply +1000` |
| `/skulls <+N>` | Adds skull totem points to your save (1-100000). | `/skulls +200` |
| `/burncards <card> [count]` | Adds burn cards to your save (1-99, default 1). Name as shown in the game (`windfall`, `hell can wait`) or its internal row name; spaces and case don't matter. Play them from the start saferoom's burn card screen as usual; playing one uses it up. DLC burn cards aren't offered. | `/burncards windfall 3` |
| `/burncards list [filter]` | Lists the burn cards you can add. | `/burncards list dusty` |
| `/unlockall` | Unlocks every item in the supply lines you don't have yet (not burn cards, which are consumables, and not DLC items). In Fort Hope. `/unlockall check` only counts them. | `/unlockall check`, `/unlockall` |

In the `~` window these are the Cheats tab's **Your own save** section (supply points, skull totem points, a burn card
picker with a count, Unlock all); in someone else's game it is at the top of the tab.

Before the first own-save change of a game session, your save is copied next to itself as
`PlayerProfileSettings-b4bcoop-backup-<date>-<time>.sav` (and `.json`), in
`%LOCALAPPDATA%\Back4Blood\Steam\Saved\SaveGames`; the reply shows the full path. To go back, quit the game and
copy the backup over `PlayerProfileSettings.sav`. If the backup can't be made, nothing is changed.

### Not included, and why

- **Game's own developer cheats**: `God`, `GiveUnlock`, `GiveSupplyPoints`, `Heal` and the other built-in cheat
  commands are empty in the released game, and it never gives a player the engine's cheat manager. The commands above
  are rebuilt from the game's own functions instead.
- **Noclip and size for other players**: collision and size aren't shared over the network, so their own game would
  keep stopping them at walls or at their normal size. Fly works for them.
- **Reviving dead heroes**: dead heroes are rescued from the rescue closets as usual; `/revive` gets downed heroes up.
- **Spawning bosses** (the Abomination, sleepers): they are scripted into their maps and don't work spawned anywhere.

## Add-ons

Add-ons change how the game looks (textures, models, UI), Left 4 Dead style: drop a file in a folder, restart (or get
free ones from the `~` window's **Browse** tab, see [Browse (add-on shop)](#browse-add-on-shop)). Add-ons are **only on your PC**: other players don't need them and don't see them. If you have
a survivor skin add-on, you see it on every survivor wearing that outfit; players without it see the normal outfit.
Some add-ons **add** outfits instead of replacing one: they are in the game's customization screen for every survivor
(kept for that survivor; your profile keeps its previous outfit), and `/model list outfits` lists them, `/model <name>`
puts one on for the session; players with the same add-on see it on you, the others your survivor. A host with
`addons_policy=none` refuses them.

**Install:** an add-on is one `.pak` file. Put it in the `b4bcoop-addons` folder in the game folder (next to
`Back4Blood.exe`; create the folder if it isn't there). An add-on zip already contains that folder: extract it into
the game folder. Restart the game. **Remove:** delete the `.pak` file.

**On/off and load order:** the game writes `b4bcoop-addons\addonlist.txt`, one line per add-on:
```
holly_magenta.pak=1
holly_green.pak=0
```
`=1` on, `=0` off. New add-ons are added at the bottom, switched on. The game loads them top to bottom: when two
add-ons change the same file, **the one further down wins**. Move lines to change that. The `~` window's **Add-ons**
tab (checkbox, **Up**/**Down**) and `/addons on|off` edit this file for you; editing it by hand while the game runs is
picked up too. Switching an add-on **on** loads it right away when that is safe (the same rules as the Browse tab's
**Add**, below), else at the next start; switching off and changing the load order apply at the next start.

**`/addons`**: the list with numbers (in `addonlist.txt` order), the state of each (`on`, `off`, `on after restart`, `off after restart`,
`on, NOT LOADED`) and conflicts:
```
2 add-on(s), load order (a later one wins):
1. Holly magenta portrait 1.0 [on] holly_magenta.pak
2. Holly green portrait 1.0 [on] holly_green.pak
conflict: holly_green.pak overrides holly_magenta.pak (2 file(s))
```
`<#>` in `/addons on|off|info` is that number, the file name or the title (or a unique part of it).

**Cosmetic or gameplay:** b4bcoop looks at the files in every add-on and sorts it into one of two kinds (what the
add-on says about itself doesn't count):
- **cosmetic**: textures, materials, character, weapon and prop models (characters on the game's own skeleton),
  animations, cloth, sounds, UI, effects. Changes only what you see and hear.
- **gameplay**: anything else, e.g. physics assets, data tables, blueprints, skeletons, maps, config files. Could
  change how the game plays, so a host can refuse these (`addons_policy=cosmetic`).

`/addons` shows the kind of each add-on, `/addons info <#>` the first file that made it "gameplay".

**Joining someone with add-ons:** which add-ons you have **stays on your PC**: your game sends nobody a list, names,
ids or counts, and nobody can see what you run (not even the host). Instead the host **announces** which add-ons it
allows (`addons_policy`), and **your game checks your own add-ons** before joining:

| `addons_policy=` | Who may join |
|---|---|
| `any` (default) | Everyone, whatever they run |
| `cosmetic` | Everyone with cosmetic add-ons only (no gameplay add-ons) |
| `none` | Only players with no add-ons at all |
| `match` | Cosmetic add-ons are free; gameplay add-ons must be exactly the host's. The host's own gameplay add-ons' short ids are shown to people joining (that's how they can match them); the other policies reveal nothing about the host's add-ons |

If yours don't fit, your game doesn't join and tells only you, e.g. `Could not join: This host allows only cosmetic
add-ons; turn off Walker data table test (~ window, tab Add-ons, or /addons off <#>) and restart the game.` (or `This
host allows no add-ons; ...`, `This host requires the same gameplay add-ons as theirs. Turn off: ... Turn on or
install: ...`). The host sees nothing about it. Add-ons apply at game start, so switch them off and restart before
joining again. The host's own add-ons don't matter under `cosmetic`, `any` and `none`.

- Steam **Join Game** / invites: the check happens before connecting.
- Joining by IP address (`host_ip=1`): your game doesn't know the rule yet, so the host's first answer tells it; if
  your add-ons fit, your game joins again at once by itself (you may see "Unable to join" flash by).
- A host on an **older b4bcoop** (0.7.0 and before) doesn't announce its rule: your game assumes `cosmetic` (their
  default). Over an IP address an older host doesn't check you at all.
- A player on an older b4bcoop (0.7.0 and before) still sends its add-on list when joining; a newer host checks it
  as before but never shows or logs it. Update to stop sending it.

**`/addons policy <x>`** (host) changes the policy until the host quits (for new joiners: players already in the
session keep playing, also after the next map change); `addons_policy=` in `b4bcoop.ini` keeps it
(the `~` window's Add-ons tab sets both). There is no list of other players' add-ons (`/addons players` is gone).

Messages (in your chat after the game starts):
- `Add-on conflict: "B" overrides "A". /addons`: both change the same files; B wins (it is further down the list).
  Fine if that's what you want; otherwise switch one off or reorder `addonlist.txt`.
- `Add-on "A" is installed twice (a.pak and a-any.pak): switch one off.`: the same add-on under two file names (e.g.
  your own copy and the one from the Browse tab). Switch one off or delete one of the files.
- `Add-ons a.pak and b.pak both add outfit x: b.pak's is used. Switch one off.`: two add-ons bring an outfit (or a
  weapon look) of the same name; `/model x` wears the one further down the list.
- `Add-ons "A" and "B" mix parts of one asset (may crash): switch one off.`: each add-on replaces a different part of
  the same asset, which can crash the game. Switch one off.
- `N add-on(s) could not be loaded`: `/addons info <#>` says why: `damaged ... download it again`, `not a Back 4
  Blood add-on pak` (made for another game, or not with b4bcoop's tool), `rename it ... to plain letters` (file or
  folder name with accents or other special characters).
- `Add-ons are off: unsupported game build`: your Back 4 Blood version isn't the one this b4bcoop supports.

Only install add-ons from people you trust. Making add-ons: the mod maker's kit (`b4bcoop-modkit-<version>.zip` from
the releases, `modkit/` in the source repository); players don't need it.

## Browse (add-on shop)

The **Browse** tab of the `~` window lists free add-ons from the b4bcoop add-on shop
(github.com/actuallydan/back4blood-shop): outfits, weapon looks and replacements that their makers released under a
public license (CC0, CC-BY, MIT, ...). Nothing is downloaded until you click, and nobody else learns which add-ons
you have.

1. **Get the add-on list** downloads the list (and the pictures). It carries the add-on shop's signature: a list
   that isn't signed with it is not shown. **Refresh the list** gets it again. Search box and filter: all, new looks,
   replacements, the ones in your add-ons folder.
2. **Add** downloads the add-on, checks its size and SHA-256 against the signed list (a damaged or changed file is
   deleted: "Nothing was changed"), and puts it into your add-ons folder as `<name>.pak`, switched on, last in the
   load order.
   - **New looks** (added outfits, weapon looks) are ready at once, in Fort Hope, in the middle of a mission and in
     someone else's session: wear them in the **Models** tab, the game's customization screen (also if you opened it
     before) or with `/model <name>`. Players with the same add-on see it on you, the others your survivor.
   - **Replacements** (an add-on that changes something of the game, e.g. a survivor's textures) are ready at once too
     when the game isn't using what they replace right now; otherwise they say "applies after a restart (it replaces
     an asset the game is using now: ...)" and load the next time the game starts.
   - In someone else's session the host's `addons_policy` decides as it would when you join: under `none` the add-on
     waits for the next start ("this host allows no add-ons"; with it on you can't join that host again).
   - Gameplay add-ons and replacements of files the game reads only when it starts (sound banks, translations,
     config) always wait for the next start.
3. **Remove** (click twice) switches the add-on off at once and deletes its file the next time the game starts.
   **Undo** takes that back until then.
4. **Update** appears when the list has a newer file than yours. It is downloaded now and replaces the old file at
   the next start. If nothing of the old version is in use right now (e.g. nobody wears that outfit), the new version
   is also **ready at once** ("Updated and ready now"); otherwise it says why it waits (e.g. you are wearing it) and
   **Undo** cancels it until the restart.
5. **Installed (as batman.pak)** instead of **Add**: you already have that add-on under another file name (the same
   content, or an add-on that adds the same outfit or weapon look), so it isn't added twice. The **Add-ons** tab
   switches it on/off.

Added add-ons are ordinary add-ons: the **Add-ons** tab switches them on/off and sets the load order, and the host's
`addons_policy` treats them like any other. A row shows "needs b4bcoop X or newer" when the add-on needs a newer
b4bcoop (the **Updates** tab gets it). Files: the add-on in `b4bcoop-addons`, downloads and pending changes in the
hidden `b4bcoop-addons\.shop` folder. Getting the list and downloading are the only times the tab contacts anything
(GitHub, only while that request runs). `shop=0` in `b4bcoop.ini` hides the tab.

## Updates

The **Updates** tab of the `~` window updates b4bcoop from inside the game. Nothing happens until you click, and
nothing checks in the background.

1. **Check for updates** asks GitHub for the latest b4bcoop release and shows its version, its protocol and the first
   lines of its notes.
   - "Same protocol as yours": after updating you can still play with friends who haven't updated.
   - "Protocol N (yours: M)": after updating you can only play with friends who have updated too.
2. **Download and install on next start** downloads the release zip, checks its size, its SHA-256 and the b4bcoop
   release signature, and only then puts the new files in place. If any check fails, nothing changes and the tab says
   why. The game you're playing keeps the version it started with.
3. **Restart the game** to run the new version. (b4bcoop can't swap itself out while the game runs.)

What an update changes: `X3DAudio1_7.dll`, `xinput1_3.dll` and the `b4bcoop-*.txt` files. It never touches
`b4bcoop.ini`, your add-ons, `b4bcoop-bans.txt` or the logs. The files you had before go to the
`b4bcoop-update\backup` folder next to `Back4Blood.exe`.

- **Go back to ... on next start** (under "Previous version", with the backup's version) puts the backup back. Click twice.
- If a new version fails to start twice in a row (the game crashes before you've played about 15 seconds), the third
  start puts the previous version back by itself and runs without b4bcoop that one time. The tab then says
  `<new> did not start properly 2 times, so <old> was put back`.
- A host with another b4bcoop version refused you (`Everyone needs the same version`): the tab shows the host's
  version with a **Check** button, which offers exactly that version.
- The check and the download (and the Browse tab's list and downloads) are the only time b4bcoop contacts anything besides Steam: `api.github.com`,
  `github.com` and GitHub's download server, and only while that request runs. GitHub allows 60 checks an hour per
  internet address; the tab tells you when that's used up.
- Releases from before the updater (0.6.1 and older) can't be installed from the tab; it links the release page
  instead.
- `updates=0` in `b4bcoop.ini` hides the tab.
- Updating by hand still works: extract the new zip over the old files, like the first install.

## b4bcoop.ini options

**Where:** `Gobi\Binaries\Win64\b4bcoop.ini` in the game folder (Steam → right-click **Back 4 Blood** → **Manage** →
**Browse local files**). The zip puts it there.

**How to edit:** open it in a text editor (Notepad on Windows, Kate/KWrite on Steam Deck Desktop Mode). Every setting
is off until you remove the `;` at the start of its line. Save: the game picks the change up within about two seconds,
also while you play, and says so in your chat (`b4bcoop.ini: applied thirdperson_distance`).

```ini
; before: off
;teamsize=5
; after: on
teamsize=5
```

Rules:
- One setting per line, `name=value`, the name at the very start of the line, lowercase, no space before `=`.
- A line starting with `;` or `#` is ignored.
- No `b4bcoop.ini` at all = all defaults.
- **Applied while the game runs:** `thirdperson`, `thirdperson_key`, `thirdperson_distance`, `thirdperson_side`,
  `thirdperson_height`, `thirdperson_fov`, `thirdperson_aimfix`, `thirdperson_freelook`,
  `flashlight_key`, `flashlight_sticky`,
  `flashlight_width`, `flashlight_range`, `flashlight_brightness`,
  `allow_joins`, `allow_steamids`, `presence`, `teamsize` (from the next map), `overlay`, `overlay_key`,
  `overlay_scale`, `addons_policy`, `updates`, `launch` (read at the next game start). Only the lines you changed count: an edit doesn't undo what you set with a chat command this
  session. Deleting or commenting out a line = back to its default.
- **Need a game restart** (chat: `b4bcoop.ini: host_ip changed; restart the game for that`): `auto_signin`, `host`, `join`,
  `host_ip`, `steam_p2p`, `presence_addr`, `netguard`, `netguard_eos`, `netguard_allow`.
- **Updating b4bcoop** (extracting a new zip) replaces `b4bcoop.ini` with a fresh one: note your changes first.
  Bans (`b4bcoop-bans.txt`) are kept.

### Common options

| Option | Default | Values | What it does |
|---|---|---|---|
| `host` | `1` | `0`, `1` | `0`: don't host, your offline game stays private |
| `teamsize` | game's 4 | `5` (up to `8`) | Host: survivors per team |
| `flashlight_key` | `L` | a letter, a digit, a key code, or `off` | The flashlight toggle key |
| `flashlight_sticky` | `1` | `0`, `1` | Host: a manual flashlight choice stays until the next map |
| `flashlight_width` | `100` | `25`-`300` | Your flashlight's cone width, percent of the game's (your screen only) |
| `flashlight_range` | `100` | `25`-`400` | How far your flashlight reaches, percent of the game's (your screen only) |
| `flashlight_brightness` | `100` | `0`-`500` | Your flashlight's brightness, percent of the game's (your screen only) |
| `thirdperson` | `0` | `0`, `1` | `1`: start in third person (`/thirdperson`) |
| `thirdperson_key` | `N` | a letter, a digit, a key code, or `off` | The third-person toggle key |
| `thirdperson_distance` | `180` | `50`-`600` | Third-person camera distance (`/thirdperson distance`) |
| `thirdperson_side` | `40` | `-150`-`150` | Over-the-shoulder offset, negative = left (`/thirdperson side`) |
| `thirdperson_height` | `0` | `-100`-`150` | Camera height offset (`/thirdperson height`) |
| `thirdperson_fov` | `0` (game's) | `60`-`130` | Third-person field of view (`/thirdperson fov`) |
| `thirdperson_aimfix` | `1` | `0`, `1` | `0`: no aim correction, shots land beside the crosshair by the camera offset |
| `thirdperson_freelook` | `1` | `0`, `1` | `0`: no free look; standing still, the mouse turns your hero as before |
| `overlay` | `1` | `0`, `1` | `0`: no `~` window |
| `overlay_key` | `~` | a letter, a digit or a key code | The key that opens the `~` window |
| `overlay_scale` | `1.25` | `0.5`-`4` | Size of the `~` window's text |
| `allow_joins` | `friends` | `friends`, `anyone` | Host: who may join |
| `allow_steamids` | none | Steam IDs | Host: these players may always join |
| `presence` | `1` | `0`, `1` | `0`: friends don't see **Join Game** on you |
| `netguard` | `block` | `block`, `log`, `off` | Blocks the game's online services while you play |
| `join` | none | `steam:<id>` | Always join this host automatically |
| `host_ip` | `0` | `0`, `1` | **Advanced**: host and join by IP address |
| `auto_signin` | `1` | `0`, `1` | `0`: the game's own Sign in step and Online/Offline question (co-op starts skip them by default) |
| `launch` | `ask` | `ask`, `coop`, `online` | At the game start: ask co-op or online, or always one of them (see [Co-op or online](#co-op-or-online-at-the-game-start)) |

**`host`**: you host by default: whenever you're in your offline Fort Hope, your Steam friends can join. `host=0`
keeps your game private; `/host` still turns hosting on for one game session. Setting `join=` also turns hosting
off, unless you add `host=1`.
```ini
host=0
```

**`teamsize`**: `5` = 5 survivors (tested: 5 players, or fewer players + bots; alone you get 4 bots). Up to `8` is
accepted but untested. Values of 4 or less change nothing. Only the host needs it: friends joining with no
`teamsize` see all 5 survivors (HUD, post-round lineup), and a friend who has it set while the host doesn't just
plays in a normal 4-survivor game. More players than survivor slots get `Server full.`. The character-select list
at the start of a mission shows 4 names; the 5th player still gets a survivor.
```ini
teamsize=5
```

**`flashlight_key`**: works only while the game window is in front.
- A letter or digit: `flashlight_key=F`.
- A Windows key code, written `0x..`: e.g. `0x70` = F1, `0x71` = F2 ... `0x7B` = F12.
- `off` turns the key off (the `/flashlight` command still works). Note: `flashlight_key=0` binds the **0** key; it
  doesn't turn the key off.
- Steam Deck / controller: map a button to the **L** key in Steam Input (controller settings).
```ini
flashlight_key=F
```

**`flashlight_sticky`**: host setting, applies to every player in the host's game. `1` (default): once someone
switches their light by hand, dark or bright areas no longer switch it back, until the next map (or `/flashlight auto`
on the host). `0`: the game's automatic switching always wins.

**`flashlight_width`** / **`flashlight_range`** / **`flashlight_brightness`**: your own flashlight beam, in percent of
the game's (100 = unchanged), for very dark places. Easiest in the `~` window's **Flashlight** tab (sliders, **Reset
beam**). They apply at once, in first and third person, as host or as a joined player, and only on your screen: the
others see your light as the game draws it, and you see theirs that way. The cone stops at 160 degrees (about 175%
of the game's 90-degree first-person beam). A wider beam spreads the same light over more area: raise the brightness with
it to keep it as bright.
```ini
flashlight_width=180
flashlight_brightness=150
```

**`thirdperson`** / **`thirdperson_key`**: your own camera, like `/thirdperson`. `thirdperson=1` starts every game
in third person (`/thirdperson` still switches it). `thirdperson_key` (**N** by default) takes the same values as
`flashlight_key`; `off` turns the key off.
```ini
thirdperson=1
thirdperson_key=0x74
```
(`0x74` = F5.)

**`thirdperson_distance`** / **`thirdperson_side`** / **`thirdperson_height`** / **`thirdperson_fov`**: the camera
settings of `/thirdperson distance|side|height|fov`, from the start of every game. A closer camera over the right
shoulder with a wider view:
```ini
thirdperson_distance=150
thirdperson_side=40
thirdperson_fov=100
```

**`thirdperson_freelook`**: free look around your hero while standing still in third person (on by default); when
you move, shoot, aim or use something, your hero turns to where the camera looks. Off:
```ini
thirdperson_freelook=0
```

**`allow_joins`**: host only. `friends` (default): only your Steam friends can join. `anyone`: anyone who can reach
you, friends or not. Someone refused shows up in your chat: `Refused a join from Alex (steam:7656119...): not on the
host's Steam friends list. To let them in: allow_steamids=7656119... in b4bcoop.ini.`
```ini
allow_joins=anyone
```

**`allow_steamids`**: host only. Lets specific people in who aren't your Steam friends (e.g. a friend of a friend),
whatever `allow_joins` says. 17-digit Steam IDs, separated by commas; `steam:` in front is fine. Up to 32. The easiest
way to get an ID: the "Refused a join" line above.
```ini
allow_steamids=7656119XXXXXXXXXX,7656119YYYYYYYYYY
```

**`presence`**: `0` = your friends don't get **Join Game** on you in Steam. They can still join with
`/join steam:<your id>` (if your join rules let them in).

**`netguard`**: `block` (default) blocks the game's attempts to reach Epic / WB / Turtle Rock servers while you play
co-op; Steam and the co-op connection are not affected. `log` only writes what it would block to the log. `off` turns
it off. Try `netguard=off` only if something won't start or connect, and tell us.

**`join`**: optional; **Join Game** in Steam is the normal way to join. With `join=steam:<host's Steam ID>` your game
joins that host by itself whenever you're alone in your offline Fort Hope (after the sign-in), and
keeps trying until it gets in (a few seconds after a failed attempt, then every 15 seconds). Several hosts can be listed with commas; they're tried in turn. It
turns hosting off (unless `host=1`). A Steam **Join Game** click overrides it.
```ini
join=steam:7656119XXXXXXXXXX
```

**`host_ip`**: **advanced, most players never need this.** Hosts and joins by IP address instead of through Steam.
- The host needs port forwarding (UDP 7777) on their router, and Windows shows a Firewall prompt.
- Everyone in the game sets `host_ip=1`; joiners use `/join <host's IP>` (or `join=<IP>` in the ini). Port 7777 is
  assumed; another port: `/join 1.2.3.4:7778`.
- Your game is then reachable from the network, not only through Steam. Steam joins keep working.
- Without it, IP joins are refused with `Joining by IP address is off...`, and an IP-only host's **Join Game** says
  `That host only takes joins by IP address, which are off here (host_ip=0).`

### Rarely needed

| Option | Default | What it does |
|---|---|---|
| `steam_p2p` | `1` | `0`: no joins through Steam at all (then only `host_ip=1` joins work) |
| `outfits_screen` | `1` | `0`: add-on outfits are not offered in the game's customization screen (only `/model`); restart to apply |
| `presence_addr` | your LAN address | With `host_ip=1`: the address friends' **Join Game** connects to, e.g. `presence_addr=203.0.113.5:7777` (your public IP) |
| `netguard_eos` | `1` | `0`: don't switch off the Epic Online Services network layer (troubleshooting only) |
| `netguard_allow` | none | With `netguard=block`: host names to let through anyway, comma-separated, `*.example.com` for a whole domain (troubleshooting only) |
| `addons` | `1` | `0`: load no add-ons at all (troubleshooting) |
| `addons_dir` | `<game>\b4bcoop-addons` | Another add-ons folder, a full Windows path, e.g. `addons_dir=D:\b4b-addons` |
| `addons_policy` | `cosmetic` | Host: which add-ons joiners may have: `cosmetic`, `any`, `none`, `match` (see Add-ons; joiners check themselves, `match` shows them your gameplay add-ons' ids) |
| `updates` | `1` | `0`: hide the `~` window's Updates tab (see [Updates](#updates)) |
| `shop` | `1` | `0`: hide the `~` window's Browse tab (the add-on shop, see [Browse](#browse-add-on-shop)) |

## Launch options & troubleshooting

### Co-op or online at the game start

Every Steam **Play** first asks how you want to play:
- **b4bcoop co-op**: the mod as usual (offline mode, Steam friends join you).
- **Online**: the official online game with Easy Anti-Cheat. Before Easy Anti-Cheat starts, b4bcoop moves its game
  file `Gobi\Binaries\Win64\X3DAudio1_7.dll` (and an old `dwmapi.dll`, if there) into the folder `b4bcoop-online`
  next to `Back4Blood.exe`, so the online game can't load any b4bcoop code. When you pick co-op the next time, it
  moves back. `b4bcoop-online\README.txt` says the same while it is there. Before each online start the offline save
  is copied to `PlayerProfileSettings-b4bcoop-before-online-<date>-<time>.sav` (and `.json`) in
  `%LOCALAPPDATA%\Back4Blood\Steam\Saved\SaveGames`, the 5 newest kept: to restore one, quit the game and copy it
  over `PlayerProfileSettings.sav`.
- With no input at all (no key, mouse or controller) it picks **b4bcoop co-op** after 15 seconds (the countdown
  shows in the question); any input stops the countdown.
- Closing the question (X, Esc, controller **B**) starts nothing.
- **Remember my choice** writes `launch=coop` or `launch=online` into `b4bcoop.ini` and skips the question from then
  on. To be asked again: hold **Shift** while the game starts (from pressing Play until the question shows), or `~`
  window → **Settings** → **Game start** → "Ask every time" (in co-op), or `launch=ask` / delete the line.
- Controller: **A** co-op, **Y** online, **X** remember, **B** close.
- A Steam **Join Game** or invite from a b4bcoop friend always starts co-op, whatever you remembered.
- Linux/Steam Deck: the first start after installing has no question (co-op, as before). It sets a Wine setting for
  `Back4Blood.exe` in the game's Proton prefix (`xinput1_3=native,builtin`) so that the launcher file loads; from the
  next start on the question appears, and co-op starts skip the Easy Anti-Cheat launcher like on Windows.
- A co-op start skips the game's own sign-in: b4bcoop presses Sign in and answers the Online/Offline question with
  Offline before it appears, so you go straight from the title screen to Fort Hope. `auto_signin=0` shows the game's
  sign-in again; picking **Online** there signs you in **Offline** anyway (b4bcoop never takes a game online) and
  your chat says how to play online.

Launch options (Steam → right-click **Back 4 Blood** → **Properties** → **Launch Options**) choose without asking:

| Launch option | Start |
|---|---|
| `-b4bcoop=off` (or `-b4bcoop=online`) | Online, b4bcoop switched off (the same as picking **Online**) |
| `-b4bcoop=coop` | b4bcoop co-op |
| `-b4bcoop=ask` | Ask, even with a remembered choice |

If a game ever starts with `-b4bcoop=off` but with b4bcoop loaded (Linux/Steam Deck before that first co-op start, or
the `xinput1_3.dll` next to `Back4Blood.exe` missing), b4bcoop closes it before it starts, with a message, rather than
let it go online; start it again.
`Gobi\Binaries\Win64\b4bcoop-launcher.log` shows each start's choice (`choice: online (the prompt)`, `switched off:
...`, `switched on: ...`).

### The log, and reporting a problem

- The log is `Gobi\Binaries\Win64\b4bcoop-<date>-<time>-<number>.log` in the game folder. Every game start writes a new one (the last 20 are kept): take
  the **newest**. Its first lines show the version, e.g. `b4bcoop 0.3.0 (protocol 1)`.
- No new log after starting the game = the mod didn't load (check that all files from the zip are in place).
- If the mod doesn't load or the start question doesn't appear, also `Gobi\Binaries\Win64\b4bcoop-launcher.log` (if it
  exists).
- A line `unsupported game build (signature mismatch)` means your Back 4 Blood version isn't the one this b4bcoop
  release supports; the mod then does nothing.

To report a problem, [open an issue](https://github.com/actuallydan/b4b-coop/issues) with:
1. Your b4bcoop version, and Windows / Linux / Steam Deck.
2. What you did and what happened (roughly when, and any message you saw).
3. The newest log from **your** PC, and if it's about joining, the host's and the joiner's logs from that session.

The logs contain your Steam name and Steam ID and those of the players you played with.

### Messages you may see

When the host refuses a join, the game's popup says why (title "COULD NOT JOIN" with the message below), and the
same reason appears in your chat as `Could not join: ...` once the popup is closed. A refusal that retrying can't fix
(another version, add-ons, a ban) stops the join; "Server full." and a locked session are tried again after a minute.

| Message | Meaning | What to do |
|---|---|---|
| `Host runs b4bcoop X (protocol N); you have Y (protocol M). Everyone needs the same version.` | You and the host have different b4bcoop versions | Press `~`, tab **Updates**: **Check** the host's version and install it (or everyone installs the latest release). Both versions are in the message; the host sees `<name> could not join: they have ...` |
| `This host allows only cosmetic add-ons; turn off ...` (or `allows no add-ons`, `requires the same gameplay add-ons`) | Your game checked your add-ons against the host's `addons_policy`: some don't fit. Only you see this | `/addons off <#>` (or the `~` Add-ons tab) for the ones named, restart the game, join again; or the host sets `/addons policy any` |
| `Server full.` | Every survivor slot is taken by a player (bot slots count as free) | Wait for a free slot, or the host sets `teamsize=5` |
| `This host only accepts their Steam friends. ...` | You're not on the host's Steam friends list | Become Steam friends, or the host adds you with `allow_steamids=` |
| `Could not reach the host over Steam. Is it still hosting? ...` | The host quit, or refused you (not friends) | Check the host is in Fort Hope or a mission; see the line above |
| `You are banned from this session.` | The host banned you | Ask the host to `/unban` you |
| `The host locked the session.` | The host used `/lock` | Ask the host to `/unlock` |
| `You were kicked by the host.` / `You were banned by the host.` | The host removed you | You're back in your own Fort Hope |
| `Joining by IP address is off. ...` | You tried an IP join without `host_ip=1` | Use **Join Game** in Steam or `/join steam:<id>` |
| `Ignored a reward from the host that failed a safety check ...` | Your game rejected a reward outside normal mission limits, to protect your save | Nothing; if it keeps happening, report it with your log |
