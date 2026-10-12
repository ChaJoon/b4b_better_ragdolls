"""Headless preview render of a model file (fbx, glb/gltf, obj, blend): front, side and back views side by side,
Cycles on the CPU (works without a GPU). For checking a model before and after the pipeline steps.

  blender -b --python blender/preview.py -- <model> <out.png> [--size 512] [--pose test|crouch|turn|walk|aim]
      [--views front,side,back,top,front3q] [--zoom 1.0] [--focus <object name substring>] [--textures <json>]
  --sim <work>/fit3p/manifest.json: the cloth simulation meshes (skirts, coats, capes) drawn as red wire.
  --pose test bends arms, legs, spine and head of a B4B skeleton (skmgltf export or b4bfit output) to check weights.
  --pose crouch|turn|walk|aim: game-like poses (hips and knees folded; hips turned under an upper body that keeps
  aiming; a stride; arms up with the head turned): where the game bends a survivor most. Render the template the same
  way to compare.
  --textures: {"<material / slot name>": "<base colour png>"}, e.g. <work>/preview_textures.json written by
  `b4bmod survivor` (the fitted model with the textures made for the game). Views follow the model's own facing when
  it has a B4B skeleton (front = the face).
  --face <work>/face_preview.json [--face-pose AH|Joy|...] [--face-blink DEG]: close-up of the head with the face bones
  where the game will have them (moved onto the model's face) and one of the survivor's face poses applied the way the
  game adds it (lip-sync visemes AH, E, OW, MBP ...; expressions Joy, Anger, Surprise ...), and/or the upper eyelids
  rotated shut by DEG degrees (a blink). Without a pose: the face at rest. --face-view mouth|eyes: a closer look at
  the mouth or the eyes (with --zoom). Back faces of one-sided materials are left out (as in the game).
  --fp hold: first-person arms (an FP_ mesh: b4bfit's fitfp/lod*.glb or the retail FP mesh) in a two-handed gun hold
  (arms bent onto fixed hand targets in front of the camera bone), seen from the FP camera (90 deg); render the retail
  FP mesh the same way to compare: the game's FP view is tight, arms off the FP skeleton's joints don't show.
"""
import bpy, math, os, sys
from mathutils import Matrix, Vector

argv = sys.argv[sys.argv.index("--") + 1:]
src, out = argv[0], argv[1]
opts = {"size": "512", "views": "front,side,back", "zoom": "1.0", "focus": "", "pose": "", "textures": "", "face": "",
        "face_pose": "", "face_blink": "0", "face_view": "face", "sim": "", "fp": ""}
i = 2
while i < len(argv):
    opts[argv[i].lstrip("-").replace("-", "_")] = argv[i + 1]; i += 2
size = int(opts["size"])

bpy.ops.wm.read_factory_settings(use_empty=True)
ext = os.path.splitext(src)[1].lower()
if ext == ".fbx": bpy.ops.import_scene.fbx(filepath=src)
elif ext in (".glb", ".gltf", ".vrm"):
    bpy.ops.import_scene.gltf(filepath=src)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from b4bfit import vrm0_colors
    vrm0_colors(src)                              # MToon _Color as sRGB, as the pipeline reads it
elif ext == ".obj": bpy.ops.wm.obj_import(filepath=src)
elif ext == ".blend": bpy.ops.wm.open_mainfile(filepath=src)
for o in list(bpy.data.objects):
    if o.type in ("CAMERA", "LIGHT"): bpy.data.objects.remove(o)
# --pose test: bend the B4B skeleton's limbs (checks skin weights: no exploding or stuck vertices)
TEST_POSE = {"upperarm_l": (0, 0, -50), "upperarm_r": (0, 0, 50), "lowerarm_l": (0, 0, -70), "lowerarm_r": (70, 0, 0),
             "hand_l": (0, 60, 0), "spine_02": (0, 25, 0), "neck_01": (0, 0, 25), "head": (20, 0, 0),
             "thigh_l": (0, 0, 45), "calf_l": (0, 0, -70), "thigh_r": (0, 0, -20), "foot_r": (0, 0, 30)}
if opts["pose"] == "test":
    import math as _m
    for a in [o for o in bpy.data.objects if o.type == "ARMATURE"]:
        for name, (x, y, z) in TEST_POSE.items():
            pb = a.pose.bones.get(name)
            if pb:
                pb.rotation_mode = "XYZ"
                pb.rotation_euler = (_m.radians(x), _m.radians(y), _m.radians(z))
    bpy.context.view_layer.update()
# --pose crouch|turn|walk|aim: game-like poses of a B4B skeleton, each joint turned about the body's own axes (lat =
# towards the left side, up, fwd = where the hero faces) in degrees, parents first. For checking weights where the
# game bends most: crouching (hips and knees folded), turning in place (hips turned under an upper body that keeps
# aiming), a walking stride, aiming with the head turned.
GAME_POSES = {
    "crouch": [("pelvis", "lat", 15), ("thigh_l", "lat", -95), ("thigh_r", "lat", -85), ("calf_l", "lat", 120),
               ("calf_r", "lat", 125), ("foot_l", "lat", -30), ("foot_r", "lat", -35), ("spine_01", "lat", 15),
               ("spine_02", "lat", 15), ("spine_03", "lat", 5), ("head", "lat", -20)],
    "turn": [("pelvis", "up", 45), ("thigh_l", "up", -15), ("thigh_r", "up", -15), ("thigh_l", "lat", -20),
             ("calf_l", "lat", 25), ("spine_01", "up", -15), ("spine_02", "up", -15), ("spine_03", "up", -15),
             ("neck_01", "up", 10), ("head", "up", 25)],
    "walk": [("thigh_l", "lat", -40), ("calf_l", "lat", 30), ("thigh_r", "lat", 25), ("calf_r", "lat", 55),
             ("foot_r", "lat", -20), ("pelvis", "up", 8), ("spine_02", "up", -8), ("upperarm_l", "lat", 25),
             ("upperarm_r", "lat", -25), ("lowerarm_r", "lat", -30)],
    "aim": [("upperarm_r", "lat", -40), ("upperarm_r", "fwd", 25), ("lowerarm_r", "lat", -60), ("upperarm_l", "lat", -55),
            ("upperarm_l", "fwd", -35), ("lowerarm_l", "lat", -30), ("spine_01", "up", -10), ("spine_02", "up", -15),
            ("spine_03", "lat", -10), ("head", "up", 35), ("head", "lat", 15)],
}
if opts["pose"] in GAME_POSES:
    for a in [o for o in bpy.data.objects if o.type == "ARMATURE"]:
        bh = lambda n: a.data.bones[n].head_local
        if not all(n in a.data.bones for n in ("upperarm_l", "upperarm_r", "head", "pelvis")): continue
        lat = (bh("upperarm_l") - bh("upperarm_r")).normalized()
        up = bh("head") - bh("pelvis"); up = (up - lat * up.dot(lat)).normalized()
        axes = {"lat": lat, "up": up, "fwd": lat.cross(up).normalized()}
        depth = lambda n: len(a.data.bones[n].parent_recursive)
        steps = [s for s in GAME_POSES[opts["pose"]] if s[0] in a.pose.bones]
        for name, ax, deg in sorted(steps, key=lambda s: depth(s[0])):
            bpy.context.view_layer.update()
            pb = a.pose.bones[name]
            h = pb.head.copy()                    # armature space, posed
            R = Matrix.Rotation(math.radians(deg), 4, axes[ax])
            pb.matrix = Matrix.Translation(h) @ R @ Matrix.Translation(-h) @ pb.matrix
        bpy.context.view_layer.update()


def face_pose(fj, pose_name, blink_deg):
    """Move the face bones to the mesh's bind positions (face_preview.json "moved") and pose them like the game: each
    face pose is additive in bone-local (parent) space: rotation = delta * rest, translation = rest + delta."""
    import json as _j
    from mathutils import Matrix as M4, Quaternion as Q
    d = _j.load(open(fj))
    arm = next(a for a in bpy.data.objects if a.type == "ARMATURE")
    C = M4.Diagonal((0.01, -0.01, 0.01, 1.0))                 # UE cm -> Blender m (y flipped)
    Ci = C.inverted()
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")
    Mw = arm.matrix_world
    for n, p in d["moved"].items():
        eb = next((e for e in arm.data.edit_bones if e.name.lower() == n), None)
        if eb is None: continue
        dv = Mw.inverted() @ (C @ Vector(p).to_4d()).to_3d() - eb.head
        eb.head += dv; eb.tail += dv
    bpy.ops.object.mode_set(mode="POSE")
    rest = d["rest"]
    def ue_q(q): return Q((q[3], q[0], q[1], q[2]))
    def local(n, delta=None):
        par, q, t = rest[n]
        R = ue_q(q).to_matrix().to_4x4()
        tt = Vector(t)
        if delta:
            R = ue_q(delta[0:4]).to_matrix().to_4x4() @ R
            tt = tt + Vector(delta[4:7])
        return M4.Translation(tt) @ R
    deltas = dict(d["poses"].get(pose_name, {})) if pose_name else {}
    if pose_name and pose_name not in d["poses"]:
        print("no face pose", pose_name, "- poses:", ", ".join(d["poses"]))
    a = math.radians(float(blink_deg))
    if a:
        for sd in ("l", "r"):
            deltas[f"eyelid_upper_{sd}"] = [0.0, math.sin(a / 2), 0.0, math.cos(a / 2), 0, 0, 0]
    G, Gp = {}, {}
    for n in rest:                                           # parents come first (reference skeleton order)
        par = rest[n][0]
        G[n] = (G[par] @ local(n)) if par else local(n)
        Gp[n] = (Gp[par] @ local(n, deltas.get(n))) if par else local(n, deltas.get(n))
    order = sorted(arm.pose.bones, key=lambda pb: len(pb.parent_recursive))
    for pb in order:
        if pb.name not in G: continue
        S = C @ Gp[pb.name] @ G[pb.name].inverted() @ Ci
        pb.matrix = Mw.inverted() @ S @ Mw @ pb.bone.matrix_local
        bpy.context.view_layer.update()
    bpy.ops.object.mode_set(mode="OBJECT")
    return arm


# --fp hold: FP skeleton space (x forward, y left, z up; the camera bone at the eyes): right hand on the grip, left
# hand on the fore-end, elbows down and out; two-bone IK per arm, the hand keeps its bind bend to the forearm
FP_HOLD = {"r": (Vector((0.30, -0.15, 1.40)), Vector((0.1, -0.6, -1.0))),
           "l": (Vector((0.52, -0.02, 1.46)), Vector((0.0, 0.7, -1.0)))}


def fp_hold():
    from mathutils import Matrix as M4
    a = next(x for x in bpy.data.objects if x.type == "ARMATURE")
    bpy.context.view_layer.objects.active = a
    bpy.ops.object.mode_set(mode="POSE")
    Mw = a.matrix_world
    cam_b = a.data.bones.get("camera")
    eye = Mw @ cam_b.head_local if cam_b else Vector((0, 0, 1.65))
    for sd, (tgt, pole) in FP_HOLD.items():
        names = [f"upperarm_{sd}", f"lowerarm_{sd}", f"hand_{sd}"]
        if not all(n in a.pose.bones for n in names): continue
        S, E, W = (Mw @ a.data.bones[n].head_local for n in names)
        T = tgt + (eye - Vector((0, 0, 1.65)))
        l1, l2 = (E - S).length, (W - E).length
        d = min((T - S).length, (l1 + l2) * 0.999)
        u = (T - S).normalized(); T = S + u * d
        x = (l1 * l1 - l2 * l2 + d * d) / (2 * d)
        h = math.sqrt(max(l1 * l1 - x * x, 0.0))
        pp = (pole - u * pole.dot(u)).normalized()
        E2 = S + u * x + pp * h
        R1 = (E - S).normalized().rotation_difference((E2 - S).normalized()).to_matrix().to_4x4()
        M1 = M4.Translation(S) @ R1 @ M4.Translation(-S)
        Wm = M1 @ W
        R2 = (Wm - E2).normalized().rotation_difference((T - E2).normalized()).to_matrix().to_4x4()
        M2 = M4.Translation(E2) @ R2 @ M4.Translation(-E2) @ M1
        for n, M in zip(names[:2], (M1, M2)):
            pb = a.pose.bones[n]
            pb.matrix = Mw.inverted() @ M @ Mw @ pb.bone.matrix_local
            bpy.context.view_layer.update()
    bpy.ops.object.mode_set(mode="OBJECT")
    return eye


FP_EYE = fp_hold() if opts["fp"] else None
FACE_ARM = face_pose(opts["face"], opts["face_pose"], opts["face_blink"]) if opts["face"] else None

for o in list(bpy.data.objects):                  # glTF importer bone-shape helpers
    if o.type == "MESH" and (o.name.startswith("Icosphere") or not o.users_scene or o.hide_render):
        bpy.data.objects.remove(o)
if opts["sim"]:
    # the cloth simulation meshes (b4bfit manifest extras "cloth", Blender metres) as red wire over the model
    import json
    red = bpy.data.materials.new("b4b_sim"); red.use_nodes = True
    bsdf = red.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (1, 0, 0, 1)
    bsdf.inputs["Emission Color"].default_value = (1, 0, 0, 1); bsdf.inputs["Emission Strength"].default_value = 3
    for k, sd in enumerate(json.load(open(opts["sim"])).get("extras", {}).get("cloth", [])):
        me = bpy.data.meshes.new(f"b4b_sim{k}")
        me.from_pydata([tuple(v) for v in sd["verts"]], [], [tuple(t) for t in sd["tris"]])
        ob = bpy.data.objects.new(f"b4b_sim{k}", me); bpy.context.scene.collection.objects.link(ob)
        me.materials.append(red)
        # no even offset: at the open panel's thin triangles it pushes the wire metres out (the framing then took
        # those spikes in and drew the model tiny)
        wf = ob.modifiers.new("wire", "WIREFRAME"); wf.thickness = 0.006; wf.use_even_offset = False
meshes = [o for o in bpy.data.objects if o.type == "MESH" and (not opts["focus"] or opts["focus"] in o.name)]
if opts["textures"]:
    import json, re
    tex = {k.lower(): v for k, v in json.load(open(opts["textures"])).items()}
    for o in meshes:
        for i, m in enumerate(o.data.materials):
            if m is None: continue
            p = tex.get(re.sub(r"\.\d{3}$", "", m.name).lower())
            if not p or not os.path.exists(p): continue
            nm = bpy.data.materials.new(m.name + "_prev"); nm.use_nodes = True
            b = nm.node_tree.nodes["Principled BSDF"]
            t = nm.node_tree.nodes.new("ShaderNodeTexImage"); t.image = bpy.data.images.load(p)
            if "hair" in p.lower() and "_mm_" in p.lower():        # hair multimask: A = strands, colour from the json
                info = p + ".json"
                col = json.load(open(info))["color_linear"] if os.path.exists(info) else (0.1, 0.07, 0.05)
                b.inputs["Base Color"].default_value = (*col, 1)
                nm.node_tree.links.new(t.outputs["Alpha"], b.inputs["Alpha"])
            else:
                nm.node_tree.links.new(t.outputs["Color"], b.inputs["Base Color"])
                if "haircolor_bc" in p.lower():                     # hair colour texture: A = strands (masked)
                    nm.node_tree.links.new(t.outputs["Alpha"], b.inputs["Alpha"])
            if not b.inputs["Alpha"].is_linked:
                t.image.alpha_mode = "CHANNEL_PACKED"   # A is a mask for the game's shader, not opacity (else black)
            b.inputs["Roughness"].default_value = 0.7
            nm.use_backface_culling = m.use_backface_culling
            o.data.materials[i] = nm
if FACE_ARM is not None:
    # one-sided materials as in the game (glTF doubleSided off): back faces transparent, so a mouth without an
    # inside shows the hole the game shows instead of the head's inner walls
    for m in {m for o in meshes for m in o.data.materials if m is not None and m.use_backface_culling and m.use_nodes}:
        nt = m.node_tree
        out_ = next((n for n in nt.nodes if n.type == "OUTPUT_MATERIAL"), None)
        if out_ is None or not out_.inputs["Surface"].links: continue
        src_sock = out_.inputs["Surface"].links[0].from_socket
        geo = nt.nodes.new("ShaderNodeNewGeometry"); tr = nt.nodes.new("ShaderNodeBsdfTransparent")
        mix = nt.nodes.new("ShaderNodeMixShader")
        nt.links.new(geo.outputs["Backfacing"], mix.inputs[0])
        nt.links.new(src_sock, mix.inputs[1]); nt.links.new(tr.outputs[0], mix.inputs[2])
        nt.links.new(mix.outputs[0], out_.inputs["Surface"])
# B4B skeleton: views relative to the model's facing (heroes face +X)
arm = next((a for a in bpy.data.objects if a.type == "ARMATURE" and all(n in a.data.bones for n in
                                                                          ("pelvis", "upperarm_l", "upperarm_r", "head"))), None)
face_rot = None
if arm is not None:
    bw = lambda n: arm.matrix_world @ arm.data.bones[n].head_local
    lat = (bw("upperarm_l") - bw("upperarm_r")).normalized()
    up = (bw("head") - bw("pelvis")); up = (up - lat * up.dot(lat)).normalized()
    fwd = lat.cross(up).normalized()
    # a camera "in front" looks from +fwd; the default "front" direction is -Y
    face_rot = Vector((0, -1, 0)).rotation_difference(fwd)
dg = bpy.context.evaluated_depsgraph_get()
pts = []
for o in meshes:
    if o.name.startswith("b4b_sim"): continue    # frame the model; the simulation wire is drawn over it
    ev = o.evaluated_get(dg)
    m = ev.to_mesh()
    pts += [o.matrix_world @ v.co for v in m.vertices]
    ev.to_mesh_clear()
lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
c = (lo + hi) / 2
ext_ = max(hi - lo) / float(opts["zoom"])
if FACE_ARM is not None:                          # close-up of the face
    hb = FACE_ARM.matrix_world @ FACE_ARM.data.bones["head"].head_local
    c = hb + (face_rot @ Vector((0, -0.09, 0.015)) if face_rot is not None else Vector((0, 0, 0.015)))
    look = {"mouth": ("lip_upper", "lip_lower"), "eyes": ("eye_l", "eye_r")}.get(opts["face_view"])
    fb = {b.name.lower(): b for b in FACE_ARM.data.bones}
    if look and all(n in fb for n in look):              # centred on those face bones (moved onto the face)
        c = sum((FACE_ARM.matrix_world @ fb[n].head_local for n in look), Vector()) / 2
    ext_ = 0.26 / float(opts["zoom"])
print("bounds", tuple(round(x, 3) for x in lo), tuple(round(x, 3) for x in hi))
if FP_EYE is not None and "--views" not in argv: opts["views"] = "fp"

scene = bpy.context.scene
scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = 16
scene.render.resolution_x = scene.render.resolution_y = size
scene.render.film_transparent = False
world = bpy.data.worlds.new("w"); scene.world = world
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (0.6, 0.6, 0.62, 1)
world.node_tree.nodes["Background"].inputs[1].default_value = 1.0
sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
sun.data.energy = 3.0
sun.rotation_euler = (math.radians(50), 0, math.radians(30))
scene.collection.objects.link(sun)
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
cam.data.type = "ORTHO"
cam.data.ortho_scale = ext_ * 1.1
scene.collection.objects.link(cam)
scene.camera = cam
dirs = {"front": Vector((0, -1, 0)), "back": Vector((0, 1, 0)), "side": Vector((1, 0, 0)),
        "left": Vector((-1, 0, 0)), "top": Vector((0, 0, 1)), "front3q": Vector((0.7, -0.7, 0.2)).normalized()}
tiles = []
base = os.path.splitext(out)[0]
for v in opts["views"].split(","):
    if v == "fp":                                 # the FP camera: at the camera bone, looking forward (+X), 90 deg
        cam.data.type = "PERSP"; cam.data.sensor_fit = "HORIZONTAL"; cam.data.angle = math.radians(90)
        cam.data.clip_start = 0.01
        cam.location = FP_EYE
        cam.rotation_euler = Vector((1, 0, 0)).to_track_quat("-Z", "Y").to_euler()
        sun.rotation_euler = Vector((-0.3, 0.3, 1.0)).normalized().to_track_quat("Z", "Y").to_euler()
        p = f"{base}_{v}.png"
        scene.render.filepath = p
        bpy.ops.render.render(write_still=True)
        tiles.append(p)
        continue
    d = dirs[v]
    if face_rot is not None and v != "top": d = face_rot @ d
    cam.location = c + d * ext_ * 3
    look = (c - cam.location).normalized()
    cam.rotation_euler = look.to_track_quat("-Z", "Y").to_euler()
    sun.rotation_euler = (look * -1 + Vector((0.3, -0.2, 0.6))).normalized().to_track_quat("Z", "Y").to_euler()
    p = f"{base}_{v}.png"
    scene.render.filepath = p
    bpy.ops.render.render(write_still=True)
    tiles.append(p)
# stitch
imgs = [bpy.data.images.load(p) for p in tiles]
import numpy as np
arrs = [np.array(im.pixels[:]).reshape(size, size, 4) for im in imgs]
full = np.concatenate(arrs, axis=1)
res = bpy.data.images.new("out", size * len(arrs), size)
res.pixels[:] = full.ravel()
res.filepath_raw = out; res.file_format = "PNG"; res.save()
for p in tiles: os.remove(p)
print("wrote", out)
