"""Eyes on the game's eye material (b4bfit, `b4bmod survivor`): docs/investigations/mesh-mods.md §24.

A survivor's eyes use Master_Eye_Refractive_M (the template's "Eyes" slot): eye shading, a refracted iris, darker
corners. A model's eyes on the skin slot (skin shading, full-bright sclera, no corner shadow) stared in game. This
module puts them on the game's eye material instead:

  - the eye system: the model's eye-named materials (eyeballs) plus any other material whose faces all sit on those
    eyeballs, in front (a cornea/iris layer such as an RE Engine "refl" shell that carries the eye texture);
  - each eye is baked as seen from the front: a grid of rays along the survivor's forward axis (+X) onto the eye
    system's faces, each hit coloured from its material's base colour image at the hit's UV (the material's own
    UV mapping node applied), the front-most layer winning;
  - the iris is found in that picture: the pupil is the dark spot near the eyeball's middle, the iris edge the
    steepest brightening of the brightness profile around it; no clear iris (painted anime eyes, solid black or
    glowing eyes) -> the eyes stay where they were (the skin slot);
  - the eye faces get the retail eye UV layout: a planar projection from the front, pupil at (0.5, 0.5), iris radius
    IRIS_UV (the Iris UV Radius the material instance gets), u towards the survivor's left, v down;
  - two images for the material instance (a copy of the template's eye MI, b4bmodel): ScleraColor = the eye from the
    front in that layout, IrisColor = the iris filling the image (edge at IRIS_EDGE of the size, as the retail iris
    textures). Both eyes share them (averaged where both have a hit).

Everything here is deterministic: the regions probe (b4bmodel decides the slots from its result) and the 3P run get the
same answer on the same fitted model.
"""
import math, os, re
import bpy
from mathutils import Vector, Matrix
from mathutils.bvhtree import BVHTree
from mathutils.geometry import barycentric_transform

IRIS_UV = 0.17            # Iris UV Radius of the eye material (the retail survivors' 0.17-0.174)
IRIS_EDGE = 0.47          # the iris edge in the IrisColor image, as a share of its size from the centre
SCLERA_PX = 512           # ScleraColor size
IRIS_PX = 512             # IrisColor size
PROBE_PX = 192            # detection bake
PROBE_HALF = 0.018        # detection bake: +-1.8 cm around the eyeball's middle
IRIS_MIN, IRIS_MAX = 0.0030, 0.0095      # plausible iris radius (m): real eyes ~0.6 cm, stylised up to ~0.9
CONTRAST_MIN = 0.10       # sclera brighter than the iris by at least this (sRGB luminance 0..1)
COVER_MIN = 0.85          # share of the disc around the iris (1.3 x its radius) the bake must hit
LAYER_REACH = 1.6         # a layer's vertices within this x the eyeball radius of a ball centre (in front) are on it
OVERLAY_RX = re.compile(r"lash|brow(?!n)|eyeline|eye_?liner|shadow|occlu|tear|cornea|moisture|highlight|wet", re.I)


def _base(mat):
    return re.sub(r"\.\d{3}$", "", mat.name) if mat else "none"


def _parts(meshes, names):
    """[(mesh object, material index, base name)] of the given base material names."""
    want = {n.lower() for n in names}
    out = []
    for m in meshes:
        for i, mt in enumerate(m.data.materials):
            if _base(mt).lower() in want and any(p.material_index == i for p in m.data.polygons):
                out.append((m, i, _base(mt)))
    return out


def _verts(m, idx):
    mw = m.matrix_world
    vs = {v for p in m.data.polygons if p.material_index == idx for v in p.vertices}
    return [mw @ m.data.vertices[v].co for v in vs]


def _see_through(mat):
    """A material drawn with its image's alpha or a constant alpha below 1 (tear lines, eye shadow cards): left out
    of the eye (in front, it would colour the bake)."""
    if mat is None or not mat.node_tree: return False
    b = next((n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
    if b is None: return False
    a = b.inputs["Alpha"]
    return a.is_linked or a.default_value < 0.99


def _sphere(pts):
    import numpy as np
    P = np.array([tuple(p) for p in pts])
    A = np.c_[2 * P, np.ones(len(P))]
    b = (P ** 2).sum(1)
    c, *_ = np.linalg.lstsq(A, b, rcond=None)
    ctr = c[:3]
    r = math.sqrt(max(c[3] + float((ctr ** 2).sum()), 0.0))
    return Vector(ctr), r


def eye_system(meshes, eye_names, log):
    """The eye-named materials' eyeballs (per side: centre, radius) and the materials on them. Returns
    ({"l"|"r": (centre, radius)}, [material base names]) or (None, why)."""
    named = _parts(meshes, eye_names)
    if not named:
        return None, "no eye material on the model"
    pts = [p for m, i, _ in named for p in _verts(m, i)]
    mid = (min(p.y for p in pts) + max(p.y for p in pts)) / 2
    balls = {}
    for sd, sel in (("l", lambda p: p.y > mid), ("r", lambda p: p.y <= mid)):
        s = [p for p in pts if sel(p)]
        if len(s) < 8:
            return None, f"eye material faces only on one side ({len(s)} vertices on the {sd} side)"
        c, r = _sphere(s)
        if not 0.006 <= r <= 0.025:
            # not ball-shaped (a flat painted eye, a whole-face material named "eye"): the eye material needs a ball
            return None, f"the eye material's {sd} side is not eyeball-shaped (sphere radius {r * 100:.1f} cm)"
        balls[sd] = (c, r)
    mats = sorted({n for _, _, n in named})
    # other materials whose faces all lie on the front of the eyeballs (an opaque, textured cornea/iris shell such as
    # an RE Engine "refl" layer, often a little wider than the ball: it covers the lid opening)
    for m in meshes:
        for i, mt in enumerate(m.data.materials):
            n = _base(mt)
            if n in mats or OVERLAY_RX.search(n) or _see_through(mt): continue
            vs = _verts(m, i)
            if not vs: continue
            on = 0
            for p in vs:
                c, r = balls["l" if p.y > mid else "r"]
                if (p - c).length <= LAYER_REACH * r and p.x >= c.x: on += 1
            if on >= 0.95 * len(vs):
                mats.append(n)
                log(f"eyes: {n} sits on the eyeballs (an eye layer): part of the eyes")
    return balls, sorted(set(mats))


class _Sampler:
    """Base colour of a material at a UV: its image (with the shader's UV mapping node) or its colour."""
    def __init__(self, mat, path, maxpx=1024):
        import numpy as np
        self.px = None
        self.col = (1.0, 1.0, 1.0)
        self.map = None
        if mat is not None and mat.node_tree:
            b = next((n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
            if b:
                c = b.inputs["Base Color"].default_value
                self.col = tuple(_lin2srgb(x) for x in c[:3])
                tex = _image_node(b.inputs["Base Color"])
                if tex is not None and tex.inputs["Vector"].is_linked:
                    mp = tex.inputs["Vector"].links[0].from_node
                    if mp.type == "MAPPING":
                        self.map = (mp.vector_type, Vector(mp.inputs["Location"].default_value),
                                    mp.inputs["Rotation"].default_value.copy(), Vector(mp.inputs["Scale"].default_value))
        if path and os.path.isfile(path):
            try:
                img = bpy.data.images.load(path, check_existing=False)
            except RuntimeError:
                img = None
            if img is not None and img.size[0]:
                img.colorspace_settings.name = "Non-Color"           # the values as stored (sRGB)
                w, h = img.size
                if max(w, h) > maxpx:
                    img.scale(max(1, w * maxpx // max(w, h)), max(1, h * maxpx // max(w, h)))
                    w, h = img.size
                a = np.empty(w * h * 4, dtype=np.float32)
                img.pixels.foreach_get(a)
                self.px = a.reshape(h, w, 4)[:, :, :3]                # bottom row first (Blender)
                bpy.data.images.remove(img)

    def uv(self, u, v):
        if self.map is None: return u, v
        kind, loc, rot, sc = self.map
        p = Vector((u, v, 0.0))
        if kind == "POINT":
            p = rot.to_matrix() @ (p * sc) + loc
        elif kind == "TEXTURE":
            p = (rot.to_matrix().inverted() @ (p - loc)) / sc if all(sc) else p
        return p.x, p.y

    def at(self, u, v):
        if self.px is None: return self.col
        u, v = self.uv(u, v)
        h, w = self.px.shape[:2]
        x = (u % 1.0) * w - 0.5
        y = (v % 1.0) * h - 0.5
        x0, y0 = int(math.floor(x)), int(math.floor(y))
        fx, fy = x - x0, y - y0
        x0 %= w; y0 %= h
        x1, y1 = (x0 + 1) % w, (y0 + 1) % h
        p = self.px
        c = (p[y0, x0] * (1 - fx) + p[y0, x1] * fx) * (1 - fy) + (p[y1, x0] * (1 - fx) + p[y1, x1] * fx) * fy
        return (float(c[0]), float(c[1]), float(c[2]))


def _lin2srgb(c):
    return c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055


def _image_node(sock, depth=0):
    if not sock.is_linked or depth > 6: return None
    n = sock.links[0].from_node
    if n.type == "TEX_IMAGE": return n
    for inp in n.inputs:
        if inp.type in ("RGBA", "VECTOR", "VALUE") and inp.is_linked:
            r = _image_node(inp, depth + 1)
            if r is not None: return r
    return None


class _Side:
    """One eye's triangles (world positions, per-corner UVs, sampler) in a BVH."""
    def __init__(self, tris):
        self.tris = tris
        verts, faces = [], []
        for k, (a, b, c, *_rest) in enumerate(tris):
            verts += [a, b, c]
            faces.append((3 * k, 3 * k + 1, 3 * k + 2))
        self.bvh = BVHTree.FromPolygons(verts, faces, all_triangles=True)
        self.front = max(max(a.x, b.x, c.x) for a, b, c, *_r in tris) + 0.02

    def colour(self, y, z):
        """(colour, from an image) of the front-most eye surface at (y, z), or None."""
        hit = self.bvh.ray_cast(Vector((self.front, y, z)), Vector((-1.0, 0.0, 0.0)), 0.2)
        if hit[0] is None: return None
        a, b, c, ua, ub, uc, smp = self.tris[hit[2]]
        uv = barycentric_transform(hit[0], a, b, c, Vector((ua[0], ua[1], 0)), Vector((ub[0], ub[1], 0)),
                                   Vector((uc[0], uc[1], 0)))
        return smp.at(uv.x, uv.y), smp.px is not None


def _sides(meshes, names, balls, samplers):
    mid = (balls["l"][0].y + balls["r"][0].y) / 2
    tris = {"l": [], "r": []}
    for m, i, n in _parts(meshes, names):
        me = m.data
        me.calc_loop_triangles()
        uvl = me.uv_layers.active
        mw = m.matrix_world
        for t in me.loop_triangles:
            if t.material_index != i: continue
            P = [mw @ me.vertices[v].co for v in t.vertices]
            U = [tuple(uvl.data[l].uv) if uvl else (0.0, 0.0) for l in t.loops]
            sd = "l" if (P[0] + P[1] + P[2]).y / 3 > mid else "r"
            tris[sd].append((P[0], P[1], P[2], U[0], U[1], U[2], samplers[n]))
    return {sd: _Side(t) for sd, t in tris.items() if t}


def _bake(side, cy, cz, half, n):
    """n x n RGB (rows top first, columns towards the survivor's left = +Y) of the eye seen from the front over
    [cy-half, cy+half] x [cz-half, cz+half], the hit mask and where the hit surface has an image (an untextured
    eyeball behind a textured layer only has its flat colour)."""
    import numpy as np
    img = np.zeros((n, n, 3), np.float32)
    hit = np.zeros((n, n), bool)
    tex = np.zeros((n, n), bool)
    for r in range(n):
        z = cz + half - (r + 0.5) / n * 2 * half
        for c in range(n):
            res = side.colour(cy - half + (c + 0.5) / n * 2 * half, z)
            if res is not None:
                img[r, c] = res[0]; hit[r, c] = True; tex[r, c] = res[1]
    return img, hit, tex


def _lum(img):
    return img[..., 0] * 0.299 + img[..., 1] * 0.587 + img[..., 2] * 0.114


def find_iris(img, hit, tex, half):
    """Pupil (y, z offset from the bake's centre, m) and iris radius (m) in a detection bake, or (None, why). The
    iris is read from the surfaces with an image (tex) where there are any; coverage counts every surface."""
    import numpy as np
    n = img.shape[0]
    px = 2 * half / n
    L = _lum(img)
    use = tex if tex.any() else hit
    yy, xx = np.mgrid[0:n, 0:n]
    d = np.hypot(yy - n / 2 + 0.5, xx - n / 2 + 0.5) * px
    near = use & (d < 0.007)
    if near.sum() < 20: return None, "the eye has no surface in front of its middle"
    thr = np.percentile(L[near], 6)
    dark = near & (L <= thr)
    py, pxx = yy[dark].mean(), xx[dark].mean()
    rr = np.hypot(yy - py, xx - pxx) * px
    step = 0.0003
    edges = np.arange(0.0, 0.013, step)
    prof = np.array([L[use & (rr >= a) & (rr < a + step)].mean() if (use & (rr >= a) & (rr < a + step)).sum() >= 4
                     else np.nan for a in edges])
    if np.isnan(prof).all(): return None, "no eye surface around the pupil"
    sm = np.convolve(np.nan_to_num(prof, nan=float(np.nanmean(prof))), np.ones(3) / 3, mode="same")
    grad = np.diff(sm)
    lo, hi = int(IRIS_MIN / step), min(int(IRIS_MAX / step), len(grad) - 1)
    k = lo + int(np.argmax(grad[lo:hi]))
    R = (k + 1) * step
    inner = use & (rr > 0.5 * R) & (rr < 0.85 * R)
    outer = use & (rr > 1.15 * R) & (rr < 1.5 * R)
    if inner.sum() < 10 or outer.sum() < 10: return None, f"too little eye surface around the iris (r {R * 100:.2f} cm)"
    contrast = float(np.median(L[outer]) - np.median(L[inner]))
    disc = rr < 1.3 * R
    cover = float((hit & disc).sum() / max(1, disc.sum()))
    if contrast < CONTRAST_MIN:
        return None, f"no iris found (sclera {np.median(L[outer]):.2f} vs iris {np.median(L[inner]):.2f})"
    if cover < COVER_MIN:
        return None, f"the eye surface covers only {cover:.0%} around the iris"
    oy = (pxx + 0.5 - n / 2) * px              # +Y = towards the survivor's left
    oz = -(py + 0.5 - n / 2) * px
    return (oy, oz, R, contrast), None


def _fill(img, hit):
    """Texels without a hit take their nearest hit's colour (a few dilation passes), the rest the mean colour."""
    import numpy as np
    img, hit = img.copy(), hit.copy()
    for _ in range(64):
        if hit.all(): break
        acc = np.zeros_like(img); cnt = np.zeros(hit.shape, np.float32)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            h2 = np.roll(hit, (dy, dx), (0, 1)); i2 = np.roll(img, (dy, dx), (0, 1))
            acc += i2 * h2[..., None]; cnt += h2
        new = (~hit) & (cnt > 0)
        if not new.any(): break
        img[new] = acc[new] / cnt[new][:, None]
        hit = hit | new
    if not hit.all():
        img[~hit] = img[hit].mean(0) if hit.any() else 0.5
    return img


def _polar_fill(img, ok):
    """Iris image: texels inside the iris circle (radius 0.5 of the image) that the bake didn't see in the eye's
    image (the textured layer ends above the iris: RE Engine's lid-shaped shell) take the colour at the same radius
    from the nearest angle that was seen, so the iris stays round and keeps its pattern."""
    import numpy as np
    n = img.shape[0]
    na, nr = 720, n // 2
    ang = np.linspace(0, 2 * np.pi, na, endpoint=False)
    rad = (np.arange(nr) + 0.5)
    ys = np.clip((n / 2 - 0.5 - np.outer(rad, np.sin(ang))).round().astype(int), 0, n - 1)
    xs = np.clip((n / 2 - 0.5 + np.outer(rad, np.cos(ang))).round().astype(int), 0, n - 1)
    pol, pok = img[ys, xs], ok[ys, xs]                     # (nr, na, 3), (nr, na)
    for i in range(nr):
        good = np.nonzero(pok[i])[0]
        if len(good) == 0 or len(good) == na: continue
        bad = np.nonzero(~pok[i])[0]
        dist = np.abs(bad[:, None] - good[None, :])
        dist = np.minimum(dist, na - dist)
        pol[i, bad] = pol[i, good[np.argmin(dist, axis=1)]]
        pok[i, bad] = True
    out = img.copy()
    yy, xx = np.mgrid[0:n, 0:n]
    r = np.hypot(yy - n / 2 + 0.5, xx - n / 2 + 0.5)
    a = (np.arctan2(n / 2 - 0.5 - yy, xx - n / 2 + 0.5) % (2 * np.pi)) / (2 * np.pi) * na
    ri, ai = np.clip(r.astype(int), 0, nr - 1), a.astype(int) % na
    fix = (~ok) & (r < nr) & pok[ri, ai]
    out[fix] = pol[ri[fix], ai[fix]]
    return out, ok | fix


def _save(img, path):
    import numpy as np
    n = img.shape[0]
    im = bpy.data.images.new(os.path.basename(path), n, n, alpha=True)
    im.colorspace_settings.name = "Non-Color"
    a = np.ones((n, n, 4), np.float32)
    a[..., :3] = np.clip(img, 0, 1)
    im.pixels.foreach_set(np.ascontiguousarray(a[::-1]).ravel())
    im.filepath_raw = path; im.file_format = "PNG"
    im.save()
    bpy.data.images.remove(im)


def detect(meshes, eye_names, tex_of, log):
    """Find the eyes and their iris. tex_of(material) -> base colour image path or None. Returns a dict with the eye
    system's materials, per side (centre, pupil, radius) and the iris radius, or {"ok": False, "why": ...}."""
    balls, mats = eye_system(meshes, eye_names, log)
    if balls is None:
        return {"ok": False, "why": mats}
    mats_obj = {}
    for m, i, n in _parts(meshes, mats):
        mats_obj.setdefault(n, m.data.materials[i])
    samplers = {n: _Sampler(mt, tex_of(mt)) for n, mt in mats_obj.items()}
    if not any(s.px is not None for s in samplers.values()):
        return {"ok": False, "why": "the eyes have no colour image (nothing to take the iris from)"}
    sides = _sides(meshes, mats, balls, samplers)
    res = {"ok": True, "materials": mats, "sides": {}}
    for sd in ("l", "r"):
        if sd not in sides: return {"ok": False, "why": f"no eye faces on the {sd} side"}
        c, r = balls[sd]
        img, hit, tex = _bake(sides[sd], c.y, c.z, PROBE_HALF, PROBE_PX)
        found, why = find_iris(img, hit, tex, PROBE_HALF)
        if found is None:
            return {"ok": False, "why": f"{sd} eye: {why}"}
        oy, oz, R, contrast = found
        res["sides"][sd] = {"ball": [c.x, c.y, c.z], "ball_r": r, "pupil": [c.y + oy, c.z + oz], "iris_r": R,
                            "contrast": round(contrast, 3)}
    rl, rr = res["sides"]["l"]["iris_r"], res["sides"]["r"]["iris_r"]
    if max(rl, rr) > 1.35 * min(rl, rr):
        return {"ok": False, "why": f"the two irises differ too much ({rl * 100:.2f} / {rr * 100:.2f} cm)"}
    res["iris_r"] = (rl + rr) / 2
    res["_sides"] = sides
    return res


def layout(meshes, eye_names, tex_of, out_dir, log):
    """3P run: detect, bake ScleraColor/IrisColor into out_dir, give the eye faces the retail eye UVs. Returns the
    manifest extras part {"materials", "iris_uv", "sclera", "iris", "iris_cm"} or {"ok": False, "why"}."""
    import numpy as np
    d = detect(meshes, eye_names, tex_of, log)
    if not d.get("ok"): return d
    R = d["iris_r"]
    k = IRIS_UV / R                                         # UV per metre
    sides = d.pop("_sides")
    S, I = SCLERA_PX, IRIS_PX
    acc_s, n_s, t_s = np.zeros((S, S, 3), np.float32), np.zeros((S, S), np.float32), np.zeros((S, S), bool)
    acc_i, n_i = np.zeros((I, I, 3), np.float32), np.zeros((I, I), np.float32)
    for sd, info in d["sides"].items():
        py, pz = info["pupil"]
        img, hit, tex = _bake(sides[sd], py, pz, 0.5 / k, S)
        acc_s += img * hit[..., None]; n_s += hit; t_s |= tex
        img, hit, tex = _bake(sides[sd], py, pz, R * 0.5 / IRIS_EDGE, I)
        acc_i += img * tex[..., None]; n_i += tex             # the iris only from the eye's image
    sclera = acc_s / np.maximum(n_s, 1)[..., None]
    # surfaces without an image (an untextured eyeball behind a textured layer) take the sclera's own colour: the
    # median of the textured ring just outside the iris
    yy, xx = np.mgrid[0:S, 0:S]
    rr = np.hypot(yy - S / 2 + 0.5, xx - S / 2 + 0.5) / S / k
    ring = t_s & (rr > 1.15 * R) & (rr < 1.6 * R)
    if ring.sum() >= 10:
        sclera[(n_s > 0) & ~t_s] = np.median(sclera[ring], axis=0)
    sclera = _fill(sclera, n_s > 0)
    iris, ok = _polar_fill(acc_i / np.maximum(n_i, 1)[..., None], n_i > 0)
    iris = _fill(iris, ok)
    os.makedirs(out_dir, exist_ok=True)
    ps, pi = os.path.join(out_dir, "eye_sclera.png"), os.path.join(out_dir, "eye_iris.png")
    _save(sclera, ps); _save(iris, pi)
    # the eye faces: planar UVs from the front, pupil at (0.5, 0.5) (Blender UV: v up)
    mid = (d["sides"]["l"]["ball"][1] + d["sides"]["r"]["ball"][1]) / 2
    nf = 0
    for m, i, n in _parts(meshes, d["materials"]):
        me = m.data
        uvl = me.uv_layers.active or me.uv_layers.new(name="UVMap")
        mw = m.matrix_world
        for p in me.polygons:
            if p.material_index != i: continue
            cen = mw @ p.center
            py, pz = d["sides"]["l" if cen.y > mid else "r"]["pupil"]
            for li in p.loop_indices:
                v = mw @ me.vertices[me.loops[li].vertex_index].co
                uvl.data[li].uv = (0.5 + k * (v.y - py), 0.5 + k * (v.z - pz))
            nf += 1
    log(f"eyes: the game's eye material for {', '.join(d['materials'])} ({nf} faces): iris radius "
        f"{R * 100:.2f} cm (l {d['sides']['l']['iris_r'] * 100:.2f}, r {d['sides']['r']['iris_r'] * 100:.2f}), "
        f"sclera/iris contrast {d['sides']['l']['contrast']:.2f}/{d['sides']['r']['contrast']:.2f}; eye UVs from the "
        f"front, pupil centred, Iris UV Radius {IRIS_UV}")
    return {"ok": True, "materials": d["materials"], "iris_uv": IRIS_UV, "iris_cm": round(R * 100, 3),
            "sclera": ps, "iris": pi}
