"""Secondary motion for survivor models fitted by b4bfit.py (3P): long hair that swings and skirts that sway.

The game gives heroes two kinds of it, and a custom mesh can use both without new blueprints:
  1. Hair on the survivor's physics bones. Some survivors have a ponytail/braid chain under `head` (Holly, Holly
     Elite 06, Walker Elite 03, Doc Elite 03: hair_00..02; Mom: hair_00_l/r..hair_02_l/r) whose bodies are
     PhysType_Simulated in the survivor's physics asset; the hero anim blueprint's RigidBody node simulates them.
     `rig_hair` skins the model's back hair to that chain (by height below its root, behind the head). `pin_hair`
     (every survivor) then gives long hair lying on the body a share of the torso under it, as much as its clearance
     needs, so it no longer sweeps through the back, bottom and legs with the head or the chain.
  2. Cloth. Skirts, dresses, coat tails and capes become cloth sections: `cloth_regions` separates their faces into
     objects the importer turns into their own sections, and builds a low-poly simulation mesh for each garment: a
     closed tube for skirts and closed coats, an open panel (around the back, the front left open) for open-front
     coats and capes; its top row is fixed (the waist, or the hips for a cape). modkit/cloth.py writes it
     into a clothing asset of the outfit (adding one if the outfit has none) and maps the render vertices onto it.
  3. --skirt legs instead of cloth: `skirt_legs` skins skirts, dresses and coat tails to the pelvis and thighs (each
     half follows its thigh, front and back shared), the way some game rigs hang their skirt bones under the thighs.
  4. Unrigged models: `drop_template_gear` keeps the survivor's gear bones (Holly's `backpack`) out of the weights they
     copy from its mesh.
How it works: docs/investigations/mesh-mods.md §14 (b4b-coop repository).
"""
import bmesh, math, os, re
from mathutils import Vector
from mathutils.kdtree import KDTree

HAIR_RX = re.compile(r"hair|fur\b|ponytail|pony_?tail|afro|bangs?\b|fringe|braid|\bbun\b|mane|wig|lekku|montral|"
                     r"head_?tails?", re.I)
NOT_HAIR_RX = re.compile(r"lash|brow(?!n)|eyeline|eye_?liner|iris|eye|beard|mustache|moustache|stubble", re.I)
# clothes that went to the hair slot for their alpha (VRoid's lace dress F00_002_01_Onepice_01_CLOTH): not hair
CLOTHES_RX = re.compile(r"cloth|dress|onepi|skirt|shirt|blouse|jacket|coat|pants|trouser|shoe|boot|sock|glove|sleeve",
                        re.I)
SKIRT_RX = re.compile(r"skirt|dress|kilt|gown|tutu", re.I)
BOTTOMS_RX = re.compile(r"bottom|lower|pants|trouser|shorts?\b|jean", re.I)
CLOTH_PREFIX = "B4BCLOTH_"


def smooth(e0, e1, x):
    t = max(0.0, min(1.0, (x - e0) / (e1 - e0))) if e1 != e0 else float(x >= e0)
    return t * t * (3 - 2 * t)


def weights_of(m):
    names = {g.index: g.name for g in m.vertex_groups}
    return [{names[g.group]: g.weight for g in v.groups if g.weight > 0} for v in m.data.vertices]


def set_weights(m, W):
    m.vertex_groups.clear()
    groups = {}
    for vi, d in enumerate(W):
        for n, w in d.items():
            if w <= 1e-4: continue
            g = groups.get(n) or groups.setdefault(n, m.vertex_groups.new(name=n))
            g.add([vi], w, "REPLACE")


def vertex_materials(m):
    mats = [re.sub(r"\.\d{3}$", "", x.name) if x else "" for x in m.data.materials]
    out = [""] * len(m.data.vertices)
    for p in m.data.polygons:
        n = mats[p.material_index] if p.material_index < len(mats) else ""
        for v in p.vertices: out[v] = n
    return out


# ---- 1. hair on the survivor's physics bones ------------------------------------------------------------------------

def chain_weights(s, n):
    """Tent weights along a chain of n bones for chain parameter s (0 = root joint, i = joint i, beyond n-1 = past the
    last joint): bone i owns the segment after its joint."""
    s = max(0.5, min(n - 0.5, s))
    w = [max(0.0, 1.0 - abs(s - (i + 0.5))) for i in range(n)]
    t = sum(w) or 1.0
    return [x / t for x in w]


def chain_param(z, zs):
    """Chain parameter of height z along joints at heights zs (hanging down: decreasing)."""
    if z >= zs[0]: return 0.0
    for i in range(len(zs) - 1):
        if zs[i] >= z >= zs[i + 1]:
            return i + (zs[i] - z) / max(1e-6, zs[i] - zs[i + 1])
    last = max(1e-3, zs[-2] - zs[-1]) if len(zs) > 1 else 0.05
    return len(zs) - 1 + (zs[-1] - z) / last


def chain_at(C, z):
    """Horizontal (x, y) of a hanging chain (joint positions, root first) at height z: between its joints by height,
    above the root the root's, below the last joint the last joint's (the hair hangs on straight down)."""
    s = chain_param(z, [c.z for c in C])
    i = int(min(len(C) - 1, max(0.0, s)))
    if i >= len(C) - 1: return C[-1].x, C[-1].y
    f = s - i
    return C[i].x + (C[i + 1].x - C[i].x) * f, C[i].y + (C[i + 1].y - C[i].y) * f


def smoothed(m, idx, val, iters=12):
    """val {vertex: 0..1} averaged over the mesh's edges among idx (vertices at the same place joined: UV seams split
    them), iters rounds: a share that changes across a hair card spreads along the card instead of stretching it."""
    nb = {i: set() for i in idx}
    for e in m.data.edges:
        a, b = e.vertices
        if a in nb and b in nb: nb[a].add(b); nb[b].add(a)
    at = {}
    for i in idx:
        at.setdefault(tuple(round(c, 5) for c in m.data.vertices[i].co), []).append(i)
    for same in at.values():
        for i in same: nb[i].update(j for j in same if j != i)
    v = dict(val)
    for _ in range(iters):
        v = {i: 0.5 * v[i] + 0.5 * sum(v[j] for j in nb[i]) / len(nb[i]) if nb[i] else v[i] for i in idx}
    return v


HAIR_REACH = 0.35      # m: an unrigged model's hair vertex further than this from the survivor's hair chain isn't hair


def head_bones(tpl):
    """The head and every bone below it (face bones), plus the neck."""
    kids = {}
    for b in tpl.arm.data.bones:
        if b.parent: kids.setdefault(b.parent.name, []).append(b.name)
    out, todo = {"neck_01", "neck_02"}, ["head"]
    while todo:
        n = todo.pop()
        if n in out: continue
        out.add(n); todo += kids.get(n, [])
    return out


def rig_hair(tpl, meshes, chains, is_hair_mat, log=print, swing=1.0, rigged=True):
    """Skin the back hair of `meshes` (template bind pose, Blender metres, heroes face +X) to the survivor's simulated
    hair chains (lists of bone names, root first). A vertex takes the chain in proportion to how far below the chain's
    root it hangs (full 7 cm below) and how far behind the head centre it is (none 3 cm in front), or how near it hangs
    to the chain (Mom's pigtail chains hang in front of the shoulders: a braid or lekku along them swings too, locks
    framing the face don't; smoothed over the mesh, so a card is not stretched between its near and far edge). Along
    the chain by height. Between two chains (hair down the middle of the back on Mom's pair) it takes both, blended by
    distance, so the two sides don't tear apart. swing scales that share (0..1). Only hair that hangs from the head: a
    material placed on the hair slot often carries more (anime rips put the stockings, bows and trims on the hair
    texture sheet), and the back of the legs or a bow on the back then swung with the ponytail. Rigged models:
    vertices mostly on the head (and neck); unrigged ones: within HAIR_REACH of the chain and not on a limb. Returns
    the number of vertices moved onto the chains."""
    chains = [[b for b in c if b in tpl.pos] for c in chains]
    chains = [c for c in chains if c]
    if not chains or "head" not in tpl.pos: return 0
    H = tpl.pos["head"]
    heads = head_bones(tpl)
    moved, total, skipped = 0, 0, 0
    for m in meshes:
        mats = vertex_materials(m)
        W = weights_of(m)
        hb = hair_by_bones(m)
        idx = []
        for v, mat in zip(m.data.vertices, mats):
            if not (is_hair_mat(mat) or (hb is not None and hb[v.index])): continue
            total += 1
            w0 = W[v.index]
            t0 = sum(w0.values()) or 1.0
            if rigged:
                if sum(x for n, x in w0.items() if n in heads) < 0.5 * t0:
                    skipped += 1; continue
            else:
                p = m.matrix_world @ v.co
                if min((p - tpl.pos[b]).length for c in chains for b in c) > HAIR_REACH or \
                        (w0 and LIMB_RX.match(max(w0, key=w0.get))):
                    skipped += 1; continue
            idx.append(v.index)
        if not idx: continue
        near, along = {}, {}
        for i in idx:
            p = m.matrix_world @ m.data.vertices[i].co
            nc = []
            for c in chains:
                C = [tpl.pos[b] for b in c]
                cx, cy = chain_at(C, p.z)
                nc.append((math.hypot(p.x - cx, p.y - cy), c, C))
            nc.sort(key=lambda t: t[0])
            near[i] = (p, nc)
            # hanging below the chain's root near the chain (within 5 cm, none from 8 cm), smoothed over the mesh
            along[i] = smooth(0.08, 0.05, nc[0][0]) if p.z < nc[0][2][0].z else 0.0
        along = smoothed(m, set(idx), along)
        changed = False
        for i in idx:
            p, nc = near[i]
            share = [(1.0, nc[0])]
            if len(nc) > 1:                         # blend the two nearest chains over 4 cm of distance difference
                t = smooth(-0.04, 0.04, nc[1][0] - nc[0][0])
                share = [(0.5 + 0.5 * t, nc[0]), (0.5 - 0.5 * t, nc[1])]
            a_back = max(smooth(0.03, -0.03, p.x - H.x), along[i])
            add = []
            for k, (dd, best, C) in share:
                a_low = smooth(C[0].z, C[0].z - 0.07, p.z)
                d = max(0.0, min(1.0, swing)) * a_back * a_low * k
                if d > 1e-3: add.append((d, best, chain_weights(chain_param(p.z, [c.z for c in C]), len(best))))
            dt = sum(d for d, _, _ in add)
            if dt <= 1e-3: continue
            w = {b: x * (1 - dt) for b, x in W[i].items()}
            for d, best, cw in add:
                for b, x in zip(best, cw):
                    if x > 0: w[b] = w.get(b, 0.0) + d * x
            W[i] = w
            moved += 1
            changed = True
        if changed: set_weights(m, W)
    log(f"hair: {moved} of {total} hair vertices on the survivor's physics hair bones "
        f"({'; '.join(','.join(c) for c in chains)})" + (f"; {skipped} left as they are: not hanging from the head "
                                                         f"(legs, bows or trims on the hair material)" if skipped else ""))
    return moved


# Source bones that carry hair (Fortnite dyn_C_hair_1, VRoid J_Sec_Hair1_01, MMD 髪): a vertex mostly on them is hair
# whatever its material is called (Fortnite rips put the hair on a "FaceAcc" material, next to the sunglasses)
HAIR_BONE_RX = re.compile(r"hair|ponytail|pony_?tail|braid|pigtail|twin_?tail|髪", re.I)
NOT_HAIR_BONE_RX = re.compile(r"band|clip|pin\b|tie\b|acc|ornament|ribbon|bow\b|flower", re.I)
HAIR_ATTR = "b4bhair"


def mark_hair_bones(objs, log=print):
    """Each source vertex's weight share on hair bones (HAIR_BONE_RX), kept as the vertex attribute HAIR_ATTR through
    the fit, which renames the source bones to the survivor's. Returns the number of vertices mostly on them."""
    n = 0
    for m in objs:
        if m.type != "MESH": continue
        hit = {g.index for g in m.vertex_groups if HAIR_BONE_RX.search(g.name) and not NOT_HAIR_BONE_RX.search(g.name)}
        if not hit: continue
        a = m.data.attributes.get(HAIR_ATTR) or m.data.attributes.new(HAIR_ATTR, "FLOAT", "POINT")
        for v in m.data.vertices:
            tot = sum(g.weight for g in v.groups)
            x = sum(g.weight for g in v.groups if g.group in hit) / tot if tot else 0.0
            a.data[v.index].value = x
            n += x >= 0.5
    if n: log(f"hair: {n} vertices on the model's own hair bones (counted as hair, whatever the material)")
    return n


def hair_by_bones(m):
    """[bool] per vertex: mostly on the source model's hair bones (mark_hair_bones) and not of a garment's or the eyes'
    material (a cape or collar weighted to the hair bones stays what it is), or None without that mark."""
    a = m.data.attributes.get(HAIR_ATTR)
    if a is None: return None
    mats = vertex_materials(m)
    worn = lambda n: bool(n and (NOT_HAIR_RX.search(n) or CLOTHES_RX.search(n) or CAPE_RX.search(n) or
                                 COAT_RX.search(n) or SKIRT_RX.search(n) or ACCESSORY_RX.search(n)))
    bad = {n: worn(n) for n in set(mats)}
    return [x.value >= 0.5 and not bad[n] for x, n in zip(a.data, mats)]


# ---- 1b. long hair lying on the body follows the body ---------------------------------------------------------------
# Hair skinned to the head (or to the physics hair chain behind it) moves rigidly with it: a vertex L metres below the
# pivot moves about L x angle when the head turns against the upper body (aiming up or down, looking round: ~30 deg in
# game) or the chain swings (hair_00..02 against the head: 10-50 deg measured live, running and turning). Hair that
# hangs down the back to the waist or the knees then sweeps 20-60 cm through the back, the bottom and the legs, while
# it lies only a few cm off them. So each hair vertex keeps only as much of the head/chain motion as its clearance from
# the body allows; the rest of it follows the torso bones of the body under it (spine, pelvis, clavicles): hair near the
# head and hair hanging far off the body still swings, hair lying on the back moves with the back.
TORSO_RX = re.compile(r"^(pelvis|spine_\d+|clavicle_[lr])$")
PIN_HEAD_DEG = 30.0      # head against the upper body
PIN_CHAIN_DEG = 40.0     # physics hair chain against the head
PIN_SLACK = 0.015        # m: hair may come this much closer than its clearance (it sinks into cloth/hair before it shows)


def pin_hair(tpl, meshes, is_hair_mat, log=print):
    """Long hair that lies on the body (back, shoulders, chest) follows the torso where it lies instead of swinging
    through it with the head or the physics hair chain (see above). is_hair_mat(material): hair material; vertices
    mostly on the model's own hair bones count too (hair_by_bones). Only hair that hangs from the head (mostly on head,
    neck, face or hair_* bones) is changed. Returns the number of vertices that got a share of the torso."""
    heads = head_bones(tpl)
    piv_h = tpl.pos.get("neck_01") or tpl.pos.get("head")
    if piv_h is None: return 0
    roots = {}
    for b in tpl.pos:                    # each hair_* bone's chain root (the topmost hair_* bone above it)
        if b.startswith("hair_"):
            m_ = re.match(r"^hair_\d+(_[lr])?$", b)
            roots[b] = tpl.pos.get("hair_00" + (m_.group(1) or "") if m_ else b, tpl.pos[b])
    moving = lambda b: b in heads or b.startswith("hair_")
    th, tc = math.radians(PIN_HEAD_DEG), math.radians(PIN_CHAIN_DEG)
    # the body: every vertex that isn't hair and isn't on the head (clothes included); the torso: those mostly on
    # torso bones, with those weights
    hairs = {}
    bpts, tpts, tws = [], [], []
    for m in meshes:
        if m.name.startswith(CLOTH_PREFIX): continue
        mats = vertex_materials(m)
        W = weights_of(m)
        hb = hair_by_bones(m)
        mw = m.matrix_world
        idx = []
        for v, w, mat in zip(m.data.vertices, W, mats):
            t = sum(w.values())
            if not t: continue
            hair = is_hair_mat(mat) or (hb is not None and hb[v.index])
            mov = sum(x for b, x in w.items() if moving(b)) / t
            if hair:
                if mov >= 0.5: idx.append(v.index)
                continue
            if mov >= 0.5: continue
            p = mw @ v.co
            bpts.append(p)
            tw = {b: x for b, x in w.items() if TORSO_RX.match(b)}
            if sum(tw.values()) >= 0.5 * t:
                s = sum(tw.values()); tpts.append(p); tws.append({b: x / s for b, x in tw.items()})
        if idx: hairs[m] = idx
    if not hairs or len(bpts) < 30 or len(tpts) < 10: return 0
    kb = KDTree(len(bpts))
    for i, p in enumerate(bpts): kb.insert(p, i)
    kb.balance()
    kt = KDTree(len(tpts))
    for i, p in enumerate(tpts): kt.insert(p, i)
    kt.balance()
    pinned, total, deep = 0, 0, 0.0
    for m, idx in hairs.items():
        W = weights_of(m)
        mw = m.matrix_world
        keep = {}
        for i in idx:
            p = mw @ m.data.vertices[i].co
            w = W[i]; t = sum(w.values())
            sh = sum(x for b, x in w.items() if b in heads) / t
            sc = sum(x for b, x in w.items() if b.startswith("hair_")) / t
            lc = 0.0
            if sc > 0:
                lc = sum(x * (p - roots[b]).length for b, x in w.items() if b in roots) / (sc * t)
            reach = (sh + sc) * (p - piv_h).length * th + sc * lc * tc        # how far it can move against the body
            clear = kb.find(p)[2]
            keep[i] = 1.0 if reach <= 1e-6 else min(1.0, (clear + PIN_SLACK) / reach)
        keep = smoothed(m, set(idx), keep)
        changed = False
        for i in idx:
            k = keep[i]
            total += 1
            if k > 0.98: continue
            p = mw @ m.data.vertices[i].co
            w = W[i]
            acc, sw = {}, 0.0
            for _, j, d in kt.find_n(p, 6):
                f = 1.0 / max(d, 1e-3); sw += f
                for b, x in tws[j].items(): acc[b] = acc.get(b, 0.0) + x * f
            mv = sum(x for b, x in w.items() if moving(b))
            nw = {b: (x * k if moving(b) else x) for b, x in w.items()}
            for b, x in acc.items(): nw[b] = nw.get(b, 0.0) + (1 - k) * mv * x / sw
            W[i] = nw
            pinned += 1; changed = True
            deep = max(deep, 1 - k)
        if changed: set_weights(m, W)
    if pinned:
        log(f"hair: {pinned} of {total} hanging hair vertices follow the body where they lie on it (back, shoulders), "
            f"up to {100 * deep:.0f}% (they would swing through it with the head or the hair chain)")
    return pinned


# ---- 2. skirts, coats and capes as cloth ---------------------------------------------------------------------------

LIMB_RX = re.compile(r"^(upperarm|lowerarm|hand|thumb|index|middle|ring|pinky|thigh|calf|foot|ball)_", re.I)
ARM_RX = re.compile(r"^(upperarm|lowerarm|hand|wrist|elbow|shoulder|clavicle|thumb|index|middle|ring|pinky)_", re.I)
COAT_RX = re.compile(r"coat|jacket|trench|duster|robe|parka|frock|poncho|tunic|apron", re.I)
CAPE_RX = re.compile(r"cape|cloak|mantle|shawl", re.I)


SKIRT_SLICE = 0.02      # m: the garment is cut by horizontal planes this far apart below the crotch
SKIRT_CLOSED = 0.6      # share of those cuts a skirt closes at its front and back edges (trousers: next to none)


def material_tris(meshes, name):
    """World-space triangles of material `name`'s faces (n-gons fanned)."""
    out = []
    for m in meshes:
        mats = [re.sub(r"\.\d{3}$", "", x.name) if x else "" for x in m.data.materials]
        mw, vs = m.matrix_world, m.data.vertices
        for p in m.data.polygons:
            if p.material_index >= len(mats) or mats[p.material_index] != name or not face_ok(m, name, p): continue
            c = [mw @ vs[v].co for v in p.vertices]
            out += [(c[0], c[k], c[k + 1]) for k in range(1, len(c) - 1)]
    return out


def plane_cut(tris, h):
    """The segments where the plane z = h cuts the triangles, as [((x, y), (x, y))]."""
    segs = []
    for t in tris:
        pts = []
        for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
            if (a.z - h) * (b.z - h) < 0:
                f = (h - a.z) / (b.z - a.z)
                pts.append((a.x + f * (b.x - a.x), a.y + f * (b.y - a.y)))
        if len(pts) == 2: segs.append(pts)
    return segs


def is_skirt_shaped(tris, tpl, why=None):
    """A skirt covers the gap between the legs below the crotch; trousers/shorts leave it open. Cut by horizontal
    planes below the crotch, a skirt crosses the middle line between the legs at the cut's front and back edges (its
    front and back panels) all the way down to its hem; trousers touch it at most with the inner thighs, in the middle
    of the cut (thick or close legs), and armour plates or a codpiece cover it for a few cuts at most. Planes cut the
    faces, not just the vertices: a low-poly skirt has no vertex on the middle line. why: a list that gets the numbers
    for the log."""
    if not tris or "thigh_l" not in tpl.pos: return False
    crotch = (tpl.pos["thigh_l"].z + tpl.pos["thigh_r"].z) / 2
    cy = (tpl.pos["thigh_l"].y + tpl.pos["thigh_r"].y) / 2
    low = min(min(v.z for v in t) for t in tris)
    tris = [t for t in tris if min(v.z for v in t) < crotch - 0.03]
    counted = closed = 0
    h = crotch - 0.03
    while h > low:
        segs = plane_cut(tris, h)
        h -= SKIRT_SLICE
        if not segs: continue
        counted += 1
        xs = [q[0] for s in segs for q in s]
        x0, x1 = min(xs), max(xs)
        edge = 0.25 * (x1 - x0)
        cross = [a[0] + (cy - a[1]) / (b[1] - a[1]) * (b[0] - a[0]) for a, b in segs
                 if (a[1] - cy) * (b[1] - cy) <= 0 and a[1] != b[1]]
        closed += any(x >= x1 - edge for x in cross) and any(x <= x0 + edge for x in cross)
    if why is not None: why.append(f"closed between the legs at {closed} of {counted} heights below the crotch")
    return counted > 0 and closed / counted >= SKIRT_CLOSED


def material_labels(meshes):
    """{material name: name + colour image file names} (game rips: 'Material #35 jacket.png')."""
    import os
    out = {}
    for m in meshes:
        for x in m.data.materials:
            if not x: continue
            n = re.sub(r"\.\d{3}$", "", x.name)
            lab = n
            if x.node_tree:
                imgs = {os.path.basename(nd.image.filepath or nd.image.name) for nd in x.node_tree.nodes
                        if nd.type == "TEX_IMAGE" and nd.image is not None
                        and nd.image.colorspace_settings.name != "Non-Color"
                        and not re.search(r"normal|_n\.|_nrm|mask|rough|metal|_ao|occlusion|_orm|spec|gloss|height|bump",
                                          nd.image.filepath or nd.image.name, re.I)}
                if imgs: lab += " " + " ".join(sorted(imgs))
            out[n] = lab
    return out


# --cloth MAT@BONES: only the faces of MAT that hang on source bones matching BONES (game rips mark their dangly parts
# with their own physics bones: Fortnite dyn_skirt_*, dyn_belt_*): {material: vertex attribute}. b4bfit writes the
# attribute (bone_attr) at import, before the source bones are renamed to the survivor's
BONE_FILTER = {}
# game rips' own garment physics bones (Fortnite dyn_skirt_*, dyn_skit_layer_*, dyn_coat_bk_*, dyn_cape_* ...): with
# --cloth auto, a material with enough faces on them gets those faces as cloth (MAT@GARMENT_BONES), whatever its name.
# Collars, lapels, sleeves, belts, hair and ruffles on the shoulders have their own dyn_ bones and are not matched
GARMENT_BONES = r"^dyn_(main_)?(skirt|skit|dress|gown|coat|cape|cloak|robe|poncho|kilt|tail)"   # = b4bmodel's


def bone_attr(rx):
    """The vertex attribute holding each vertex's weight share on source bones matching rx."""
    import hashlib
    return "b4bcb_" + hashlib.md5(rx.encode()).hexdigest()[:8]


def parse_cloth_spec(spec):
    """'MAT[@BONES][:cape|:lower],...' -> [(material lower-case, bones regex or None, kind or None)]."""
    out = []
    for x in (spec or "").split(","):
        x = x.strip()
        if not x or x in ("auto", "on", "off"): continue
        k = None
        if re.search(r":(cape|lower)$", x, re.I): x, k = x.rsplit(":", 1)
        rx = None
        if "@" in x: x, rx = x.split("@", 1)
        out.append((x.lower(), rx or None, k.lower() if k else None))
    return out


def face_ok(m, name, poly):
    """A face of material `name` counts for it (BONE_FILTER: most of its vertices on the listed bones)."""
    attr = BONE_FILTER.get(name)
    if not attr: return True
    a = m.data.attributes.get(attr)
    if a is None: return False
    return sum(a.data[v].value for v in poly.vertices) / len(poly.vertices) >= 0.5


def garment_kind(pts, tpl):
    """'cape' when the garment hangs from the shoulders behind the body and leaves the chest bare, else 'lower'
    (skirts, dresses, coats: simulated from the waist down)."""
    if not pts or "spine_03" not in tpl.pos: return "lower"
    pel, sp3 = tpl.pos["pelvis"], tpl.pos["spine_03"]
    chest = [p for p in pts if sp3.z - 0.12 < p.z < sp3.z + 0.05]
    if max(p.z for p in pts) < sp3.z or len(chest) < 10: return "lower"
    front = sum(1 for p in chest if p.x > sp3.x + 0.03)
    return "cape" if front / len(chest) < 0.1 and min(p.z for p in pts) < pel.z else "lower"


# worn things that are never skirts even when their material says "dress" (<Name>_Dress_Necklace, a belt or thigh
# strap cut from the dress's texture sheet, boots whose material is called "dress")
ACCESSORY_RX = re.compile(r"necklace|pendant|jewel|chain|choker|collar|brooch|buckle|belt|strap|bracelet|armband|"
                          r"bangle|earring|panties|underwear|\bbra\b|thong|bikini|lingerie|sock|stocking|shoe|boot|heel|"
                          r"sandal|glove|garter|harness|holster|pouch|bag\b", re.I)
HANG_ABOVE = 0.03        # m: a skirt starts at least this far above the crotch (it hangs from the waist)
HANG_BELOW = 0.12        # m: ... and reaches at least this far below it (over the legs; as cloth_regions' minimum)
HANG_WIDTH = 0.10        # m: ... and is this wide there (a strand of beads or a chain is not a skirt)


def garment_groups(meshes, name):
    """{object name: [world points]} of material `name`'s faces per object, faces skinned to the arms left out (a
    coat's sleeves, bracelets)."""
    out = {}
    for m in meshes:
        mi = {i for i, x in enumerate(m.data.materials) if x and re.sub(r"\.\d{3}$", "", x.name) == name}
        if not mi: continue
        W = weights_of(m)
        arm = [sum(x for b, x in w.items() if ARM_RX.match(b)) / (sum(w.values()) or 1.0) for w in W]
        mw = m.matrix_world
        pts = []
        for p in m.data.polygons:
            if p.material_index in mi and sum(arm[v] for v in p.vertices) / len(p.vertices) < 0.5 and \
                    face_ok(m, name, p):
                pts += [mw @ m.data.vertices[v].co for v in p.vertices]
        if pts: out[m.name] = pts
    return out


def hang_check(pts, tpl, kind):
    """None when the points hang like a garment of that kind, else why not (for the log)."""
    top, bot = max(p.z for p in pts), min(p.z for p in pts)
    if kind == "cape":
        return None if top - bot >= HANG_BELOW else f"only {100 * (top - bot):.0f} cm tall"
    if "thigh_l" not in tpl.pos or "thigh_r" not in tpl.pos: return None
    crotch = (tpl.pos["thigh_l"].z + tpl.pos["thigh_r"].z) / 2
    knee = (tpl.pos["calf_l"].z + tpl.pos["calf_r"].z) / 2 if "calf_l" in tpl.pos and "calf_r" in tpl.pos else None
    if top < crotch + HANG_ABOVE:
        where = "on the feet" if knee is not None and top < knee else "on the legs"
        return f"sits {where} (top {100 * top:.0f} cm, crotch {100 * crotch:.0f} cm): nothing hangs from the waist"
    if bot > crotch - HANG_BELOW:
        return (f"ends {100 * (bot - crotch):+.0f} cm from the crotch (bottom {100 * bot:.0f} cm): it doesn't hang over "
                f"the legs")
    below = [p for p in pts if p.z < crotch - 0.03]
    w = max(max(p.x for p in below) - min(p.x for p in below), max(p.y for p in below) - min(p.y for p in below))
    if w < HANG_WIDTH:
        return f"only {100 * w:.0f} cm wide below the crotch (a strand, chain or tassel)"
    return None


def cloth_materials(meshes, tpl, spec, log=print, cloth_ok=None):
    """({material: kind}, {material: object names}) to simulate. spec 'auto': named (material or colour image) like a
    skirt/dress/coat/cape, or like bottoms and skirt-shaped; else a comma list of material names, each optionally
    ':cape' / ':lower' to force how it hangs. kind 'lower' = simulated below the waist (skirts, dresses, coat tails),
    'cape' = below the hips, hanging behind.
    Names alone aren't trusted: each object's part of a garment material must hang like one (from above the crotch to
    well below it, wide enough; hang_check), and accessories are left out by name (necklace, belt, strap, boots ...;
    ACCESSORY_RX), so a necklace or a thigh strap cut from the dress's texture sheet stays skinned. In auto mode a
    material with no such part is not simulated (the log says why and how to force it); a listed material keeps the
    parts that pass, or all of them if none does. cloth_ok(material) False: its slot can't draw cloth (skipped)."""
    labels = material_labels(meshes)
    names = set()
    for m in meshes:
        names.update(x for x in vertex_materials(m) if x)
    out = {}
    forced = {}
    BONE_FILTER.clear()
    parts = [x.strip() for x in (spec or "").split(",") if x.strip()]
    auto = not parts or parts[0] in ("auto", "on")
    if not auto or len(parts) > 1:
        # a list of materials; after "auto," (b4bmodel: materials with their own garment bones) they come on top of
        # what auto finds by name
        want, bones = {}, {}
        for x, rx, k in parse_cloth_spec(spec):
            want[x] = k
            if rx: bones[x] = rx
        for n in names:
            if n.lower() in want:
                out[n] = want[n.lower()]; forced[n] = True
                if n.lower() in bones:
                    BONE_FILTER[n] = bone_attr(bones[n.lower()])
                    got = sum(1 for m in meshes for p in m.data.polygons
                              if p.material_index < len(m.data.materials) and m.data.materials[p.material_index]
                              and re.sub(r"\.\d{3}$", "", m.data.materials[p.material_index].name) == n
                              and face_ok(m, n, p))
                    log(f"cloth: {n!r}: only its faces on the source bones /{bones[n.lower()]}/ ({got} faces)")
        missing = set(want) - {n.lower() for n in out}
        if missing: log(f"cloth: no material {sorted(missing)} (materials: {sorted(names)})")
        if auto:
            for n in out: forced[n] = False                 # found by its bones: it must still hang like a garment
    if auto:
        for n in sorted(names):
            if n in out: continue
            lab = labels.get(n, n)
            if CAPE_RX.search(lab): out[n] = "cape"; continue
            if SKIRT_RX.search(lab) or COAT_RX.search(lab):
                if ACCESSORY_RX.search(n):
                    log(f"cloth: {n!r} is named like an accessory ({ACCESSORY_RX.search(n).group(0)}): not simulated "
                        f"(if it is a skirt or coat: --cloth {n})")
                    continue
                out[n] = None; continue
            if BOTTOMS_RX.search(lab):
                why = []
                if is_skirt_shaped(material_tris(meshes, n), tpl, why):
                    out[n] = "lower"
                    log(f"cloth: {n!r} named like bottoms and shaped like a skirt ({why[0]})")
                else: log(f"cloth: {n!r} looks like trousers/shorts (open between the legs{': ' + why[0] if why else ''}): "
                          f"not simulated (if it is a skirt: --cloth {n})")
    objs = {}
    for n in list(out):
        if cloth_ok is not None and not cloth_ok(n):
            log(f"cloth: {n!r} is on a slot whose material can't draw cloth (bUsedWithClothing off, e.g. a skin slot): "
                f"skinned, not simulated; put it on a clothing slot (--slot {n}=<Body/Torso/...>) to make it cloth")
            del out[n]; continue
        groups = garment_groups(meshes, n)
        if not groups:
            del out[n]; continue
        kind = out[n] or garment_kind([p for ps in groups.values() for p in ps], tpl)
        ok, why = {}, {}
        for ob, pts in groups.items():
            acc = ACCESSORY_RX.search(ob)
            if acc and not SKIRT_RX.search(ob) and not COAT_RX.search(ob) and not CAPE_RX.search(ob):
                why[ob] = f"named like an accessory ({acc.group(0)})"; continue
            w = hang_check(pts, tpl, kind)
            if w: why[ob] = w
            else: ok[ob] = pts
        if not ok and forced.get(n):
            log(f"cloth: {n!r} (--cloth): no part hangs like a garment ({'; '.join(f'{k}: {v}' for k, v in why.items())}); "
                f"simulated as asked")
            ok, why = groups, {}
        if not ok:
            log(f"cloth: {n!r} named like a garment but doesn't hang like one: "
                + "; ".join(f"{k}: {v}" for k, v in sorted(why.items())) + f": skinned, not simulated (if it is a skirt, "
                f"dress or coat: --cloth {n})")
            del out[n]; continue
        for ob, w in sorted(why.items()):
            log(f"cloth: {n!r} on {ob} stays skinned: {w}")
        out[n] = out[n] or garment_kind([p for ps in ok.values() for p in ps], tpl)
        objs[n] = set(ok)
    return out, objs


def cut_region(tpl, meshes, mats, cap, tag, log=print, dry=False, keep=None, objs=None):
    """Split the faces of `mats` below height `cap` off into new objects named <tag>... Returns (pieces, points,
    inward fraction) where the inward fraction is the share of faces facing the body axis (a lining). dry: only the
    points, nothing split. objs: {material: object names} the material is cut from (others keep it skinned)."""
    pieces, pts, inward, nf = [], [], 0, 0
    axis = tpl.pos["pelvis"]
    for m in list(meshes):
        mi = {i for i, x in enumerate(m.data.materials) if x and re.sub(r"\.\d{3}$", "", x.name) in mats
              and (objs is None or m.name in objs.get(re.sub(r"\.\d{3}$", "", x.name), ()))}
        if not mi: continue
        bm = bmesh.new(); bm.from_mesh(m.data)
        mw = m.matrix_world
        nm = mw.to_3x3().inverted().transposed()
        # a coat's sleeves hang below the waist too: faces skinned to the arms stay skinned
        W = weights_of(m)
        arm = [sum(x for b, x in w.items() if ARM_RX.match(b)) / (sum(w.values()) or 1.0) for w in W]
        names = [re.sub(r"\.\d{3}$", "", x.name) if x else "" for x in m.data.materials]
        polys = m.data.polygons
        sel = [f for f in bm.faces if f.material_index in mi and (mw @ f.calc_center_median()).z < cap
               and sum(arm[v.index] for v in f.verts) / len(f.verts) < 0.5
               and face_ok(m, names[f.material_index], polys[f.index])
               and (keep is None or keep(mw @ f.calc_center_median()))]
        if not sel: bm.free(); continue
        for f in sel:
            for v in f.verts: pts.append(mw @ v.co)
            c = mw @ f.calc_center_median(); n = nm @ f.normal
            nf += 1
            if n.x * (c.x - axis.x) + n.y * (c.y - axis.y) < 0: inward += 1
        if dry: bm.free(); continue
        c = m.copy(); c.data = m.data.copy(); c.name = tag + m.name
        for col in m.users_collection: col.objects.link(c)
        cut = {f.index for f in sel}
        bm2 = bmesh.new(); bm2.from_mesh(c.data)
        bm2.faces.ensure_lookup_table()
        bmesh.ops.delete(bm2, geom=[f for f in bm2.faces if f.index not in cut], context="FACES")
        bm2.to_mesh(c.data); bm2.free()
        bmesh.ops.delete(bm, geom=sel, context="FACES")
        bm.to_mesh(m.data); bm.free()
        pieces.append(c)
    return pieces, pts, inward / max(1, nf)


def add_back_faces(pieces, log=print, inset=0.0015):
    """One-sided garments show nothing from inside once they swing open: give every face a reversed copy 1.5 mm
    inside (normals pointing inward), so the inside is drawn."""
    n = 0
    for c in pieces:
        me = c.data
        corner = [tuple(x.vector) for x in me.corner_normals]           # the originals' normals (custom or not)
        nf, nl = len(me.polygons), len(me.loops)
        bm = bmesh.new(); bm.from_mesh(me)
        orig = list(bm.faces)
        # the inset direction per point, not per vertex: a flat-shaded garment is split at every corner, and copies
        # moved along each face's own normal came apart (a torn inside, and LODs that can't weld it)
        key = {v: (round(v.co.x * 1e5), round(v.co.y * 1e5), round(v.co.z * 1e5)) for v in bm.verts}
        acc = {}
        for v in bm.verts:
            acc[key[v]] = acc.get(key[v], Vector()) + v.normal
        vn = {v: (acc[key[v]].normalized() if acc[key[v]].length > 1e-9 else v.normal.copy()) for v in bm.verts}
        dup = bmesh.ops.duplicate(bm, geom=orig)
        new_faces = [g for g in dup["geom"] if isinstance(g, bmesh.types.BMFace)]
        for v_old, v_new in dup["vert_map"].items():
            if isinstance(v_old, bmesh.types.BMVert) and v_old in vn:
                v_new.co = v_old.co - vn[v_old] * inset
        bmesh.ops.reverse_faces(bm, faces=new_faces)
        bm.to_mesh(me); bm.free()
        # originals keep their normals (same loop order: bmesh writes the old faces first); copies get their own
        # (inward) vertex normals
        loops = corner[:nl] + [tuple(me.vertices[me.loops[li].vertex_index].normal) for li in range(nl, len(me.loops))]
        me.normals_split_custom_set(loops)
        n += nf
    log(f"cloth: one-sided garment: {n} faces got a reversed copy (the inside shows when it swings)")


def circ_runs(flags):
    """Circular runs of True in a list: [(start, length)]."""
    n = len(flags)
    if all(flags): return [(0, n)]
    s = next(i for i in range(n) if not flags[i])
    runs, i = [], 0
    while i < n:
        j = (s + i) % n
        if flags[j]:
            k = 0
            while k < n and flags[(j + k) % n]: k += 1
            runs.append((j, k)); i += k
        else:
            i += 1
    return runs


SIM_SPACING = 0.05       # m: target particle spacing of a simulation mesh (see build_sim)
SIM_MAX_VERTS = 480
SIM_RADIUS_Q = 0.5       # the simulation surface's radius: this quantile of the garment's points per cell (the render surface on both sides of it)
SIM_LIFT = 0.004         # m: ... plus this


def build_sim(tpl, pts, kind, top, log=print, rows=None):
    """Simulation mesh for one garment (Blender metres, heroes face +X). A closed tube when the garment goes all the way
    round, else an open panel over the covered arc (the front of an open coat, a cape's front left out). Rows from
    `top` (fixed) to the hem, which each column takes from the garment (a coat longer at the back)."""
    centre_bone = "spine_03" if kind == "cape" and "spine_03" in tpl.pos else "pelvis"
    cx, cy = tpl.pos[centre_bone].x, tpl.pos[centre_bone].y
    if kind != "cape":
        xs = [p.x for p in pts]; ys = [p.y for p in pts]
        cy = (min(ys) + max(ys)) / 2                      # left/right: the garment's middle; front/back: the body's
        cx = (cx + (min(xs) + max(xs)) / 2) / 2
    hem = min(p.z for p in pts) - 0.01
    L = top - hem
    ang = lambda p: math.atan2(p.y - cy, p.x - cx) % (2 * math.pi)
    # angular coverage over height bands: a front left open shows as sectors empty in most bands
    NB, NH = 72, 10
    seen = [set() for _ in range(NB)]
    for p in pts:
        seen[int(ang(p) / (2 * math.pi) * NB) % NB].add(min(NH - 1, int((top - p.z) / max(L, 1e-6) * NH)))
    cover = [len(s) / NH for s in seen]
    gaps = [r for r in circ_runs([c < 0.34 for c in cover]) if r[1] < NB]
    gap = max(gaps, key=lambda r: r[1]) if gaps else None
    closed = gap is None or gap[1] * 360 / NB < 25
    if closed:
        a0, span = 0.0, 2 * math.pi
    else:
        gs, gl = gap
        # the covered arc: from the gap's end round to its start (the few points inside the gap, e.g. where the
        # fronts meet at the waist, map onto the edge columns), half a bin of margin on both sides
        binw = 2 * math.pi / NB
        a0 = (gs + gl) * binw - binw / 2
        span = (NB - gl) * binw + binw
    # density: NvCloth collides the body's capsules with the particles only, so a leg slips through a garment whose
    # particles are further apart than the leg is thick (a calf near the ankle: ~10 cm). Particles about SIM_SPACING
    # apart at the garment's widest (retail long coats and skirts: 4-5 cm, 240-280 particles), at most SIM_MAX_VERTS
    sp = float(os.environ.get("B4B_CLOTH_SPACING", SIM_SPACING))
    rmax = max(math.hypot(p.x - cx, p.y - cy) for p in pts)
    circ = span * rmax
    for _ in range(20):
        cols = max(5 if not closed else 12, int(round(circ / sp)) + (0 if closed else 1))
        rows_ = rows or max(5, int(round(L / sp)) + 1)
        if cols * rows_ <= SIM_MAX_VERTS: break
        sp *= 1.1
    rows = rows_
    col_ang = [a0 + span * j / (cols if closed else cols - 1) for j in range(cols)]

    def col_of(p):
        r = (ang(p) - a0) % (2 * math.pi)
        if closed: return int(round(r / span * cols)) % cols
        if r > span + (2 * math.pi - span) / 2: r -= 2 * math.pi       # just before the arc's start
        return max(0, min(cols - 1, int(round(r / span * (cols - 1)))))
    # per-column hem, smoothed with the neighbours
    bot = [None] * cols
    for p in pts:
        j = col_of(p)
        if bot[j] is None or p.z < bot[j]: bot[j] = p.z
    for j in range(cols):
        if bot[j] is None: bot[j] = hem
    sm = []
    for j in range(cols):
        nb = [bot[j]] + [bot[(j + d) % cols] for d in (-1, 1) if closed or 0 <= j + d < cols]
        sm.append(min(min(nb) + 0.02, bot[j]) - 0.01)
    bot = [max(hem, min(b, top - 0.08)) for b in sm]
    zs = lambda k, j: top - (top - bot[j]) * k / (rows - 1)
    # the simulation surface follows the garment's inner side (a low quantile of its points' distance from the axis in
    # each cell and the rows next to it): the render surface is mapped at an offset along the normals, and what lies
    # inside the particles goes into a leg that the particles themselves are held off (the outermost points, as
    # before, left a pleated or layered dress 5-10 cm inside its particles)
    q = float(os.environ.get("B4B_CLOTH_RADIUS_Q", SIM_RADIUS_Q))
    cell = [[[] for _ in range(cols)] for _ in range(rows)]
    pk = []
    for p in pts:
        j = col_of(p)
        k = int(round((top - p.z) / max(1e-6, top - bot[j]) * (rows - 1)))
        r = math.hypot(p.x - cx, p.y - cy)
        pk.append((k, j, r))
        for kk in (k - 1, k, k + 1):
            if 0 <= kk < rows: cell[kk][j].append(r)
    R = [[None] * cols for _ in range(rows)]
    for k in range(rows):
        for j in range(cols):
            c = sorted(cell[k][j])
            if c: R[k][j] = c[min(len(c) - 1, int(q * len(c)))]
    for k in range(rows):
        filled = [j for j in range(cols) if R[k][j] is not None]
        for j in range(cols):
            if R[k][j] is not None: continue
            if filled:
                jj = min(filled, key=lambda f: min((f - j) % cols, (j - f) % cols) if closed else abs(f - j))
                R[k][j] = R[k][jj]
            else:
                R[k][j] = R[k - 1][j] if k else 0.12
    # how far the garment lies inside its particles (a capsule must hold the particles that much further out)
    inside = sorted(max(0.0, R[min(rows - 1, max(0, k))][j] + SIM_LIFT - r) for k, j, r in pk)
    verts, depth = [], []
    for k in range(rows):
        for j in range(cols):
            a = col_ang[j]; r = R[k][j] + SIM_LIFT
            verts.append(Vector((cx + r * math.cos(a), cy + r * math.sin(a), zs(k, j))))
            depth.append(k / (rows - 1))
    tris = []
    for k in range(rows - 1):
        for j in range(cols if closed else cols - 1):
            a, b = k * cols + j, k * cols + (j + 1) % cols
            c, d = a + cols, b + cols
            tris += [(a, b, d), (a, d, c)]
    shape = "tube" if closed else f"open panel over {math.degrees(span):.0f} deg"
    return {"verts": [list(v) for v in verts], "tris": tris, "depth": depth, "rows": rows, "segments": cols,
            "closed": closed, "length_m": L, "centre": [cx, cy], "shape": shape, "arc": [a0, span],
            "inside": inside[int(0.9 * (len(inside) - 1))] if inside else 0.0}


SIM_BONES = {"lower": ({"pelvis", "spine_01", "spine_02", "thigh_l", "thigh_r"}, ("pelvis", "spine_01", "spine_02")),
             "cape": ({"spine_01", "spine_02", "spine_03", "clavicle_l", "clavicle_r", "neck_01"},
                      ("spine_03", "clavicle_l", "clavicle_r", "neck_01"))}


SEAM_BONE_RX = re.compile(r"^(pelvis|spine_\d+|clavicle_[lr]|neck_\d+|thigh_[lr]|thigh_twist_\d+_[lr])$")


def sim_weights(tpl, verts, kind, cols, meshes=None):
    """Skinning of the simulation vertices: the template body's weights near each (hips/legs for skirts and coats,
    the upper back for capes); the fixed top row only on the waist / shoulder-blade bones, or, where the garment was
    cut out of a surface that goes on above it (a dress's skirt under its bodice), the weights of the model's own
    skinned vertices at the seam: the cloth's edge then moves exactly like the skinned edge it meets (else they
    drift apart and the seam tears open)."""
    allowed, top_bones = SIM_BONES[kind]
    seam = None
    if meshes:
        spts, sw = [], []
        for m in meshes:
            if m.name.startswith(CLOTH_PREFIX): continue
            mw = m.matrix_world
            for v, w in zip(m.data.vertices, weights_of(m)):
                # only body at the seam: a braid or a hand hanging by the waist must not drive the cloth's edge
                b = {n: x for n, x in w.items() if SEAM_BONE_RX.match(n)}
                t, tb = sum(w.values()), sum(b.values())
                if t and tb >= 0.8 * t: spts.append(mw @ v.co); sw.append(b)
        if spts:
            seam = KDTree(len(spts))
            for i, p in enumerate(spts): seam.insert(p, i)
            seam.balance()
    kd_pts, kd_w = [], []
    for tm in tpl.meshes:
        for v, w in zip(tm.data.vertices, weights_of(tm)):
            ww = {n: x for n, x in w.items() if n in allowed}
            if sum(ww.values()) > 0.5:
                kd_pts.append(tm.matrix_world @ v.co); kd_w.append(ww)
    kd = KDTree(len(kd_pts))
    for i, p in enumerate(kd_pts): kd.insert(p, i)
    kd.balance()
    out = []
    for k, v in enumerate(verts):
        acc = {}
        for co, i, dist in kd.find_n(Vector(v), 6):
            f = 1.0 / max(dist, 1e-3)
            for n, x in kd_w[i].items(): acc[n] = acc.get(n, 0.0) + x * f
        if k < cols:
            near = [(i, d) for _, i, d in seam.find_n(Vector(v), 8)] if seam else []
            near = [(i, d) for i, d in near if d < 0.04]
            if near:
                acc = {}
                for i, d in near:
                    f = 1.0 / max(d, 1e-3)
                    for n, x in sw[i].items(): acc[n] = acc.get(n, 0.0) + x * f
            else:
                acc = {n: x for n, x in acc.items() if n in top_bones} or {top_bones[0]: 1.0}
        top4 = sorted(acc.items(), key=lambda x: -x[1])[:4]
        t4 = sum(x for _, x in top4) or 1.0
        out.append({n: x / t4 for n, x in top4})
    return out


def limb_family(b):
    """thigh_twist_01_l / hip_twist_01_l -> thigh_l, calf_twist_01_r / knee_twist_01_r -> calf_r, else b."""
    m = re.match(r"^(thigh|hip|calf|knee)_twist_\d+_([lr])$", b)
    if not m: return b
    return ("thigh_" if m.group(1) in ("thigh", "hip") else "calf_") + m.group(2)


COLLIDE_MARGIN = 0.01    # m: a capsule reaches this far past the limb's outer skin (the cloth's particles hold it there,
                         # the render surface between them sags a little toward the limb)
CLEAR_MARGIN = 0.015     # m: ... but stays this far inside the garment at rest (else the cloth is pushed out into a bell)


def body_capsules(tpl, meshes, log=print, skip_mats=()):
    """Collision capsules fitted to the model's own legs and body (Blender metres): per bone the segment to its child
    joint, tapered: each end's radius covers the skin around that half of the bone (vertices mostly weighted to it or
    its twist bones; 90th percentile of their distance from the bone line, + COLLIDE_MARGIN: a limb isn't centred on
    its bone, the calf and the thigh's back stick out). Garment faces don't count: whole garment materials, or with
    --cloth MAT@BONES only MAT's faces on those bones (a game rip's body material holds the legs and the dress).
    A leg without skin of its own (hidden under a long dress and left out of the model) takes the other leg's.
    fit_capsules then fits them to each garment. For the clothing asset's collision (cloth.py)."""
    segs = {"pelvis": ("pelvis", "spine_01"), "spine_01": ("spine_01", "spine_02"), "spine_02": ("spine_02", "spine_03"),
            "spine_03": ("spine_03", "neck_01"), "thigh_l": ("thigh_l", "calf_l"), "thigh_r": ("thigh_r", "calf_r"),
            "calf_l": ("calf_l", "foot_l"), "calf_r": ("calf_r", "foot_r")}
    dist = {b: ([], []) for b in segs}                  # distances along the first / second half of the bone
    skipped = 0
    for m in meshes:
        if m.name.startswith(CLOTH_PREFIX): continue
        mats = vertex_materials(m)
        filt = {n: m.data.attributes.get(BONE_FILTER[n]) for n in skip_mats if n in BONE_FILTER}
        for i, (v, w, mat) in enumerate(zip(m.data.vertices, weights_of(m), mats)):
            if not w: continue
            if mat in skip_mats:                        # garments hanging off the body don't count
                if mat not in filt or (filt[mat] is not None and filt[mat].data[i].value >= 0.5):
                    skipped += 1; continue
            acc = {}
            for n, x in w.items(): acc[limb_family(n)] = acc.get(limb_family(n), 0.0) + x
            b, x = max(acc.items(), key=lambda kv: kv[1])
            if b not in segs or x < 0.6 * sum(acc.values()): continue
            a, c = segs[b]
            if a not in tpl.pos or c not in tpl.pos: continue
            A, C = tpl.pos[a], tpl.pos[c]
            p = m.matrix_world @ v.co
            e = C - A; t = max(0.0, min(1.0, (p - A).dot(e) / max(e.length_squared, 1e-9)))
            if 0.05 < t < 0.95: dist[b][0 if t < 0.5 else 1].append((p - (A + e * t)).length)
    caps, borrowed = [], []
    q = lambda d, f: sorted(d)[int(f * (len(d) - 1))]
    for b in segs:
        d0, d1 = dist[b]
        if len(d0) + len(d1) < 30 and b[-2:] in ("_l", "_r"):
            o = b[:-1] + ("r" if b.endswith("l") else "l")
            if len(dist[o][0]) + len(dist[o][1]) >= 30: d0, d1 = dist[o]; borrowed.append(b)
        if len(d0) + len(d1) < 30: continue
        both = d0 + d1
        r_in = q(both, 0.5) * 0.95                      # the skin's median distance, a little inside (the old radius)
        ends = [q(d, 0.9) + COLLIDE_MARGIN if len(d) >= 10 else q(both, 0.9) + COLLIDE_MARGIN for d in (d0, d1)]
        a, c = segs[b]
        caps.append({"bone": b, "a": list(tpl.pos[a]), "b": list(tpl.pos[c]), "ra": max(r_in, ends[0]),
                     "rb": max(r_in, ends[1]), "r": max(r_in, *ends), "r_min": r_in})
    caps += hair_capsules(tpl, meshes)
    if caps:
        log("cloth: collision capsules " + ", ".join(f"{c['bone']} r{c.get('ra', c['r']) * 100:.0f}"
                                                    + (f"-{c['rb'] * 100:.0f}" if 'rb' in c else '') for c in caps)
            + " cm" + (f" ({', '.join(borrowed)} from the other leg: no skin of its own)" if borrowed else ""))
    return caps


def fit_capsules(caps, sim, kind, log=print):
    """The capsules one garment collides with: those that reach its height (a skirt: legs, hips; a cape: back too), each
    end's radius grown by how far the garment lies inside its particles (sim["inside"]), then held CLEAR_MARGIN inside
    the garment's free simulation vertices at rest (a capsule wider than the garment there would push it out at once;
    tight skirts), never below the skin it covers (body_capsules' radius)."""
    V = [Vector(v) for v in sim["verts"]]
    grow = min(0.04, sim.get("inside", 0.0))          # the garment's part inside its particles must clear the skin too
    free = [V[i] for i, d in enumerate(sim["depth"]) if d > 0]
    top = max(v.z for v in V)
    out, fitted = [], []
    for c in caps:
        A, B = Vector(c["a"]), Vector(c["b"])
        if min(A.z, B.z) > top + 0.05: continue          # above the garment (a skirt and the chest)
        if kind == "lower" and c["bone"] in ("spine_02", "spine_03"): continue
        c = dict(c)
        floor = {k: c[k] for k in ("ra", "rb") if k in c}   # the skin itself stays covered, whatever the garment does
        if grow and "ra" in c and not c["bone"].startswith("hair_"):
            c["ra"] += grow; c["rb"] += grow
        e = B - A
        for end, P in (("ra", A), ("rb", B)):
            if end not in c: continue
            # the free vertices around this end's half of the bone
            near = []
            for v in free:
                t = (v - A).dot(e) / max(e.length_squared, 1e-9)
                if (0.0 <= t < 0.5) if end == "ra" else (0.5 <= t <= 1.0):
                    near.append((v - (A + e * max(0.0, min(1.0, t)))).length)
            if near:
                lim = min(near) - CLEAR_MARGIN
                if lim < c[end]:
                    r = max(floor[end], lim)
                    if r < c[end] - 0.005: fitted.append(f"{c['bone']}{'+' if end == 'ra' else '-'} {c[end] * 100:.0f}->{r * 100:.0f}")
                    c[end] = r
        if "ra" in c: c["r"] = max(c["ra"], c["rb"])
        out.append(c)
    if grow: log(f"cloth: capsules {grow * 100:.1f} cm thicker: 90 % of the garment lies within that of its particles")
    if fitted: log(f"cloth: capsules kept inside the garment at rest: {', '.join(fitted)} cm")
    return out


def hair_capsules(tpl, meshes):
    """Capsules along the survivor's physics hair chain (hair_00..02, hair_00_l ...) where the model's hair hangs on it
    (rig_hair): the cloth then collides with a long braid or ponytail that swings on those bones, and a robe or cape
    is pushed aside instead of the hair passing through it. Last bone: to the far end of the hair it carries."""
    chains = {}
    for b in tpl.pos:
        m = re.match(r"^hair_(\d+)(_[lr])?$", b)
        if m: chains.setdefault(m.group(2) or "", []).append((int(m.group(1)), b))
    if not chains: return []
    pts = {}
    for m in meshes:
        if m.name.startswith(CLOTH_PREFIX): continue
        mw = m.matrix_world
        for v, w in zip(m.data.vertices, weights_of(m)):
            if not w: continue
            b, x = max(w.items(), key=lambda kv: kv[1])
            if b.startswith("hair_") and x >= 0.5: pts.setdefault(b, []).append(mw @ v.co)
    caps = []
    for side, bones in chains.items():
        bones = [b for _, b in sorted(bones)]
        for i, b in enumerate(bones):
            P = pts.get(b, [])
            if len(P) < 30: continue
            A = tpl.pos[b]
            if i + 1 < len(bones): C = tpl.pos[bones[i + 1]]
            else:                                          # the tip: the hair's far end along the chain
                d = (A - tpl.pos[bones[i - 1]]).normalized() if i else Vector((0, 0, -1))
                C = A + d * max(0.02, max((p - A).dot(d) for p in P))
            e = C - A
            dd = sorted((p - (A + e * max(0.0, min(1.0, (p - A).dot(e) / max(e.length_squared, 1e-9))))).length
                        for p in P)
            # inside the hair (lower quartile of its distance to the bone line, at most 6 cm): a braid's strands fan out
            # and loose locks ride on the same bone; a fat capsule would hold the cloth off the back
            caps.append({"bone": b, "a": list(A), "b": list(C), "r": min(0.06, max(0.01, dd[len(dd) // 4] * 0.9))})
    return caps


def hem_on_leg(tpl, hem_z):
    """Where a hem ends on the model's legs: 0 at the hips, ~0.5 at the knees, 1 at the ankles (can go past both).
    cloth.py picks the garment's tuning from it (jacket tails vs a long coat, a mini skirt vs a long one)."""
    hip = [tpl.pos[b].z for b in ("thigh_l", "thigh_r") if b in tpl.pos]
    ank = [tpl.pos[b].z for b in ("foot_l", "foot_r") if b in tpl.pos]
    if not hip or not ank: return 0.5
    h, a = sum(hip) / len(hip), sum(ank) / len(ank)
    return (h - hem_z) / max(1e-6, h - a)


def cloth_regions(tpl, meshes, mats, log=print, objs=None):
    """mats: {material: kind} from cloth_materials. Splits each garment kind's faces off into objects named
    B4BCLOTH_... (the first garment) / B4BCLOTH_R<k>_... and builds its simulation mesh. Returns (new objects, [sim dict]). sim (Blender
    metres): verts, tris, depth (0 at the fixed top row .. 1 at the hem), weights ({bone: w} per sim vertex), kind,
    closed, length_m, hem_leg (hem_on_leg), collision capsules. cloth.py tunes each garment from these."""
    if not mats: return [], []
    kinds = {}
    for n, k in mats.items(): kinds.setdefault(k, set()).add(n)
    all_pieces, sims = [], []
    caps = None
    for kind in ("lower", "cape"):
        ms = kinds.get(kind)
        if not ms: continue
        if kind == "lower":
            cap = tpl.pos["spine_01"].z + 0.02 if "spine_01" in tpl.pos else 1e9
        else:
            # from the hips: the upper cape stays skinned over the back (a cloth panel there can't follow a backpack,
            # belt pouches or the shoulders and they poke through); below, the cape's max distance grows with depth
            # squared (cloth.py), so it starts still at the cut and swings more toward the hem
            cap = tpl.pos["pelvis"].z if "pelvis" in tpl.pos else tpl.pos["spine_03"].z if "spine_03" in tpl.pos else 1e9
        tag = f"{CLOTH_PREFIX}R{len(sims)}_" if sims else CLOTH_PREFIX
        _, pts, _ = cut_region(tpl, meshes, ms, cap, tag, log, dry=True, objs=objs)
        if not pts: continue
        top = max(p.z for p in pts)
        if top - min(p.z for p in pts) < 0.12:
            log(f"cloth: {sorted(ms)} hangs only {100 * (top - min(p.z for p in pts)):.0f} cm below the "
                f"{'waist' if kind == 'lower' else 'hips'}: skinned, not simulated")
            continue
        sim = build_sim(tpl, pts, kind, top, log)
        keep = None
        if not sim["closed"]:
            # faces in the gap (e.g. where a coat's fronts still meet just below the waist) stay skinned: the open
            # panel doesn't reach them
            cx, cy = sim["centre"]; a0, span = sim["arc"]
            keep = lambda c: (math.atan2(c.y - cy, c.x - cx) - a0) % (2 * math.pi) <= span
        pieces, _, inward = cut_region(tpl, meshes, ms, cap, tag, log, keep=keep, objs=objs)
        if not sim["closed"] and inward < 0.2:
            add_back_faces(pieces, log)
        sim["weights"] = sim_weights(tpl, sim["verts"], kind, sim["segments"], meshes)
        sim["kind"] = kind
        sim["hem_leg"] = hem_on_leg(tpl, top - sim["length_m"])
        if caps is None: caps = body_capsules(tpl, meshes, log, set(mats))
        sim["collision"] = fit_capsules(caps, sim, kind, log)
        log(f"cloth: {sorted(ms)} -> {sum(len(c.data.polygons) for c in pieces)} cloth faces ({kind}), simulation mesh "
            f"{sim['shape']}, {sim['rows']}x{sim['segments']} ({len(sim['verts'])} vertices), top {top * 100:.0f} cm, "
            f"hem {(top - sim['length_m']) * 100:.0f} cm")
        all_pieces += pieces
        sims.append(sim)
    return all_pieces, sims


# ---- 3. skirts that follow the legs (--skirt legs) -----------------------------------------------------------------
# Instead of cloth: the skirt is skinned to the hips and thighs the way some game rigs do it (Fortnite's Doja Cat
# hangs her skirt bones under thigh_l/thigh_r): at the waist on the pelvis, from just below the hip joints on the thigh
# of its side, the front and back centre shared by both thighs. A leg lifting carries its half of the skirt along and
# the front and back stretch smoothly between the legs; crouching, the front rests on the thighs (a slight plume) and
# the back drapes over the seat. Rigs that hang their skirt bones under the pelvis (most Fortnite skirts: dyn_skirt_*,
# MSTR_skirt_*) otherwise leave it rigid on the hips: the thighs go through it, and as cloth it tore and floated.
LEG_GARMENT_BONES = re.compile(r"skirt|skit_|dress|kilt|gown|tutu|^dyn_(main_)?(coat|robe|poncho)", re.I)
SKIRT_ATTR = "b4bskirt"
SKIRT_TOP = 0.04         # m above the hip joints: the front starts leaving the pelvis for the thighs ...
SKIRT_FULL = 0.10        # m below them: ... and is on the thighs from here down
SKIRT_BACK_FULL = 0.22   # m below them: the back's share on the thighs grows down to here ...
SKIRT_BACK = 0.65        # ... up to this (the rest stays with the pelvis: it drapes over the seat when crouching; more
                         # on the thighs and the seat came through it)
SKIRT_SPLIT = 0.6        # x half the hip width: the band around the front/back centre shared by both thighs


def mark_skirt_bones(objs, log=print):
    """Each source vertex's weight share on the rig's own skirt/dress/coat bones (LEG_GARMENT_BONES), kept as the
    vertex attribute SKIRT_ATTR through the fit, which renames those bones to the pelvis or a thigh. Returns the number
    of vertices mostly on them."""
    n = 0
    for m in objs:
        if m.type != "MESH": continue
        hit = {g.index for g in m.vertex_groups if LEG_GARMENT_BONES.search(g.name)}
        if not hit: continue
        a = m.data.attributes.get(SKIRT_ATTR) or m.data.attributes.new(SKIRT_ATTR, "FLOAT", "POINT")
        for v in m.data.vertices:
            tot = sum(g.weight for g in v.groups)
            x = sum(g.weight for g in v.groups if g.group in hit) / tot if tot else 0.0
            a.data[v.index].value = x
            n += x >= 0.5
    if n: log(f"skirt: {n} vertices on the model's own skirt bones")
    return n


def skirt_share(m, tpl, cap):
    """{vertex: 0..1} how much of each vertex is skirt: its share on the source's skirt bones (mark_skirt_bones), or 1
    for faces of a material named like a skirt, dress or coat (not an accessory), both only below the waist (cap) and
    not on faces skinned to the arms (a coat's sleeves)."""
    out = {}
    a = m.data.attributes.get(SKIRT_ATTR)
    mw = m.matrix_world
    if a is not None:
        for v in m.data.vertices:
            x = a.data[v.index].value
            if x > 0: out[v.index] = x
    names = [re.sub(r"\.\d{3}$", "", x.name) if x else "" for x in m.data.materials]
    labels = material_labels([m])
    named = {i for i, n in enumerate(names) if n and (SKIRT_RX.search(labels.get(n, n)) or
                                                       COAT_RX.search(labels.get(n, n)))
             and not ACCESSORY_RX.search(n) and not CAPE_RX.search(labels.get(n, n))}
    if named and not ACCESSORY_RX.search(m.name):
        W = weights_of(m)
        arm = [sum(x for b, x in w.items() if ARM_RX.match(b)) / (sum(w.values()) or 1.0) for w in W]
        for p in m.data.polygons:
            if p.material_index in named and sum(arm[v] for v in p.vertices) / len(p.vertices) < 0.5:
                for v in p.vertices: out[v] = 1.0
    # only below the waist (fading in over 3 cm): above it the garment is a top and keeps its weights
    for i in list(out):
        out[i] *= smooth(cap, cap - 0.03, (mw @ m.data.vertices[i].co).z)
        if out[i] <= 1e-3: del out[i]
    return out


def leg_weights(p, tpl):
    """{pelvis, thigh_l, thigh_r: w} for a skirt point p (world, bind pose)."""
    hl, hr = tpl.pos["thigh_l"], tpl.pos["thigh_r"]
    mid = (hl + hr) / 2
    lat = hl - hr; lat.z = 0.0
    half = max(0.03, lat.length / 2)
    lat.normalize()
    fr = smooth(mid.z + SKIRT_TOP, mid.z - SKIRT_FULL, p.z)
    bk = SKIRT_BACK * smooth(mid.z, mid.z - SKIRT_BACK_FULL, p.z)
    front = smooth(mid.x - 0.06, mid.x + 0.06, p.x)              # B4B heroes face +X
    s = bk + (fr - bk) * front
    left = smooth(-SKIRT_SPLIT * half, SKIRT_SPLIT * half, (p - mid).dot(lat))
    return {"pelvis": 1.0 - s, "thigh_l": s * left, "thigh_r": s * (1.0 - left)}


def skirt_legs(tpl, meshes, log=print):
    """--skirt legs: skirts, dresses and coat tails below the waist follow the pelvis and thighs (leg_weights) instead
    of being cloth or rigid on the hips; each vertex by how much of it is skirt (skirt_share). Returns the number of
    vertices changed."""
    if any(b not in tpl.pos for b in ("pelvis", "thigh_l", "thigh_r")):
        log("skirt: the skeleton has no pelvis/thighs: --skirt legs does nothing"); return 0
    cap = tpl.pos["spine_01"].z + 0.02 if "spine_01" in tpl.pos else tpl.pos["pelvis"].z + 0.1
    n, objs = 0, []
    for m in meshes:
        share = skirt_share(m, tpl, cap)
        if not share: continue
        W = weights_of(m)
        mw = m.matrix_world
        for i, g in share.items():
            w = W[i]
            tot = sum(w.values()) or 1.0
            new = {b: x / tot * (1.0 - g) for b, x in w.items()}
            for b, x in leg_weights(mw @ m.data.vertices[i].co, tpl).items():
                new[b] = new.get(b, 0.0) + x * g
            W[i] = {b: x for b, x in new.items() if x > 1e-4}
            n += 1
        set_weights(m, W)
        objs.append(m.name)
    if n:
        log(f"skirt: {n} vertices of {', '.join(objs)} follow the pelvis and thighs (no cloth): on the pelvis at the "
            f"waist, on the thigh of their side below the hips, front and back shared by both")
    else:
        log("skirt: --skirt legs: no skirt found (no skirt/dress bones on the rig, no material named like a skirt, "
            "dress or coat)")
    return n


# ---- 4. the survivor's gear bones in weights copied from the template -----------------------------------------------
# Unrigged models take their weights from the survivor's mesh (nearest points). Survivors carry gear on their own bones
# (Holly Elite 00's bag on `backpack`, under spine_03, 8700 of her vertices; gas masks), which the animations move apart
# from the body: a model's lower back, hips and skirt copied onto `backpack` swung out behind her as a stiff block when
# she crouched or ran. Those template vertices get the body's weights under them instead (before any copying).
GEAR_BONE_RX = re.compile(r"^(backpack|gasmask)(_|$)", re.I)


def drop_template_gear(tpl, log=print):
    """Template mesh weights on gear bones (GEAR_BONE_RX) -> the weights of the nearest template vertices that aren't
    gear (the body under the bag), each vertex by its gear share. Returns the vertex count changed."""
    meshes = [(tm, weights_of(tm)) for tm in tpl.meshes]
    gear = lambda w: sum(x for b, x in w.items() if GEAR_BONE_RX.match(b)) / (sum(w.values()) or 1.0)
    body = []
    for tm, W in meshes:
        mw = tm.matrix_world
        body += [(mw @ v.co, w) for v, w in zip(tm.data.vertices, W) if w and gear(w) < 0.05]
    if not body: return 0
    kd = KDTree(len(body))
    for i, (p, _) in enumerate(body): kd.insert(p, i)
    kd.balance()
    n, bones = 0, set()
    for tm, W in meshes:
        mw, hit = tm.matrix_world, False
        for v, w in zip(tm.data.vertices, W):
            g = gear(w) if w else 0.0
            if g <= 0: continue
            tot = sum(w.values())
            new = {b: x / tot for b, x in w.items() if not GEAR_BONE_RX.match(b)}
            near, f = {}, 0.0
            for _, i, d in kd.find_n(mw @ v.co, 4):
                k = 1.0 / max(d, 1e-4); f += k
                for b, x in body[i][1].items(): near[b] = near.get(b, 0.0) + x * k
            for b, x in near.items(): new[b] = new.get(b, 0.0) + g * x / f
            bones.update(b for b in w if GEAR_BONE_RX.match(b))
            W[v.index] = new; n += 1; hit = True
        if hit: set_weights(tm, W)
    if n:
        log(f"weights: the survivor's gear bones ({', '.join(sorted(bones))}) left out of the weights copied from it "
            f"({n} template vertices take the body's under them): the model doesn't swing on them")
    return n
