"""Secondary motion for imported survivor meshes: the template's physics hair chains, and skirts as cloth.

  hair_chains(skm_file, src)        simulated hair bone chains of the template (its physics asset), for b4bfit
  cloth_assets(skm)                 the template's clothing assets (MeshClothingAssets)
  apply(skm, sims, src=...)         write simulation meshes (from blender/b4bdangle.py) into the outfit's clothing
                                    assets (new ones added to the package when it has too few) and bind the cloth
                                    sections of the first LODs to them

Cloth data (UE 4.25 ClothingSystemRuntimeCommon, NvCloth; all tagged properties, uprops.py): per clothing LOD a
ClothPhysicalMeshData (Vertices, Normals, Indices, WeightMaps {1: MaxDistance, 2/3: backstop, 4: anim drive},
InverseMasses, BoneData (skinning to UsedBoneNames, 12 slots), NumFixedVerts, MaxBoneWeights) + CollisionData, then
native TransitionUp/DownSkinData. The fabric is cooked by the engine at load. Render side (skm.py): a cloth section
has one FMeshToMeshVertData per vertex: position/normal/tangent = sum_i bary_i * (V_i - N_i * dist) over a simulation
triangle (V, N = stored vertices/normals: stored normals point along -cross(B-A, C-A)), plus the LOD's cloth vertex
buffer (all cloth sections' records in section order, per section (offset, base vertex)). Checked on retail Holly
Elite 00 (flannel + sleeves). docs/investigations/mesh-mods.md §14.
"""
import copy, math, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import skm, skmgltf, upkg, uprops

CLOTH_LODS = 3            # mesh LODs 0..2 simulate (retail: 3 of 5); later LODs skin the skirt to the hips/legs


# ---- template inspection -------------------------------------------------------------------------------------------

def _obj_prop(s, name):
    p = s.props.get(name)
    if not p: return None
    return s.pkg.obj_path(upkg.R(s.data, p["off"]).i32())


def simulated_bones(skm_file, src):
    """(bones whose physics-asset body is PhysType_Simulated (lower case), the physics asset's path or None). The set
    is None when the physics asset isn't extracted under src."""
    s = skm.SkeletalMesh(skm_file)
    pa = _obj_prop(s, "PhysicsAsset")
    if not pa: return set(), None
    f = upkg.game_path_to_file(pa.split(".")[0], src)
    if not f or not os.path.exists(f): return None, pa
    p = upkg.Package(f)
    sim = set()
    for e in p.exports:
        if p.class_name(e) != "SkeletalBodySetup": continue
        t, _ = uprops.parse(p, bytes(p.export_data(e)))
        bn, pt = uprops.find(t, "BoneName"), uprops.find(t, "PhysicsType")
        if bn and pt and p.names[pt.value[0]] == "PhysType_Simulated":
            sim.add(p.names[bn.value[0]].lower())
    return sim, pa


def hair_chains(skm_file, src):
    """Chains (root first) of the template's hair bones that its physics asset simulates (PhysType_Simulated bodies
    under `head`; bones in between without a body swing along)."""
    s = skm.SkeletalMesh(skm_file)
    sim, pa = simulated_bones(skm_file, src)
    if not sim: return [], pa
    bones = [s.name(b[:2]).lower() for b in s.m["refskel"]["bones"]]
    parent = [b[2] for b in s.m["refskel"]["bones"]]
    kids = {}
    for i, par in enumerate(parent): kids.setdefault(par, []).append(i)

    def under_head(i):
        while i >= 0:
            if bones[i] == "head": return True
            i = parent[i]
        return False
    def sim_above(i):                 # a simulated hair bone further up: i is inside that chain (Mom: 00 and 02 of 3)
        i = parent[i]
        while i >= 0 and bones[i] != "head":
            if bones[i] in sim: return True
            i = parent[i]
        return False
    chains = []
    for i, b in enumerate(bones):
        if b in sim and "hair" in b and under_head(i) and not sim_above(i):
            c, j = [b], i
            while len(kids.get(j, [])) == 1:
                j = kids[j][0]; c.append(bones[j])
            chains.append(c)
    return chains, pa


def cloth_assets(s):
    """[(asset name, export)] of MeshClothingAssets, in order (a section's cloth_asset_index indexes this)."""
    p = s.props.get("MeshClothingAssets")
    if not p: return []
    r = upkg.R(s.data, p["off"])
    out = []
    for _ in range(r.i32()):
        i = r.i32()
        out.append((s.pkg.obj(i), s.pkg.exports[i - 1] if i > 0 else None))
    return out


def asset_physics_assets(s):
    """/Game/... paths of the physics assets the mesh's clothing assets collide with (their bodies are counted against
    NvCloth's sphere limit and listed in UsedBoneNames: b4bmodel extracts them)."""
    out = []
    for _, e in cloth_assets(s):
        if e is None: continue
        t, _ = uprops.parse(s.pkg, bytes(s.pkg.export_data(e)))
        pa = uprops.find(t, "PhysicsAsset")
        if pa is not None and pa.value:
            path = s.pkg.obj_path(pa.value)
            if path and path.split(".")[0] not in out: out.append(path.split(".")[0])
    return out


# ---- geometry ------------------------------------------------------------------------------------------------------

def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def mul(a, k): return (a[0] * k, a[1] * k, a[2] * k)
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(a):
    l = math.sqrt(dot(a, a))
    return mul(a, 1.0 / l) if l > 1e-12 else (0.0, 0.0, 1.0)


def bary(p, a, b, c):
    v0, v1, v2 = sub(b, a), sub(c, a), sub(p, a)
    d00, d01, d11, d20, d21 = dot(v0, v0), dot(v0, v1), dot(v1, v1), dot(v2, v0), dot(v2, v1)
    den = d00 * d11 - d01 * d01
    if abs(den) < 1e-12: return (1.0, 0.0, 0.0)
    v = (d11 * d20 - d01 * d21) / den
    w = (d00 * d21 - d01 * d20) / den
    return (1.0 - v - w, v, w)


def point_tri_dist2(p, a, b, c):
    """Squared distance from p to triangle abc (clamped barycentric)."""
    u, v, w = bary(p, a, b, c)
    if u >= 0 and v >= 0 and w >= 0:
        q = add(add(mul(a, u), mul(b, v)), mul(c, w))
        d = sub(p, q); return dot(d, d)
    best = 1e30
    for x, y in ((a, b), (b, c), (c, a)):
        e = sub(y, x); t = max(0.0, min(1.0, dot(sub(p, x), e) / max(dot(e, e), 1e-12)))
        d = sub(p, add(x, mul(e, t))); best = min(best, dot(d, d))
    return best


def solve(q, A, B, C, NA, NB, NC):
    """(u, v, w, d) with u*(A+NA*d) + v*(B+NB*d) + w*(C+NC*d) = q (the GPU skin cache's cloth formula, normals given
    as they enter it: the negated stored normals)."""
    d = 0.0
    nsum = add(add(NA, NB), NC)
    for _ in range(12):
        a, b, c = add(A, mul(NA, d)), add(B, mul(NB, d)), add(C, mul(NC, d))
        n = norm(cross(sub(b, a), sub(c, a)))
        if dot(n, nsum) < 0: n = mul(n, -1)
        e = dot(sub(q, a), n)
        qp = sub(q, mul(n, e))
        u, v, w = bary(qp, a, b, c)
        ni = add(add(mul(NA, u), mul(NB, v)), mul(NC, w))
        k = dot(ni, n)
        if abs(k) < 1e-4: break
        d += e / k
        if abs(e) < 1e-5: break
    return u, v, w, d


class Sim:
    """A simulation mesh in UE space (cm) with the normals the mapping uses."""
    def __init__(self, verts, tris):
        self.V, self.T = verts, [tuple(t) for t in tris]
        acc = [(0.0, 0.0, 0.0)] * len(verts)
        for a, b, c in self.T:
            n = cross(sub(verts[b], verts[a]), sub(verts[c], verts[a]))
            for x in (a, b, c): acc[x] = add(acc[x], n)
        self.Nm = [norm(n) for n in acc]                    # runtime normals (the mapping's)
        self.N = [mul(n, -1) for n in self.Nm]              # stored normals (retail convention)
        self.adj = [[] for _ in verts]
        for ti, t in enumerate(self.T):
            for x in t: self.adj[x].append(ti)

    def candidates(self, p, k=6):
        near = sorted(range(len(self.V)), key=lambda i: dot(sub(self.V[i], p), sub(self.V[i], p)))[:k]
        return sorted({t for i in near for t in self.adj[i]})

    def record(self, p, n, t, tris=None):
        """FMeshToMeshVertData tuple for a point p with unit normal n and tangent t (cm)."""
        tris = tris or self.candidates(p)
        order = sorted(tris, key=lambda i: point_tri_dist2(p, *(self.V[x] for x in self.T[i])))
        best = None
        # the nearest triangle first; a point far off the surface (a layer well inside a pleated skirt) can make the
        # offset solve run away on it (a vertex thrown a metre off): then the next triangles, the best reconstruction
        for ti in order[:6]:
            a, b, c = self.T[ti]
            args = (self.V[a], self.V[b], self.V[c], self.Nm[a], self.Nm[b], self.Nm[c])
            pos = solve(p, *args)
            q = [sum(w * (self.V[x][k] - self.N[x][k] * pos[3]) for w, x in zip(pos[:3], (a, b, c))) for k in range(3)]
            err = math.dist(q, p)
            if best is None or err < best[0]: best = (err, ti, args, pos)
            if err < 0.05: break
        _, ti, args, pos = best
        a, b, c = self.T[ti]
        nrm = solve(add(p, n), *args)
        tan = solve(add(p, t), *args)
        return tuple(pos) + tuple(nrm) + tuple(tan) + (a, b, c, 0, 0.0, 0)


# ---- writing -------------------------------------------------------------------------------------------------------

def blender_to_ue(p):
    return (p[0] * 100.0, -p[1] * 100.0, p[2] * 100.0)


def _pm_set(pm, name, value):
    p = uprops.find(pm, name)
    if p is None: raise SystemExit(f"cloth: the template's clothing asset has no {name}")
    if isinstance(p.value, dict): p.value["items"] = value
    else: p.value = value


def _bone_data(template_el, infl):
    """One ClothVertBoneData element (tagged: NumInfluences, BoneIndices[12], BoneWeights[12])."""
    el = copy.deepcopy(template_el)
    for p in el:
        if p.name == "NumInfluences": p.value = len(infl)
        elif p.name == "BoneIndices": p.value = infl[p.index][0] if p.index < len(infl) else 0
        elif p.name == "BoneWeights": p.value = float(infl[p.index][1]) if p.index < len(infl) else 0.0
    return uprops.Tagged(el)


def inverse_masses(V, T, fixed, depth=None, hem_mass=1.0):
    """Per-vertex inverse masses from the vertex areas (mean mass 1); hem_mass > 1 makes the lower part heavier
    (x hem_mass at the hem, easing in from the top: depth^2), so a coat's hem swings slow and settles."""
    m = [0.0] * len(V)
    for a, b, c in T:
        cr = cross(sub(V[b], V[a]), sub(V[c], V[a]))
        ar = math.sqrt(dot(cr, cr)) / 2.0
        for x in (a, b, c): m[x] += ar / 3.0
    mean = sum(m) / max(1, len(m)) or 1.0
    if depth is not None and hem_mass != 1.0:
        m = [x * (1.0 + (hem_mass - 1.0) * depth[i] ** 2) for i, x in enumerate(m)]
    return [0.0 if fixed[i] or m[i] <= 0 else mean / m[i] for i in range(len(V))]


# ---- per-garment tuning --------------------------------------------------------------------------------------------
# UE 4.25 UClothConfigNv defaults (the class default object; retail configs only store what differs: Walker Elite 07's
# coat Damping 0.5, SelfCollisionStiffness 0.1, CollisionThickness 1.1; Doc Elite 03's jacket Damping 0.5,
# TetherStiffness 1.1; Holly Elite 00's free flannel GravityScale 3, LinearDrag 0.8, StretchLimit 1.2, 60/30 Hz;
# Karlee Elite 06 and Holly Elite 04 all defaults). Retail sets the garment apart mostly by its max distances: long
# coats nearly free at the hem (Walker E07 ~0 / 17 / 57 / 93 / 100 cm over five height bands of a 64 cm coat, Doc E03
# 0 / 11 / 31 / 60 / 75 over 58 cm), short pieces held close (Karlee E06 0-5 cm, Holly E04 0-7 cm).
NV_DEFAULTS = {"stiffness": 1.0, "stiffness_mult": 1.0, "stretch": 1.0, "compress": 1.0, "bend": 1.0,
               "self_radius": 0.0, "self_stiffness": 0.0, "self_cull": 1.0, "damping": 0.4, "friction": 0.1,
               "linear_drag": 0.2, "angular_drag": 0.2, "linear_inertia": 1.0, "angular_inertia": 1.0,
               "centrifugal": 1.0, "solver_hz": 120.0, "stiffness_hz": 100.0, "gravity": 1.0, "tether_stiffness": 1.0,
               "tether_limit": 1.0, "thickness": 1.0}
# What each garment gets (on top of NV_DEFAULTS). maxd: max distance at the hem as a share of the garment's length,
# maxd_exp: how it grows from the fixed top row (depth ** maxd_exp: > 1 keeps the upper part close and frees the
# tails), maxd_cap: cm; hem_mass: see inverse_masses; self: self-collision (radius from the mesh spacing).
# Drag = how much of the body's own motion the cloth is carried along with, inertia = how much of the body's
# acceleration it feels (NvCloth local-space simulation): a heavy coat follows the body more and flings out less;
# a cape feels all of it and is barely carried, so it lifts off the back and trails when running.
GARMENTS = {
    "skirt":        dict(maxd=0.45, maxd_exp=1.5, hem_mass=1.0, damping=0.4, linear_drag=0.2, angular_drag=0.2),
    "long skirt":   dict(maxd=0.55, maxd_exp=1.5, hem_mass=1.5, damping=0.45, gravity=1.2, linear_drag=0.25,
                         angular_drag=0.25, linear_inertia=0.9, angular_inertia=0.9),
    "jacket tails": dict(maxd=0.35, maxd_exp=1.2, maxd_cap=8.0, hem_mass=1.5, damping=0.7, gravity=1.2,
                         linear_drag=0.5, angular_drag=0.5, linear_inertia=0.5, angular_inertia=0.5, centrifugal=0.5),
    "long coat":    dict(maxd=1.0, maxd_exp=1.6, hem_mass=2.5, damping=0.6, gravity=1.5, linear_drag=0.35,
                         angular_drag=0.35, linear_inertia=0.7, angular_inertia=0.6, centrifugal=0.6, self=True),
    "cape":         dict(maxd=1.0, maxd_exp=2.0, hem_mass=2.0, damping=0.25, friction=0.0, bend=0.6,
                         linear_drag=0.05, angular_drag=0.1, linear_inertia=1.0, angular_inertia=0.8, self=True),
}
NUMERIC = [k for k in NV_DEFAULTS] + ["maxd", "maxd_exp", "maxd_cap", "hem_mass"]


def garment_profile(sd):
    """(garment name, tuning dict) for a simulation mesh from blender/b4bdangle.py: kind (lower / cape), closed (tube)
    or open panel, hem_leg (0 hips, 0.5 knees, 1 ankles). Coats between jacket tails and a long coat are blended by
    where the hem ends. B4B_CLOTH_TUNE='{"damping": 0.5, ...}' overrides values (experiments)."""
    import json
    hem = sd.get("hem_leg", 0.5)
    if sd.get("kind") == "cape":
        name, t = "cape", dict(GARMENTS["cape"])
    elif sd.get("closed", True):
        name = "long skirt" if hem > 0.55 else "skirt"
        t = dict(GARMENTS[name])
    else:
        w = max(0.0, min(1.0, (hem - 0.2) / 0.5))              # hem at the upper thigh -> 0, below the knee -> 1
        a, b = GARMENTS["jacket tails"], GARMENTS["long coat"]
        t = {}
        for k in set(a) | set(b):
            if k == "maxd_cap":                                 # only real jacket tails are held close
                if w <= 0.2: t[k] = a[k]
                continue
            if k == "self": t[k] = w >= 0.5; continue
            x, y = a.get(k, NV_DEFAULTS.get(k)), b.get(k, NV_DEFAULTS.get(k))
            t[k] = x + (y - x) * w
        name = "long coat" if w >= 0.8 else ("jacket tails" if w <= 0.2 else "coat")
    env = os.environ.get("B4B_CLOTH_TUNE")
    if env:
        t.update(json.loads(env))
    return name, t


def self_collision_indices(V, fixed, radius):
    """The engine's FClothPhysicalMeshData::BuildSelfCollisionData: free vertices at least `radius` apart."""
    out, r2 = [], radius * radius
    for i, v in enumerate(V):
        if fixed[i]: continue
        if all(dot(sub(v, V[j]), sub(v, V[j])) >= r2 for j in out): out.append(i)
    return out


def write_config(pkg, ce, t):
    """Every ClothConfigNv value of the tuning t (NV_DEFAULTS where t has none) into the config export ce, written
    out explicitly: the config may be the donor's or the template's own (Holly Elite 00's flannel: gravity x3)."""
    v = dict(NV_DEFAULTS); v.update({k: x for k, x in t.items() if k in NV_DEFAULTS})
    for n in ("FloatProperty", "StructProperty", "Vector", "ClothConstraintSetupNv", "None"): pkg.add_name(n)
    f = lambda name, x: uprops.Prop(name, "FloatProperty", 0, None, None, float(x), pkg.fname_of(name))

    def cons(name, stiff):
        kids = [f("Stiffness", stiff), f("StiffnessMultiplier", v["stiffness_mult"]), f("StretchLimit", v["stretch"]),
                f("CompressionLimit", v["compress"])]
        return uprops.Prop(name, "StructProperty", 0, (pkg.fname_of("ClothConstraintSetupNv"), b"\0" * 16), None,
                           kids, pkg.fname_of(name))
    vec = lambda name, x: _vec_prop(pkg, name, (float(x),) * 3)
    new = [cons("VerticalConstraint", v["stiffness"]), cons("HorizontalConstraint", v["stiffness"]),
           cons("BendConstraint", v["bend"]), cons("ShearConstraint", v["stiffness"]),
           f("SelfCollisionRadius", v["self_radius"]), f("SelfCollisionStiffness", v["self_stiffness"]),
           f("SelfCollisionCullScale", v["self_cull"]), vec("Damping", v["damping"]), f("Friction", v["friction"]),
           vec("LinearDrag", v["linear_drag"]), vec("AngularDrag", v["angular_drag"]),
           vec("LinearInertiaScale", v["linear_inertia"]), vec("AngularInertiaScale", v["angular_inertia"]),
           vec("CentrifugalInertiaScale", v["centrifugal"]), f("SolverFrequency", v["solver_hz"]),
           f("StiffnessFrequency", v["stiffness_hz"]), f("GravityScale", v["gravity"]),
           f("TetherStiffness", v["tether_stiffness"]), f("TetherLimit", v["tether_limit"]),
           f("CollisionThickness", v["thickness"])]
    old = bytes(pkg.export_data(ce))
    tree, end = uprops.parse(pkg, old)
    names = {p.name for p in new}
    tree = [p for p in tree if p.name not in names] + new       # anything else the config had stays
    pkg.set_export_data(ce, uprops.write(pkg, tree) + old[end:])


def config_export(pkg, tree):
    """The ClothConfigNv export a clothing asset's ClothConfigs map points at, or None."""
    cm = uprops.find(tree, "ClothConfigs")
    if cm is None: return None
    for _, idx in cm.value["items"]:
        if isinstance(idx, int) and 0 < idx <= len(pkg.exports) and pkg.class_name(pkg.exports[idx - 1]) == "ClothConfigNv":
            return pkg.exports[idx - 1]
    return None


def _transitions(sim, n):
    """Identity mapping of a simulation mesh onto itself (clothing LODs share one mesh)."""
    recs = []
    for i, v in enumerate(sim.V):
        tris = sim.adj[i]
        a, b, c = sim.T[tris[0]]
        other = b if a == i else a
        t = sub(sim.V[other], v); t = norm(sub(t, mul(sim.N[i], dot(t, sim.N[i]))))
        recs.append(sim.record(v, sim.N[i], t, tris))
    blob = b"".join(struct.pack("<" + skm.MESH_TO_MESH, *r) for r in recs)
    return blob, len(recs)


# ---- clothing assets: the outfit's own, or new ones --------------------------------------------------------------
# An outfit without a clothing asset gets one: a ClothingAssetCommon export (outer: the mesh) with its ClothConfigNv
# (outer: the asset), copied from the donor below (tagged layout + NvCloth config of a retail long coat), names
# re-pointed into the outfit's package, plus the imports they need (classes, class default objects, the collision
# physics asset). The event-driven loader's preload dependencies are the retail ones (3P_Walker_Elite_07_SKM:
# config <- asset <- mesh). upkg.Package writes the new name/import/export maps. mesh-mods.md §14.
DONOR = "/Game/TU15/Characters/Heroes/Walker/Meshes/Elite/Elite_07/3P_Walker_Elite_07_SKM"
LEG_PA = "/Game/TU15/Characters/Heroes/Walker/Meshes/Elite/Elite_07/3P_Walker_Elite_07_Cloth_PA"
NEEDS = [DONOR, LEG_PA]          # b4bmodel extracts these when the outfit gets a new clothing asset


def _skm_tags(s):
    t, end = uprops.parse(s.pkg, s.data)
    assert end == s.props_end
    return t


def add_mesh_clothing_asset(s, idx):
    """Append export `idx` to the mesh's MeshClothingAssets (the tag is added when the mesh has none)."""
    pkg = s.pkg
    t = _skm_tags(s)
    p = uprops.find(t, "MeshClothingAssets")
    if p is None:
        for n in ("MeshClothingAssets", "ArrayProperty", "ObjectProperty"): pkg.add_name(n)
        p = uprops.Prop("MeshClothingAssets", "ArrayProperty", 0, pkg.fname_of("ObjectProperty"), None, [],
                        pkg.fname_of("MeshClothingAssets"))
        after = max((i for i, q in enumerate(t) if q.name in ("PhysicsAsset", "ShadowPhysicsAsset")), default=len(t) - 1)
        t.insert(after + 1, p)
    p.value = list(p.value) + [idx]
    s.set_tags(uprops.write(pkg, t))
    return len(p.value) - 1


def new_clothing_asset(s, src, k, pa_path=None, log=print):
    """Add clothing asset number k (and its config) to the mesh's package. pa_path: collision physics asset
    (/Game/... object path) or None. Returns (index in MeshClothingAssets, export)."""
    import hashlib
    df = upkg.game_path_to_file(DONOR, src)
    if not df or not os.path.exists(df):
        raise SystemExit(f"cloth: {DONOR} is not extracted (b4bmod extract '{DONOR}')")
    d = upkg.Package(df)
    dca = next(e for e in d.exports if d.class_name(e) == "ClothingAssetCommon")
    dcf = next(e for e in d.exports if d.class_name(e) == "ClothConfigNv")
    pkg = s.pkg
    skm_i = pkg.exports.index(s.export) + 1
    ca_cls, ca_cdo = pkg.import_class("/Script/ClothingSystemRuntimeCommon", "ClothingAssetCommon")
    cf_cls, cf_cdo = pkg.import_class("/Script/ClothingSystemRuntimeNv", "ClothConfigNv")
    pa = pkg.import_object(pa_path, "/Script/Engine", "PhysicsAsset") if pa_path else 0
    ca_data = bytes(d.export_data(dca)); cf_data = bytes(d.export_data(dcf))
    ca_tree, ca_end = uprops.parse(d, ca_data)
    cf_tree, cf_end = uprops.parse(d, cf_data)
    ca_i = len(pkg.exports) + 1                     # the asset first, its config right after
    cf_i = ca_i + 1
    objmap = {d.exports.index(dcf) + 1: cf_i}
    ca_new = uprops.remap(ca_tree, d, pkg, lambda i: objmap.get(i, 0))
    cf_new = uprops.remap(cf_tree, d, pkg)
    uprops.find(ca_new, "PhysicsAsset").value = pa
    base = s.export["name"] + "_Clothing"
    uprops.find(ca_new, "AssetGuid").value = hashlib.md5(f"b4bcoop cloth {base} {k}".encode()).digest()
    ca_i2 = pkg.add_export(ca_cls, ca_cdo, skm_i, pkg.fname_of(base, k + 1),
                           uprops.write(pkg, ca_new) + ca_data[ca_end:],
                           cbs=([pa] if pa else []) + [cf_i], sbc=[ca_cls, ca_cdo], cbc=[skm_i])
    cf_i2 = pkg.add_export(cf_cls, cf_cdo, ca_i, pkg.fname_of("ClothConfigNv"),
                           uprops.write(pkg, cf_new) + cf_data[cf_end:], sbc=[cf_cls, cf_cdo], cbc=[ca_i])
    assert (ca_i2, cf_i2) == (ca_i, cf_i)
    pkg.preload_deps()[skm_i]["cbs"].append(ca_i)
    ai = add_mesh_clothing_asset(s, ca_i)
    log(f"cloth: new clothing asset {pkg.exports[ca_i - 1]['name']} in {os.path.basename(pkg.path)} "
        f"(collision: {pa_path.split('.')[-1] if pa_path else 'own capsules'})")
    return ai, pkg.exports[ca_i - 1]


def pa_bones(pa_path, src):
    """Bone names of the bodies of a physics asset (/Game/... path), lower case; [] if it isn't extracted."""
    if not pa_path: return []
    f = upkg.game_path_to_file(pa_path.split(".")[0].split(":")[0], src or "")
    if not f or not os.path.exists(f): return []
    p = upkg.Package(f)
    out = []
    for e in p.exports:
        if p.class_name(e) != "SkeletalBodySetup": continue
        t, _ = uprops.parse(p, bytes(p.export_data(e)))
        bn = uprops.find(t, "BoneName")
        if bn: out.append(p.names[bn.value[0]].lower())
    return out


MAX_SPHERES = 32                 # NvCloth's limit on collision spheres per cloth (capsules are pairs of them)
CAPSULE_PRIORITY = ("thigh_l", "thigh_r", "calf_l", "calf_r", "pelvis", "spine_01", "spine_02", "spine_03")


def pa_spheres(pa_path, src):
    """Collision spheres a physics asset gives the cloth (a sphere 1, a capsule 2), None if it isn't extracted."""
    if not pa_path: return 0
    f = upkg.game_path_to_file(pa_path.split(".")[0].split(":")[0], src or "")
    if not f or not os.path.exists(f): return None
    p = upkg.Package(f)
    n = 0
    for e in p.exports:
        if p.class_name(e) != "SkeletalBodySetup": continue
        t, _ = uprops.parse(p, bytes(p.export_data(e)))
        ag = uprops.find(t, "AggGeom")
        if ag is None: continue
        for k, w in (("SphereElems", 1), ("SphylElems", 2)):
            x = uprops.find(ag.value, k)
            if x is not None: n += w * len(x.value["items"])
    return n


def fname_str(pkg, s):
    """FName of string s in pkg (as the name map holds it: whole, or split into base + number), added if missing."""
    import re
    try:
        return uprops.name_ref(pkg, s)
    except KeyError:
        m = re.match(r"^(.*)_(0|[1-9]\d*)$", s)
        return pkg.fname_of(m.group(1), int(m.group(2)) + 1) if m else pkg.fname_of(s)


def _vec_prop(pkg, name, v, index=0):
    return uprops.Prop(name, "StructProperty", index, (pkg.fname_of("Vector"), b"\0" * 16), None,
                       struct.pack("<3f", *v), pkg.fname_of(name))


def collision_data(pkg, caps, used, G, bone_names, scale=1.0):
    """(Spheres, SphereConnections) elements for capsules [{bone, a, b, r}] (UE cm, bind pose): two spheres in the
    bone's space joined by a connection; BoneIndex = index in the asset's UsedBoneNames."""
    for n in ("BoneIndex", "Radius", "LocalPosition", "SphereIndices", "IntProperty", "FloatProperty", "Vector"):
        pkg.add_name(n)
    spheres, conns = [], []
    for c in caps:
        b = c["bone"]
        if b not in used or b not in bone_names: continue
        inv = skmgltf.minv(G[bone_names.index(b)])
        ids = []
        for end in ("a", "b"):
            lp = skmgltf.mp(inv, c[end])
            r = c.get("r" + end, c["r"])                # tapered: each end its own radius (NvCloth cones)
            spheres.append(uprops.Tagged([
                uprops.Prop("BoneIndex", "IntProperty", 0, None, None, used.index(b), pkg.fname_of("BoneIndex")),
                uprops.Prop("Radius", "FloatProperty", 0, None, None, float(r * scale), pkg.fname_of("Radius")),
                _vec_prop(pkg, "LocalPosition", lp)]))
            ids.append(len(spheres) - 1)
        conns.append(uprops.Tagged([
            uprops.Prop("SphereIndices", "IntProperty", 0, None, None, ids[0], pkg.fname_of("SphereIndices")),
            uprops.Prop("SphereIndices", "IntProperty", 1, None, None, ids[1], pkg.fname_of("SphereIndices"))]))
    return spheres, conns


def collision_mode():
    """B4B_CLOTH_COLLISION (for experiments): pa (physics asset only), own (fitted capsules only), both (default);
    B4B_CLOTH_COLLISION_SCALE multiplies the capsule radii (x3 made the coat bell out: the engine uses them)."""
    return os.environ.get("B4B_CLOTH_COLLISION", "both").lower()


def apply(s, sims, log=print, cloth_lods=CLOTH_LODS, src=None):
    """s: skm.SkeletalMesh with LODs built by skmgltf (cloth sections flagged `cloth_tag`, `cloth_region` k). Writes
    simulation mesh k into clothing asset k of the mesh: the template's own ones first (biggest first; their config
    and collision physics asset kept), new ones added to the package for the rest (src: the extract folder with
    DONOR/LEG_PA). Binds the cloth sections of LODs < cloth_lods. Returns the clothing asset names."""
    if not sims: return None
    pkg = s.pkg
    existing = []
    for ai, (name, e) in enumerate(cloth_assets(s)):
        t, end = uprops.parse(pkg, bytes(pkg.export_data(e)))
        lods = uprops.find(t, "LodData").value["items"]
        nv = len(uprops.find(uprops.find(lods[0], "PhysicalMeshData").value, "Vertices").value["items"])
        existing.append((nv, ai, e))
    existing.sort(key=lambda x: -x[0])
    mode = collision_mode()
    bone_names = [s.name(b[:2]).lower() for b in s.m["refskel"]["bones"]]
    G = skmgltf.bone_globals(s.m["refskel"])
    targets = []
    for k, sd in enumerate(sims):
        if k < len(existing):
            targets.append((existing[k][1], existing[k][2]))
            continue
        pa = None
        if mode != "own":
            pa = LEG_PA + "." + LEG_PA.rsplit("/", 1)[-1] if sd.get("kind", "lower") == "lower" else None
        targets.append(new_clothing_asset(s, src, k, pa, log))
    names = []
    for k, (sd, (ai, ae)) in enumerate(zip(sims, targets)):
        names.append(write_asset(s, k, sd, ai, ae, bone_names, G, mode, src, log, cloth_lods))
    # render side
    bound = 0
    sims_objs = [x[0] for x in names]
    for li, lod in enumerate(s.m["lods"]):
        if li >= cloth_lods: break
        P = lod["positions"]["positions"]
        TG = lod["static_vb"]["tangents"]
        blob, mapping, off = b"", [], 0
        for sec in lod["sections"]:
            if not sec.get("cloth_tag"):
                mapping.append((0, 0)); continue
            k = min(max(0, sec.get("cloth_region", 0)), len(sims) - 1)
            sim, guid = sims_objs[k], names[k][2]
            recs = []
            for j in range(sec["num_vertices"]):
                vi = sec["base_vertex_index"] + j
                tx = skm.unpack_normal(TG[vi][0:4])[:3]; tz = skm.unpack_normal(TG[vi][4:8])[:3]
                recs.append(sim.record(P[vi], norm(tz), norm(tx)))
            sec["cloth_mapping"] = recs
            sec["cloth_asset_index"] = targets[k][0]
            ts = two_sided_slot(s, sec["material_index"])
            if ts is not None:
                if li == 0: log(f"cloth: section on slot {s.name(s.m['materials'][ts]['slot_name'])} (two-sided "
                                f"variant of {s.name(s.m['materials'][sec['material_index']]['slot_name'])})")
                sec["material_index"] = ts
            sec["clothing_data"] = (guid, li)
            mapping.append((off, sec["base_vertex_index"]))
            blob += b"".join(struct.pack("<" + skm.MESH_TO_MESH, *r) for r in recs)
            off += len(recs)
            bound += 1
        if off:
            lod["cloth_vb"] = skm.Obj(strip=(1, 0), data=(64, off, blob), index_mapping=mapping)
            fix_buffers_size(s, lod)
    err = max(map_error(sim, s, cloth_lods, k) for k, sim in enumerate(sims_objs))
    # how far the drawn garment lies from its particles (the collision holds the particles; what lies further in can
    # still go into a leg): b4bdangle builds the simulation surface in the middle of the garment's layers
    ds = sorted(abs(r[3]) for sec in s.m["lods"][0]["sections"] for r in (sec.get("cloth_mapping") or []))
    if ds:
        log(f"cloth: drawn garment within {ds[len(ds) // 2]:.1f} cm (half of it) / {ds[int(0.9 * (len(ds) - 1))]:.1f} cm "
            f"(90 %) of its simulation surface")
    log(f"cloth: {bound} cloth section(s) bound, mapping error max {err:.3f} cm")
    return [x[1] for x in names]


def write_asset(s, k, sd, ai, ae, bone_names, G, mode, src, log, cloth_lods):
    """Simulation mesh sd -> clothing asset export ae. Returns (Sim, asset name, AssetGuid)."""
    pkg = s.pkg
    aname = ae["name"]
    tree, tend = uprops.parse(pkg, bytes(pkg.export_data(ae)))
    V = [blender_to_ue(p) for p in sd["verts"]]
    T = [tuple(t) for t in sd["tris"]]
    sim = Sim(V, T)
    # the Y flip (Blender -> UE) mirrors the winding: orient so stored normals point away from the garment's axis
    if sd.get("centre"):
        cx, cy = sd["centre"][0] * 100.0, -sd["centre"][1] * 100.0
    else:
        cx = sum(p[0] for p in V) / len(V); cy = sum(p[1] for p in V) / len(V)
    out = sum(dot(sim.N[i], (V[i][0] - cx, V[i][1] - cy, 0.0)) for i in range(len(V)))
    if out < 0:
        T = [(a, c, b) for a, b, c in T]; sim = Sim(V, T)
    L = sd["length_m"] * 100.0
    garment, tune = garment_profile(sd)
    share, ex, capd = tune["maxd"], tune.get("maxd_exp", 1.0), tune.get("maxd_cap", 1e9)
    maxd = [0.0 if d <= 1e-6 else max(0.5, min(capd, d ** ex * L * share)) for d in sd["depth"]]
    fixed = [x == 0.0 for x in maxd]
    if tune.get("self"):
        # self-collision: spheres a little under half the closest free spacing, so neighbours never fight at rest
        el = sorted(math.dist(V[a], V[b]) for t in T for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0]))
                    if not (fixed[a] and fixed[b]))
        tune.setdefault("self_radius", round(min(3.0, 0.4 * el[len(el) // 10]), 2))
        tune.setdefault("self_stiffness", 0.5)
    sci = self_collision_indices(V, fixed, tune["self_radius"]) if tune.get("self_radius") else []
    used = []
    for w in sd["weights"]:
        for b in w:
            if b.lower() not in used: used.append(b.lower())
    missing = [b for b in used if b not in bone_names]
    if missing: raise SystemExit(f"cloth: bones {missing} are not in the mesh")
    # collision: the physics asset's bodies and our capsules need their bones in UsedBoneNames too (the engine maps
    # collision bones through it; retail assets list their physics asset's bones even without weights on them)
    pa_i = uprops.find(tree, "PhysicsAsset")
    pa_path = pkg.obj_path(pa_i.value) if pa_i and pa_i.value else None
    caps = [dict(c, a=blender_to_ue(c["a"]), b=blender_to_ue(c["b"]),
                 **{k: c[k] * 100.0 for k in ("r", "ra", "rb", "r_min") if k in c})
            for c in (sd.get("collision") or [])] if mode != "pa" else []
    # NvCloth takes at most 32 collision spheres (a capsule is two); the physics asset's come first, our capsules
    # fill the rest, legs first (a skirt or coat passes through a leg it doesn't collide with)
    pa_n = pa_spheres(pa_path, src)
    room = max(0, (MAX_SPHERES - (pa_n if pa_n is not None else 12)) // 2)
    if len(caps) > room:
        order = lambda c: next((i for i, b in enumerate(CAPSULE_PRIORITY) if c["bone"] == b), len(CAPSULE_PRIORITY))
        keep = sorted(caps, key=order)[:room]
        log(f"cloth: {len(caps) - len(keep)} capsule(s) left out ({', '.join(c['bone'] for c in caps if c not in keep)}):"
            f" NvCloth takes {MAX_SPHERES} spheres, the physics asset has {pa_n if pa_n is not None else '~12'}")
        caps = [c for c in caps if c in keep]
    for b in pa_bones(pa_path, src) + [c["bone"] for c in caps]:
        if b in bone_names and b not in used: used.append(b)
    lod_items = uprops.find(tree, "LodData").value["items"]
    tmpl = lod_items[0]
    tpm = uprops.find(tmpl, "PhysicalMeshData").value
    bd_el = uprops.find(tpm, "BoneData").value["items"][0]
    bone_data = [_bone_data(bd_el, sorted(((used.index(b.lower()), w) for b, w in wd.items()), key=lambda x: -x[1]))
                 for wd in sd["weights"]]
    trans, ntr = _transitions(sim, len(V))
    scale = float(os.environ.get("B4B_CLOTH_COLLISION_SCALE", "1"))
    spheres, conns = collision_data(pkg, caps, used, G, bone_names, scale) if caps else ([], [])
    new_lods = []
    for li in range(cloth_lods):
        el = copy.deepcopy(tmpl)
        pm = uprops.find(el, "PhysicalMeshData").value
        _pm_set(pm, "Vertices", [struct.pack("<3f", *v) for v in V])
        _pm_set(pm, "Normals", [struct.pack("<3f", *n) for n in sim.N])
        _pm_set(pm, "Indices", [x for t in T for x in t])
        wm = uprops.find(pm, "WeightMaps").value["items"]
        for kk, v in wm:
            uprops.find(v, "Values").value = list(maxd) if kk == 1 else []
        _pm_set(pm, "InverseMasses", inverse_masses(V, T, fixed, sd["depth"], tune.get("hem_mass", 1.0)))
        _pm_set(pm, "BoneData", bone_data)
        _pm_set(pm, "MaxBoneWeights", max(len(w) for w in sd["weights"]))
        _pm_set(pm, "NumFixedVerts", sum(fixed))
        _pm_set(pm, "SelfCollisionIndices", list(sci))
        cd = uprops.find(el, "CollisionData")
        if cd is not None and isinstance(cd.value, list):
            sp, sc = uprops.find(cd.value, "Spheres"), uprops.find(cd.value, "SphereConnections")
            if sp is not None and sc is not None:
                sp.value["items"] = list(spheres); sc.value["items"] = list(conns)
        up = (struct.pack("<i", ntr) + trans) if li > 0 else struct.pack("<i", 0)
        down = (struct.pack("<i", ntr) + trans) if li < cloth_lods - 1 else struct.pack("<i", 0)
        el.tail = up + down
        new_lods.append(el)
    uprops.find(tree, "LodData").value["items"] = new_lods
    uprops.find(tree, "LodMap").value = list(range(cloth_lods))
    uprops.find(tree, "UsedBoneNames").value = [fname_str(pkg, b) for b in used]
    uprops.find(tree, "UsedBoneIndices").value = [bone_names.index(b) for b in used]
    guid = uprops.find(tree, "AssetGuid").value
    old = bytes(pkg.export_data(ae))
    pkg.set_export_data(ae, uprops.write(pkg, tree) + old[tend:])
    ce = config_export(pkg, tree)
    if ce is not None:
        write_config(pkg, ce, tune)
    else:
        log(f"cloth: {aname} has no ClothConfigNv export: its config is left as it is")
    log(f"cloth: tuned as {garment} (hem at {sd.get('hem_leg', 0.5):.2f} of the leg, {L:.0f} cm): damping "
        f"{tune.get('damping', NV_DEFAULTS['damping']):.2f}, gravity x{tune.get('gravity', 1.0):.2f}, drag "
        f"{tune.get('linear_drag', NV_DEFAULTS['linear_drag']):.2f}, inertia {tune.get('linear_inertia', 1.0):.2f}, "
        f"hem mass x{tune.get('hem_mass', 1.0):.1f}, max distance {share:.2f} x length ^{ex:.1f}"
        f"{f' (cap {capd:.0f} cm)' if capd < 1e8 else ''}"
        f"{f', self-collision r {tune['self_radius']:.1f} cm on {len(sci)} vertices' if sci else ''}")
    log(f"cloth: {sd.get('shape', 'tube')} {len(V)} vertices / {len(T)} triangles -> {aname} ({cloth_lods} LODs, max "
        f"distance up to {max(maxd):.0f} cm, {sum(fixed)} fixed, bones {used}"
        f"{f', {len(conns)} collision capsules' if conns else ''}"
        f"{f' + {pa_n} physics asset spheres' if pa_n else ''}"
        f"{', physics asset ' + pa_path.split('.')[-1] if pa_path else ''})")
    return sim, aname, guid


def two_sided_slot(s, slot):
    """A slot whose material instance is the two-sided variant of `slot`'s (retail: <x>_2sided_MI, a child of <x>_MI
    with the TwoSided override, same textures), else None. Cloth folds show the inside of a one-sided skirt."""
    import re
    def mi(i):
        o = s.pkg.obj(s.m["materials"][i]["material"]) or ""
        return o.lower()
    want = mi(slot)
    for i in range(len(s.m["materials"])):
        n = mi(i)
        if i != slot and n != want and re.sub(r"_?(2|two)_?sided", "", n) == want: return i
    return None


def fix_buffers_size(s, lod):
    """BuffersSize = byte size of the streamed block (skmgltf.build_lod computes it the same way)."""
    ar = skm.Ar(s.pkg); skm.ser_lod(ar, lod, s.m); full = len(ar.b)
    ar2 = skm.Ar(s.pkg)
    skm.ser_strip(ar2, lod["strip"]); ar2.bool32(False); ar2.bool32(True); ar2.packed_array("h", lod["required_bones"])
    ar2.array(lambda x: skm.ser_section(ar2, x), lod["sections"]); ar2.packed_array("h", lod["active_bones"]); ar2.u32(0)
    lod["buffers_size"] = full - len(ar2.b)


def map_error(sim, s, n, region=0):
    """Largest distance between a cloth vertex and its reconstruction from the simulation mesh in the bind pose."""
    worst = 0.0
    for lod in s.m["lods"][:n]:
        P = lod["positions"]["positions"]
        for sec in lod["sections"]:
            if not sec.get("cloth_tag") or max(0, sec.get("cloth_region", 0)) != region: continue
            for k, r in enumerate(sec.get("cloth_mapping") or []):
                i = r[12:15]; b = r[0:4]
                q = [sum(b[j] * (sim.V[i[j]][c] - sim.N[i[j]][c] * b[3]) for j in range(3)) for c in range(3)]
                worst = max(worst, math.dist(q, P[sec["base_vertex_index"] + k]))
    return worst
