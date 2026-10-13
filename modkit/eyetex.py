"""Eye textures without Blender (b4bmod convert): read/write 8-bit PNGs, find the iris in an eye texture laid out the
usual way for game eyes (a planar projection from the front, pupil at the middle) and cut it out as a retail-style
IrisColor image (the iris filling it, edge at IRIS_EDGE of the size). Pure Python (zlib only).
docs/investigations/mesh-mods.md §24.
"""
import math, struct, zlib

IRIS_EDGE = 0.47          # as blender/b4beyes.py: where the iris edge sits in an IrisColor image
CONTRAST_MIN = 0.10


def read_png(path):
    """(width, height, rows) of an 8-bit RGB/RGBA/grey PNG; rows = [bytes of RGBA per row], top row first."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n": raise ValueError(f"{path}: not a PNG")
    pos, idat, w = 8, b"", None
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", body)
            if depth != 8 or inter: raise ValueError(f"{path}: only 8-bit, non-interlaced PNGs")
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + n
    ch = {0: 1, 2: 3, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * ch
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(ch, stride): line[i] = (line[i] + line[i - ch]) & 255
        elif f == 2:
            for i in range(stride): line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride): line[i] = (line[i] + ((line[i - ch] if i >= ch else 0) + prev[i]) // 2) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                b = prev[i]; c = prev[i - ch] if i >= ch else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if ch == 4: rows.append(bytes(line))
        elif ch == 3: rows.append(bytes(b for i in range(w) for b in (line[3 * i], line[3 * i + 1], line[3 * i + 2], 255)))
        elif ch == 1: rows.append(bytes(b for v in line for b in (v, v, v, 255)))
        else: rows.append(bytes(b for i in range(w) for b in (line[2 * i],) * 3 + (line[2 * i + 1],)))
    return w, h, rows


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + r for r in rows)
    def chunk(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def _lum(rows, w, h, step):
    """Luminance (0..1) on a grid every `step` pixels: {(x, y): lum}."""
    out = {}
    for y in range(0, h, step):
        r = rows[y]
        for x in range(0, w, step):
            i = 4 * x
            out[(x, y)] = (0.299 * r[i] + 0.587 * r[i + 1] + 0.114 * r[i + 2]) / 255.0
    return out


def find_iris(w, h, rows):
    """(pupil u, pupil v, iris radius) in UV (image space, v down) of a front-planar eye texture, or (None, why)."""
    step = max(1, min(w, h) // 256)
    L = _lum(rows, w, h, step)
    near = [(k, v) for k, v in L.items() if math.hypot(k[0] / w - 0.5, k[1] / h - 0.5) < 0.12]
    if len(near) < 20: return None, "texture too small"
    vals = sorted(v for _, v in near)
    thr = vals[max(0, len(vals) * 6 // 100)]
    dark = [k for k, v in near if v <= thr]
    cu = sum(k[0] for k in dark) / len(dark) / w
    cv = sum(k[1] for k in dark) / len(dark) / h
    if math.hypot(cu - 0.5, cv - 0.5) > 0.06:
        return None, f"no pupil at the texture's middle ({cu:.2f}, {cv:.2f}): not a front-projected eye texture"
    rstep, nbins = 0.005, 90
    acc, cnt = [0.0] * nbins, [0] * nbins
    for k, v in L.items():
        b = int(math.hypot(k[0] / w - cu, k[1] / h - cv) / rstep)
        if b < nbins: acc[b] += v; cnt[b] += 1
    prof = [acc[i] / cnt[i] if cnt[i] else None for i in range(nbins)]
    mean = sum(p for p in prof if p is not None) / max(1, sum(1 for p in prof if p is not None))
    prof = [p if p is not None else mean for p in prof]
    sm = [(prof[max(0, i - 1)] + prof[i] + prof[min(nbins - 1, i + 1)]) / 3 for i in range(nbins)]
    lo, hi = int(0.08 / rstep), int(0.36 / rstep)
    k = max(range(lo, hi), key=lambda i: sm[i + 1] - sm[i])
    R = (k + 1) * rstep
    def ring(a, b):
        vs = sorted(v for kk, v in L.items() if a * R < math.hypot(kk[0] / w - cu, kk[1] / h - cv) < b * R)
        return vs[len(vs) // 2] if vs else None
    inner, outer = ring(0.5, 0.85), ring(1.15, 1.5)
    if inner is None or outer is None or outer - inner < CONTRAST_MIN:
        return None, f"no iris found (sclera {outer} vs iris {inner})"
    return (cu, cv, R), None


def iris_image(w, h, rows, cu, cv, R, size):
    """size x size RGBA rows: the iris (radius R around (cu, cv), UV) filling the image, edge at IRIS_EDGE."""
    half = R * 0.5 / IRIS_EDGE
    out = []
    for y in range(size):
        v = cv - half + (y + 0.5) / size * 2 * half
        fy = v * h - 0.5
        y0 = int(math.floor(fy)); ty = fy - y0
        r0, r1 = rows[min(h - 1, max(0, y0))], rows[min(h - 1, max(0, y0 + 1))]
        line = bytearray(4 * size)
        for x in range(size):
            u = cu - half + (x + 0.5) / size * 2 * half
            fx = u * w - 0.5
            x0 = int(math.floor(fx)); tx = fx - x0
            a, b = 4 * min(w - 1, max(0, x0)), 4 * min(w - 1, max(0, x0 + 1))
            for c in range(3):
                top = r0[a + c] * (1 - tx) + r0[b + c] * tx
                bot = r1[a + c] * (1 - tx) + r1[b + c] * tx
                line[4 * x + c] = int(round(top * (1 - ty) + bot * ty))
            line[4 * x + 3] = 255
        out.append(bytes(line))
    return out
