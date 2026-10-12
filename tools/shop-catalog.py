#!/usr/bin/env python3
"""Build and sign the add-on shop's catalog (#36, docs/investigations/shop.md §4): what the shop repo's workflow runs
when an add-on release is published, and what tools/shop-test.py uses for its local test catalog.

  shop-catalog.py build <entries.json> [-o catalog.json] [--allow-license SPDX]
  shop-catalog.py sign [--key PEM] <catalog.json>        -> catalog.json.sig (tools/sign-release.sh, ed25519)
  shop-catalog.py verify [--pub PEM|64 hex] <catalog.json>
The shop has its own key, never the release key (native/src/signkeys.h, shop.md §2). Defaults: --key
~/.local/share/b4b-coop/keys/shop-signing.key (Dan's copy; the shop repo's workflow passes its secret), --pub
docs/shop-signing.pub.pem. The older form `sign <key> <catalog.json>` / `verify <pub> <catalog.json>` still works.

entries.json (the shop repo's; tools/shop-test.py calls its test copy shop.json; paths relative to it):
  {"url_template": "https://github.com/actuallydan/back4blood-shop/releases/download/{id}-v{version}/{id}.pak",
   "thumb_template": "https://raw.githubusercontent.com/actuallydan/back4blood-shop/main/thumbs/{thumb}",
   "addons": [{"id": "casual_joe", "pak": "paks/casual_joe.pak", "license": "CC0-1.0", "license_url": "https://...",
               "author": "...", "name": "(default: the addoninfo title)", "version": "(default: addoninfo, else 1.0)",
               "description": "(default: addoninfo)", "thumb": "casual_joe.png", "min_b4bcoop": "0.8.0"}]}
Everything the agent relies on is computed from the pak here (size, SHA-256, content id, class, what it adds or
replaces); the pak must be one of our add-ons (modkit/addon.py format) and carry a known public license.
"""
import argparse, hashlib, json, os, re, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "modkit"))
import addon  # noqa: E402  (modkit/addon.py: reads our paks, classifies them like the agent)

# Public licenses the shop takes without --allow-license (content: Creative Commons; code-style licenses for tools/art
# released that way). "All rights reserved", NC/ND variants and unknown strings are refused.
LICENSES = {"CC0-1.0", "CC-BY-3.0", "CC-BY-4.0", "CC-BY-SA-3.0", "CC-BY-SA-4.0", "MIT", "Apache-2.0", "BSD-2-Clause",
            "BSD-3-Clause", "OFL-1.1", "Unlicense"}
SHOP_KEY = os.path.expanduser("~/.local/share/b4b-coop/keys/shop-signing.key")
SHOP_PUB = os.path.join(REPO, "docs", "shop-signing.pub.pem")
ID_RX = re.compile(r"^[a-z0-9][a-z0-9_-]{0,31}$")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()


def what_it_does(info, files):
    """(adds, replaces) lists shown to players: outfit=/weapon= lines and new files under Gobi/Content/b4bcoop/ add;
    every other package replaces the game's own file of that path."""
    adds = []
    for v in info.get("outfit", []):
        f = v.split("|")
        adds.append(f"outfit {f[0]}")   # any survivor can wear it: its base survivor is no news to a player
    for v in info.get("weapon", []):
        f = v.split("|")
        adds.append(f"weapon look {f[0]}" + (f" ({f[1]})" if len(f) > 1 and f[1] else ""))
    new = [n for n in files if n.lower().startswith("gobi/content/b4bcoop/")]
    if new and not adds: adds.append(f"{len(new)} new file(s)")
    pk = sorted({os.path.splitext(n)[0] for n in files if not n.lower().startswith("gobi/content/b4bcoop/")})
    replaces = [os.path.basename(p) for p in pk[:3]] + ([f"and {len(pk) - 3} more"] if len(pk) > 3 else [])
    return adds, replaces


def build(a):
    src = json.load(open(a.manifest))
    base = os.path.dirname(os.path.abspath(a.manifest))
    url_t = src.get("url_template", "https://github.com/actuallydan/back4blood-shop/releases/download/{id}-v{version}/{id}.pak")
    thumb_t = src.get("thumb_template", "https://raw.githubusercontent.com/actuallydan/back4blood-shop/main/thumbs/{thumb}")
    allow = set(LICENSES) | set(a.allow_license or [])
    out, seen, bad = [], set(), 0
    for e in src.get("addons", []):
        aid = e.get("id", "")
        def refuse(why):
            nonlocal bad
            bad += 1
            print(f"refused {aid or '?'}: {why}", file=sys.stderr)
        if not ID_RX.match(aid): refuse("id must be [a-z0-9][a-z0-9_-]*, at most 32 characters"); continue
        if aid in seen: refuse("duplicate id"); continue
        lic = e.get("license", "")
        if lic not in allow: refuse(f"license {lic!r} is not a known public license ({', '.join(sorted(LICENSES))})"); continue
        pak = os.path.join(base, e.get("pak", f"paks/{aid}.pak"))
        try:
            cid, info, files, (gameplay, kinds, reasons) = addon.load_addon(pak, True)
        except (OSError, SystemExit, ValueError, KeyError) as x:
            refuse(f"{pak}: not a b4bcoop add-on pak ({x})"); continue
        version = e.get("version") or info.get("version") or "1.0"
        adds, replaces = what_it_does(info, files)
        it = {"id": aid, "name": e.get("name") or info.get("title") or aid, "author": e.get("author") or info.get("author", ""),
              "license": lic, "version": version, "class": "gameplay" if gameplay else "cosmetic", "kinds": ", ".join(kinds),
              "adds": adds, "replaces": replaces, "description": e.get("description") or info.get("description", ""),
              "size": os.path.getsize(pak), "sha256": sha256(pak), "content_id": cid}
        if e.get("license_url"): it["license_url"] = e["license_url"]
        it["url"] = e.get("url") or url_t.format(id=aid, version=version)
        if e.get("thumb"):
            tp = os.path.join(base, e.get("thumb_path", os.path.join("thumbs", e["thumb"])))
            if os.path.getsize(tp) > (1 << 20): refuse(f"thumbnail {tp} is over 1 MB"); continue
            it["thumb"] = e.get("thumb_url") or thumb_t.format(thumb=e["thumb"], id=aid, version=version)
            it["thumb_sha256"] = sha256(tp)
        if e.get("min_b4bcoop"): it["min_b4bcoop"] = e["min_b4bcoop"]
        seen.add(aid)
        out.append(it)
        print(f"{aid}: {it['name']} v{version}, {lic}, {it['class']}, {it['size']} bytes, adds {adds or '-'}, replaces {replaces or '-'}")
    cat = {"b4bcoop-shop": 1, "updated": time.strftime("%Y-%m-%d", time.gmtime()), "addons": out}
    data = json.dumps(cat, indent=1, ensure_ascii=False).encode() + b"\n"
    if len(data) > (4 << 20): sys.exit("catalog over 4 MB (the agent's limit)")
    open(a.out, "wb").write(data)
    print(f"{a.out}: {len(out)} add-on(s){f', {bad} refused' if bad else ''}")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build"); b.add_argument("manifest"); b.add_argument("-o", "--out", default="catalog.json")
    b.add_argument("--allow-license", action="append")
    s = sub.add_parser("sign"); s.add_argument("--key", default=SHOP_KEY); s.add_argument("args", nargs="+", metavar="catalog.json")
    v = sub.add_parser("verify"); v.add_argument("--pub", default=SHOP_PUB); v.add_argument("args", nargs="+", metavar="catalog.json")
    a = ap.parse_args()
    sr = os.path.join(REPO, "tools", "sign-release.sh")
    if a.cmd == "build": sys.exit(build(a))
    if len(a.args) > 2: ap.error(f"{a.cmd}: one catalog (or the older form: <key> <catalog.json>)")
    key, cat = (a.args if len(a.args) == 2 else [a.key if a.cmd == "sign" else a.pub, a.args[0]])
    if a.cmd == "sign": sys.exit(subprocess.run([sr, "sign", key, cat]).returncode)
    sys.exit(subprocess.run([sr, "verify", key, cat]).returncode)


if __name__ == "__main__":
    main()
