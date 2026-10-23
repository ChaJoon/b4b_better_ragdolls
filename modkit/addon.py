#!/usr/bin/env python3
"""b4bcoop add-on packer (#20; docs/investigations/addons.md, player side: docs/COMMANDS.md "Add-ons").
Mod makers run it through `b4bmod pack / check` (modkit/README.md).

An add-on is one .pak (modkit/b4bpak.py format: v9, B4B footer, uncompressed, unencrypted; v8 paks load too) that
the agent mounts from <game>/b4bcoop-addons/ after the retail paks. Besides the cooked files it holds
`b4bcoop-addoninfo.txt` (title,
author, version, category, description; `outfit=` / `weapon=` lines for added outfits and weapon looks, `b4bmod
survivor|weapon --as`), which the agent shows in /addons.

  addon.py pack <src> [-o OUT.pak] [--name N] [--title T] [--author A] [--version V] [--category C]
                [--description D] [--zip]
      <src> is a folder of cooked files laid out like the game (Gobi/Content/..., Engine/Content/...), as
      `dumpassets` extracts them and the modkit tools write them, with an optional addoninfo.txt at its top
      (`key=value` lines, or L4D's `addontitle "..."` KeyValues). <src> may also be one of our uncompressed .pak
      files (e.g. from `b4bpak.py pack`): its files are repacked with the addoninfo.
      --zip also writes OUT.zip = b4bcoop-addons/<name>.pak, which players unzip into the game folder.
      The add-on's content class (cosmetic or gameplay-affecting, from the files: see classify) is written to the
      addoninfo as `content=`; the agent derives it again at load and never trusts the file.
  addon.py info <addon.pak>          addoninfo, content id (index SHA1), content class and files
  addon.py check <addons dir>        what the agent will do with that folder: load order from addonlist.txt, on/off,
                                     conflicts (same file in two enabled add-ons; the later one wins)
Never commit or ship game assets: build add-ons under ~/.local/share/b4b-coop/.
"""
import argparse, hashlib, io, os, re, struct, sys, zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import b4bpak  # noqa: E402

INFO = "b4bcoop-addoninfo.txt"
KEYS = ("title", "author", "version", "category", "description", "content")
CATEGORIES = ("survivors", "ridden", "weapons", "items", "ui", "sounds", "maps", "misc")
PKG_EXT = (".uasset", ".uexp", ".ubulk", ".uptnl", ".umap")
ENTRY_HDR = 53


def parse_info(text):
    """Same rules as the agent (addons.c parse_info): key=value, or KeyValues `addontitle "value"`."""
    out = {}
    for line in text.splitlines():
        l = line.strip()
        if not l or l[0] in "#;{}" or l.startswith("//"):
            continue
        if "=" in l:
            k, v = l.split("=", 1)
        elif l.startswith('"'):
            q = l.find('"', 1)
            if q < 0:
                continue
            k, v = l[1:q], l[q + 1:]
        else:
            k, _, v = l.partition(" ") if " " in l else l.partition("\t")
        k, v = k.strip().strip('"').lower(), v.strip()
        if len(v) >= 2 and v[0] == '"' and v[-1] == '"':
            v = v[1:-1]
        if k.startswith("addon"):
            k = k[5:]
        if k in KEYS:
            out[k] = v
        elif k in ("outfit", "weapon"):   # added outfits / weapon looks (several lines each)
            out.setdefault(k, []).append(v)
    return out


OUTFIT_RX = re.compile(r"^[a-z][a-z0-9_]{0,31}$")


def check_outfits(info, names):
    """outfit= lines (same rules as the agent, native/src/addons.c): errors for the packer."""
    errs, have = [], {n.lower() for n in names}
    for v in info.get("outfit", []):
        f = v.split("|")
        if len(f) < 3 or not OUTFIT_RX.match(f[0]):
            errs.append(f"outfit={v}: expected <name>|<survivor>|<3P mesh>|<FP mesh>|<title>, name of a-z 0-9 _")
            continue
        for p in f[2:4]:
            if not p:
                continue
            pkg = p.split(".")[0]
            if not pkg.startswith("/Game/") or f"gobi/content/{pkg[6:]}.uasset".lower() not in have:
                errs.append(f"outfit {f[0]}: {p} is not in this add-on")
    for v in info.get("weapon", []):   # name|weapon code|FP mesh|3P static mesh|3P skeletal mesh|title
        f = v.split("|")
        if len(f) < 3 or not OUTFIT_RX.match(f[0]) or not re.fullmatch(r"[A-Za-z0-9]{1,15}", f[1]):
            errs.append(f"weapon={v}: expected <name>|<weapon code>|<FP mesh>|<3P static mesh>|<3P skeletal mesh>|<title>")
            continue
        if not any(f[2:5]):
            errs.append(f"weapon {f[0]}: no mesh")
        for p in f[2:5]:
            if not p:
                continue
            pkg = p.split(".")[0]
            if not pkg.startswith("/Game/") or f"gobi/content/{pkg[6:]}.uasset".lower() not in have:
                errs.append(f"weapon {f[0]}: {p} is not in this add-on")
    return errs


# ---- content class (#22). Same rules as native/src/addonclass.c: keep both in sync. ----
# A package is cosmetic when every export's class is in COSMETIC (module, class; "X*" = prefix, None = whole module);
# anything else is gameplay-affecting. SkeletalMesh also needs an imported Skeleton (a game skeleton).
COSMETIC = [
    ("/Script/Engine", c, "textures") for c in ("Texture2D", "TextureCube", "Texture2DArray", "VolumeTexture",
                                                "TextureRenderTarget2D", "TextureRenderTargetCube", "TextureLightProfile")
] + [
    ("/Script/Engine", c, "materials") for c in ("Material", "MaterialInstanceConstant", "MaterialFunction*",
                                                 "SubsurfaceProfile")
] + [("/Script/Gobi", "DrenchAssetUserData", "materials")   # an MI's wet-look parameter overrides
] + [
    ("/Script/Engine", c, "meshes") for c in ("SkeletalMesh", "SkeletalMeshSocket", "MorphTarget",
                                              "SkeletalMeshLODSettings", "StaticMesh", "StaticMeshSocket",
                                              "BodySetup", "NavCollision")   # a static mesh's own collision
] + [("/Script/NavigationSystem", "NavCollision", "meshes")] + [
    ("/Script/Engine", c, "animations") for c in ("AnimSequence", "AnimMontage", "AnimComposite", "BlendSpace*",
                                                  "AimOffsetBlendSpace*", "PoseAsset")
] + [
    ("/Script/Engine", c, "sounds") for c in ("SoundWave", "SoundCue", "SoundClass", "SoundMix", "SoundAttenuation",
                                              "SoundConcurrency", "SoundSubmix", "ReverbEffect", "SoundNode*")
] + [("/Script/ClothingSystemRuntimeCommon", None, "meshes"), ("/Script/ClothingSystemRuntimeNv", None, "meshes"),
     ("/Script/AkAudio", None, "sounds"), ("/Script/UMG", None, "ui"), ("/Script/MovieScene", None, "ui"),
     ("/Script/MovieSceneTracks", None, "ui")] + [
    ("/Script/Engine", c, "ui") for c in ("Font", "FontFace", "StringTable", "SlateBrushAsset")
] + [("/Script/SlateCore", "SlateWidgetStyleAsset", "ui")] + [
    ("/Script/CoreUObject", c, "ui") for c in ("Function", "DelegateFunction", "SparseDelegateFunction")
] + [
    ("/Script/Engine", c, "effects") for c in ("ParticleSystem", "ParticleEmitter", "ParticleSpriteEmitter",
                                               "ParticleLODLevel", "ParticleModule*", "Distribution*",
                                               "InterpCurveEdSetup")
] + [("/Script/Niagara", None, "effects")]
GAMEPLAY = [("PhysicsAsset", "physics asset"), ("SkeletalBodySetup", "physics asset"),
            ("PhysicsConstraintTemplate", "physics asset"), ("PhysicalMaterial", "physical material"),
            ("DataTable", "data table"),
            ("GuidDataTable", "data table"), ("CompositeDataTable", "data table"), ("CurveTable", "curve table"),
            ("CompositeCurveTable", "curve table"), ("Curve*", "curve"), ("BlueprintGeneratedClass", "blueprint"),
            ("AnimBlueprintGeneratedClass", "animation blueprint"), ("Skeleton", "skeleton"),
            ("World", "map"), ("LevelSequence", "level sequence")]
KINDS = ("textures", "materials", "meshes", "sounds", "ui", "effects", "animations")


def _match(name, pat):
    return name.startswith(pat[:-1]) if pat.endswith("*") else name == pat


def _judge(module, cls):
    """(True, kind) or (False, why)"""
    for m, c, kind in COSMETIC:
        if module == m and (c is None or _match(cls, c)):
            return True, kind
    for c, why in GAMEPLAY:
        if _match(cls, c):
            return False, why
    return False, cls or "unknown class"


def judge_package(b):
    """One cooked .uasset header -> (True, {kinds}) or (False, why). B4B: legacy -7, unversioned, 104-byte exports."""
    try:
        tag, legacy = struct.unpack_from("<Ii", b, 0)
        if tag != 0x9E2A83C1 or legacy != -7:
            return False, "unreadable package"
        p = 20
        ncv = struct.unpack_from("<i", b, p)[0]; p += 4 + ncv * 20 + 4           # custom versions, TotalHeaderSize

        def fstr(p):
            n = struct.unpack_from("<i", b, p)[0]
            return p + 4 + (n if n >= 0 else -n * 2)
        p = fstr(p)                                                               # FolderName
        flags, nn, no = struct.unpack_from("<Iii", b, p); p += 12
        if not flags & 0x80000000:
            p = fstr(p)                                                           # LocalizationId
        p += 8                                                                    # GatherableTextData
        ec, eo, ic, io = struct.unpack_from("<iiii", b, p)
        names, p = [], no
        for _ in range(nn):
            n = struct.unpack_from("<i", b, p)[0]
            names.append(b[p + 4:p + 3 + n].decode("latin-1") if n > 0 else None)
            p += 4 + (n if n >= 0 else -n * 2) + 4
        nm = lambda i: names[i] if 0 <= i < len(names) else None  # noqa: E731
        imps = []
        for i in range(ic):
            _, _, cn, _, outer, on, _ = struct.unpack_from("<iiiiiii", b, io + i * 28)
            imps.append((nm(cn), outer, nm(on)))
        classes = [struct.unpack_from("<i", b, eo + e * 104)[0] for e in range(ec)]
    except (struct.error, IndexError):
        return False, "unreadable package"
    kinds, skel_mesh = set(), False
    skel_import = any(cn == "Skeleton" for cn, _, _ in imps)
    for ci in classes:
        if ci < 0 and -ci <= len(imps):
            cn, outer, on = imps[-ci - 1]
            if cn == "WidgetBlueprintGeneratedClass":
                kinds.add("ui"); continue
            if cn != "Class":
                return False, f"instance of blueprint {on}"
            module = imps[-outer - 1][2] if outer < 0 and -outer <= len(imps) else None
            ok, what = _judge(module, on)
            if not ok:
                return False, what
            skel_mesh |= on == "SkeletalMesh"
            kinds.add(what)
        elif 0 < ci <= len(classes):
            cc = classes[ci - 1]
            if cc < 0 and -cc <= len(imps) and imps[-cc - 1][2] == "WidgetBlueprintGeneratedClass":
                kinds.add("ui"); continue
            return False, "blueprint"
        else:
            return False, "unreadable package"
    if skel_mesh and not skel_import:
        return False, "skeletal mesh without a game skeleton"
    return True, kinds


def classify(items):
    """items: [(name, bytes)] (names like Gobi/Content/...). Returns (gameplay: bool, kinds: [..], reasons: [..])."""
    lower = {n.lower() for n, _ in items}
    kinds, reasons = set(), []
    for n, data in sorted(items):
        k, base = n.lower(), os.path.basename(n)
        stem, ext = os.path.splitext(k)
        if ext == ".uasset":
            ok, what = judge_package(data)
            if ok:
                kinds |= what
            else:
                reasons.append(f"{what}: {base}")
        elif ext == ".uexp":
            if stem + ".uasset" not in lower and stem + ".umap" not in lower:   # a map's header is its .umap
                reasons.append(f"part of a package without its .uasset: {base}")
        elif ext in (".ubulk", ".uptnl"):
            pass
        elif ext == ".umap":
            reasons.append(f"map: {base}")
        elif ext in (".locres", ".ufont"):
            kinds.add("ui")
        elif ext in (".bnk", ".wem"):
            kinds.add("sounds")
        elif ext in (".ushaderbytecode", ".ushadercode"):
            kinds.add("materials")
        elif ext == ".ini":
            reasons.append(f"config: {base}")
        else:
            reasons.append(f"file type: {base}")
    return bool(reasons), [k for k in KINDS if k in kinds], reasons


def class_label(gameplay, kinds, reasons):
    if gameplay:
        return f"gameplay ({len(reasons)} file{'s' if len(reasons) != 1 else ''})"
    return "cosmetic" + (f" ({', '.join(kinds)})" if kinds else "")


def info_text(info):
    return "".join(f"{k}={info[k]}\r\n" for k in KEYS if info.get(k)) + \
        "".join(f"{k}={v}\r\n" for k in ("outfit", "weapon") for v in info.get(k, []))


def check_footer(path, ft):
    """The agent's footer checks (native/src/pakfmt.c), same messages: v9 (222-byte footer) or v8 (221), B4B magic,
    plain unfrozen index inside the file. The index SHA1 is checked by b4bpak.read_index."""
    ver, fs = ft["version"], ft["footer_size"]
    if ft["magic"] != b4bpak.MAGIC or (ver, fs) not in ((9, 222), (8, 221)):
        why = "not a pak file" if ft["file_size"] < 221 else \
            f"damaged, or not a Back 4 Blood add-on pak (magic {ft['magic']:#x} v{ver})"
        raise SystemExit(f"{path}: {why}")
    if ft["encrypted_index"]:
        raise SystemExit(f"{path}: encrypted pak index: not made with modkit/addon.py")
    end = ft["file_size"] - fs
    if ft["frozen"] or ft["index_size"] > 64 << 20 or ft["index_offset"] + ft["index_size"] > end:
        raise SystemExit(f"{path}: damaged pak (bad index position)")


def read_pak_files(path):
    """(name relative to ../../../, bytes) of every file in one of our uncompressed paks."""
    with open(path, "rb") as f:
        check_footer(path, b4bpak.read_footer(f))
    ft, mount, entries = b4bpak.read_index(path)
    prefix = mount
    while prefix.startswith("../"):
        prefix = prefix[3:]
    out = []
    with open(path, "rb") as f:
        for e in entries:
            if e["method"]:
                raise SystemExit(f"{path}: {e['name']} is compressed; only uncompressed paks can be repacked")
            f.seek(e["offset"] + ENTRY_HDR)
            out.append((prefix + e["name"], f.read(e["size"])))
    return ft, out


def collect(src):
    """Files to pack from a folder: [(name, path)], plus addoninfo text if present."""
    files, info = [], None
    for root, dirs, names in os.walk(src):
        dirs.sort()
        for n in sorted(names):
            p = os.path.join(root, n)
            rel = os.path.relpath(p, src).replace(os.sep, "/")
            if rel.lower() in ("addoninfo.txt", INFO):
                info = open(p, encoding="utf-8-sig").read()
                continue
            files.append((rel, p))
    return files, info


def check_names(names):
    errs, warns = [], []
    for n in names:
        top = n.split("/", 1)[0]
        if top not in ("Gobi", "Engine"):
            errs.append(f"{n}: not under Gobi/ or Engine/ (lay the files out like the game: Gobi/Content/...)")
        if not n.isascii():
            errs.append(f"{n}: non-ASCII file name")
    lower = {n.lower() for n in names}
    for n in names:
        base, ext = os.path.splitext(n)
        if ext.lower() in (".uasset", ".umap") and (base + ".uexp").lower() not in lower:
            warns.append(f"{n}: no {os.path.basename(base)}.uexp next to it (cooked packages come as {ext.lower()} + .uexp)")
        if ext.lower() in (".uexp", ".ubulk") and (base + ".uasset").lower() not in lower and (base + ".umap").lower() not in lower:
            warns.append(f"{n}: no {os.path.basename(base)}.uasset next to it: part of a package only, mixes with the "
                         "game's own .uasset (may crash)")
    return errs, warns


def write_pak(dst, items, mount="../../../"):
    """items: [(name, bytes)], written in sorted order (deterministic output)."""
    items = sorted(items, key=lambda t: t[0])
    index = [b4bpak.fstring(mount), struct.pack("<i", len(items))]
    with open(dst, "wb") as f:
        for name, data in items:
            sha1 = hashlib.sha1(data).digest()
            off = f.tell()
            f.write(b4bpak.entry(0, len(data), sha1))
            f.write(data)
            index.append(b4bpak.fstring(name) + b4bpak.entry(off, len(data), sha1))
        idx = b"".join(index)
        ioff = f.tell()
        f.write(idx)
        f.write(struct.pack("<II", b4bpak.VERSION, b4bpak.MAGIC) + bytes(16) + b"\0" + hashlib.sha1(idx).digest() +
                struct.pack("<QQ", len(idx), ioff) + b"\0" + bytes(160))
    return hashlib.sha1(idx).hexdigest()


def safe_name(s):
    s = re.sub(r"[^A-Za-z0-9_-]+", "_", s).strip("_")
    return s or "addon"


def cmd_pack(a):
    src = a.src.rstrip("/\\")
    if os.path.isfile(src):
        _, items = read_pak_files(src)
        old = [d for n, d in items if n.lower() == INFO]
        items = [(n, d) for n, d in items if n.lower() != INFO]
        info = parse_info(old[0].decode("utf-8-sig", "replace")) if old else {}
    else:
        paths, text = collect(src)
        items = [(n, open(p, "rb").read()) for n, p in paths]
        info = parse_info(text) if text else {}
    for k in KEYS:
        if getattr(a, k):
            info[k] = getattr(a, k)
    if not items:
        raise SystemExit(f"{src}: no files")
    errs, warns = check_names([n for n, _ in items])
    errs += check_outfits(info, [n for n, _ in items])
    for w in warns:
        print("warning:", w, file=sys.stderr)
    if errs:
        raise SystemExit("\n".join(errs))
    name = safe_name(a.name or os.path.splitext(os.path.basename(a.out or src))[0])
    out = a.out or name + ".pak"
    info.setdefault("title", name)
    if info.get("category") and info["category"].lower() not in CATEGORIES:
        print(f"warning: category {info['category']!r} is not one of {', '.join(CATEGORIES)}", file=sys.stderr)
    gameplay, kinds, reasons = classify(items)
    if info.get("content") and info["content"].split()[0].lower() != ("gameplay" if gameplay else "cosmetic"):
        print(f"note: content={info['content']!r} replaced: the files say {'gameplay' if gameplay else 'cosmetic'}",
              file=sys.stderr)
    info["content"] = class_label(gameplay, kinds, reasons)
    for r in reasons[:10]:
        print("gameplay-affecting:", r, file=sys.stderr)
    if gameplay:
        print("note: hosts with addons_policy=cosmetic or none refuse gameplay-affecting add-ons (the default, any, lets them in)", file=sys.stderr)
    items.append((INFO, info_text(info).encode("utf-8")))
    cid = write_pak(out, items)
    print(f"{out}: {len(items) - 1} file(s) + addoninfo, content id {cid}")
    for k in KEYS:
        if info.get(k):
            print(f"  {k}: {info[k]}")
    for v in info.get("outfit", []):
        print(f"  outfit: {v.split('|')[0]} (in game: /model {v.split('|')[0]})")
    for v in info.get("weapon", []):
        print(f"  weapon look: {v.split('|')[0]} for the {v.split('|')[1] if '|' in v else '?'} (in game: /model {v.split('|')[0]})")
    if a.zip:
        zpath = os.path.splitext(out)[0] + ".zip"
        with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
            zi = zipfile.ZipInfo(f"b4bcoop-addons/{os.path.basename(out)}", date_time=(2020, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            z.writestr(zi, open(out, "rb").read())
        print(f"{zpath}: unzip into the game folder (creates b4bcoop-addons/{os.path.basename(out)})")


def load_addon(path, with_class=False):
    ft, items = read_pak_files(path)
    info, files = {}, []
    for n, d in items:
        if n.lower() == INFO:
            info = parse_info(d.decode("utf-8", "replace"))
        else:
            files.append(n)
    if with_class:
        return ft["index_sha1"], info, files, classify([(n, d) for n, d in items if n.lower() != INFO])
    return ft["index_sha1"], info, files


def cmd_info(a):
    cid, info, files, (gameplay, kinds, reasons) = load_addon(a.pak, True)
    print(f"{a.pak}: content id {cid} (short {cid[:8]}), {len(files)} file(s)")
    for k in KEYS:
        print(f"  {k}: {info.get(k, '')}")
    for v in info.get("outfit", []):
        print(f"  outfit: {v}")
    for v in info.get("weapon", []):
        print(f"  weapon: {v}")
    print(f"  class (from the files): {class_label(gameplay, kinds, reasons)}")
    for r in reasons:
        print("    gameplay-affecting:", r)
    for n in files:
        print("  ", n)


def read_list(d):
    order = []
    p = os.path.join(d, "addonlist.txt")
    if os.path.exists(p):
        for line in open(p, encoding="utf-8", errors="replace"):
            l = line.strip()
            if not l or l[0] in "#;" or "=" not in l:
                continue
            k, v = (s.strip().strip('"') for s in l.split("=", 1))
            if k and k.lower() not in (o[0].lower() for o in order):
                order.append((k, v.lower() not in ("0", "off", "false")))
    return order


def cmd_check(a):
    d = a.dir
    listed = read_list(d)
    present = {f.lower(): f for f in os.listdir(d) if f.lower().endswith(".pak")}
    order = [(present[k.lower()], on) for k, on in listed if k.lower() in present]
    known = {k.lower() for k, _ in listed}
    order += [(present[k], True) for k in sorted(present, key=str.lower) if k not in known]
    owner, conflicts = {}, {}
    for i, (f, on) in enumerate(order):
        try:
            cid, info, files, cls = load_addon(os.path.join(d, f), True)
            why, label = "", class_label(*cls)
        except SystemExit as e:
            cid, info, files, why, label = "-", {}, [], str(e), ""
        print(f"{i + 1}. {f} [{'on' if on else 'off'}] {info.get('title', f[:-4])} {info.get('version', '')}"
              f"{' ' + label if label else ''} id {cid[:8]}{'  NOT LOADED: ' + why if why else ''}")
        if not on:
            continue
        for n in files:
            k = n.lower()
            if k in owner and owner[k] != f:
                conflicts.setdefault((owner[k], f), []).append(n)
            owner[k] = f
    stems = {}
    for k, f in owner.items():
        base, ext = os.path.splitext(k)
        s = base if ext in PKG_EXT else k
        if s in stems and stems[s] != f:
            print(f"conflict: {stems[s]} and {f} mix parts of one asset ({s}): may crash, switch one off")
        stems.setdefault(s, f)
    for (lo, hi), names in conflicts.items():
        print(f"conflict: {hi} overrides {lo} ({len(names)} file(s), e.g. {names[0]})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack")
    p.add_argument("src")
    p.add_argument("-o", "--out")
    p.add_argument("--name", help="file name without .pak (default: from -o or the folder name)")
    for k in KEYS:
        p.add_argument("--" + k)
    p.add_argument("--zip", action="store_true")
    p = sub.add_parser("info"); p.add_argument("pak")
    p = sub.add_parser("check"); p.add_argument("dir")
    a = ap.parse_args()
    {"pack": cmd_pack, "info": cmd_info, "check": cmd_check}[a.cmd](a)


if __name__ == "__main__":
    main()
