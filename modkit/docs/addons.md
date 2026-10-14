# Add-ons: packing, testing, sharing

A b4bcoop add-on is **one `.pak` file** that players put into `<game>\b4bcoop-addons\` (next to `Back4Blood.exe`).
The mod loads it after the game's own files, so your files replace the game's files with the same path. Players
manage them in the game's `~` window (tab Add-ons), with `/addons` (chat) or `addonlist.txt` in that folder. The
player side is in the mod's b4bcoop-COMMANDS.txt, "Add-ons".

## Your mod folder
Everything b4bmod writes with `-o mymod` lands in `mymod/Gobi/Content/...`, the same paths as in the game. `pack`
takes **every** file in that folder, so keep only what you changed in it (no untouched copies of game files: they
conflict with other add-ons for nothing and mean shipping the game's files).

Optional `mymod/addoninfo.txt` (or the `pack` options) describes the add-on:
```
title=Walker red jacket
author=you
version=1.0
category=survivors
description=Red jacket for Walker's Elite 00 outfit.
```
Categories (free text, these are suggested): survivors ridden weapons items ui sounds maps misc. Left 4 Dead style
`addontitle "..."` lines work too.

## Pack
```
b4bmod pack mymod -o walker_red.pak [--title T] [--author A] [--version V] [--category C] [--description D] [--zip]
```
- `--zip` also writes `walker_red.zip` containing `b4bcoop-addons/walker_red.pak`: what you share. Players extract
  it into the game folder.
- The packer checks the layout (files must be under `Gobi/` or `Engine/`; a `.uasset` needs its `.uexp`) and prints
  the **content id** (changes with every change to the files) and the **content class**:
  - **cosmetic**: only textures, materials, meshes, animations, sounds, UI, effects;
  - **gameplay**: anything else (data tables, blueprints, physics assets, maps, ...).
  Hosts that allow cosmetic add-ons only (`addons_policy=cosmetic`; the default is `any`) refuse gameplay ones, so a
  survivor skin should come out cosmetic. The class is computed from the files by the packer and again by the game; what you write in
  `content=` is ignored.

## Test
```
b4bmod install walker_red.pak        copies it into <game>\b4bcoop-addons (close the game first)
b4bmod check                          what the game will load: order, on/off, class, conflicts
b4bmod check walker_red.pak           one add-on's info and file list
b4bmod uninstall walker_red
```
Start the game (the b4bcoop mod must be installed), type `/addons` in chat: your add-on is listed `[on, cosmetic]`.
Changes to the folder apply at the next game start.

## Conflicts and load order
Two add-ons that change the same file: the one further down in `addonlist.txt` wins (the game shows a notice).
Two add-ons that each change part of one asset (for example one has the `.uasset`, the other the `.ubulk`) can crash
the game: the game warns about that ("mixed"). Keep each asset's files together in one add-on.

## Sharing
- Share the zip (or the `.pak`), never extracted game files.
- Everyone sees only their own add-ons: yours changes the game on your PC only. Friends without it see the normal
  look, and joining works as long as the host allows your add-on's class.
- Add-ons are made for one game version: after a game update, re-extract and rebuild (the paks' AES key and paths
  may change).
