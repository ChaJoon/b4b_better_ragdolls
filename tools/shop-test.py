#!/usr/bin/env python3
"""Live test of the add-on shop, the `~` window's Browse tab (#36, docs/investigations/shop.md "Tests"), on test
instance 1 with a local test catalog.

The source folder (--src, default ~/.local/share/b4b-coop/shop-test/src; never committed: test paks) holds shop.json
(tools/shop-catalog.py format, with "{base}" in url_template/thumb_template), paks/ and thumbs/. Expected ids:
casual_joe (an added outfit), ak47 (an added weapon look), holly_green (a replacement of a game file; its update is
paks/holly_magenta.pak), future (min_b4bcoop 9.0.0). The catalog is built and signed with a throwaway key and served
on 127.0.0.1; the instance runs the dev build under test with shop_catalog= / shop_pubkey= and its own empty add-ons
folder (addons_dir= in the test prefix). Steps:
  1. no add-ons folder at start; a list with a bad signature is refused; the good list shows 4 add-ons + thumbnails
  2. refused Add: needs a newer b4bcoop; damaged download (SHA-256); truncated download; nothing is left behind
  3. Add casual_joe (the tab's button) in Fort Hope, with the customization screen opened (and closed) before: mounted
     at once, in /model list outfits, its row on the customization screen opened again, /model casual_joe worn
  4. Add holly_green: added, applies after a restart (it replaces Holly's party portrait, which Fort Hope has loaded)
  5. in a mission: Add ak47: mounted at once, weapon look usable; Remove ak47: switched off, removed after restart
  6. restart: ak47 deleted and gone from addonlist.txt, holly_green and casual_joe mounted at start
  7. newer casual_joe (a v1.1 repack, made here) and holly_green in the list: casual_joe's Update is loaded at once
     (nothing of it is loaded) and worn; holly_green's waits for the restart (its portrait is in use); restart -> both
     new files in place
  8. casual_joe renamed to my_joe.pak + a copy: logged and noticed as the same add-on twice; the Browse tab shows
     casual_joe "installed as my_joe.pak" and refuses to add it again; /addons on for an add-on that was off at the
     start (ak_off.pak, the ak47 look) loads it at once
  9. multi.sh 3 (host addons_policy=cosmetic with casual_joe, client 2 without add-ons, client 3 without): client 2
     Adds casual_joe in the host's Fort Hope: loaded at once, worn; the host (with the add-on) shows the outfit on
     client 2's hero, client 3 (without) the survivor with the replicated row
 10. multi.sh 2, host addons_policy=none: client 2's Add is kept for the next start ("this host allows no add-ons")
--real instead: no shop_catalog=/shop_pubkey= (the build's own URL and shop key): the real shop repo's list is
fetched from GitHub, verified and shown with as many add-ons as it has; screenshot. Adds nothing.
Needs the build under test installed (launch/install.sh) and the lane's lock (taken here unless --no-lock).
  B4B_LANE=2 B4B_STEAM=flatpak B4B_GPU=4090 tools/shop-test.py [--no-lock] [--real]"""
import argparse, glob, hashlib, http.server, json, os, re, shutil, subprocess, sys, threading, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import e2e
from lane import GAME, ROOT, LANE

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORTRAIT = "/game/ui/textures/common/characters/heroes/image_holly_partyportrait"   # holly_green replaces it
BIN = os.path.join(GAME, "Gobi", "Binaries", "Win64")
OUT = f"/tmp/b4b-shop{'-l2' if LANE == 2 else ''}-{time.strftime('%Y%m%d-%H%M%S')}"
ADDONS = os.path.join(ROOT, "test1", "shop-addons")
results = []


def log(msg): e2e.log(msg)


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    log(f"{'PASS' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}")
    return ok


def sha(p):
    try: return hashlib.sha256(open(p, "rb").read()).hexdigest()
    except OSError: return None


def winpath(p): return "Z:" + p.replace("/", "\\")


class Shop:
    """The test catalog: built by tools/shop-catalog.py from --src, signed with a throwaway key, served here."""
    def __init__(self, src):
        self.src, self.dir = src, os.path.join(OUT, "catalog")
        os.makedirs(self.dir)
        self.key = os.path.join(OUT, "test-signing.key")
        subprocess.run(["openssl", "genpkey", "-algorithm", "ed25519", "-out", self.key], check=True, capture_output=True)
        self.pubhex = subprocess.run([os.path.join(REPO, "tools/sign-release.sh"), "pubhex", self.key], check=True,
                                     capture_output=True, text=True).stdout.strip()
        self.mode, self.swap = "ok", {}

    def build(self, port, overrides=None):
        m = json.load(open(os.path.join(self.src, "shop.json")))
        base = f"http://127.0.0.1:{port}"
        m["url_template"] = m["url_template"].replace("{base}", base)
        m["thumb_template"] = m["thumb_template"].replace("{base}", base)
        for e in m["addons"]:
            for k, v in (overrides or {}).get(e["id"], {}).items(): e[k] = v
        mp = os.path.join(self.src, ".shop-test.json")
        json.dump(m, open(mp, "w"))
        cat = os.path.join(self.dir, "catalog.json")
        r = subprocess.run([sys.executable, os.path.join(REPO, "tools/shop-catalog.py"), "build", mp, "-o", cat], capture_output=True, text=True)
        os.remove(mp)
        open(os.path.join(OUT, "catalog-build.log"), "a").write(r.stdout + r.stderr)
        subprocess.run([sys.executable, os.path.join(REPO, "tools/shop-catalog.py"), "sign", "--key", self.key, cat], check=True, capture_output=True)
        self.files = {"catalog.json": open(cat, "rb").read(), "catalog.json.sig": open(cat + ".sig", "rb").read()}
        for e in m["addons"]:
            self.files[f"{e['id']}.pak"] = os.path.join(self.src, e.get("pak", f"paks/{e['id']}.pak"))
            if e.get("thumb"): self.files[f"thumbs/{e['thumb']}"] = os.path.join(self.src, "thumbs", e["thumb"])
        log(f"catalog: {len(json.loads(self.files['catalog.json'])['addons'])} add-on(s), key {self.pubhex[:16]}...")


class Handler(http.server.BaseHTTPRequestHandler):
    shop: Shop = None

    def log_message(self, fmt, *a):
        with open(os.path.join(OUT, "server.log"), "a") as f: f.write(f"{time.strftime('%H:%M:%S')} {self.shop.mode} {fmt % a}\n")

    def do_GET(self):
        s, name = self.shop, self.path.lstrip("/")
        if name not in s.files:
            self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
        v = s.files[name]
        data = v if isinstance(v, bytes) else open(v, "rb").read()
        if s.mode == "badsig" and name == "catalog.json.sig": data = bytes([data[0] ^ 1]) + data[1:]
        if s.mode == "badpak" and name.endswith(".pak"): data = data[:5000] + bytes([data[5000] ^ 0x10]) + data[5001:]
        full = len(data)
        if s.mode == "truncated" and name.endswith(".pak"): data = data[: full // 3]
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(full))
        self.end_headers()
        try: self.wfile.write(data)
        except OSError: pass
        if len(data) != full: self.close_connection = True


def status():
    return e2e.agent(1, "shop", "status")


def row(s, aid):
    m = re.search(rf"^  {re.escape(aid)} \[([\w-]+)\] dl=(\S+) pending=(\d) thumb=(\S+) local=(\S*) msg=(.*)$", s, re.M)
    return m.groups() if m else None


def wait_shop(pred, t=180):
    return e2e.wait_for(lambda: (lambda s: s if pred(s) else None)(status()), t, 1) or status()


def wait_item(aid, t=300):
    """until the item's download is finished (dl done/error) and no worker runs"""
    return wait_shop(lambda s: "busy=0" in s and row(s, aid) and row(s, aid)[1] in ("done", "error", "-"), t)


def addonlist():
    try: return open(os.path.join(ADDONS, "addonlist.txt"), errors="replace").read()
    except OSError: return ""


def start(S, extra):
    S.ini_extra = ";".join(extra)
    ok = S.launch()
    up = e2e.wait_for(lambda: "shop:" in status() and e2e.has_hero(1), 300, 3)   # in Fort Hope with a hero
    return check("instance 1 up (Fort Hope)", ok and up)


def shot(name):
    e2e.screenshot(1, name)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-lock", action="store_true")
    ap.add_argument("--src", default=os.path.expanduser("~/.local/share/b4b-coop/shop-test/src"))
    ap.add_argument("--real", action="store_true", help="the real shop repo's list with the built-in key (no override)")
    a = ap.parse_args()
    os.makedirs(OUT)
    e2e.OUT = OUT
    log(f"lane {LANE}, game {GAME}, out {OUT}")
    lock = os.path.join(REPO, "launch", "gamelock.sh")
    if not a.no_lock: subprocess.run([lock, "acquire", "shop-test"], check=True)
    S = e2e.Session("shop", 1)
    try:
        run_real(S) if a.real else run(a, S)
    finally:
        S.stop()
        if not a.no_lock: subprocess.run([lock, "release", "shop-test"])
    w = max(len(n) for n, _, _ in results) if results else 10
    print("\n" + "\n".join(f"{'PASS' if ok else 'FAIL'}  {n:<{w}}  {d}" for n, ok, d in results))
    bad = sum(not ok for _, ok, _ in results)
    print(f"\n{len(results) - bad}/{len(results)} passed; artifacts in {OUT}")
    sys.exit(1 if bad else 0)


REAL_CATALOG = "https://raw.githubusercontent.com/actuallydan/back4blood-shop/main/catalog.json"


def run_real(S):
    """the real shop repo, no override: its signed list verified with the built-in shop key"""
    want = len(json.load(urllib.request.urlopen(REAL_CATALOG, timeout=30))["addons"])
    log(f"real catalog: {want} add-on(s)")
    if os.path.isdir(ADDONS): shutil.rmtree(ADDONS)
    if not start(S, [f"addons_dir={winpath(ADDONS)}"]): return
    s = status()
    check("no override: default list, shop key", f"list={REAL_CATALOG} key=shop" in s, s.splitlines()[1] if "\n" in s else s[:120])
    e2e.agent(1, "overlay", "open"); e2e.agent(1, "overlay", "tab", "Browse")
    e2e.agent(1, "overlay", "press", "Get the add-on list")
    s = wait_shop(lambda s: ("phase=ready" in s or "phase=error" in s) and "busy=0" in s, 90)
    msg = re.search(r"msg=(.*)", s).group(1)[:140] if "msg=" in s else s[:140]
    check(f"real list fetched and verified ({want} add-on(s))", "phase=ready" in s and f"items={want} " in s and (f"{want} add-on(s) in the list" in s if want else "its list is empty" in s), msg)
    check("netguard scope around the request", bool(S.logs[1].grep(r"netguard: updater scope closed", False)))
    time.sleep(2)
    shot("real-browse.png")


def run(a, S):
    shop = Shop(a.src)
    Handler.shop = shop
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    shop.build(port)
    log(f"test catalog on 127.0.0.1:{port}")
    if os.path.isdir(ADDONS): shutil.rmtree(ADDONS)
    extra = [f"shop_catalog=http://127.0.0.1:{port}/catalog.json", f"shop_pubkey={shop.pubhex}", f"addons_dir={winpath(ADDONS)}"]

    # 1. the list
    if not start(S, extra): return
    g = S.logs[1]
    check("no add-ons folder at start", bool(g.grep(r"addons: no add-ons folder", False)))
    e2e.agent(1, "overlay", "open"); e2e.agent(1, "overlay", "tab", "Browse")
    shop.mode = "badsig"
    e2e.agent(1, "overlay", "press", "Get the add-on list")
    s = wait_shop(lambda s: "phase=error" in s or "phase=ready" in s, 60)
    check("list with a bad signature refused", "phase=error" in s and "signature" in s, re.search(r"msg=(.*)", s).group(1)[:120] if "msg=" in s else s[:120])
    shop.mode = "ok"
    e2e.agent(1, "overlay", "press", "Get the add-on list")
    s = wait_shop(lambda s: "phase=ready" in s and "busy=0" in s and "items=4" in s, 90)
    check("list shown (4 add-ons)", "items=4" in s, re.search(r"msg=(.*)", s).group(1)[:120] if "msg=" in s else s[:120])
    time.sleep(2)
    s = status()
    th = {i: (row(s, i) or [None] * 6)[3] for i in ("casual_joe", "ak47", "holly_green")}
    check("thumbnails decoded and uploaded (PNG + JPEG)", all(v == "uploaded" for v in th.values()), str(th))
    check("netguard scope around the requests", bool(g.grep(r"netguard: updater scope closed", False)))
    shot("1-browse-list.png")

    # 2. refused downloads
    r = e2e.agent(1, "shop", "add", "future")
    check("Add refused: needs a newer b4bcoop", "needs b4bcoop 9.0.0" in r, r.splitlines()[0] if r else "")
    for mode, want in (("badpak", "damaged"), ("truncated", "")):
        shop.mode = mode
        e2e.agent(1, "shop", "add", "ak47")
        s = wait_item("ak47")
        rw = row(s, "ak47") or ("?",) * 6
        check(f"{mode}: download refused", rw[1] == "error" and want in rw[5], rw[5][:140])
        left = sorted(os.listdir(ADDONS)) + sorted(os.listdir(os.path.join(ADDONS, ".shop"))) if os.path.isdir(ADDONS) else []
        check(f"{mode}: nothing left behind", not [f for f in left if f.endswith((".pak", ".part"))], str(left))
    shop.mode = "ok"

    # 3. an added outfit, usable at once (also on a customization screen that was opened before)
    e2e.agent(1, "overlay", "close")
    cs = e2e.agent(1, "mdl", "cs", "open")
    time.sleep(3)
    before = e2e.agent(1, "mdl", "cs", "list")
    e2e.agent(1, "mdl", "cs", "close")
    time.sleep(2)
    check("customization screen opened and closed before the Add", "screen open" in cs and before.strip() and "casual_joe" not in before, cs.strip()[:80])
    e2e.agent(1, "overlay", "open"); e2e.agent(1, "overlay", "tab", "Browse")
    g.mark()
    e2e.agent(1, "overlay", "press", "Add##casual_joe")
    s = wait_item("casual_joe")
    rw = row(s, "casual_joe") or ("?",) * 6
    check("Add casual_joe: downloaded, verified, ready now", rw[1] == "done" and "ready now" in rw[5], rw[5][:140])
    check("casual_joe mounted at runtime", bool(g.grep(r"addons: casual_joe\.pak -> mounted \(read order \d+, at runtime\)")))
    check("addonlist.txt has casual_joe.pak=1", "casual_joe.pak=1" in addonlist())
    lst = e2e.agent(1, "model", "list", "outfits")
    check("/model list outfits (the Models tab's list) has casual_joe right after Add", "casual_joe" in lst, lst.strip().splitlines()[-1][:100] if lst.strip() else "")
    rows = e2e.wait_for(lambda: g.grep(r"models: \d+ customization-screen row\(s\) added"), 5, 0.5)
    check("customization-screen rows added at once", bool(rows), rows[0][:120] if rows else "")
    e2e.agent(1, "overlay", "close")
    cs = e2e.agent(1, "mdl", "cs", "open")
    time.sleep(3)
    after = e2e.agent(1, "mdl", "cs", "list")
    m = re.search(r"^.*b4bcoop\.outfit\.casual_joe.*$", after, re.M)
    check("customization screen opened again: the casual_joe outfit is listed, unlocked", bool(m) and "locked=0" in m.group(0), m.group(0).strip()[:120] if m else after.strip()[-120:])
    shot("3-customization-screen.png")
    e2e.agent(1, "mdl", "cs", "close")
    time.sleep(2)
    e2e.agent(1, "thirdperson", "on")
    out = e2e.agent(1, "model", "casual_joe")
    worn = e2e.wait_for(lambda: g.grep(r"models: hero slot \d+ wears outfit casual_joe\b"), 30, 1)
    check("/model casual_joe right after Add", bool(worn), out.strip().splitlines()[0][:100] if out.strip() else "")
    time.sleep(3)
    shot("3-casual_joe-worn.png")
    e2e.agent(1, "overlay", "open"); e2e.agent(1, "overlay", "tab", "Browse")
    time.sleep(1)
    shot("3-browse-after-add.png")

    # 4. a replacement of an asset the game has loaded (Holly's party portrait, in memory in Fort Hope): after a restart
    pl = e2e.agent(1, "paks", "loaded", PORTRAIT)
    g.mark()
    e2e.agent(1, "shop", "add", "holly_green")
    s = wait_item("holly_green")
    rw = row(s, "holly_green") or ("?",) * 6
    check("Add holly_green: applies after a restart (it replaces an asset in use now)", "in memory" in pl and rw[1] == "done" and
          "after a restart" in rw[5] and "using now" in rw[5], rw[5][:160])
    check("holly_green not mounted now", not g.grep(r"holly_green\.pak -> mounted"))

    # 5. in a mission: add + remove
    e2e.agent(1, "overlay", "close")
    e2e.agent(1, "mission", "Easy")
    inm = e2e.wait_for(lambda: e2e.MAP_B in S.world(1) and e2e.has_hero(1), 300, 5)
    if check("mission loaded", bool(inm), S.world(1)):
        e2e.agent(1, "ready")   # past the character select: the match runs
        time.sleep(20)
        g.mark()
        e2e.agent(1, "shop", "add", "ak47")
        s = wait_item("ak47")
        rw = row(s, "ak47") or ("?",) * 6
        check("in a mission: Add ak47 ready now", rw[1] == "done" and "ready now" in rw[5], rw[5][:140])
        check("ak47 mounted at runtime in the mission", bool(g.grep(r"addons: ak47\.pak -> mounted \(read order \d+, at runtime\)")))
        out = e2e.agent(1, "model", "ak47")
        check("weapon look ak47 usable right after Add", "now looks like" in out, out.strip().splitlines()[0][:120] if out.strip() else "")
        e2e.agent(1, "thirdperson", "on")
        time.sleep(20)
        check("game alive 20 s after the mission mount", e2e.MAP_B in S.world(1) and e2e.has_hero(1))
        shot("5-mission-ak47.png")
        e2e.agent(1, "overlay", "open"); e2e.agent(1, "overlay", "tab", "Browse")
        e2e.agent(1, "overlay", "press", "Remove##ak47"); e2e.agent(1, "overlay", "press", "Remove##ak47")   # confirm click
        time.sleep(2)
        s = status()
        rw = row(s, "ak47") or ("?",) * 6
        check("Remove ak47: pending, switched off", rw[0] == "remove-pending" and "ak47.pak=0" in addonlist(), f"{rw[0]} / {rw[5][:80]}")
        shot("5-browse-remove-pending.png")

    # 6. restart
    S.stop()
    time.sleep(3)
    if not start(S, extra): return
    g = S.logs[1]
    check("restart: ak47 deleted", not os.path.exists(os.path.join(ADDONS, "ak47.pak")) and bool(g.grep(r"shop: removed ak47\.pak", False)))
    check("restart: ak47 gone from addonlist.txt", "ak47" not in addonlist(), addonlist().replace("\r\n", " | ")[-160:])
    check("restart: holly_green mounted at start", bool(g.grep(r"addons: holly_green\.pak -> mounted \(read order", False)))
    check("restart: casual_joe mounted at start", bool(g.grep(r"addons: casual_joe\.pak -> mounted \(read order", False)))

    # 7. updates: casual_joe's (nothing of it loaded) at once, holly_green's (its portrait in use) after the restart
    joe11 = os.path.join(OUT, "casual_joe-1.1.pak")   # same files, other addoninfo: another pak (and content id)
    r = subprocess.run([sys.executable, os.path.join(REPO, "modkit/addon.py"), "pack", os.path.join(a.src, "paks", "casual_joe.pak"),
                        "-o", joe11, "--version", "1.1", "--title", "Casual Joe v1.1"], capture_output=True, text=True)
    open(os.path.join(OUT, "casual_joe-1.1.log"), "w").write(r.stdout + r.stderr)
    shop.build(port, {"holly_green": {"pak": "paks/holly_magenta.pak", "version": "1.1"}, "casual_joe": {"pak": joe11, "version": "1.1"}})
    e2e.agent(1, "shop", "fetch")
    s = wait_shop(lambda s: "phase=ready" in s and "busy=0" in s and "items=4" in s, 90)
    s = wait_shop(lambda s: all((row(s, i) or ["?"])[0] == "update-available" for i in ("holly_green", "casual_joe")), 30)
    check("newer holly_green and casual_joe offered as updates", all((row(s, i) or ["?"])[0] == "update-available" for i in ("holly_green", "casual_joe")),
          ", ".join(f"{i} {(row(s, i) or ['?'])[0]}" for i in ("holly_green", "casual_joe")))
    g.mark()
    e2e.agent(1, "shop", "add", "casual_joe")
    s = wait_item("casual_joe")
    rw = row(s, "casual_joe") or ("?",) * 6
    check("Update casual_joe (not loaded): ready now", rw[0] == "update-pending" and "Updated and ready now" in rw[5] and
          bool(g.grep(r"addons: update of casual_joe\.pak \(.*casual_joe\.pak\.new\) -> mounted \(read order \d+, at runtime")), rw[5][:120])
    info = e2e.agent(1, "slash", "addons", "info", "casual_joe")
    check("the add-on is the new version now", "Casual Joe v1.1 1.1" in info, info.strip().splitlines()[0][:100] if info.strip() else "")
    e2e.agent(1, "model", "casual_joe")
    worn = e2e.wait_for(lambda: g.grep(r"models: hero slot \d+ wears outfit casual_joe\b"), 30, 1)
    check("updated casual_joe worn right after the Update", bool(worn))
    r2 = e2e.agent(1, "shop", "add", "casual_joe")
    check("a second update in the same session is refused", "restart the game before the next update" in r2, r2.splitlines()[0] if r2 else "")
    g.mark()
    e2e.agent(1, "shop", "add", "holly_green")
    s = wait_item("holly_green")
    rw = row(s, "holly_green") or ("?",) * 6
    check("Update holly_green (portrait in use): waits for the restart", rw[0] == "update-pending" and "not now" in rw[5] and "using now" in rw[5] and
          os.path.exists(os.path.join(ADDONS, ".shop", "holly_green.pak.new")) and not g.grep(r"update of holly_green\.pak .* -> mounted"), rw[5][:160])
    e2e.agent(1, "overlay", "open"); e2e.agent(1, "overlay", "tab", "Browse")
    time.sleep(2)
    shot("7-browse-updates.png")
    S.stop()
    time.sleep(3)
    if not start(S, extra): return
    g = S.logs[1]
    want = sha(os.path.join(a.src, "paks", "holly_magenta.pak"))
    check("restart: the updated files are in place", sha(os.path.join(ADDONS, "holly_green.pak")) == want and bool(g.grep(r"shop: updated holly_green\.pak", False))
          and sha(os.path.join(ADDONS, "casual_joe.pak")) == sha(joe11) and bool(g.grep(r"shop: updated casual_joe\.pak", False)))

    # 8. the same add-on under other file names (a player's own copy, e.g. batman.pak for the shop's batman-any)
    S.stop()
    time.sleep(3)
    os.rename(os.path.join(ADDONS, "casual_joe.pak"), os.path.join(ADDONS, "my_joe.pak"))
    shutil.copy(os.path.join(ADDONS, "my_joe.pak"), os.path.join(ADDONS, "my_joe_copy.pak"))
    # and an add-on that is off at this start (for /addons on below): the ak47 weapon look under another name
    shutil.copy(os.path.join(a.src, "paks", "mod_ak47.pak"), os.path.join(ADDONS, "ak_off.pak"))
    with open(os.path.join(ADDONS, "addonlist.txt"), "a") as f: f.write("ak_off.pak=0\r\n")
    if not start(S, extra): return
    g = S.logs[1]
    check("same add-on twice: logged as a duplicate", bool(g.grep(r"addons: duplicate: my_joe\.pak and my_joe_copy\.pak are the same add-on", False)))
    check("same add-on twice: player notice", bool(g.grep(r"addons: notice: Add-on .* is installed twice \(my_joe\.pak and my_joe_copy\.pak\)", False)))
    e2e.agent(1, "shop", "fetch")
    s = wait_shop(lambda s: "phase=ready" in s and "busy=0" in s and "items=4" in s, 90)
    rw = row(s, "casual_joe") or ("?",) * 6
    check("Browse: casual_joe shown as installed (as my_joe.pak)", rw[0] == "installed-as" and "installed as my_joe.pak: same content" in rw[5], f"{rw[0]} / {rw[5][:100]}")
    r = e2e.agent(1, "shop", "add", "casual_joe")
    check("Browse: no second copy added", "already installed as my_joe.pak" in r and not os.path.exists(os.path.join(ADDONS, "casual_joe.pak")), r.splitlines()[0] if r else "")
    e2e.agent(1, "overlay", "open"); e2e.agent(1, "overlay", "tab", "Browse")
    time.sleep(2)
    shot("8-browse-installed-as.png")
    g.mark()
    r = e2e.agent(1, "slash", "addons", "on", "ak_off")
    check("/addons on (an add-on off at start): loaded at once", "on, ready now" in r and bool(g.grep(r"addons: ak_off\.pak -> mounted \(read order \d+, at runtime\)")),
          r.strip().splitlines()[0][:100] if r.strip() else "")
    lst = e2e.agent(1, "model", "list", "weapons")
    check("its weapon look is listed right after", "ak47" in lst, lst.strip().splitlines()[-1][:100] if lst.strip() else "")
    S.stop()
    time.sleep(3)
    try:
        run_client(a, extra[:2])
    finally:
        for k in ("B4B_INI_EXTRA1", "B4B_INI_EXTRA2", "B4B_INI_EXTRA3"): os.environ.pop(k, None)
    srv.shutdown()


def faces(n):
    """instance n's heroes (dev `face`): [(idx, mesh, you)]"""
    return [(int(m.group(1)), m.group(2), bool(m.group(3))) for m in
            re.finditer(r"^#(\d+) \S+ mesh=(\S+)(?: at=\([^)]*\))?( \(you\))?", e2e.agent(n, "face"), re.M)]


def fresh_dir(path, paks=()):
    if os.path.isdir(path): shutil.rmtree(path)
    os.makedirs(path)
    for p in paks: shutil.copy(p, path)
    return winpath(path)


def run_client(a, shop_lines):
    """9-10: Add as a client in someone else's Fort Hope"""
    joe = os.path.join(a.src, "paks", "casual_joe.pak")
    c2 = os.path.join(ROOT, "test2", "shop-addons")
    # 9. host (addons_policy=cosmetic, with casual_joe), client 2 (Adds it), client 3 (no add-ons)
    os.environ["B4B_INI_EXTRA1"] = f"addons_dir={fresh_dir(os.path.join(ROOT, 'test1', 'shop-addons-host'), [joe])};addons_policy=cosmetic"
    os.environ["B4B_INI_EXTRA2"] = f"addons_dir={fresh_dir(c2)}"
    os.environ["B4B_INI_EXTRA3"] = f"addons_dir={fresh_dir(os.path.join(ROOT, 'test3', 'shop-addons'))}"
    S = e2e.Session("shopmp", 3, ini_extra=";".join(shop_lines))
    try:
        up = S.launch() and e2e.wait_for(lambda: all(e2e.has_hero(i) for i in (1, 2, 3)), 180, 3)
        if not check("multi.sh 3: host + 2 clients in the host's Fort Hope", up): return
        h, c, v = S.logs[1], S.logs[2], S.logs[3]
        check("client 2 checked its add-ons against the host's policy (cosmetic)", bool(c.grep(r"addons: host \S+ asks us to check our add-ons against its policy cosmetic", False)))
        e2e.agent(2, "shop", "fetch")
        wait_shop2 = lambda pred, t: e2e.wait_for(lambda: (lambda s: s if pred(s) else None)(e2e.agent(2, "shop", "status")), t, 1) or e2e.agent(2, "shop", "status")
        wait_shop2(lambda s: "phase=ready" in s and "busy=0" in s and "items=4" in s, 90)
        for g in (h, c, v): g.mark()
        e2e.agent(2, "shop", "add", "casual_joe")
        s = wait_shop2(lambda s: "busy=0" in s and (row(s, "casual_joe") or ["", "x"])[1] in ("done", "error"), 300)
        rw = row(s, "casual_joe") or ("?",) * 6
        check("client: Add casual_joe in the host's Fort Hope: ready now", rw[1] == "done" and "ready now" in rw[5] and
              bool(c.grep(r"addons: casual_joe\.pak -> mounted \(read order \d+, at runtime\)")), rw[5][:140])
        e2e.agent(2, "model", "casual_joe")
        rx = r"models: hero slot \d+ wears outfit casual_joe\b"
        check("client: casual_joe worn at once", bool(e2e.wait_for(lambda: c.grep(rx), 30, 1)))
        check("host (with the add-on): the client's hero wears casual_joe", bool(e2e.wait_for(lambda: h.grep(rx), 30, 1)))
        hh = e2e.wait_for(lambda: next((f for f in faces(1) if not f[2] and "/b4bcoop/outfits/casual_joe/" in f[1].lower()), None), 15, 1)
        check("host shows the outfit's mesh on the client's hero", bool(hh), hh[1] if hh else str(faces(1))[:160])
        time.sleep(3)
        vf = faces(3)
        dump = e2e.agent(3, "mdl", "dump")
        check("client 3 (no add-on): the row arrives, the survivor's own meshes are shown", "b4bcoop.outfit.casual_joe" in dump and
              not any("/b4bcoop/" in f[1].lower() for f in vf) and not v.grep(rx), str([f[1].rsplit('/', 1)[-1] for f in vf])[:160])
        for i in (2, 3): e2e.agent(i, "popup", "close")   # the "asked to check" refusal's popup from the first join
        if hh:
            e2e.agent(1, "face", "look", hh[0], 260, 0)
            time.sleep(3)
            shot("9-host-sees-client-outfit.png")
        e2e.agent(2, "thirdperson", "on")
        time.sleep(3)
        e2e.screenshot(2, "9-client-wears-outfit-3p.png")
        e2e.screenshot(3, "9-client3-view.png")
    finally:
        S.stop()
    time.sleep(3)
    # 10. host addons_policy=none: the client's Add stays for the next start
    os.environ["B4B_INI_EXTRA1"] = f"addons_dir={fresh_dir(os.path.join(ROOT, 'test1', 'shop-addons-host'))};addons_policy=none"
    os.environ["B4B_INI_EXTRA2"] = f"addons_dir={fresh_dir(c2)}"
    os.environ.pop("B4B_INI_EXTRA3", None)
    S = e2e.Session("shopnone", 2, ini_extra=";".join(shop_lines))
    try:
        up = S.launch() and e2e.wait_for(lambda: all(e2e.has_hero(i) for i in (1, 2)), 180, 3)
        if not check("multi.sh 2 with host addons_policy=none", up): return
        c = S.logs[2]
        e2e.agent(2, "shop", "fetch")
        e2e.wait_for(lambda: "phase=ready" in e2e.agent(2, "shop", "status"), 90, 1)
        c.mark()
        e2e.agent(2, "shop", "add", "casual_joe")
        s = e2e.wait_for(lambda: (lambda s: s if "busy=0" in s and (row(s, "casual_joe") or ["", "x"])[1] in ("done", "error") else None)(e2e.agent(2, "shop", "status")), 300, 1) or ""
        rw = row(s, "casual_joe") or ("?",) * 6
        check("client of a host with addons_policy=none: Add kept for the next start", rw[1] == "done" and "allows no add-ons" in rw[5] and
              not c.grep(r"casual_joe\.pak -> mounted") and "casual_joe.pak=1" in open(os.path.join(c2, "addonlist.txt")).read(), rw[5][:140])
    finally:
        S.stop()


if __name__ == "__main__":
    main()
