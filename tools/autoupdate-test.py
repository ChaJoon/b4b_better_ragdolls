#!/usr/bin/env python3
"""Live test of the automatic update before the game starts (the launcher, docs/investigations/updater.md §7) on test
instance 1, through the root stub (B4B_STUB=1) the way Steam's Play starts the game, never online.

Builds two "releases" of this tree (B4B_BUILD_VERSION: a newer 0.x.y+1-test and an older 0.x.y-1-test), packs and
signs them like update-test.py (throwaway ed25519 key, fake GitHub API on 127.0.0.1 with github.com-like redirects)
and runs the launcher with the dev ini keys auto_update=1, update_api=, update_pubkey= in the instance's own
b4bcoop.ini (never the game folder's). Cases (--only a,b,...):
  github     the real api.github.com releases/latest: this version is the latest, nothing shown (needs VERSION = the
             latest release)
  release    the same with the PLAYER launcher (installed from native/out/release): on by default, nothing shown
  blackhole  update_api=https://10.255.255.1 (no answer): the game starts after ~2 s, no window
  dns        update_api=https://b4bcoop-test.invalid: name not resolved, no window
  slowapi    the fake API answers after 6 s: given up after 2 s, no window
  newer      remembered launch=coop: the window, progress, installed, the stub's own mapped xinput1_3.dll moved to the
             backup, the game runs the new version (start 1, started fine); the Updates tab shows the result
  goback     (after newer) Go back in the game, restart: the version gone back from is not installed again
  slowdl     a ~40 KB/s download: the window for the whole download, gamescope frames every 0.5 s (B4B_GPU)
  prompt     the co-op prompt first (no remembered choice), then the window and the install
  badmanifestsig, badhash, badzipsig, truncated   refused, the reason shown, nothing changed, the old version starts
  cancel     a slow download, Cancel pressed (dev B4B_LAUNCHER_UPDATE=cancel): nothing changed
  joinnewer, joindown, joinold, joinequal   Steam join strings: the host's newer version installed / older version
             with another protocol installed / older with ours: no request / equal: no request
  pending, skip, failed   no request while a fresh update is pending; a skip= or failed\\ version not installed
  revert     an auto-installed version that never gets healthy (update_never_healthy=1): starts 1 and 2, the third
             puts the old one back, the fourth start doesn't install it again
  off        auto_update=0: no request; the Updates tab's checkbox writes and removes auto_update=1
Window screenshots (the launcher's dev B4B_LAUNCHER_SHOT) and launcher logs go to the output folder.
Needs the build under test installed (launch/install.sh) and the lane's lock (taken here unless --no-lock).
  B4B_LANE=2 B4B_STEAM=flatpak B4B_GPU=4090 tools/autoupdate-test.py [--no-lock] [--only newer,cancel] [--out DIR]"""
import argparse, glob, hashlib, http.server, json, os, re, shutil, socket, subprocess, sys, threading, time, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lane import GAME, ROOT, PORT_BASE, LANE

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(GAME, "Gobi", "Binaries", "Win64")
UPD = os.path.join(GAME, "b4bcoop-update")
PREFIX = os.path.join(ROOT, "test1")
INI = os.path.join(PREFIX, "b4bcoop.ini")
LLOG = os.path.join(BIN, "b4bcoop-launcher.log")
SHOTS_IN = os.path.join(PREFIX, "autoupd-shots")
SELF_ID = "76561198994546085"   # lane 2's account: a join to ourselves goes nowhere
ALL = ("github release blackhole dns slowapi newer goback slowdl prompt badmanifestsig badhash badzipsig truncated cancel "
       "joinnewer joindown joinold joinequal pending skip failed revert off").split()
T0 = time.time()
OUT = None
results = []


def log(msg):
    line = f"[{time.time() - T0:7.1f}s] {msg}"
    print(line, flush=True)
    with open(os.path.join(OUT, "autoupdate-test.log"), "a") as f: f.write(line + "\n")


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    log(f"{'PASS' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}")
    return ok


def agent(*args, timeout=25):
    try:
        s = socket.create_connection(("127.0.0.1", PORT_BASE), timeout=timeout)
        s.settimeout(timeout)
        s.sendall((" ".join(args) + "\n").encode())
        chunks = []
        while d := s.recv(65536): chunks.append(d)
        return b"".join(chunks).decode(errors="replace")
    except OSError:
        return ""


def sha(p):
    try: return hashlib.sha256(open(p, "rb").read()).hexdigest()
    except OSError: return None


def read(p):
    try: return open(p, errors="replace").read()
    except OSError: return ""


def wait(fn, t, step=1.0):
    end = time.time() + t
    while time.time() < end:
        r = fn()
        if r: return r
        time.sleep(step)
    return fn()


def snapshot():
    files = {"X3DAudio1_7.dll": os.path.join(BIN, "X3DAudio1_7.dll"), "xinput1_3.dll": os.path.join(GAME, "xinput1_3.dll"),
             "game b4bcoop.ini": os.path.join(BIN, "b4bcoop.ini"), "b4bcoop-bans.txt": os.path.join(BIN, "b4bcoop-bans.txt"),
             "README": os.path.join(GAME, "b4bcoop-README.txt")}
    snap = {k: sha(p) for k, p in files.items()}
    for dp, _, fs in os.walk(os.path.join(GAME, "b4bcoop-addons")):
        for f in fs: snap["addons/" + f] = sha(os.path.join(dp, f))
    return snap


# ---------------------------------------------------------------- fake releases + API
class Release:
    def __init__(self, version, protocol, key):
        self.version = version
        self.dir = os.path.join(OUT, "release-" + version)
        os.makedirs(self.dir)
        build = os.path.join(OUT, "build-" + version)
        log(f"building {version} into {build}")
        env = dict(os.environ, B4B_BUILD_VERSION=version, B4B_BUILD_OUT=build)
        subprocess.run([os.path.join(REPO, "native", "build.sh")], env=env, check=True, stdout=subprocess.DEVNULL)
        self.dll, self.xinput = os.path.join(build, "X3DAudio1_7.dll"), os.path.join(build, "xinput1_3.dll")
        self.zip_name = f"b4bcoop-{version}.zip"
        zp = os.path.join(self.dir, self.zip_name)
        with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED) as z:
            for d in ("Gobi/", "Gobi/Binaries/", "Gobi/Binaries/Win64/"): z.writestr(d, b"")
            z.write(self.dll, "Gobi/Binaries/Win64/X3DAudio1_7.dll")
            z.writestr("Gobi/Binaries/Win64/b4bcoop.ini", b"; AUTO-UPDATE TEST INI: must never be installed\r\nhost=0\r\n")
            z.writestr("b4bcoop-README.txt", f"b4bcoop {version} test release\r\n".encode())
            z.write(self.xinput, "xinput1_3.dll")
        sr = os.path.join(REPO, "tools", "sign-release.sh")
        man = subprocess.run([sr, "manifest", zp, version, str(protocol)], check=True, capture_output=True, text=True).stdout
        open(os.path.join(self.dir, "b4bcoop-update.txt"), "w", newline="\n").write(man)
        subprocess.run([sr, "sign", key, zp, os.path.join(self.dir, "b4bcoop-update.txt")], check=True, stdout=subprocess.DEVNULL)
        self.files = {n: open(os.path.join(self.dir, n), "rb").read() for n in os.listdir(self.dir)}
        log(f"release {version}: zip {len(self.files[self.zip_name])} bytes")


MODE = {"m": "ok"}
REQS = []   # (time, path, user agent)


class Handler(http.server.BaseHTTPRequestHandler):
    rels = {}
    latest = None
    port = 0

    def log_message(self, fmt, *a):
        with open(os.path.join(OUT, "server.log"), "a") as f: f.write(f"{time.strftime('%H:%M:%S')} {MODE['m']} {fmt % a}\n")

    def send(self, code, body=b"", ctype="application/octet-stream", headers=(), length=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body) if length is None else length))
        for k, v in headers: self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        REQS.append((time.time(), self.path, self.headers.get("User-Agent", "")))
        base, mode = f"http://127.0.0.1:{self.port}", MODE["m"]
        m = re.fullmatch(r"/repos/actuallydan/b4b-coop/releases/(latest|tags/v([\w.\-]+))", self.path)
        if m:
            if mode == "slowapi": time.sleep(6)
            r = self.latest if m.group(1) == "latest" else self.rels.get(m.group(2))
            if not r: return self.send(404, b'{"message":"Not Found"}', "application/json")
            assets = [{"name": n, "size": len(b), "browser_download_url": f"{base}/dl/{r.version}/{n}"} for n, b in sorted(r.files.items())]
            body = {"tag_name": f"v{r.version}", "name": f"b4b-coop v{r.version}", "html_url": f"{base}/release",
                    "body": "## New\n- test release", "assets": assets}
            try: return self.send(200, json.dumps(body).encode(), "application/json")
            except OSError: return
        m = re.fullmatch(r"/dl/([\w.\-]+)/([\w.\-]+)", self.path)
        if m: return self.send(302, b"", headers=[("Location", f"{base}/assets/{m.group(1)}/{m.group(2)}")])
        m = re.fullmatch(r"/assets/([\w.\-]+)/([\w.\-]+)", self.path)
        r = self.rels.get(m.group(1)) if m else None
        if not r or m.group(2) not in r.files: return self.send(404, b"not found")
        name, data = m.group(2), r.files[m.group(2)]
        if mode == "badmanifestsig" and name == "b4bcoop-update.txt.sig": data = bytes([data[0] ^ 1]) + data[1:]
        if mode == "badhash" and name == r.zip_name: data = data[:1000] + bytes([data[1000] ^ 0x20]) + data[1001:]
        if mode == "badzipsig" and name == r.zip_name + ".sig": data = data[:40] + bytes([data[40] ^ 4]) + data[41:]
        try:
            if mode == "truncated" and name == r.zip_name:
                self.send(200, data[: len(data) // 2], length=len(data))
                self.close_connection = True
                return
            if mode in ("throttle", "slowdl") and name == r.zip_name:   # ~100 KB/s (slowdl: 40 KB/s), visible progress
                self.send_response(200)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                step = 16384 if mode == "throttle" else 8192
                for i in range(0, len(data), step):
                    self.wfile.write(data[i:i + step]); self.wfile.flush(); time.sleep(0.16 if mode == "throttle" else 0.2)
                return
            self.send(200, data)
        except OSError:
            pass   # the launcher cancelled / gave up


def serve(rels, latest):
    Handler.rels, Handler.latest = {r.version: r for r in rels}, latest
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    Handler.port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    log(f"fake GitHub API on 127.0.0.1:{Handler.port}")
    return srv


# ---------------------------------------------------------------- instance control
def procs():
    out = []
    for d in glob.glob("/proc/[0-9]*"):
        try:
            if f"B4B_PREFIX={PREFIX}".encode() not in open(d + "/environ", "rb").read().split(b"\0"): continue
            out.append((int(d[6:]), open(d + "/comm").read().strip(),
                        open(d + "/cmdline", "rb").read().replace(b"\0", b" ").decode(errors="replace")))
        except OSError:
            pass
    return out


def stop():
    subprocess.run([os.path.join(REPO, "launch", "multi-stop.sh"), "1"], capture_output=True, text=True)
    time.sleep(2)
    for p in procs():
        try: os.kill(p[0], 9)
        except OSError: pass
    time.sleep(1)


INI0 = []


def ini_set(lines):
    base = [l for l in INI0 if not l.startswith(("launch=", "auto_update=", "update_api=", "update_pubkey=", "update_repo=",
                                                  "update_never_healthy="))]
    open(INI, "w", newline="\r\n").write("\n".join(base + lines) + "\n")


def gamescope_shot(path):
    """B4B_GPU: the headless gamescope's whole frame, as a player would see it (the launcher's windows included)"""
    for pid, _, _ in procs():
        try: env = dict(kv.split("=", 1) for kv in open(f"/proc/{pid}/environ", "rb").read().decode(errors="replace").split("\0") if "=" in kv)
        except OSError: continue
        gd = env.get("GAMESCOPE_WAYLAND_DISPLAY")
        if not gd: continue
        rt = env.get("XDG_RUNTIME_DIR", "")
        e = dict(os.environ, XDG_RUNTIME_DIR=rt, GAMESCOPE_WAYLAND_DISPLAY=os.path.basename(gd))
        r = subprocess.run(["gamescopectl", "screenshot", path], env=e, capture_output=True, text=True, timeout=10)
        return r.returncode == 0
    return False


def launch(tag, lines, args=(), env_extra=None, frames=0):
    """the root stub as Steam starts it; returns when the launcher has started the game (or nothing).
    frames: also take gamescope screenshots every 0.5 s (up to that many) while the launcher runs"""
    stop()
    ini_set(lines)
    if os.path.exists(LLOG): os.remove(LLOG)
    os.makedirs(SHOTS_IN, exist_ok=True)
    for f in glob.glob(os.path.join(SHOTS_IN, "*")): os.remove(f)
    env = dict(os.environ, B4B_STUB="1", B4B_LAUNCHER_SHOT="Z:" + SHOTS_IN.replace("/", "\\") + "\\" + tag + "-")
    for k in ("B4B_LAUNCHER_ANSWER", "B4B_LAUNCHER_UPDATE"): env.pop(k, None)
    env.update(env_extra or {})
    log(f"start {tag}: stub {' '.join(args)} ini {lines} env {env_extra or {}}")
    REQS.clear()
    t = time.time()
    out = open(os.path.join(OUT, f"{tag}.out"), "w")
    subprocess.Popen([os.path.join(REPO, "launch", "instance.sh"), "1", *args], env=env, stdout=out, stderr=subprocess.STDOUT,
                     stdin=subprocess.DEVNULL, start_new_session=True)
    fin = lambda: re.search(r"redirect \(no EAC\)|nothing started|CreateProcessW failed", read(LLOG))
    if frames:
        os.makedirs(os.path.join(OUT, "frames"), exist_ok=True)
        wait(lambda: "choice:" in read(LLOG), 240, 0.2)
        for i in range(frames):
            if fin(): break
            gamescope_shot(os.path.join(OUT, "frames", f"{tag}-{i:03d}-{time.time() - t:05.1f}s.png"))
            time.sleep(0.5)
    done = wait(fin, 240, 0.5)
    ll = read(LLOG)
    open(os.path.join(OUT, f"{tag}-launcher.log"), "w").write(ll)
    for b in sorted(glob.glob(os.path.join(SHOTS_IN, "*.bmp"))):
        os.makedirs(os.path.join(OUT, "shots"), exist_ok=True)
        png = os.path.join(OUT, "shots", os.path.basename(b)[:-4] + ".png")
        subprocess.run(["magick", b, "-alpha", "off", png], capture_output=True)
    log(f"{tag}: launcher {'finished' if done else 'DID NOT FINISH'} in {time.time() - t:.0f} s; requests: "
        f"{[p for _, p, _ in REQS]}")
    return ll


def ms(ll, pat):
    m = re.search(r"\[\s*(\d+) ms\] [^\n]*" + pat, ll)
    return int(m.group(1)) if m else None


def gap(ll):   # ms between the choice and the game's CreateProcessW: what the update costs at the start
    a, b = ms(ll, r"choice: "), ms(ll, r"redirect \(no EAC\)")
    return (b - a) if a is not None and b is not None else None


def shots(tag):
    return sorted(os.path.basename(p) for p in glob.glob(os.path.join(OUT, "shots", tag + "-*.png")))


def newest_agent_log(since):
    c = [p for p in glob.glob(os.path.join(BIN, "b4bcoop-test1-*.log")) if os.path.getmtime(p) >= since]
    return read(max(c, key=os.path.getmtime)) if c else ""


DEV = {}


def reset():
    """the build under test back in place, no update state"""
    stop()
    if os.path.isdir(UPD): shutil.rmtree(UPD)
    for name, dst in (("X3DAudio1_7.dll", os.path.join(BIN, "X3DAudio1_7.dll")), ("xinput1_3.dll", os.path.join(GAME, "xinput1_3.dll"))):
        if os.path.exists(dst): os.remove(dst)
        shutil.copy(DEV[name], dst)


def no_window(tag, ll):
    return "progress window" not in ll and not shots(tag)


# ---------------------------------------------------------------- the cases
def run(a):
    global INI0
    vf = open(os.path.join(REPO, "VERSION")).read()
    ver = re.search(r"version=(\S+)", vf).group(1)
    proto = int(re.search(r"protocol=(\d+)", vf).group(1))
    x, y, z = (int(n) for n in ver.split("-")[0].split("."))
    newer, older = f"{x}.{y}.{z + 1}-test", f"{x}.{y}.{max(z - 1, 0)}-test"
    INI0 = read(INI).splitlines()
    for n in ("X3DAudio1_7.dll", "xinput1_3.dll"):
        DEV[n] = os.path.join(OUT, "under-test-" + n)
        shutil.copy(os.path.join(BIN, n) if n.startswith("X3D") else os.path.join(GAME, n), DEV[n])
    check("build under test installed (dev launcher with the update)", b"auto-update-1" in open(DEV["xinput1_3.dll"], "rb").read()
          and b"B4B_LAUNCHER_UPDATE" in open(DEV["xinput1_3.dll"], "rb").read())
    key = os.path.join(OUT, "test-signing.key")
    subprocess.run(["openssl", "genpkey", "-algorithm", "ed25519", "-out", key], check=True, capture_output=True)
    pub = subprocess.run([os.path.join(REPO, "tools", "sign-release.sh"), "pubhex", key], check=True, capture_output=True, text=True).stdout.strip()
    want = set(a.only.split(",")) if a.only else set(ALL)
    need_rel = want - {"github", "release", "blackhole", "dns", "off", "joinold", "joinequal", "pending"}
    rels = [Release(newer, proto, key), Release(older, proto - 1 if proto > 1 else 1, key)] if need_rel else []
    srv = serve(rels, rels[0] if rels else None)
    api = f"update_api=http://127.0.0.1:{Handler.port}"
    fake = ["auto_update=1", api, f"update_pubkey={pub}"]
    coop = ["launch=coop"]
    old_x, old_a = sha(DEV["xinput1_3.dll"]), sha(DEV["X3DAudio1_7.dll"])
    reset()

    if "github" in want:
        ll = launch("github", coop + ["auto_update=1"])
        check("github: real releases/latest answered, same version: nothing to install",
              f"GitHub's release is v{ver}, this is {ver}: nothing to install" in ll,
              (re.search(r"update: (GitHub|check|no answer)[^\r\n]*", ll) or re.search("(.*)", "")).group(0))
        check("github: no window", no_window("github", ll))
        g = gap(ll)
        m = re.search(r"answered in (\d+) ms", ll)
        check("github: start delayed by the check only", g is not None and g < 2500,
              f"{g} ms from the choice to the game's start; GitHub answered in {m.group(1) if m else '?'} ms")
    if "release" in want:
        rel_dir = os.path.join(REPO, "native", "out", "release")
        for n, dst in (("X3DAudio1_7.dll", os.path.join(BIN, "X3DAudio1_7.dll")), ("xinput1_3.dll", os.path.join(GAME, "xinput1_3.dll"))):
            os.remove(dst); shutil.copy(os.path.join(rel_dir, n), dst)
        ll = launch("release", coop)   # no auto_update line: the player default
        check("release: player launcher on by default", "(on by default): checking https://api.github.com/repos/actuallydan/b4b-coop/releases/latest" in ll,
              (re.search(r"update: [^\r\n]*checking[^\r\n]*", ll) or re.search("(.*)", "")).group(0))
        check("release: same version, nothing shown", f"this is {ver}: nothing to install" in ll and no_window("release", ll), f"gap {gap(ll)} ms")
        reset()
    for tag, url, want_txt in (("blackhole", "https://10.255.255.1", "no answer within 2000 ms"),
                               ("dns", "https://b4bcoop-test.invalid", "check failed")):
        if tag not in want: continue
        ll = launch(tag, coop + ["auto_update=1", f"update_api={url}"])
        g = gap(ll)
        check(f"{tag}: gave up, game started", want_txt in ll and "redirect (no EAC)" in ll,
              (re.search(r"update: (no answer|check failed)[^\r\n]*", ll) or re.search("(.*)", "")).group(0))
        check(f"{tag}: no window, at most ~2 s", no_window(tag, ll) and g is not None and g < 2600, f"{g} ms")
    if "slowapi" in want:
        MODE["m"] = "slowapi"
        ll = launch("slowapi", coop + fake)
        MODE["m"] = "ok"
        g = gap(ll)
        check("slowapi: given up after 2 s, no window", "no answer within 2000 ms" in ll and no_window("slowapi", ll) and g is not None and g < 2600, f"{g} ms")

    if "newer" in want or "goback" in want:
        reset()
        t = time.time()
        ll = launch("newer", coop + fake)
        check("newer: window shown, installed", f"b4bcoop {newer} is available: downloading, with a progress window" in ll
              and f"installed {newer} over {ver}" in ll, f"gap {gap(ll)} ms; shots {shots('newer')}")
        check("newer: the launcher's own requests (User-Agent b4bcoop-launcher/)", REQS and all(u.startswith("b4bcoop-launcher/") for _, _, u in REQS),
              str({u for _, _, u in REQS}))
        check("newer: new files in place", sha(os.path.join(GAME, "xinput1_3.dll")) == sha(rels[0].xinput) and sha(os.path.join(BIN, "X3DAudio1_7.dll")) == sha(rels[0].dll))
        check("newer: the stub's mapped xinput1_3.dll moved into the backup", sha(os.path.join(UPD, "backup", ver, "xinput1_3.dll")) == old_x
              and sha(os.path.join(UPD, "backup", ver, "Gobi", "Binaries", "Win64", "X3DAudio1_7.dll")) == old_a)
        check("newer: zip's ini not installed, game ini untouched", b"AUTO-UPDATE TEST" not in open(os.path.join(BIN, "b4bcoop.ini"), "rb").read()
              if os.path.exists(os.path.join(BIN, "b4bcoop.ini")) else True)
        check("newer: auto.txt", f"installed {newer} over {ver} at the game start" in read(os.path.join(UPD, "auto.txt")))
        up = wait(lambda: "pong" in agent("ping"), 240, 2)
        lg = newest_agent_log(t)
        check("newer: the game runs the new version, start 1 counted", up and f"b4bcoop {newer} (protocol" in lg and "start 1 of the freshly installed" in lg)
        ok = wait(lambda: f"{newer} started fine" in newest_agent_log(t), 120, 3)
        check("newer: marked healthy after 15 s", ok)
        agent("overlay", "open"); agent("overlay", "tab", "Updates"); time.sleep(2)
        subprocess.run([os.path.join(REPO, "launch", "shot.sh"), "1", os.path.join(OUT, "shots", "newer-updates-tab.png")], capture_output=True)
        check("newer: Updates tab shows the last automatic update", "last: " in newest_agent_log(t) and "automatic update on" in newest_agent_log(t),
              (re.search(r"update: Updates tab on[^\r\n]*", newest_agent_log(t)) or re.search("(.*)", "")).group(0)[-160:])
        if "goback" in want:
            r = agent("update", "goback")
            check("goback: went back in the game", "phase=installed" in r and f"skip={newer}" in read(os.path.join(UPD, "state.txt")), r.strip()[:120])
            ll = launch("goback", coop + fake)
            check("goback: not installed again by itself", "was put back before" in ll and f"installed {newer}" not in ll and no_window("goback", ll))
            check("goback: old files stay", sha(os.path.join(GAME, "xinput1_3.dll")) == old_x and sha(os.path.join(BIN, "X3DAudio1_7.dll")) == old_a)
    if "slowdl" in want:   # a slow connection (~40 KB/s): what the player sees for the whole download
        reset()
        MODE["m"] = "slowdl"
        ll = launch("slowdl", coop + fake, frames=80)
        MODE["m"] = "ok"
        check("slowdl: installed after a visible download", f"installed {newer} over {ver}" in ll and any("downloading" in s for s in shots("slowdl")),
              f"gap {gap(ll)} ms; shots {shots('slowdl')}")
    if "prompt" in want:
        reset()
        ll = launch("prompt", fake, env_extra={"B4B_LAUNCHER_ANSWER": "coop"})
        check("prompt: the prompt, then the window, installed", "choice: b4bcoop co-op (the prompt)" in ll and f"installed {newer} over {ver}" in ll,
              f"shots {shots('prompt')}")
    for mode, why in (("badmanifestsig", "not safe to install: the update manifest's signature"), ("badhash", "not safe to install: download damaged"),
                      ("badzipsig", "not safe to install: the download's signature"), ("truncated", "download incomplete|download failed")):
        if mode not in want: continue
        reset()
        before = snapshot()
        MODE["m"] = mode
        ll = launch(mode, coop + fake)
        MODE["m"] = "ok"
        m = re.search(r"update: failed: ([^\r\n]*)|update: (not installed[^\r\n]*)", ll)
        check(f"{mode}: refused with the reason", m and re.search(why, m.group(0)), m.group(0) if m else "")
        check(f"{mode}: nothing changed, no backup/staged", snapshot() == before and not os.path.isdir(os.path.join(UPD, "backup"))
              and not os.path.isdir(os.path.join(UPD, "staged")))
        check(f"{mode}: the old version starts, the window said so", "redirect (no EAC)" in ll and any("failed" in s for s in shots(mode)), str(shots(mode)))
    if "cancel" in want:
        reset()
        before = snapshot()
        MODE["m"] = "throttle"
        ll = launch("cancel", coop + fake, env_extra={"B4B_LAUNCHER_UPDATE": "cancel"})
        MODE["m"] = "ok"
        check("cancel: skipped during the download", "skipped by the player" in ll and "downloading" in ll)
        check("cancel: nothing changed", snapshot() == before and not os.path.isdir(os.path.join(UPD, "backup")))
        check("cancel: auto.txt says so", "you skipped it" in read(os.path.join(UPD, "auto.txt")))

    def join(tag, p, v):
        return launch(tag, coop + fake, ["+b4bcoop_join", f"steam:{SELF_ID}", f"proto:{p}", f"ver:{v}"])
    if "joinnewer" in want:
        reset()
        ll = join("joinnewer", proto, newer)
        check("joinnewer: asked for the host's version, installed", f"releases/tags/v{newer}" in ll and f"installed {newer} over {ver}" in ll,
              str([p for _, p, _ in REQS][:1]))
    if "joindown" in want:
        reset()
        ll = join("joindown", proto - 1, older)
        check("joindown: older host, other protocol: its version installed", f"releases/tags/v{older}" in ll and f"installed {older} over {ver}" in ll)
        check("joindown: older files in place", sha(os.path.join(GAME, "xinput1_3.dll")) == sha(rels[1].xinput))
    for tag, v in (("joinold", f"{x}.{y}.{max(z - 1, 0)}"), ("joinequal", ver)):
        if tag not in want: continue
        reset()
        ll = join(tag, proto, v)
        check(f"{tag}: no request at all", "that works with" in ll and "checking" not in ll and not REQS and no_window(tag, ll), f"gap {gap(ll)} ms")
    if "pending" in want:
        reset()
        os.makedirs(UPD, exist_ok=True)
        open(os.path.join(UPD, "state.txt"), "w").write(f"installed={ver}\r\nprevious=0.0.1\r\npending=1\r\nstarts=1\r\nfiles=\r\nresult=\r\n")
        ll = launch("pending", coop + fake)
        check("pending: no request while a fresh update waits for its 15 s", "hasn't had its first 15 s" in ll and not REQS)
    for tag in ("skip", "failed"):
        if tag not in want: continue
        reset()
        os.makedirs(UPD, exist_ok=True)
        if tag == "skip": open(os.path.join(UPD, "state.txt"), "w").write(f"installed={ver}\r\npending=0\r\nskip={newer}\r\n")
        else: os.makedirs(os.path.join(UPD, "failed", newer))
        ll = launch(tag, coop + fake)
        check(f"{tag}: the put-back version is not installed", "was put back before" in ll and no_window(tag, ll)
              and sha(os.path.join(GAME, "xinput1_3.dll")) == old_x)
    if "revert" in want:
        reset()
        sick = coop + fake + ["update_never_healthy=1"]
        for n in (1, 2, 3):
            t = time.time()
            ll = launch(f"revert{n}", sick)
            if n == 1: check("revert 1: installed", f"installed {newer} over {ver}" in ll)
            if n == 2: check("revert 2: no check while pending", "hasn't had its first 15 s" in ll and not REQS)
            if n < 3:
                wait(lambda: "not marking" in newest_agent_log(t), 240, 2)
                check(f"revert {n}: runs {newer}, start {n}, not healthy", f"start {n} of the freshly installed" in newest_agent_log(t))
            else:
                wait(lambda: "running nothing this time" in newest_agent_log(t), 240, 2)
                check("revert 3: put back, ran nothing", "was put back" in newest_agent_log(t) and sha(os.path.join(GAME, "xinput1_3.dll")) == old_x)
        ll = launch("revert4", sick)
        check("revert 4: not installed again", "was put back before" in ll and sha(os.path.join(BIN, "X3DAudio1_7.dll")) == old_a and no_window("revert4", ll))
    if "off" in want:
        reset()
        ll = launch("off", coop + fake[1:] + ["auto_update=0"])
        check("off: auto_update=0, no request", "automatic update off (auto_update=0)" in ll and not REQS and no_window("off", ll), f"gap {gap(ll)} ms")
        up = wait(lambda: "pong" in agent("ping"), 240, 2)
        agent("overlay", "open"); agent("overlay", "tab", "Updates"); time.sleep(2)
        subprocess.run([os.path.join(REPO, "launch", "shot.sh"), "1", os.path.join(OUT, "shots", "off-updates-tab.png")], capture_output=True)
        agent("overlay", "press", "##autoupdate"); time.sleep(3)
        on = re.search(r"(?m)^auto_update=1\r?$", read(INI)) is not None
        subprocess.run([os.path.join(REPO, "launch", "shot.sh"), "1", os.path.join(OUT, "shots", "off-updates-tab-checked.png")], capture_output=True)
        agent("overlay", "press", "##autoupdate"); time.sleep(3)
        gone = not re.search(r"(?m)^auto_update=", read(INI))
        check("off: the tab's checkbox writes auto_update=1, then removes it (dev default off)", up and on and gone)
    srv.shutdown()


def main():
    global OUT
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-lock", action="store_true")
    ap.add_argument("--only", default="")
    ap.add_argument("--out", default=f"/tmp/b4b-autoupdate-{time.strftime('%Y%m%d-%H%M%S')}")
    a = ap.parse_args()
    OUT = a.out
    os.makedirs(OUT, exist_ok=True)
    log(f"lane {LANE}, game {GAME}, prefix {PREFIX}, out {OUT}")
    lock = os.path.join(REPO, "launch", "gamelock.sh")
    if not a.no_lock: subprocess.run([lock, "acquire", "autoupdate-test"], check=True)
    ini_orig = read(INI)
    try:
        run(a)
    finally:
        stop()
        open(INI, "w").write(ini_orig)
        if DEV: reset()
        shutil.rmtree(SHOTS_IN, ignore_errors=True)
        if not a.no_lock: subprocess.run([lock, "release", "autoupdate-test"])
    w = max(len(n) for n, _, _ in results) if results else 10
    print("\n" + "\n".join(f"{'PASS' if ok else 'FAIL'}  {n:<{w}}  {d}" for n, ok, d in results))
    bad = sum(not ok for _, ok, _ in results)
    print(f"\n{len(results) - bad}/{len(results)} passed; artifacts in {OUT}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
