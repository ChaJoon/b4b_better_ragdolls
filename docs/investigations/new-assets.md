# New assets under new paths: add-ons that add instead of replace (design, #20/#21, epic #23)

Status 2026-09-25, build 14216215. Package side (`b4bmod rename`, §2, §5), added survivor outfits (§8:
`b4bmod survivor --as`, addoninfo `outfit=`, `/model <outfit>`) and added weapon looks (§9: `b4bmod weapon --as`,
addoninfo `weapon=`, `/model <look>`) work live, and replacement mods convert into added outfits (§10, `b4bmod
convert`); since #33 added outfits are also on the game's own customization screen (§11). Today every add-on replaces game files at their paths: a survivor model *is*
Mom's Elite 04 for whoever has the add-on. Goal: an add-on that brings an **extra** outfit (or weapon look) at its own
paths, selectable next to the game's, with nothing of the game's replaced.

## 1. What "adding" needs, piece by piece
| Piece | Replace (today) | Add (this design) |
|---|---|---|
| Packages (mesh, MIs, textures) | template's paths | copies under `/Game/b4bcoop/<addon id>/...` (§2) |
| References between them | template's own | rewritten to the copies (SKM -> MIs -> textures), shared retail ones kept (skeleton, physics asset, masters, micro-detail textures) |
| Something that names the outfit | the retail customization row | a row the agent adds at runtime (§3) |
| Selecting it | customization screen / `/model` | `/model <addon outfit>` (§4); the customization screen (§11) |
| Other players | see it if they have the add-on | same, plus a fallback for those without (§4) |

## 2. Packages at new paths (`b4bmod rename`, prototyped)
- `b4bmod rename <asset> </Game/new/Path/Name> -o <moddir> [--ref </Game/old>=</Game/new>]...` writes a copy as a new
  package. Name-map entries are changed **in place** (package path, object name, and each `--ref` pair's package path
  and name), so every FName index in the export data (bone names, slots, properties) stays valid; asset-registry
  records and tag values are renamed alike. Textures go through the texture writer again (their inline mips and
  `SkipOffset` are absolute offsets that move with the header); `.ubulk` follows the new name.
- A chain: textures first, then MIs with `--ref <old texture>=<new texture>`, then the mesh with `--ref <old MI>=<new
  MI>`. Checked offline: `3P_Test_SKM` (from our fitted Mom Elite 04) -> `Test_Torso_MI` -> `Test_Torso_BC_T` under
  `/Game/b4bcoop/test/`; skm.py and upkg re-read them, the mesh's import table points at the new MI, the MI at the new
  texture, everything else still at the retail packages.
- Pipeline change: `b4bmod survivor|weapon ... --as <name>` would fit/cook as today, then rename every file it wrote
  (template-owned textures, MIs, meshes) to `/Game/b4bcoop/<addon>/<name>/...` with the refs rewritten. Template-owned
  MIs the pipeline didn't edit but the new mesh still uses (e.g. `Head_LOD_MI`, whose textures the pipeline replaced)
  must be copied too, or they would show the retail textures on the model's UVs. Rule: copy the template's whole
  owned material set.
- Why `/Game/b4bcoop/<addon id>/`: add-ons never collide with each other or with the game (the conflict check in
  addons.c is path-based and stays quiet), and the class stays **cosmetic** (addon.py classifies by file type; new
  paths don't change that).

## 3. The engine side: loading packages nobody references
- Loading: `LoadAsset_Blocking` / `StaticLoadObject` with an FSoftObjectPath resolves the package through the pak
  file list (`FPakPlatformFile`), not the asset registry: packages in a mounted add-on load like retail ones (§5).
  paks.c already mounts add-ons before the first load and exempts them from the signature checks.
- Not found through the **asset registry** (the cooked `AssetRegistry.bin` is the game's; add-on packages are not in
  it): `mdl reg`, primary-asset scans and anything that enumerates assets won't list them. So the agent must be told
  the paths: `b4bcoop-addoninfo.txt` gets lines like
  `outfit=<name>|<hero>|/Game/b4bcoop/<id>/<name>/3P_<...>_SKM|/Game/b4bcoop/<id>/<name>/FP_<...>_SKM`
  (`weapon=` likewise: an item code and its meshes; see §6).
- Package names: the loader looks for the object `<path>.<name>`; `rename` changes the export's name with the path,
  so the two match.

## 4. Selecting and replicating an added outfit
Reuse the NPC-body path of models.c (model-swap.md): the look travels as the hero's replicated
`PlayerSlot.CurrentCustomizationSet`, with a **made-up outfit row** `b4bcoop.addon.<addon id>.<name>` in the hero's own
customization table (LastEquipSlot Outfit). The game finds no such row and applies nothing; `tick_npc`-style code on
every b4bcoop machine sees the row, looks the name up in its own add-on list and sets `CharacterMesh0` (3P) and
`FirstPersonArms` (FP, the part NPC bodies lack) to the add-on's meshes, empties head/legs.
- `/model <name>` offers add-on outfits by name next to the retail ones (`/model` list, `/models`), only on the
  machine that has the add-on; the host's `/models off` and the ServerSelectCustomizationSet refusal treat
  `b4bcoop.addon.*` like `b4bcoop.npc.*` (known name or refuse).
- **Players without the add-on**: the row is unknown to them; the game applies nothing, so they keep seeing the
  outfit that was applied before (the survivor's own). Same as the "everyone sees only their own add-ons" rule
  (addons.md §7): no protocol-visible difference except the row name, which protocol 2 already carries for NPCs.
  A player with a different version of the add-on (same name, other content id) sees their own version.
- Saves: models.c's campaign-run hook already swaps foreign rows out before the host saves the run; add
  `b4bcoop.addon.*` to it. The profile is never written by `/model`. If the customization screen is ever used (below),
  a profile could hold an add-on row: on a machine without the add-on the game drops invalid sets
  (`GetProfileCustomization` falls back to the default skin), which needs a live check before shipping that part.
- **Customization screen** (later, optional): the screen lists rows of `<Hero>_Customization_DT`. The agent could add
  a real row at runtime (DataTable `RowMap` insert of a `CharacterCustomizationRow` 0x320 copied from the template row,
  `ThirdPersonMeshDefinition.Mesh`/`FirstPersonMeshDefinition.Mesh` pointed at the add-on paths, a new GUID row
  name). Unknowns: unlock checks (entitlement/`Products_DT`), the thumbnail and name text, and profile persistence.
  Not needed for `/model`.

## 5. Live check (models-next, 2026-09-25, Proton, `multi.sh 2`, add-ons via `addons_dir=`)
Add-on `test_newpath.pak` (7 files: `3P_Test_SKM` -> `Test_Torso_MI` -> `Test_Torso_BC_T` under
`/Game/b4bcoop/test/`, the texture tinted red), loaded with `addons: 3. test_newpath.pak ... cosmetic`.
`mdl load /Game/b4bcoop/test/3P_Test_SKM.3P_Test_SKM` -> loaded (skeleton `3P_Biped_SK`), `mdl mesh 1 CharacterMesh0
<same>` (now clears the component's material overrides) -> the client's hero renders our model with a **red jacket**
(`newpath_face_crop.png`): the mesh at a new path loaded through the pak, its import of the new MI resolved, the MI's
import of the new texture resolved. No LogLinker/LogStreaming errors. So packages at new paths need nothing from the
engine side beyond mounting; what's missing is only something that names them (§3-§4).

## 6. Weapons
A new weapon *look* (skin) is a material set, not a new item: `Skin_Sets/*` MIs are chosen by the equipped skin row
(`<Code>_Customization_DT`). An added skin = new MIs + textures at new paths and a made-up skin row, applied by the
agent to the weapon's mesh components (`BaseSkeletalMesh_1P`, `BaseStaticMesh_3P`) like outfits. A new *model* for an
existing gun (without replacing it for everyone who has the add-on) needs the agent to swap those components' meshes
on the weapon actor for players who chose it; pickups (`<Code>_N_Pickup_BP` -> `3P_<Code>_SM`) and the dropped-magazine
particle (`VFX_EmptyMag_<Code>_3P_P`) keep the retail mesh unless they are also swapped. A new gun (new stats, new item
row) is gameplay content (data tables, blueprints) and out of scope for cosmetic add-ons.

## 7. Work, in order
1. `b4bmod survivor --as <name>`: rename pass over the pipeline output (+ copy the template's other owned MIs).
2. addoninfo `outfit=` lines; addons.c parses them into a list (name, hero, 3P, FP).
3. models.c: catalogue entries from that list, `b4bcoop.addon.<id>.<name>` rows, apply 3P + FP meshes, refusal and
   campaign-run hooks extended. Chat: `/model` lists them.
4. Live: add-on outfit on host and client, both with the add-on; client without it (sees the survivor); map change,
   checkpoint restart, campaign-run save.
5. Later: weapon skins, customization-screen rows.

## 8. Added outfits: implemented (2026-09-25, branch `models-outfits`, build 14216215, Proton, lane 1)
- **modkit** (`b4bmodel.py as_outfit`, `b4bmod survivor ... --as <name> [--title T]`): the pipeline writes into
  `<work>/stage` at the template's paths; then every package it wrote (meshes, textures, hair MI) and every
  `MaterialInstanceConstant` of the template's own folder the meshes import (directly or as a parent) is copied with
  `b4bmod rename` to `/Game/b4bcoop/outfits/<name>/<same base name>` with one `--ref` per copied package (so only
  package-path names change). Unchanged retail textures, skeleton, physics asset, shared/master materials stay
  referenced. Checked after: no copy imports a package that was copied. `<moddir>/addoninfo.txt` gets
  `outfit=<name>|<template survivor>|<3P object path>|<FP object path>|<title>` (replaced per name, several allowed);
  `addon.py pack` keeps these lines (also when repacking a pak), refuses one whose meshes aren't in the add-on.
  Test: CC0 MakeHuman `survivor.fbx` on Mom Elite 04 -> `casual_joe`, 23 packages (12 textures, 9 MIs, 3P + FP
  mesh), 58 files, cosmetic, ~55 s. The pak holds nothing outside `Gobi/Content/b4bcoop/`.
- **Agent**: `addons.c` parses `outfit=` lines (name `[a-z][a-z0-9_]{0,31}`, `/Game/` ASCII paths, `Pkg` ->
  `Pkg.Pkg`); `addons_outfits()` = those of mounted add-ons, a name in two add-ons: the later one wins. `models.c`:
  row `b4bcoop.outfit.<name>` in the wearer's own customization table (like `b4bcoop.npc.*`, §4), resolved after
  catalogue rows and survivor names, before NPCs. `tick_npc` on every machine: body = the 3P mesh (skeleton checked
  `3P_Biped_SK`), head/legs emptied, `FirstPersonArms` = the FP mesh (`FP_Biped_SK`). `compose` completes the set's
  head/torso/legs for made-up rows, which is what machines without the add-on (or b4bcoop) show. Host:
  `set_sane` accepts any well-formed outfit name (the host needn't have the add-on); refused under `/models off`
  (foreign row) and `addons_policy=none` (notice `The host turned model swaps off for add-on outfits
  (addons_policy=none).`); the campaign-run hook and `/models off` reset already cover it (a made-up row is foreign).
- **Protocol: no new bump.** The made-up outfit row needs host support (older hosts refuse it as "not a
  customization row"), which is what protocol 2 (the NPC-body bump on `models`, not released; `main` is 1) already
  stands for; outfits ship in the same unreleased protocol 2.
- Chat: `/model list outfits` (title, add-on), overview line `add-on outfits (/model list outfits): ...`, reply
  `(an add-on outfit: players without that add-on see your survivor)`.

### Live results (`multi.sh 3`: host + client 2 with `casual_joe.pak` via `addons_dir=`, client 3 without)
- Fort Hope: host (Holly) and client 2 (Doc) `/model casual_joe`: `models: hero slot 0/1 wears outfit casual_joe` on
  both; `mdl dump` there: `CharacterMesh0` = `/Game/b4bcoop/outfits/casual_joe/3P_Mom_Elite_04_SKM`,
  `FirstPersonArms` = `.../FP_Mom_Elite_04_SKM`, head/legs empty, overrides 0. Client 3 (no add-on): same set
  (`b4bcoop.outfit.casual_joe`), shows `3P_Doc_Head_03` + `Torso_00` + `Legs_00` (and Holly's pieces): the survivor.
  Screenshots `forthope_1/2.png` (each sees the other in the casual outfit).
- Mission Evansburgh B after character select (timer pick, heroes changed to Evangelo/Holly): re-applied on all
  three machines as above; host view `missionB_host.png` (client's hero in the outfit, host's FP arms in the
  outfit's sleeve). `ready` + `endmission 1`: `models: campaign run saved with 2 survivor(s) in their own look`;
  seamless transition to C: re-applied on all three. No `b4bcoop` string in any profile `.json`/`.sav` afterwards.
- `/models off`: `2 look(s) reset`, client's next `/model casual_joe` refused (`models are off`); `/models on` +
  `/addons policy none`: refused (`add-on outfit, addons_policy=none`), client stops resending.
- Second run (`multi.sh 2`, host with the add-on, client without): the client sees the host's Holly in her base
  pieces (`forthope_client_noaddon.png`), not invisible. Client 3's window in the first run never repainted (stale
  title screen), so its view is only from `mdl dump`.
- Screenshots: `~/.local/share/b4b-coop/outfits/shots/` (not committed). `tools/e2e.py --quick` on this build:
  13/13 PASS.

### Limits / open
- Without the add-on you see the wearer's base pieces, not their equipped Elite outfit (the set holds one outfit
  handle; the made-up row takes it).
- Same name in two different add-ons (or versions) = each player sees their own version.
- Weapons: §9.

### Customization screen
Built (#33): §11.

## 9. Added weapon looks (2026-09-25, branch `models-weapon-as`, build 14216215, Proton, lane 1)
### Design
- **What "adding" means for a weapon**: a player picks a look for one retail weapon (the item, stats, animations,
  pickups stay the game's); only the weapon actor that player carries shows the add-on's meshes. Not a new item, not a
  skin in the customization screen (that needs a real `WeaponCustomizationRow`, Products/unlock checks, an icon and a
  profile entry: `AppliedWeaponSkins` / `ApplyWeaponSkinCommand`).
- **What replicates**: every weapon actor (`Item`) has an `ItemMeshManagementComponent` (Item +0x770) whose
  `CustomizationRow` (+0x258, FDataTableRowHandle of the weapon's `<Code>_Customization_DT`) is its skin, set by the
  owning client with the server RPC `ServerCustomizationRow` (exec thunk 0x142197CE0; `_Validate` 0x1418BBE60 = true;
  `_Implementation` **0x1418BBE90**, vtable 0x14547C7B0 +0x500: copies table+row, applies, **no check**). The property
  replicates to everyone **except the owner** (seen live: the host's write never reached the owning client), so the
  owner sets its own copy (as the game does) and runs `OnRep_CustomizationRow`.
- **ApplyCustomization 0x1418BA5D0** (called by the implementation, the OnRep thunk 0x142197CB0 when +0x27F is set,
  and the first-person mesh setup 0x1418BBD50): a row the table doesn't have takes the no-skin path (+0x27F = 1,
  0x1418BABA0: empties the 3P components' `OverrideMaterials`, meshes untouched). So a **made-up row**
  `b4bcoop.weapon.<name>` shows the default weapon on every machine without the add-on or without b4bcoop: never
  invisible. The FP skin materials are set once at FP mesh setup; ApplyCustomization leaves them.
- **Putting the look on** (`weaponlooks.c`, every machine): for a weapon whose row names a look of a mounted add-on,
  each mesh component in the item's `FirstPersonMeshComponents` / `ThirdPersonMeshComponents` whose mesh has the name
  of one of the look's meshes (the add-on's copies keep the template's names: `AR02_SKM`, `3P_AR02_SM`,
  `3P_AR02_SKM`) gets the add-on's mesh (SetSkeletalMesh/SetStaticMesh, overrides emptied). Hooked right after
  ApplyCustomization (instant on OnRep, pickups, FP setup) plus a 1 s tick. When the row stops being ours, the retail
  mesh and the FP materials (kept at swap time) go back **before** the game applies the new skin.
- **Chooser**: `/model <look>` (and `Use` in the `~` window's Wear) = a wish per weapon code; the owner's agent sends
  `ServerCustomizationRow(<Code>_Customization_DT, b4bcoop.weapon.<name>)` for every item of that code in its
  inventory (`Inventory.EquipmentSlots`, class `<Code>_<n>_BP_C`) whose row differs, and remembers the row it
  replaced; `/model reset` sends that row back.
- **Host rules**: hook on the implementation: a `b4bcoop.*` row from a remote player is refused when malformed,
  under `/models off` or `addons_policy=none` (notice `The host turned weapon looks off (/models).` /
  `(addons_policy=none).`, the client's agent then reverts its own copy). `/models off` resets every look to no skin
  and tells the owners (the reset doesn't replicate to them).
- **Saves**: the campaign run keeps items as `ItemRowAndQuantity` {row, quantity, attachments, unbolted, clip ammo}:
  no skin, so nothing to swap out. The profile is only written by the customization screen (`ApplyWeaponSkinCommand`).
- **Protocol: no bump.** A retail host (any b4bcoop version) accepts the row (no validation); clients without the
  add-on see the default weapon. The host rules are host-side only.
- modkit: `b4bmod weapon <model> --fp-mesh <code> --as <name>` runs the weapon pipeline into a staging folder
  (skins `keep`, no dropped magazine, only `3P_*` static meshes), then copies FP mesh, 3P static mesh, 3P skeletal mesh
  (if any) with their textures and the weapon folder's material instances (`Skin_Sets/Skin_Default/*`) to
  `/Game/b4bcoop/weapons/<name>/` (same rename pass as outfits, `as_copy`), and writes
  `weapon=<name>|<code>|<FP>|<3P SM>|<3P SKM>|<title>`. `addon.py` checks and keeps the lines; `addons.c` parses them
  (`addons_weapons()`).

### Live results (`multi.sh 2`, add-ons via `addons_dir=`)
Add-on `mod_ak47.pak`: CC0 AK (loafbrr) on AR02, `--as ak47`, 22 packages / 52 files under `/Game/b4bcoop/weapons/ak47/`,
cosmetic, ~25 s. AR02 handed out with `giveitem <slot> row Weapons_DT DF038C6A4ED79AB7FDCF9CAB8D742DC7`.
- Fort Hope and mission Evansburgh B, both with the add-on, both `/model ak47`: `wlooks: Hergmgurk puts ak47 on
  AR02_1_BP_C_...` (host), `... wears ak47 (2 mesh(es))` on both machines; `wlook dump`: `BaseSkeletalMesh_1P` =
  `/Game/b4bcoop/weapons/ak47/AR02_SKM`, `BaseStaticMesh_3P` = `.../3P_AR02_SM`, overrides 0; bots' AR02 retail.
  Screenshots: FP AK on host and client, client's hero holding the 3P AK seen by the host
  (`m_host_sees_client_full.png`), host's hero with the AK seen by the client (`m_client_sees_host_full.png`).
- Weapon swap (quick swap to melee and back, `wlook swap`): the AK is back as soon as it is drawn
  (`swap_strip_host.png`); the meshes are never reset by a swap (no re-apply logged).
- Drop + pick up (client `wlook drop 0`, host `giveitem 1 <pickup#>`): the new AR02 gets the look in one try on both.
- Chapter transition B -> C (`ready`, `endmission 1`): re-sent and re-applied on the new map (first run: not
  re-applied, the mesh loader stopped after 3 loads across GC; fixed: only failures count). Campaign run saved while
  on: no `b4bcoop` in either profile `.json`.
- `/model reset` (client): row back to the skin it had (`B49B419C...`), FP overrides 10, 3P 4 as before.
- `/models off` (host): `2 look(s) reset`, the client's own copy reverted via the notice; its next `/model ak47`
  refused (`models are off`). `/addons policy none`: refused with the notice, the client back to its skin;
  `cosmetic` again: accepted.
- Client **without** the add-on (`addons_none`): `/model list weapons` = none, `/model ak47` = `no model`; it sees
  the host's AR02 as the retail gun with row `b4bcoop.weapon.ak47` (`noaddon_client_sees_host_retail.png`), host
  still sees its own AK; after the chapter transition likewise.
- `~` window, Models tab: the look with **Use** (`overlay press Use` -> `/model ak47` applied) and **Reset**
  (`overlay_models_tab.png`).
- `tools/e2e.py --quick`: 14/14 (a first run had 13/14: 3 UDP sockets of the client on 0.0.0.0 in one ss sample;
  the same check failed once before on lane 1 at 13:21, not related).
- Screenshots: `~/.local/share/b4b-coop/weaponas/shots/` (not committed).

### Floor: a dropped weapon keeps its look (2026-09-25, branch `models-floorlooks`, lane 2)
- **What a dropped weapon is**: the host destroys the `Item` and spawns an `ItemPickup` (`<Code>_N_Pickup_BP`, the
  item row's `DroppedLootClass`), replicated. Its `ItemRowsAndQuantities` (replicated, `ItemRowAndQuantity`
  {row, quantity, attachments, unbolted, clip ammo}) has **no skin**: retail skins don't show on the floor either. It
  shows `3P_<Code>_SM` on `StaticMeshComponent` (+0x2A8) and `InterpolatedStaticMeshComponent` (+0x2B0), both set.
  `CreationContext` (+0x310, replicated): 0 dropped from player, 1 loot (world spawns), 3 player item.
  `GetPreviousOwner` (0x142199930) reads a weak pointer at **+0x320**: set on the host (the dropping hero), **empty on
  clients**; `Owner`/`Instigator` are empty on both (`wlook pickups`).
- **Pairing** (`weaponlooks.c`, every machine with the add-on, no protocol): the weapons wearing a look are
  remembered with their hero and its position (each 1 s check + the ApplyCustomization hook); every frame, one that
  left its hero's inventory (drop, swap for a pickup) opens a 4 s search (object scan 5x/s) for an `ItemPickup`
  showing that look's `3P_<Code>_SM`: the one whose PreviousOwner is the hero (host), else the nearest with context 0/3
  within 3 m of where the hero stood (clients). It gets the look's 3P static mesh on both components (overrides
  emptied, the originals kept); a 1 s check re-applies if the game sets the mesh again. The row changing on a weapon
  that stays (`/model reset`, `/models off`) is not a drop. `/models off` (host; clients from the notice) puts the
  retail meshes back on the floor too. Machines without the add-on never touch the pickup (retail AR02/LMG01).
- **Picked up by someone else**: the new Item has no skin row; the new owner's client sets its own (retail skin from
  its profile, or its `/model` wish for that code). So the look follows the **chooser**, not the weapon, like retail
  skins; the original owner gets it back on its next pickup of that type (its wish). Chosen because the look is a
  personal choice and the new owner may not have the add-on.
- Dev: `wlook pickups [mesh substr]` (pickup, mesh, context, Owner/Instigator/PreviousOwner, position, floor look),
  `wlook use [mesh substr]` (`HeroUseComponent.ForcePressUse` on the nearest pickup = pressing F), `wlook drop <n>`.

### Live results, floor + LMG01 (`multi.sh 3`, lane 2: host + client 2 with `mod_ak47.pak` + `mod_aklmg.pak` via
`addons_dir=`, client 3 without add-ons; Evansburgh B, then C)
- `mod_aklmg.pak`: the CC0 AK on **LMG01** (`b4bmod weapon AK.fbx --fp-mesh LMG01 --slot AkMaterial=M249_Receiver_M2_FP
  --slot Ammunition=M249_Drum_M2_FP ... --as aklmg`, 33 packages, 36 s). It failed first: `LMG01_SK not extracted`
  (the `--as` copy follows the meshes' imports into the template folder; AR02_SK had been extracted by hand before);
  fixed: `--as` extracts the template's folder.
- LMG01 has a **skeletal** 3P weapon: `BaseMesh_3P` [SkeletalMeshComponent] = `3P_LMG01_SKM` (no `BaseStaticMesh_3P`);
  paired by name as designed: host and client 2 `wlook dump` = `/Game/b4bcoop/weapons/aklmg/3P_LMG01_SKM` + FP
  `aklmg/LMG01_SKM`; client 3 the retail meshes with row `b4bcoop.weapon.aklmg`. Client 2 sees the host's hero
  holding the AK-LMG (`floor_ak_client.png`); FP fire + reload on client 2 (`cheatprobe input lmb`, `input 0x52`):
  73 -> 80 rounds, the look stays through the reload animation (`lmg_reload_strip.png`).
- Client 2 drops its AK (AR02): host `ak47 on the floor: AR02_1_Pickup_BP_C_... (its dropper, 2 mesh(es))`, client 2
  `(nearest, 2 mesh(es))`, 10 ms after the drop; client 3 retail (`floor_ak_host.png`, `floor_ak_client.png`,
  `floor_ak_noaddon.png`).
- Host picks it up (`wlook use AR02`) while holding the AK-LMG: its LMG drops and gets `aklmg` on the floor on both
  add-on machines; the host's new AR02 gets the host's own skin row (`B49B419C...`), shown retail. Client 2 picks up
  the LMG: its own skin (`214B2A48...`). Then `/model aklmg` (client 2) and `/model ak47` (host): applied on all add-on
  machines.
- Client 3 (no add-on) drops its own AR02: no look on anyone's floor copy; then picks up the host's dropped AK: its own
  skin row, retail everywhere.
- Chapter B -> C (`endmission 1`): client 2's LMG re-applied on host and client 2. A drop in the pre-round saferoom
  (before `ready`) spawns no pickup at all (the game's; the search ends after 4 s). On C: client 2's dropped LMG
  `aklmg` on the floor on host + client 2 (`floor_lmg_host.png`), retail on client 3 (`floor_lmg_noaddon.png`).
- `/models off` (host): both floor copies `back to its own mesh` on the host and on client 2 (from the notice).
- `tools/e2e.py --quick` (lane 2, this build): 14/14 PASS.
- Screenshots: `~/.local/share/b4b-coop/floorlooks/shots/` (not committed).
- State then: late joiners, drops far from others and the client-side position race were open (done below).

### Late joiners and host-named pickups (2026-09-25, branch `models-looks2`, lane 1, `B4B_GPU=4090`)
- **What a joiner can read**: held weapons: the skin row replicates with the weapon actor, so a late joiner's
  `OnRep_CustomizationRow` -> ApplyCustomization hook puts the look on (verified below, nothing added). Dropped
  pickups: nothing replicated says who dropped them or which look (`ItemRowsAndQuantities` has no skin, PreviousOwner
  is host-only, Owner/Instigator empty); abusing a replicated gameplay field (attachments, row) would reach the next
  owner's item. So the **host names them**: it pairs the drop exactly (PreviousOwner; fallback nearest within 3 m),
  **with or without the add-on** (it tracks any `b4bcoop.weapon.*` row), and once the pickup lies still (2 checks
  0.25 s apart moving < 2 cm, or 5 s) sends every client a **data line** `wlook floor <look> <code> <x> <y> <z>`.
- **Data line**: `ClientTeamMessage` with a third Type `b4bcoopdata` (chat.c `CHAT_DATA_TYPE`, `admin_data_to`), never
  shown. An agent without a handler passes it to the engine, which shows nothing; clients without b4bcoop likewise.
  Purely additive and cosmetic, so **protocol stays 2** (a mixed session just behaves like before).
- **Late join**: each remote PlayerController first seen gets, 2 s later, `wlook hello` plus a `wlook floor` line
  (current position) for every told pickup still lying there. A told pickup that moves > 30 cm afterwards (seen: lay
  still 0.5 s at the hero's feet, then fell 1.4 m) is sent `wlook gone <old>` + `floor <new>`.
- **Looted pickups are not destroyed**: after a pickup, the actor stays at 0,0,0 with quantity 0 (`items`: `x0`),
  maybe for reuse. Quantity 0 = gone: the host drops it from its list and sends `wlook gone`; every machine puts the
  retail mesh back on a floor copy that became quantity 0; drops never pair with one.
- **Clients**: the instant guess stays (nearest within 3 m, as before). A host line is matched to the pickup of that
  weapon type lying within 1 m of the given spot (context 0/3 only, not already host-named; kept pending until the
  pickup is replicated, e.g. out of relevancy, until map change or `gone`); duplicates are ignored. The host-named
  pickup is marked; a guess of the same look within 5 m of it is taken back unless another pending line covers it;
  once the host is known to send lines (`hello` on this connection) a guess not confirmed in 10 s is taken back.
  Older hosts: guesses stay (old behavior). This makes the same-spot race (item swap between two heroes) moot.
- Dev: `wlook pickups` shows `(host)`/`(guess)`, `told`, and the host/client line counters; `wlook drophero <item#>
  <hero#>...` (host: those heroes drop in one frame), `wlook guess <x> <y> <look>` (force a client guess).

Live (`multi.sh 2` + a 3rd instance started mid-mission, hot-join + `takeover`; `mod_ak47.pak` via `addons_dir=`):
- Host + client with the add-on, both `/model ak47`, both drop: host `ak47 on the floor: ... (its dropper, 2
  mesh(es), told once it lies still)`, `told 1 player(s): wlook floor ak47 AR02 11243 340 611` 2 s later; client
  guess 10 ms after the drop, then `(the host's, 0 cm off, 2 mesh(es))`: host and client positions agree to 1 cm.
- Late joiner (3rd instance, add-on): `told Hergmgurk the weapon looks on the floor (2)` -> both pickups `(the host's,
  0 cm off)`; `wlook dump`: client 2's held AR02 `3P_AR02_SM` = `/Game/b4bcoop/weapons/ak47/...` (row replicated).
  Screenshots `late_floor.png`, `latejoiner_sees_floor.png` (presented frames, HUD incl.).
- Host **without** the add-on, two clients with it: host `ak47 on the floor ... (its dropper, 0 mesh(es))` + told;
  both clients `(the host's, 0 cm off, 2 mesh(es))`; a relaunched late joiner got it 3 s after joining (spectating).
- Same-frame drops of two heroes (`wlook drophero 0 2 1`) facing each other 70 cm apart: pickups landed crossed
  (each near the other hero); guesses were right (the first scan runs while both are still at their hands) and the
  host lines confirmed them. Forced wrong guess (`wlook guess`): `taken back (the host named another pickup)` at the
  host's line; a forced guess the host never names: `not confirmed by the host, taken back` after 10 s. Two
  same-look drops 2 m apart: before the "another pending line" rule the second guess flickered off for one tick.
- Host picks up the client's floor AK: client gets `wlook gone ak47 AR02 11266 339 611`, host `... back to its own
  mesh` on the looted pickup, the host's new AR02 its own `/model`; a late joiner then gets only the pickup still
  lying there.
- `tools/e2e.py --quick --no-lock` (lane 1, this build): 14/14 PASS. Logs: `~/.local/share/b4b-coop/looks2/run{1,2,3}`,
  screenshots `~/.local/share/b4b-coop/looks2/shots/` (not committed).

### Decisions: world pickups and the dropped magazine
- **World pickups nobody dropped** (loot, context 1) **stay retail**: a look is a player's choice and nobody chose
  these; whoever picks one up gets their own choice on the new weapon, as retail skins (which don't show on any
  pickup either).
- **Dropped magazine stays retail**: it is the Cascade system `VFX_EmptyMag_<Code>_3P_P` (only 8 weapons), always
  loaded (`find VFX_EmptyMag`: all 8 packages resident), whose mesh is in the shared asset's
  `ParticleModuleTypeDataMesh_0`; Cascade has no per-instance mesh parameter, so changing it for one player means a
  per-look copy of the particle system (modkit, plus a magazine mesh `--as` doesn't build today) and a hook on the
  3P reload montage's particle spawn. Visible only in third person for under a second. Follow-up if wanted.

### Limits / open
- Clients guess a pickup by position until the host's line arrives (~0.5-2 s after landing); with an older host the
  guess is final. World pickups nobody dropped and the dropped-magazine particle stay retail (above).
- While the look is on, players without the add-on see the default weapon, not the owner's skin.
- One look per weapon code per player.
- The client test window sometimes stays on the loading screen image (not repainted) although the game runs; views
  from that client were checked with `wlook dump`.


## 10. Replacement mods -> added outfits, worn by any survivor (2026-09-27, branch `anycleaner`, lane 2 Flatpak)
Community survivor mods are replacement paks: they override one survivor's outfit (`3P_<Hero>_Elite_NN_SKM` + `FP_`
arms, their own MIs/textures under their own folders or the outfit's, a physics asset, portraits, ability card).
`b4bmod convert <pak> --as <name>` (`modkit/convert.py`) makes an add-on outfit of each replaced outfit.

**Reading**: `pakx list|extract --pak <file>` reads one pak with CUE4Parse's `StreamedFileProvider`. Some community
paks carry a **v8 footer** (221 bytes: same B4B field order, no `bIndexIsFrozen`); the game mounts them, so pakx
presents them as v9 through a stream that swaps the footer (`FooterStream`), the file untouched. Stock-magic paks
(0x5A6F12E1) are refused.

**What is copied** (`plan`): from each replaced 3P mesh (on `3P_Biped_SK`; `_LastGen` variants skipped when a normal
one exists) and its FP arms, every package the mod has that they import, recursively; a retail MI/Material only when
it imports something the mod replaced (re-textures). Each package is judged with addon.py's classifier: cosmetic ones
are copied with `b4bmod rename` to `/Game/b4bcoop/outfits/<name>/<base>` (parent folder kept only on a base-name
clash); a **PhysicsAsset** (the mod's own, or a missing one) is never copied: each mesh's import is pointed at the
PhysicsAsset of the retail mesh it replaced (`--ref <mod PA>=<retail PA>` per mesh, e.g. the mod's own
`3P_<Name>_PA` -> `3P_Holly_PA`, missing `FP_Mom_PA` -> `FP_Biped_PA`); anything else non-cosmetic is left out with a note. UI is never
reached from the meshes. Outfits whose meshes are the same (same `.uexp`/`.ubulk` and imports: one look put on five
Doc outfits) count once; several -> `<name>_01..`, `--only`, `--list`.

**Community-cook quirks found and handled** (each a load error or crash on the first live run):
- Meshes importing their MIs with class `Material` (the objects are `MaterialInstanceConstant`): the async loader logs
  `FindExistingImport class mismatch MaterialInstanceConstant != Material` and drops the import. `rename
  --import-class <pkg>=<Class>` (new) sets an import's class to what the target package exports; convert passes it
  for every copied package.
- `BulkDataStartOffset` 4 bytes low (header + uexp - 8). UAssetAPI recomputes the export's SerialSize from it, so a
  renamed mesh came out 4 bytes short: `LogStreaming Fatal: SkeletalMesh ...: Serial size mismatch: Got 30303406,
  Expected 30303402` (crash in FAsyncLoadingThread). `keep_layout` restores the original sizes/offsets/bulk start,
  moved by the header's size change, whenever the `.uexp` is unchanged.
- Textures cooked without `BULKDATA_NoOffsetFixUp`: `.ubulk` mip offsets are stored relative to
  `BulkDataStartOffset` (negative: -6792 for the first mip). Texture.cs reads them as `stored + BulkDataStartOffset`
  and writes them relative to the new summary value (checked: the only bytes that change are the offsets).
- References to packages in neither the mod nor the game (a texture of another mod by the same author, missing
  textures): kept, with a warning; the original mod has the same hole.

**Agent (models.c)**: nothing was needed for body types. `3P_Biped_SK` takes spine/limb translations from each mesh's
own reference skeleton (mesh-mods.md §13), so an outfit keeps its template's proportions on any hero (female 3P:
pelvis 94.8 / head 154.2 cm, male 102.9 / 165.0), and `FP_Biped_SK` meshes of all survivors share one bind pose
(checked on Walker/Hoffman/Doc/Holly/Mom/Karlee: `upperarm_r` (-3.7, 18.1, 146.5), `hand_r` (15.1, 50.6, 114.8) in
all). Added: **hitboxes stay the wearer's**: before the first swap on a pawn, the PhysicsAsset of the game's own body
mesh is noted and set back with `SetPhysicsAsset` (-> `PhysicsAssetOverride`, kept by later `SetSkeletalMesh`), for
add-on outfits and NPC bodies; logged `models: hero slot N wears outfit <name> (hitboxes: 3P_<Hero>_PA)`.

**Live** (`multi.sh 3`, lane 2 / Flatpak Steam, host + client 2 with the 6 converted add-ons + an earlier `--as`
outfit made on Holly, client 3 `addons=0`; screenshots `~/.local/share/b4b-coop/anycleaner/live-*/shots/`, not
committed): all 7 `content: cosmetic`, accepted by the host with the default `addons_policy`. Fort Hope: a male
outfit on Karlee (3 of them), female outfits on Evangelo (4) and on Jim; each seen by the other player with its own
proportions (female outfit on Evangelo: shorter, slimmer; male one on Karlee: taller, broad). Evansburgh: host
Evangelo in a Holly-template outfit holding the SMG with both hands in third person, first-person arms of the outfit
on the gun; client Holly in an Evangelo-template outfit (host view, client FP); bot Sharice in a Jim-template outfit,
bot Heng in the Holly-template one. Hitboxes logged as the wearer's (`3P_Evangelo_PA`, `3P_Holly_PA`,
`3P_Sharice_PA`, `3P_Karlee_PA`). Client 3 sees the survivors (Evangelo retail, Karlee's base pieces), no outfit
logged there. No stretching seen in any view.

## 11. Added outfits on the game's customization screen (#33, 2026-10-04, lane 2 / Flatpak Proton, build 14216215)
Every mounted add-on outfit now shows up in the game's own customization screen (the Fort Hope wardrobe), for every
survivor, next to the game's outfits. Implemented in `models.c` (section "added outfits on the game's customization
screen").

### How
- **Rows**: every 5 s (cheap once done: per table, the number of outfits already checked) each add-on outfit gets a
  real `CharacterCustomizationRow` named `b4bcoop.outfit.<name>` in every survivor's `<Hero>_Customization_DT`,
  **on this machine only**. Same name as the made-up row of `/model` (§8), so the replicated set, the host relay, the
  refusals and the campaign-run hook treat both alike; on a machine with the add-on the game itself now finds the row
  and applies it (heroes, cutscene stand-ins, mannequins), on a machine without it the row is unknown as before.
- **The row**: allocated with GMalloc (0x320), built field by field (no shallow copies of the template's TMaps/FTexts):
  FTableRowBase vtable, `Quality`, `EquipSound` from the survivor's default outfit row; `DisplayName` = the outfit's
  title in upper case and `Description` = "Add-on outfit (<add-on>). Players without this add-on see your survivor."
  (`KismetTextLibrary.Conv_StringToText`, whose parameter is `inString`); `Slot` Outfit; `ThirdPersonMeshDefinition`
  and `FirstPersonMeshDefinition` soft paths = the add-on's meshes (no FP mesh: the survivor's default outfit's arms,
  never an empty FP mesh); material maps empty, console override and animation overrides empty. A row is only made
  when the 3P mesh's package is in the pak layer (`paks_file_exists`): the game would otherwise put an empty mesh on.
- **RowMap insert** (no engine function for it in the shipping build): TMap<FName, uint8*> layout in this build
  (elements 0x18 {key, value, HashNextId, HashIndex}, allocation bits inline +0x10 / heap +0x20, NumBits +0x28,
  MaxBits +0x2c, free list +0x30/+0x34, hash inline +0x38 / heap +0x40, HashSize +0x48); FName hash as the set's
  FindId (0x140BCE720) computes it: `(blk << 21) + blk + (off << 16) + off + (off >> 4) + number` (blk = id >> 18,
  off = id & 0xFFFF). Before inserting, the formula is checked against every existing element's HashIndex (mismatch,
  holes in the sparse array, bad hash size: no rows, logged, `/model` only). Elements grow by 16, allocation bits
  move to the heap past 128 (Dan's ~130 outfits x 12 tables = ~1560 rows).
- **Unlock check**: the screen locks a row only when a `Products_DT` product unlocks it (0x141C76960 scans the products'
  `Unlock` handles), so our rows are unlocked. The outfit count on the screen includes them ("7/11").
- **Name / thumbnail**: the outfit list shows names only (no thumbnails in that list); the selected outfit shows on the
  screen's mannequin as preview (the mannequin applies the real row).
- **Hitboxes**: the game applies the row with the template's mesh and its PhysicsAsset; `tick_npc` now takes the
  wearer's PhysicsAsset from the set's torso piece, or (outfit-only sets, as the screen sends) the survivor's default
  outfit, and keeps it as the override (log `hitboxes: 3P_<Hero>_PA`).
- **Pieces**: the screen sends outfit-only sets (head/torso/legs empty). The host completes the pieces of a set whose
  outfit is one of our rows before it replicates (`selectset_detour`, `complete_pieces`), so players without the
  add-on see the survivor's base pieces, never an invalid set (a cutscene stand-in would put the default skin on an
  invalid set, 0x141611AC8).

### The profile: decisions
- **The profile never holds a `b4bcoop.*` row.** Hook on the profile component's
  `EquipCharacterCustomizationSet(hero, set)` (0x141BC3C00, logs "applying customization set to %s"; only caller: the
  customization screen when it closes with a changed look, 0x141CFD280): in a copy of the set every slot naming one of
  our rows gets the profile's previous handle for that slot (read with the original `GetProfileCustomization`), and
  `LastEquipSlot` the previous one; the screen's own set (which it sends with `ServerSelectCustomizationSet` right after)
  is left alone, so the outfit is worn at once. Log: `models: profile keeps <hero>'s look (<row>) instead of add-on
  outfit <name>`. So the game without b4bcoop, without the add-on or with `outfits_screen=0` loads a profile it knows
  and shows the survivor's previous look; nothing to clean up after uninstalling.
- **The choice lives in `b4bcoop-outfits.txt`** next to `b4bcoop.ini` (`<survivor> <outfit>` per line, comment header;
  not touched by the updater, which keeps unknown files). Equipping a game outfit or piece for that survivor removes
  the line. Since 2026-10-16 the `~` window's **Wear** / **Take off** writes / removes the same line without the
  screen and re-inits the look from the profile path below at once (`models_wear`, overlay.md "Add-ons tab").
- **Reading it back**: hook on `FCharacterCustomizationUtils::GetProfileCustomization(pc, hero, out)` (0x141B769C0;
  callers: `AGobiPlayerState::InitCustomizationSet` at every hero pick -> `ServerSelectCustomizationSet`, the
  customization screen, the mannequins): for our own controller only, when the survivor has a remembered outfit and its
  row exists here (add-on mounted), the outfit slot becomes that row with `LastEquipSlot` Outfit and the pieces are
  completed. Missing add-on: the profile's look, untouched. `/model reset` goes through the same path (your saved
  look, add-on outfit included).
- **Host refusals**: `/models off` and `addons_policy=none` refuse the row as before. When the host's refusal notice
  arrives after we answered with a remembered outfit, the client stops answering with it (until the host's `/models
  on` or the next session as a host) and re-sends the profile's own look (`ClientInitCustomizationRowForSelectedCharacter`
  locally), so it is never left without a look (tested live 2026-10-04, see below). With `/models off` known,
  no substitution at all.
- **Host and campaign run**: a remembered outfit is a foreign row for the campaign-run hook (`foreign()` now treats
  every `b4bcoop.*` row as foreign even when it is a real row of the survivor's table), so the run saves the survivor's
  own look; the host notes its profile look as the clean set.
- `/model` stays: it is session-only, any outfit for any survivor; the catalogue skips our rows.
- Opt-out: ini `outfits_screen=0` (no rows; `/model` still works). The rows also stay off if one of the two profile
  hooks fails its signature check, so an equipped row can never reach the profile.

### Live results (`multi.sh 2`, add-ons `batman.pak`, `ciri.pak`, `laracroft.pak` via `addons_dir=`)
- `mdl screen`: 36 rows (3 outfits x 12 survivors), hash check passed. Screen (`mdl cs open`, the outfit list through the
  widget's own OutfitButton handler): Hoffman's list = his 8 outfits + `Batman`, `Ciri`, `Lara Croft`, unlocked, count
  7/11 (`cust5.png`); `mdl cs equip b4bcoop.outfit.ciri` -> Hoffman in Ciri on the screen's mannequin (`cust6.png`).
- Closing the screen (the widget's `OnBackEvent` twice): client log `hoffman now wears ciri on the customization
  screen`, `profile keeps hoffman's look (hoffman_elite_00) instead of add-on outfit ciri`, `applying customization set
  to Hoffman`; host `selects ... outfit=b4bcoop.outfit.ciri`, both `hero slot 1 wears outfit ciri`. Second pick for Jim
  (Lara Croft): host log `selects head=jim_head_00 torso=jim_torso_00 legs=jim_legs_00 outfit=b4bcoop.outfit.laracroft`
  (pieces completed), `hitboxes: 3P_Jim_PA`; the host sees the client's Jim as Lara Croft (`host_sees_lara.png`).
- `/model reset` on the client: `jim wears add-on outfit laracroft (customization screen)` (the profile path answered
  with the remembered outfit). After a game restart the client got Hoffman at character select: `hoffman wears add-on
  outfit ciri (customization screen)` (from the file).
- Profiles after the deferred save (chapter end, campaign run saved by the host): no `b4bcoop` string in either
  `PlayerProfileSettings.json`.
- Client with `addons=0` and the file still naming `hoffman ciri`: `pick: hoffman ciri (add-on not here)`, no rows, the
  profile's look.
- Screenshots: `~/.local/share/b4b-coop/screen-test/shots/` (not committed).
- Host refusals (2026-10-04, lane 2, client picked `laracroft` for walker/holly on the screen): host `/models off` ->
  `refused a look ...: models are off`, client `sent the profile's own look instead`, slot `walker_elite_00`; in the
  next mission (hero pick) no substitution at all (`host_off` known). `/models on` -> the client re-sends through the
  profile path and wears `b4bcoop.outfit.laracroft` again (before: only at the next hero pick). `/addons policy none`
  mid-session, mission start: `refused a look ...: add-on outfit, addons_policy=none`, client back to
  `walker_elite_00`. Both profiles after the deferred save: no `b4bcoop` string.

### Limits / open
- No thumbnail: the game's outfit list has none; a product row with an icon would be needed for the store-style
  views. The name is the add-on's title, upper-cased like the game's.
- Rows are per machine: a player without the add-on never sees it in their screen (same rule as `/model`).
- The remembered outfit is per survivor and per machine (not per Steam account); a profile restored from elsewhere
  keeps its own retail look.
- A runtime-mounted add-on (Get more, `/addons on`) gets its rows at the next frame (`models_outfits_refresh`), also
  when the screen was opened earlier in the session: the screen asks the table each time it opens (shop.md §1a). An
  updated add-on with other mesh paths has its existing rows repointed.
