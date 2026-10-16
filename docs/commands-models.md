# Survivor looks: `/model`

Change how your survivor looks for this game session, using outfits and characters the game already has (or outfits
your add-ons add), and put an add-on's model on your weapon.

Only want an add-on outfit or weapon look? The `~` window is easier: tab **Add-ons**, **Wear** (outfits) or **Use**
(weapon looks); see [below](#the--window-add-ons-tab). This page is the chat command `/model`, which also does other
survivors' outfits and Fort Hope NPC bodies.

Nothing about `/model` is saved: your profile, your unlocked outfits and your campaign runs stay as they are. Quit
the game (or `/model reset`) and you look like before. (Add-on outfits picked with the `~` window's **Wear** or in the game's
customization screen are kept: see [below](#add-on-outfits-on-the-games-customization-screen).)

## Commands

| Command | Who | What it does |
|---|---|---|
| `/model list` | everyone | The survivors and how many looks each has |
| `/model list <survivor>` | everyone | That survivor's outfits, heads, torsos and legs |
| `/model list npc` | everyone | Fort Hope NPCs, other survivors and cultists you can wear |
| `/model list outfits` | everyone | Outfits your add-ons add (none without such add-ons) |
| `/model list weapons` | everyone | Weapon looks your add-ons add (none without such add-ons) |
| `/model <name>` | everyone | You now look like that |
| `/model reset` | everyone | Back to your own look (from your profile), weapons back to your own skins |
| `/model` | everyone | What you wear now, and your current `/model` |
| `/model <player> <name>` | host | Changes another player's or a bot's look; everyone is told |
| `/model <player> reset` | host | That player or bot back to their own look |
| `/models off` | host | No more model swaps for anyone; swapped looks go back to normal |
| `/models on` | host | Model swaps allowed again (the default) |

`<player>` is the number from `/players` or a name, as for `/kick`.

## Names

- **A whole survivor**: `/model holly`, `/model walker`, `/model doc` ... = that survivor's default outfit on your
  survivor. The 12 names: `evangelo walker holly mom doc hoffman jim karlee heng sharice dan tala`.
- **One outfit**: `<survivor>_elite_<nn>`, e.g. `/model karlee_elite_03`, `/model walker_elite_07`. Outfits named
  `..._v2`, `_v3` are colour variants of the same outfit.
- **One piece**: `<survivor>_head_<nn>`, `_torso_<nn>`, `_legs_<nn>`, e.g. `/model holly_head_03`. Pieces add up:
  `/model holly_head_03` then `/model walker_legs_01` gives you both, on top of your own torso. An outfit replaces
  them all.
- **An NPC**: `/model vanessa`, `/model emmett`, `/model survivor_f_03`, `/model guard_m_01`, `/model pow_male_02`,
  `/model cultistsniper` ... (`/model list npc`).

- **An add-on outfit**: add-ons can add outfits (made with the mod maker's kit, `b4bmod survivor --as <name>`, or
  converted from a survivor replacement mod with `b4bmod convert`). `/model list outfits` lists yours, e.g.
  `/model casual_joe`. Works on any survivor: a woman's outfit on Walker or a man's on Holly keeps its own body
  proportions, and you keep your own survivor's hitboxes.
- **An add-on weapon look**: add-ons can add a model for one of the game's weapons (`b4bmod weapon --as <name>`).
  `/model list weapons` lists them with their weapon, e.g. `/model ak47` = your AR02 shows the AK. It stays on every
  AR02 you get (pickups, new maps) until `/model reset`; your survivor's look is separate.

## Add-on outfits on the game's customization screen

Your add-on outfits are also in the game's own wardrobe (the customization screen in Fort Hope), for every survivor,
at the end of the outfit list, unlocked, with the add-on's name (no picture). Pick one there like any outfit: unlike
`/model` it is **kept** for that survivor, also after a restart, in every mission and cutscene, until you pick another
outfit for them. The `~` window's **Wear** makes the same choice without the screen ([below](#the--window-add-ons-tab)).

- Your Back 4 Blood profile never stores it: the profile keeps that survivor's previous outfit, and b4b-coop remembers
  the add-on outfit in `b4bcoop-outfits.txt` (next to `b4bcoop.ini`). Without b4b-coop, or without that add-on, the
  game shows the previous outfit; nothing breaks.
- Players with the same add-on see it on you; everyone else sees your survivor (their base head, torso and legs).
- `/model reset` brings back this saved look. A host with `/models off` or `addons_policy=none` refuses it: you then wear
  your profile's outfit in that game.
- `outfits_screen=0` in `b4bcoop.ini` keeps add-on outfits out of the customization screen and turns **Wear** off
  (`/model` still works).

## The `~` window: Add-ons tab

The `~` window has no lists of looks: the commands above are typed in chat. Its **Add-ons** tab has one button per
add-on look: an add-on outfit's **Wear** puts it on your survivor at once and keeps it for that survivor (the same
saved choice as the customization screen above; **Wear** on another outfit swaps, **Take off** brings back the
survivor's own outfit; in Fort Hope or a mission, as host or in someone's session), an add-on weapon look's **Use** /
**Stop** puts it on your weapon of that type or takes it off. **Model swaps allowed**
(`/models on|off`, host) is under **Host rules** at the bottom of that tab (greyed out on a client).

`/model list holly` shows names without the survivor in front: `elite_04` there is `/model holly_elite_04`.

## Examples

```
/model list
/model list karlee
/model karlee_elite_05
/model holly_head_03
/model walker_legs_01
/model emmett
/model reset
/model 2 doc_elite_03        (host: the player or bot #2 from /players)
/models off                  (host)
```

## What others see

- **Survivor outfits and pieces**: everyone sees them, also players without b4bcoop. You keep your own survivor's
  voice, perks, name and portrait; only the body changes, in third person and in your first-person arms.
- **NPC looks**: only players with b4bcoop see them. Players without it see your own survivor.
- **Add-on outfits**: only players who have the same add-on see them (third person and your first-person arms).
  Everyone else sees your survivor in their base pieces (head, torso and legs of your profile), not your outfit.
- **Add-on weapon looks**: only players with the same add-on see the model in your hands; everyone else sees the
  normal weapon (without your skin). A weapon you drop keeps your look on the floor for players with the add-on
  (the normal weapon for everyone else), also for players who join later. Whoever picks it up gets their own choice
  on it: their skin, or their own `/model` look for that weapon (you get yours back when you pick it up again), like
  the game's own skins. Weapons lying in the map that nobody dropped, and the empty magazine a reload drops, stay the
  game's.
- Your look stays through map changes, chapters, death and respawn, cutscenes and the lineups, until `/model reset`
  or the end of the game session. If you change survivor at character select, your `/model` is put on the new survivor.
- Bots: only the host can change them (`/model <bot #> <name>`). When a player takes over that bot, the player's own
  look (or their own `/model`) is used.

## Replies

- `you now look like karlee_elite_05 (/model reset to undo)`
- `no model 'xyz' (/model list)`
- `models are off (/models on)`: the host turned swaps off.
- `(an add-on outfit: players without that add-on see your survivor)`: after `/model <add-on outfit>`.
- `The host turned model swaps off for add-on outfits (addons_policy=none).` (client): the host allows no add-ons.
- `your AR02 now looks like AK-47 (/model reset to undo; players without that add-on see the AR02)`: a weapon look.
- `The host turned weapon looks off (/models).` / `(addons_policy=none).` (client): your weapons are back to your own
  skins; `/model <weapon look>` again once the host allows it.
- `The host turned model swaps off (/models).` (client): the host refused your look; after 3 tries:
  `The host did not accept your model ...`.
- `[host] Holly now looks like doc_elite_03`: the host changed a player's or a bot's look.

## Limits

- Besides add-on outfits, only looks the game ships: survivor outfits (all of them, also ones you haven't unlocked) and NPC bodies that use
  the survivors' skeleton. Ridden and special NPCs (Hag, Sleeper, Titan) use other skeletons and are not offered.
- NPC bodies have no first-person arms: you keep your survivor's arms.
- Add-on weapon looks can't be picked in the weapon customization screen, only with `/model` (or the `~` window).
  The host's `/models off` also turns add-on outfits and weapon looks off.
- `/models off` stops swaps between survivors and NPC looks. It can't tell a player's own outfit change from a
  `/model` of one of their own survivor's outfits, so those stay allowed.
- Mixing pieces of different survivors (a head from one, legs from another) can show seams.
