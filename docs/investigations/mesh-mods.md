# Mesh mods: B4B skeletal mesh format, writer and glTF import (#18, #21, epic #23)

Status 2026-09-26, build 14216215. Branches `models-meshes`, `models-fullmodel`, `models-proportions` (§13). Tools: `modkit/upkg.py` (package
reader/writer, property dump, MI reader), `modkit/skm.py` (SKM render data parse/edit/write),
`modkit/skmgltf.py` (glTF/FBX export/import), `modkit/sm.py` (static meshes), `modkit/b4bmodel.py`
(model -> survivor / weapon pipeline, `b4bmod survivor|weapon`), `modkit/blender/b4bfit.py` (headless Blender fitting),
`modkit/blender/preview.py` (headless renders), `tools/modkit/testassets/mpfb_survivor.py` (test human),
`modkit/dotnet/pakx` (offline extraction, run via `modkit/b4bmod.py`). Guides: modkit/docs/meshes.md.

## TL;DR
- **Format solved.** `skm.py` parses the whole cooked `USkeletalMesh` native part and writes it back
  **byte-identically for 593/593 retail `*_SKM` packages** (heroes, zombies, weapons, props; cloth, vertex colours,
  8 influences, 16/32-bit indices included). The "unknown 8-byte trailer" is not a new field in `USkeletalMesh`: it is
  a TRS **bool32 in `FSkeletalMeshRenderData::Serialize`** before the stock `NumInlinedLODs`/`NumNonOptionalLODs` bytes,
  then the stock empty `DummyObjs` array (details §1).
- Ray-tracing data: every retail LOD stores an **empty** `TArray<uint8> RayTracingData` (int32 0). RT geometry is built
  at runtime; a writer only has to write the count.
- **glTF round trip is lossless** for geometry: export retail → glTF → import gives identical positions, UVs, indices
  and skin weights (tangents ±1 LSB). Through Blender 5.1 (import + export): positions/UVs/indices identical, tangents
  recomputed by Blender (MikkTSpace), weights identical once "all bone influences" is on.
- **New meshes from Blender import** onto a template mesh (its skeleton, physics asset, material slots, LOD settings):
  a box figure built in Blender imports as a valid package (CUE4Parse reads it; our parser re-reads it).
- **Verified in game** (2026-09-25, Proton, `launch/multi.sh 1`, mission Evansburgh): (1) retail Holly Elite_04 3P
  and FP meshes edited by `skm.py edit --inflate` render inflated and animate normally; (2) a box figure made in
  headless Blender and imported with `skmgltf.py import` onto Mom Elite_04 (3P, 1 LOD, adjacency stripped) and onto her
  FP arms renders, animates with `3P_Hero_ABP`/`FP_Hero_ABP` and takes the template's materials. No mesh warnings in
  the log (§4).
- **Real models end to end, verified live** (§8, 2-player sessions, Proton): a clothed, textured MakeHuman character
  (CC0, from an **FBX** with a Mixamo rig) as Mom's Elite 04 outfit (3P + FP arms, 5 decimated LODs, own textures), and
  a CC0 AK (**FBX** + PNGs) as the AR02 (FP skeletal, 3P skeletal, 3P/pickup static meshes, dropped magazine), both as
  add-ons. Seen: survivor idle/aiming in Fort Hope and a mission, FP arms holding weapons, the AK in first person, the
  FP **reload animation moving our magazine**, the 3P AK in a bot's hands; add-on player vs. vanilla player both ways.
  Not seen: firing / muzzle flash (fire can't be simulated), the pickup and dropped-magazine static meshes in the world.
- `b4bmod survivor|weapon` (`modkit/b4bmodel.py`) does it in one command (~40 s / ~30 s): fit in Blender (bone map, pose
  fit onto the template joints, weights, slots/atlases, LOD decimation), cook, build the textures into the template's own
  texture packages, retarget weapon skins. FBX, glTF, OBJ, DAE, .blend in; unrigged characters get the template's weights.
- **Static meshes solved**: `sm.py` parses/writes cooked `UStaticMesh` **byte-identically for 2118/2118 retail meshes**
  (§1b); the section's extra TRS bool is `bVisibleInRayTracing`. **Other players see a weapon as a static mesh**
  (`3P_<Code>_SM` on the weapon actor's `BaseStaticMesh_3P`), not `3P_<Code>_SKM`.
- **Characters found online** (§10): VRM, Rigify, Mixamo, UE-named and unrigged-in-parts models with no hand-made
  settings (bone maps, slots, shared textures, T-pose all automatic), live on 5 survivors; faces follow head/jaw only
  (the game animates faces with face bones driven by a PoseAsset, §10).
- **Own proportions** (§13): `3P_Biped_SK` takes body bone translations from each mesh's bind skeleton (retail female
  heroes use it), so 3P models keep their own limb/torso/neck lengths (mesh bind skeleton rewritten); FP arms stay
  fitted (FP skeleton: all bones animated). Live: short-legged monster vs the old stretched fit side by side.
- No retail survivor, FP-arms or weapon mesh has morph targets (0 of 320 hero/weapon SKMs): faces are bone-driven, so
  the importer never has to write morphs. Weapon **skins** are material sets for the retail UVs: the pipeline points
  every skin MI of the weapon at the model's textures (a player with a skin equipped sees the model as made).

## 1. Cooked layout (static RE of Back4Blood.exe + 593-file validation)
Functions (VA, build 14216215):

| Function | VA | Notes |
|---|---|---|
| `USkeletalMesh::Serialize` | 0x144006BD0 | stock order; bCooked read inline; `DummyObjs` at 0x1440073E8 (0x14262A0D0) |
| `FSkeletalMeshRenderData::Serialize` | 0x144022EE0 | LOD array (0x144031BB0), **bool32 → +0x16**, u8 NumInlinedLODs (+0x11), u8 NumNonOptionalLODs (+0x12); +0x13/+0x14 = Num - NumInlined; +0x15 = owner `bSupportRayTracing` (+0x198 bit 0) |
| `FSkeletalMeshLODRenderData::Serialize` | 0x14401FB20 | stock 4.25 |
| `…::SerializeStreamedData` | 0x14401F770 | stock 4.25 + `RayTracingData` (0x143F9CDE0, TArray<uint8>) last |
| `…::SerializeAvailabilityInfo` | 0x14401F970 | only for non-inlined LODs (none in retail) |

Before loading, `USkeletalMesh::Serialize` seeds the render data's +0x16 with bit 2 of the mesh flag byte +0x168
(SDK order at +0x168: bUseFullPrecisionUVs, bUseHighPrecisionTangentBasis, **bExplicitCPUSkinning**, bCPUAccessOnly,
bHasBeenSimplified, bHasVertexColors (bit 5, confirmed: gates the colour buffer), …, bEnablePerPolyCollision (bit 7)),
so the TRS bool is most likely the cooked `bExplicitCPUSkinning`. Retail value: 0 in all 593 meshes.

Export data after the tagged properties (all little-endian, `bool` = 4 bytes, `strip` = 2 bytes FStripDataFlags):
```
int32 bHasGuid (0)
strip (1,0)                                   editor data stripped
FBoxSphereBounds ImportedBounds               7 floats
TArray<FSkeletalMaterial>                     {i32 Material, FName SlotName, bool bImportedName (0) [FName],
                                               FMeshUVChannelInfo {bool, bool, float[4]}}
FReferenceSkeleton                            TArray<{FName, i32 Parent}>, TArray<FTransform 40B>, TMap<FName,i32>
bool bCooked (1)
TArray<FSkeletalMeshLODRenderData>            below
bool  b4b_render_flag (0)          <- TRS (render data +0x16)
uint8 NumInlinedLODs (= LOD count) <- stock
uint8 NumNonOptionalLODs (0)       <- stock
TArray<UObject*> DummyObjs (0)     <- stock
```
Old reading ("stock DummyObjs = 4 bytes, B4B = 8 bytes `00 00 NN 00 00 00 00 00`") = CUE4Parse reading the TRS bool as
NumInlined/NumNonOptional (0, 0) and then the real NumInlined (NN), NumNonOptional (0) and DummyObjs as "8 bytes".

LOD (`FSkeletalMeshLODRenderData`):
```
strip (1,0); bool bIsLODCookedOut (0); bool bInlined (1 in all retail)
TArray<int16> RequiredBones                   all ref bones for LOD0-3
TArray<FSkelMeshRenderSection>:
   strip (1,0); u16 MaterialIndex; u32 BaseIndex; u32 NumTriangles; bool bRecomputeTangent
   (no RecomputeTangentsVertexMaskChannel byte: FRecomputeTangentCustomVersion is 1)
   bool bCastShadow; u32 BaseVertexIndex; TArray<FMeshToMeshVertData 64B> ClothMappingData
   TArray<u16> BoneMap; i32 NumVertices; i32 MaxBoneInfluences; i16 CorrespondClothAssetIndex
   FClothingSectionData {FGuid, i32}; [strip.class & 1 == 0:] DuplicatedVertices {TArray<u32> DupVertData,
   TArray<{u32 Length, u32 Index}> DupVertIndexData}; bool bDisabled
TArray<int16> ActiveBoneIndices               = bone maps + all ancestors, sorted
u32 BuffersSize                               = exact byte size of the streamed block below (verified)
-- streamed block --
strip (1, class)                              class & 1 = CDSF_AdjacencyData stripped
FMultisizeIndexContainer                      u8 size (2|4), bulk {i32 elemsize, i32 count, data}; absolute vertex indices
FPositionVertexBuffer                         i32 stride 12, i32 n, bulk float3
FStaticMeshVertexBuffer                       strip; i32 NumTexCoords; i32 n; bool FullPrecisionUVs; bool HighPrecTangents;
                                              bulk {TangentX, TangentZ} FPackedNormal (int8/127; TangentX.w 127,
                                              TangentZ.w = binormal sign ±127); bulk UV (half2 or float2), vertex-major
FSkinWeightVertexBuffer                       strip; bool bVariableBonesPerVertex (0); u32 MaxInfluences (4|8);
                                              u32 NumBones (= n*Max); u32 n; bool b16BitBoneIndex (0);
                                              bulk u8 [indices into the section BoneMap x Max, weights x Max (sum 255)];
                                              lookup: strip, i32 0, bulk (4, 0)
[bHasVertexColors] FColorVertexBuffer         strip; i32 stride 4; i32 n; bulk BGRA
[!adjacency stripped] FMultisizeIndexContainer adjacency (12 indices per triangle)
[any section has cloth] FSkeletalMeshVertexClothBuffer  strip; bulk raw; TArray<{u32,u32}> index mapping
TMap<FName, FRuntimeSkinWeightProfileData>    empty in retail
TArray<uint8> RayTracingData                  empty in retail
```
Retail variety (593 SKMs, 2281 LODs): 4 or 8 influences, never variable/16-bit; 72 LODs with 32-bit indices; weapons
use float UVs, characters half; 1148 LODs with vertex colours (all white on heroes); 30 LODs with cloth; adjacency
present everywhere (retail PC cooks keep it). FP arms: 2 LODs; 3P heroes: 5; zombies: 6.

Static meshes: `FStaticMeshSection` has an extra `bVisibleInRayTracing` bool (CUE4Parse `StaticMesh.HasVisibleInRayTracing`);
no static mesh writer yet (weapons and characters are skeletal).

## 1b. Static meshes (cooked `UStaticMesh`, 2118/2118 byte-identical)
Validated on every `*_SM` under `Items/` and `Environments/Props/[A-C]*` (`sm.py roundtrip`). Stock 4.25 cooked layout
plus the TRS section bool. After the tagged properties:
```
bool bHasGuid; strip; bool bCooked; i32 BodySetup; i32 NavCollision; FGuid LightingGuid; TArray<i32> Sockets
TArray<FStaticMeshLODResources>:
   strip (1, 8 = RT resources stripped); TArray<FStaticMeshSection> {i32 MaterialIndex, u32 FirstIndex, u32 NumTriangles,
     u32 MinVertexIndex, u32 MaxVertexIndex, bool bEnableCollision, bool bCastShadow, bool bForceOpaque,
     bool bVisibleInRayTracing (TRS / 4.26)}
   float MaxDeviation; bool bIsLODCookedOut; bool bInlined (all retail: inlined)
   SerializeBuffers: strip; FPositionVertexBuffer; FStaticMeshVertexBuffer (as in SKMs); FColorVertexBuffer
     (stride 0 when empty); FRawStaticIndexBuffer x5 {bool b32Bit, bulk u8, bool bShouldExpandTo32Bit}: indices,
     reversed (empty in retail), depth-only (= the full index list in retail), reversed depth-only (empty),
     [wireframe: editor, stripped], adjacency (present, empty); per-section + whole FWeightedRandomSampler
     {TArray<float>, TArray<i32>, float} (all empty in retail)
   FStaticMeshBuffersSize {SerializedBuffersSize = payload bytes of all vertex + index buffers, DepthOnlyIBSize,
     ReversedIBsSize} (checked on all 2118)
u8 NumInlinedLODs; strip (distance fields); per LOD bool bValid [+ FDistanceFieldVolumeData] (none valid in the set)
FBoxSphereBounds; bool bLODsShareStaticLighting; 8 x FPerPlatformFloat {bool, float} screen sizes
bool bHasOccluderData; bool bHasSpeedTreeWind; TArray<FStaticMaterial {i32, FName, FMeshUVChannelInfo}>
```
The tagged `ExtendedBounds` (what the engine culls with) is rewritten with the bounds. Collision is a separate
`BodySetup` export (tagged `AggGeom` + cooked PhysX data); the writer keeps the template's (weapons: one box), so a model
of about the template's size keeps sensible collision. `sm.py import` writes plain models (node transforms applied);
`sm.py from-skinned` moves a model fitted onto a skeletal template into a static template's space with the rigid
transform that maps the retail skeletal mesh onto the retail static one (area-weighted surface moments: centroid,
principal axes, signs from the third moment; retail 3P_AR02_SKM -> 3P_AR02_SM and pickup: same size to 0.5 cm, rotated
90° about X; magazine part -> `AR02_MagEmpty_3P_SM`: identity rotation).

**Every material slot must keep a section.** A 3P static weapon with only the slots the model uses (2 of 4) was
invisible on other players (bVisible stayed off on `BaseStaticMesh_3P`); with one zero-area triangle per unused slot,
in slot order like retail, it renders (§8). Game code addresses the weapon's sections by index (skins, attachments,
the magazine during reload), so `from-skinned` always writes all slots. (Skeletal meshes didn't need it.)

## 2. Tools
```
# offline extraction from the retail paks (no game running; same bytes as the agent's dumpassets): modkit/dotnet/pakx via
# the modkit (AES key built in; `b4bmod config aes_key`, B4B_AES_KEY or --aes-key override it)
modkit/b4bmod.sh extract '/Game/Characters/Heroes/Holly/*'          # into ~/.local/share/b4b-coop/extract
modkit/b4bmod.sh find 'Heroes/Holly/.*_SKM$'

.venv/bin/python modkit/skm.py info <x_SKM.uasset>            # bounds, materials, LODs, sections
.venv/bin/python modkit/skm.py roundtrip <files...>           # parse + write, byte compare
.venv/bin/python modkit/skm.py edit <in.uasset> <out.uasset> --inflate 2.5 --scale-section '*:5:1.5' --material '*:1:4'
.venv/bin/python modkit/skmgltf.py export <x_SKM.uasset> <out.glb>     # reference for Blender
.venv/bin/python modkit/skmgltf.py import <template_SKM.uasset> <in.glb|.fbx> <out/Gobi/Content/.../x_SKM.uasset> [--lod <LOD1 model>]... [--lods N] [--socket NAME=x,y,z] [--bone NAME=x,y,z]
.venv/bin/python modkit/sm.py info|roundtrip|sizes <x_SM.uasset>...      # static meshes
.venv/bin/python modkit/sm.py import <template_SM> <model> <out> [--lod ...] | from-skinned <SM> <SKM> <out> <lod0.glb>... [--only-bone mag]
.venv/bin/python modkit/upkg.py props <x.uasset> [export]              # tagged properties, readable
modkit/b4bmod.sh survivor|weapon <model> ... -o mymod [--install]   # the whole pipeline (§6; modkit/docs/meshes.md)
.venv/bin/python tools/b4bpak.py pack <out dir containing Gobi/> <mods dir>/b4bmod_x.pak
```
Blender: File > Import > glTF (the export), keep the armature, edit or replace the mesh (vertex groups named after
bones), name materials after the template's slots (`skm.py info` lists them; Blender's `.001` suffixes are ignored),
File > Export > glTF with **Include > "All Bone Influences"** on. Headless test: `blender -b --python
modkit/blender/blocky.py -- <export.glb> <out.glb> fp|3p`.

Import rules (skmgltf.py):
- Coordinates: glTF = (x, z, y)·0.01 of UE. Swapping Y/Z is a reflection, so UE's clockwise triangles are glTF's
  counter-clockwise ones without reordering; bone quaternions (−x, −z, −y, w); tangent sign negated.
- Joints map to the **template mesh's** reference skeleton by name (a hero mesh has a subset of `3P_Biped_SK`: Holly
  160 of 179 bones). The vertices must be in the template's bind pose: joints that moved are reported
  (Blender re-orients bone axes on import, so only joint positions are compared; `--bind rebind` re-skins to the
  template pose using full matrices, only for rigs whose bone axes were kept).
- One section per template material slot; up to 8 influences (4 unless needed), u8 weights summing to 255, bone map
  per section (≤ 255 bones), duplicated-vertex buffers, active/required bones, bounds, BuffersSize are computed.
- Adjacency is **stripped** (`CDSF_AdjacencyData`, the engine's own path for platforms without tessellation). Vertex
  colours only if the template has `bHasVertexColors` (white by default). UV precision follows the template.
- LODs: each `--lod <model>` is a real LOD (b4bfit decimates them); `--lods N` without `--lod` writes N copies (old
  behaviour). The tagged `LODInfo` array is truncated to the LOD count and every `LODMaterialMap` entry set to -1.
- Input: glTF/glb as is; FBX/OBJ/DAE/.blend are converted by Blender (`blender/b4bfit.py convert`). The model must
  already be on the template's skeleton (bone names, bind pose); `b4bmodel.py`/`b4bfit.py` do that fitting.
- Kept from the template package: skeleton, physics/shadow physics asset, materials, sockets, sampling info, clothing
  assets (left unbound: no section references them). Templates with morph targets are refused.

## 3. What a survivor / weapon / FP arms replacement needs
- **3P survivor body**: `3P_<Hero>_…_SKM` on `3P_Biped_SK` (plus head/legs pieces for non-outfit rows); animated by
  `3P_Hero_ABP` (all heroes share it); physics asset comes from the template. Materials: the template's slots (MICs of
  B4B masters; new textures are the textures agent's side).
- **FP arms**: `FP_<Hero>_…_SKM` on **`FP_Biped_SK`** (197 bones: body, arms, fingers, plus all weapon bones and
  `camera`), 2 LODs, 2 slots (sleeve `Torso`, `ArmSkin`). Its bind pose is **not** the 3P one (≈1.09× taller, arms
  differ), so FP arms are authored against the FP export (`skmgltf.py export FP_…_SKM`), not cut from the 3P body.
- **FP weapons**: `Items/Weapons/<Type>/<Id>/Meshes/<Id>_SKM` are also on `FP_Biped_SK` (weapon bones `gun`, `mag`,
  `bolt`, …), float UVs, vertex colours, 2 LODs. **3P weapons** (`3P_<Id>_SKM`) have their own skeleton (`AR01_SK`).
- **What other players see**: the weapon actor (`AR02_1_BP_C` ...) has `BaseSkeletalMesh_1P` (the FP SKM),
  `BarrelStaticMesh_1P`/`HipSightStaticMesh_1P`/`ADSSightStaticMesh_1P` (attachments), and for third person
  **`BaseStaticMesh_3P` = `3P_<Id>_SM`** (+ `BarrelMesh_3P`, `SightMesh_3P`). `3P_<Id>_SKM` exists for AR01, AR02,
  LMG01, Sni01 only; we replace it too.
- **Who references which weapon mesh** (name maps of all 115,815 retail `.uasset` headers, `b4bmod extract --regex
  '\.uasset$'`, 4 s, 321 MB): world pickups `<Id>_N_Pickup_BP` show **`3P_<Id>_SM`** (with `Skin_Default/*_3P_Glint_MI`
  overrides), not `<Id>_Pickup_SM`: no package references `AR02_Pickup_SM`, `HG01_Pickup_SM`, ... (guns; melee
  `Hatchet01_Pickup_SM` is used). The dropped magazine is the Cascade system `VFX/Systems/EmptyMags/
  VFX_EmptyMag_<Id>_3P_P` (mesh particles of `<Id>_MagEmpty_3P_SM` with `Weapon_<Id>_Mag_FP_MI`), fired by the 3P
  reload montage `BaseAnims3P/.../WPN_<Id>_Stand_Reload_AM`; only 8 exist (AR02, AR03, AR04, HG01, HG03, LMG02, SMG02,
  SMG03; `HG04_Mag_Empty_3P_SM` is only in `ChendaVendor_BP`). `3P_AR02_SKM` is used only by two Finleyville church
  cinematics; `3P_LMG01_SKM` is LMG01's 3P weapon (`LMG01_BP`); `3P_AR01_SKM`, `3P_Sni01_SKM` are unreferenced.
  Other 3P pieces in the weapon BPs: `3P_<Id>_Ironsights_SM` (AR01/03/05, SG02, SMG05, Sni01/02),
  `3P_AR04_Carry_Handle_SM`.
- `b4bmod weapon` infers the meshes from the FP mesh's folder (listing only, no extraction): `3P_<Id>_SKM`,
  `3P_<Id>_SM` + `<Id>_Pickup_SM` (names compared without `_` and case), the folder's one `*Empty*_SM`; the rest is
  printed as "not replaced". Without a 3P SKM the static meshes come from the FP fit (`sm.py from-skinned` with the FP
  SKM as reference): retail FP SKM and 3P SM have the same principal sizes (AR02 17.5/4.5/1.1 vs 17.9/4.4/1.1 cm, HG01,
  SMG01, SG01, AR03, Sni01 alike), rotated 90° about X. Static meshes' own textures (e.g. `SMG02_Primary_3P_N_T`) are
  built too (fit set -> static slot by MI, MI name without `_FP/_3P`, or base colour). AK on AR02 with only `--fp-mesh`:
  pak byte-identical to the explicit-path run (sha256 d49da7d1...). Live: a spawned `AR02_1_Pickup_BP` shows our AK
  (`cheatprobe spawnactor`, `pickup2_crop.png`), with the pickup glint sweep of `Skin_Default/*_3P_Glint_MI` over it.
  Dropped magazine not seen: `cheatprobe emitter VFX_EmptyMag_<Id>_3P_P` shows nothing for the retail HG01 either (the
  test path, not the mesh), and an emptied-clip reload by a bot 1.5 m away showed no falling magazine in 12 frames.
- Sockets: the FP weapon SKM has `SkeletalMeshSocket` exports relative to `gun` (`muzzle`, `holo`, `scope`, `laser`,
  ...); the 3P weapon skeleton has a `muzzle` **bone**. `skmgltf.py import --socket muzzle=x,y,z` / `--bone muzzle=...`
  move them (b4bmodel does it from the model's muzzle marker or barrel tip).
- Not supported: cloth on a template without a clothing asset (skirts on one that has: §14), morph targets (heads), new bones
  (the mesh's ref skeleton must be a subset of the Skeleton asset), new material masters.

## 4. In-game results (live, 2026-09-25)
Setup: `launch/install.sh` (models dev build), `B4B_INI_EXTRA='modpaks=Z:\...\meshes\paks_edit' launch/multi.sh 1`,
`b4b.py mission Easy`, then in the mission `mdl mesh <hero#> CharacterMesh0|FirstPersonArms <path>` and `mdl bring/look`
to put meshes on bots in front of the camera. Screenshots in `~/.local/share/b4b-coop/meshes/shots/` (not committed).

| Test | Pak | Result |
|---|---|---|
| Retail edit, 3P: `3P_Holly_Elite_04_SKM --inflate 4` (all 5 LODs, cloth section included) | mounted at startup | Bot Holly is visibly puffed up next to a bot wearing the unmodified `…_NonCloth_SKM` (`cmp1.png`, `fp_fat.png`); animation, materials, cloth normal |
| Retail edit, FP: `FP_Holly_Elite_04_SKM --inflate 2.5` | same | thick first-person arms/sleeves (`fp_fat.png`) |
| New mesh, 3P: Blender box figure → `3P_Mom_Elite_04_SKM` (1 LOD, LODInfo truncated, no adjacency, 3 sections on slots Head/Torso/Legs) | `mountpak` at runtime (order 1100), then `mdl mesh` | box figure "[BOT] KARLEE" walks, aims, follows (`blocky7.png`) with Mom's textures |
| New mesh, FP: Blender box arms → `FP_Mom_Elite_04_SKM` (1 LOD) | same | box forearms/hands holding the weapon (`blocky1.png`) |

Log: `paks: precacher: first read from a mod pak (…b4bmod_meshtest.pak)`; no LogSkeletalMesh/LogStreaming entries for the
modded packages (the only streaming errors are retail `MAP_Evansburgh_B_Debug` ones). With the test instance's low
settings `r.SkeletalMeshLODBias=1`, so edits must cover LOD1+ (both tools write every LOD they keep).

## 5. Next / open
- Firing / muzzle flash position and the dropped magazine are not seen live yet (pickup: seen, §3) (fire and weapon
  drops can't be triggered unattended; `giveitem` replaces without dropping).
- ADS: the sight line follows the template's `ironsights` bones; a model with a different sight height aims slightly
  off through its own sights (the AK is within ~1.5 cm). Moving those bones per weapon is untested.
- Hair: done (§9); in the model's own texture colours since §11.
- Face animation: the model's face is skinned to `head` (and jaw if the rig has one); B4B's face bones (eyelids, lips)
  don't move it. What following the game's face animation would take: §10.
- New bones (rebuild the ref skeleton from the Skeleton asset), new material masters: not supported. Cloth and swinging
  hair: §14.
- Hitboxes follow the template's physics asset; everyone sees their own add-ons (addons.md §7).

## 6. How the model pipeline works (`b4bmodel.py`, `blender/b4bfit.py`)
- **Bone map** (character): source bone names are matched as UE4 mannequin names (as is), Mixamo (`mixamorig:Hips`
  -> pelvis, `Spine/Spine1/Spine2` -> spine_01..03, `LeftArm` -> upperarm_l, `LeftHandIndex1` -> index_01_l, ...),
  3ds Max Biped (`Bip01 L UpperArm`, `L Finger0`...), VRoid/VRM (`J_Bip_L_UpperArm`, `J_Bip_C_UpperChest`) or Rigify
  (`DEF-spine`=pelvis ... `DEF-spine.006`=head, `DEF-upper_arm.L`, `DEF-f_index.01.L`), and by a generic reader (side
  from Left/Right/L_/_l/.L, part from words like upper_arm/forearm/shin/thigh/up_leg/toe, finger numbers; numbered
  `arm`/`leg` joint chains spread over upperarm..hand / thigh..ball; the spine = the path pelvis -> head spread over
  spine_01..03, neck_01/02; pelvis = common ancestor of the thighs if unnamed; last neck joint = head if unnamed). Only
  bones that weight vertices (and their ancestors) are candidates. The option with most required bones wins, its gaps
  filled from the generic reading; `--bonemap {"src": "b4b"}` adds or overrides. Required: pelvis, spine_01, head,
  both arms (upperarm/lowerarm/hand) and legs (thigh/calf/foot); missing ones -> error + `bonemap_template.json`.
- **Orientation/scale**: frames from the mapped joints (lateral = right->left upper arm, up = pelvis->head); B4B heroes
  face +X. Uniform scale = template head-to-feet height / source's.
- **Pose fit**: every mapped limb bone gets a transform D that puts its joint on the template joint and aims it at the
  template's next mapped joint (upperarm -> lowerarm, hand -> middle_01 ...), with a stretch along the segment; the
  source bone is first re-pointed (edit mode, mesh unchanged) at its aim joint, so the stretch is along its own axis
  and the pose holds it without shear (`inherit_scale NONE`). Torso (pelvis -> neck) and neck (-> head) get one D per
  chain: per-segment fitting squashed/stretched torsos in bands on rigs with other spine spacing (VRoid x0.57..x1.83,
  Mixamo hips x0.43). Every other bone moves with the mapped bone it follows (`own_bones`: mapped ancestor, else the
  mapped bone of the same name in another layer (Rigify `ORG-`/`MCH-`/`DEF-`), else the nearest mapped segment:
  MakeHuman's Rigify helpers `DEF-elbow-helper.L`, `DEF-knee-helper.L` hang off ORG bones and stayed behind before:
  detached upper arms, jagged knees, the mouth left under the chin). Jaw: re-weighted only (joint not moved).
  Result: 0.00 cm joint error on all test rigs. The armature modifier is then applied: the mesh sits in the template's
  bind pose (A-pose), which is what the game skins against. Since §13 this full fit is `--proportions fit` (and
  always used for FP arms); the 3P default only turns the segments and gives the mesh a bind skeleton with the
  model's own joints.
- **Weights**: source groups renamed to template bones; unmapped source bones (twist, extra face bones) go to the
  nearest mapped ancestor. `--twist template` (default) splits each limb weight among the template's twist bones
  (`upperarm_twist_01`, `lowerarm_twist_01`, `elbow_twist_01`, `wrist_twist_01`, `thigh_twist_01`, `calf_twist_01`,
  ...) in the proportions of the nearest template vertices (so forearm roll twists like retail). No armature:
  the model is stood up by its bounding box (`--facing`, default -Y = Blender's front), scaled to the template's
  stature from its skeleton ((head joint - ground) x 1.115, ground = balls of the feet - 4.5 cm, measured on Walker,
  Holly, Hoffman 3P meshes; an FP arms mesh has no feet), then `unpose_arms`: arm direction shoulder -> most lateral
  vertices (named arm parts: top -> far end) against the template's same measure; the template's upperarm is posed
  like the model's, weights come from the posed template (16 nearest, restricted to the part's bones for objects
  named like `arm-left`, `head`, `torso`), then the inverse pose is applied to the model. Checked with
  `preview.py --pose test` (bent limbs) on the template, the rigged fit, the unrigged fit and a UE-named rig: all
  deform alike.
- **FP arms**: the same fit against the `FP_Biped` export (its own bind pose), then only faces whose vertices are >= 50 %
  weighted to arm bones stay (hands + sleeves). A probe run tells which materials survive in first person.
- **Slots, texture sets, atlases**: each source material goes to a template slot (`--slot MAT=SLOT`). A *texture set*
  is what a slot's material instance samples; b4bmodel finds which slots share one (retail FP `ArmSkin` samples the
  outfit's `Torso` textures with the skin master) and packs every material that must live in one set into a grid atlas
  (UVs remapped into tiles; clamped to 0..1). The same layout is used for 3P and FP. Tile images are composed in Blender
  (numpy): base colour (sRGB as is), normal (green flipped: glTF/Blender maps are OpenGL style; `--normal-dx` if yours
  are DirectX), PBR = R AO, G roughness, B metallic (retail convention, checked on weapon and hero PBR textures), A and
  any other owned texture (hair multimask ...) = the retail texture's average; microtile masks = 0 (no retail fabric
  detail on our UVs). Only textures in the template's own folder are replaced; shared ones never. Encoded by
  `b4bmod texture` (format and sRGB of the original; size since §16: what the tiles' images need, packed, at most
  the retail texture's size).
- **LODs**: the mesh welded by position first (§14c), then Blender Decimate (collapse) per LOD ratio (`--lods 1,0.5,0.3,0.15,0.06`), weights kept. `skmgltf.py import`
  writes them as real LODs, keeps the template's `LODInfo` (screen sizes) for as many LODs as written and sets every
  `LODMaterialMap` entry to -1 (retail LOD3/4 remap sections to `*_LOD` materials by section index, which would
  scramble ours). UV channel count = the template's (extra channels = UV0). Up to 8 influences (retail LOD0 uses 8).
- **Weapons**: parts are separate objects; names pick the bone (`mag|magazine|clip|drum` -> mag, `bolt|slide`,
  `trigger`, `charging handle`, `safety|selector`, `ejector|dust cover`, `hammer`, `stock`, `cylinder`; `--part RX=BONE`),
  the rest is `gun`. The model is turned (`--forward/--up`, default +X/+Z), scaled to the template's length
  (`--scale fit`), and moved so its trigger object sits on the template's trigger bone (else bounding-box centres). The
  muzzle is an empty named `muzzle` or the barrel tip; it moves the FP `muzzle` socket and the 3P `muzzle` bone. Static
  meshes (3P, pickup) come from the 3P fit via `sm.py from-skinned`, the dropped magazine from its `mag` part.
- **Skins** (`--skins retarget`, default): every `Skin_Sets/*_MI` of the weapon (also `/Game/TUxx/...`), extracted with
  `b4bmod extract --regex`, gets `Base Surface Texture`/`Base Normal`/`PBR` pointed at the textures we wrote for the same
  part and view (51 of the AR02's 132 skin MIs: the receiver and magazine parts the AK uses; one 3P MI without own
  texture parameters inherits from its FP parent).
- Test asset (not committed; `tools/modkit/testassets/mpfb_survivor.py`, dev only): MPFB 2.0.17 (Blender extension, GPL) + the MakeHuman system
  asset pack (CC0): male, `male_casualsuit05` + `shoes03` + `short02` hair + low-poly eyes, eyebrows baked into the skin
  texture (hero heads are opaque), Mixamo rig, FBX + PNGs. Weapon: "AK" by loafbrr (opengameart.org/content/ak, CC0):
  FBX with separate Bolt/Magazine/Trigger objects, PBR PNGs (albedo, normal, roughness, metalness, AO).

## 7. Guides (for mod makers)
Moved to the kit: [modkit/docs/meshes.md](../../modkit/docs/meshes.md) ("Make a survivor model", "Make a weapon
model", doing the fitting by hand, lower-level tools). The mod maker runs `b4bmod survivor|weapon <model> ...`, which
extracts the templates and what they reference (`tree`), runs `modkit/b4bmodel.py`, packs the add-on (`--install`
installs it); `b4bmodel.py` stays usable directly (`b4bmod model ...` or `python modkit/b4bmodel.py ...`).

## 8. Live results: real models (2026-09-25, Proton, `launch/multi.sh 2`, add-ons per instance via `addons_dir=`)
Add-ons: `survivor_mh.pak` (37 files, cosmetic: textures, meshes) and `ak47_loafbrr.pak` (v1.2: 136 files incl. 51 skin
MIs, cosmetic: textures, materials, meshes). Screenshots in `~/.local/share/b4b-coop/fullmodel/shots/` (not committed).
Dev helpers added: `giveitem <slot> row <DataTable> <Row>` (AR02 = `Weapons_DT DF038C6A4ED79AB7FDCF9CAB8D742DC7`, found by
parsing `Weapons_DT` for `AR02_1_BP`), `poke <addr> <bytes>` (clip count: `ClipAmmoComponent` +0x2AC).

| Session | Test | Result |
|---|---|---|
| 1: host add-ons, client none | Fort Hope, both `/model mom_elite_04`, host looks at the client (`fh_host_3p*.png`) | our survivor, idle animation, jacket/jeans/boots/hair/face; client sees vanilla Mom Elite 04 on the host (`fh_client_look*.png`) |
| 1 | mission, host FP (`m_host_fp.png`) | our FP arms (skin hand, jacket sleeve) holding the SMG; client in 3P shows our model holding a pistol (`m_host_3p_client.png`) |
| 2: client add-ons, host none | Fort Hope: client sees the host as our survivor (`fh2_client_sees_host.png`), host sees vanilla (`fh2_host_sees_client_vanilla.png`); login `2c0g,c91d7af3c,c27afddbc` accepted by `addons_policy=cosmetic` | as expected both ways |
| 2 | AR02 given to both: client FP = our AK geometry with the profile's graffiti skin (before skin retargeting); 3P static AK **invisible** on host hero and bot | fixed by keeping one section per slot (§1b) |
| 2b | same, new `3P_AR02_SM` | host hero (our survivor) holds our 3P AK, visible (`m3_client_host_front.png`) |
| 3: host add-ons v1.2 | host FP holds our AK with our textures despite the equipped skin (`m4_host_fp.png`) | skin retargeting works |
| 3 | clip poked to 0 -> automatic reload (`m4_auto1..6.png`) | FP reload animation: left hand pulls **our magazine** out and inserts it (mag bone), clip 0 -> 20 |
| 3 | bot Heng given AR02, teleported in front of the host (`m4_bots_near.png`) | our 3P AK (static mesh, our textures) in the bot's hands |

No `Fatal`/`LogSkeletalMesh`/`LogStaticMesh` errors in any log. Key presses (`cheatprobe key 0x52` = R, bound to
`AbilityReload`) and mouse clicks don't trigger game actions in an unfocused test window (chat keys do), so reload was
triggered by emptying the clip and firing wasn't tested.

## 9. Hair (models-next, 2026-09-25)
- `Master_Hair_M`: BLEND_Masked, two-sided, DitherOpacityMask, shading model from the material. Every retail hero
  hair MI sets static switch **Enable MultiMask** (+ Use AO, Use PDO, useFacingAO, UsesVertexColors, Variation,
  useFlowMapTexture, SubtractTipFromDepth). Textures: `Hair MultiMask` (the outfit's own `*_Hair_MM_T`, BC7 RGBA) +
  shared Holly `Root/Alpha/ID/Depth` (DXT1, unused with MultiMask on: the MM's channels look like them). On UV0 of
  Mom Elite 04's hair section, MM **A** is the strand coverage (0.27 mean at triangle centres vs 0.24 image mean, 0 in
  gaps); RGB inside strands ~(0.45, 0.28, 0.3). No colour texture: colour = `RootColor` -> `TipColor` (vectors).
- Retail hair vertex colours are mostly **black** (8 % white): with UsesVertexColors, `Vertex Color Multiplier`
  (Mom Elite 04: 0.51, 0, 0.536 = purple) tints the white-coloured strands. Our meshes were white everywhere ->
  purple hair (seen live, `hair_face2_crop.png`).
- Pipeline (`b4bmodel.py`/`b4bfit.py`): a model material put on a slot whose master is `Master_Hair_M` (`--slot
  hair=Hair`) gets role `hairmm`: A = the model's alpha (alpha map, else base colour's alpha), RGB = the retail MM's
  in-strand average; the Hair MI (if owned by the template's folder) gets `RootColor` = 0.6 x and `TipColor` = the
  model's mean hair colour (linear), via `b4bmod mi`; the hair section's vertex colours are written black
  (`skmgltf.import_gltf(slot_colors=)`).
- Live (MakeHuman `short02` hair, 2048² RGBA with alpha, on Mom Elite 04 `Hair`): dark brown hair with see-through
  strand edges at fringe and sideburns (`hair_face3_crop.png`, `hair_face3_zoom.png`) instead of the opaque helmet.
  Screenshots in `~/.local/share/b4b-coop/fullmodel/shots_next/` (not committed).
- Limits: one colour gradient per hair (no per-strand texture colour: the master has none); FP arms never show hair.
  Superseded as the default by §11 (`--hair texture`); this is `--hair tint`.

## 10. Characters found online (models-characters, 2026-09-25)
Goal: take a humanoid someone published and get it into the game with one command. Test set (licenses in
`~/.local/share/b4b-coop/characters/LICENSES.txt`, nothing committed): VRoid "Sendagaya Shino" (CC0, VRM 0.x,
opengameart.org/content/vroid-studio-cc0-models), "Horror Monster" (CC0, Mixamo rig, opengameart.org/content/horror-monster-0),
Kenney "Blocky Characters" (CC0, unrigged parts), two MPFB/MakeHuman humans (CC0 assets) made with
`tools/modkit/testassets/mpfb_survivor.py` (new `--phenotype`, `--rig rigify.human`: Rigify-generated, 930 bones, glb),
Khronos CesiumMan (CC-BY 4.0, numbered joints; mapping test only, its texture is a logo). All with `--as` (added outfits)
and FP arms, no `--slot` given.

| Model | Source rig | Survivor | Worked | Still off |
|---|---|---|---|---|
| Shino (VRoid, 17 materials, alpha hair, 42 shape keys) | VRM `J_Bip_*` + 100 hair/skirt joints | Holly Elite 00 | bones 52/158 mapped (VRoid table), auto slots: skin/face/mouth/eyewhite -> Head atlas, clothes -> Body atlas + Gear, hair + lashes/brows/eyeline + iris -> Hair (masked), highlights dropped; live: 3P (host, others with add-on), FP arms, idle/run | hair one colour (fixed in §11: the "light blue" was a grey texture without its MToon colour), skirt and hair rigid, no eyelid/mouth animation |
| Horror Monster (2.4 m, one material) | Mixamo, 2-chain fingers | Walker Elite 00 | 34/41 mapped; textures: FBX linked the mask map as base colour -> reclassified by name, Unity mask map -> PBR; live: 3P idle/run, FP claws holding a bat | legs stretched x1.7-1.8 (short legs on the survivor skeleton; model proportions kept since §13) |
| "bulky" (MPFB, 2.15 m, afro, overalls) | UE4 mannequin names | Hoffman Elite 00 | 53/53; live as a bot: run, aim, shoot | afro hair renders as one grey-brown colour |
| "shorty" (MPFB, 1.27 m, ponytail) | Rigify full rig (DEF- + ORG/MCH/face) | Doc Elite 00 | 54 mapped (Rigify table); after `own_bones`: no detached arms/knees/mouth; live: 3P, FP arms | neck squashed (x0.39: Rigify neck starts below the shoulders); thighs x1.5 (the model's own since §13) |
| Blocky (Kenney, parts: head/torso/arm-left/...) | none, arms hanging down | Karlee Elite 00 | un-posed 53 deg into the A-pose, parts keep their limb; live: 3P after the LOD fix | boxes deform like boxes; arms offset from the survivor's shoulders |
| CesiumMan | `Skeleton_arm_joint_L__4_`, `leg_joint_R_2` | Walker | 19/19 mapped by the generic reader (numbered chains, neck_2 = head) | not taken into the game (logo texture) |

Live (lane 1, Proton, `B4B_GPU=4090`, `multi.sh 3`: host + client 2 with the 5 add-ons via `addons_dir=`, client 3
without): Fort Hope and Evansburgh B; host/clients `/model <outfit>`, host `/model <bot|player> <outfit>`; every
machine with the add-ons shows the outfits (`mdl dump`: `CharacterMesh0` = `/Game/b4bcoop/outfits/<name>/3P_...`,
`FirstPersonArms` = `.../FP_...`), client 3 shows the survivors' base pieces (`3P_Holly_Torso_00` ...). Screenshots
(presented frames, `launch/shot.sh`): `~/.local/share/b4b-coop/characters/shots/` (not committed). Seen: idle, run and
aim (bot with `bulky` during a horde), FP arms holding SMG / bat / pistol. Not seen: reload and crouch (no unattended
trigger), firing. `tools/e2e.py --quick --no-lock`: 14/14.

What broke and was fixed (all in `modkit/`):
- `.vrm` refused by b4bmod; glb/vrm textures are embedded (no file path): written out from `packed_file` bytes.
- VRM MToon (unlit) materials have no Principled BSDF links: images taken from the node tree by colour space.
- Texture roles: `normal` contained `orm` (normal maps read as ORM); FBX had the mask map on Base Color and a bogus
  metallic 1.0 (characters now ignore constant metallic); `baseMap`, mask maps, gloss understood.
- Unused material slots (after `--drop` / the FP cut) broke atlases and the FP probe: only materials of faces count.
- Shape keys made the armature modifier fail: removed with a note (survivors have no morph targets).
- Holly Elite 00's head colour lives in `Elite_02/Textures`, hero hair masks in `Meshes/Shared`,
  `Characters/Shared/Textures/Hair`; the old "only textures of the template's folder" rule left the face and hair
  with retail textures on our UVs. Now shared textures get a copy in `<template folder>/Textures/` and the MI is
  pointed at it (`b4bmod mi set` adds the override); a shared MI (`Holly_Hair_MI`) is copied to `<folder>/Materials/`
  and the cooked meshes repointed (`b4bmod rename` in place with `--ref`). `b4bmod mi` no longer extracts its
  `set` values (new paths aren't in the game).
- Low-poly LODs: Blender Decimate collapsed Kenney's 12-triangle boxes to single triangles; the test instances show
  LOD1+ (`r.SkeletalMeshLODBias=1`), so the blocky outfit was one brown triangle in game. LODs keep >= 1500 triangles.
- Add-ons were 150-200 MB (4096 atlases, 4096 hair masks): hair masks capped at 2048, `--max-texture` option.
- `preview.py`: `--textures` (the textures made for the game), views follow a B4B skeleton's facing (heroes face +X,
  so "front" rendered their side).

### Facial animation: bones, and what a custom head would need
- B4B faces are **bone-driven, no morph targets** (0 of 320 hero/weapon SKMs have morphs, §TL;DR). Per hero
  `<Hero>FacialAnimationConfig` (class `FacialAnimationData`): `FacePoseAsset` = `FacePoses_<Hero>_PoseAsset`
  (`PoseAsset`, `bAdditivePose=1`, `RetargetSource=3P_<Hero>_SKM`, skeleton `3P_Biped_SK`), `ExpressionCurves`
  (Relaxed, Anger, Caring, Concerned, Disgust, Joy, Fear, Interested, Wounded, Playful, Sad, Surprise) and
  `LipsyncPhonemeVisemeMapping` (phoneme -> viseme pose: AH, OW, EH, E, ER, L, CH, N, MBP, MouthOpen_TEMP ...).
  Lines come with `Characters/Lipsync/English(US)/LipsyncLines_English_<Hero>_DT` (row struct `LipsyncLineRow`, 15 MB
  for Holly: per-line phoneme timing). Blinks: additive `BaseAnims3P/Additives/3P_M_Blink_ADD_AS`; faces in
  `3P_Jim_FacePoses_AS`, `Mom_FacePoses_AS`.
- The pose asset's tracks (its name map): `jaw`, `tongue`, `upper/lower_teeth`, `lip_upper/lower(_l/_r)`,
  `lip_corner_upper/lower_l/r`, `chin`, `cheek_upper/lower_l/r`, `nose`, `nostril_l/r`, `brow`, `eyebrow(_01..03)_l/r`,
  `eyelid_upper/lower(_01..03)_l/r`, `eye_l/r`, `eyeball_l/r`, `eye_inner/outer_l/r`, `ear_l/r` (all in every hero
  mesh's skeleton, `3P_Biped_SK`).
- So a custom head follows the game's talking/blinking/expressions if its face vertices are **skinned to those face
  bones** where they sit. Two ways, neither built:
  1. *Warp the face to the survivor's*: find the model's facial landmarks (eye corners, lid lines, mouth corners,
     jaw line; from its own face bones if the rig has them (VRoid `J_Adj_*_FaceEye`, Rigify `DEF-lip.*`, `DEF-lid.*`),
     else from the geometry around the template's landmarks), deform the model's face so they land on the
     template's, then copy the template head's face weights (`--weights transfer` limited to the head). Works with
     the existing importer (template bind pose kept); the face gets the survivor's proportions.
  2. *Move the face bones to the model's face*: write the mesh's own reference pose with the face bones at the
     model's landmarks (the importer keeps the template's ref skeleton; `skm.py` can write `FReferenceSkeleton`,
     the engine computes the inverse bind matrices from it at load). The poses are additive, so they add
     rotations/offsets around the new positions; needs a live check of the skeleton's per-bone translation
     retargeting (base animations keying face bones in "Animation" mode would pull them back to Holly's positions).
  (Built as a combination, §12.) Both need eyelids closed by rotation about the eye centre: the model's eyeballs must sit where `eye_l/r` are (or be
  moved there), and lids must be real geometry (VRoid/anime faces often draw eyes and lashes as textures and blink with
  shape keys: those would need the lids modelled, or stay static).
- Cheapest useful step: jaw. Rigs with a jaw bone (Rigify, many game rigs) already get the jaw re-weighted (bind-only
  mapping); `lip`/`jaw` weights from the template on a warped mouth region (way 1, mouth only) would make heads talk.


## 11. Hair in the model's own colours (models-hair, 2026-09-25)
Goal: VRoid/anime hair with gradients, highlights, dyed tips, and the iris/lash layers on the hair slot, in their real
colours instead of one averaged root-to-tip colour (§9).

**The hero hair material can't draw a colour texture.** `Master_Hair_M` (cached expression data, 42 scalar, 17 vector,
10 texture parameters): textures `Hair MultiMask`, `Root`, `Alpha`, `Depth`, `Unique_Hair_Value` (= the hero's
`*_Hair_ID_T`: per-strand random value for `RandomHueVariation`/`RandomValueVariation` through the `HueShift`
function), `FlowMapTexture`, `AO_2ndUV`, drench/blood; colour only from `RootColor`/`TipColor`/`Vertex Color
Multiplier`. Static switches seen over all 80 retail `Master_Hair_M` MIs (heroes, NPCs, specials): Enable MultiMask,
EnableHighQuality, SubtractTipFromDepth, Use AO, Use PDO, UsesVertexColors, Variation, useFacingAO,
useFlowMapTexture: none selects a colour map. A new switch combination would need shaders the cooked game doesn't
have, so every option below reuses a retail material instance's cooked permutation.

**Vertex colours do carry colour** (live test, lane 1: MPFB "bulky" on Hoffman, whose `Hoffman_Hair_MI` has
UsesVertexColors; hair vertices banded by height R/G/B/W/K, `RootColor = TipColor = 0.8 grey`, multiplier white,
random variation 0; `fh_vc_zoom.png`): blue and green bands showed as blue and green strands, the black band showed the
MI's grey. So colour = the vertex colour where it isn't black, Root/Tip where it is (retail: mostly black strands,
white ones tinted by the multiplier, §9). Usable for per-vertex colour (a dense hair mesh with the texture baked
into its vertices, black avoided), but only at vertex resolution: texture highlights, drawn strands and the colour
of alpha layers (iris) are lost. Not built.

**Other hero-usable masked masters with a colour texture** (all 2125 character MIs scanned: parent, base property
overrides, static switches): `Master_Zombie_Outfit_M` (BLEND_Masked, **two-sided**, DitherOpacityMask, MSM_Cloth,
bUsedWithSkeletalMesh) with static switch **`Enable BC.A Opacity Mask`**: opacity = the base colour's alpha. Retail
MIs with it: `CultistMelee_Hair_MI` (the Cultist Melee's hair; Wounds off, Microtile (R) on), `CultistArcher/
Grenadier_Fringe_MI`, `Armored_Swat_01_Helmet_MI`, zombie pants. Others: `Master_Alphatest_M`
(`Evangelo_Head_01_HairPaint_MI`, one-sided, default lit), `Master_Hero_Outfit_M` (`Sharice_Elite_02_Hair_MI`, no
BC-alpha switch). Chosen: **a copy of `CultistMelee_Hair_MI`**: two-sided + dithered mask like hero hair, wounds off,
white vertex colours on the cultist's hair (so none needed), textures BC7 sRGB RGBA (A = coverage) / BC5 normal /
BC7 PBR (R AO, G roughness, B metallic, like hero PBR). Its master's vector defaults are neutral (Skin/Cloth Color
white, wound ellipsoids at 999). Overrides: the microtile detail intensities 0 (`Detail BC/Roughness/NRM Intensity
(R)`, `Detail Height Scale (R)`: a fabric pattern otherwise), `Fuzz Spread 0`, `Fuzz Brightness 0.1` (the cloth sheen
of Sharice Elite 02's hair). Copying an MI keeps its cooked static permutation (the native tail); the copy renders.

Pipeline (`b4bmodel.py --hair texture`, the default; `--hair tint` = §9):
- `TexTool.hair_texture_set`: for a slot whose master is `Master_Hair_M`, the cultist MI and its 3 textures are copied
  into the template's folder as `<HairMI>Color_MI` / `<HairMI>Color_BC_T|N_T|PBR_T` (`b4bmod rename`, texture refs
  rewritten), the textures composed from the hair set's tiles, the scalars set, and the meshes' hair slot repointed
  to the copy (same code as adopting a shared MI). The template's hair MI is left alone; `--as` copies the new MI and
  textures into the outfit folder like any written package. Hair vertex colours stay white.
- `b4bfit.py compose` role `hairbc`: RGB = each tile's base colour (sRGB), A = its alpha (alpha map, else base colour
  alpha); see-through texels get the colour of the nearest strands (pull-push over a mip pyramid) so mips and
  filtering don't pull black/white fringes into the strand edges. Colour at the set's size (up to `--max-texture`),
  normal/PBR at 256 unless the model has such maps.
- **Base colour factors** were ignored for every material (a texture x colour material came out as the bare
  texture): now read from the Multiply node Blender's glTF importer builds (`basecolor_factor`) and applied in linear
  space in `basecolor`, `hairbc` and `hairmm`. VRoid hair and brows are grey textures x the MToon `_Color`: Shino's
  hair is a dark navy (`_Color` 0.098, 0.141, 0.22), not light blue. VRM 0.x `_Color` is an sRGB value that VRoid also
  copies unconverted into glTF's linear `baseColorFactor`; MToon renderers (UniVRM, three-vrm) read it as sRGB, so
  `vrm0_colors` (b4bfit import and preview.py) sets the Multiply node to its linear value.
- `preview.py`: `*HairColor_BC*` textures drive alpha; `.vrm` opens directly (with the MToon colours).

Live (lane 1, Proton, `B4B_GPU=4090`, `multi.sh 3`: host and client 2 with 4 add-ons via `addons_dir=`, client 3
`addons=0`; add-ons `shino` (texture), `shinotint` (tint), `bulky` (texture), `bulkytint` (the vertex colour test)):
- Fort Hope, dusk: client 2 as `shino`, brought in front of the host (`fh_host_sees_shino_front.png`,
  `fh_shino_face_zoom.png`): dark navy hair, cut bangs with see-through edges, **amber irises and brown lashes** (on the
  hair slot, own colours); the same shot with `shinotint` (`before_after_fh.png`, left): one teal-grey colour, irises
  and lashes in the hair colour (empty black eyes). `bulky` (`fh_bulky_pair.png`, left): the afro's curls with their
  texture and see-through edges against the sky, not one grey-brown blob.
- Evansburgh B saferoom: `shino` from client 2 (add-on; `m_c2_sees_shino.png`): hair, hair clip in its own cyan, FP
  arms holding a bat; client 3 (no add-on; `m_c3_noaddon.png`, `fh_noaddon_sees_vanilla.png`) sees the survivor's
  retail pieces (`mdl dump`: `3P_Doc_Torso_00`, `3P_Holly_...` base meshes).
- No sorting artefacts (masked: drawn in the opaque pass); no `LogMaterial`/default-material/Fatal lines in the three
  logs. Side-by-side with Blender: `prev/shinoh_cmp.png` (source VRM / texture / tint), `prev/bulky_cmp.png`.
- `tools/e2e.py --quick --no-lock`: 14/14.
Screenshots and previews: `~/.local/share/b4b-coop/hairwork/{shots,prev}/` (not committed).

Limits: cloth shading instead of the hair shader (no anisotropic hair highlight, no pixel depth offset); the mask is
the texture's alpha at the 0.333 clip, dithered (soft alpha falls into a dither pattern that TAA smooths); hair
strands still move rigidly with the head (no dangle bones).

## 12. Faces: talking and blinking (models-faces, 2026-09-26)
Custom heads now follow the game's face animation (lip-sync, blinks, expressions). Code: `modkit/blender/b4bface.py`
(called from `b4bfit.py` for `character --mode 3p`, `--face auto|off`), bind positions written by `skmgltf.py`
(`set_bone_positions`, now hierarchy-correct), face poses read by `modkit/poseasset.py`, preview `blender/preview.py
--face`.

**Game side (static + live):**
- `FacePoses_<Hero>_PoseAsset` (names vary: `FacePoses_Doc_PoseAsset_NEW`, `Faceposes_Karlee_PoseAsset`): additive,
  22 poses = expressions (Anger, Disgust, Fear, Joy, Sad, Surprise, Wounded, Relaxed, Interested, Caring, Concerned,
  Playful, MouthOpen_TEMP) + visemes (AH, CH, E, EH, ER, L, N, MBP, OW); `PoseContainer` tagged: PoseNames (FSmartName =
  FName), Tracks (157 bone names), Poses[].LocalSpacePose sparse via `TrackToBufferIndex` (int->int map). Holly: AH =
  jaw 12 deg + lip_lower_l/r 12 deg + lip corners 0.6 cm; MBP = lip_lower 46 deg, lip_upper 20 deg; E = jaw 13 deg;
  eyelids 3-16 deg in expressions. No blink pose: blinks are `3P_M_Blink_ADD_AS`; live the upper lids turn up to
  ~28 deg (`eyelid_upper_l` sampled every ~30 ms, retail and custom heroes alike).
- `3P_Biped_SK` BoneTree (179 nodes, same order as its ref skeleton): all face bones `OrientAndScale`, spine/limbs
  `Skeleton`, root/ik/hair `Animation`. So a face bone's translation comes from the mesh's own reference pose; moving
  the face bones' bind positions per mesh is honoured, and the additive poses rotate them in place. No
  `RetargetBasePose` in cooked meshes; the engine recomputes inverse bind matrices from the ref skeleton.
- Face bones (`face_master` children, 57): eye_l/r and eyelid parents at the eye centre with eyelid_*_01..03 children on
  the lids, jaw hinge 7 cm behind the lips (Holly/Doc share the same female head skeleton positions), lip bones on the
  lip surface, cheeks, brows, nose, ears, teeth, tongue.
- Speech: `DialogueComponent.SayLine(SpokenLineParams{ResponseName})` builds the Wwise event `DX_<Voice>_<Response>`,
  which doesn't match the real events (`Dx_B_Walker_Ping_Affirmative_01`): no line plays. The comm wheel does:
  `PlayerWaypointsComponent.ServerSpawnCommWheelPing(pc, transform, action)` with Approve=2, GoHere=4, Warning=7,
  Ready=8, Wait=9 makes the hero speak (Thank=10: "Failed to find comm wheel action definition"); per-line cooldowns.
  The lip-sync plays on the host and on clients (host's hero, jaw up to 4.4 deg / lip_lower 25 deg on both machines).

**Modkit (b4bface.py):**
1. Before the fit changes the model: per-vertex displacement of a mouth-open and a blink shape key (names via
   `gltf_morph_names`: Blender 5.1 imports primitive-level targetNames as `target_N`; VRM 0.x blendShapeGroups /
   VRM 1.0 expressions give the presets a/aa, blink, blink_l/r), the rig's jaw-side weights (jaw, chin, lip.B,
   lower teeth, tongue), and the fitted positions of the source bones (rest joint x the fit transform; the evaluated
   pose is skewed by Rigify constraints).
2. Landmarks on the model in a face frame (forward, left, up at the head joint): eyes (source eye bones, else eye
   material islands, else ball-shaped eye-sized islands; painted eyes get their centre pushed back), lips/corners/
   crease (Rigify lip bones, else the mouth-open key: top of the moved region at the front = lower lip, first static
   vertex above = upper lip, sides of the moved band = corners; else the middle profile: deepest dent between the lip
   bumps), chin/nose from the profile. The template's landmarks are its bones (crease from its profile).
3. Warp model->template (uniform scale + offset, Gaussian RBF residual, sigma 0.35 x eye distance) for the weights, the
   inverse for the bones. Each head vertex takes the template's face weights at its warped place (4 nearest template
   vertices of the same class: skin, mouth interior; eyeball islands go whole to eyeball_l/r); upper/lower lip is
   decided on each side separately (template: its own jaw weights; model: mouth-open key, else vertex normals at the
   crease, else the rig's jaw weights, else the lip line), so no weights are interpolated across the lips. The
   vertex's head weight is split into face bones; a blink key's moved vertices go to eyelid_upper/lower_<side>.
4. All 57 face bones move to the warped template positions (jaw hinge by the uniform part only; a per-axis scale put
   Doc's hinge 7.6 cm back); the importer writes them into the mesh's reference skeleton (`manifest.extras.face_bones_m`
   -> `skmgltf.import_gltf(bones=...)`).

**Results (offline preview with the hero's poses, `preview.py --face`):** Rigify MPFB "shorty" on Doc: AH opens the
mouth cleanly (first tries: a hanging centre flap from upper-lip vertices with jaw weights, fixed by per-side lip
classification), OW rounds, MBP presses, Joy smiles, a 30 deg blink closes the lids. VRoid Shino on Holly: mouth from
the `A` key, lids from `Blink` (big anime eyes close about halfway at 40 deg). MPFB game-engine "bulky" and the Mixamo
monster: eyes/mouth from geometry and the profile (monster eyes not found: scaled from the template).

**Live (lane 2, Proton, `multi.sh 2`, both instances with the add-ons, Evansburgh B):** `face` dev command
(testing.c): face bone deltas from the ref pose (`GetDeltaTransformFromRefPose`), ref positions
(`GetRefPosePosition`), `face comm <action>`, `face look <hero#>` (camera in front of a hero's face). The custom
heroes' ref poses carry the moved bones; host's shorty speaking (comm wheel): jaw 4-10 deg, lip_lower 25 deg, seen
on host and client; blinks 17-28 deg on the custom heads. Screenshots (presented frames, client looking at the host,
not committed): `~/.local/share/b4b-coop/faces/shots/` (`t_*`, `seq_*` talking with subtitles, `b_1` open /
`b_24` blinking, `faces_live_summary.png`).

Open then (fixed in §12b): the lower lip pouted on 25 deg lip_lower curls; models without a mouth interior showed a
hole; painted anime eyes only half closed; eyes guessed without eye mesh/blink key. `face say` (SayLine) doesn't find
events (naming above).

### 12b. Face polish (models-facepolish, 2026-09-26)
Test heads: shorty (MPFB Rigify) on Doc, Shino (VRoid) on Holly, Shino without her `FaceMouth` material (no mouth
interior), bulky (MPFB game engine, fused lips, no key, no face bones) on Hoffman, the Mixamo monster on Walker.
Offline check: `preview.py --face ... --face-view mouth|eyes` (close-ups centred on the moved lip/eye bones; back faces
of one-sided materials are now left out, so a missing mouth interior shows as the hole the game shows), retail
comparison = the template glb with its own ref skeleton and the same pose asset frames.
- **Regression found:** since own proportions (§13) the template head's skin reaches down the stretched neck, so the
  bounding-box seed put shorty's eyes 4.6 cm too high and its Rigify eye bones were no longer accepted (eyes "scaled
  from the template", 0 eyeballs). Seed now cuts the template at the model's cut height; the mouth/nose/chin seeds are
  then shifted by the offset of the eyes found.
- **Lip curls** (CH/ER/N/OW lip_lower 26 deg, MBP 46): weighted mean offset of the lower-lip weights from their bone was
  (-0.35, 0.1, -0.5) cm, reach 3.5-4.4 cm; retail (0.0, -0.1, -0.1) cm, reach 2.1-2.4 cm. So the curl swung a region
  reaching the chin around a pivot above it (the pout, the dented chin in MBP). `fit_lip_bones`: each lip bone keeps
  weight only within the survivor's own reach for that bone (85th percentile .. max, scaled by mouth width; the rest to
  the jaw / head), then the bone moves to the centre of what it moves, offset like the survivor's (<= 0.8 cm). After:
  offsets (0.0, -0.2, -0.1) cm, reach 1.9-2.2 cm, i.e. retail's; the lips curl in place.
- **Lip line on low-poly heads:** the middle profile now comes from the mesh's cut with the middle plane, bins holding
  only the back of the head are dropped, and among the dents the one where the survivor has its crease (ratio nose tip ->
  crease -> chin bottom, measured on the template) wins over the deeper fold under the lower lip (bulky: crease was
  found at the mentolabial fold 1.6 cm too low, so the jaw dropped the chin and both lips stayed shut). Corners are
  searched at the lips' front, not in a deep mouth slit. At the corners the upper/lower split blends into the template's
  corner weights and the jaw share is averaged over neighbours (spiky triangles when the mouth opened).
- **Mouth-open key sign:** VRoid's `A` also lifts the upper lip; only what the key moves down counts as the jaw side
  now (Shino's upper lip edge had gone down with the jaw, stretching the philtrum).
- **Seams:** open-edge vertices of head meshes that sit on another mesh's vertex (VRoid face/neck) get the same
  weights (average), so the jaw line doesn't open.
- **Mouth interior:** none found (no mouth/teeth/tongue material, < 12 vertices behind the lips facing into the mouth)
  -> a cavity is added to the lip's mesh: narrow opening 6 mm behind the crease, widening inside, closed 4 cm back,
  128 faces facing inwards, upper half on the head, lower half on the jaw, UVs on the darkest texel of the lip material's
  base colour (Shino: the lip line, reddish). `--mouth on|off` forces it. Shino without FaceMouth: AH shows a red mouth
  instead of the hole; models with an interior (Shino, MakeHuman heads) are left alone.
- **Blink with the model's key:** the blink key is captured as vectors (world, through the fit by a least-squares affine
  map of the head), and the pivot of `eyelid_upper_<side>` goes where one 28 deg turn moves the lid edge onto the keyed
  place (+8 % and the lower lid's lift, which the game's blink leaves alone); each lid vertex takes the share of the turn
  its key moves it. Shino: pivot 7.2 cm behind the lid, closed at 26-28 deg (before: a third closed at 28, half at 60).
  Expressions turn the lids too (3-16 deg): with the long lever big eyes squint more in Anger.
- **Eyes not found** (monster: no eye bones/mesh/key): warning in the log, eyelid/eyeball weights left off (no blinking
  skin patch); `--face-eyes x,y,z;x,y,z` (each pupil, Blender coordinates of the file as imported) tags the nearest 40
  vertices before the fit and maps the points through it (Kabsch); an eyeball mesh near the point gives the centre.
  Check on shorty: given pupils -> eye centre (7.5, 3.4, 4.3) cm vs its ORG-eye bone (7.5, 3.4, 4.4).
Live (lane 2, Proton, `B4B_GPU=4090`, `multi.sh 2`, both with the shorty/shino/shinonm add-ons via `addons_dir=`, Fort
Hope, no native change): shorty (client) talking seen by the host: lips curl in place, blinks close; Shino (host) seen by
the client, game slowed with `/slomo 0.15`: `eyelid_upper_l` peaks at 29.3 deg (q.y 0.253), its ref pose now 5.8 cm
behind `eye_l`, the frame at the blink shows both big eyes shut; shinonm (no interior) speaking, jaw 4.8 deg: the mouth
opens onto the red cavity, no hole. Screenshots (not committed): `~/.local/share/b4b-coop/facepolish/shots/`
(`sblink_*`, `nm_t*`, `talk_*`), previews and `facepolish_live_summary.png` in `.../facepolish/prev/`.
Left: bulky's upper-lip corners still stretch into thin spikes at the widest AH (MakeHuman inner-mouth geometry on the
skin material); the monster's tentacle face gives no usable profile (mouth landmarks are guesses).

## 13. Characters keep their own proportions (models-proportions, 2026-09-26)
Goal: a model with other proportions than the survivor (short legs, long neck, a giant torso) looks like itself in
third person instead of being stretched onto the survivor's joints (§10: monster legs x1.8, shorty neck x0.39).

**The game already retargets per mesh.** `3P_Biped_SK`'s `BoneTree` (tagged property, `TranslationRetargetingMode`
per bone; `upkg.py props 3P_Biped_SK.uasset`, names from its native `FReferenceSkeleton` after the guid flag):
- `Skeleton` (translation from the **mesh's** reference skeleton, rotation from the animation): spine_isolate,
  spine_01..03, neck_01/02, head, upperarm/lowerarm/hand, thigh/calf/foot/ball, all twist bones, backpack, weapon,
  camera, sleeve/chain/gasmask bones.
- `OrientAndScale` (animated translation turned and scaled by mesh ref / skeleton ref): **pelvis** (so the root motion
  height follows the mesh's pelvis height), clavicles, fingers, face bones, `ik_hand_root/gun/l/r`.
- `AnimationRelative`: `ik_foot_l/r`; `Animation`: root, ik_foot_root, hair_*, camera_movement.
- Retail uses it: Holly/Doc/Karlee/Mom 3P meshes have a smaller skeleton than Walker/Hoffman with **other ratios**
  (pelvis 94.8 vs 102.9 cm, head joint 154.2 vs 165.0, calf 39.6 vs 46.4 (x0.85), thigh 41.5 vs 42.4 (x0.98),
  lowerarm 23.7 vs 26.1, clavicle 11.5 vs 14.0) with the same local bind **rotations** and the same animations. IK
  bones sit on their FK bone (`ik_hand_gun` = `ik_hand_r` = hand_r, `ik_hand_l` = hand_l, `ik_foot_l` = foot_l);
  `weapon` keeps x and scales z with the shoulder height (118.98 vs 128.21).
- `FP_Biped_SK`: **every bone `Animation`** and all FP meshes (Holly's too) share one bind pose: first-person arms can't
  keep other lengths (the animation would put the bones back); they stay fully fitted.

**Pipeline** (`b4bfit.py`, 3P only; `--proportions own` default, `fit` = the old full fit, a number = geometric blend
of the segment lengths):
- After the uniform scale (head-to-feet height), each mapped segment is only **turned** onto the template's direction
  (torso and neck still as one piece each), joints chained through the **template's** hierarchy from the pelvis
  (source hierarchies differ: Rigify hangs thighs and shoulders off `ORG-` bones), then the whole model is moved so its
  soles (lowest foot/ball-weighted vertices) are on the template's ground.
- `rebind_template`: the template armature and meshes are posed onto those joints (per segment turn + stretch;
  unmapped bones with their mapped ancestor, IK bones with their FK bone, `weapon` z by the shoulder-height ratio) and
  that becomes their rest pose, so twist weights and the face step compare against a template of the same shape.
  Bind rotations are unchanged (animations need them).
- The moved bones go to manifest `extras.bind_bones_m`; `skmgltf.import_gltf(bind_bones=)` writes them into the mesh's
  `FReferenceSkeleton` (positions only, rotations kept; parents first) before reading the glTF (face bones after), so the joint check
  compares against the new bind pose (0.000 cm on all test models).
- Log: `proportions (model / survivor, same height): legs x0.67, torso x1.64, neck x0.70, arms x0.94 ...` then pelvis
  and head joint heights and the ground shift; `bind skeleton: N bones moved`.
- Unrigged models: unchanged (no joints to keep; placed by bounds, weights from the template).

Test set (proportions model / survivor after the uniform scale, 3P):

| Model | Survivor | legs | torso | neck | arms | shoulders | pelvis (survivor) |
|---|---|---|---|---|---|---|---|
| Horror Monster (Mixamo) | Walker E00 | x0.67 | x1.64 | x0.70 | x0.94 | x1.21 | 75.0 cm (102.9) |
| "shorty" (Rigify) | Doc E00 | x0.91 | x1.08 | x2.58 (Rigify neck starts at the chest) | x0.94 | x1.22 | 73.6 cm (94.8) |
| "bulky" (UE names) | Hoffman E00 | x1.03 | x1.11 | x0.73 | x1.02 | x0.91 | 99.1 cm (102.9) |
| Shino (VRoid) | Holly E00 | x1.08 | x0.95 | x0.69 | x0.98 | x0.79 | 104.2 cm (94.8) |

The old full fit stretched each of these back to x1.00 (monster calves x1.8, torso x0.62; shorty neck x0.39; every
model's neck ~x1.4). Offline: joint error 0.00 cm, the cooked meshes' bind rotations identical to the template's
(`skm.py` re-read), IK bones on their FK bones, `preview.py --pose test` bends cleanly. Previews:
`~/.local/share/b4b-coop/proportions/prev/{monster_front_cmp,shorty_cmp}.png` (source / fit / own / own posed).
First Rigify run chained through the source hierarchy: its thighs and shoulders hang off `ORG-` bones, so they stayed
at the template's joints while the spine moved (a giraffe neck); fixed by chaining through the template's tree.

Live (lane 1, Proton, `B4B_GPU=4090`, `multi.sh 3`: host and client 2 with the add-ons (`addons_dir=`), client 3
`addons=0`; Evansburgh B saferoom; outfits `monster`, `shorty`, `bulky`, `shino` (own) and `monsterfit`,
`shortyfit` (`--proportions fit`); screenshots `~/.local/share/b4b-coop/proportions/shots/`, not committed):
- `before_after_monster_saferoom.png`: bot in `monsterfit` (left: stilt legs, short torso) next to client 2 in
  `monster` (right: the model's big torso and short legs), both standing on the floor, heads at the same height.
- Client 2 as `monster`, `shorty`, `shino` seen by the host holding the SMG with both hands (`h_sees_monster_front.png`,
  `pair_shorty.png`, `pair_shino_bulky.png`: bot `bulky`); bot `monster` running back to the host (`run_crops.png`).
- Client 2 first person as `monster` (arms always fully fitted): SMG, AR, pistol (`c2_fp_smg.png`,
  `c2_fp_primary.png`, `c2_fp_pistol.png`), grips as before.
- Client 3 without the add-on: `mdl dump` shows the survivors' base pieces (`3P_Walker_Torso_02` ...), screenshot
  `c3_noaddon.png`.
- No `LogSkeletalMesh`/`LogAnimation`/Fatal lines in the three logs. `tools/e2e.py --quick --no-lock`: 14/14.

- After merging the face rig (§12, same run of `b4bfit`: body bind skeleton first, face bones after, both in the
  mesh's reference skeleton; joint check 0.000 cm): Fort Hope, client as `shino` (face rigged, own proportions) and
  `monster` seen by the host (`fh_merged_shino.png`; `before_after_monster_forthope.png`: the old stretched fit from
  §10 left, now right). `e2e.py --quick --no-lock` 14/14 on the merged build.

Tradeoffs / limits: the hitboxes are the template's physics bodies on the moved bones (sizes unchanged); very long or
short arms keep their own length, so the 3P hands may sit off the weapon's grips (the test set's arms are within 6 % of
the survivors'; `--proportions 0.5` or `fit` if a model shows it); first-person arms always take the survivor's
proportions (FP skeleton); unrigged models are placed by their bounds as before. Not seen: crouch, reload, climbing.

## 14. Secondary motion: swinging hair, skirts, coats and capes as cloth (models-cloth, models-coats, 2026-09-26)
Custom long hair and skirts used to move rigidly with head/pelvis. Code: `modkit/blender/b4bdangle.py` (called from
`b4bfit.py` 3P, `--hair_bones`, `--cloth`), `modkit/cloth.py` (template inspection, clothing asset writer, render
mapping), `modkit/uprops.py` (tagged-property tree parse/write, byte-identical on retail cloth/PA exports),
section tagging in `skmgltf.py` (glTF nodes `B4BCLOTH_*` -> own section). Flags: `b4bmod survivor ... --hair-physics
auto [--hair-swing 0..1] --cloth auto|off|MAT,...` (default auto since 2026-09-26: on where the template supports it).

**How retail heroes get secondary motion (static):**
- **RigidBody node**: `3P_Hero_ABP` has 3 `AnimNode_RigidBody` (OverridePhysicsAsset None = the mesh's physics
  asset, BaseBoneSpace `spine_03`, ComponentLinearAcc/VelScale 0). Bodies with `PhysType_Simulated` in hero PAs:
  `3P_Holly_PA` hair_00..02 (capsule r 2.8, 1 kg, damping 8/8; constraints to head swing 20/30 deg, hair_01 15/20,
  twist locked), `3P_Holly_Elite_06_PA`/`3P_Walker_Elite_03_PA` hair_00..02, `3P_Doc_Elite_03_PA` hair_00..01,
  `3P_Mom_PA` hair_00/02_l/r (pigtails), backpack/gasmask on others. Holly E00's chain: hair_00 (-7, 0.4, 154) behind
  the head -> hair_02 (-14.7, 0.4, 143) (UE cm, heroes face +X). Live: `face 4 hair_00 hair_01 hair_02` on a bot
  shows 10-23 deg deltas changing sample to sample (retail Holly E00 and our mesh alike). No AnimDynamics nodes.
- **Cloth**: 30 retail SKM LODs have cloth (`*_Cloth_PA` physics assets for collision: Holly, Holly E04, Doc, Doc E03,
  Jim Torso 01, Karlee E06, Walker E07, Dan). Holly Elite 00 (Shino's template): `ClothingAssetCommon` exports
  `Cloth_Sleeve_L_0` (12 sim verts), `Cloth_Sleeve_R_1` (10), `Cloth_Flannel_3` (76 verts / 116 tris: the flannel
  tied round the waist, MaxDistance ~300 = free, collision `3P_Holly_Cloth_PA` pelvis/spine_01/thighs) with one
  `ClothConfigNv` each (flannel: StretchLimit 1.2, LinearDrag 0.8, solver 60 Hz, GravityScale 3). LodMap [0,1,2]
  (mesh LODs 3-4 no cloth); sections flannel1..3 (Body / Body_2sided MIs) are the render sections, Cloth1..3 hidden.
- Everything is **tagged properties** (no cooked fabric: NvCloth cooks at load): per clothing LOD a
  `ClothLODDataCommon` {PhysicalMeshData {Vertices, Normals, Indices (uint32), WeightMaps {1 MaxDistance, 2/3
  backstop, 4 anim drive}, InverseMasses, BoneData (tagged ClothVertBoneData: NumInfluences, BoneIndices[12] u16
  into UsedBoneNames, BoneWeights[12]), MaxBoneWeights, NumFixedVerts, SelfCollisionIndices}, CollisionData (empty:
  collision comes from the PA)} + native tail TransitionUpSkinData, TransitionDownSkinData (FMeshToMeshVertData
  arrays: LOD k's up/down, empty at the ends). Top: PhysicsAsset, ClothConfigs {name: ClothConfigNv}, LodData, LodMap,
  UsedBoneNames, UsedBoneIndices (mesh bone indices), AssetGuid (= the sections' ClothingData guid).
- Render side (skm.py already parsed it): a cloth section has one FMeshToMeshVertData (64 B: 3x float4 bary+dist for
  position/normal/tangent, u16 SourceMeshVertIndices[4], float Weight 0, pad) per vertex; the LOD's cloth vertex
  buffer = all cloth sections' records in section order, index mapping per section (u32 offset, u32 base vertex;
  (0,0) for others). **Formula checked on retail**: position = sum_i b_i (V_i - N_i d) with V/N the stored sim
  vertices/normals (median error 0.01-0.12 cm on the 3 Holly sections; with +N: 2 cm); stored normals =
  -cross(B-A, C-A) (all three assets), i.e. the mapping runs on the runtime normals cross(B-A, C-A).

**What a custom mesh can use without blueprints:** (a) its own cloth data in the template's existing clothing asset
export (same package, so `--as` copies it; keeps the asset's config and collision PA) - built; (b) weights on a
template's simulated hair chain - built; new physics bodies/bones would need our own physics asset and skeleton
(not done).

**Built:**
- Hair (`rig_hair`): hair-slot vertices (not lashes/brows/iris) behind the head centre (full at 3 cm behind, none 3 cm
  in front) and below the chain root (full 7 cm below) blend onto the chain; along it by height (tent weights per
  bone, below the last joint = last bone). `cloth.hair_chains` finds chains in the template's PA (simulated bodies
  named hair under `head`, bodiless bones in between included). Shino: 3859 of 13057 hair vertices.
- Cloth (`cloth_region` + `cloth.apply`): skirt faces below spine_01+2 cm split into `B4BCLOTH_*` objects (own section
  on the same slot, moved to the slot's two-sided variant `<x>_2sided_MI` if the template has one); sim mesh =
  7 rings x 20 sectors from the waist to 1 cm below the hem, radius = outermost skirt vertex per bin + 4 mm; ring 0
  fixed (MaxDistance 0), others MaxDistance = depth x length x 0.45 (Shino: up to 15 cm); sim vertices skinned with the
  template body's nearest weights limited to pelvis/spine_01/02/thighs (waist ring: hips only); InverseMasses from
  vertex areas (mean mass 1). Written into the template's biggest clothing asset (Holly E00: Cloth_Flannel_3), 3
  identical clothing LODs with identity transitions, LodMap [0,1,2]; render records solved exactly (fixed point on d,
  bind-pose reconstruction error 0.000 cm), cloth vertex buffer + BuffersSize recomputed. The other clothing assets
  stay unbound (as before).

**Live (lane 2, Proton, `multi.sh 2`, host + client with the add-on, Evansburgh B saferoom; bots given the outfit with
`model <#> shinocloth`, made to run with the new dev `face walk <hero#> x y z` = SimpleMoveToLocation on the bot's
controller):** the skirt sways while turning/walking and flares/trails behind while running, the long hair swings on
hair_00..02 (deltas 5-23 deg while moving), on host and client; no cloth/skeletal-mesh errors in either log. First
build (Body MI, one-sided) showed see-through slits between pleats when the cloth moved; on the two-sided MI none seen.
Screenshots (presented frames, not committed): `~/.local/share/b4b-coop/cloth/shots3/` (`w_best.png` running,
`w_grid.png` sequence, `cl_grid.png` client view), first build `shots/run3_skirt.png` (slits). `e2e --quick`: 14/14. After merging §13 (own proportions, default: Shino's bind
skeleton moved, waist 114 cm) the same build and live run: skirt and hair still swing (`shots4/running_best.png`,
`shots4/grid.png`); the chain/sim are built after the template is rebound, so they use the model's joints.

Open: long hair can dip into the back when the chain swings (`--hair-swing`); both flags default to auto (Dan,
2026-09-26: on when the model/template supports it). The two limits noted here first (only outfits with a clothing
asset; the ring closing open coats) are gone: §14b.

### 14b. Cloth on any outfit: new clothing assets, open panels, capes (models-coats, 2026-09-26)
**Package writer** (`upkg.Package`): `add_name`, `add_import` / `import_object` / `import_class`, `add_export(class,
template, outer, name, data, sbs/cbs/sbc/cbc)`; `save()` rewrites the whole header when the structure changed
(`rebuild_header`): name map with the engine's two name hashes (`FCrc::Strihash_DEPRECATED` on the MSB-first
`CRCTable_DEPRECATED`, upper-cased; `FCrc::StrCrc32` 4 bytes per char), import map, export map (104 B; new exports
get bNotAlwaysLoadedForEditorGame 1 as retail), depends map (empty per new export), asset registry bytes, preload
dependency array re-laid (FirstExportDependency = running offset or -1), summary counts/offsets, Generations
(export count, name count), BulkDataStartOffset; serial offsets move by the header's size change, new export data
goes after the last export. New entries are appended, so no FName/FPackageIndex inside existing export data changes.
Checks: `upkg.py rebuild-check` = header rebuild of an unedited package byte-identical: **1695/1695** extracted
packages (meshes, textures, MIs, physics assets), 0 name-hash mismatches. A Walker Elite 00 mesh with an added
clothing asset: UAssetAPI parses 3 exports and re-writes it byte-identically (`tools/modkit/uassetrt check`),
CUE4Parse (`assetcheck`) lists SkeletalMesh + ClothingAssetCommon + ClothConfigNv, the engine loads it (below).
**What a new clothing asset is** (retail reference: `3P_Walker_Elite_07_SKM` vs `3P_Walker_Elite_07_NoCloth_SKM`,
same mesh without cloth: tags differ only by `MeshClothingAssets`): exports ClothingAssetCommon (outer = mesh,
template `Default__ClothingAssetCommon`) and ClothConfigNv (outer = the asset); imports `/Script/
ClothingSystemRuntimeCommon`, `/Script/ClothingSystemRuntimeNv`, both classes and CDOs, the collision physics asset
(package + object); preload deps: config sbc [class, CDO] cbc [asset]; asset cbs [PA, config] sbc [class, CDO] cbc
[mesh]; mesh cbs += asset. `cloth.new_clothing_asset` copies the tagged layout and NvCloth config of Walker Elite
07's coat asset (the donor, extracted on demand; names re-pointed into the outfit's package by `uprops.remap`), new
AssetGuid (md5 of mesh name + index, reproducible), appends it to the mesh's `MeshClothingAssets` tag (added after
`ShadowPhysicsAsset` when missing). Outfits that have clothing assets still reuse theirs first; each garment gets
its own asset (a skirt + a cape = two).
**Collision.** Two sources, both written: the physics asset (garments hanging from the waist on a new asset:
`3P_Walker_Elite_07_Cloth_PA`, thighs/calves/pelvis capsules, imported) and our own capsules in each clothing LOD's
`CollisionData` (Spheres + SphereConnections, BoneIndex = index in `UsedBoneNames`, LocalPosition in the bone's bind
space) fitted to the model: per bone (pelvis, spine_01..03, thighs, calves; twist bones counted with their limb) the
median distance of the skin mostly weighted to it from the joint segment, x0.95. Every collision bone is also put
in `UsedBoneNames` (retail lists its PA's calf bones without weights on them). Live check that the engine uses our
capsules: a build with only our capsules, radii x3 (`B4B_CLOTH_COLLISION=own B4B_CLOTH_COLLISION_SCALE=3`), showed
the coat pushed out into a wide bell around the legs; normal radii keep it hanging. Capes use only our capsules
(the hero PAs carry bodies a model may not have, e.g. Walker's simulated backpack r17 on the back).
**Simulation meshes** (`b4bdangle.cloth_regions`): garments by material name or colour image name (skirt/dress/kilt,
coat/jacket/trench/duster/robe/poncho..., cape/cloak/mantle), bottoms that are skirt-shaped, or `--cloth MAT[:cape|
:lower],...`. Kind: `cape` when it hangs behind from above the chest and leaves the chest bare (simulated below
spine_03, top row on spine_03/clavicles/neck), else `lower` (below spine_01+2 cm, top row on pelvis/spine). Faces
skinned mostly to arm bones (a coat's sleeves) stay skinned. Angular coverage in 72 x 5 deg bins over 10 height
bands: sectors covered in < 1/3 of the bands form the gap; >= 25 deg = **open panel** over the covered arc (the
front left open), else the closed tube as before. Faces inside the gap (where a coat's fronts still meet below the
waist) stay skinned. Columns ~18 deg, rows ~9 cm (5-10), each column's hem from the garment (longer at the back),
radius = outermost garment vertex per bin + 4 mm. Max distance at the hem: tube 0.45 x length, open panel / cape 0.8.
A one-sided open garment (< 20 % of its faces facing the body) gets a reversed copy of each face 1.5 mm inside, so
its inside draws when it swings (tubes keep using the outfit's two-sided material variant when it has one).
`blender/preview.py ... --sim <work>/fit3p/manifest.json` draws the simulation meshes as red wire over the model.
**Test characters** (local, not committed; `tools/modkit/testassets/garments.py` makes CC0 garments from a body's
cross-sections): (1) a game-rip character with a floor-length open coat on Walker Elite 00 (no clothing asset):
open panel 315 deg, 10x19, 6672 cloth faces; (2) the CC0 MPFB man with a generated long coat open below the waist
on Hoffman Elite 00: open panel 325 deg, 9x19; (3) the CC0 VRoid girl (skirt) with a generated cape on Mom Elite 04:
skirt tube 5x20 + cape panel 185 deg 10x11, two new clothing assets. Mapping error 0.000 cm on all.
**Live** (lane 1, Proton, `B4B_GPU=4090`, `multi.sh 3`: host + client 2 with the add-ons via `addons_dir=`, client 3
`addons=0`; dev `face walk` also moves a player's own hero: SimpleMoveToLocation works on its PlayerController):
Fort Hope, the host in the long coat walking and turning in 3P: the coat tails swing out to the side with folds and
settle; client 2 sees the host in the coat, client 3 sees the survivor's own pieces (its hero list: retail meshes);
client 2 walking in the cape: the cape sways behind the legs; Evansburgh B saferoom: a bot in the long coat walking,
vaulting onto crates and jumping down (coat bunches during the vault, hangs again after). No crash, no cloth or
skeletal-mesh errors in the three logs (only the retail LogCustomization "Forcing load" lines).
`e2e.py --quick --no-lock`: 14/14. Screenshots local (`~/.local/share/b4b-coop/characters/`).
Found on the way: a flat-shaded model reaches the fit as a triangle soup and its LODs come out shredded (decimation
can't collapse unshared edges); test prefixes render LOD1 (`r.SkeletalMeshLODBias=1`), so it showed at once. Fixed
in §14c (weld before decimation). Per-garment tuning and self-collision: §14c.
Open: the panel is a cylinder around the body axis, so a cape spread wide over the arms or a very full skirt is
approximated.

### 14c. Watertight LODs, per-garment cloth tuning (models-lodcloth, 2026-09-26)
**LOD tearing.** Not only flat-shaded models: glTF/VRM imports arrive split along UV seams and hard edges too
(Blender's glTF importer does not merge), FBX rips partly. Metric (scratch `lodcheck.py`: per LOD glb, weld by
position at 0.01 mm, open-edge length): before, VRoid Shino LOD0 7.9 m -> LOD1 122 m / LOD3 174 m, CesiumMan 0 -> 13 /
38 m, the cape test 21 -> 145 / 185 m, game rips 132 -> 167 / 178 m (only FBX meshes with shared vertices, e.g. the
monster, were clean). `b4bfit.weld_for_decimation` (once per mesh, the base for every LOD; LOD0 stays the source):
bmesh `weld_verts` on corners with the same position (0.01 mm) and skin weights (quantised 1/20; a magazine touching
the receiver in one object stays loose); faces that exist twice with opposite winding (double-sided rips) weld as
two layers (a plain weld deleted the second face: tested, 2 -> 1). Corners keep their own UVs (loop data), so the
glTF export splits again at UV/material seams. Normals: faces whose three corner normals were within 1 deg of the
face normal stay flat (`sharp_face`), edges where two faces' corner normals differed > 5 deg at a shared point become
sharp, custom normals cleared (Blender recomputes on the decimated surface). The one-sided garment's reversed inside
copy (§14b) is offset along the per-position normal (it came apart on a flat coat). After: every LOD1-3 within
+-10 % of LOD0's open length on all 12 test builds (Shino 9.9 / 10.7 / 11.0 m, cape 20.7 / 21.4 / 21.5, flat coat
63.2 / 63.7 / 43.7, rips 141 / 142 / 138, monster/cesium/blocky 0). Previews LOD1-3 of all: no holes, flat faces flat.
Tried and dropped: a Decimate vertex group protecting UV seams/boundaries (inverted, factor 0.02-0.2): boundaries
exact but fans of long triangles over the garments and 13 % over the triangle budget.
**Cloth tuning.** Retail references (tagged `ClothConfigNv` values differ little from the UE 4.25 class defaults:
Walker E07 coat Damping 0.5, SelfCollisionStiffness 0.1 (radius 0 = off), CollisionThickness 1.1; Doc E03 jackets
Damping 0.5, TetherStiffness 1.1; Holly E00 flannel GravityScale 3, LinearDrag 0.8, StretchLimit 1.2, 60/30 Hz;
Karlee E06, Holly E04 all defaults; no retail asset has SelfCollisionIndices). What sets them apart is the max
distance map, per fifth of height from the top: Walker E07 coat (64 cm) 0.6 / 17 / 57 / 93 / 100 cm, Doc E03 lower
jacket (58 cm) 0 / 11 / 31 / 60 / 75, Karlee E06 (47 cm) 0 / 0 / 1.4 / 4.3 / 5, Holly E04 (29 cm) 0 / 1 / 2.6 / 6 / 7;
inverse masses area-based (hem ~1.5). `cloth.garment_profile` picks the garment from b4bdangle's facts (kind, closed
tube or open panel, `hem_leg` = hem on the legs, 0 hips / 0.5 knees / 1 ankles): skirt (tube, hem above the knee),
long skirt (below), jacket tails -> long coat blended by hem (0.2..0.7), cape. Table `GARMENTS` (on top of
`NV_DEFAULTS`): max distance at the hem x length with an exponent (coat 1.0 ^1.6 like Walker's ramp, tails 0.35 capped
8 cm like Karlee/Holly E04, skirt 0.45 ^1, cape 1.0 ^1), heavier hem (inverse masses / (1 + (k-1) depth^2): coat
x2.5, cape x2, long skirt x1.5), damping (skirt 0.4, coat 0.6, tails 0.7, cape 0.25), gravity (coat 1.5), drag and
inertia (coat drag 0.35 / inertia 0.7: follows the body, flings less; cape drag 0.05 / inertia 1: trails and lifts off
the back; cape friction 0, bend stiffness 0.6). `write_config` writes every value explicitly into the asset's config
export (found through `ClothConfigs`), also into a template's own (Holly E00's flannel config no longer gives Shino's
skirt gravity x3). Self-collision for long coats and capes: radius 0.4 x the 10th-percentile free edge (<= 3 cm),
stiffness 0.5, indices by the engine's BuildSelfCollisionData rule (free vertices >= radius apart): coat 2.6 cm on
152 vertices, cape 3.0 cm on 99. Package: UAssetAPI re-writes it byte-identically, rebuild-check 0 differ.
`B4B_CLOTH_TUNE='{json}'` overrides (experiments).
**Live** (lane 1, Proton, `B4B_GPU=4090`, `multi.sh 3`: host + client 2 with 12 test add-ons via `addons_dir=`,
client 3 `addons=0`; flat-shaded variants of the CC0 coat and cape tests built for this): Fort Hope, host in the flat
cape seen from its 3P camera at rest and running, the flat coat on client 2 (LOD1, intact); Evansburgh B saferoom,
the bot in the floor-length open coat running and turning (hem swings out and trails, hangs again after), then in the
flat cape + skirt (cape trails off the back while running, skirt flares on the turn, both settle); host view with
`r.SkeletalMeshLODBias` 1/2/3: the flat cape model whole at every LOD; client 2 sees the bot's cape, client 3 retail
meshes. No cloth/skeletal-mesh errors in the three logs (only the retail map's QADashBoundary streaming errors).
`e2e.py --quick --no-lock`: 14/14. Screenshots local: `~/.local/share/b4b-coop/lodcloth/shots/`.
Not measured: the exact NvCloth effect of each value (only visual); the ClothConfigNv defaults are UE 4.25's, not read
from the game's class default object (all values are written, so they don't matter).

## 15. Models from game rips (2026-09-26)
Test: a rigged FBX ripped from another game (two outfits, a Mixamo re-rig), local only. What such files look like and
what the survivor pipeline now does (all generic, `modkit/`):
- Materials `Material #25` ...; the FBX image nodes point at `<name>.fbm/head.png` (or DDS next to the FBX): textures
  came through the image nodes already; auto slots read the image names (hair/eye/head). Names that say nothing
  (`all_color` = arms/hands/torso skin) need `--slot`.
- Objects without a material (teeth/tongue/mouth using the body image): labelled `none` (inspect, probe, fit);
  default dropped with a printed hint; `--slot none=<slot>`, `--tex none=<prefix>` (inspect takes `--tex` too).
  Before: silently put on the first slot -> `--atlas ... doesn't list it`.
- DDS BC1/BC5 (DXT1/DXT5) load in Blender 5.1 as they are (same orientation as the PNG exports); `png_size` reads
  the DDS header. Companion maps by name (`head` -> `head_n.dds`, `head_ao.dds`; `all_color` -> `all_n`, `all_ao`).
  Normal maps: RGB (mean B ~1) as is, two-channel/DXT5nm with Z rebuilt, anything else (a hair `_n` with mean B
  0.50) left out. Checked the rip's green channel on the lips (upper lip underside G 0.405, lower lip top 0.564):
  OpenGL style, so no `--normal-dx`. Hair opacity lived in `hair_n.dds` alpha (colour DXT1 opaque): used for
  materials whose names say hair.
- UVs: every face of the rip at v -1..0 (the old atlas clamp flattened them all). Islands (union of faces sharing a
  vertex with the same UV) moved by whole tiles; per-face moves for islands still outside; tiling islands get
  `repeat` [nx, ny] (max 4) in the tile, the FP run takes the 3P run's repeats (`tiles.json` / `tiles_fp.json`);
  remaining faces squeezed (logged). Same image -> one tile (sha1 of the file: `head.dds` = `head_f.dds`).
- Normals: custom split normals opposite to the winding on 21 of 22 objects (faces outward by the centre test):
  rendered black in previews (and would in game); negated (`normals_split_custom_set`). The merged second outfit
  (one object) was fine.
- Unit scale: FBX armature scale 0.0254 on centimetre data (head joint 421 cm): only a message now (the fit scales
  uniformly anyway).
- Face rig: a hat brim and coat collar with head weights were taken as skin (generic material names) and the head's
  bounds included the neck -> lips found at the chin. Now: labels = material + colour image names, clothes/hats
  skipped (fallback if nothing is left), seeds from the eyes when the bounds-scaled eyes miss the found ones by
  > 2 cm, the middle profile sampled on the faces (a coarse mesh left 3-bin holes 7 cm deep), and the lip line
  walked down from the subnasale (upper lip bulge, crease, lower lip) when the nose stands out > 8 mm; the deepest
  dent alone was the lower lip / chin fold. Regression (3P fit, old vs new, 8 corpus models): VRMs unchanged
  (shape-key mouths), bulky/rigify better (lip 0.2 vs -3.8 cm, eyes found), monster lips no longer behind the face.
- `preview.py`: colour textures whose alpha is a mask for the game's shader rendered black (Cycles premultiplies
  an unlinked alpha): alpha mode channel-packed.

Live (lane 1, Proton, `B4B_GPU=4090`, `multi.sh 3`: host + client 2 with both add-ons via `addons_dir=`, client 3
`addons=0`; Walker Elite 00 template, `--as`, own proportions; add-ons 145 / 97 MB at 4096): Fort Hope and
Evansburgh B, both outfits seen by the other player (face close-ups, full body idle), client 3 sees the survivors'
base pieces; host FP (gloved hands, SMG) and client FP (coat sleeve, bat); the host's 3P aiming a pistol seen by the
client; a bot in the outfit (`/model 4 <name>`) holding a rifle and running (`face walk`). Talking: the jaw bone moves
during a voice line (up to 1.1 deg measured on one line; under the hat brim nothing visible). No Fatal/skeletal-mesh
errors. `e2e.py --quick --no-lock`: 14/14. Screenshots stay local (`~/.local/share/b4b-coop/characters/`).
Open: the coat has no cloth (open front: `--cloth` would close it; the Walker template has no clothing asset anyway); auto slots can't know that an unnamed image is
skin.

## 16. Hands-off builds: texture sizes, placement without names, determinism (models-determinism, 2026-09-26)
Goal: an FBX in, one `b4bmod survivor` run, no `--slot`/`--tex`, small add-ons, same bytes every time. Test set: the 8
models of §10/§15's regression (4 VRoid, Horror Monster, two MPFB, CesiumMan) on varied templates (Holly E00, Walker
E00/E01, Mom E00, Karlee E02, Hoffman E00, Doc E00) plus the two game-rip outfits of §15 (Walker E00).

Texture sizes (the old rule was `source x grid`, capped 4096: two 2048 tiles -> a 4096 atlas, a 2x2 grid for 2-3 tiles,
constant normal/PBR maps at 4096, 1024 images enlarged):
- Retail sizes (`b4bmod info`): hero Head/Arms/Gear 2048, Body 4096 (Walker, Holly, Hoffman E00); hair masks <= 2048.
- b4bmodel packs each atlas before the fit (`tiles.json` `layout`: canvas + rects per material; b4bfit uses the rects,
  the FP run gets the same layout): tile = its images' pow2 size (non-square kept: 4096x2048 stays 2:1), guillotine
  packing tallest first into the smallest square/2:1 canvas where the tiles keep >= 85 % of their size, at most the
  retail texture's size. Over the cap the tile with the highest texel density (image texels its UVs use / area on
  the fitted model, head materials /4) is halved first, halved tiles that fit again are grown back; equal tiles scale
  together. Each role then gets its own size: halved from the canvas while every tile's image of that role keeps its
  resolution (no normal/PBR images -> 256 flat). `--max-texture N` overrides the cap (also above retail).
- compose (Blender numpy) and `b4bmod texture` handle non-square textures (pow2 sides).

Placement without names (`auto_slots`; the log gives the reason per material):
- inspect: per material, rasterized UV coverage (128^2) of its colour image -> `look` {cover, clear (see-through
  share of the texels it uses), skin (skin-colour share), color}; `own_objects` (objects with only that material).
  Objects without a material: the image where their UVs land on painted texels no material uses (`none -> all_color.png
  98 %` on the rip; teeth/tongue region). A colour-linked image with normal-map texels is used as the normal map.
- `--probe regions` (3P fit without face/cloth, ~7-15 s): per material and per template slot the area share by body
  region (dominant template bone -> head/torso/arms/hands/legs/feet), plus each material's area.
- Rules after the name rules: alpha cards need see-through texels where the faces draw (`look.clear >= 0.05`: VRoid
  shoes with 40 % transparent image but 4 % under their UVs were on the hair slot before); skin by look (>= 50 % skin
  texels, not on the feet: brown leather); skin with < 10 % head -> the template's non-head skin slot (Walker
  `ArmSkin`, Holly `Arm`); clothes -> the non-gear cloth slot with the best region overlap (Walker E00: Arms 68 % arms
  29 % hands, Body torso/legs/feet); head-worn -> gear; cloth slots with the same colour texture count once (Holly's
  flannel1-3); a material placed by look joins the slot of other materials with the same image.
- Result: every model builds with zero flags; the rip outfits get exactly the `--slot` mapping they needed before
  (all_color skin -> ArmSkin, arm accessories -> Arms, TheHat -> Gear, bare teeth/tongue -> ArmSkin with all_color);
  the VRoid/MPFB/monster/Cesium assignments are unchanged except body skin -> Holly's Arm slot (Shino) and AvatarSample
  E's shoes -> Body. Preview renders (front/back/side + face, `preview.py --textures`) of old and new builds match.

Determinism: b4bfit iterated sets of bone names; Blender's Python randomizes string hashes per process (and ignores
PYTHONHASHSEED without `--python-use-system-env`), so equal-weight influences came out in another order (2 vertices of
the rip's LOD0: JOINTS_0/1 swapped -> a different pak). `run_blender` now passes `--python-use-system-env` with
`PYTHONHASHSEED=0` (other PYTHON* variables dropped). Two full builds of the 10 models: byte-identical paks.

| Model (template) | before | after |
|---|---|---|
| Sendagaya Shino (Holly E00) | 182.9 MB | 31.8 MB |
| Base Male (Walker E01) | 165.9 MB | 18.8 MB |
| AvatarSample E (Mom E00) | 161.4 MB | 19.5 MB |
| Sakurada Fumiriya (Karlee E02) | 168.9 MB | 24.7 MB |
| Horror Monster (Walker E00, one 2048 set) | 37.6 MB | 37.6 MB |
| MPFB bulky (Hoffman E00) | 150.0 MB | 38.5 MB |
| MPFB short Rigify (Doc E00) | 200.7 MB | 34.5 MB |
| CesiumMan (Walker E00) | 32.9 MB | 4.6 MB |
| game-rip outfit 1 (Walker E00) | 144.9 MB, 12 `--slot`/`--tex` | 51.3 MB, no flags |
| game-rip outfit 2 (Walker E00) | 97.3 MB, 8 `--slot` | 40.0 MB, no flags |

Open: the pipeline's extraction isn't safe for several builds at once on one extract folder (two parallel runs
rewrote the same package: "being used by another process" / an empty read); one build at a time is fine. A game rip
whose skin image is 4096x2048 now gets at most the template's 2048 arms texture (`--max-texture 4096` keeps more).
Open: auto slots can't know that an unnamed image is skin. (The coat's cloth: done in §14b, open panel on a new
clothing asset.)

## 17. Character regression suite (`tools/charsuite.py`, 2026-09-26)
The model-mods counterpart of `tools/e2e.py`: every test character built, previewed and worn in game, one command.
- **Manifest** (models can't be committed): `~/.local/share/b4b-coop/characters/suite.json` (or `--manifest`,
  `$B4B_CHARSUITE`); format in the tool's docstring, CC0 example with sources/licences `tools/charsuite-example.json`.
  Entry: `name` (outfit, `/model <name>`), `model`, `outfit` (`Hero/Elite_NN`, `TU11/Hero/Elite_NN` or a `/Game/...`
  3P path; FP next to it unless `fp`), `flags` (extra b4bmod args; the table counts them), `expect`, `mission`.
- **Run**: `B4B_LANE=2 B4B_GPU=4090 tools/charsuite.py --twice --vanilla --install` (~28 min builds for 13 characters
  with `--twice`, ~6 min game). `--only a,b`, `--no-build` (reuse `--build-dir`), `--no-preview`, `--no-game`,
  `--mission N`, `--no-lock`. Builds run one at a time in their own extract folder
  (`~/.local/share/b4b-coop/charsuite/extract`, `B4B_EXTRACT`), so they never collide with another build.
- **Checks**: build exit + add-on's `outfit=` name + size, `--twice` byte-identical paks, preview.py stills (3P + face);
  game: add-ons into the lane's game folder (the lock's backup restores the player's own), host `addons_policy=any`,
  `multi.sh 2` (+ `addons=0` 3rd with `--vanilla`); each add-on's content class from the host log (anything but
  cosmetic = FAIL: a default host refuses such a joiner); per outfit host `model <name>`, `models: hero slot N wears
  outfit <name>` on host and client, the outfit mesh on the host's hero in both `face` lists, the vanilla client
  shows the survivor and no "wears" line; new Fatal / `b4bcoop/` / skeletal-mesh / material / cloth / add-on / pak
  errors since the first swap (startup lines are the baseline), all instances answering. Mission: `mission Easy`,
  `ready` (ends the character select), the `mission` entries worn again (host FP with a weapon, 3P, client view).
- **Framing**: a client can't teleport its own pawn (the server corrects it), so the host stands in front of each
  client's hero (`face look <idx>` on the host) and the client's camera sees the host full body (260 cm) and face
  (70 cm). Dev `face` now lists each hero's position and `(you)` to pair heroes across machines.
- **Output**: table, `contact.png` (one row per character: previews, host 3P, client face/body, no add-ons, mission),
  `results.json`, build/preview logs, `notes.txt` (build warnings), game logs, agent transcript in
  `/tmp/b4b-charsuite[-l2]-<time>/`; exit 1 on any failure.

First run (models tip 868015a, lane 2, Proton; builds `/tmp/b4b-charsuite-l2-20260926-050615`, game
`/tmp/b4b-charsuite-l2-20260926-054405`): 13 characters (the 11 of the example + the two game-rip outfits of §15), all
built with **0 flags**, all **deterministic**, 3.6-52.9 MB, all worn and seen by host and client in Fort Hope, vanilla
client sees the survivor, Evansburgh for shino + rip outfit 1, **0 new log errors**. The smaller textures (§16) look
right in game. One failure: `avatar_e` (Mom E00) is classed **gameplay** by the game (`DrenchAssetUserData` on the
copied `Mom_Elite_00_A_FP_Skin_MI`), so a host with the default `addons_policy=cosmetic` refuses a joiner with it
("Host allows cosmetic add-ons only"); found because it blocked the first session. Next: the classification (native
`addons.c` content scan) or the pipeline's copy of that MI should treat the Drench user data on an outfit's own MI as
cosmetic.

## 18. First real-world models: slots, garment detection, FP arms, mesh budget (models-fixB, 2026-09-26)
Two bought/downloaded characters (a rigged FBX with a slit dress + jewellery, 116k vertices; an unrigged FBX with
jeans, boots and a shirt, 100k vertices; both on Holly Elite 00, local only) exposed four general pipeline bugs.
- **Clothes on a skin slot.** A beige dress material has mostly skin-coloured texels, and "dress" was in none of the
  clothing name lists, so `auto_slots` put it (and its lining) on `Arm` ("skin away from the head"). `Arm`'s master
  is `Master_Hero_Head_M`, which has no `bUsedWithClothing` (read from the Material export: Outfit_M and Hair_M have
  it, Head_M only `bUsedWithSkeletalMesh/StaticLighting/MorphTargets`), so the engine drew the cloth sections with
  the default material: `Master_Hero_Head_M missing bUsedWithClothing ... Default Material` in the log, dark grey dress.
  Fix: garment words in the material name (dress, skirt, gown, coat, jacket, robe, cape, lining ...; `GARMENT_RX`)
  always mean clothes; `master_uses_clothing()` reads the flag from each template slot's master (extracting it when
  needed; name guess as a fallback) and b4bfit gets `--cloth_slots`: a material on any other slot is never made cloth
  (logged, with the `--slot` to fix it).
- **Names trusted blindly for cloth.** a material named `<Name>_Dress_Necklace` matched "dress"; the dress material was also used
  by a belt, panties and a thigh strap, which went into the skirt's cloth sections (mapping error 26 cm, spike
  triangles at the hips); the other model's boots use a material named `dress` (skirt with its hem at -1 cm, a boot
  flapping off the foot). Fix (`b4bdangle.cloth_materials`): per object x material, a garment must hang like one
  (`hang_check`: top >= crotch + 3 cm, bottom <= crotch - 12 cm, >= 10 cm wide below the crotch; capes: >= 12 cm tall)
  and objects/materials named like accessories (`ACCESSORY_RX`: necklace, belt, strap, bracelet, panties, boot, heel
  ...) stay skinned; `cut_region` only cuts the objects that passed. Auto mode skips a material with no passing part
  and says `--cloth <mat>`; a `--cloth` list keeps the passing parts, or all if none passes.
  Result: dress cloth mapping error 3.5 cm (was 26), boots skinned.
- **No FP arms without `--fp`.** `b4bmod survivor` now adds the outfit's own `FP_` mesh (same folder, 3P_ -> FP_,
  looked up in the pak index, as charsuite does) and prints it; warns when there is none; `--fp none` = game arms.
  `b4bmodel.py` does the same from the extract folder.
- **No LOD0 reduction.** LOD0 was the model as is (174k / 168k triangles vs Holly Elite 00's 94k). `make_lods` now
  budgets LOD0 at the template's LOD0 (render vertices and triangles; `--max-verts N`, `--keep-density`): the welded
  model (as for the distance LODs) is collapse-decimated with UV seams marked (the decimator keeps seams as edges)
  and the head's vertices in an inverted vertex group (weight 0.5, factor 0.002: at ratio 0.5 the face keeps ~0.65,
  the body ~0.4; an inverted weight of 0 never collapses, so hair skinned to the head stayed whole); cloth sections
  stay whole (their two-sided layers must match); distance LOD ratios stay relative to LOD0. FP arms get the FP
  template's budget.
  | model | LOD0 before | LOD0 after | 3P mesh | FP mesh | add-on |
  |---|---|---|---|---|---|
  | dress model | 116k v / 174k t | 68k v / 94k t | 24.2 -> 13.8 MB | 0.8 MB | 90.9 -> 105.6 MB (dress now in the 4096x2048 Body set, was 1024 in Arm) |
  | jeans model | 100k v / 168k t | 59k v / 94k t | 20.7 -> 9.3 MB | 11.2 -> 2.5 MB | 139.6 -> 119.5 MB (67 MB = three 4096 Body textures) |
  The rest of the size is textures at the model's own resolution (capped at the retail size): `--max-texture 2048`.
- `preview.py --sim` framed the model tiny: the Wireframe modifier's even offset threw thin sim triangles metres out
  and the framing took them in. Now no even offset, and the framing ignores the sim wire.
- Live (lane 2, Proton, `tools/charsuite.py --only ... --mission 3`, `/tmp/b4b-charsuite-l2-fixB-1`): 7/7 PASS, 0 new
  log errors (no `bUsedWithClothing` line): both new models worn by host and client in Fort Hope and Evansburgh (dress
  textured with its pattern, boots on the feet, FP arms from the default `--fp`), shino/avatar_e/bulky/coattest/
  capetest unchanged (same sizes, skirt/coat/cape still cloth). Open: the unrigged model's FP mesh is its whole body
  (FP arm extraction keeps everything for unrigged models), so no arms show in first person.

## 19. The same two models: jaw, eyes, torn chin, unrigged FP arms, hair physics (models-fixA, 2026-09-26)
Two new local test characters (a paid Auto-Rig Pro FBX, 310 bones, 61 mapped; an unrigged Daz FBX), both on Holly
Elite 00, now in the local suite manifest. General bugs they showed, all in `modkit/`:
- **Mouth pulled wide open, tongue out (rigged, also `--face off`)**: not the FBX's stored pose (75 bones away from
  rest, but the imported mesh is closed-mouthed at rest and in that pose; phase 2 sets every pose bone to `D @ rest`
  anyway). The cause: bones under a `BIND_ONLY` bone (the jaw: ARP `c_chin_*`, `c_lips_bot*`, `c_teeth_bot*`, `tong_*`
  under `c_jawbone.x`) own-map to the jaw, which has no `D_of`, so they got the identity and stayed at the source
  place while the head moved (own proportions: +7.6 cm). Now they take the jaw's `D_used` (its parent's transform).
- **Face looking up ~15 deg**: the head (no aim joint) took the neck chain's rotation; the model's neck leans 15.2 deg
  forward, the survivor's bind neck 0, so straightening it tipped the face up. The head now keeps the model's own
  orientation (both look ahead at rest).
- **White eyes**: the clear `Cornea` shell (no image, blend HASHED) went opaque on the skin slot over the iris. Eye
  materials named cornea/tearline/moisture, or see-through without an image, are now dropped (`b4bmodel.auto_slots`).
- **Chin torn off when talking (unrigged)**: its weights come from the template's nearest vertices, *face bones
  included*; the face rig then re-skins only vertices near the template's face (fade 8-20 mm) and left the rest with
  the template's jaw weights taken at unwarped positions (hair by the chin followed the jaw too). Copied template
  weights now give face bones to `head` (`face_bones_of`/`fold_face`); plus `b4bface.smooth_jaw_edges`: around every
  edge whose jaw share jumps > 0.3 outside the lip band, the share is averaged over the mesh within 1.5 cm (175 -> 66
  jump edges on the unrigged head, the rest on the lip line; 432 vertices blended).
- **No hands in first person (unrigged) — half done.** (a) The cut: FP weights come from the FP template, which is
  only arms, so every vertex got arm weights and `keep_arms` kept the whole body (100k verts). Now unrigged FP also
  needs the vertex's nearest template body segment to be an arm bone within 12 cm (`fp_arm_mask`), hair materials and
  materials with < 10 % of their faces on the arms are left out: 25.5k verts (template 26k). (b) Still no hands in
  game after the cut. Bisected live with the add-on's FP mesh swapped (`exp` builds, lane 1): the template's own arms
  and the rigged model's arms show; the unrigged arms don't, whatever their weights, UVs or normals; the rigged
  arms **moved 8 cm back** vanish too. So FP arms must sit on the FP skeleton's joints within a few cm (the FP view
  is tight; hands end up below it). The unrigged un-pose only turned the upper arm (straight arm vs the FP bind
  pose's 40 deg elbow; hands 8 cm short, palm-in). Now (`unpose_arms(chain=True)` + `_unpose_chain`): each segment
  (upper arm, forearm, hand) mapped onto the FP skeleton's (turn + stretch + move, applied by hand as linear blend
  skinning because Blender poses can't hold the stretch), the hand by a PCA frame (fingers, thumb side, palm normal),
  hand/finger bones only inside the model's hands. Hand centroid now matches the template's (5, 4, -6 cm from the hand
  bone vs 6, 3, -7). Last live look: sleeves now in view, but distorted (a sleeve stretched up past the gun), hands
  not yet clean. Next: check the forearm/sleeve weights after the chain un-pose (sleeve vertices near the elbow),
  maybe weight sleeves by segment instead of nearest template surface; then re-run the suite. Also fixed on the way:
  imported meshes keep the template's constant extra UV channels (FP Arms slot UV1 = 0) and never get bounds smaller
  than the template's (neither alone was the cause). 8 influences are fine (the rigged FP with 8 draws hands).
- **Hair physics "no simulated hair bones" on Holly E00**: false; `3P_Holly_PA` simulates hair_00..02 (all 28 hero PAs
  scanned: Holly, Holly E06, Walker E03 hair_00..02; Doc E03 hair_00..01; Mom, Mom E07 pigtails; backpack/gasmask
  elsewhere). The physics asset simply wasn't extracted (not among the mesh's material/texture references), and
  `hair_chains` returned no chain. `dangle_args` now extracts it; `hair_chains` no longer adds a second chain for a
  simulated bone below another one (Mom: `['hair_02_l']` twice). Docs (meshes.md) were right; Mom E07 added.

## 20. Finishing the two models: FP arms from the 3P fit, arm surface fit, textures, mouth landmarks (models-fixA, 2026-09-26)
Same two local models as §19 (rigged ARP FBX, unrigged Daz FBX). Offline checks with a new preview mode:
`preview.py --fp hold` poses the FP skeleton's arms by two-bone IK onto fixed hand targets in front of the `camera`
bone (right hand on the grip, left on the fore-end) and renders from the camera (90 deg); the retail FP mesh rendered
the same way is the reference (charsuite now adds it to the previews: column "preview FP"). It reproduced the live
§19 finding offline: the unrigged sleeve stretched up past the hands, the rigged arms matching the retail ones.
- **Unrigged FP arms = the rigged path.** The 3P fit of an unrigged model (the regions probe, `--rigged_out`) saves the
  model on the survivor skeleton (bind pose, template weights) as `<work>/rigged3p.blend` (object names kept); the FP
  probe and FP fit load it (`--rigged`, identity bone map of the survivor names) and fit it like a rigged model: every
  joint exactly on the FP skeleton's (`pose fit: max joint error 0.00 cm`). `_unpose_chain` stays as the fallback
  (no `--rigged`), used when an arm doesn't fit onto the model's surface (below). The blocky figure's FP build had
  failed since §19 (`fp_arm_mask` cut every face: "no skinned mesh in the glTF"); when that cut leaves nothing the
  faces skinned to the arms are kept (12 per arm box, as before §19).
- **Arm surface fit (unrigged, 3P and so FP).** The tip guess (`left arm is 4 deg from the template's pose`) left the
  hands 8 cm behind and below the template's: the model's hands took forearm and thigh weights. `fit_arm_pose` turns
  the template's clavicle, upper arm, forearm (coordinate search, mean distance of each segment's points and those
  below it to the model's nearest vertex, limits 25/70/70 deg), then the hand by frames: the model's hand = vertices
  reached over the welded mesh within 1.3 hand lengths of the wrist, on its far side (so the thigh next to a hanging
  hand isn't taken); direction wrist -> hand centre, palm normal = thinnest PCA axis (sign nearest the template's).
  Result: hand surface distance 4.6 -> 1.7 cm, 1834/1886 hand vertices; FP hands where the retail ones are. Fit worse
  than 3 cm (blocky figure: 9-10 cm) -> the old tip turn (`no fit onto the model's surface`).
- **Palm roll (rigged).** The hand's aim-only turn left the palm wherever the model had it; now also the knuckle line
  (pinky_01 -> index_01) onto the template's (`palms turned l -2 deg, r 2 deg` on the rigged model), fingers keep the
  hand's roll (parent's turn first, then the shortest turn onto their segment).
- **Wrong dress colour (rigged model).** Its FBX links the dress's colour image only to Alpha (colour socket empty), so
  the name guess took `<Mat>Inner_BaseColor` (substring of another material's set): an olive dress instead of black
  with gold vines. Now an alpha-only image whose name says base colour is the colour too, and the name guess prefers
  files named exactly after the material. Then the dress followed its necklace (same image) to Gear, where cloth
  would be refused: garment-named materials no longer count as "placed by look". Circlet (was on the Head skin slot:
  pale, "skin colours 100%") and other jewellery words (circlet, crown, tiara, bangle, armband, anklet, pendant,
  amulet) go to Gear.
- **Cheek slash in Joy/OW (rigged).** Mouth corners from the mouth-open key were the sides of everything it moves:
  model half width 4.3 cm (template 2.4), the corner bones sat on the cheeks. Now the corners are where the key's motion
  jumps across the lip slit (> 40 % of its max within 3 mm of height, 2 mm bins walking out from the middle): 2.2 cm.
  Smile, "O" and "AH" read as a mouth.
- **Chin left behind (unrigged).** The profile chin was the front-most point under the lower lip = the bottom of a
  full lower lip (0.4 cm below it); the warp then sent the real chin below the template's (5-7 cm from its surface):
  no jaw weight, the chin stayed while the lip dropped. The chin is now the bulge below the fold under the lower lip
  (-0.8 cm): the whole chin follows the jaw.
- Also: ARP jaw-side bones (`c_lips_bot*`, `c_teeth_bot*`, `tong_*`) recognised for `b4b_face_jawsrc`.
- Not fixed: the rigged model's inner upper lip hangs slightly into an opened mouth (AH), the unrigged model's lips
  part at the corners in a smile (its mouth interior is the generated one); both small at talking strength.
- Live (lane 2, isolated Flatpak Steam account, `B4B_STEAM=flatpak B4B_GPU=4090`; native gameprocess_log unchanged,
  2545 `AppID 924970` lines before and after): `charsuite.py --only <two new test models> --vanilla --mission 2 --motion`
  2/2 PASS, 0 new log errors, no `missing usage flag` / `bUsedWithClothing` line (dress cloth on the Body slot);
  host FP holds the SMG with the model's own hands and bracelets; black dress with gold vines, gold circlet in game.
  Full suite (15 characters, all with 0 flags but the rigged model's `--normal-dx`, all deterministic): 15/15 PASS
  (`/tmp/b4b-charsuite-l2-final`, after merging models 1b25e15); `e2e.py --quick` 14/14
  (`/tmp/b4b-e2e-l2-final`). New charsuite `--motion`: talking face (client view while the host's
  hero says a line) and the outfit on a bot running across the host's view.

## 21. Unrigged models that bend wrong in game: placement, simulated bones, weight source (ada-fix, 2026-10-12)
Ada Wong (RE4 remake, unrigged glTF, `--as adawong` on Karlee Elite 00) passed the suite but in game "her whole body
morphs grotesquely" when crouching or turning, with dark flaps at the hips. Three causes, found with new game-like
poses in `preview.py --pose crouch|turn|walk|aim` (each joint turned about the body's own axes; render the template the
same way to compare):
- **Placement.** An unrigged model is stood up by its bounding box, centred on the pelvis. Ada's hands reach 28 cm
  forward in her rest pose, so the box's middle sat in front of her body and the whole body ended up 10-13 cm behind
  the skeleton (calf vertices 12 cm behind the calf bone, face 12 cm behind Karlee's): every joint turned her about an
  axis in front of her, and the face rig found nothing ("lips 0.2 cm from the head joint"; v1 had guessed the face
  bones at half scale). Fix (`b4bfit`, 3P unrigged): the legs' band (25-42 % of the stature, 2 % trimmed front-back
  middle) is put on the template's same band (`unrigged: centred front to back on the legs: moved +13.0 cm`). Ada's face
  is now found (eyes from the eye mesh, mouth and chin from the profile; 15300 vertices on face bones, eyelids blink).
- **Simulated bones.** Weights come from the template's nearest surface. Karlee's physics asset simulates `backpack`
  (her jacket's back and hood, up to 73 % per vertex): Ada's back, hips, dress and holster got up to 70 % of it and
  swung with that body. Now b4bmodel reads the template's `PhysType_Simulated` bodies (`cloth.simulated_bones`) and
  passes `--sim_bones`; b4bfit gives those bones' template weights to the other bones of each vertex (or the parent)
  before anything is copied (`weights: the survivor's simulated bones backpack left out ...`); hair chains the
  model's hair is put on (`--hair_bones`) keep theirs.
- **Weight source.** Karlee Elite 00's jacket tails cover her hips and thighs: a tight dress copied pelvis weights
  there and hung like a curtain between the knees when crouching (dark tears where the leggings under it poked
  through). Karlee Elite 03 (tight suit, same skeleton and physics asset, no weights on `backpack`) gives body-like
  weights: the dress bends with the thighs. Guide: pick a tight-fitting outfit for unrigged models in tight clothes.
- Ada v2: `--outfit TU07/Karlee/Elite_03`, `--cloth off --hair-physics off --hair-pin off` (Dan: nothing on her should
  swing; the hair pin had put up to 84 % of her bob on spine_03, which stretches it when the head turns). Stills: crouch,
  turn, walk and aim clean; at full crouch a few leggings vertices poke through the dress at the back of the thigh.
- Live (lane 2, Flatpak Steam, `B4B_STEAM=flatpak B4B_GPU=4090`, new `charsuite.py --mission 1 --poses`): Ada worn in
  Fort Hope and Evansburgh, host crouching (22 cm lower), jogging, turning in place and aiming seen from the client and
  the host's 3P camera: no stretching, no flaps, nothing swinging; PASS, 0 new log errors. The leg centring also
  moves the unrigged Pink/Yellow Rangers 4-6 cm (their knees bend cleaner in `--pose crouch`); their paks are v1.

## 22. Skirts that follow the legs instead of cloth (`--skirt legs`, skirt-fixes, 2026-10-12)
Dan's report: Ariana Grande's and both Olivia Rodrigo skirts deform and separate; Doja Cat's skirt, which simply bends
with her thighs when crouching, is the look to copy. The Pink Ranger's rear "does something strange" when she moves or
crouches (a stiff block behind the hips).
- **Why Doja Cat works**: her rig (Fortnite `F_MED_TangyRadishFlame`) hangs its skirt bones under the thighs
  (`base_front_skirt_r` <- `thigh_r`) and weights most of the skirt to `thigh_*`, glutes and pelvis directly. Measured
  on her outer surface: pelvis at the waist, ~50 % thigh 4 cm below the hip joints, fully on the thigh of its side
  10-15 cm below (back slower: pelvis 0.4 at -9 cm), a hard left/right split at the front and back centre. b4bfit's
  `own_bones` sends unmapped bones to their mapped ancestor, so her skirt bones became thigh weights. No cloth: her
  `Body` material has no `dyn_skirt*` faces (GARMENT_BONES is anchored `^dyn_(main_)?skirt`, hers are `dyn_*_skirt_*`).
- **Why the others fail**: Ariana (`dyn_skirt_*_base` <- `pelvis`), SOUR Olivia (`dyn_skirt_*_base` <- `pelvis`),
  Lover Girl Olivia (`MSTR_skirt_*` <- `pelvis`) hang their skirt bones under the pelvis, so the skirt became rigid on
  the pelvis (thighs through it when crouching or walking). Ariana and SOUR also have >= 150 faces on `dyn_skirt_*`, so
  `--cloth auto` made them cloth (closed tubes, hem 0.21/0.26 of the leg): that is what tore and floated in game. Lover
  Girl was not cloth (rigid only).
- **Pink Ranger**: an unrigged FBX, weights copied from Holly Elite 00's mesh. Holly carries a bag on her `backpack`
  bone (child of spine_03, 8700 template vertices); the model's lower back and the back of its skirt flap took
  `backpack` 0.7-1.0, and the animations move that bone on its own. Not a physics body (3P_Holly_PA simulates only
  hair_00..02), so the ada-fix branch's simulated-bone filter doesn't catch it.
- **Fix** (modkit/blender/b4bdangle.py §3, §4): `--skirt legs` (b4bmodel -> b4bfit): `mark_skirt_bones` stores each
  source vertex's share on bones named skirt/skit_/dress/kilt/gown/tutu or `dyn_coat|robe|poncho` (attribute
  `b4bskirt`); `skirt_legs` (3P, after the rename/rebind, before the twist split; unrigged: after the template copy)
  blends each such vertex (and faces of materials named like a skirt/dress/coat, below spine_01 + 2 cm, not arm-skinned)
  towards `leg_weights`: pelvis 1 - s, thigh_l s * left, thigh_r s * (1 - left); left = smoothstep over +-0.6 x half
  the hip width (front/back centre shared); s at the front from hip joint + 4 cm (0) to -10 cm (1), at the back up to
  0.65 from the hip joint to -22 cm (first try: back like the front at 0.8 max: the seat came through the back of Lover
  Girl's dress when crouching), blended front/back over +-6 cm. Lower garments are then dropped from the cloth (capes
  stay). `drop_template_gear` (unrigged only, always): template vertices on `backpack`/`gasmask` get the weights of
  the nearest non-gear template vertices before any copying.
- **Offline checks** (posed previews with the ada-fix branch's `preview.py --pose crouch|walk` + a hip close-up;
  local, not committed): old builds crouching: thighs through the front of Ariana's, both Olivias', Charlotte's
  skirts, Golden Zoey's front flap rigid; new: front rests on the thighs (slight plume), back drapes, front stretches
  between the legs mid-stride, no tears. The Pink Ranger's `backpack` weights are gone (max 0 on the rebuilt mesh); the
  bone's own animation can't be shown offline.
- **Sweep** of the 94 fnx + 14 fortnite outfits by source rig (garment bones and their mapped root) and published
  build (cloth or not): rebuilt with `--skirt legs` also Charlotte (pleated skirt on pelvis bones, rigid), Golden Zoey
  (front flap rigid), Mariah Carey (closed-tube cloth dress like Ariana's). Same rig type but left alone: robes and
  coat tails (Kylo Ren closed cloth tunic, Mace Windu / Anakin rigid robes, Korra, the Mandalorians, Darth Vader's
  cape: open cloth panels; Rey, Aang, He-Man, Scorpion, Shuri, Stormtrooper Samurai flaps), and skirts already on
  thigh bones like Doja Cat's (Mother of Thorns Doja, Tour Ready Sabrina, Golden Mira).
- **Live** (lane 1, Proton, `B4B_GPU=5090`, `charsuite.py --no-build --install --crouch --mission 0` on the rebuilt
  paks; new `--crouch`: the host holds its crouch key (InputSettings `HeroCrouchKeyboard` = LeftControl, dev
  `cheatprobe press`) while the client's camera and the host's 3P camera take shots): all 7 worn by host and client,
  0 new log errors; crouching, every skirt rests on the thighs in front and drapes at the back, nothing torn or
  floating; the Pink Ranger's rear stays on her hips. Screenshots local (`/tmp/b4b-charsuite-skirts-live1`, `-live2`).
