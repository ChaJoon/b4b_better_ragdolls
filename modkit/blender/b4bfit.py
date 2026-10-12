"""Fit a modder's model (FBX, glTF, VRM, OBJ, .blend) onto a B4B template skeleton, headless in Blender. Writes glTF files
that `skmgltf.py import` turns into a cooked skeletal mesh, plus a manifest (materials -> template slots, textures,
atlas rectangles) for the texture step. Driven by b4bmodel.py (`b4bmod survivor|weapon`); usable on its own.
Guide: docs/meshes.md; how it works: docs/investigations/mesh-mods.md §6 (b4b-coop repository).

  blender -b --python blender/b4bfit.py -- character --template T.glb --source model.fbx --out DIR
        [--mode 3p|fp] [--proportions own|fit|0..1] [--bonemap map.json] [--lods 1,0.5,0.25,0.12,0.05]
        [--slot SRCMAT=SLOT]... [--drop REGEX]
        [--weights source|transfer] [--twist template|none] [--textures DIR] [--facing -y] [--atlas SET=m1,m2]...
        [--drop_mat MATERIAL]... [--face auto|off] [--mouth auto|on|off] [--face_eyes x,y,z;x,y,z] [--hair_bones a,b,c;d,e [--hair_swing 1]] [--hair_pin on|off] [--cloth auto|MAT,...]
        [--skirt cloth|legs] (legs: skirts/dresses/coat tails skinned to the pelvis and thighs, not cloth)
        [--slotset SLOT=SET]... [--tex MAT=<prefix|dir>]... [--probe 1]
  blender -b --python blender/b4bfit.py -- weapon --template T.glb --source gun.fbx --out DIR
        [--forward +x] [--up +z] [--scale fit|<factor>] [--anchor trigger|grip|none] [--part REGEX=BONE]...
        [--slot SRCMAT=SLOT]... [--lods 1,0.5] [--textures DIR]
  blender -b --python blender/b4bfit.py -- convert <in> <out.glb>
  blender -b --python blender/b4bfit.py -- inspect <in> <out.json>     objects, materials, bones
  blender -b --python blender/b4bfit.py -- compose <jobs.json>          texture sets -> PNGs (b4bmodel)

character: the source's armature is mapped onto the template's bones by name (UE4 mannequin names as is, Mixamo,
  3ds Max Biped, VRoid/VRM, Rigify DEF- bones, else a generic reading of the names; or --bonemap {"srcbone": "b4bbone"}),
  turned to face +X like the template, then posed so every mapped limb joint lands on the template's joint (rotation
  + stretch along the bone; torso and neck as one piece each); the pose is applied to the meshes, so they sit in the
  template's bind pose. Weights: the source's, renamed; other bones (twist, helpers, face, hair) give theirs to the
  mapped bone they follow; with --twist template each limb's weight is split among the template's twist bones as the
  template mesh splits it at the nearest point. An unrigged source (no armature) is stood up by its bounds, its arms
  un-posed from a T-pose/arms-down onto the template's A-pose, weights from the template mesh (named parts like
  arm-left only take that limb's bones). fp: the same against an FP_Biped export, then only faces skinned to the arms
  are kept.
weapon: rigid parts. Source objects are bound to weapon bones by name (magazine/mag -> mag, bolt, trigger,
  charging handle, ...; --part overrides; everything else -> the template's main weapon bone), turned so the barrel
  points along the template's +X and scaled to the template's length, and moved so the trigger (object named
  trigger, else --anchor none) sits on the template's trigger bone. Markers (empties or objects) named muzzle, mag,
  shell_eject, ... become socket/bone positions in the manifest.
Materials: every source material maps to a template slot (--slot, else by name, else the first slot). Several
  materials on one slot are packed into an atlas (UVs remapped into tiles; the texture step composes the images).
"""
import bpy, bmesh, json, math, os, re, sys
from mathutils import Vector, Matrix, Quaternion
from mathutils.kdtree import KDTree

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def log(*a):
    print("b4bfit:", *a, flush=True)


def parse(argv, multi=("slot", "part", "tex", "atlas", "slotset", "drop_mat")):
    pos, opts = [], {k: [] for k in multi}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a.startswith("--"):
            k = a[2:].replace("-", "_")
            v = argv[i + 1] if i + 1 < len(argv) else ""
            if k in multi: opts[k].append(v)
            else: opts[k] = v
            i += 2
        else:
            pos.append(a); i += 1
    return pos, opts


# ---- scene helpers --------------------------------------------------------------------------------------------------

def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def import_any(path):
    before = set(bpy.data.objects)
    ext = os.path.splitext(path)[1].lower()
    if ext == ".fbx":
        # Blender 5.x's FBX importer still sets Light.cycles.cast_shadow, which Cycles no longer has: an FBX with a
        # light in it (character exports often carry their scene lights) fails to import. Lights are dropped anyway.
        try:
            from cycles import properties as cycles_props
            if "cast_shadow" not in cycles_props.CyclesLightSettings.bl_rna.properties:
                cycles_props.CyclesLightSettings.cast_shadow = bpy.props.BoolProperty()
        except Exception:
            pass
        fbx_bind_matrix_fallback()
        with open(path, "rb") as f:
            ascii_fbx = not f.read(20).startswith(b"Kaydara FBX Binary")
        if ascii_fbx:                              # Blender reads only binary FBX: convert (blender/fbxascii.py)
            import tempfile
            d = os.path.dirname(os.path.abspath(__file__))
            if d not in sys.path: sys.path.insert(0, d)
            import fbxascii
            tmp = tempfile.mkdtemp(prefix="b4bfbx")
            binary = os.path.join(tmp, os.path.basename(path))
            v = fbxascii.to_binary(path, binary)
            log(f"FBX: {os.path.basename(path)} is ASCII FBX {v}: converted to binary for Blender's importer")
            try:
                bpy.ops.import_scene.fbx(filepath=binary, use_anim=False, ignore_leaf_bones=False,
                                         automatic_bone_orientation=False)
            finally:
                import shutil
                shutil.rmtree(tmp, ignore_errors=True)
        else:
            bpy.ops.import_scene.fbx(filepath=path, use_anim=False, ignore_leaf_bones=False,
                                     automatic_bone_orientation=False)
    elif ext in (".glb", ".gltf", ".vrm"):                # VRM 0.x/1.0 = glTF 2.0 with extensions (ignored)
        bpy.ops.import_scene.gltf(filepath=path)
        if gltf_bind_pose_broken([o for o in bpy.data.objects if o not in before]):
            # the rest pose Blender guesses from the inverse bind matrices is off by orders of magnitude (Sketchfab's
            # conversions of UE rips: centimetre bind matrices under a metre skeleton; pelvis at 93 m): the glTF's own
            # node pose is the model as it looks, and the mesh is bound to it
            log("glTF: the bind pose the file implies is broken (skeleton far larger than the model): using the "
                "file's node pose as the rest pose")
            for o in [o for o in bpy.data.objects if o not in before]:
                bpy.data.objects.remove(o)
            bpy.ops.import_scene.gltf(filepath=path, guess_original_bind_pose=False)
        vrm0_colors(path)
    elif ext == ".obj":
        bpy.ops.wm.obj_import(filepath=path)
    elif ext == ".dae":
        bpy.ops.wm.collada_import(filepath=path)
    elif ext == ".blend":
        with bpy.data.libraries.load(path) as (src, dst):
            dst.objects = list(src.objects)
        for o in dst.objects:
            if o is not None: bpy.context.scene.collection.objects.link(o)
    else:
        raise SystemExit(f"unsupported model type {ext} (fbx, glb, gltf, vrm, obj, dae, blend; others: open it in "
                         f"Blender and export FBX or glTF)")
    new = [o for o in bpy.data.objects if o not in before]
    shapes = {pb.custom_shape for a in new if a.type == "ARMATURE" for pb in a.pose.bones if pb.custom_shape}
    for o in list(new):                             # bone display shapes (the glTF importer's "Icosphere")
        if o in shapes:
            new.remove(o); bpy.data.objects.remove(o)
    bpy.context.view_layer.update()
    # drop animation data: we fit the rest pose
    for o in new:
        if o.animation_data: o.animation_data_clear()
    for o in stray_attachments(new):
        log(f"dropping {o.name}: a separate piece on {', '.join(g.name for g in o.vertex_groups[:3]) or 'no bone'}: a "
            f"weapon, or a prop below the body's feet whose socket the export lost (game rips)")
        new.remove(o); bpy.data.objects.remove(o)
    for o in scene_props(new):
        log(f"dropping {o.name}: not skinned and too big for / away from the body (a backdrop, floor or pedestal of the "
            f"scene the model was shown in)")
        new.remove(o); bpy.data.objects.remove(o)
    return new


def scene_props(objs):
    """Unskinned meshes of a skinned model that can't be something it wears: larger than half its height, or centred
    outside its bounding box (Sketchfab scenes: backdrop planes, a pedestal). Small ones near the body (glasses, a
    badge) stay: they are attached rigidly later."""
    skinned = [o for o in objs if o.type == "MESH" and any(m.type == "ARMATURE" and m.object for m in o.modifiers)]
    if not skinned: return []
    def box(o):
        pts = [o.matrix_world @ Vector(c) for c in o.bound_box]
        return [min(p[i] for p in pts) for i in range(3)], [max(p[i] for p in pts) for i in range(3)]
    boxes = [box(o) for o in skinned]
    lo = [min(b[0][i] for b in boxes) for i in range(3)]; hi = [max(b[1][i] for b in boxes) for i in range(3)]
    h = max(hi[i] - lo[i] for i in range(3))
    out = []
    for o in objs:
        if o.type != "MESH" or o in skinned or (o.parent and o.parent.type == "ARMATURE" and o.parent_type == "BONE"):
            continue
        a, b = box(o)
        c = [(a[i] + b[i]) / 2 for i in range(3)]
        if max(b[i] - a[i] for i in range(3)) > 0.5 * h or any(c[i] < lo[i] or c[i] > hi[i] for i in range(3)):
            out.append(o)
    return out


WEAPON_BONE_RX = re.compile(r"melee|weapon|pickaxe|handle|blaster|pistol|rifle|^ik_hand_gun", re.I)


def stray_attachments(objs):
    """Rigid props (all weights on one bone) of a skinned model that hang mostly below its feet, or that hang on a weapon
    bone: Fortnite rips carry the pickaxe (bone Melee_TwoHanded_Handle) / back bling under the root when the socket's
    transform is lost."""
    skinned = [o for o in objs if o.type == "MESH" and len(o.data.vertices) and
               any(m.type == "ARMATURE" and m.object for m in o.modifiers)]
    if len(skinned) < 2: return []
    dg = bpy.context.evaluated_depsgraph_get()
    zs = {}
    for o in skinned:
        ev = o.evaluated_get(dg)
        me = ev.to_mesh()
        zs[o] = sorted((o.matrix_world @ v.co).z for v in me.vertices)
        ev.to_mesh_clear()
    main = max(skinned, key=lambda o: len(zs[o]))
    floor, top = zs[main][0], zs[main][-1]
    h = top - floor
    out = []
    for o in skinned:
        if o is main or len(zs[o]) >= len(zs[main]): continue
        # the pickaxe's own handle bone, a holstered blaster (Fortnite dyn_blaster): most vertices on a weapon bone
        names = {g.index: g.name for g in o.vertex_groups}
        on = sum(1 for v in o.data.vertices if v.groups and
                 WEAPON_BONE_RX.search(names[max(v.groups, key=lambda g: g.weight).group]))
        weapon = on >= 0.9 * len(o.data.vertices)
        if len(o.vertex_groups) > 1 and not weapon: continue
        below = sum(1 for z in zs[o] if z < floor - 0.05 * h)
        deep = zs[o][0] < floor - 0.15 * h                                   # nothing worn reaches that far below the soles
        if (h > 0 and (below > 0.3 * len(zs[o]) or deep)) or weapon: out.append(o)
    return out


def fbx_bind_matrix_fallback():
    """Blender's FBX importer keys a mesh's bind matrices by the armature its bones ended up in; game rips with several
    skeletons in one file (Fortnite: body + pickaxe, meshes named *.ao) bind a mesh under one armature and link it
    under another, and the import dies with a KeyError. The importer assumes one bind matrix per mesh anyway: patch its
    helper nodes so that lookup returns the mesh's own (or none: the armature's bind matrix is then used). Changes
    nothing for files that import as they are."""
    from io_scene_fbx import import_fbx
    from mathutils import Matrix

    class AnySetup(dict):
        def __missing__(self, key):
            if not getattr(AnySetup, "said", False):
                AnySetup.said = True
                log("FBX: a mesh is bound under another skeleton than the one it is linked to (several skeletons in "
                    "one file): its own bind matrix is used")
            return next(iter(self.values()), (Matrix(), None))

    cls = import_fbx.FbxImportHelperNode
    if getattr(cls, "_b4b_any_setup", False): return
    init = cls.__init__

    def patched(self, *a, **kw):
        init(self, *a, **kw)
        self.armature_setup = AnySetup(self.armature_setup)
    cls.__init__ = patched
    cls._b4b_any_setup = True


def gltf_bind_pose_broken(objs):
    """A skeleton whose rest (guessed bind pose) spans over 5x the space its posed bones do."""
    def span(pts):
        if len(pts) < 2: return 0.0
        return max((max(p[i] for p in pts) - min(p[i] for p in pts)) for i in range(3))
    for a in objs:
        if a.type != "ARMATURE" or len(a.data.bones) < 3: continue
        rest = [a.matrix_world @ b.head_local for b in a.data.bones]
        pose = [a.matrix_world @ pb.head for pb in a.pose.bones]
        r, q = span(rest), span(pose)
        if q > 1e-6 and r > 5 * q: return True
    return False


def vrm0_colors(path):
    """VRM 0.x MToon: the colour is materialProperties[]._Color, an sRGB (Unity) value that exporters such as VRoid
    Studio also copy unconverted into glTF's (linear) baseColorFactor, which Blender's importer puts on a Multiply node.
    MToon renderers (UniVRM, three-vrm) read it as sRGB: so do we (VRoid hair: a grey texture x a dark hair colour)."""
    import struct
    try:
        with open(path, "rb") as f:
            head = f.read(20)
            if head[:4] != b"glTF": return
            j = json.loads(f.read(struct.unpack_from("<I", head, 12)[0]))
    except (OSError, ValueError):
        return
    props = {m.get("name"): m for m in j.get("extensions", {}).get("VRM", {}).get("materialProperties", [])}
    for mat in bpy.data.materials:
        mp = props.get(re.sub(r"\.\d{3}$", "", mat.name))
        c = (mp or {}).get("vectorProperties", {}).get("_Color") if mp and mp.get("shader") == "VRM/MToon" else None
        if not c or not mat.node_tree: continue
        lin = [x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c[:3]]
        for n in mat.node_tree.nodes:
            if n.type in ("MIX", "MIX_RGB") and n.blend_type == "MULTIPLY":
                const = [i for i in n.inputs if i.enabled and i.type == "RGBA" and not i.is_linked]
                if len(const) == 1: const[0].default_value = (*lin, 1.0)


def select_only(objs, active=None):
    for o in bpy.context.view_layer.objects:
        if o is not None: o.select_set(False)
    for o in objs:
        if o is not None: o.select_set(True)
    bpy.context.view_layer.objects.active = active or (objs[0] if objs else None)


def apply_transforms(objs):
    """Bake object transforms (FBX cm scale, axis rotations) into data, parents first."""
    for o in sorted(objs, key=lambda o: len(o.users_collection) and depth(o)):
        pass
    roots = [o for o in objs if o.parent is None or o.parent not in objs]
    select_only(objs, objs[0])
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)


def depth(o):
    d = 0
    while o.parent: o = o.parent; d += 1
    return d


def bone_world(arm, name, tail=False):
    b = arm.data.bones[name]
    return arm.matrix_world @ (b.tail_local if tail else b.head_local)


def mesh_children(arm, objs):
    return [o for o in objs if o.type == "MESH" and (o.parent == arm or any(m.type == "ARMATURE" and m.object == arm
                                                                                for m in o.modifiers))]


def bone_parented_as_skinned(arm, objs):
    """Meshes parented to one bone of the rig without weights (Rigify/Maya rigs: eyeballs on their eye bones, teeth on
    the jaw) -> skinned to that bone with weight 1, where the rest pose puts them: the fit then moves them with the
    bone's mapped owner (they used to lose their place and all weights with the rig, and vanished: no eyes to blink)."""
    out = []
    todo = [o for o in objs if o.type == "MESH" and o.parent == arm and o.parent_type == "BONE" and
            o.parent_bone in arm.data.bones and not any(m.type == "ARMATURE" for m in o.modifiers)]
    if not todo: return out
    pp = arm.data.pose_position
    arm.data.pose_position = "REST"
    bpy.context.view_layer.update()
    mw = {o: o.matrix_world.copy() for o in todo}
    arm.data.pose_position = pp
    for o in todo:
        bone = o.parent_bone
        o.parent = None; o.parent_type = "OBJECT"; o.parent_bone = ""
        o.matrix_world = mw[o]
        o.vertex_groups.clear()
        o.vertex_groups.new(name=bone).add(range(len(o.data.vertices)), 1.0, "REPLACE")
        arm.data.bones[bone].use_deform = True     # a control bone (.blend rigs) would leave them behind in the fit
        md = o.modifiers.new("Armature", "ARMATURE"); md.object = arm
        out.append((o.name, bone))
    bpy.context.view_layer.update()
    return out


# ---- bone maps ------------------------------------------------------------------------------------------------------
# A source rig is mapped onto the template's bones by name. Known naming schemes first (UE4 mannequin as is, Mixamo,
# 3ds Max Biped, VRoid/VRM, Blender Rigify DEF- bones), then a generic reader of bone names (side + body part + finger
# number, e.g. "upper_arm.L", "Arm_L", "LeftForeArm", "R_Thigh", "thumb.02.R") for everything else. Only bones that
# weight vertices (and their ancestors) are candidates, so control/helper bones of animation rigs are ignored.

B4B_MAIN = ["pelvis", "spine_01", "spine_02", "spine_03", "neck_01", "neck_02", "head",
            "clavicle_l", "upperarm_l", "lowerarm_l", "hand_l", "clavicle_r", "upperarm_r", "lowerarm_r", "hand_r",
            "thigh_l", "calf_l", "foot_l", "ball_l", "thigh_r", "calf_r", "foot_r", "ball_r"]
FINGERS = ["thumb", "index", "middle", "ring", "pinky"]
for s in ("l", "r"):
    for f in FINGERS:
        B4B_MAIN += [f"{f}_0{k}_{s}" for k in (1, 2, 3)]
REQUIRED = ["pelvis", "spine_01", "head", "upperarm_l", "lowerarm_l", "hand_l", "upperarm_r", "lowerarm_r", "hand_r",
            "thigh_l", "calf_l", "foot_l", "thigh_r", "calf_r", "foot_r"]
# bones only re-weighted (joint not moved onto the template's): moving them would shift the model's own face
BIND_ONLY = {"jaw"}
SPINE_CHAIN = {"pelvis", "spine_01", "spine_02", "spine_03"}
NECK_CHAIN = {"neck_01", "neck_02"}

MIXAMO = {"hips": "pelvis", "spine": "spine_01", "spine1": "spine_02", "spine2": "spine_03", "neck": "neck_01",
          "head": "head"}
BIPED = {"pelvis": "pelvis", "spine": "spine_01", "spine1": "spine_02", "spine2": "spine_03", "neck": "neck_01",
         "head": "head"}
VROID = {"c_hips": "pelvis", "c_spine": "spine_01", "c_chest": "spine_02", "c_upperchest": "spine_03",
         "c_neck": "neck_01", "c_head": "head"}
RIGIFY = {"spine": "pelvis", "spine.001": "spine_01", "spine.002": "spine_02", "spine.003": "spine_03",
          "spine.004": "neck_01", "spine.005": "neck_02", "spine.006": "head", "jaw": "jaw"}
for side, s in (("Left", "l"), ("Right", "r")):
    MIXAMO.update({f"{side}Shoulder": f"clavicle_{s}", f"{side}Arm": f"upperarm_{s}",
                   f"{side}ForeArm": f"lowerarm_{s}", f"{side}Hand": f"hand_{s}", f"{side}UpLeg": f"thigh_{s}",
                   f"{side}Leg": f"calf_{s}", f"{side}Foot": f"foot_{s}", f"{side}ToeBase": f"ball_{s}"})
    L = side[0]
    VROID.update({f"{L}_Shoulder": f"clavicle_{s}", f"{L}_UpperArm": f"upperarm_{s}", f"{L}_LowerArm": f"lowerarm_{s}",
                  f"{L}_Hand": f"hand_{s}", f"{L}_UpperLeg": f"thigh_{s}", f"{L}_LowerLeg": f"calf_{s}",
                  f"{L}_Foot": f"foot_{s}", f"{L}_ToeBase": f"ball_{s}"})
    RIGIFY.update({f"shoulder.{L}": f"clavicle_{s}", f"upper_arm.{L}": f"upperarm_{s}", f"forearm.{L}": f"lowerarm_{s}",
                   f"hand.{L}": f"hand_{s}", f"thigh.{L}": f"thigh_{s}", f"shin.{L}": f"calf_{s}",
                   f"foot.{L}": f"foot_{s}", f"toe.{L}": f"ball_{s}"})
    BIPED.update({f"{L} Clavicle": f"clavicle_{s}", f"{L} UpperArm": f"upperarm_{s}", f"{L} Forearm": f"lowerarm_{s}",
                  f"{L} Hand": f"hand_{s}", f"{L} Thigh": f"thigh_{s}", f"{L} Calf": f"calf_{s}",
                  f"{L} Foot": f"foot_{s}", f"{L} Toe0": f"ball_{s}"})
    for fi, (f, m) in enumerate((("Thumb", "thumb"), ("Index", "index"), ("Middle", "middle"), ("Ring", "ring"),
                                 ("Pinky", "pinky"))):
        for k in (1, 2, 3):
            MIXAMO[f"{side}Hand{f}{k}"] = f"{m}_0{k}_{s}"
            VROID[f"{L}_{'Little' if f == 'Pinky' else f}{k}"] = f"{m}_0{k}_{s}"
            RIGIFY[f"{'thumb' if m == 'thumb' else 'f_' + m}.0{k}.{L}"] = f"{m}_0{k}_{s}"
            BIPED[f"{L} Finger{fi}{'' if k == 1 else k - 1}"] = f"{m}_0{k}_{s}"
TABLES = {"Mixamo": MIXAMO, "3ds Max Biped": BIPED, "VRoid/VRM": VROID, "Rigify": RIGIFY}
TABLES = {n: {k.lower(): v for k, v in t.items()} for n, t in TABLES.items()}


def norm_name(n):
    n = n.split("|")[-1].split(":")[-1]
    n = re.sub(r"^(mixamorig\d*[:_]|bip0?0?1[ _]|def[-_]|j_bip_|armature[:_]|bone[_ ]|b_)", "", n, flags=re.I)
    return n.strip().lower()


# generic reader: tokens of a bone name
_SKIP_RX = re.compile(r"(^|[^a-z])(end|nub|tip|top_?end|twist|roll|helper|ik|pole|target|mch|org|wgt|vis|ctrl|ctl|"
                      r"tweak|fk|socket|null|dummy|adj|sec|bust|breast|hair|skirt|tail|prop|weapon|attach|hlp|jiggle)([^a-z]|$)"
                      r"|headtop|_end$|\.end$|end$", re.I)
_PARTS = [  # (regex on the side-stripped name, part); first match wins, order matters
    (r"thumb", "thumb"), (r"index|pointer|fore_?finger", "index"), (r"middle|mid_?finger", "middle"),
    (r"ring", "ring"), (r"pinky|little|small_?finger", "pinky"),
    (r"toe|ball", "ball"), (r"foot|ankle", "foot"),
    (r"shin|calf|lower_?leg|knee|leg_?lower|leg2|lowleg", "calf"), (r"thigh|up_?leg|upper_?leg|leg_?upper|leg1|upleg", "thigh"),
    (r"hand|wrist|palm", "hand"), (r"fore_?arm|lower_?arm|arm_?lower|elbow|arm2|lowarm", "lowerarm"),
    (r"upper_?arm|arm_?upper|up_?arm|arm1|uparm", "upperarm"), (r"clavicle|shoulder|collar", "clavicle"),
    (r"(^|_)arm(_|$)", "arm"), (r"(^|_)leg(_|$)", "calf_or_thigh"),
    (r"pelvis|hips?$|^hip|root_?hips", "pelvis"), (r"neck", "neck"), (r"(^|_)head(_?\d+)?$|skull", "head"),
    (r"upper_?chest|chest|spine|torso|back|abdomen|waist|belly|ribs", "spine"), (r"jaw", "jaw"),
]
_PARTS = [(re.compile(a), b) for a, b in _PARTS]


def side_of(raw):
    """('l'|'r'|None, name with the side marker removed), from Left/Right, L_/_L/.L/ L, l_/_l (lower case too)."""
    n = raw.split("|")[-1].split(":")[-1]
    n = re.sub(r"^(mixamorig\d*[:_]|bip0?0?1[ _]|def[-_]|j_bip_|armature[:_]|bone[_ ]|b_)", "", n, flags=re.I)
    for rx, sd in ((r"left", "l"), (r"right", "r")):
        m = re.search(rx, n, re.I)
        if m: return sd, (n[:m.start()] + n[m.end():])
    m = re.search(r"(^|[ _.\-])([LlRr])([ _.\-]|$)", n) or re.search(r"(?<=[a-z0-9])([LR])$", n)
    if m:
        g = m.group(2) if m.re.groups >= 2 else m.group(1)
        return g.lower(), (n[:m.start()] + " " + n[m.end():])
    return None, n


def parse_bone(raw):
    """(part, side, finger index or None) or None for a bone name the generic reader doesn't understand."""
    if _SKIP_RX.search(raw.lower()): return None
    sd, rest = side_of(raw)
    rest = re.sub(r"(?<=[a-z])(?=[A-Z])", "_", rest).lower()
    rest = re.sub(r"[ .\-]+", "_", rest).strip("_")
    base = re.sub(r"_?\d+$", "", rest).strip("_")
    num = re.search(r"(\d+)\D*$", rest)
    for rx, part in _PARTS:
        if rx.search(base):
            if part in FINGERS:
                if sd is None: return None
                k = int(num.group(1)) if num else 1
                if k == 0: k = 1                        # thumb_0 / finger0 numbering
                return (part, sd, k)
            if part in ("pelvis", "neck", "head", "spine", "jaw"):
                return (part, None, int(num.group(1)) if num else 0) if sd is None else None
            return (part, sd, None) if sd else None
    # 3ds Max Biped numbering under another prefix (Source engine ValveBiped.Bip01_L_Finger12): FingerN is finger N
    # from the thumb (0), a second digit the joint along it
    m = re.search(r"finger_?(\d)(\d)?$", rest)
    if m and sd and int(m.group(1)) < len(FINGERS):
        return (FINGERS[int(m.group(1))], sd, int(m.group(2) or 0) + 1)
    return None


def generic_map(arm, cands):
    """Generic mapping of the candidate bones. The spine chain is the path from the pelvis to the head: its bones are
    spread over spine_01..03 and neck_01/02."""
    parsed = {b.name: parse_bone(b.name) for b in arm.data.bones if b.name in cands}
    out = {}
    fingers = {}
    # limbs named only "arm"/"leg" with numbered joints (arm_joint_L_1, leg_2_R ...): spread along the chain
    for part, names in (("arm", {3: ["upperarm", "lowerarm", "hand"], 4: ["clavicle", "upperarm", "lowerarm", "hand"]}),
                        ("calf_or_thigh", {2: ["thigh", "calf"], 3: ["thigh", "calf", "foot"],
                                           4: ["thigh", "calf", "foot", "ball"]})):
        for sd in ("l", "r"):
            chain = sorted([n for n, p in parsed.items() if p and p[0] == part and p[1] == sd],
                           key=lambda n: depth_bone(arm.data.bones[n]))
            if len(chain) < 2: continue
            use = names.get(len(chain)) or names[max(names)]
            for n, t in zip(chain, use):
                out[f"{t}_{sd}"] = n
                parsed[n] = None
    for n, p in parsed.items():                      # a lone "arm" is the upper arm
        if p and p[0] == "arm": parsed[n] = ("upperarm", p[1], None)
    for n, p in parsed.items():
        if not p: continue
        part, sd, k = p
        if part in FINGERS:
            fingers.setdefault((part, sd), []).append((k, n))
        elif part in ("clavicle", "upperarm", "lowerarm", "hand", "thigh", "calf", "foot", "ball", "jaw"):
            key = "jaw" if part == "jaw" else f"{part}_{sd}"
            if key not in out or bone_rank(arm.data.bones[n]) < bone_rank(arm.data.bones[out[key]]):
                out[key] = n            # the segment nearest the root (Rigify: upper_arm.L, not upper_arm.L.001)
    # finger chains: the three bones nearest the hand, by depth (1-based numbering or not)
    # when the finger's bones hold a parent -> child chain of three, that chain: a separate bone of the same finger next
    # to it on the hand (a metacarpal like Indexfar_l beside IndexFinger1_L) shifted every finger joint by one
    def run(n, names):
        kids = sorted(c.name for c in arm.data.bones[n].children if c.name in names)
        return [n] + max((run(c, names) for c in kids), key=len, default=[])
    for (part, sd), lst in fingers.items():
        lst.sort(key=lambda kn: depth_bone(arm.data.bones[kn[1]]))
        names = {n for k, n in lst}
        roots = [n for k, n in lst if not (arm.data.bones[n].parent and arm.data.bones[n].parent.name in names)]
        chain = max((run(r, names) for r in roots), key=len)
        use = chain[:3] if len(chain) >= 3 else [n for k, n in lst[:3]]
        for i, n in enumerate(use):
            out[f"{part}_0{i + 1}_{sd}"] = n
    # plain "Leg" as thigh when there's a separate lower leg, else as calf
    for n, p in parsed.items():
        if p and p[0] == "calf_or_thigh":
            key = "thigh_" + p[1] if f"calf_{p[1]}" in out and f"thigh_{p[1]}" not in out else "calf_" + p[1]
            out.setdefault(key, n)
    # pelvis: named, else the common ancestor of both thighs
    pel = next((n for n, p in parsed.items() if p and p[0] == "pelvis"), None)
    if pel is None and "thigh_l" in out and "thigh_r" in out:
        al = ancestors(arm, out["thigh_l"]); ar = set(ancestors(arm, out["thigh_r"]))
        pel = next((a for a in al if a in ar), None)
    head = next((n for n, p in sorted(parsed.items(), key=lambda x: depth_bone(arm.data.bones[x[0]]))
                 if p and p[0] == "head"), None)
    if pel: out["pelvis"] = pel
    if head: out["head"] = head
    if pel and head and pel in ancestors(arm, head):
        path = [a for a in reversed(ancestors(arm, head)) if a != pel and pel in ancestors(arm, a)]
        necks = [a for a in path if parsed.get(a) and parsed[a][0] == "neck"]
        spines = [a for a in path if a not in necks]
    elif pel and not head:
        # no bone named head: the last of a neck chain (neck_joint_1, neck_joint_2) is the head
        necks = sorted([n for n, p in parsed.items() if p and p[0] == "neck"], key=lambda n: depth_bone(arm.data.bones[n]))
        if len(necks) >= 2 and pel in ancestors(arm, necks[-1]):
            head = out["head"] = necks[-1]
            path = [a for a in reversed(ancestors(arm, head)) if a != pel and pel in ancestors(arm, a)]
            necks = [a for a in path if parsed.get(a) and parsed[a][0] == "neck"]
            spines = [a for a in path if a not in necks]
        else:
            spines = necks = []
    else:
        spines = necks = []
    if pel and head:
        spread(out, spines, ["spine_01", "spine_02", "spine_03"])
        spread(out, necks, ["neck_01", "neck_02"])
    return out


def spread(out, bones, names):
    """Map a chain of source bones onto template chain names: first and last kept, the rest spread evenly."""
    if not bones: return
    if len(bones) <= len(names):
        for n, b in zip(names, bones): out[n] = b
        return
    for i, n in enumerate(names):
        out[n] = bones[round(i * (len(bones) - 1) / (len(names) - 1))]


def ancestors(arm, name):
    b = arm.data.bones[name].parent
    out = []
    while b is not None:
        out.append(b.name); b = b.parent
    return out


def weighted_bones(arm, meshes):
    """Bones that weight any vertex of the meshes, plus their ancestors (the rest are control/helper bones)."""
    names = {b.name for b in arm.data.bones}
    used = set()
    for m in meshes:
        idx = {g.index: g.name for g in m.vertex_groups if g.name in names}
        if not idx: continue
        hit = set()
        for v in m.data.vertices:
            for ge in v.groups:
                if ge.weight > 1e-4 and ge.group in idx: hit.add(ge.group)
        used |= {idx[i] for i in hit}
    for n in list(used):
        used |= set(ancestors(arm, n))
    return used or names


def build_bonemap(arm, targets, user_map=None, meshes=()):
    """source bone name -> template bone name, and a description of what was recognised.
    targets = set of template bone names."""
    tl = {t.lower(): t for t in targets}
    cands = weighted_bones(arm, meshes) if meshes else {b.name for b in arm.data.bones}
    options = []
    # template names as they are (UE4 mannequin / B4B rigs)
    m = {}
    for b in arm.data.bones:
        if b.name in cands and norm_name(b.name) in tl: m[b.name] = tl[norm_name(b.name)]
    options.append(("UE4 mannequin names", m))
    for tname, table in TABLES.items():
        m = {}
        for b in arm.data.bones:
            if b.name not in cands: continue
            t = table.get(norm_name(b.name))
            if t is not None and t.lower() in tl: m[b.name] = tl[t.lower()]
        options.append((tname, m))
    g = generic_map(arm, cands)
    options.append(("generic (read from the bone names)", {s: tl[t.lower()] for t, s in g.items() if t.lower() in tl}))

    def score(m):
        v = set(m.values())
        return (sum(1 for r in REQUIRED if r in v), len(v))
    kind, best = max(options, key=lambda o: score(o[1]))
    best = dict(best)
    # fill gaps of a named scheme with the generic reading (e.g. a Mixamo rig with extra spine bones)
    have = set(best.values())
    for s, t in options[-1][1].items():
        if t not in have and s not in best:
            best[s] = t; have.add(t)
    if user_map:
        srcnames = {b.name for b in arm.data.bones}
        for k, v in user_map.items():
            if v not in targets: raise SystemExit(f"--bonemap: {v!r} is not a template bone (template bones: "
                                                  f"{', '.join(b for b in B4B_MAIN if b in targets)} ...)")
            if k not in srcnames: raise SystemExit(f"--bonemap: the model has no bone {k!r}")
            best = {s: t for s, t in best.items() if t != v}
            best[k] = v
    # one source bone per target: keep the one nearest the root
    seen = {}
    for sb in sorted(best, key=lambda n: bone_rank(arm.data.bones[n])):
        t = best[sb]
        if t not in seen: seen[t] = sb
    return {sb: t for t, sb in seen.items()}, kind


_LAYER_RX = re.compile(r"^(org|mch|def|ctrl|ctl|drv|jnt|bind)[-_.]", re.I)
CONTROL_NAMES = {"head": "head", "neck": "neck_01", "hips": "pelvis"}     # animation controls named after a body part


def own_bones(arm, bmap, inv, tp):
    """{bone: the mapped bone it follows} for every bone of the source armature (None: above the pelvis, e.g. root)."""
    alias = {}
    for n in bmap:
        alias.setdefault(_LAYER_RX.sub("", n).lower(), n)
    pos = {b.name: (b.head_local.copy(), b.tail_local.copy()) for b in arm.data.bones}
    # mapped segments: joint -> next mapped joint (or the bone's own tail)
    segs = {}
    for n, t in bmap.items():
        aim = next((x for x in AIM.get(t, []) if x in inv), None)
        segs[n] = (pos[n][0], pos[inv[aim]][0] if aim else pos[n][1])

    def seg_dist(p, a, b):
        ab = b - a
        f = 0.0 if ab.length_squared < 1e-12 else max(0.0, min(1.0, (p - a).dot(ab) / ab.length_squared))
        return (a + ab * f - p).length

    above = set(ancestors(arm, inv["pelvis"])) if "pelvis" in inv else set()   # root, Global, Position ...
    owner = {}
    for b in sorted(arm.data.bones, key=depth_bone):
        n = b.name
        if n in bmap: owner[n] = n; continue
        if n in above: owner[n] = None; continue
        a = alias.get(_LAYER_RX.sub("", n).lower())
        if a: owner[n] = a; continue
        # a control bone named after the body part it moves (Rigify's `head`, `neck`, `hips`): the hair, eyes and
        # accessories rigged under it follow that part (its parents are MCH-/FK chains of the spine: the head-tails,
        # hair and eyes under Rigify's head control stuck to the chest)
        a = inv.get(CONTROL_NAMES.get(_LAYER_RX.sub("", n).lower(), ""))
        if a: owner[n] = a; continue
        po = owner.get(b.parent.name) if b.parent else None
        if po: owner[n] = po; continue
        mid = (pos[n][0] + pos[n][1]) / 2
        owner[n] = min(segs, key=lambda m: seg_dist(mid, *segs[m])) if segs else None
    return owner


def bonemap_help(arm, bmap, missing, out_dir, targets):
    """Write <out>/bonemap_template.json (every source bone, the mapped ones filled in) and return the error text."""
    os.makedirs(out_dir, exist_ok=True)
    tmpl = {b.name: bmap.get(b.name, "") for b in arm.data.bones}
    p = os.path.join(out_dir, "bonemap_template.json")
    json.dump(tmpl, open(p, "w"), indent=1)
    have = ", ".join(f"{s}->{t}" for s, t in sorted(bmap.items(), key=lambda x: B4B_MAIN.index(x[1])
                                                     if x[1] in B4B_MAIN else 999)[:40])
    return (f"the model's skeleton: no bone found for {', '.join(missing)}.\n"
            f"  recognised so far: {have or 'nothing'}\n"
            f"  Write a bone map: a JSON file {{\"<your bone>\": \"<b4b bone>\", ...}} and pass --bonemap <file>. A starting "
            f"point with all {len(tmpl)} of your bones is in\n  {p}\n  (fill in the empty ones you need, delete the rest). "
            f"b4b bones: {', '.join(b for b in B4B_MAIN[:23] if b in targets)}, fingers like index_01_l.")


def depth_bone(b):
    d = 0
    while b.parent: b = b.parent; d += 1
    return d


def bone_rank(b):
    """Which of two bones named for the same segment is it: the one nearest the root, then the one the limb goes on
    from (more bones below it). Source rigs have a helper `Bip01_R_Knee` beside `Bip01_R_Calf`, both under the thigh at
    the knee: whichever came first in the file was taken, and on Coach and Rochelle that was the helper, so the shin
    stayed on the thigh (in a crouch the lower leg swung out sideways and the boot flattened)."""
    return (depth_bone(b), -len(b.children_recursive))


# segment end used to aim each template bone (its "tail"): the head of the first of these that is mapped
AIM = {"pelvis": ["spine_01", "spine_02", "spine_03", "neck_01", "head"], "spine_01": ["spine_02", "spine_03", "neck_01", "head"],
       "spine_02": ["spine_03", "neck_01", "head"], "spine_03": ["neck_01", "neck_02", "head"],
       "neck_01": ["neck_02", "head"], "neck_02": ["head"]}
for s in ("l", "r"):
    AIM.update({f"clavicle_{s}": [f"upperarm_{s}"], f"upperarm_{s}": [f"lowerarm_{s}"], f"lowerarm_{s}": [f"hand_{s}"],
                f"hand_{s}": [f"middle_01_{s}", f"index_01_{s}", f"ring_01_{s}"], f"thigh_{s}": [f"calf_{s}"],
                f"calf_{s}": [f"foot_{s}"], f"foot_{s}": [f"ball_{s}"]})
    for f in FINGERS:
        AIM[f"{f}_01_{s}"] = [f"{f}_02_{s}"]; AIM[f"{f}_02_{s}"] = [f"{f}_03_{s}"]


# ---- template ------------------------------------------------------------------------------------------------------

class Template:
    def __init__(self, path):
        objs = import_any(path)
        self.arm = next(o for o in objs if o.type == "ARMATURE")
        self.arm.name = "B4B_Template"
        self.meshes = [o for o in objs if o.type == "MESH" and not o.name.startswith("Icosphere")]
        for o in objs:
            if o.type == "MESH" and o.name.startswith("Icosphere"): bpy.data.objects.remove(o)
        self.bones = [b.name for b in self.arm.data.bones]
        self.pos = {b.name: self.arm.matrix_world @ b.head_local for b in self.arm.data.bones}
        # slot names = glTF material names of the export (one per UE slot, in slot order) + UE material names
        self.slots = []
        for o in self.meshes:
            for m in o.data.materials:
                if m and m.name not in self.slots: self.slots.append(re.sub(r"\.\d{3}$", "", m.name))

    def hide_meshes(self):
        for o in self.meshes: o.hide_set(True); o.hide_render = True


def facing_frame(pos, lat_a, lat_b, up_a, up_b):
    """Orthonormal frame (lateral, forward, up) from joint positions; lateral = a->b."""
    lat = (pos[lat_b] - pos[lat_a]).normalized()
    up = (pos[up_b] - pos[up_a])
    up = (up - lat * up.dot(lat)).normalized()
    fwd = up.cross(lat).normalized()
    return Matrix((lat, fwd, up)).transposed()


# ---- character ------------------------------------------------------------------------------------------------------

LIMB_FAMILIES = {}
for s in ("l", "r"):
    LIMB_FAMILIES[f"upperarm_{s}"] = [f"upperarm_{s}", f"upperarm_twist_01_{s}", f"shoulder_twist_01_{s}"]
    LIMB_FAMILIES[f"lowerarm_{s}"] = [f"lowerarm_{s}", f"lowerarm_twist_01_{s}", f"elbow_twist_01_{s}",
                                      f"wrist_twist_01_{s}"]
    LIMB_FAMILIES[f"thigh_{s}"] = [f"thigh_{s}", f"thigh_twist_01_{s}", f"hip_twist_01_{s}"]
    LIMB_FAMILIES[f"calf_{s}"] = [f"calf_{s}", f"calf_twist_01_{s}", f"knee_twist_01_{s}"]
    LIMB_FAMILIES[f"foot_{s}"] = [f"foot_{s}", f"foot_twist_01_{s}"]
ARM_BONES_RX = re.compile(r"^(upperarm|lowerarm|hand|wrist|elbow|shoulder|thumb|index|middle|ring|pinky)_")
FP_HAIR_RX = re.compile(r"hair|ponytail|pony_?tail|bangs?\b|fringe|braid|\bbun\b|wig|beard|mustache|moustache", re.I)
FP_MIN_SHARE = 0.1        # unrigged FP: a material keeps its arm faces only if at least this share of it is on the arms



# ---- proportions: the model's own segment lengths (3P) ---------------------------------------------------------------
# 3P_Biped_SK takes the translation of every body bone (spine, neck, head, arms, legs, twist bones) from the MESH's
# reference skeleton (retarget mode Skeleton), the pelvis height scaled (OrientAndScale), fingers/clavicles/IK bones
# oriented and scaled: retail female heroes use it (Holly's 3P skeleton is 154 cm to the head joint, Walker's 165,
# with other segment ratios, same animations). So a 3P mesh may keep the model's own limb lengths: every segment is
# only turned onto the template's direction (the template's bind rotations stay, the animations need them) and the
# mesh's bind skeleton gets the model's joints. FP_Biped_SK animates every bone's translation (all Animation mode):
# first-person arms are always fitted onto the template. docs/investigations/mesh-mods.md §13.

GROUPS = [("legs", [("thigh", "calf"), ("calf", "foot")]), ("arms", [("upperarm", "lowerarm"), ("lowerarm", "hand")]),
          ("hands", [("hand", "middle_01")]), ("feet", [("foot", "ball")])]
# IK bones sit on their FK bone in retail bind poses (Holly and Walker alike): they follow it
IK_FOLLOW = {"ik_hand_gun": "hand_r", "ik_hand_r": "hand_r", "ik_hand_l": "hand_l", "ik_foot_l": "foot_l",
             "ik_foot_r": "foot_r"}


def proportions_keep(o, mode):
    """--proportions own (default, 1) | fit (0) | a number in between: how much of the model's own segment lengths
    the 3P fit keeps (lengths blended geometrically). FP: always 0."""
    v = str(o.get("proportions") or "own").strip().lower()
    if mode != "3p":
        return 0.0
    if v in ("own", "keep"): return 1.0
    if v == "fit": return 0.0
    try:
        f = float(v)
    except ValueError:
        raise SystemExit(f"--proportions {v}: own, fit or a number from 0 (fit) to 1 (own)")
    return max(0.0, min(1.0, f))


def proportions_report(sp, inv, tp, keep, mode="3p"):
    """Log the model's segment lengths against the template's (sp: source joint positions after the uniform scale)."""
    def length(pairs, side=None):
        tot_s = tot_t = 0.0
        for a, b in pairs:
            a_, b_ = (f"{a}_{side}", f"{b}_{side}") if side else (a, b)
            if a_ not in inv or b_ not in inv or a_ not in tp or b_ not in tp: return None
            tot_s += (sp[inv[b_]] - sp[inv[a_]]).length; tot_t += (tp[b_] - tp[a_]).length
        return tot_s / tot_t if tot_t > 1e-6 else None
    rows = []
    for name, pairs in GROUPS:
        r = [x for x in (length(pairs, "l"), length(pairs, "r")) if x]
        if r: rows.append((name, sum(r) / len(r)))
    top = next((x for x in ("neck_01", "neck_02", "head") if x in inv and x in tp), None)
    if top and "pelvis" in inv: rows.insert(1, ("torso", length([("pelvis", top)])))
    if top and top != "head" and "head" in inv: rows.insert(2, ("neck", length([(top, "head")])))
    for name, a, b in (("shoulders", "upperarm_l", "upperarm_r"), ("hips", "thigh_l", "thigh_r")):
        if a in inv and b in inv:
            rows.append((name, (sp[inv[a]] - sp[inv[b]]).length / max(1e-6, (tp[a] - tp[b]).length)))
    txt = ", ".join(f"{n} x{r:.2f}" for n, r in rows if r)
    how = ("first-person arms: fitted onto the FP skeleton (its animations move every bone)" if mode != "3p" else
           "kept: the mesh's bind skeleton gets the model's joints" if keep == 1.0 else
           "stretched onto the survivor's joints" if keep == 0.0 else f"blended ({keep:.2f} of the model's own)")
    log(f"proportions (model / survivor, same height): {txt}; {how}")
    far = [n for n, r in rows if r and abs(math.log(r)) > 0.25]
    if far and keep == 0.0 and mode == "3p":
        log(f"  {', '.join(far)} differ by more than 25 %: they look stretched or squashed (--proportions own keeps "
            f"them)")


FINGER_RX = re.compile(r"^(thumb|index|middle|ring|pinky)_0\d_[lr]$")


def hand_roll(t, q, dir_t, spos, inv, tp):
    """The hand's turn about its own axis: the knuckle line (pinky_01 -> index_01) onto the template's, so the palm
    faces the way the survivor's does (the shortest turn onto the hand's direction leaves the palm wherever the model
    had it: a palm-in hand held the gun on its side). Returns (q, degrees turned or None)."""
    sd = t[-1]
    a, b = f"index_01_{sd}", f"pinky_01_{sd}"
    if not all(x in inv and x in tp for x in (a, b)): return q, None
    ax = dir_t.normalized()
    ks = q @ (spos[inv[a]] - spos[inv[b]]); kt = tp[a] - tp[b]
    ks -= ax * ks.dot(ax); kt -= ax * kt.dot(ax)
    if ks.length < 1e-4 or kt.length < 1e-4: return q, None
    ks.normalize(); kt.normalize()
    ang = math.atan2(ks.cross(kt).dot(ax), ks.dot(kt))
    return Quaternion(ax, ang) @ q, math.degrees(ang)


def own_bind_pose(tpl, sarm, meshes, bmap, inv, spos, D_of):
    """The model's own proportions: stand the chained joints on the template's ground (the model's soles where the
    template's are) and return the mapped joints' new positions {template bone: Vector}. Moves every D in D_of."""
    owner = own_bones(sarm, bmap, inv, tpl.pos)
    feet = {inv[x] for x in ("foot_l", "foot_r", "ball_l", "ball_r") if x in inv}
    tground = min((tm.matrix_world @ v.co).z for tm in tpl.meshes for v in tm.data.vertices)
    low = low_any = None
    for m in meshes:
        names = {g.index: g.name for g in m.vertex_groups}
        for v in m.data.vertices:
            if not v.groups: continue
            g = max(v.groups, key=lambda x: x.weight)
            ob = owner.get(names.get(g.group))
            if ob not in D_of: continue
            z = (D_of[ob] @ (m.matrix_world @ v.co)).z
            low_any = z if low_any is None else min(low_any, z)
            if ob in feet: low = z if low is None else min(low, z)
    low = low if low is not None else low_any
    shift = Matrix.Translation(Vector((0, 0, tground - low))) if low is not None else Matrix.Identity(4)
    for k in D_of: D_of[k] = shift @ D_of[k]
    P = {t: D_of[sb] @ spos[sb] for sb, t in bmap.items() if sb in D_of}
    if "pelvis" in P:
        log(f"proportions: pelvis {P['pelvis'].z * 100:.1f} cm above the ground (survivor {tpl.pos['pelvis'].z * 100:.1f}),"
            f" head joint {P['head'].z * 100:.1f} cm (survivor {tpl.pos['head'].z * 100:.1f}); soles on the survivor's "
            f"ground ({(tground - low) * 100 if low is not None else 0:+.1f} cm)")
    return P


def rebind_template(tpl, P, chains, chain_of, o):
    """Move the template's joints onto P (the model's), segment by segment (turn + stretch; bones that aren't mapped
    move with their mapped ancestor, IK bones with their FK bone, `weapon` scales with the shoulder height), deform its
    meshes along, and make that the template armature's rest pose. Records every moved bone in manifest extras
    bind_bones_m (Blender world metres), which the importer writes into the mesh's reference skeleton."""
    arm = tpl.arm
    tp = dict(tpl.pos)
    bones = sorted(arm.data.bones, key=depth_bone)
    D, Q, axis = {}, {}, {}

    def seg(n, a, c):
        dt, dm = tp[c] - tp[a], P[c] - P[a]
        if dt.length < 1e-6 or dm.length < 1e-6: return None
        q = dt.normalized().rotation_difference(dm.normalized())
        st, u = dm.length / dt.length, dt.normalized()
        S = Matrix.Identity(3) + (st - 1.0) * Matrix(((u.x * u.x, u.x * u.y, u.x * u.z), (u.y * u.x, u.y * u.y, u.y * u.z),
                                                      (u.z * u.x, u.z * u.y, u.z * u.z)))
        return Matrix.Translation(P[n]) @ q.to_matrix().to_4x4() @ S.to_4x4() @ Matrix.Translation(-tp[n]), q, (u, st)

    for b in bones:
        n = b.name
        anc = b.parent
        while anc is not None and anc.name not in P: anc = anc.parent
        if n in P:
            ch = chain_of.get(n)
            if ch:
                a, c = chains[ch]
            else:
                a, c = n, next((x for x in AIM.get(n, []) if x in P), None)
            r = seg(n, a, c) if c else None
            if r:
                D[n], Q[n], axis[n] = r
            else:                       # end bones (head, ball, finger tips): the parent's turn, no stretch
                q = Q.get(anc.name, Quaternion()) if anc is not None else Quaternion()
                D[n] = Matrix.Translation(P[n]) @ q.to_matrix().to_4x4() @ Matrix.Translation(-tp[n]); Q[n] = q
        elif anc is not None:
            D[n], Q[n] = D[anc.name], Q[anc.name]
            if anc.name in axis: axis[n] = axis[anc.name]
        else:
            D[n] = Matrix.Identity(4)
    for n, fk in IK_FOLLOW.items():
        if n in tp and fk in D:
            D[n] = Matrix.Translation(D[fk] @ tp[n] - tp[n]); axis.pop(n, None)
    ua = [P[x].z / tp[x].z for x in ("upperarm_l", "upperarm_r") if x in P and tp[x].z > 1e-3]
    if "weapon" in tp and ua:
        w = tp["weapon"]; r = sum(ua) / len(ua)
        D["weapon"] = Matrix.Translation(Vector((w.x, w.y, w.z * r)) - w)
    # stretched bones point along their stretch axis (a pose holds scale along the bone, not shear)
    select_only([arm], arm)
    bpy.ops.object.mode_set(mode="EDIT")
    for eb in arm.data.edit_bones:
        eb.inherit_scale = "NONE"
        eb.use_connect = False
        u, st = axis.get(eb.name, (None, 1.0))
        if u is not None and abs(st - 1.0) > 1e-4:
            roll_ref = eb.z_axis.copy()
            eb.tail = eb.head + u * eb.length
            eb.align_roll(roll_ref)
    bpy.ops.object.mode_set(mode="POSE")
    M = {}
    for pb in sorted(arm.pose.bones, key=lambda x: depth_bone(x.bone)):
        pb.matrix = D[pb.name] @ pb.bone.matrix_local
        bpy.context.view_layer.update()
        M[pb.name] = pb.matrix.copy()
    bpy.ops.object.mode_set(mode="OBJECT")
    for tm in tpl.meshes:
        select_only([tm], tm)
        for md in list(tm.modifiers):
            if md.type == "ARMATURE":
                bpy.ops.object.modifier_apply(modifier=md.name)
    # the posed skeleton becomes the rest pose
    select_only([arm], arm)
    bpy.ops.object.mode_set(mode="EDIT")
    for eb in arm.data.edit_bones:
        m = M[eb.name]
        sy = m.col[1].xyz.length
        length = eb.length
        eb.matrix = m.normalized()
        eb.length = max(1e-4, length * sy)
    bpy.ops.object.mode_set(mode="POSE")
    for pb in arm.pose.bones: pb.matrix_basis = Matrix.Identity(4)
    bpy.ops.object.mode_set(mode="OBJECT")
    for tm in tpl.meshes:
        md = tm.modifiers.new("Armature", "ARMATURE"); md.object = arm
    tpl.pos = {b.name: arm.matrix_world @ b.head_local for b in arm.data.bones}
    err = max(((tpl.pos[n] - p).length for n, p in P.items() if n in tpl.pos), default=0.0)
    moved = {n: p for n, p in tpl.pos.items() if (p - tp[n]).length > 1e-6}
    o.setdefault("extras", {})["bind_bones_m"] = {n: [p.x, p.y, p.z] for n, p in moved.items()}
    far = max(((tpl.pos[n] - tp[n]).length for n in moved), default=0.0)
    log(f"bind skeleton: {len(moved)} bones moved to the model's proportions (up to {far * 100:.1f} cm; joint error "
        f"{err * 100:.2f} cm)")


def fit_character(o):
    tpl = Template(o["template"])
    # --rigged: an unrigged model as the 3P fit rigged it (save_rigged), used for its first-person arms
    src_objs = import_any(o.get("rigged") or o["source"])
    src_objs = [x for x in src_objs if x.name in bpy.data.objects]
    if o.get("drop"):
        rx = re.compile(o["drop"], re.I)
        for x in list(src_objs):
            if x.type == "MESH" and rx.search(x.name):
                log("dropping", x.name); bpy.data.objects.remove(x); src_objs.remove(x)
    if o.get("drop_mat"):
        dm = {x.lower() for x in o["drop_mat"]}
        for x in [x for x in src_objs if x.type == "MESH"]:
            mats_x = list(x.data.materials)
            gone = lambda i: (re.sub(r"\.\d{3}$", "", mats_x[i].name).lower() in dm) if i < len(mats_x) and mats_x[i] \
                else NO_MATERIAL in dm                     # faces without a material: --slot none=drop
            if not any(gone(i) for i in {p.material_index for p in x.data.polygons}): continue
            bm = bmesh.new(); bm.from_mesh(x.data)
            bmesh.ops.delete(bm, geom=[f for f in bm.faces if gone(f.material_index)], context="FACES")
            bm.to_mesh(x.data); bm.free()
            if not x.data.polygons:
                log("dropping", x.name, "(only dropped materials)"); bpy.data.objects.remove(x); src_objs.remove(x)
    mark_cloth_bones(src_objs, o.get("cloth", ""))
    if o.get("mode", "3p") == "3p": dangle_module().mark_hair_bones(src_objs, log)
    if o.get("mode", "3p") == "3p" and o.get("skirt") == "legs": dangle_module().mark_skirt_bones(src_objs, log)
    keyed = [x.name for x in src_objs if x.type == "MESH" and x.data.shape_keys]
    face_on = o.get("mode", "3p") == "3p" and o.get("face", "auto") != "off"
    face_src = None
    if face_on and keyed:                           # face rig: what the mouth-open / blink keys move, before removal
        fm = face_module()
        fk = fm.capture_shape_keys([x for x in src_objs if x.type == "MESH"], fm.gltf_morph_names(o["source"]))
        if fk: log("face: shape keys used as landmarks: " + ", ".join(f"{r} {n}" for r, ns in fk.items() for n in ns))
    if face_on and o.get("face_eyes"):              # eye positions given by the modder: tag them before the fit
        face_module().capture_eye_points([x for x in src_objs if x.type == "MESH"], o["face_eyes"], log)
    for x in src_objs:
        if x.type == "MESH" and x.data.shape_keys:
            x.shape_key_clear()                     # the base shape stays
    if keyed:
        log(f"shape keys (face expressions, morphs) removed from {keyed}: survivors have no morph targets; the face "
            + ("is rigged to the survivor's face bones instead" if face_on else "follows the head (and jaw) bones"))
    arms = [x for x in src_objs if x.type == "ARMATURE"]
    meshes = [x for x in src_objs if x.type == "MESH"]
    if not meshes: raise SystemExit("no mesh in the model (after --drop)")
    mode = o.get("mode", "3p")
    targets = set(tpl.bones)
    # FBX files often carry cm scale / axis rotations on the objects: bake them into the data first
    apply_transforms(src_objs)
    fix_inverted_normals(meshes)

    own_bind = chains = chain_of = None
    if arms:
        sarm = max(arms, key=lambda a: len(a.data.bones))
        user_map = None
        if o.get("bonemap"):
            try:
                user_map = json.load(open(o["bonemap"], encoding="utf-8-sig"))
            except (OSError, ValueError) as e:
                raise SystemExit(f"--bonemap {o['bonemap']}: not a readable JSON file ({e})")
            user_map = {k: v for k, v in user_map.items() if v}
        if o.get("rigged"):                         # our own 3P rig: the survivor's names, weighted or not
            user_map = {b.name: b.name for b in sarm.data.bones if b.name in targets and
                        (b.name in B4B_MAIN or "_twist_" in b.name)}
        on_bones = bone_parented_as_skinned(sarm, meshes)
        if on_bones:
            log("meshes parented to a bone, skinned to it: " + ", ".join(f"{n} ({b})" for n, b in on_bones))
        skinned = mesh_children(sarm, meshes)
        bmap, kind = build_bonemap(sarm, targets, user_map, skinned)
        missing = [b for b in REQUIRED if b not in bmap.values()]
        log(f"source armature {sarm.name!r}: {len(sarm.data.bones)} bones, {len(bmap)} mapped ({kind})")
        log("  " + ", ".join(f"{s}->{t}" for s, t in sorted(bmap.items(), key=lambda x: B4B_MAIN.index(x[1])
                                                               if x[1] in B4B_MAIN else 999)))
        if missing:
            raise SystemExit(bonemap_help(sarm, bmap, missing, o["out"], targets))
        inv = {v: k for k, v in bmap.items()}
        smeshes = skinned
        others = [m for m in meshes if m not in smeshes]
        if others:
            log("meshes not skinned to the rig (parented rigidly to head):", [m.name for m in others])
        # 1. orient + scale: source frame -> template frame
        spos = {b.name: sarm.matrix_world @ b.head_local for b in sarm.data.bones}
        sp = {t: spos[s] for s, t in bmap.items()}
        tp = tpl.pos
        Ts = facing_frame(sp, "upperarm_r", "upperarm_l", "pelvis", "head")
        Tt = facing_frame(tp, "upperarm_r", "upperarm_l", "pelvis", "head")
        R = (Tt @ Ts.inverted()).to_4x4()
        sh = (sp["head"] - (sp["foot_l"] + sp["foot_r"]) / 2).length
        th = (tp["head"] - (tp["foot_l"] + tp["foot_r"]) / 2).length
        k = th / sh
        pel_s, pel_t = sp["pelvis"], tp["pelvis"]
        G = Matrix.Translation(pel_t) @ R @ Matrix.Scale(k, 4) @ Matrix.Translation(-pel_s)
        log(f"orient: rotate {math.degrees(R.to_quaternion().angle):.1f} deg, scale {k:.3f} "
            f"(model {sh * 100:.0f} cm head to feet, template {th * 100:.0f} cm)")
        unit_hint(sh, k)
        for x in [sarm] + meshes:
            if x.parent is None or x.parent not in [sarm] + meshes:
                x.matrix_world = G @ x.matrix_world
        apply_transforms([sarm] + meshes)
        for m in others:                           # rigid pieces (glasses, hats): follow the head
            vg = m.vertex_groups.new(name=inv["head"]); vg.add(range(len(m.data.vertices)), 1.0, "REPLACE")
            md = m.modifiers.new("Armature", "ARMATURE"); md.object = sarm
        smeshes = mesh_children(sarm, meshes)
        # proportions: the model's segments against the survivor's (legs, torso, neck, arms)
        keep = proportions_keep(o, mode)
        proportions_report({b.name: b.head_local.copy() for b in sarm.data.bones}, inv, tp, keep, mode)
        # 2. pose every mapped bone so its joint lands on the template joint and it aims at the template's next joint;
        #    bones in between (twist, extra spine/neck segments, helpers) and below (face, hair) move with their mapped
        #    ancestor's deformation
        select_only([sarm], sarm)
        bpy.ops.object.mode_set(mode="EDIT")
        for eb in sarm.data.edit_bones:
            eb.inherit_scale = "NONE"
            eb.use_connect = False
        # point every mapped bone at its aim joint (rest orientation only; the mesh doesn't move), so the stretch
        # along the segment is a scale along the bone's own axis: no shear, which a pose can't hold
        ebs = sarm.data.edit_bones
        # chains fitted as one piece: (first mapped bone, the next mapped joint above the chain)
        chains, chain_of = {}, {}
        for cname, members, tops in (("spine", ["pelvis", "spine_01", "spine_02", "spine_03"], ["neck_01", "neck_02", "head"]),
                                     ("neck", ["neck_01", "neck_02"], ["head"])):
            have = [x for x in members if x in inv and x in tp]
            top = next((x for x in tops if x in inv and x in tp), None)
            if have and top:
                chains[cname] = (have[0], top)
                for x in have: chain_of[x] = cname
        for sb, t in bmap.items():
            aim = next((x for x in AIM.get(t, []) if x in inv and x in tp and x not in BIND_ONLY), None)
            if aim is None or t in BIND_ONLY: continue
            eb, ea = ebs[sb], ebs[inv[aim]]
            d = ea.head - eb.head
            if t in chain_of:
                base, top = chains[chain_of[t]]
                d = (ebs[inv[top]].head - ebs[inv[base]].head).normalized() * max(d.length, 1e-3)
            if d.length > 1e-5:
                roll_ref = eb.z_axis.copy()
                eb.tail = eb.head + d
                eb.align_roll(roll_ref)
        bpy.ops.object.mode_set(mode="POSE")
        order = sorted(sarm.pose.bones, key=lambda pb: depth_bone(pb.bone))
        spos = {b.name: b.head_local.copy() for b in sarm.data.bones}          # armature space == world now
        worst = 0.0
        D_of = {}
        chain_D = {}

        # template hierarchy: with the model's own proportions each joint is carried by the transform of its parent
        # in the TEMPLATE's tree (source rigs differ: Rigify hangs thighs and shoulders off ORG- bones)
        tparent = {b.name: b.parent.name if b.parent else None for b in tpl.arm.data.bones}
        tdepth = {b.name: depth_bone(b) for b in tpl.arm.data.bones}

        def mapped_tparent(t):
            p = tparent.get(t)
            while p is not None and (p not in inv or inv[p] not in D_of): p = tparent.get(p)
            return p

        def joint_target(sb, t):
            """Where a mapped joint goes: the template's joint (fit); with the model's own proportions (keep > 0),
            where its mapped parent's transform carries it (segments only turned, joints chained from the pelvis)."""
            if keep == 0.0: return tp[t]
            p = mapped_tparent(t)
            return D_of[inv[p]] @ spos[sb] if p is not None else tp[t].copy()

        def chain_fit(chain):
            base, top = chains[chain]
            a_s, b_s = spos[inv[base]], spos[inv[top]]
            ds, dt = b_s - a_s, tp[top] - tp[base]
            if ds.length <= 1e-5 or dt.length <= 1e-5: return None
            q = ds.normalized().rotation_difference(dt.normalized())
            st = (dt.length / ds.length) ** (1.0 - keep)
            y = ds.normalized()
            S = Matrix.Identity(3) + (st - 1.0) * Matrix(((y.x * y.x, y.x * y.y, y.x * y.z),
                                                          (y.y * y.x, y.y * y.y, y.y * y.z),
                                                          (y.z * y.x, y.z * y.y, y.z * y.z)))
            a_t = joint_target(inv[base], base)
            log(f"  {chain} ({base} -> {top}) fitted as one piece, stretched x{st:.2f}")
            return Matrix.Translation(a_t) @ q.to_matrix().to_4x4() @ S.to_4x4() @ Matrix.Translation(-a_s)
        # phase 1: the fit transform D of every mapped bone (joint onto the template joint, aimed, stretched)
        q_of = {}
        rolls = {}
        fit_order = order if keep == 0.0 else \
            sorted((pb for pb in order if bmap.get(pb.name) in tdepth), key=lambda pb: tdepth[bmap[pb.name]])
        for pb in fit_order:
            t = bmap.get(pb.name)
            if t is None or t in BIND_ONLY: continue
            rest = pb.bone.matrix_local.copy()
            ch = chain_of.get(t)
            if ch is not None and ch not in chain_D:
                chain_D[ch] = chain_fit(ch)
            if chain_D.get(ch) is not None:
                # torso and neck: one transform per chain (pelvis -> neck, neck -> head), so segment lengths that
                # differ from the template's (Mixamo hips, VRM and Rigify spines) don't squash and stretch it in bands
                D_of[pb.name] = chain_D[ch]
                q_of[pb.name] = chain_D[ch].to_quaternion()
                continue
            aim = next((x for x in AIM.get(t, []) if x in inv and x in tp and x not in BIND_ONLY), None)
            head_s = spos[pb.name]
            head_t = joint_target(pb.name, t)
            if aim:
                dir_s = spos[inv[aim]] - head_s
                dir_t = tp[aim] - tp[t]
            else:
                dir_s = dir_t = None
            if dir_s is not None and dir_s.length > 1e-6 and dir_t.length > 1e-6:
                q = dir_s.normalized().rotation_difference(dir_t.normalized())
                if FINGER_RX.match(t):
                    # fingers keep the hand's roll (the shortest turn onto their own segment alone drops it)
                    p = tparent.get(t)
                    qp = q_of.get(inv[p]) if p in inv else None
                    if qp is not None:
                        q = (qp @ dir_s).normalized().rotation_difference(dir_t.normalized()) @ qp
                elif t in ("hand_l", "hand_r"):
                    q, r = hand_roll(t, q, dir_t, spos, inv, tp)
                    if r is not None: rolls[t] = r
                st = (dir_t.length / dir_s.length) ** (1.0 - keep)
            else:
                # no aim joint (end bones, head): keep the nearest mapped parent's rotation change; the head keeps the
                # model's own orientation (both look ahead at rest: turning it with a forward-leaning neck that is
                # straightened onto the survivor's made the face look up)
                if t == "head":
                    q = Quaternion()
                elif keep == 0.0:
                    par = pb.parent
                    while par is not None and par.name not in q_of: par = par.parent
                    q = q_of[par.name] if par is not None else Quaternion()
                else:
                    p = mapped_tparent(t)
                    q = q_of[inv[p]] if p is not None else Quaternion()
                st = 1.0
            # stretch along the bone's own axis (= the segment, see above) about the head; rotate; move the head
            # onto the template joint
            ya = rest.col[1].xyz.normalized()
            S = Matrix.Identity(3) + (st - 1.0) * Matrix(((ya.x * ya.x, ya.x * ya.y, ya.x * ya.z),
                                                          (ya.y * ya.x, ya.y * ya.y, ya.y * ya.z),
                                                          (ya.z * ya.x, ya.z * ya.y, ya.z * ya.z)))
            D_of[pb.name] = Matrix.Translation(head_t) @ q.to_matrix().to_4x4() @ S.to_4x4() @ Matrix.Translation(-head_s)
            q_of[pb.name] = q
            worst = max(worst, ((D_of[pb.name] @ head_s) - head_t).length)
        own_bind = own_bind_pose(tpl, sarm, smeshes, bmap, inv, spos, D_of) if keep > 0.0 else None
        # which mapped bone each other bone moves with (and gives its weights to): its mapped ancestor, or the mapped
        # bone of the same name in another layer (Rigify ORG-/MCH- vs DEF-), else the mapped bone nearest to it
        # (helper bones parented outside the deform chain, e.g. MakeHuman elbow/knee helpers)
        owner = own_bones(sarm, bmap, inv, tp)
        # phase 2: pose every bone, root first (a pose is stored relative to the parent's)
        D_used = {}
        for pb in order:
            t = bmap.get(pb.name)
            if t is not None and t not in BIND_ONLY:
                D = D_of[pb.name]
            else:
                ob = owner.get(pb.parent.name) if (t in BIND_ONLY and pb.parent) else owner.get(pb.name)
                # a bone under a bind-only bone (chin, lips, teeth, tongue under the jaw) moves with the jaw, which
                # moves with its own parent (identity here left the lower face behind: the mouth pulled wide open)
                D = D_of[ob] if ob in D_of else D_used.get(ob, Matrix.Identity(4)) if ob else Matrix.Identity(4)
            pb.matrix = D @ pb.bone.matrix_local
            D_used[pb.name] = D
            bpy.context.view_layer.update()
        log(f"pose fit: max joint error {worst * 100:.2f} cm" +
            (" (palms turned " + ", ".join(f"{t[-1]} {r:.0f} deg" for t, r in sorted(rolls.items())) + ")" if rolls else ""))
        bpy.ops.object.mode_set(mode="OBJECT")
        # 3. bake the pose into the meshes
        for m in smeshes:
            select_only([m], m)
            for md in list(m.modifiers):
                if md.type == "ARMATURE" and md.object == sarm:
                    bpy.ops.object.modifier_apply(modifier=md.name)
        # 4. weights: rename to template bones (other bones -> their owner above)
        def target_of(name):
            ob = owner.get(name)
            return bmap[ob] if ob in bmap else "pelvis"
        if face_on:                                 # face rig: which vertices the rig's jaw/lower lip moves
            face_module().capture_jaw_weights(smeshes)
        for m in smeshes:
            rename_groups(m, {g.name: target_of(g.name) for g in m.vertex_groups})
        if face_on:
            face_src = face_module().capture_source_bones(sarm, D_used)
        bpy.data.objects.remove(sarm)
    else:
        # unrigged: stand it like the template (the model faces --facing, default -y = Blender's front view), scale to
        # the template's height, feet on the ground, centred on the pelvis; weights come from the template mesh
        log("source has no armature: placing it by its bounding box, weights from the template mesh (nearest surface)")
        for x in meshes:                             # a hierarchy of parts (head, torso, arm-left ...): flatten it
            if x.parent is not None:
                mw = x.matrix_world.copy(); x.parent = None; x.matrix_world = mw
        smeshes = meshes
        o["weights"] = "transfer"
        dangle_module().drop_template_gear(tpl, log)   # Holly's bag bone: no part of the model goes on it
        face = AXES[o.get("facing", "-y")]
        tfwd = Vector((1, 0, 0))                    # B4B heroes face +X
        R = face.rotation_difference(tfwd).to_matrix().to_4x4()
        for x in meshes:
            if x.parent is None or x.parent not in meshes: x.matrix_world = R @ x.matrix_world
        apply_transforms(meshes)
        vs = [x.matrix_world @ v.co for x in meshes for v in x.data.vertices]
        lo = Vector((min(v.x for v in vs), min(v.y for v in vs), min(v.z for v in vs)))
        hi = Vector((max(v.x for v in vs), max(v.y for v in vs), max(v.z for v in vs)))
        # the template's stature from its skeleton (an FP arms mesh has no feet): ground 4.5 cm below the balls of
        # the feet, top of the head 11.5 % above the head joint (measured on the 3P hero meshes)
        ground = (tpl.pos["ball_l"].z + tpl.pos["ball_r"].z) / 2 - 0.045
        th = (tpl.pos["head"].z - ground) * 1.115
        k = th / max(1e-6, hi.z - lo.z)
        pel = tpl.pos["pelvis"]
        G = Matrix.Translation(Vector((pel.x, pel.y, ground))) @ Matrix.Scale(k, 4) @ \
            Matrix.Translation(-Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, lo.z)))
        for x in meshes:
            if x.parent is None or x.parent not in meshes: x.matrix_world = G @ x.matrix_world
        apply_transforms(meshes)
        log(f"unrigged: scale {k:.3f}")
        unit_hint((hi.z - lo.z) / 1.13, k)             # the head joint sits about 1/1.13 of the stature up
        # arms in another pose than the template's A-pose (T-pose, hanging down): pose the template's arms like the
        # model's, take the weights from the posed template, then un-pose the model into the template's bind pose
        fitted = unpose_arms(tpl, meshes, chain=mode == "fp")
        o["weights"] = "done"
        if mode == "3p" and o.get("rigged_out"):
            if fitted:
                save_rigged(o["rigged_out"], tpl.arm, meshes)
            else:                                   # arms too unlike the survivor's: FP from the joint-by-joint un-pose
                log("unrigged: first-person arms from the model itself (the arms didn't fit onto the survivor's)")
    if own_bind:
        # the template (skeleton + meshes) takes the model's proportions too: later steps (twist weights, the face)
        # compare against it, and its joints become the mesh's bind skeleton (manifest extras bind_bones_m)
        rebind_template(tpl, own_bind, chains, chain_of, o)
    # --skirt legs: skirts follow the hips and thighs (before the twist split, which shares the thighs' part like the
    # legs'; an unrigged model's after its weights come from the template)
    skirt = mode == "3p" and o.get("skirt") == "legs" and not o.get("probe")
    if skirt and o.get("weights") != "transfer":
        dangle_module().skirt_legs(tpl, smeshes, log)
    if o.get("weights") == "transfer":
        transfer_weights(tpl, smeshes, all_groups=True)
        if skirt: dangle_module().skirt_legs(tpl, smeshes, log)
    elif o.get("twist", "template") == "template":
        transfer_weights(tpl, smeshes, all_groups=False)
    if face_on and not o.get("probe"):
        rig_face_bones(o, tpl, smeshes, face_src)
    if mode == "3p" and not o.get("probe"):
        smeshes = secondary_motion(o, tpl, smeshes)
    if mode == "fp":
        segs = body_segments(tpl) if not arms else None
        saved = {m: m.data.copy() for m in smeshes} if segs else {}
        for m in smeshes:
            keep_arms(m, fp_arm_mask(tpl, m, segs) if segs else None)
        if segs and not any(len(m.data.polygons) for m in smeshes):
            # nothing left by the body-segment test (blocky figures: arm boxes far from the survivor's arm bones):
            # the faces skinned to the arms, as before that test
            log("fp: no arm faces by the survivor's skeleton: keeping the faces skinned to the arms")
            for m in smeshes:
                old_me = m.data; m.data = saved[m]; bpy.data.meshes.remove(old_me)
                keep_arms(m, None)
        else:
            for me in saved.values(): bpy.data.meshes.remove(me)
        fp_drop_hanging(tpl, smeshes)
        smeshes = [m for m in smeshes if len(m.data.polygons)]
    if o.get("probe") == "regions":
        body_regions(tpl, smeshes, os.path.join(o["out"], "regions.json"))
        return
    if o.get("probe"):
        mats = []
        for m in smeshes:
            for mt in used_materials(m):
                n = re.sub(r"\.\d{3}$", "", mt.name) if mt else NO_MATERIAL
                if n not in mats: mats.append(n)
        os.makedirs(o["out"], exist_ok=True)
        json.dump({"materials": mats}, open(os.path.join(o["out"], "probe.json"), "w"))
        log("probe: materials", mats)
        return
    tpl.hide_meshes()
    # bind to the template armature
    for m in smeshes:
        for md in list(m.modifiers):
            if md.type == "ARMATURE": m.modifiers.remove(md)
        m.parent = tpl.arm
        m.matrix_parent_inverse = tpl.arm.matrix_world.inverted()
        md = m.modifiers.new("Armature", "ARMATURE"); md.object = tpl.arm
    finish(o, tpl, smeshes)


# body regions: which part of the body each material covers (on the fitted model, by the template bones that move
# its vertices) and which part each template slot covers; b4bmodel places materials whose names say nothing by them
REGION_KEYS = [("head", "head"), ("neck_01", "head"), ("neck_02", "head"), ("pelvis", "torso"), ("spine_", "torso"),
               ("clavicle_", "torso"), ("upperarm_", "arms"), ("lowerarm_", "arms"), ("hand_", "hands"),
               ("thigh_", "legs"), ("calf_", "legs"), ("foot_", "feet"), ("ball_", "feet")]
REGIONS = ("head", "torso", "arms", "hands", "legs", "feet")


def bone_regions(arm):
    """{bone: region} for every bone of a B4B skeleton: the region of the nearest listed bone up its parent chain."""
    out = {}
    for b in arm.data.bones:
        x, r = b, None
        while x is not None and r is None:
            r = next((reg for k, reg in REGION_KEYS if x.name == k or (k.endswith("_") and x.name.startswith(k))), None)
            x = x.parent
        out[b.name] = r or "torso"
    return out


def mesh_regions(m, breg, label):
    """{label(material index): {region: share of the area}} of a mesh skinned to B4B bones."""
    import collections
    names = {g.index: g.name for g in m.vertex_groups}
    vreg = []
    for v in m.data.vertices:
        acc = collections.defaultdict(float)
        for ge in v.groups:
            if ge.weight > 0 and names.get(ge.group) in breg: acc[breg[names[ge.group]]] += ge.weight
        vreg.append(max(acc.items(), key=lambda kv: (kv[1], kv[0]))[0] if acc else None)
    out = collections.defaultdict(lambda: collections.defaultdict(float))
    for p in m.data.polygons:
        k = label(p.material_index)
        if k is None: continue
        for vi in p.vertices:
            r = vreg[vi]
            if r: out[k][r] += p.area / len(p.vertices)
    return out


def body_regions(tpl, meshes, path):
    """regions.json: {"materials": {material: {region: share}}, "slots": {template slot: {region: share}}, "area":
    {material: m²}}."""
    breg = bone_regions(tpl.arm)

    def norm(d):
        t = sum(d.values()) or 1.0
        return {r: round(d[r] / t, 4) for r in REGIONS if d.get(r)}
    res = {"materials": {}, "slots": {}}
    for key, ms, is_tpl in (("materials", meshes, False), ("slots", tpl.meshes, True)):
        acc = {}
        for m in ms:
            mats = list(m.data.materials)
            def label(i, mats=mats):
                mt = mats[i] if i < len(mats) else None
                return re.sub(r"\.\d{3}$", "", mt.name) if mt else (None if is_tpl else NO_MATERIAL)
            for k, d in mesh_regions(m, breg, label).items():
                tgt = acc.setdefault(k, {})
                for r, v in d.items(): tgt[r] = tgt.get(r, 0.0) + v
        res[key] = {k: norm(d) for k, d in sorted(acc.items())}
        if key == "materials":            # surface each material covers (m², fitted): atlas tiles by texel density
            res["area"] = {k: round(sum(d.values()), 6) for k, d in sorted(acc.items())}
    os.makedirs(os.path.dirname(path), exist_ok=True)
    json.dump(res, open(path, "w"), indent=1, sort_keys=True)
    for k, d in res["materials"].items():
        log(f"regions: {k}: " + ", ".join(f"{r} {v:.0%}" for r, v in sorted(d.items(), key=lambda kv: -kv[1]) if v >= 0.05))


UNIT_MIXUPS = [(1 / 2.54, "centimetres stored as inches"), (2.54, "inches stored as centimetres"),
               (0.01, "centimetres read as metres"), (100.0, "metres read as centimetres"),
               (0.1, "millimetres read as centimetres"), (0.001, "millimetres read as metres"),
               (1 / 30.48, "centimetres stored as feet"), (0.3048, "feet read as metres")]


def unit_hint(sh, k):
    """The file's unit scale, judged by the height of the head joint (a person: about 1.3-1.8 m). A wrong unit only
    changes the uniform scale the fit applies anyway; this tells the modder what happened."""
    if 0.6 <= sh <= 2.6: return
    f, why = min(UNIT_MIXUPS, key=lambda x: abs(math.log(sh * x[0] / 1.55)))
    fits = 0.9 <= sh * f <= 2.3
    log(f"  the file's unit scale looks off: its head joint is {sh * 100:.0f} cm above the feet (a person: 130-180)"
        + (f"; x{f:.4g} gives {sh * f * 100:.0f} cm ({why}: the exporter's unit setting)" if fits else "")
        + f". Harmless: the model is scaled to the survivor's height (x{k:.3f})")


def fix_inverted_normals(meshes):
    """Custom (split) normals that point against the faces' winding (game rips: the ripper flipped one of them). The
    game draws one side of each face (by winding) and lights it by the normals: inverted normals render nearly black.
    When the faces point outward (judged from each mesh's centre, over the whole model) the normals are turned around;
    when the faces point inward too, the model is inside out: said, not changed."""
    bad, n_all, out, n_faces = [], 0, 0, 0
    for m in meshes:
        me = m.data
        if not me.polygons or not getattr(me, "has_custom_normals", False): continue
        cn = me.corner_normals
        dis = sum(1 for p in me.polygons for li in p.loop_indices if p.normal.dot(cn[li].vector) < 0)
        n_all += 1
        if dis > 0.9 * len(me.loops):
            bad.append(m)
            c = sum((v.co for v in me.vertices), Vector()) / len(me.vertices)
            out += sum(1 for p in me.polygons if p.normal.dot(p.center - c) > 0)
            n_faces += len(me.polygons)
    if not bad: return
    if out < 0.6 * n_faces:
        log(f"  normals: {len(bad)} objects' normals point against their faces, and the faces point inward: the model "
            f"may be inside out (Blender: select all, Mesh > Normals > Recalculate Outside, export again)")
        return
    for m in bad:
        me = m.data
        me.normals_split_custom_set([(-c.vector).to_tuple() for c in me.corner_normals])
    log(f"  normals: {len(bad)} of {n_all} objects had their normals pointing inward, against the faces (seen in game "
        f"rips): turned around ({', '.join(m.name for m in bad[:6])}{' ...' if len(bad) > 6 else ''})")


def face_module():
    """blender/b4bface.py (face bones: talking, blinking), next to this script."""
    d = os.path.dirname(os.path.abspath(__file__))
    if d not in sys.path: sys.path.insert(0, d)
    import b4bface
    return b4bface


def rig_face_bones(o, tpl, meshes, src_bones):
    """3P: skin the face to the template's face bones and record their new bind positions for the importer
    (manifest extras face_bones_m: {bone: [x, y, z]} Blender world metres)."""
    face_module().MOUTH_INTERIOR[0] = o.get("mouth", "auto")
    face_module().HAIR_SLOT_MATS.update(m for m, sl in (x.split("=", 1) for x in o.get("slot", []))
                                        if re.search(r"hair", sl, re.I))
    moved = face_module().rig_face(tpl, meshes, src_bones, log)
    if moved:
        o.setdefault("extras", {})["face_bones_m"] = {b: [p.x, p.y, p.z] for b, p in moved.items()}


def dangle_module():
    """blender/b4bdangle.py (hair on physics bones, skirts as cloth), next to this script."""
    d = os.path.dirname(os.path.abspath(__file__))
    if d not in sys.path: sys.path.insert(0, d)
    import b4bdangle
    return b4bdangle


def secondary_motion(o, tpl, meshes):
    """3P: --hair_bones "a,b,c;d,e": skin the back hair to the survivor's simulated hair chains (--hair_swing 0..1);
    --cloth auto|MAT[:cape|:lower],...: split skirts, coat tails and capes off as cloth sections and build their
    simulation meshes (manifest extras "cloth", written by modkit/cloth.py). Returns the meshes (cloth pieces added)."""
    pin = o.get("hair_pin", "on") != "off"
    if not o.get("hair_bones") and not pin and o.get("cloth", "off") in ("off", ""): return meshes
    dm = dangle_module()
    slot_of = dict(x.split("=", 1) for x in o.get("slot", []))
    def is_hair(mat):
        if not mat or dm.NOT_HAIR_RX.search(mat): return False
        sl = slot_of.get(mat)
        return bool(re.search(r"hair", sl, re.I)) and not dm.CLOTHES_RX.search(mat) if sl else \
            bool(dm.HAIR_RX.search(mat))
    if o.get("hair_bones"):
        chains = [c.split(",") for c in o["hair_bones"].split(";") if c]
        dm.rig_hair(tpl, meshes, chains, is_hair, log, float(o.get("hair_swing", 1.0)),
                    rigged=o.get("weights") != "done")
    if pin:
        # after the chain: long hair lying on the back keeps only the swing its clearance from the body allows
        dm.pin_hair(tpl, meshes, is_hair, log)
    if o.get("cloth", "off") not in ("off", ""):
        # --cloth_slots: the slots whose master material has bUsedWithClothing (b4bmodel reads it): a cloth section
        # on any other slot renders the engine's default material (grey) and the game logs "missing bUsedWithClothing"
        cloth_ok = None
        if o.get("cloth_slots") is not None:
            slot_of = dict(x.split("=", 1) for x in o.get("slot", []))
            ok_slots = {x.lower() for x in o["cloth_slots"].split(",") if x}
            cloth_ok = lambda mat: slot_of.get(mat, "").lower() in ok_slots if mat in slot_of else True
        mats, objs = dm.cloth_materials(meshes, tpl, o["cloth"], log, cloth_ok)
        if o.get("skirt") == "legs":                # skirts, dresses, coat tails follow the legs instead (skirt_legs)
            low = sorted(n for n, k in mats.items() if k == "lower")
            if low: log(f"cloth: {low} not simulated (--skirt legs: they follow the pelvis and thighs)")
            mats = {n: k for n, k in mats.items() if k != "lower"}
        pieces, sims = dm.cloth_regions(tpl, meshes, mats, log, objs)
        if sims:
            meshes = [m for m in meshes if len(m.data.polygons)] + pieces
            o.setdefault("extras", {})["cloth"] = sims
        elif o["cloth"] not in ("auto", "on"):
            log("cloth: nothing to simulate")
    return meshes


def garment_bone_faces(parts):
    """Faces (of [(object, polygons)]) mostly on the source rig's own garment physics bones (b4bdangle.GARMENT_BONES:
    Fortnite dyn_skirt_*, dyn_coat_* ...), for b4bmodel: such a material goes on a clothing slot and its skirt or coat
    becomes cloth with --cloth auto. Returns the face count (0 without such bones)."""
    rx = re.compile(dangle_module().GARMENT_BONES, re.I)
    n, pts, allz = 0, [], []
    for o, ps in parts:
        hit = {g.index for g in o.vertex_groups if rx.search(g.name)}
        mw = o.matrix_world
        allz += [(mw @ v.co).z for v in o.data.vertices]
        if not hit: continue
        share = []
        for v in o.data.vertices:
            tot = sum(g.weight for g in v.groups)
            share.append(sum(g.weight for g in v.groups if g.group in hit) / tot if tot else 0.0)
        for p in ps:
            if sum(share[i] for i in p.vertices) / len(p.vertices) >= 0.5:
                n += 1
                pts += [mw @ o.data.vertices[i].co for i in p.vertices]
    if not n: return 0
    # it must hang like a skirt or coat (b4bdangle.hang_check, before the fit: the crotch at half the body's
    # height, or the rig's thigh joints): coat tails a hand long, a collar or a hip ruffle stay skinned, and the
    # material keeps its own slot
    z0, z1 = min(allz), max(allz)
    H = max(1e-6, z1 - z0)
    crotch = z0 + 0.5 * H
    arm = next((md.object for o, _ in parts for md in o.modifiers if md.type == "ARMATURE" and md.object), None)
    if arm:                                         # the rig's thigh joints, when it names them
        th = [arm.matrix_world @ b.head_local for b in arm.data.bones
              if re.search(r"(^|[_.\- ])(thigh|upperleg|upper_leg|upleg)", b.name, re.I)
              and not re.search(r"twist|roll|helper|ik|dyn", b.name, re.I)]
        if th: crotch = min(p.z for p in th)
    top, bot = max(p.z for p in pts), min(p.z for p in pts)
    below = [p for p in pts if p.z < crotch - 0.02 * H]
    wide = below and max(max(p.x for p in below) - min(p.x for p in below),
                         max(p.y for p in below) - min(p.y for p in below)) >= 0.065 * H
    if top < crotch + 0.02 * H or bot > crotch - 0.08 * H or not wide:
        return 0
    return n


def mark_cloth_bones(objs, spec):
    """--cloth MAT@BONES: each vertex's weight share on source bones matching BONES, kept as a vertex attribute
    (b4bdangle.bone_attr) through the fit, which renames the source bones to the survivor's."""
    if "@" not in (spec or ""): return
    dm = dangle_module()
    for _, rx, _ in dm.parse_cloth_spec(spec):
        if not rx: continue
        r = re.compile(rx, re.I)
        name, n = dm.bone_attr(rx), 0
        for m in objs:
            if m.type != "MESH": continue
            hit = {g.index for g in m.vertex_groups if r.search(g.name)}
            a = m.data.attributes.get(name) or m.data.attributes.new(name, "FLOAT", "POINT")
            for v in m.data.vertices:
                tot = sum(g.weight for g in v.groups)
                x = sum(g.weight for g in v.groups if g.group in hit) / tot if tot else 0.0
                a.data[v.index].value = x
                n += x >= 0.5
        log(f"cloth: {n} vertices on the source bones /{rx}/")


def used_materials(m):
    """The materials a mesh's faces use (slots without faces, e.g. after --drop or the FP cut, don't count)."""
    idx = sorted({p.material_index for p in m.data.polygons})
    mats = list(m.data.materials)
    return [mats[i] if i < len(mats) else None for i in idx] if mats else [None]


def rename_groups(m, mapping):
    """Merge vertex groups by target name (weights summed)."""
    import collections
    acc = collections.defaultdict(dict)
    idx2name = {g.index: mapping.get(g.name, g.name) for g in m.vertex_groups}
    for v in m.data.vertices:
        for ge in v.groups:
            t = idx2name[ge.group]
            acc[t][v.index] = acc[t].get(v.index, 0.0) + ge.weight
    m.vertex_groups.clear()
    for t, d in acc.items():
        g = m.vertex_groups.new(name=t)
        for vi, w in d.items():
            g.add([vi], w, "REPLACE")


def mesh_weights(m):
    names = {g.index: g.name for g in m.vertex_groups}
    return [{names[ge.group]: ge.weight for ge in v.groups if ge.weight > 0} for v in m.data.vertices]


def set_weights(m, W):
    m.vertex_groups.clear()
    groups = {}
    for vi, d in enumerate(W):
        for n, w in d.items():
            if w <= 0: continue
            g = groups.get(n) or groups.setdefault(n, m.vertex_groups.new(name=n))
            g.add([vi], w, "REPLACE")


def transfer_weights(tpl, meshes, all_groups):
    """all_groups: copy the nearest template vertex's weights (unrigged source). Otherwise only split each limb bone's
    weight among its twist bones in the template's proportions at the nearest template vertex."""
    kd_pts, kd_w = [], []
    dg = bpy.context.evaluated_depsgraph_get()
    for tm in tpl.meshes:
        W = mesh_weights(tm)
        for v, w in zip(tm.data.vertices, W):
            kd_pts.append(tm.matrix_world @ v.co); kd_w.append(w)
    kd = KDTree(len(kd_pts))
    for i, p in enumerate(kd_pts): kd.insert(p, i)
    kd.balance()
    fam_of = {b: fam for fam, bs in LIMB_FAMILIES.items() for b in bs}
    face = face_bones_of(tpl) if all_groups else set()
    for m in meshes:
        W = mesh_weights(m)
        out = []
        for v, w in zip(m.data.vertices, W):
            p = m.matrix_world @ v.co
            if all_groups:
                hits = kd.find_n(p, 4)
                acc = {}
                tot = 0.0
                for co, i, d in hits:
                    f = 1.0 / max(d, 1e-4)
                    tot += f
                    for n, x in kd_w[i].items(): acc[n] = acc.get(n, 0.0) + x * f
                out.append(fold_face({n: x / tot for n, x in acc.items()}, face))
                continue
            nw = {}
            tw = None
            for n, x in w.items():
                fam = LIMB_FAMILIES.get(n)
                if not fam:
                    nw[n] = nw.get(n, 0.0) + x; continue
                if tw is None:
                    tw = {}
                    for co, i, d in kd.find_n(p, 3):
                        for tn, tx in kd_w[i].items(): tw[tn] = tw.get(tn, 0.0) + tx
                parts = {b: tw.get(b, 0.0) for b in fam if b in tpl.bones}
                s = sum(parts.values())
                if s <= 1e-6:
                    nw[n] = nw.get(n, 0.0) + x
                else:
                    for b, y in parts.items():
                        if y > 0: nw[b] = nw.get(b, 0.0) + x * y / s
            out.append(nw)
        set_weights(m, out)
    log("weights:", "copied from the template" if all_groups else "limb weights split among the template's twist bones")


def face_bones_of(tpl):
    """Template bones of the face (face_master and below, the jaw and below): an unrigged model's weights copied from
    the template give them to the head, the face rig (b4bface) then skins the model's own face; copied as they are,
    they moved whatever sat near the template's chin or lids (hair, a mismatched jaw line) and tore at the face rig's
    edge."""
    kids = {}
    for b in tpl.arm.data.bones:
        if b.parent: kids.setdefault(b.parent.name, []).append(b.name)
    out, todo = set(), [r for r in ("face_master", "jaw") if r in tpl.arm.data.bones]
    while todo:
        n = todo.pop()
        if n in out: continue
        out.add(n); todo += kids.get(n, [])
    return out


def fold_face(w, face):
    if not face or not any(n in face for n in w): return w
    out = {}
    for n, x in w.items():
        k = "head" if n in face else n
        out[k] = out.get(k, 0.0) + x
    return out


def part_bones(name):
    """Template bones a named part of an unrigged model may be weighted to (None: any)."""
    p = parse_bone(name) or parse_bone(re.sub(r"[-_ .]?(mesh|geo|obj)\d*$", "", name, flags=re.I))
    if not p: return None
    part, sd = p[0], p[1]
    arm_all = ("clavicle", "upperarm", "lowerarm", "hand", "wrist", "elbow", "shoulder") + tuple(FINGERS)
    fam = {"upperarm": arm_all, "lowerarm": ("lowerarm", "hand", "wrist", "elbow") + tuple(FINGERS),
           "hand": ("hand", "wrist") + tuple(FINGERS), "clavicle": ("clavicle", "upperarm", "shoulder"),
           "thigh": ("thigh", "calf", "foot", "ball", "knee", "hip"), "calf_or_thigh": ("thigh", "calf", "foot", "ball", "knee", "hip"),
           "calf": ("calf", "foot", "ball", "knee"), "foot": ("foot", "ball"), "ball": ("ball",)}.get(part)
    if fam and sd: return {"prefixes": fam, "side": sd}
    if part == "head": return {"exact": ("head", "neck_02")}
    if part in ("spine", "pelvis"): return {"exact": ("pelvis", "spine_01", "spine_02", "spine_03", "neck_01")}
    return None


def allowed(bone, rule):
    if rule is None: return True
    if "exact" in rule: return bone in rule["exact"]
    return bone.endswith("_" + rule["side"]) and bone.startswith(rule["prefixes"])


def unpose_arms(tpl, meshes, chain=False):
    """Unrigged model: bring its arms into the template's bind pose. The template armature is posed so its arms point
    like the model's (upperarm rotated about the shoulder), the weights are copied from the posed template mesh (named
    parts like "arm-left" only take bones of that part), then the inverse pose is applied to the model.
    chain (first person): the FP bind pose bends the elbow (~40 deg) and the hand, so the model's straight arm is
    matched joint by joint (elbow and wrist on its shoulder-tip line at the template's proportions): upperarm, lowerarm
    and hand each turned. Turning only the upperarm left the model's hand on the template's metacarpals, so in game the
    hand hung below the view (no hands in first person). Returns True when both arms fitted onto the model's surface
    (fit_arm_pose; the FP arms are then made from this fit, save_rigged)."""
    arm = tpl.arm
    tv = [tm.matrix_world @ v.co for tm in tpl.meshes for v in tm.data.vertices]
    mv = [(m, m.matrix_world @ v.co) for m in meshes for v in m.data.vertices]
    rules = {m.name: part_bones(m.name) for m in meshes}
    named = {m.name: r for m, r in ((m, rules[m.name]) for m in meshes) if r}
    if named: log("unrigged: parts by name (their vertices only take those bones):",
                  {n: (r["prefixes"][1] + "_" + r["side"]) if "prefixes" in r else r["exact"][0] for n, r in named.items()})
    tlo = Vector((min(p.x for p in tv), min(p.y for p in tv), min(p.z for p in tv)))
    thi = Vector((max(p.x for p in tv), max(p.y for p in tv), max(p.z for p in tv)))
    rot, hands = {}, {}
    mkd = KDTree(len(mv))
    for i, (m, p) in enumerate(mv): mkd.insert(p, i)
    mkd.balance()
    graph = None
    fits, angs, errs = {}, {}, {}
    for sd, sign in (("l", 1), ("r", -1)):          # heroes face +X: their left is +Y
        ua = tpl.pos.get(f"upperarm_{sd}")
        if ua is None: continue
        # the template's arm tip: its most lateral vertices on that side, same measure as for the model
        def tip(points):
            pts = sorted(points, key=lambda p: -sign * p.y)[:max(8, len(points) // 400)]
            return sum(pts, Vector()) / len(pts)
        t_tip = tip([p for p in tv if sign * p.y > 0])
        arm_objs = [m for m in meshes if (named.get(m.name) or {}).get("side") == sd and
                    "upperarm" in (named.get(m.name) or {}).get("prefixes", ())]
        if arm_objs:
            pts = [p for m, p in mv if m in arm_objs]
            shoulder = max(pts, key=lambda p: p.z)          # the part's top end
            top = [p for p in pts if p.z > shoulder.z - 0.03]
            shoulder = sum(top, Vector()) / len(top)
            m_tip = max(pts, key=lambda p: (p - shoulder).length)
        else:
            # the template's shoulder, placed proportionally in the model's bounds
            mlo = Vector((min(p.x for m, p in mv), min(p.y for m, p in mv), min(p.z for m, p in mv)))
            mhi = Vector((max(p.x for m, p in mv), max(p.y for m, p in mv), max(p.z for m, p in mv)))
            f = Vector(((ua.x - tlo.x) / max(1e-6, thi.x - tlo.x), (ua.y - tlo.y) / max(1e-6, thi.y - tlo.y),
                        (ua.z - tlo.z) / max(1e-6, thi.z - tlo.z)))
            shoulder = Vector((mlo.x + f.x * (mhi.x - mlo.x), mlo.y + f.y * (mhi.y - mlo.y), mlo.z + f.z * (mhi.z - mlo.z)))
            m_tip = tip([p for m, p in mv if sign * p.y > 0])
        dt, dm = (t_tip - ua), (m_tip - shoulder)
        if dt.length < 1e-4 or dm.length < 1e-4: continue
        side = 'left' if sd == 'l' else 'right'
        el, wr = tpl.pos.get(f"lowerarm_{sd}"), tpl.pos.get(f"hand_{sd}")
        if chain and el is not None and wr is not None:
            # each segment of the template's arm mapped onto the model's (turned, stretched along itself, moved):
            # the model's joints land exactly on the FP skeleton's; 8 cm off and the game's FP view misses the hands
            def seg(a0, a1, b0, b1):
                da, db = a1 - a0, b1 - b0
                d = da.normalized()
                k = db.length / max(da.length, 1e-6)
                S = Matrix.Identity(3) + (k - 1.0) * Matrix(((d.x * d.x, d.x * d.y, d.x * d.z),
                                                             (d.y * d.x, d.y * d.y, d.y * d.z),
                                                             (d.z * d.x, d.z * d.y, d.z * d.z)))
                q = d.rotation_difference(db.normalized())
                return q, k, Matrix.Translation(b0) @ (q.to_matrix() @ S).to_4x4() @ Matrix.Translation(-a0)
            l1, l2, l3 = (el - ua).length, (wr - el).length, (t_tip - wr).length
            d = dm.normalized()
            e_m = shoulder + d * (dm.length * l1 / (l1 + l2 + l3))
            w_m = shoulder + d * (dm.length * (l1 + l2) / (l1 + l2 + l3))
            parts = [seg(ua, el, shoulder, e_m), seg(el, wr, e_m, w_m), seg(wr, t_tip, w_m, m_tip)]
            # the hand: its own frame (along the fingers, across to the thumb, palm normal) from the template's hand
            # vertices and the model's, so a hand hanging palm-in is turned into the FP grip's orientation
            ht = hand_frame([tm.matrix_world @ v.co for tm in tpl.meshes for v, w in zip(tm.data.vertices, mesh_weights(tm))
                             if w and max(w, key=w.get).endswith("_" + sd) and HAND_BONE_RX.match(max(w, key=w.get))], wr)
            fa = (w_m - e_m).normalized()
            hl = (m_tip - w_m).length
            hm = hand_frame([p for mm, p in mv if (p - w_m).dot(fa) > 0.01 * hl and
                             (p - (w_m + fa * (p - w_m).dot(fa))).length < 0.8 * hl], w_m)
            hands[sd] = (w_m, fa, hl)
            if ht and hm:
                R3 = hm @ ht.transposed()
                k2 = parts[1][1]
                q3 = R3.to_quaternion()
                parts[2] = (q3, k2, Matrix.Translation(w_m) @ (R3 * k2).to_4x4() @ Matrix.Translation(-wr))
            log(f"unrigged: {side} arm onto the FP skeleton: " + ", ".join(
                f"{n} {math.degrees(q.angle):.0f} deg x{k:.2f}" for n, (q, k, M) in zip(("upper arm", "forearm", "hand"),
                                                                                       parts)))
            rot[sd] = [(f"{b}_{sd}", M) for b, (q, k, M) in zip(("upperarm", "lowerarm", "hand"), parts)]
            continue
        q = dt.normalized().rotation_difference(dm.normalized())
        ang = math.degrees(q.angle)
        log(f"unrigged: {side} arm is {ang:.0f} deg from the template's pose")
        angs[sd] = ang
        # then each arm segment turned onto the model's own arm (its surface): the tip guess alone left a hand
        # hanging behind the template's by 8 cm, which took forearm and thigh weights (and missed the FP view)
        if graph is None: graph, ntree = model_graph(meshes), NormalTrees(meshes)
        fit = fit_arm_pose(tpl, mkd, graph, sd, q if ang > 8 else Quaternion(), side, ntree, errs)
        fits[sd] = bool(fit)
        if fit:
            rot[sd] = fit
        elif ang > 8:
            rot[sd] = [(f"upperarm_{sd}", Matrix.Translation(ua) @ q.to_matrix().to_4x4() @ Matrix.Translation(-ua))]
    if chain and rot:
        return _unpose_chain(tpl, meshes, rules, rot, hands)
    # pose the template (armature space == world: the template armature has no transform of its own)
    Mw = arm.matrix_world

    def pose(inverse):
        select_only([arm], arm)
        bpy.ops.object.mode_set(mode="POSE")
        for sd, bones in rot.items():
            for name, M in bones:                     # parents first: each set in armature space
                pb = arm.pose.bones[name]
                pb.matrix = Mw.inverted() @ (M.inverted() if inverse else M) @ Mw @ pb.bone.matrix_local
                bpy.context.view_layer.update()
        bpy.ops.object.mode_set(mode="OBJECT")
    pose(False)
    if os.environ.get("B4B_DEBUG_ARMS"):
        bpy.ops.wm.save_as_mainfile(filepath=os.environ["B4B_DEBUG_ARMS"], copy=True)
    # weights from the posed template (evaluated meshes), restricted by part names
    dg = bpy.context.evaluated_depsgraph_get()
    pts, wts = [], []
    for tm in tpl.meshes:
        ev = tm.evaluated_get(dg)
        me = ev.to_mesh()
        W = mesh_weights(tm)
        for v, w in zip(me.vertices, W):
            pts.append(tm.matrix_world @ v.co); wts.append(w)
        ev.to_mesh_clear()
    kd = KDTree(len(pts))
    for i, p in enumerate(pts): kd.insert(p, i)
    kd.balance()
    face = face_bones_of(tpl)
    outs = {}
    for m in meshes:
        rule = rules[m.name]
        out = []
        for v in m.data.vertices:
            p = m.matrix_world @ v.co
            acc, tot = {}, 0.0
            for co, i, d in kd.find_n(p, 16 if rule else 4):
                w = {n: x for n, x in wts[i].items() if allowed(n, rule)}
                if not w: continue
                f = 1.0 / max(d, 1e-4); tot += f
                for n, x in w.items(): acc[n] = acc.get(n, 0.0) + x * f
            if not acc and rule:                     # nothing of that part nearby: the part's first bone
                first = rule["exact"][0] if "exact" in rule else f"{rule['prefixes'][0]}_{rule['side']}"
                acc, tot = {first: 1.0}, 1.0
            out.append(fold_face({n: x / tot for n, x in acc.items()}, face) if tot else {"pelvis": 1.0})
        outs[m] = out
    # arms far from the template's pose (hanging down) and fitted closely onto the model's own arm: that arm apart
    # from what it hangs against. Not arms turned by their tip only or fitted loosely (the posed template's arm isn't
    # on the model's: its tube would cut the model's arm), nor arms near the A-pose (turned a little, the fade into
    # the clothes bends smoothly with them)
    fitted_rot = {sd: r for sd, r in rot.items() if fits.get(sd) and angs.get(sd, 0.0) > HARD_ARM_ANGLE
                  and errs.get(sd, 1.0) < HARD_ARM_FIT}
    if fitted_rot: harden_arms(tpl, [m for m in meshes if not rules[m.name]], fitted_rot, outs, pts, wts)
    for m in meshes: set_weights(m, outs[m])
    log("weights: copied from the template" + (" (posed like the model)" if rot else ""))
    fitted = len(fits) == 2 and all(fits.values())    # both arms fitted onto the model's surface
    if not rot: return fitted
    # un-pose: skin to the template with the inverse arm rotation, apply
    pose(True)
    before = {m: [v.co.copy() for v in m.data.vertices] for m in meshes}
    for m in meshes:
        md = m.modifiers.new("Unpose", "ARMATURE"); md.object = arm
        select_only([m], m)
        bpy.ops.object.modifier_apply(modifier=md.name)
    if fitted_rot: tear_stretched([m for m in meshes if not rules[m.name]], before)
    for pb in arm.pose.bones:
        pb.matrix_basis = Matrix.Identity(4)
    bpy.context.view_layer.update()
    log("unrigged: arms moved into the template's pose")
    return fitted


ARM_BONE_RX = re.compile(r"^(upperarm|lowerarm|hand|wrist|elbow|shoulder|thumb|index|middle|ring|pinky)_(.*_)?[lr]$")


def arm_segments(tpl, rot, sd, twts, tpts, bvh):
    """The model's arm as the fit posed the template's (rot): [(name, start, axis, core, reach, t0)] for the upper
    arm, forearm and hand. Rays from the posed bone to the model's outermost surface in front, behind and away from
    the body (not the nearest surface, which may be the coat the arm hangs against) give the arm's width and middle:
    the tube is moved onto that middle; core: what is that close is arm (1.3 x its radius + 1.5 cm, at least the
    template's own arm radius); reach: beyond it nothing is (1.9 x + 2 cm). Without those rays: around the bone, the
    template's radius and the outer side's distance. t0: where along the segment the arm starts to count (the
    shoulder, up to 0.4 of the upper arm, blends as the template does)."""
    pos = tpl.pos
    order = [f"{p}_{sd}" for p in ("clavicle", "upperarm", "lowerarm", "hand")]
    if not all(b in pos for b in order[1:]): return []
    Mb = dict(rot.get(sd, ()))
    M, Ms = Matrix.Identity(4), {}
    for b in order:
        if b in Mb: M = Mb[b]
        Ms[b] = M
    S, E, W = (Ms[b] @ pos[b] for b in order[1:])
    hand = [tpts[i] for i, w in enumerate(twts) if w and max(w, key=w.get).endswith("_" + sd) and
            HAND_BONE_RX.match(max(w, key=w.get))]
    fa = (W - E).normalized()
    hl = max([(p - W).dot(fa) for p in hand] or [0.15])
    T = W + fa * hl
    lat = Vector((0, 1 if sd == "l" else -1, 0))
    pre_of = {"upper arm": ("upperarm_", "shoulder_"), "forearm": ("lowerarm_", "elbow_"),
              "hand": ("hand_", "wrist_", "thumb_", "index_", "middle_", "ring_", "pinky_")}
    segs = []
    for name, a, b, t0 in (("upper arm", S, E, 0.4), ("forearm", E, W, 0.0), ("hand", W, T, 0.0)):
        ax = b - a
        if ax.length < 1e-4: continue
        ds = []                                       # tpts: the template posed like the model, as the bones
        for i, w in enumerate(twts):
            if not w: continue
            bn = max(w, key=w.get)
            if not (bn.endswith("_" + sd) and bn.startswith(pre_of[name])): continue
            t = (tpts[i] - a).dot(ax) / ax.length_squared
            if 0.2 < t < 0.8: ds.append((tpts[i] - (a + ax * t)).length)
        core = sorted(ds)[len(ds) // 2] if len(ds) > 10 else 0.03
        d = ax.normalized()
        out = lat - d * lat.dot(d)
        if out.length < 0.3: out = Vector((0, 0, 1)) - d * d.z       # an arm held out sideways: its top
        out.normalize()
        fb = d.cross(out).normalized()

        def last(o, dr):                              # the last surface that way (within 12 cm)
            q, x = o, None
            for _ in range(8):
                hit = bvh.ray_cast(q, dr, 0.12 - (q - o).length)
                if hit[0] is None: break
                x = (hit[0] - o).length; q = hit[0] + dr * 1e-4
            return x
        outs_, rads, offs = [], [], []
        for t in (0.3, 0.5, 0.7):
            o = a + ax * t
            ro, rf, rb = last(o, out), last(o, fb), last(o, -fb)
            if ro is not None: outs_.append(ro)
            if rf is not None and rb is not None:
                # the arm's width across (front to back), and its middle that way: the sleeve is round, so its
                # centre lies that far in from its outer side (the side against the body may be one with the coat)
                R = (rf + rb) / 2
                rads.append(R)
                offs.append(fb * ((rf - rb) / 2) + out * ((ro if ro is not None else R) - R))
        if len(rads) >= 2:
            R = sorted(rads)[len(rads) // 2]
            off = sum(offs, Vector()) / len(offs)
            segs.append((name, a + off, ax, max(core, 1.3 * R + 0.015), 1.9 * max(core, R) + 0.02, t0))
        else:
            r = sorted(outs_)[len(outs_) // 2] if outs_ else 0.0
            segs.append((name, a, ax, core, 1.6 * max(core, r) + 0.01, t0))
    return segs


def harden_arms(tpl, meshes, rot, outs, tpts, twts):
    """Arm or not, one or the other: below the shoulder, a vertex of the model's arm takes only that arm's bones, any
    other vertex none of them. Copied as they are, the weights fade from the arm into the clothes it hangs against (a
    coat's side and back, its hem by the hand, hair over the shoulder) over many centimetres, and un-posing the arm
    pulled that whole fade out with it in long streaks. The arm: inside the tube around each posed arm bone as thick as
    the model's arm there (arm_segments), else, where that isn't measured, most of its weights on the arm; lightly
    smoothed over the surface so the arm comes away along one seam (tear_stretched)."""
    from mathutils.bvhtree import BVHTree
    verts, polys = [], []
    for m in meshes:
        mw, base = m.matrix_world, len(verts)
        verts += [mw @ v.co for v in m.data.vertices]
        polys += [[base + i for i in p.vertices] for p in m.data.polygons]
    bvh = BVHTree.FromPolygons(verts, polys)
    body = [i for i, w in enumerate(twts) if w and not any(ARM_BONE_RX.match(n) for n in w)]
    bkd = KDTree(max(1, len(body)))
    for i in body: bkd.insert(tpts[i], i)
    bkd.balance()
    ids, nb = welded_ids(meshes)
    report = []
    for sd in ("l", "r"):
        if sd not in rot: continue
        segs = arm_segments(tpl, rot, sd, twts, tpts, bvh)
        if not segs: continue
        ai = [i for i, w in enumerate(twts) if any(ARM_BONE_RX.match(n) and n.endswith("_" + sd) for n in w)]
        akd = KDTree(max(1, len(ai)))
        for i in ai: akd.insert(tpts[i], i)
        akd.balance()
        mine = lambda n: ARM_BONE_RX.match(n) and n.endswith("_" + sd)
        A, beside = [], []
        for m in meshes:
            mw = m.matrix_world
            for v, w in zip(m.data.vertices, outs[m]):
                p = mw @ v.co
                a, near = None, False
                share = sum(x for n, x in w.items() if mine(n))
                # the nearest segment (as a capsule: the outside of a bent elbow lies past both of its segments);
                # not the shoulder (up to t0) and not past the hand (a bag in it keeps what it has)
                best = None
                for sg in segs:
                    name, a0, ax, core, reach, t0 = sg
                    t = (p - a0).dot(ax) / ax.length_squared
                    dist = (p - (a0 + ax * max(0.0, min(1.0, t)))).length
                    if best is None or dist < best[0]: best = (dist, t, sg)
                if best is not None and best[2][5] <= best[1] and (best[1] <= 1.0 or best[2][0] != "hand") and \
                        best[0] < 2.5 * best[2][4]:
                    dist, t, (name, a0, ax, core, reach, t0) = best
                    near = True
                    # a hand's fingers spread or curl away from its line: no limit there
                    a = 1.0 if dist <= core else 0.0 if dist >= reach and name != "hand" else share
                if a is None: a = share
                A.append(a); beside.append(near)
        A = smooth_over(ids, nb, A, SMOOTH_ARM)
        k = n_arm = n_body = 0
        for m in meshes:
            mw = m.matrix_world
            for v, w in zip(m.data.vertices, outs[m]):
                a, near = A[k], beside[k]; k += 1
                if not near: continue
                a0 = sum(x for n, x in w.items() if mine(n))
                p = mw @ v.co
                if a >= 0.5:
                    if a0 >= 1.0 - 1e-4: continue
                    nw = {n: x for n, x in w.items() if mine(n)}
                    if sum(nw.values()) < 0.2:                # the arm's bones from the template's arm there
                        nw = {}
                        for co, i, d in akd.find_n(p, 4):
                            f = 1.0 / max(d, 1e-4)
                            for n, x in twts[i].items():
                                if mine(n): nw[n] = nw.get(n, 0.0) + x * f
                    n_arm += 1
                else:
                    if a0 <= 1e-4: continue
                    nw = {n: x for n, x in w.items() if not mine(n)}
                    if sum(nw.values()) < 0.2:                # the template's body there
                        nw = {}
                        for co, i, d in bkd.find_n(p, 4):
                            f = 1.0 / max(d, 1e-4)
                            for n, x in twts[i].items(): nw[n] = nw.get(n, 0.0) + x * f
                    n_body += 1
                t = sum(nw.values()) or 1.0
                w.clear(); w.update({n: x / t for n, x in nw.items()})
        report.append(f"{'left' if sd == 'l' else 'right'} (" + ", ".join(
            f"{n} {c * 100:.0f}-{r * 100:.0f}" for n, _, _, c, r, _ in segs) + f" cm): {n_arm} vertices to the "
            f"arm, {n_body} off it")
    if report: log("unrigged: arms made one piece, apart from the clothes they hang against: " + "; ".join(report))


SMOOTH_ARM = 3          # smoothing passes over the arm's share before it is made hard (harden_arms)
HARD_ARM_ANGLE = 20     # degrees from the template's arm: arms posed further are made hard (harden_arms) ...
HARD_ARM_FIT = 0.035    # m: ... when the fitted upper arm and forearm lie at most this far from the model's surface


def welded_ids(meshes):
    """(welded id of every vertex of the meshes in order, neighbour sets of the welded ids): the model_graph weld."""
    key, ids, nb = {}, [], []
    for m in meshes:
        mw = m.matrix_world
        loc = []
        for v in m.data.vertices:
            p = mw @ v.co
            kk = (round(p.x * 5000), round(p.y * 5000), round(p.z * 5000))
            if kk not in key: key[kk] = len(nb); nb.append(set())
            loc.append(key[kk])
        for e in m.data.edges:
            a, b = loc[e.vertices[0]], loc[e.vertices[1]]
            if a != b: nb[a].add(b); nb[b].add(a)
        ids += loc
    return ids, nb


def smooth_over(ids, nb, vals, passes):
    """Per-vertex values averaged with their welded neighbours `passes` times; back per vertex."""
    import numpy as np
    n = len(nb)
    x, c = np.zeros(n), np.zeros(n)
    np.add.at(x, ids, vals); np.add.at(c, ids, 1.0)
    x /= np.maximum(c, 1)
    src = np.array([i for i, s in enumerate(nb) for _ in s], dtype=np.int64)
    dst = np.array([j for s in nb for j in s], dtype=np.int64)
    deg = np.bincount(src, minlength=n).astype(float)
    for _ in range(passes):
        acc = np.bincount(src, weights=x[dst], minlength=n)
        x = np.where(deg > 0, 0.5 * x + 0.5 * acc / np.maximum(deg, 1), x)
    return [float(x[i]) for i in ids]


TEAR_BACK = 0.12       # m: arm faces this close to a cut (tear_stretched) get a reversed copy


def arm_share(m, rx=None):
    """Each vertex's weight share on arm bones (below the clavicle; rx: those bones only)."""
    arm = {g.index for g in m.vertex_groups if ARM_BONE_RX.match(g.name) and (rx is None or rx.match(g.name))}
    out = []
    for v in m.data.vertices:
        t = sum(g.weight for g in v.groups)
        out.append(sum(g.weight for g in v.groups if g.group in arm) / t if t else 0.0)
    return out


def arm_back_faces(meshes, pts, reach, inset=0.0015):
    """Arm faces (all vertices on the arm bones; not the hands: a hand is closed) with their centre within reach of
    any of pts get a reversed copy inset along the vertex normals (as b4bdangle.add_back_faces does for a whole
    garment); weights and UVs come along."""
    kd = KDTree(len(pts))
    for i, p in enumerate(pts): kd.insert(p, i)
    kd.balance()
    n = 0
    for m in meshes:
        me, mw = m.data, m.matrix_world
        arm, hand = arm_share(m), arm_share(m, HAND_BONE_RX)
        sel = [f.index for f in me.polygons if all(arm[v] > 0.99 and hand[v] < 0.5 for v in f.vertices) and
               kd.find(mw @ f.center)[2] <= reach]
        if not sel: continue
        corner = [tuple(x.vector) for x in me.corner_normals]
        nl = len(me.loops)
        bm = bmesh.new(); bm.from_mesh(me); bm.faces.ensure_lookup_table()
        # copied by hand in face order (bmesh.ops.duplicate orders the new vertices by memory address: the same
        # model gave a different add-on each run)
        dl = bm.verts.layers.deform.active
        uvs = list(bm.loops.layers.uv.values())
        cols = list(bm.loops.layers.color.values())
        nv = {}
        for f in [bm.faces[i] for i in sel]:
            for v in f.verts:
                if v.index in nv: continue
                c = bm.verts.new(v.co - v.normal * inset)
                if dl is not None:
                    for g, w in v[dl].items(): c[dl][g] = w
                nv[v.index] = c
            loops = list(f.loops)[::-1]                        # reversed: the copy faces inward
            g = bm.faces.new([nv[l.vert.index] for l in loops], f)
            for lo, ln in zip(loops, g.loops):
                for L in uvs: ln[L].uv = lo[L].uv
                for L in cols: ln[L] = lo[L]
        bm.to_mesh(me); bm.free()
        # originals keep their normals (bmesh writes the old faces first), the copies face inward
        me.normals_split_custom_set(corner[:nl] + [tuple(me.vertices[me.loops[li].vertex_index].normal)
                                                   for li in range(nl, len(me.loops))])
        n += len(sel)
    if n: log(f"unrigged: {n} arm faces next to the cut got a reversed copy (the inside of the sleeve is drawn)")


def welded_pieces(meshes, skip=None):
    """The meshes' faces in connected pieces, welded by position (UV seams and split parts joined): ({mesh: welded id
    of each vertex}, find(welded id) -> piece). skip: {mesh: face indices} left out of the joining."""
    key, parent = {}, []

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]; i = parent[i]
        return i
    vid = {}
    for m in meshes:
        mw = m.matrix_world
        ids = []
        for v in m.data.vertices:
            p = mw @ v.co
            kk = (round(p.x * 5000), round(p.y * 5000), round(p.z * 5000))
            if kk not in key: key[kk] = len(parent); parent.append(len(parent))
            ids.append(key[kk])
        vid[m] = ids
    for m in meshes:
        ids, sk = vid[m], (skip or {}).get(m, ())
        for f in m.data.polygons:
            if f.index in sk: continue
            r = find(ids[f.vertices[0]])
            for v in f.vertices[1:]:
                q = find(ids[v])
                if q != r: parent[q] = r
    return vid, find


def tear_stretched(meshes, before, k=3.0, min_len=0.03, crumbs=0.015):
    """Faces the un-posing stretched to over k times their length (and over min_len): where the model's arm was one
    surface with the clothes it hung against (scans, generated models), a sheet now spans from the body to the arm.
    They are removed, and with them the crumbs the cut leaves: pieces (welded by position) under `crumbs` of the
    model's faces that were joined to the rest only through removed faces (a bag's handle between the hand and the
    bag), which would float in the air."""
    bad = {}
    for m in meshes:
        me, B = m.data, before[m]
        V = me.vertices
        bad[m] = set()
        for f in me.polygons:
            vs = list(f.vertices)
            for a, b in zip(vs, vs[1:] + vs[:1]):
                l1 = (V[a].co - V[b].co).length
                if l1 > min_len and l1 > k * max((B[a] - B[b]).length, 1e-5):
                    bad[m].add(f.index); break
    total = sum(len(x) for x in bad.values())
    if not total: return
    vid, find = welded_pieces(meshes, bad)                # pieces of what is left
    size, cut = {}, set()
    for m in meshes:
        ids = vid[m]
        for f in m.data.polygons:
            r = find(ids[f.vertices[0]])
            if f.index in bad[m]: cut.update(find(ids[v]) for v in f.vertices)
            else: size[r] = size.get(r, 0) + 1
    nf = sum(len(m.data.polygons) for m in meshes)
    small = {r for r in cut if size.get(r, 0) < crumbs * nf}
    n_small = 0
    edge = []                                             # where the cut runs on the arms
    for m in meshes:
        ids = vid[m]
        gone = [f.index for f in m.data.polygons if f.index in bad[m] or find(ids[f.vertices[0]]) in small]
        n_small += len(gone) - len(bad[m])
        if not gone: continue
        arm = arm_share(m)
        edge += [m.matrix_world @ m.data.vertices[v].co for i in bad[m] for v in m.data.polygons[i].vertices
                 if arm[v] > 0.99 and find(ids[v]) not in small]
        bm = bmesh.new(); bm.from_mesh(m.data); bm.faces.ensure_lookup_table()
        bmesh.ops.delete(bm, geom=[bm.faces[i] for i in gone], context="FACES")
        bm.to_mesh(m.data); bm.free()
    log(f"unrigged: {total} faces stretched between the body and an arm (the arm was one surface with the clothes it "
        f"hung against): removed" + (f", and {n_small} faces of {len(small)} small pieces cut loose by it" if n_small
                                     else ""))
    # the arm has no inner side where it was one with the clothes: the game's one-sided materials would show the
    # sleeve hollow (in first person the forearm's open side faces the camera). The arm's faces around the cut get a
    # reversed copy, so its inside is drawn
    if edge: arm_back_faces(meshes, edge, TEAR_BACK)


ARM_PARTS = (("clavicle", ("clavicle_",)), ("upperarm", ("upperarm_", "shoulder_")),
             ("lowerarm", ("lowerarm_", "elbow_")), ("hand", ("hand_", "wrist_", "thumb_", "index_", "middle_", "ring_",
                                                             "pinky_")))
ARM_FIT_MAX = 0.03         # m: mean hand surface distance above which the segment fit is not used
ARM_FIT_LIMIT = {"clavicle": 25, "upperarm": 70, "lowerarm": 70, "hand": 70}    # degrees per segment


def model_graph(meshes):
    """The model's vertices welded by position (all objects, UV seams closed) and their edges: (points, neighbours)."""
    import numpy as np
    key, pts, nb = {}, [], []
    ids = []
    for m in meshes:
        mw = m.matrix_world
        loc = []
        for v in m.data.vertices:
            p = mw @ v.co
            k = (round(p.x * 5000), round(p.y * 5000), round(p.z * 5000))
            if k not in key:
                key[k] = len(pts); pts.append(tuple(p)); nb.append(set())
            loc.append(key[k])
        for e in m.data.edges:
            a, b = loc[e.vertices[0]], loc[e.vertices[1]]
            if a != b: nb[a].add(b); nb[b].add(a)
    return np.array(pts).reshape(-1, 3), nb


def hand_vertices(graph, J, fa, reach):
    """The model's hand: vertices reached over the mesh within `reach` of the ones around the wrist joint J, on the
    far side of the wrist (fa: forearm direction). Over the surface, so a thigh next to a hanging hand isn't taken."""
    import heapq
    import numpy as np
    P, nb = graph
    d0 = np.linalg.norm(P - np.array(tuple(J)), axis=1)
    seeds = np.nonzero(d0 < 0.04)[0]
    if not len(seeds): return []
    dist = {int(i): 0.0 for i in seeds}
    h = [(0.0, int(i)) for i in seeds]
    heapq.heapify(h)
    while h:
        d, i = heapq.heappop(h)
        if d > dist.get(i, 1e9): continue
        for j in nb[i]:
            nd = d + float(np.linalg.norm(P[i] - P[j]))
            if nd < reach and nd < dist.get(j, 1e9):
                dist[j] = nd; heapq.heappush(h, (nd, j))
    f = np.array(tuple(fa))
    return [Vector(P[i]) for i in dist if float((P[i] - np.array(tuple(J))) @ f) > 0.2 * reach / 1.3]


class NormalTrees:
    """The model's vertices (world space) in KD-trees by the direction their normal faces (26 directions, each tree
    holding the vertices within 60 deg of it): the nearest vertex facing about the same way as a given normal; and
    the model's surface for rays (bvh)."""
    DIRS = [Vector((x, y, z)).normalized() for x in (-1, 0, 1) for y in (-1, 0, 1) for z in (-1, 0, 1) if x or y or z]

    def __init__(self, meshes):
        from mathutils.bvhtree import BVHTree
        points, normals, polys = [], [], []
        for m in meshes:
            mw, mn = m.matrix_world, m.matrix_world.to_3x3().inverted_safe().transposed()
            base = len(points)
            for v in m.data.vertices:
                points.append(mw @ v.co); normals.append(mn @ v.normal)
            polys += [[base + i for i in p.vertices] for p in m.data.polygons]
        self.bvh = BVHTree.FromPolygons(points, polys)
        groups = [[] for _ in self.DIRS]
        for i, (p, n) in enumerate(zip(points, normals)):
            if n.length < 1e-9: continue
            n = n.normalized()
            for g, d in zip(groups, self.DIRS):
                if n.dot(d) > 0.5: g.append(i)
        self.trees = []
        for g in groups:
            kd = KDTree(max(1, len(g)))
            for i in g: kd.insert(points[i], i)
            kd.balance()
            self.trees.append(kd if g else None)

    def dist(self, p, n, clip):
        """Distance from p to the nearest vertex whose normal faces about like n (at most clip)."""
        if n.length < 1e-9: return clip
        kd = self.trees[max(range(len(self.DIRS)), key=lambda j: self.DIRS[j].dot(n))]
        return min(kd.find(p)[2], clip) if kd else clip

    def outermost(self, p, d, reach):
        """Distance from p along d to the last surface of the model that way (within reach), or None."""
        last, o = None, p
        for _ in range(8):
            hit = self.bvh.ray_cast(o, d, reach - (o - p).length)
            if hit[0] is None: break
            last = (hit[0] - p).length
            o = hit[0] + d * 1e-4
        return last


def fit_arm_pose(tpl, mkd, graph, sd, q0, side, ntree=None, errs=None):
    """Unrigged model: turn the template's arm segment by segment (clavicle, upper arm, forearm; each about its
    joint, children carried along) so its surface lies on the model's (mean distance to the model's nearest vertex,
    each segment's points and those below it), then the hand onto the model's hand (found over the mesh from the
    wrist: its long axis, and the thumb side by PCA). q0: the upper arm's first guess. Returns
    [(bone, total world transform)] parents first, or None."""
    import numpy as np
    pos = tpl.pos
    names = [p for p, _ in ARM_PARTS]
    if not all(f"{p}_{sd}" in pos for p in names): return None
    pts = {p: [] for p in names}
    nrm = {p: [] for p in names}
    for tm in tpl.meshes:
        tn = tm.matrix_world.to_3x3().inverted_safe().transposed()
        for v, w in zip(tm.data.vertices, mesh_weights(tm)):
            if not w: continue
            b = max(w, key=w.get)
            if not b.endswith("_" + sd): continue
            for p, pre in ARM_PARTS:
                if b.startswith(pre):
                    pts[p].append(tuple(tm.matrix_world @ v.co)); nrm[p].append(tuple((tn @ v.normal).normalized()))
                    break
    if any(len(pts[p]) < 20 for p in names[1:]): return None
    P = {p: np.array(x[::max(1, len(x) // 400)] if x else np.zeros((0, 3))).reshape(-1, 3) for p, x in pts.items()}
    N = {p: np.array(x[::max(1, len(x) // 400)] if x else np.zeros((0, 3))).reshape(-1, 3) for p, x in nrm.items()}
    tot = {p: Matrix.Identity(4) for p in names}
    CLIP = 0.10
    lat = Vector((0, 1 if sd == "l" else -1, 0))     # heroes face +X: their left is +Y

    def near(A, NA=None):
        if not len(A): return np.zeros(0)
        if ntree is not None and NA is not None:
            # to the nearest model vertex facing the same way: an arm pressed into a coat's side or lying across the
            # skirt is near the model's surface everywhere, but only a real arm has surface facing every way round it
            out = []
            for a, n in zip(A, NA):
                a, n = Vector(a), Vector(n)
                e = ntree.dist(a, n, CLIP)
                if n.dot(lat) > 0.6:
                    # the arm's outer side against the model's outermost surface that way: inside the coat, next to
                    # the arm, the coat's wall is near too, but the sleeve is still further out
                    last = ntree.outermost(a, lat, CLIP)
                    if last is not None: e = max(e, last)
                out.append(e)
            return np.array(out)
        return np.array([min(mkd.find(Vector(a))[2], CLIP) for a in A])

    def moved(p, M):
        A = P[p]
        if not len(A): return A
        R = np.array(M)
        return A @ R[:3, :3].T + R[:3, 3]

    def turned(p, M):
        return N[p] @ np.array(M)[:3, :3].T if len(N[p]) else N[p]

    def err(k, S):
        e = [near(moved(p, S @ tot[p]), turned(p, S @ tot[p])).mean() for p in names[k:] if len(P[p])]
        if names[k] == "hand":                         # the model's vertices around the hand -> the template's hand
            H = moved("hand", S @ tot["hand"])
            c = H.mean(0)
            r = 0.6 * float(np.linalg.norm(H - c, axis=1).max())
            around = [x[0] for x in mkd.find_range(Vector(c), r)]
            if around:
                hk = KDTree(len(H))
                for i, h in enumerate(H): hk.insert(Vector(h), i)
                hk.balance()
                e.append(np.mean([min(hk.find(a)[2], CLIP) for a in around[::max(1, len(around) // 400)]]))
        return float(np.mean(e))

    out, report = [], []
    before = err(len(names) - 1, Matrix.Identity(4))
    for k, p in enumerate(names):
        J = (tot[names[k - 1]] if k else Matrix.Identity(4)) @ pos[f"{p}_{sd}"]
        about = lambda q: Matrix.Translation(J) @ q.to_matrix().to_4x4() @ Matrix.Translation(-J)
        q = q0.copy() if p == "upperarm" else Quaternion()
        if p == "hand":
            E = tot["lowerarm"] @ pos[f"lowerarm_{sd}"]
            fa = (J - E).normalized()
            H = [Vector(x) for x in moved("hand", tot["hand"])]
            hl = max((x - J).dot(fa) for x in H)
            mh = hand_vertices(graph, J, fa, 1.3 * hl)
            ht, hm = palm_frame(H, J), palm_frame(mh, J)
            if ht is not None and hm is not None:
                # along the hand (wrist -> its centre), then about that axis until the palms' normals agree (the
                # flat hand's thinnest direction; its sign: the one nearer the template's, turns stay under 90 deg)
                q = ht[0].rotation_difference(hm[0])
                ax = hm[0]
                a = q @ ht[1]; b = hm[1] if hm[1].dot(q @ ht[1]) >= 0 else -hm[1]
                a = a - ax * a.dot(ax); b = b - ax * b.dot(ax)
                roll = math.atan2(a.cross(b).dot(ax), a.dot(b)) if a.length > 1e-6 and b.length > 1e-6 else 0.0
                q = Quaternion(ax, roll) @ q
                if math.degrees(q.angle) > ARM_FIT_LIMIT[p]:
                    q = Quaternion(q.axis, math.radians(ARM_FIT_LIMIT[p]))
            S = about(q)
            tot["hand"] = S @ tot["hand"]
            out.append((f"hand_{sd}", tot["hand"].copy()))
            report.append(f"hand {math.degrees(q.angle):.0f} deg ({len(mh)} hand vertices)")
            continue
        best = err(k, about(q))
        for step in (12, 6, 3, 1.5, 0.75):
            for _ in range(12):
                better = False
                for ax in ((1, 0, 0), (0, 1, 0), (0, 0, 1)):
                    for sg in (1, -1):
                        qq = Quaternion(Vector(ax), math.radians(step * sg)) @ q
                        if math.degrees(qq.angle) > ARM_FIT_LIMIT[p]: continue
                        e = err(k, about(qq))
                        if e < best - 1e-6:
                            best, q, better = e, qq, True
                if not better: break
        S = about(q)
        for x in names[k:]: tot[x] = S @ tot[x]
        out.append((f"{p}_{sd}", tot[p].copy()))
        report.append(f"{p} {math.degrees(q.angle):.0f} deg")
    after = err(len(names) - 1, Matrix.Identity(4))
    if after > ARM_FIT_MAX:
        # the template's arm doesn't lie on the model's anywhere (stylised or blocky proportions): the tip guess
        log(f"unrigged: {side} arm: no fit onto the model's surface (hand {after * 100:.1f} cm off); arm turned by "
            f"its tip only")
        return None
    # how far the upper arm and forearm lie from the model's surface (the hand is checked above): an arm fitted
    # with its elbow off the model's (a local best of the turn search) still passes the hand test
    seg = {p: float(np.mean([min(mkd.find(Vector(a))[2], CLIP) for a in moved(p, tot[p])]))
           for p in ("upperarm", "lowerarm") if len(P[p])}
    if errs is not None: errs[sd] = max(seg.values(), default=0.0)
    log(f"unrigged: {side} arm fitted onto the model's: " + ", ".join(report) +
        f"; hand surface distance {before * 100:.1f} -> {after * 100:.1f} cm (" +
        ", ".join(f"{p} {x * 100:.1f}" for p, x in seg.items()) + " cm)")
    return out


HAND_BONE_RX = re.compile(r"^(hand|thumb|index|middle|ring|pinky)_")


def palm_frame(pts, wrist):
    """(direction wrist -> hand centre, palm normal = the hand's thinnest PCA axis) of a hand's vertices, or None."""
    import numpy as np
    if len(pts) < 30: return None
    P = np.array([tuple(p) for p in pts])
    c = P.mean(0)
    d = Vector(tuple(c)) - wrist
    if d.length < 1e-4: return None
    w, V = np.linalg.eigh(np.cov((P - c).T))
    return d.normalized(), Vector(tuple(V[:, 0])).normalized()


def hand_frame(pts, wrist):
    """Rotation matrix (columns: along the fingers, towards the thumb, palm normal) of a hand's vertices by PCA: the
    long axis points away from the wrist, the middle one towards the side the thumb sticks out (third moment)."""
    import numpy as np
    if len(pts) < 30: return None
    P = np.array([tuple(p) for p in pts])
    c = P.mean(0)
    w, V = np.linalg.eigh(np.cov((P - c).T))
    major, mid = V[:, 2], V[:, 1]
    if (c - np.array(tuple(wrist))) @ major < 0: major = -major
    if (((P - c) @ mid) ** 3).sum() < 0: mid = -mid
    n = np.cross(major, mid)
    return Matrix([list(major), list(mid), list(n)]).transposed()


def _unpose_chain(tpl, meshes, rules, rot, hands):
    """unpose_arms for first person: the per-bone transforms (with stretch, which a Blender pose can't hold on these
    bones) applied by hand, linear blend skinning: the template's vertices posed like the model for the weights, then
    the model's vertices (and custom normals) through the inverse transforms onto the FP skeleton."""
    import numpy as np
    arm = tpl.arm
    Mb = {name: M for bones in rot.values() for name, M in bones}
    Mi = {n: M.inverted() for n, M in Mb.items()}
    owner = {}
    for b in arm.data.bones:                          # a bone below a turned one (fingers, twists) moves with it
        x = b
        while x is not None and x.name not in Mb: x = x.parent
        owner[b.name] = x.name if x is not None else None

    def skin(p, w, mats):
        acc, tot = Vector(), 0.0
        for n, x in w.items():
            o = owner.get(n)
            acc += x * ((mats[o] @ p) if o else p); tot += x
        return acc / tot if tot > 0 else p
    pts, wts = [], []
    for tm in tpl.meshes:
        W = mesh_weights(tm)
        for v, w in zip(tm.data.vertices, W):
            pts.append(skin(tm.matrix_world @ v.co, w, Mb)); wts.append(w)
    kd = KDTree(len(pts))
    for i, p in enumerate(pts): kd.insert(p, i)
    kd.balance()
    face = face_bones_of(tpl)
    for m in meshes:
        rule = rules[m.name]
        out = []
        for v in m.data.vertices:
            p = m.matrix_world @ v.co
            acc, tot = {}, 0.0
            for co, i, d in kd.find_n(p, 16 if rule else 4):
                w = {n: x for n, x in wts[i].items() if allowed(n, rule)}
                if not w: continue
                f = 1.0 / max(d, 1e-4); tot += f
                for n, x in w.items(): acc[n] = acc.get(n, 0.0) + x * f
            if not acc and rule:
                first = rule["exact"][0] if "exact" in rule else f"{rule['prefixes'][0]}_{rule['side']}"
                acc, tot = {first: 1.0}, 1.0
            w = fold_face({n: x / tot for n, x in acc.items()}, face) if tot else {"pelvis": 1.0}
            # hand and finger bones only for the model's hands (the thigh next to a hanging hand would follow it)
            inhand = any((p - wm).dot(fa) > 0 and (p - (wm + fa * (p - wm).dot(fa))).length < 0.8 * hl
                         for wm, fa, hl in hands.values())
            if not inhand and any(HAND_BONE_RX.match(n) for n in w):
                rest = {n: x for n, x in w.items() if not HAND_BONE_RX.match(n)}
                tot2 = sum(rest.values())
                if tot2 > 1e-6: w = {n: x / tot2 for n, x in rest.items()}
            out.append(w)
        set_weights(m, out)
        # un-pose: each vertex by its weights' inverse transforms; custom normals by the blended linear part
        me = m.data
        mw, mwi = m.matrix_world, m.matrix_world.inverted()
        lin = []
        for v, w in zip(me.vertices, out):
            p = mw @ v.co
            A = Matrix(((0, 0, 0), (0, 0, 0), (0, 0, 0))); tot = 0.0
            for n, x in w.items():
                o = owner.get(n)
                A += x * (Mi[o].to_3x3() if o else Matrix.Identity(3)); tot += x
            v.co = mwi @ skin(p, w, Mi)
            lin.append((A * (1.0 / tot)).inverted_safe().transposed() if tot > 0 else Matrix.Identity(3))
        if me.has_custom_normals:
            cn = [lin[me.loops[i].vertex_index] @ Vector(n.vector) for i, n in enumerate(me.corner_normals)]
            me.normals_split_custom_set([tuple(x.normalized()) for x in cn])
        me.update()
    log("weights: copied from the template (posed like the model)")
    log("unrigged: arms moved onto the FP skeleton's joints")


def save_rigged(path, arm, meshes):
    """The unrigged model as the 3P fit rigged it (in the survivor's bind pose, skinned to the survivor skeleton with
    the template's weights) as a .blend: its first-person arms are then fitted like a rigged model's, every joint
    exactly on the FP skeleton's (the FP view is tight: arms a few cm off the FP joints never show)."""
    import bpy as _b
    rig = arm.copy(); rig.data = arm.data.copy(); rig.name = rig.data.name = "Rig"
    rig.animation_data_clear()
    for pb in rig.pose.bones: pb.matrix_basis = Matrix.Identity(4)
    outs, names = [], []
    for m in meshes:
        names.append((m, m.name))
        nm = m.name; m.name = nm + "__b4b_orig"
        c = m.copy(); c.data = m.data.copy(); c.name = nm      # the model's own object names (gear, accessories)
        for md in list(c.modifiers): c.modifiers.remove(md)
        c.parent = rig; c.matrix_parent_inverse = rig.matrix_world.inverted()
        md = c.modifiers.new("Armature", "ARMATURE"); md.object = rig
        outs.append(c)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    _b.data.libraries.write(path, {rig, *outs}, path_remap="ABSOLUTE", fake_user=False)
    for c in outs:
        me = c.data; _b.data.objects.remove(c); _b.data.meshes.remove(me)
    for m, nm in names: m.name = nm
    ad = rig.data; _b.data.objects.remove(rig); _b.data.armatures.remove(ad)
    log(f"unrigged: rigged model for the first-person arms -> {path}")


def body_segments(tpl):
    """[(bone, head, end)] of the template skeleton's body bones: each joint to the next mapped joint (AIM), end bones
    (head, feet, finger tips) along their own bone."""
    segs = []
    pos = tpl.pos
    for b in tpl.arm.data.bones:
        n = b.name
        if n not in B4B_MAIN and not ARM_BONES_RX.match(n): continue
        aim = next((x for x in AIM.get(n, []) if x in pos), None)
        end = pos[aim] if aim else tpl.arm.matrix_world @ b.tail_local
        if n == "head": end = pos[n] + Vector((0, 0, 0.22))      # the skull, not the bone's short tail
        segs.append((n, pos[n], end))
    return segs


def fp_arm_mask(tpl, m, segs):
    """Unrigged source in first person: which vertices belong to the arms, by the nearest body segment of the
    template skeleton (the FP template mesh is only arms, so its nearest-vertex weights make everything an arm)."""
    import numpy as np
    P = np.array([tuple(m.matrix_world @ v.co) for v in m.data.vertices]).reshape(-1, 3)
    best = np.full(len(P), np.inf)
    arm = np.zeros(len(P), bool)
    for n, a, b in segs:
        a, ab = np.array(a), np.array(b) - np.array(a)
        f = np.clip((P - a) @ ab / max(float(ab @ ab), 1e-12), 0.0, 1.0)
        d = np.linalg.norm(a + f[:, None] * ab - P, axis=1)
        closer = d < best
        best[closer] = d[closer]
        arm[closer] = bool(ARM_BONES_RX.match(n))
    arm &= best < 0.12                               # nothing far from every bone (parts of the body the arm fit threw)
    return arm.tolist()


FP_HANG = 0.10           # m: a piece of the first-person arms whose vertices lie (median) further from the arm bones
FP_HANG_FACE = 0.20      # m: ... or with over FP_HANG_SHARE of them further than this, and any face all of whose
FP_HANG_SHARE = 0.2      # vertices are further, is left out (fp_drop_hanging)


def fp_drop_hanging(tpl, meshes):
    """First person: leave out what hangs off the arms: pieces (welded by position) lying mostly more than FP_HANG
    from the arm bones or reaching past FP_HANG_FACE with a good part of them, and faces further than FP_HANG_FACE.
    Wide kimono sleeves, ribbons, ornaments and props held in the hand are skinned to the forearm and hand (their own
    swinging bones aren't the survivor's): in the first-person view, where the game bends the arms right in front of
    the camera, they stood up as big planks and spikes over the gun."""
    import numpy as np
    segs = [(n, a, b) for n, a, b in body_segments(tpl) if ARM_BONES_RX.match(n) or n.startswith("clavicle_")]
    if not segs: return
    A = np.array([tuple(a) for _, a, _ in segs]); AB = np.array([tuple(b) for _, _, b in segs]) - A
    L2 = np.maximum((AB * AB).sum(1), 1e-12)

    left = np.array([n.endswith("_l") for n, _, _ in segs])

    def dist(P):
        """(distance to the nearest arm bone, on the left arm's) per point"""
        P = np.asarray(P).reshape(-1, 3)
        t = np.clip(np.einsum("nsk,sk->ns", P[:, None, :] - A[None], AB) / L2, 0.0, 1.0)
        d = np.linalg.norm(A[None] + t[..., None] * AB[None] - P[:, None, :], axis=2)
        return d.min(1), left[d.argmin(1)]
    vid, find = welded_pieces(meshes)
    D, L = {}, {}
    for m in meshes:
        D[m], L[m] = dist([tuple(m.matrix_world @ v.co) for v in m.data.vertices]) if len(m.data.vertices) else \
            (np.zeros(0), np.zeros(0, bool))
    per, side = {}, {}
    for m in meshes:
        for i, x in enumerate(vid[m]):
            r = find(x)
            per.setdefault(r, []).append(D[m][i]); side[r] = side.get(r, 0) + (1 if L[m][i] else -1)
    # each arm's biggest piece is the arm itself, however far from the survivor's bones (a blocky figure's arm boxes)
    keep = {max((r for r in per if (side[r] > 0) == sd), key=lambda r: len(per[r]), default=None) for sd in (True, False)}
    far = {r for r, ds in per.items() if r not in keep and (float(np.median(ds)) > FP_HANG or
                                                             float(np.mean(np.array(ds) > FP_HANG_FACE)) > FP_HANG_SHARE)}
    n_p, n_f = len(far), 0
    for m in meshes:
        ids, d = vid[m], D[m]
        gone = [f.index for f in m.data.polygons if find(ids[f.vertices[0]]) in far or
                (find(ids[f.vertices[0]]) not in keep and all(d[v] > FP_HANG_FACE for v in f.vertices))]
        if not gone: continue
        n_f += len(gone)
        bm = bmesh.new(); bm.from_mesh(m.data); bm.faces.ensure_lookup_table()
        bmesh.ops.delete(bm, geom=[bm.faces[i] for i in gone], context="FACES")
        bm.to_mesh(m.data); bm.free()
    if n_f:
        log(f"fp: left out {n_f} faces hanging off the arms ({n_p} pieces lying mostly over {FP_HANG * 100:.0f} cm from "
            f"the arm bones: wide sleeves, ribbons, ornaments; they would stand up over the gun in first person)")


def keep_arms(m, mask=None):
    W = mesh_weights(m)
    armv = [sum(x for n, x in w.items() if ARM_BONES_RX.match(n)) / max(1e-6, sum(w.values())) >= 0.5 for w in W]
    if mask is not None: armv = [a and k for a, k in zip(armv, mask)]
    bm = bmesh.new(); bm.from_mesh(m.data)
    kill = [f for f in bm.faces if not all(armv[v.index] for v in f.verts)]
    if mask is not None:
        # unrigged: a material with only a sliver on the arms (hair over the shoulders, trousers by the hands) is
        # not part of the arms
        tot, cut = {}, {}
        for f in bm.faces: tot[f.material_index] = tot.get(f.material_index, 0) + 1
        for f in kill: cut[f.material_index] = cut.get(f.material_index, 0) + 1
        mats = m.data.materials
        mname = lambda i: mats[i].name if i < len(mats) and mats[i] else NO_MATERIAL
        few = {i for i, n in tot.items() if 0 < n - cut.get(i, 0) and (n - cut.get(i, 0) < FP_MIN_SHARE * n or
                                                                       FP_HAIR_RX.search(mname(i)))}
        if few:
            ks = set(kill)
            kill += [f for f in bm.faces if f.material_index in few and f not in ks]
            log(f"fp: {m.name}: left out " + ", ".join(
                f"{mname(i)} ({tot[i] - cut.get(i, 0)} of {tot[i]} "
                f"faces on the arms)" for i in sorted(few)))
    bmesh.ops.delete(bm, geom=kill, context="FACES")
    bm.to_mesh(m.data); bm.free()
    log(f"fp: {m.name}: kept {len(m.data.polygons)} arm faces")


# ---- materials, atlas, LODs, export ---------------------------------------------------------------------------------

TEX_KEYS = {"basecolor": ("albedo", "basecolor", "base_color", "basemap", "base_map", "diffuse", "color", "colour", "_bc",
                          "_d.", "_d_", "_diff", "_col", "_alb", "_cl."),
            "normal": ("normal", "_n.", "_n_", "_nrm", "_nor", "norm"),
            "roughness": ("rough",), "metallic": ("metal",), "ao": ("_ao", "occlusion", "ambient"),
            "orm": ("rmao", "_orm", "orm.", "_arm.", "occlusionroughnessmetallic"), "mask": ("maskmap", "mask_map", "_mask"),
            "gloss": ("gloss", "smooth"), "alpha": ("alpha", "opacity", "transparen"),
            "skip": ("emissi", "height", "displace", "_disp", "bump", "spec", "sss", "subsurface", "cavity", "curvature",
                     "thickness", "id_map", "_id.", "matcap", "shade", "_rim", "outline", "_dfl.")}
IMG_EXT = (".png", ".jpg", ".jpeg", ".tga", ".tif", ".tiff", ".bmp", ".webp", ".dds")   # DDS: BC1-BC7 (game rips)
NO_MATERIAL = "none"                  # the label of faces / objects without a material (--slot none=..., --tex none=...)
TEX_CACHE = None                      # set per run: where embedded (packed) images are written


def role_from_name(path):
    """What an image file is, from its name (None: can't tell). Checked most specific first."""
    fl = os.path.basename(path).lower()
    for key in ("skip", "normal", "orm", "mask", "roughness", "gloss", "metallic", "ao", "alpha", "basecolor"):
        if any(w in fl for w in TEX_KEYS[key]): return key
    return None


def image_file(img):
    """A file path for a Blender image: its file, else its embedded bytes (glb/vrm/FBX embedded media) written to the
    run's texture cache."""
    if img is None: return None
    p = bpy.path.abspath(img.filepath) if img.filepath else ""
    if p and os.path.isfile(p): return p
    if TEX_CACHE and (img.packed_file is not None or img.has_data or img.size[0]):
        os.makedirs(TEX_CACHE, exist_ok=True)
        base = re.sub(r"[^A-Za-z0-9_.-]+", "_", os.path.splitext(img.name)[0]) or "image"
        if img.packed_file is not None:
            data = bytes(img.packed_file.data)
            ext = ".png" if data[:8] == b"\x89PNG\r\n\x1a\n" else ".jpg" if data[:2] == b"\xff\xd8" else ".bin"
            out = os.path.join(TEX_CACHE, base + ext)
            if ext != ".bin":
                with open(out, "wb") as f: f.write(data)
                return out
        out = os.path.join(TEX_CACHE, base + ".png")
        try:
            img.filepath_raw = out; img.file_format = "PNG"; img.save()
            return out
        except RuntimeError:
            return None
    return p or None                   # a missing file: the caller looks for it by name next to the model


def socket_image(sock, seen=None):
    """Follow links back from a shader socket to an image texture node (the Image)."""
    if not sock.is_linked: return None
    n = sock.links[0].from_node
    if n.type == "TEX_IMAGE":
        return n.image
    for inp in n.inputs:
        if inp.type in ("RGBA", "VECTOR", "VALUE") and inp.is_linked:
            r = socket_image(inp)
            if r: return r
    return None


def classify_files(files):
    out = {}
    for f in sorted(files):
        k = role_from_name(f)
        if k and k != "skip" and k not in out: out[k] = f
    return out


_IMAGES = {}


TEX_DIR_NAMES = ("textures", "texture", "tex", "maps")


def source_tex_dirs(src):
    """Where a model's textures may be: its own folder, plus a textures/ folder next to that folder (download layouts
    such as Sketchfab's source/model.fbx + textures/*.png). Only such named folders of the parent, never the parent."""
    d = os.path.dirname(os.path.abspath(src))
    out = [d]
    par = os.path.dirname(d)
    if par != d:
        for f in sorted(os.listdir(par)):
            q = os.path.join(par, f)
            if f.lower() in TEX_DIR_NAMES and os.path.isdir(q) and q != d: out.append(q)
    return out


def images_in(dirs):
    for d in dirs:
        if d not in _IMAGES:
            _IMAGES[d] = sorted(os.path.join(root, f) for root, _, files in os.walk(d) for f in files
                                if f.lower().endswith(IMG_EXT))
        yield from _IMAGES[d]


# a base colour file's own name part: "head" of head.png / head_d.dds / Head_BaseColor.tga / all_color.png
_BC_SUFFIX_RX = re.compile(r"[_\-. ](albedo|base_?colou?r|diffuse|diff|colou?r|col|bc|d|alb|c)$", re.I)
_HAIR_NAME_RX = re.compile(r"hair|fur\b|ponytail|bangs?\b|fringe|braid|beard|mustache|moustache|wig|lash|brow(?!n)", re.I)


def companion_maps(bc_path, dirs, name=""):
    """Maps that belong to a base colour file by name: head.png -> head_n.dds (normal), head_ao.dds, head_rough.png
    ... (game rips and many exports keep one set per texture: <name>_n/_normal/_ao/_orm/_mask/_alpha next to it; the
    material only links the colour, or has generic names like 'Material #25'). {role: file}"""
    stem = os.path.splitext(os.path.basename(bc_path))[0]
    root = _BC_SUFFIX_RX.sub("", stem) or stem
    own = os.path.dirname(os.path.abspath(bc_path))
    out = {}
    # the colour's own folder first: downloads with colour variants (Textures_Skin1/, Textures_Skin2/) repeat the same
    # file names in each folder; then PNG/TGA before a DDS of the same name
    for f in sorted(set(images_in(list(dirs) + [os.path.dirname(bc_path)])),
                    key=lambda f: (os.path.dirname(os.path.abspath(f)) != own, f.lower().endswith(".dds"), f)):
        s = os.path.splitext(os.path.basename(f))[0]
        if s.lower() == stem.lower() or not s.lower().startswith(root.lower()): continue
        rest = s[len(root):]
        if not rest or rest[0] not in "_-. ": continue                 # hair_n, not hairband
        k = role_from_name("x" + rest + os.path.splitext(f)[1])        # what the suffix says, not the name part
        if k and k not in ("skip", "basecolor"): out.setdefault(k, f)
    return out


# a non-colour map's channel part: head_Normal_OpenGL, head_Mixed_AO, head_Roughness, head_n (Substance Painter
# exports: <set>_BaseColor, _Normal_OpenGL/_DirectX, _Roughness, _Metallic, _AO / _Mixed_AO)
_MAP_SUFFIX_RX = re.compile(r"[_\-. ](mixed[_\-. ])?(normal|nrm|nor|n|roughness|rough|metallic|metalness|metal|ao|"
                            r"ambient_?occlusion|occlusion|orm|rmao|gloss(iness)?|smoothness)([_\-. ](opengl|directx|"
                            r"ogl|gl|dx))?$", re.I)


def colour_beside(map_path):
    """The base colour file of the set a linked non-colour map belongs to, by name, in that map's own folder:
    X_Normal_OpenGL.png -> X_BaseColor.png / X_Base_Color.png (else None)."""
    stem = os.path.splitext(os.path.basename(map_path))[0]
    root = _MAP_SUFFIX_RX.sub("", stem)
    d = os.path.dirname(os.path.abspath(map_path))
    if root == stem or not os.path.isdir(d): return None
    for f in sorted(os.listdir(d), key=lambda f: (f.lower().endswith(".dds"), f)):
        s, ext = os.path.splitext(f)
        if ext.lower() not in IMG_EXT or not s.lower().startswith(root.lower()): continue
        rest = s[len(root):]
        if rest and rest[0] in "_-. " and role_from_name("x" + rest + ext) == "basecolor" and \
                not _MAP_SUFFIX_RX.search(s):
            return os.path.join(d, f)
    return None


def mask_like_alpha(path):
    """A file's alpha channel looks like a cut-out mask (hair strands): plenty of clear and of solid texels."""
    st = alpha_stats(path)
    return bool(st) and st[0] > 0.15 and st[0] < 0.9 and st[1] < 0.5


def material_name_keys(base):
    """Name parts to look for in texture file names, most specific first: the material name, then without an export
    hash / UE prefixes (MI_, M_, F_MED_ ...), then with trailing words dropped down to two words."""
    k = base.replace(" ", "_")
    keys = [k] if k and k != NO_MATERIAL else []
    s = re.sub(r"_[0-9a-f]{6,8}$", "", k)
    t = re.sub(r"^((mi|mat|m)_)?((f|m)_)?(med|sml|lrg|tal|xl)_", "", s)     # M_MED_, M_F_MED_, F_MED_
    s = t if t != s else re.sub(r"^(mi|mat|m)_", "", s)
    words = s.split("_")
    while len(words) >= 2:
        w = "_".join(words)
        if w not in keys: keys.append(w)
        words = words[:-1]
    return keys


_borrowing = set()


def files_named(key_name, tex_dirs):
    """{role: file} of the texture files named <...>key_name<_role> (the closest names first)."""
    out, cands = {}, []
    for f in images_in(tex_dirs):
        fl = os.path.basename(f).lower().replace(" ", "_")
        i = fl.find(key_name)
        if i >= 0: cands.append((len(fl) - len(key_name), f))
    for _, f in sorted(cands):
        k = role_from_name(f)
        if k and k != "skip": out.setdefault(k, f)
    return out


# UE game rips (Fortnite): parts whose material links no texture of its own but uses another set's, by the last word of
# the name: the eyes are painted in a corner of the head's texture, hair uses the "face accessory" set
BORROWED_SET = ((re.compile(r"eyes?|eyeballs?"), "head", "the eyes use the head material's"),
                (re.compile(r"hair"), "faceacc", "hair uses the face-accessory textures"))


def borrowed_set(mat):
    """(material, why) whose textures a material of the same set without its own uses: X_Eyes_<hash> -> X_Head_<hash2>,
    X_Hair -> textures T_X_FaceAcc_* (a material or just the files: then the name part to look for)."""
    keys = material_name_keys(re.sub(r"\.\d{3}$", "", mat.name).lower())
    for rx, other, why in BORROWED_SET:
        want = set()
        for k in keys:
            w = k.split("_")
            if len(w) >= 2 and rx.fullmatch(w[-1]): want.add("_".join(w[:-1] + [other]))
        if not want: continue
        for m in bpy.data.materials:
            if m is mat or not m.users: continue
            if want & set(material_name_keys(re.sub(r"\.\d{3}$", "", m.name).lower())): return m, why
        return sorted(want, key=len)[-1], why        # no such material: its texture files by name
    return None, None


def material_textures(mat, tex_dirs, user=None, n_materials=1):
    """Texture files of a material: --tex MAT=<prefix|dir> first, then the shader's linked images (embedded ones
    written out), then files named after the material in the model's folder / --textures; a model with one material
    and one texture set in its folder takes that set. Names that say what a file is win over the socket it hangs on
    (a mask map linked as base colour)."""
    base = re.sub(r"\.\d{3}$", "", mat.name).lower() if mat else NO_MATERIAL
    for k, v in (user or {}).items():
        if k.lower() == base:
            if not (os.path.isdir(v) or os.path.isdir(os.path.dirname(v) or ".")):
                raise SystemExit(f"--tex {k}={v}: no such folder or file prefix")
            files = ([os.path.join(v, f) for f in os.listdir(v)] if os.path.isdir(v) else
                     [os.path.join(os.path.dirname(v), f) for f in os.listdir(os.path.dirname(v) or ".")
                      if f.startswith(os.path.basename(v))])
            imgs = [f for f in files if f.lower().endswith(IMG_EXT)]
            got = classify_files(imgs)
            plain = sorted((f for f in imgs if role_from_name(f) is None),
                           key=lambda f: (len(os.path.basename(f)), f.lower().endswith(".dds"), f))
            if "basecolor" not in got and plain:
                got["basecolor"] = plain[0]            # --tex x=textures/head: head.png (a name that says nothing)
            if got: return with_companions(got, base, tex_dirs, mat)
            log(f"  --tex {k}={v}: no texture file recognised there (names with albedo/basecolor/diffuse, normal, "
                f"roughness, metallic, ao ...)")
    out = {}
    if mat and mat.node_tree:
        bsdf = next((n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
        if bsdf:
            for key, sock in (("basecolor", "Base Color"), ("normal", "Normal"), ("roughness", "Roughness"),
                              ("metallic", "Metallic"), ("alpha", "Alpha")):
                img = socket_image(bsdf.inputs[sock])
                if img is not None:
                    out[key] = (image_file(img), img.name)
        if not out:
            # no Principled BSDF links (unlit/toon materials, e.g. VRM MToon): the shader's image nodes, by colour space
            for n in mat.node_tree.nodes:
                if n.type != "TEX_IMAGE" or n.image is None: continue
                key = role_from_name(n.image.name) or ("normal" if n.image.colorspace_settings.name == "Non-Color"
                                                       else "basecolor")
                if key != "skip" and key not in out: out[key] = (image_file(n.image), n.image.name)
    res = {}
    for k, (p, name) in out.items():
        # files that don't exist (FBX with absolute paths from the author's machine): look next to the model / --textures
        if not p or not os.path.isfile(p):
            p = find_file(os.path.basename(p or name), tex_dirs)
            if not p:
                log(f"  material {base}: its {k} image {name!r} is not next to the model (copy it there or use --tex)")
                continue
        role = role_from_name(p)
        if role and role != "skip" and role != k and k != "alpha" and role != "basecolor" \
                and not (k == "basecolor" and role == "alpha"):
            log(f"  material {base}: {os.path.basename(p)} is linked as {k} but its name says {role}: used as {role}")
            k = role
        elif role == "skip" and k != "alpha":
            log(f"  material {base}: {os.path.basename(p)} ({k}) looks like a map the game doesn't use: ignored")
            continue
        if k == "basecolor" and role is None and "normal" not in res and looks_like_normal_map(p):
            log(f"  material {base}: {os.path.basename(p)} is linked as the colour but its texels are a normal map's "
                f"(blue ~1): used as the normal map")
            k = "normal"
        res.setdefault(k, p)
    out = res
    if "basecolor" not in out and role_from_name(out.get("alpha", "")) == "basecolor":
        # the colour image linked only through its alpha (FBX exports: the colour socket left empty)
        log(f"  material {base}: {os.path.basename(out['alpha'])} is linked only as the opacity; its name says base "
            f"colour: used as the colour too")
        out["basecolor"] = out["alpha"]
    if "basecolor" not in out:
        # only a normal (or roughness ...) map linked: Blender's FBX export keeps just the images wired straight into
        # the shader, so a colour that goes through a Mix node (x AO) is lost while the normal map stays; the set's
        # colour is next to it by name (X_Normal_OpenGL.png -> X_BaseColor.png)
        for k in ("normal", "roughness", "metallic", "ao", "orm", "gloss"):
            bc = colour_beside(out[k]) if k in out else None
            if bc:
                log(f"  material {base}: no colour image linked, only {os.path.basename(out[k])} ({k}); the colour "
                    f"of its set next to it: {os.path.basename(bc)}")
                out["basecolor"] = bc
                break
    if "basecolor" not in out and mat is not None:
        # nothing linked: guess by material name; files named exactly after it first (<mat>_BaseColor before
        # <mat>Inner_BaseColor, another material's set). Then the name as UE rips write it: material MI_X_Body_1cbb1ecc
        # (M_/MI_ prefix, F_MED_ size tag, export hash) for textures T_X_Body_D; last, trailing words dropped
        # (X_Head_WM -> X_Head), never below two words.
        for key_name in material_name_keys(base):
            cands = []
            for f in images_in(tex_dirs):
                fl = os.path.basename(f).lower().replace(" ", "_")
                i = fl.find(key_name)
                if i < 0: continue
                rest = os.path.splitext(fl)[0][i + len(key_name):]
                # X_D before X_Body_D (only a map tag left after the name), then a word boundary, then anything
                cands.append((0 if re.fullmatch(r"[_\-. ]+[a-z0-9]+(\.[a-z]+)?", rest) or not rest else
                              1 if not rest[:1].isalnum() else 2, f))
            if not cands and key_name != base.replace(" ", "_"):
                # the name part's words in order with others between (Cosmos_Body: T_M_MED_Cosmos_Heavy_Body_D),
                # whole words or their ends (Armor: BodyArmor; Body is not BodyArmor); fewest words between first
                kw = key_name.split("_")
                for f in images_in(tex_dirs):
                    fw = re.split(r"[_\-. ]+", os.path.basename(f).lower())
                    j, gap, start = 0, 0, None
                    for x, w in enumerate(fw):
                        if j < len(kw) and (w == kw[j] or (len(kw[j]) >= 4 and w.endswith(kw[j]))):  # Armor: BodyArmor
                            if start is None: start = x
                            j += 1
                        elif start is not None and j < len(kw): gap += 1
                    if j == len(kw): cands.append((2 + gap, f))
            best = min((c for c, f in cands), default=None)
            for c, f in cands:
                if c != best: continue
                k = role_from_name(f)
                if k and k != "skip": out.setdefault(k, f)
            if "basecolor" in out:
                if key_name != base.replace(" ", "_"):
                    log(f"  material {base}: textures found by the name part {key_name!r}: "
                        f"{', '.join(os.path.basename(v) for v in out.values())}")
                break
    if "basecolor" not in out and mat is not None and not _borrowing:
        other, why = borrowed_set(mat)
        got = {}
        if isinstance(other, str):                   # only texture files: T_X_FaceAcc_D ...
            got = files_named(other, tex_dirs)
            label = other
        elif other is not None:
            _borrowing.add(mat.name)
            try:
                got = material_textures(other, tex_dirs, user, n_materials)
            finally:
                _borrowing.discard(mat.name)
            label = re.sub(r"\.\d{3}$", "", other.name)
        if "basecolor" in got:
            log(f"  material {base}: no textures of its own; {why} ({label}): {os.path.basename(got['basecolor'])}")
            for k, v in got.items(): out.setdefault(k, v)
    if "basecolor" not in out and n_materials == 1:
        got = classify_files(list(images_in(tex_dirs)))
        if "basecolor" in got:
            log(f"  material {base}: using the texture set in the model's folder: "
                f"{', '.join(os.path.basename(v) for v in got.values())}")
            for k, v in got.items(): out.setdefault(k, v)
    return with_companions(out, base, tex_dirs, mat)


def with_companions(out, base, tex_dirs, mat):
    """Add the maps named after the base colour file (companion_maps) that the material doesn't link, and for hair
    whose colour has no alpha the cut-out mask some games keep in the normal map's alpha."""
    bc = out.get("basecolor")
    if not bc or not os.path.isfile(bc): return out
    add = {k: v for k, v in companion_maps(bc, tex_dirs).items() if k not in out}
    if add:
        log(f"  material {base}: maps named after {os.path.basename(bc)}: "
            + ", ".join(f"{os.path.basename(v)} ({k})" for k, v in add.items()))
        out.update(add)
    if "alpha" in out and out["alpha"] != bc: return out
    names = base + " " + os.path.basename(bc)
    if _HAIR_NAME_RX.search(names) and not mask_like_alpha(bc):
        n = out.get("normal")
        if n and os.path.isfile(n) and mask_like_alpha(n):
            log(f"  material {base}: {os.path.basename(bc)} has no cut-out alpha; the strands' mask is the alpha of "
                f"{os.path.basename(n)} (hair in game rips): used as the opacity")
            out["alpha"] = n
    return out


def basecolor_factor(mat):
    """The colour a material multiplies its base colour texture by (linear RGB), else None: glTF baseColorFactor, VRM
    MToon _Color (VRoid hair and brows: a grey texture x the hair colour), a Multiply node in a hand-made material.
    Blender's glTF importer builds it as a Mix node (Multiply, RGBA) between the image and the shader."""
    if not (mat and mat.node_tree): return None
    for n in mat.node_tree.nodes:
        if n.type not in ("MIX", "MIX_RGB") or n.blend_type != "MULTIPLY": continue
        if n.type == "MIX" and getattr(n, "data_type", "RGBA") != "RGBA": continue
        cols = [i for i in n.inputs if i.enabled and i.type == "RGBA"]
        fac = next((i for i in n.inputs if i.enabled and i.type == "VALUE"), None)
        if len(cols) != 2 or (fac is not None and fac.is_linked): continue
        img = [c for c in cols if c.is_linked and c.links[0].from_node.type == "TEX_IMAGE"]
        const = [c for c in cols if not c.is_linked]
        if len(img) != 1 or len(const) != 1: continue
        f = fac.default_value if fac is not None else 1.0
        rgb = [1.0 + f * (x - 1.0) for x in list(const[0].default_value)[:3]]
        if max(abs(x - 1.0) for x in rgb) < 1e-3: return None
        return rgb
    return None


def bsdf_values(mat):
    """Constant material values (used where the material has no texture for them)."""
    out = {}
    if mat and mat.node_tree:
        b = next((n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
        if b:
            out["basecolor"] = list(b.inputs["Base Color"].default_value)[:3]
            out["roughness"] = b.inputs["Roughness"].default_value
            out["metallic"] = b.inputs["Metallic"].default_value
        f = basecolor_factor(mat)
        if f: out["basecolor_factor"] = f
    elif mat is not None:
        out["basecolor"] = list(mat.diffuse_color)[:3]
        out["roughness"] = mat.roughness
        out["metallic"] = mat.metallic
    return out


def find_file(name, dirs):
    for d in dirs:
        for root, _, files in os.walk(d):
            if name in files: return os.path.join(root, name)
            low = {f.lower(): f for f in files}
            if name.lower() in low: return os.path.join(root, low[name.lower()])
    return None


REPEAT_MAX = 4          # an atlas tile holds at most 4x4 repeats of a tiling texture (more: squeezed)


def uv_islands(me, uv, faces):
    """Faces grouped into UV islands (connected through shared vertices with the same UV)."""
    parent = list(range(len(faces)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]; i = parent[i]
        return i
    first = {}
    loops = me.loops
    for fi, p in enumerate(faces):
        for li in p.loop_indices:
            u, v = uv.data[li].uv
            j = first.setdefault((loops[li].vertex_index, round(u, 4), round(v, 4)), fi)
            if j != fi:
                a, b = find(fi), find(j)
                if a != b: parent[a] = b
    out = {}
    for fi in range(len(faces)): out.setdefault(find(fi), []).append(fi)
    return list(out.values())


def uv_prepare(me, uv, faces):
    """Move every UV island by whole tiles so its centre lies in the 0..1 tile (identical look with a repeating
    texture) and measure what still lies outside: {"faces", "outside" (faces), "repeat" [nx, ny] a tile would need to
    hold the islands as repeats, "islands", "moved"}."""
    isl = uv_islands(me, uv, faces)
    moved = 0
    for fis in isl:
        lis = [li for fi in fis for li in faces[fi].loop_indices]
        us = [uv.data[li].uv[0] for li in lis]; vs = [uv.data[li].uv[1] for li in lis]
        du, dv = math.floor((min(us) + max(us)) / 2), math.floor((min(vs) + max(vs)) / 2)
        if du or dv:
            moved += len(fis)
            for li in lis:
                u, v = uv.data[li].uv
                uv.data[li].uv = (u - du, v - dv)
    outside, nx, ny = 0, 1, 1
    for fis in isl:
        lis = [li for fi in fis for li in faces[fi].loop_indices]
        us = [uv.data[li].uv[0] for li in lis]; vs = [uv.data[li].uv[1] for li in lis]
        # as repeats: the island moved so it starts in the first tile
        nx = max(nx, math.ceil(max(us) - math.floor(min(us) + 1e-3) - 1e-3))
        ny = max(ny, math.ceil(max(vs) - math.floor(min(vs) + 1e-3) - 1e-3))
        for fi in fis:
            fu = [uv.data[li].uv for li in faces[fi].loop_indices]
            if min(x[0] for x in fu) < -0.01 or max(x[0] for x in fu) > 1.01 or \
                    min(x[1] for x in fu) < -0.01 or max(x[1] for x in fu) > 1.01:
                outside += 1
    return {"faces": faces, "islands": isl, "outside": outside, "repeat": [min(nx, REPEAT_MAX), min(ny, REPEAT_MAX)],
            "moved": moved}


def uv_to_tile(me, uv, w, rect, rep, what):
    """UVs (after uv_prepare) into an atlas tile. rep [nx, ny] > 1: the texture is drawn nx x ny times in the tile, so
    islands start in the first repeat and are scaled by 1/n; else faces still outside are moved by whole tiles one by
    one, and what spans more than one tile is squeezed onto the tile's edge (logged)."""
    faces = w["faces"]
    nx, ny = rep
    squeezed = 0
    if (nx, ny) != (1, 1):
        for fis in w["islands"]:
            lis = [li for fi in fis for li in faces[fi].loop_indices]
            du = math.floor(min(uv.data[li].uv[0] for li in lis) + 1e-3)
            dv = math.floor(min(uv.data[li].uv[1] for li in lis) + 1e-3)
            for li in lis:
                u, v = uv.data[li].uv
                uv.data[li].uv = ((u - du) / nx, (v - dv) / ny)
    else:
        for p in faces:
            fu = [uv.data[li].uv for li in p.loop_indices]
            lo_u, hi_u = min(x[0] for x in fu), max(x[0] for x in fu)
            lo_v, hi_v = min(x[1] for x in fu), max(x[1] for x in fu)
            if lo_u >= -0.01 and hi_u <= 1.01 and lo_v >= -0.01 and hi_v <= 1.01: continue
            du, dv = math.floor((lo_u + hi_u) / 2), math.floor((lo_v + hi_v) / 2)
            for li in p.loop_indices:
                u, v = uv.data[li].uv
                uv.data[li].uv = (u - du, v - dv)
            if hi_u - lo_u > 1.02 or hi_v - lo_v > 1.02 or hi_u - du > 1.01 or lo_u - du < -0.01 or \
                    hi_v - dv > 1.01 or lo_v - dv < -0.01:
                squeezed += 1
    x0, y0, tw, th = rect
    for p in faces:
        for li in p.loop_indices:
            u, v = uv.data[li].uv
            u = min(max(u, 0.0), 1.0); v = min(max(v, 0.0), 1.0)
            # Blender UV origin is bottom-left, atlas rects are image (top-left) based
            uv.data[li].uv = (x0 + u * tw, 1.0 - (y0 + (1.0 - v) * th))
    if w["moved"]:
        log(f"  {what}: {w['moved']} faces' UVs moved by whole texture repeats into 0..1 (same look)")
    if squeezed:
        log(f"  {what}: {squeezed} faces span more than one texture repeat: squeezed into the tile (a small smear)")


def assign_slots(o, tpl, meshes, tex_dirs):
    """Rename materials to template slots and lay out texture sets.

    A texture set is the group of textures a slot's material instance samples (BC/N/PBR...). By default each slot is
    its own set. --atlas SET=mat1,mat2,... packs those source materials into one set as a grid of tiles (UVs remapped
    into each material's tile); --slotset SLOT=SET makes a slot sample another slot's set (retail FP arm skin samples
    the outfit's torso textures). The same --atlas list in the 3P and the FP run gives both the same layout.
    Returns the manifest part: {"slots": {slot: set}, "sets": {set: {"grid": g, "tiles": [...]}}}."""
    user = {}
    for x in o.get("slot", []):
        a, b = x.split("=", 1)
        user[a.lower()] = b
    atlases = {}
    for x in o.get("atlas", []):
        a, b = x.split("=", 1)
        atlases[a] = [m.strip().lower() for m in b.split(",") if m.strip()]
    slotset = {}
    for x in o.get("slotset", []):
        a, b = x.split("=", 1)
        slotset[a.lower()] = b
    tex_user = dict(x.split("=", 1) for x in o.get("tex", []))
    slots_lower = {s.lower(): s for s in tpl.slots}
    srcmats = []
    for m in meshes:
        for mat in used_materials(m):
            if mat not in srcmats: srcmats.append(mat)

    def base(mat):
        return re.sub(r"\.\d{3}$", "", mat.name) if mat else NO_MATERIAL

    slot_of = {}
    for mat in srcmats:
        bn = base(mat)
        slot = user.get(bn.lower()) or user.get((mat.name if mat else "none").lower()) or slots_lower.get(bn.lower())
        if slot is None:
            for k, v in user.items():
                if k.endswith("*") and bn.lower().startswith(k[:-1].lower()): slot = v
        if slot is None:
            slot = tpl.slots[0]
            log(f"material {bn!r}: no --slot given, using the template's first slot {slot!r}")
        if slot.lower() not in slots_lower:
            raise SystemExit(f"--slot {bn}={slot}: the template has no slot {slot!r} (slots: {tpl.slots})")
        slot_of[mat] = slots_lower[slot.lower()]
    # --tiles FILE (b4bmodel): {"alias": {set: {material: material whose tile it shares}}, "repeat": {set: {material:
    # [nx, ny]}}}: materials drawing the same textures share one atlas tile; the FP run takes the 3P run's repeats
    tiles_cfg = json.load(open(o["tiles"])) if o.get("tiles") else {}
    alias = {st.lower(): {k.lower(): v for k, v in d.items()} for st, d in tiles_cfg.get("alias", {}).items()}
    # "layout": {set: {"canvas": [W, H], "rects": {material: [x, y, w, h]}}}: b4bmodel's atlas packing (tiles sized
    # after their images); sets without one use a grid of equal tiles
    layout = {st: {"canvas": v["canvas"], "rects": {k.lower(): r for k, r in v["rects"].items()}}
              for st, v in tiles_cfg.get("layout", {}).items()}

    def grid_rect(st, names, cn):
        if st in layout and cn in layout[st]["rects"]: return tuple(layout[st]["rects"][cn])
        g = math.ceil(math.sqrt(len(names)))
        i = names.index(cn)
        return ((i % g) / g, (i // g) / g, 1.0 / g, 1.0 / g)
    forced = {st.lower(): {k.lower(): v for k, v in d.items()} for st, d in tiles_cfg.get("repeat", {}).items()}
    sets = {}
    slots_used = {}
    plan = []                                   # (mat, slot, set, tile material, rect, grid)
    for mat in srcmats:
        slot = slot_of[mat]
        st = slotset.get(slot.lower(), slot)
        slots_used[slot] = st
        canon = alias.get(st.lower(), {}).get(base(mat).lower(), base(mat))
        cn = canon.lower()
        if st in atlases:
            names = atlases[st]
            if cn not in names:
                raise SystemExit(f"material {base(mat)!r} goes to slot {slot} (texture set {st}), but --atlas {st}= "
                                 f"doesn't list it (--atlas {st}={','.join(names)})")
            g = math.ceil(math.sqrt(len(names)))
            rect = grid_rect(st, names, cn)
        else:
            g, rect = 1, (0.0, 0.0, 1.0, 1.0)
            if st in sets and sets[st]["tiles"][0]["material"].lower() != cn:
                raise SystemExit(f"materials {sets[st]['tiles'][0]['material']!r} and {base(mat)!r} both use texture "
                                 f"set {st}: list them in --atlas {st}=...")
        tiles = sets.setdefault(st, {"grid": g, "tiles": []})["tiles"]
        if st in layout: sets[st]["canvas"] = layout[st]["canvas"]
        if not any(t["material"].lower() == cn for t in tiles):
            if cn == base(mat).lower():
                tx = material_textures(mat, tex_dirs, tex_user, len(srcmats))
            else:                               # the tile of another material with the same textures
                src = next((x for x in srcmats if base(x).lower() == cn), mat)
                tx = material_textures(src, tex_dirs, tex_user, len(srcmats))
            tiles.append({"material": canon, "rect": rect, "textures": tx, "values": bsdf_values(mat)})
        plan.append((mat, slot, st, cn, rect, g))
    # UVs: whole islands moved by whole tiles into 0..1 first (the same look with a repeating texture: game rips often
    # sit at v -1..0); what still reaches past the tile in an atlas is drawn as repeats of the texture in the tile, or
    # (a few faces only) squeezed onto the tile's edge
    groups = {}                                 # (mesh, material index) -> UV island work
    for mat, slot, st, cn, rect, g in plan:
        for m in meshes:
            uv = m.data.uv_layers.active
            if uv is None: continue
            idxs = [i for i, mm in enumerate(m.data.materials) if mm == mat] or ([0] if mat is None and not m.data.materials else [])
            for i in idxs:
                faces = [p for p in m.data.polygons if p.material_index == i or
                         (mat is None and p.material_index >= len(m.data.materials))]
                if faces: groups[(m.name, i)] = uv_prepare(m.data, uv, faces)
    need = {}
    for mat, slot, st, cn, rect, g in plan:
        if g == 1: continue
        for m in meshes:
            for i, mm in enumerate(list(m.data.materials) or [None]):
                w = groups.get((m.name, i))
                if w and mm == mat and w["outside"] > 0.02 * len(w["faces"]):
                    r = need.setdefault((st, cn), [1, 1])
                    r[0] = max(r[0], w["repeat"][0]); r[1] = max(r[1], w["repeat"][1])
    for mat, slot, st, cn, rect, g in plan:        # the 3P run's repeats win (FP arms: the same texture set)
        r = forced.get(st.lower(), {}).get(cn)
        if g > 1 and r: need[(st, cn)] = list(r)
    for st, info in sets.items():
        for t in info["tiles"]:
            r = need.get((st, t["material"].lower()))
            if r and tuple(r) != (1, 1): t["repeat"] = list(r)
    for mat, slot, st, cn, rect, g in plan:
        slotmat = bpy.data.materials.get(slot) or bpy.data.materials.new(slot)
        rep = need.get((st, cn), [1, 1])
        for m in meshes:
            uv = m.data.uv_layers.active
            if mat is None and not m.data.materials:
                m.data.materials.append(None)   # an object without material slots: give it one
            for i, mm in enumerate(m.data.materials):
                if mm != mat: continue
                w = groups.get((m.name, i))
                if g > 1 and uv is not None and w:
                    uv_to_tile(m.data, uv, w, rect, rep, f"{base(mat)} ({m.name})")
                m.data.materials[i] = slotmat
        log(f"material {base(mat)} -> slot {slot}, texture set {st}" + (f" tile {rect}" if g > 1 else "") +
            (f" (tile of {cn})" if cn != base(mat).lower() else "") +
            (f", texture repeated {rep[0]}x{rep[1]} in the tile" if tuple(rep) != (1, 1) else ""))
    # atlases listed but with materials not present in this model (e.g. FP arms): still record the full layout so
    # the texture step composes the same image in both runs
    for st, names in atlases.items():
        if st in sets:
            have = {t["material"].lower() for t in sets[st]["tiles"]}
            for n in names:
                if n not in have:
                    sets[st]["tiles"].append({"material": n, "rect": grid_rect(st, names, n), "textures": {},
                                              "absent": True})
    return {"slots": slots_used, "sets": sets}


LOD_MIN_TRIS = 1500      # never decimate below this (a low-poly model's LODs stay whole: boxes collapse otherwise)
WELD_TOL = 1e-5          # m: corners closer than this are one point for the LOD weld (0.01 mm)
WELD_FLAT_DEG = 1.0      # a face whose corner normals are all within this of its face normal was flat shaded
WELD_SHARP_DEG = 5.0     # two faces whose normals differed by more than this at a shared point: sharp edge there


def weld_for_decimation(c):
    """Weld a LOD copy by position before Decimate. Models reach the fit split wherever a vertex attribute changes:
    every corner on its own in a flat-shaded model (a triangle soup), along UV seams and hard edges in glTF/VRM
    imports (Blender's glTF importer does not merge). Decimate can't collapse an edge whose sides aren't connected,
    so each piece shrank alone and the LODs came out shredded. Welded, the surface is one piece where the source is
    closed; the corners keep their own UVs (loop data: the export splits the vertices again at UV and material
    seams), weights must match (a magazine touching the receiver in one object stays loose), and faces that exist
    twice with opposite winding (double-sided rips) weld as two layers (a plain weld would delete one side). The
    source's normal splits come back as shading flags: faces that were flat stay flat, hard edges become sharp edges;
    custom normals are cleared (Blender recomputes smooth normals on the decimated surface). Returns the number of
    vertices merged."""
    me = c.data
    if not me.polygons:
        return 0
    cn = [x.vector.copy() for x in me.corner_normals]
    bm = bmesh.new()
    bm.from_mesh(me)
    bm.faces.ensure_lookup_table()
    oi = bm.loops.layers.int.new("b4b_oi")
    for f in bm.faces:
        ls = me.polygons[f.index].loop_start
        for k, lp in enumerate(f.loops):
            lp[oi] = ls + k
    dl = bm.verts.layers.deform.active
    q = 1.0 / WELD_TOL

    def pk(v):
        return (round(v.co.x * q), round(v.co.y * q), round(v.co.z * q))

    def wk(v):
        if dl is None: return ()
        return tuple(sorted((g, round(w * 20)) for g, w in v[dl].items() if w > 0.03))

    # double-sided faces: the second face over the same three points goes to the back layer
    seen, back = set(), set()
    for f in bm.faces:
        k = frozenset(pk(v) for v in f.verts)
        if k in seen: back.add(f.index)
        else: seen.add(k)
    layer = {}                                                 # vertex -> 0 front / 1 back / 2 both (keep apart)
    for f in bm.faces:
        s = 1 if f.index in back else 0
        for v in f.verts:
            layer[v] = s if layer.get(v, s) == s else 2
    target, first = {}, {}
    for v in bm.verts:
        ly = layer.get(v, 0)
        if ly == 2: continue
        k = (pk(v), wk(v), ly)
        t = first.setdefault(k, v)
        if t is not v: target[v] = t
    flat = {f.index for f in bm.faces
            if all(cn[lp[oi]].angle(f.normal, 0.0) < math.radians(WELD_FLAT_DEG) for lp in f.loops)}
    if target:
        bmesh.ops.weld_verts(bm, targetmap=target)
    cos_sharp = math.cos(math.radians(WELD_SHARP_DEG))
    for f in bm.faces:
        f.smooth = f.index not in flat if len(flat) < len(bm.faces) else False
    for e in bm.edges:
        if len(e.link_faces) != 2: continue
        f1, f2 = e.link_faces
        sharp = False
        for v in e.verts:
            l1 = next((lp for lp in f1.loops if lp.vert is v), None)
            l2 = next((lp for lp in f2.loops if lp.vert is v), None)
            if l1 and l2 and cn[l1[oi]].dot(cn[l2[oi]]) < cos_sharp:
                sharp = True
        e.smooth = not sharp
    bm.loops.layers.int.remove(oi)
    bm.to_mesh(me)
    bm.free()
    if getattr(me, "has_custom_normals", False):
        cnl = me.attributes.get("custom_normal")
        if cnl is not None: me.attributes.remove(cnl)
        else:
            select_only([c], c)
            bpy.ops.mesh.customdata_custom_splitnormals_clear()
    me.update()
    return len(target)


LOD0_KEEP_GROUP = "b4b_keep"   # vertex group of the face: decimated less (Decimate's vertex group, inverted)
LOD0_KEEP_WEIGHT = 0.5         # inverted to 0.5 (an inverted weight of 0 would never collapse: hair on the head stayed)
LOD0_KEEP_FACTOR = 0.002       # Decimate vertex_group_factor (a cost per metre of edge): at ratio 0.5 the face keeps
                               # ~0.65 of its triangles and the body ~0.4 (0.1+: the face untouched, the body at 0.2)


def render_counts(meshes):
    """(vertices as the game counts them: split where the UV changes, triangles) of meshes."""
    nv = nt = 0
    for m in meshes:
        me = m.data
        uv = me.uv_layers.active.data if me.uv_layers.active else None
        seen = set()
        for p in me.polygons:
            nt += len(p.vertices) - 2
            for li in p.loop_indices:
                vi = me.loops[li].vertex_index
                seen.add((vi, (round(uv[li].uv[0], 5), round(uv[li].uv[1], 5)) if uv else None))
        nv += len(seen)
    return nv, nt


def lod0_ratio(o, tpl, meshes):
    """How much LOD0 keeps: the template's own LOD0 is the budget (its vertices and triangles; --max_verts N instead;
    --keep_density: no reduction). Cloth sections are left whole (their two-sided layers must stay together), the
    rest takes the reduction. Returns (ratio for the non-cloth meshes, log line) or (1.0, None)."""
    if o.get("keep_density") or not tpl or not tpl.meshes: return 1.0, None
    mv, mt = render_counts(meshes)
    tv, tt = render_counts(tpl.meshes)
    if o.get("max_verts"):
        bv, bt, what = int(o["max_verts"]), None, f"--max-verts {o['max_verts']}"
    else:
        bv, bt, what = tv, tt, f"the template's LOD0: {tv} vertices, {tt} triangles"
    r = min(1.0, bv / max(1, mv), (bt / max(1, mt)) if bt else 1.0)
    if r >= 0.98: return 1.0, None
    cloth = [m for m in meshes if m.name.startswith("B4BCLOTH_")]
    ct = render_counts(cloth)[1] if cloth else 0
    rest = max(1, mt - ct)
    rr = max(0.05, min(1.0, (r * mt - ct) / rest))
    return rr, (f"LOD0: {mv} vertices, {mt} triangles is over the budget ({what}): decimated to ~{r:.2f} "
                f"(face kept, cloth sections whole; --keep-density keeps every triangle, --max-verts N sets the budget)")


def keep_face_group(tpl, m):
    """Vertex group LOD0_KEEP_GROUP on the vertices skinned to the head and its face bones (decimated less)."""
    head = {"head"}
    for b in tpl.arm.data.bones:
        if any(p.name == "head" for p in b.parent_recursive): head.add(b.name)
    names = {g.index: g.name for g in m.vertex_groups}
    idx = [v.index for v in m.data.vertices
           if sum(g.weight for g in v.groups if names.get(g.group) in head) > 0.5 * (sum(g.weight for g in v.groups) or 1)]
    g = m.vertex_groups.get(LOD0_KEEP_GROUP) or m.vertex_groups.new(name=LOD0_KEEP_GROUP)
    if idx: g.add(idx, LOD0_KEEP_WEIGHT, "REPLACE")
    return g


def decimate(c, r, keep=False):
    """Collapse-decimate object c (welded) to ratio r before its armature modifier; keep: the face goes last and UV
    seams are kept as edges (no texture smearing across islands)."""
    if keep:
        select_only([c], c)
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        try:
            bpy.ops.uv.seams_from_islands(mark_seams=True, mark_sharp=False)
        except RuntimeError:
            pass
        bpy.ops.object.mode_set(mode="OBJECT")
    d = c.modifiers.new("Decimate", "DECIMATE"); d.ratio = r; d.use_collapse_triangulate = True
    if keep and c.vertex_groups.get(LOD0_KEEP_GROUP):
        # Decimate's vertex group: an edge costs more the lower its (inverted) weights; 0 would never collapse
        d.vertex_group = LOD0_KEEP_GROUP; d.invert_vertex_group = True; d.vertex_group_factor = LOD0_KEEP_FACTOR
    # keep the decimate before the armature modifier
    while c.modifiers[0].name != "Decimate":
        bpy.context.view_layer.objects.active = c
        bpy.ops.object.modifier_move_up(modifier="Decimate")
    select_only([c], c)
    bpy.ops.object.modifier_apply(modifier="Decimate")
    g = c.vertex_groups.get(LOD0_KEEP_GROUP)
    if g: c.vertex_groups.remove(g)


def make_lods(o, meshes, tpl=None):
    ratios = [float(x) for x in o.get("lods", "1").split(",")]
    lods, welded = [], {}
    r0, why = lod0_ratio(o, tpl, meshes)

    def welded_of(m):                                        # the same welded base for every LOD
        k = m.as_pointer()
        if k not in welded:
            w = m.copy(); w.data = m.data.copy(); w.name = f"{m.name}_welded"
            bpy.context.scene.collection.objects.link(w)
            n0 = len(w.data.vertices)
            merged = weld_for_decimation(w)
            welded[k] = w
            if merged: log(f"  weld {m.name}: {n0} -> {len(w.data.vertices)} vertices")
        return welded[k]
    if r0 < 1.0:
        # LOD0 over the budget (a 100k-vertex model): decimated like a distance LOD, the face kept, UV seams kept
        log(why)
        v0, t0 = render_counts(meshes)
        out, reduced = [], set()
        for m in meshes:
            if m.name.startswith("B4BCLOTH_") or len(m.data.polygons) * r0 < 50:
                out.append(m); continue
            base = welded_of(m)
            name = m.name
            m.name = name + "_full"
            c = m.copy(); c.data = base.data.copy(); c.name = name
            bpy.context.scene.collection.objects.link(c)
            keep_face_group(tpl, c)
            reduced.add(name)
            decimate(c, r0, keep=True)
            out.append(c)
        v1, t1 = render_counts(out)
        log(f"LOD0: {v0} -> {v1} vertices, {t0} -> {t1} triangles")
        src = dict(zip([m.name for m in out], meshes))
        meshes = out
    else:
        src, reduced = {m.name: m for m in meshes}, set()
    total = sum(len(m.data.polygons) for m in src.values())
    for li, r in enumerate(ratios):
        if li == 0:
            lods.append(meshes); continue
        r = min(1.0, max(r, LOD_MIN_TRIS / max(1, total)))
        copies = []
        for m in meshes:
            c = m.copy(); c.data = m.data.copy(); c.name = f"{m.name}_LOD{li}"
            bpy.context.scene.collection.objects.link(c)
            rm = r * r0 if m.name in reduced else r          # LOD ratios stay relative to LOD0
            if rm >= 0.999:
                copies.append(c); continue
            c.data = welded_of(src[m.name]).data.copy()
            decimate(c, rm)
            copies.append(c)
        lods.append(copies)
        log(f"LOD{li}: ratio {r}: {sum(len(c.data.polygons) for c in copies)} faces")
    for w in welded.values():
        bpy.data.objects.remove(w)
    return lods


def export_glb(path, arm, meshes):
    for x in bpy.context.view_layer.objects:
        x.hide_set(x not in [arm] + meshes)
    select_only([arm] + meshes, arm)
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_all_influences=True,
                              export_skins=True, export_animations=False, export_morph=False,
                              export_apply=False, export_yup=True)


def finish(o, tpl, meshes):
    global TEX_CACHE
    out = o["out"]
    os.makedirs(out, exist_ok=True)
    TEX_CACHE = os.path.join(out, "textures")
    tex_dirs = source_tex_dirs(o["source"]) + ([o["textures"]] if o.get("textures") else [])
    for m in meshes:                                           # triangulate + clean up before LODs and export
        select_only([m], m)
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.mesh.quads_convert_to_tris()
        bpy.ops.object.mode_set(mode="OBJECT")
    mats = assign_slots(o, tpl, meshes, tex_dirs)
    lods = make_lods(o, meshes, tpl)
    files = []
    for li, lm in enumerate(lods):
        p = os.path.join(out, f"lod{li}.glb")
        export_glb(p, tpl.arm, lm)
        files.append(p)
    man = {"kind": o["kind"], "template": o["template"], "source": o["source"], "lods": files,
           "slots": mats["slots"], "sets": mats["sets"], "extras": o.get("extras", {})}
    json.dump(man, open(os.path.join(out, "manifest.json"), "w"), indent=1)
    log("wrote", out, [os.path.basename(f) for f in files])


# ---- weapon ---------------------------------------------------------------------------------------------------------

PART_RULES = [(r"mag(azine)?|clip|drum", "mag"), (r"bolt|slide", "bolt"), (r"trigger", "trigger"),
              (r"charg(ing)?[_ ]?handle|cocking", "charging_handle"), (r"safety|selector|fire[_ ]?mode", "safety_selector"),
              (r"ejector|ejection|dust[_ ]?cover", "ejection_port_door"), (r"hammer", "hammer"),
              (r"stock", "stock"), (r"cylinder", "cylinder")]
MARKERS = ("muzzle", "shell_eject", "mag", "ironsights", "scope", "laser", "flashlight", "grip", "trigger")
AXES = {"+x": Vector((1, 0, 0)), "-x": Vector((-1, 0, 0)), "+y": Vector((0, 1, 0)), "-y": Vector((0, -1, 0)),
        "+z": Vector((0, 0, 1)), "-z": Vector((0, 0, -1))}


def fit_weapon(o):
    tpl = Template(o["template"])
    src_objs = [x for x in import_any(o["source"]) if x.name in bpy.data.objects]
    apply_transforms([x for x in src_objs if x.type in ("MESH", "EMPTY")])
    meshes = [x for x in src_objs if x.type == "MESH"]
    fix_inverted_normals(meshes)
    for x in src_objs:
        if x.type == "ARMATURE":
            log("ignoring the source armature: weapon parts are bound rigidly by object name")
    # template geometry: weapon bones and extent come from the template mesh
    tb = set(tpl.bones)
    main = next((b for b in ("gun", "weapon", "root") if b in tb and b != "root"), None) or "gun"
    if "gun" in tb: main = "gun"
    tverts = [tm.matrix_world @ v.co for tm in tpl.meshes for v in tm.data.vertices]
    tlo = Vector((min(p.x for p in tverts), min(p.y for p in tverts), min(p.z for p in tverts)))
    thi = Vector((max(p.x for p in tverts), max(p.y for p in tverts), max(p.z for p in tverts)))
    # orientation: source forward/up -> template +X / +Z (glTF import of a B4B export: UE X = Blender X, UE Z = Blender Z)
    fwd = AXES[o.get("forward", "+x")]; up = AXES[o.get("up", "+z")]
    side = up.cross(fwd)
    Rsrc = Matrix((fwd, side, up)).transposed()
    R = Rsrc.inverted().to_4x4()
    for x in src_objs:
        if x.parent is None: x.matrix_world = R @ x.matrix_world
    apply_transforms([x for x in src_objs if x.type in ("MESH", "EMPTY")])
    sverts = [m.matrix_world @ v.co for m in meshes for v in m.data.vertices]
    slo = Vector((min(p.x for p in sverts), min(p.y for p in sverts), min(p.z for p in sverts)))
    shi = Vector((max(p.x for p in sverts), max(p.y for p in sverts), max(p.z for p in sverts)))
    sc = o.get("scale", "fit")
    k = (thi.x - tlo.x) / max(1e-6, shi.x - slo.x) if sc == "fit" else float(sc)
    # anchor: source trigger -> template trigger bone (else bounding-box centre -> template bbox centre)
    trig_src = next((x for x in src_objs if re.search(r"trigger", x.name, re.I)), None)
    anchor = o.get("anchor", "trigger")
    if anchor == "trigger" and trig_src is not None and "trigger" in tpl.pos:
        a_s = obj_center(trig_src)
        a_t = tpl.pos["trigger"]
    else:
        a_s = (slo + shi) / 2; a_t = (tlo + thi) / 2
    G = Matrix.Translation(a_t) @ Matrix.Scale(k, 4) @ Matrix.Translation(-a_s)
    for x in src_objs:
        if x.parent is None: x.matrix_world = G @ x.matrix_world
    apply_transforms([x for x in src_objs if x.type in ("MESH", "EMPTY")])
    log(f"weapon: scale {k:.3f} (source length {(shi.x - slo.x):.3f} m, template {(thi.x - tlo.x):.3f} m), "
        f"anchor {anchor if trig_src is not None else 'bbox'}")
    # markers -> positions (UE cm, component space of the template skeleton = Blender metres * 100, y/z as is)
    markers = {}
    for x in src_objs:
        n = x.name.lower()
        for mk in MARKERS:
            if x.type == "EMPTY" and re.search(mk, n):
                markers[mk] = list(x.matrix_world.translation)
    if "muzzle" not in markers:
        # front-most point of the model, at the height/side of the barrel end
        front = max(sverts_now(meshes), key=lambda p: p.x)
        near = [p for p in sverts_now(meshes) if p.x > front.x - 0.01]
        markers["muzzle"] = [front.x, sum(p.y for p in near) / len(near), sum(p.z for p in near) / len(near)]
        log("muzzle marker: none in the source, using the barrel tip", [round(v, 3) for v in markers["muzzle"]])
    # bind parts
    rules = [(re.compile(a, re.I), b) for a, b in PART_RULES]
    for spec in o.get("part", []):
        a, b = spec.split("=", 1); rules.insert(0, (re.compile(a, re.I), b))
    bound = []
    for m in meshes:
        bone = main
        for rx, b in rules:
            if rx.search(m.name) and b in tb: bone = b; break
        m.parent = None
        m.matrix_world = m.matrix_world
        m.vertex_groups.clear()
        g = m.vertex_groups.new(name=bone); g.add(range(len(m.data.vertices)), 1.0, "REPLACE")
        m.parent = tpl.arm; m.matrix_parent_inverse = tpl.arm.matrix_world.inverted()
        md = m.modifiers.new("Armature", "ARMATURE"); md.object = tpl.arm
        bound.append((m.name, bone))
    log("parts:", bound)
    for x in src_objs:
        if x.type == "EMPTY": bpy.data.objects.remove(x)
    tpl.hide_meshes()
    o["extras"] = {"markers_m": markers, "bone_positions_m": {b: list(p) for b, p in tpl.pos.items()},
                   "parts": bound, "scale": k}
    finish(o, tpl, meshes)


def obj_center(x):
    if x.type != "MESH": return x.matrix_world.translation.copy()
    vs = [x.matrix_world @ v.co for v in x.data.vertices]
    return sum(vs, Vector()) / len(vs)


def sverts_now(meshes):
    return [m.matrix_world @ v.co for m in meshes for v in m.data.vertices]


# ---- inspect --------------------------------------------------------------------------------------------------------

def alpha_stats(path):
    """(fraction of texels with alpha < 0.5, fraction with 0.05 < alpha < 0.95) of an image, None without alpha."""
    import numpy as np
    try:
        img = bpy.data.images.load(path, check_existing=False)
    except RuntimeError:
        return None
    if img.channels < 4 or not img.size[0]:
        bpy.data.images.remove(img); return None
    if img.size[0] * img.size[1] > 1024 * 1024:
        img.scale(max(1, img.size[0] // 2), max(1, img.size[1] // 2))
    a = np.empty(img.size[0] * img.size[1] * 4, dtype=np.float32)
    img.pixels.foreach_get(a)
    bpy.data.images.remove(img)
    al = a[3::4]
    return float((al < 0.5).mean()), float(((al > 0.05) & (al < 0.95)).mean())


# ---- what a material looks like: auto slots for models whose names say nothing (game rips: "Material #30") ---------
UV_GRID = 128                 # UV coverage and image statistics on a 128 x 128 grid
_SMALL = {}


def small_px(path, n=UV_GRID):
    """An image at n x n (float RGBA 0..1 as stored, top row first), cached; None if Blender can't read it."""
    import numpy as np
    key = (path, n)
    if key not in _SMALL:
        try:
            img = bpy.data.images.load(path, check_existing=False)
        except RuntimeError:
            _SMALL[key] = None; return None
        if not img.size[0]:
            bpy.data.images.remove(img); _SMALL[key] = None; return None
        img.colorspace_settings.name = "Non-Color"
        img.scale(n, n)
        a = np.empty(n * n * 4, dtype=np.float32)
        img.pixels.foreach_get(a)
        bpy.data.images.remove(img)
        _SMALL[key] = a.reshape(n, n, 4)[::-1].copy()
    return _SMALL[key]


def _raster(mask, a, b, c):
    import numpy as np
    n = mask.shape[0]
    x0, x1 = max(0, int(min(a[0], b[0], c[0]))), min(n - 1, int(max(a[0], b[0], c[0])))
    y0, y1 = max(0, int(min(a[1], b[1], c[1]))), min(n - 1, int(max(a[1], b[1], c[1])))
    if x1 < x0 or y1 < y0: return
    if x1 - x0 < 1 and y1 - y0 < 1:
        mask[y0, x0] = True; return
    ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1]
    px, py = xs + 0.5, ys + 0.5
    d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
    if abs(d) < 1e-12:
        mask[int((a[1] + b[1] + c[1]) / 3) % n, int((a[0] + b[0] + c[0]) / 3) % n] = True; return
    l1 = ((b[1] - c[1]) * (px - c[0]) + (c[0] - b[0]) * (py - c[1])) / d
    l2 = ((c[1] - a[1]) * (px - c[0]) + (a[0] - c[0]) * (py - c[1])) / d
    inside = (l1 >= -0.02) & (l2 >= -0.02) & (1 - l1 - l2 >= -0.02)
    if not inside.any():
        mask[min(n - 1, int((a[1] + b[1] + c[1]) / 3)), min(n - 1, int((a[0] + b[0] + c[0]) / 3))] = True
    mask[y0:y1 + 1, x0:x1 + 1] |= inside


def uv_cover(parts, n=UV_GRID, max_polys=40000):
    """n x n bool mask (image rows, top first) of the texels the faces' UVs cover. parts: [(mesh object, [polygons])].
    UVs outside 0..1 wrap (each face by its centre's whole-tile offset); big meshes are sampled (every k-th face)."""
    import numpy as np
    mask = np.zeros((n, n), bool)
    total = sum(len(ps) for _, ps in parts)
    step = max(1, total // max_polys)
    k = 0
    for m, polys in parts:
        uv = m.data.uv_layers.active
        if uv is None: continue
        for p in polys:
            k += 1
            if k % step: continue
            pts = [uv.data[li].uv for li in p.loop_indices]
            du = math.floor(sum(q[0] for q in pts) / len(pts)); dv = math.floor(sum(q[1] for q in pts) / len(pts))
            P = [((q[0] - du) * n, (1.0 - (q[1] - dv)) * n) for q in pts]
            for i in range(1, len(P) - 1):
                _raster(mask, P[0], P[i], P[i + 1])
    return mask


def skin_like(rgb):
    """Texels (sRGB 0..1) in the colours of skin: red the largest channel, orange-ish hue, not grey, not dark."""
    import numpy as np
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    mx, mn = rgb.max(axis=-1), rgb.min(axis=-1)
    sat = (mx - mn) / np.maximum(mx, 1e-6)
    return (r >= mx - 1e-6) & (g >= b - 0.02) & (r - g >= 0.03) & (sat > 0.1) & (sat < 0.75) & (mx > 0.3)


def looks_like_normal_map(path):
    """An image whose texels are the tangent-space normals of a normal map: blue about 1, red and green around 0.5."""
    a = small_px(path, 64)
    if a is None: return False
    m = a[..., :3].reshape(-1, 3).mean(axis=0)
    return bool(m[2] > 0.7 and 0.35 < m[0] < 0.65 and 0.35 < m[1] < 0.65 and a[..., 2].std() < 0.15)


def empty_texels(a):
    """Texels of an image that hold nothing: transparent, or the image's fill colour (its most common colour)."""
    import numpy as np
    q = np.round(a[..., :3] * 15).astype(np.int32)
    key = q[..., 0] * 256 + q[..., 1] * 16 + q[..., 2]
    vals, counts = np.unique(key, return_counts=True)
    mode = vals[np.argmax(counts)]
    return (a[..., 3] < 0.5) | ((key == mode) & (counts.max() > 0.08 * key.size))


def appearance(tx, parts):
    """What the texels a material's faces use look like: {"cover": share of the image used, "clear": share of those
    texels transparent (hair cards), "skin": share in skin colours, "color": mean sRGB}."""
    import numpy as np
    bc = tx.get("basecolor")
    a = small_px(bc) if bc and os.path.isfile(bc) else None
    if a is None: return {}
    mask = uv_cover(parts)
    if not mask.any(): return {}
    al = a[..., 3]
    ap = tx.get("alpha")
    if ap and ap != bc and os.path.isfile(ap):
        x = small_px(ap)
        if x is not None: al = x[..., 3] if x[..., 3].min() < 0.99 else x[..., 0]
    c = a[..., :3][mask]
    return {"cover": round(float(mask.mean()), 4), "clear": round(float((al[mask] < 0.5).mean()), 4),
            "skin": round(float(skin_like(c).mean()), 4), "color": [round(float(x), 3) for x in c.mean(axis=0)]}


def guess_bare_texture(parts, claimed, tex_dirs):
    """The colour image faces without a material draw (teeth, tongue and mouth of game rips, which use the body's image):
    the image where their UVs land on painted texels that no material's faces use. claimed: {image: mask of the texels
    the materials use}. Returns (image, score) or (None, 0)."""
    import numpy as np
    mask = uv_cover(parts)
    if not mask.any(): return None, 0.0
    best = (None, 0.0)
    cands = sorted(set(list(claimed) + [f for f in images_in(tex_dirs) if role_from_name(f) in (None, "basecolor")]),
                   key=lambda f: (f.lower().endswith(".dds"), f))
    seen = set()
    for f in cands:
        if not os.path.isfile(f) or looks_like_normal_map(f): continue
        stem = os.path.splitext(os.path.basename(f))[0].lower()
        if stem in seen: continue                    # all_color.png and all_color.dds: the same image
        seen.add(stem)
        a = small_px(f)
        if a is None: continue
        used = claimed.get(f)
        if used is None:
            used = next((v for k, v in claimed.items() if os.path.splitext(os.path.basename(k))[0].lower() == stem), None)
        free = ~empty_texels(a) & (~used if used is not None else True)
        score = float(free[mask].mean())
        if used is None: score *= 0.5                # an image no material uses: less likely
        if score > best[1] + 1e-6: best = (f, score)
    return best


def inspect(src, out, tex_user=None):
    global TEX_CACHE
    TEX_CACHE = os.path.join(os.path.dirname(out), "textures")
    objs = import_any(src)
    tex_dirs = source_tex_dirs(src)
    info = {"objects": [], "materials": [], "armatures": [], "material_info": {}}
    mats = []
    for o in objs:
        if o.type == "MESH" and not o.name.startswith("Icosphere"):
            info["objects"].append({"name": o.name, "vertices": len(o.data.vertices), "faces": len(o.data.polygons),
                                    "materials": [re.sub(r"\.\d{3}$", "", m.name) if m else None
                                                  for m in o.data.materials],
                                    "parent": o.parent.name if o.parent else None,
                                    "shape_keys": len(o.data.shape_keys.key_blocks) if o.data.shape_keys else 0})
            for m in used_materials(o):
                n = re.sub(r"\.\d{3}$", "", m.name) if m else NO_MATERIAL
                if n not in info["materials"]: info["materials"].append(n); mats.append(m)
        elif o.type == "ARMATURE":
            info["armatures"].append({"name": o.name, "bones": [b.name for b in o.data.bones]})
        elif o.type == "EMPTY":
            info["objects"].append({"name": o.name, "empty": True, "parent": o.parent.name if o.parent else None})
    faces, bare = {}, []
    for o in objs:
        if o.type != "MESH": continue
        for poly in o.data.polygons:
            if poly.material_index < len(o.data.materials) and o.data.materials[poly.material_index]:
                n = re.sub(r"\.\d{3}$", "", o.data.materials[poly.material_index].name)
            else:
                n = NO_MATERIAL
                if o.name not in bare: bare.append(o.name)
            faces[n] = faces.get(n, 0) + 1
    if bare:
        info["material_info"][NO_MATERIAL] = {"textures": {}, "faces": faces.get(NO_MATERIAL, 0), "objects": bare}
    real = [m for m in mats if m is not None]
    # the faces of each material (by name, as the pipeline labels them) and the objects they belong to
    parts, owners, solo = {}, {}, {}
    for o in objs:
        if o.type != "MESH": continue
        by = {}
        for poly in o.data.polygons:
            i = poly.material_index
            n = re.sub(r"\.\d{3}$", "", o.data.materials[i].name) if i < len(o.data.materials) and \
                o.data.materials[i] else NO_MATERIAL
            by.setdefault(n, []).append(poly)
        for n, ps in by.items():
            parts.setdefault(n, []).append((o, ps))
            owners.setdefault(n, []).append(o.name)
            if len(by) == 1: solo.setdefault(n, []).append(o.name)
    claimed = {}
    for m in real + ([None] if NO_MATERIAL in info["materials"] else []):
        n = re.sub(r"\.\d{3}$", "", m.name) if m else NO_MATERIAL
        tx = material_textures(m, tex_dirs, tex_user, len(real))
        if m is None:
            if NO_MATERIAL in info["material_info"]:
                if not tx.get("basecolor") and not any(k.lower() == NO_MATERIAL for k in (tex_user or {})):
                    f, score = guess_bare_texture(parts.get(n, []), claimed, tex_dirs)
                    if f and score >= 0.5:
                        log(f"  {n} (objects without a material: {', '.join(owners.get(n, [])[:4])}): their UVs land "
                            f"on {os.path.basename(f)} where no material draws ({score:.0%}): that image")
                        tx = with_companions({"basecolor": f}, n, tex_dirs, None)
                        info["material_info"][n]["guessed"] = f
                info["material_info"][n]["textures"] = tx
                info["material_info"][n]["look"] = appearance(tx, parts.get(n, []))
            continue
        c = bsdf_values(m).get("basecolor")
        if not tx.get("basecolor") and c and max(c) - min(c) < 0.01 and min(c) >= 0.4:
            # nothing to colour it with (a file exported without its images, only the exporter's default light grey,
            # 0.5-0.8; dark flat colours are meant): say so here, else the modder only finds out from a white preview
            log(f"  material {n}: WARNING: no colour image found and its colour is a flat grey ({c[0]:.2f}): it will "
                f"look plain white/grey; if the download has its textures elsewhere: --tex {n}=<file prefix|folder>")
        mi = {"textures": tx, "faces": faces.get(n, 0), "blend": getattr(m, "blend_method", "OPAQUE"),
              "factor": basecolor_factor(m), "objects": owners.get(n, []), "own_objects": solo.get(n, [])}
        ap = tx.get("alpha") or tx.get("basecolor")
        if ap and os.path.isfile(ap):
            st = alpha_stats(ap)
            if st: mi["alpha_clear"], mi["alpha_soft"] = st
        mi["look"] = appearance(tx, parts.get(n, []))
        gb = garment_bone_faces(parts.get(n, []))
        if gb: mi["garment_bones"] = gb
        bc = tx.get("basecolor")
        if bc and os.path.isfile(bc):
            cov = uv_cover(parts.get(n, []))
            claimed[bc] = claimed[bc] | cov if bc in claimed else cov
        info["material_info"][n] = mi
    json.dump(info, open(out, "w"), indent=1)
    log("wrote", out)


# ---- texture composition (numpy; no Pillow needed) --------------------------------------------------------------------

def load_px(path, size, gray=False, height=None):
    """Image file (PNG, JPEG, TGA, DDS ...) -> float32 array (height or size, size, 4), top row first, 0..1 as stored
    (no colour management)."""
    import numpy as np
    h = height or size
    try:
        img = bpy.data.images.load(path, check_existing=False)
    except RuntimeError as e:
        raise SystemExit(f"{path}: Blender can't read this image ({e}); convert it to PNG")
    img.colorspace_settings.name = "Non-Color"
    if not img.size[0]:
        bpy.data.images.remove(img)
        raise SystemExit(f"{path}: Blender can't read this image (a DDS in a format it doesn't know?); convert it to PNG")
    if img.size[0] != size or img.size[1] != h:
        img.scale(size, h)
    a = np.empty(size * h * 4, dtype=np.float32)
    img.pixels.foreach_get(a)
    bpy.data.images.remove(img)
    a = a.reshape(h, size, 4)[::-1]
    return a


def tile_px(path, pw, rep=None, ph=None):
    """A tile's image (pw x ph): the file, or rep [nx, ny] repeats of it (a tiling texture in an atlas tile)."""
    import numpy as np
    ph = ph or pw
    nx, ny = rep or (1, 1)
    if (nx, ny) == (1, 1): return load_px(path, pw, height=ph)
    a = load_px(path, max(1, pw // nx), height=max(1, ph // ny))
    t = np.tile(a, (ny, nx, 1))
    if t.shape[0] != ph or t.shape[1] != pw:
        t = t[(np.arange(ph) * t.shape[0]) // ph][:, (np.arange(pw) * t.shape[1]) // pw]
    return t


def tile_box(t, W, H):
    """A tile's pixel box (x, y, w, h) in a W x H atlas image (rects are 0..1, top-left origin)."""
    x0, y0, w, h = t["rect"]
    px, py = int(round(x0 * W)), int(round(y0 * H))
    return px, py, max(1, int(round((x0 + w) * W)) - px), max(1, int(round((y0 + h) * H)) - py)


def job_size(j):
    """(W, H) of a compose job (older jobs: one int = square)."""
    s = j["size"]
    return (s, s) if isinstance(s, int) else (int(s[0]), int(s[1]))


def decode_normal(n, what=""):
    """A normal map texel array -> tangent-space RGB (OpenGL/DirectX as stored). Understood: RGB normal maps,
    two-channel (BC5/ATI2: B empty) and DXT5nm (X in alpha, R constant) with Z rebuilt; anything else (a flow or
    specular map named _n) is left out (None)."""
    import numpy as np
    r, g, b, a = n[..., 0], n[..., 1], n[..., 2], n[..., 3]
    if b.mean() > 0.6:
        return n[..., :3]
    if r.std() < 0.02 and a.std() > 0.05:
        x, y, kind = a, g, "DXT5nm (X in alpha)"
    elif b.std() < 0.02:
        x, y, kind = r, g, "two-channel (BC5)"
    else:
        log(f"  {what}: not a normal map (blue channel {b.mean():.2f} on average, a normal map's is about 1): left out")
        return None
    z = np.sqrt(np.clip(1.0 - (2 * x - 1) ** 2 - (2 * y - 1) ** 2, 0.0, 1.0)) * 0.5 + 0.5
    log(f"  {what}: {kind} normal map, Z rebuilt")
    return np.stack([x, y, z], axis=-1)


def image_mean(path):
    import numpy as np
    img = bpy.data.images.load(path, check_existing=False)
    img.colorspace_settings.name = "Non-Color"
    a = np.empty(img.size[0] * img.size[1] * 4, dtype=np.float32)
    img.pixels.foreach_get(a)
    bpy.data.images.remove(img)
    return a.reshape(-1, 4).mean(axis=0)


def save_px(arr, path):
    import numpy as np
    h, w = arr.shape[:2]
    img = bpy.data.images.new("out", w, h, alpha=True)
    img.colorspace_settings.name = "Non-Color"
    img.pixels.foreach_set(np.ascontiguousarray(arr[::-1]).astype(np.float32).ravel())
    img.filepath_raw = path; img.file_format = "PNG"
    img.save()
    bpy.data.images.remove(img)


def lin2srgb(c):
    return c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055


def tinted(rgb, val):
    """sRGB texels x the material's base colour factor (linear, as glTF/Blender multiply), back to sRGB."""
    f = val.get("basecolor_factor")
    if not f: return rgb
    import numpy as np
    lin = np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4) * np.array(f, np.float32)
    return np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.maximum(lin, 0) ** (1 / 2.4) - 0.055).astype(np.float32)


def tile_alpha(tx, pw, rep=None, ph=None):
    """A tile's opacity: its alpha texture (A if it has an alpha channel, else R), else the base colour's alpha."""
    import numpy as np
    for k in ("alpha", "basecolor"):
        p = tx.get(k)
        if p and os.path.exists(p):
            a = tile_px(p, pw, rep, ph)
            if a[..., 3].min() < 0.99: return a[..., 3]
            if k == "alpha": return a[..., 0]
    return np.ones((ph or pw, pw), np.float32)


def hair_multimask(j, W, H):
    """Master_Hair_M's "Hair MultiMask" (Enable MultiMask on): A = strand alpha (masked, dithered). R/G/B (root, depth,
    id-style masks) = the retail texture's average inside its strands. Also writes <out>.json with the hair colour
    (mean base colour of the opaque texels, linear) for the MI's RootColor/TipColor."""
    import numpy as np
    canvas = np.zeros((H, W, 4), np.float32)
    rgb = np.array([0.45, 0.28, 0.3], np.float32)
    if j.get("mean_from"):
        r = load_px(j["mean_from"], 256)
        m = r[..., 3] > 0.5
        if m.any(): rgb = r[..., :3][m].mean(axis=0)
    canvas[..., :3] = rgb
    cols, weights = [], []
    for t in j["tiles"]:
        px, py, pw, ph = tile_box(t, W, H)
        tx, val = t.get("textures", {}), t.get("values", {})
        rep = t.get("repeat")
        a = tile_alpha(tx, pw, rep, ph)
        canvas[py:py + ph, px:px + pw, 3] = a
        if tx.get("basecolor") and os.path.exists(tx["basecolor"]):
            c = tinted(tile_px(tx["basecolor"], pw, rep, ph)[..., :3], val)
            m = a > 0.5
            if m.any():
                cols.append(c[m].mean(axis=0)); weights.append(float(m.sum()))
        elif "basecolor" in val:
            cols.append(np.array([lin2srgb(x) for x in val["basecolor"]])); weights.append(1.0)
    srgb = np.average(np.array(cols), axis=0, weights=weights) if cols else np.array([0.3, 0.2, 0.12])
    lin = [x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in srgb.tolist()]
    json.dump({"color_linear": lin, "color_srgb": srgb.tolist(), "retail_rgb": rgb.tolist()}, open(j["out"] + ".json", "w"))
    log("hair: colour (sRGB)", [round(x, 3) for x in srgb.tolist()], "multimask RGB", [round(float(x), 3) for x in rgb])
    return canvas


def fill_transparent(rgb, a, thresh=0.5):
    """Colour of the see-through texels = the colour of the nearest strands (pull-push over a mip pyramid), so mips
    and filtering at strand edges blend strand colour, not whatever the see-through texels held (often black/white)."""
    import numpy as np
    w = (a >= thresh).astype(np.float32)
    if w.all() or not w.any(): return rgb
    levels = [(rgb * w[..., None], w)]
    while min(levels[-1][1].shape) > 1:
        c, ww = levels[-1]
        h, wd = c.shape[0] // 2, c.shape[1] // 2
        c2 = c[:h * 2, :wd * 2].reshape(h, 2, wd, 2, 3).sum(axis=(1, 3))
        w2 = ww[:h * 2, :wd * 2].reshape(h, 2, wd, 2).sum(axis=(1, 3))
        levels.append((c2, w2))
    c, ww = levels[-1]                              # a non-square image ends in a row or column: its mean fills it
    c = np.broadcast_to(c.sum(axis=(0, 1)), c.shape); ww = np.broadcast_to(ww.sum(), ww.shape)
    fill = c / np.maximum(ww, 1e-6)[..., None]
    for c, ww in reversed(levels[:-1]):
        up = np.repeat(np.repeat(fill, 2, axis=0), 2, axis=1)[:c.shape[0], :c.shape[1]]
        own = c / np.maximum(ww, 1e-6)[..., None]
        fill = np.where((ww > 0)[..., None], own, up)
    out = rgb.copy()
    m = w == 0
    out[m] = fill[m]
    return out


def hair_basecolor(j, W, H):
    """Hair on a colour-textured masked material (b4bmodel --hair texture): RGB = the model's hair colour texture
    (sRGB as is), A = its alpha (strand coverage: masked + dithered in game); see-through texels take the colour of
    the nearest strands. Also writes <out>.json with the mean colour of the opaque texels (log / preview)."""
    import numpy as np
    canvas = np.zeros((H, W, 4), np.float32)
    canvas[..., :3] = 0.23
    cols, weights = [], []
    for t in j["tiles"]:
        px, py, pw, ph = tile_box(t, W, H)
        tx, val = t.get("textures", {}), t.get("values", {})
        rep = t.get("repeat")
        a = tile_alpha(tx, pw, rep, ph)
        region = canvas[py:py + ph, px:px + pw]
        if tx.get("basecolor") and os.path.exists(tx["basecolor"]):
            region[..., :3] = tinted(tile_px(tx["basecolor"], pw, rep, ph)[..., :3], val)
        elif "basecolor" in val:
            region[..., :3] = [lin2srgb(c) for c in val["basecolor"]]
        region[..., 3] = a
        m = a > 0.5
        if m.any():
            cols.append(region[..., :3][m].mean(axis=0)); weights.append(float(m.sum()))
    canvas[..., :3] = fill_transparent(canvas[..., :3], canvas[..., 3])
    srgb = np.average(np.array(cols), axis=0, weights=weights) if cols else np.array([0.3, 0.2, 0.12])
    json.dump({"color_srgb": srgb.tolist(), "opaque": float(np.mean(canvas[..., 3] > 0.5))}, open(j["out"] + ".json", "w"))
    log("hair: colour texture, mean (sRGB)", [round(float(x), 3) for x in srgb], "coverage",
        round(float(np.mean(canvas[..., 3] > 0.5)), 3))
    return canvas


def compose(job_path):
    """Jobs: [{out, size, role basecolor|normal|pbr|zero|mean|hairmm|hairbc, tiles, mean_from, normal_dx}]"""
    import numpy as np
    jobs = json.load(open(job_path))
    for j in jobs:
        W, H = job_size(j)
        mean = image_mean(j["mean_from"]) if j.get("mean_from") else np.array([0.5, 0.5, 0.5, 1.0])
        role = j["role"]
        if role == "zero":
            canvas = np.zeros((H, W, 4), np.float32)
        elif role == "hairmm":
            canvas = hair_multimask(j, W, H)
        elif role == "hairbc":
            canvas = hair_basecolor(j, W, H)
        elif role == "mean":
            canvas = np.tile(mean.astype(np.float32), (H, W, 1))
        else:
            fill = {"normal": (0.5, 0.5, 1.0, 1.0), "basecolor": (0.23, 0.23, 0.23, mean[3]),
                    "pbr": (1.0, 0.7, 0.0, mean[3])}[role]
            canvas = np.tile(np.array(fill, np.float32), (H, W, 1))
            for t in j["tiles"]:
                px, py, pw, ph = tile_box(t, W, H)
                tx, val = t.get("textures", {}), t.get("values", {})
                ok = lambda k: tx.get(k) and os.path.exists(tx[k])
                rep = t.get("repeat")
                load = lambda p: tile_px(p, pw, rep, ph)
                region = canvas[py:py + ph, px:px + pw]
                if role == "basecolor":
                    if ok("basecolor"):
                        region[..., :3] = tinted(load(tx["basecolor"])[..., :3], val)
                    elif "basecolor" in val:
                        region[..., :3] = [lin2srgb(c) for c in val["basecolor"]]
                    region[..., 3] = mean[3]
                elif role == "normal":
                    n = decode_normal(load(tx["normal"]), f"{t['material']}: {os.path.basename(tx['normal'])}") \
                        if ok("normal") else None
                    if n is not None:
                        region[..., 0] = n[..., 0]
                        region[..., 1] = n[..., 1] if j.get("normal_dx") else 1.0 - n[..., 1]
                        region[..., 2] = n[..., 2]
                        region[..., 3] = 1.0
                else:   # pbr: R = AO, G = roughness, B = metallic, A = the retail texture's average
                    # Unity HDRP mask map: R metallic, G AO, B detail mask, A smoothness
                    mask_i = {"ao": 1, "roughness": 3, "metallic": 0}
                    def chan(key, orm_i, const):
                        if ok(key): return load(tx[key])[..., 0]
                        if ok("orm"): return load(tx["orm"])[..., orm_i]
                        if ok("mask"):
                            c = load(tx["mask"])[..., mask_i[key]]
                            return 1.0 - c if key == "roughness" else c
                        if key == "roughness" and ok("gloss"): return 1.0 - load(tx["gloss"])[..., 0]
                        return np.full((ph, pw), const, np.float32)
                    region[..., 0] = chan("ao", 0, 1.0)
                    region[..., 1] = chan("roughness", 1, val.get("roughness", 0.7))
                    region[..., 2] = chan("metallic", 2, val.get("metallic", 0.0) if j.get("kind") != "character" else 0.0)
                    region[..., 3] = mean[3]
        save_px(np.clip(canvas, 0, 1), j["out"])
        log("composed", os.path.basename(j["out"]), role, f"{W}x{H}")


# ---- convert --------------------------------------------------------------------------------------------------------

def convert(src, dst):
    import_any(src)
    for o in list(bpy.data.objects):
        if o.type == "MESH" and o.name.startswith("Icosphere"): bpy.data.objects.remove(o)
    bpy.ops.export_scene.gltf(filepath=dst, export_format="GLB", export_all_influences=True, export_animations=False)
    log("wrote", dst)


if __name__ == "__main__":
    pos, opts = parse(argv)
    if not pos:
        print(__doc__); sys.exit(0)
    reset()
    kind = pos[0]
    opts["kind"] = kind
    if kind == "character":
        fit_character(opts)
    elif kind == "weapon":
        fit_weapon(opts)
    elif kind == "convert":
        convert(pos[1], pos[2])
    elif kind == "inspect":
        inspect(pos[1], pos[2], dict(x.split("=", 1) for x in opts.get("tex", [])))
    elif kind == "compose":
        compose(pos[1])
    else:
        raise SystemExit(f"unknown mode {kind}")
