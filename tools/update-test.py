#!/usr/bin/env python3
"""Live test of the in-game updater (#34, docs/investigations/updater.md "Tests") on test instance 1.

Builds a "newer release" dev build (B4B_BUILD_VERSION, default VERSION's next patch, e.g. 0.9.11-test), packs it like
launch/package.sh (plus an
ini the updater must never install), signs it with a throwaway ed25519 key (tools/sign-release.sh) and serves a fake
GitHub API on 127.0.0.1 (asset links redirect once, like github.com). The instance runs the dev build under test with
the dev ini keys update_pubkey= (the throwaway key) and update_api= (this server). Steps:
  1. real GitHub check over HTTPS (WinHTTP + netguard's updater scope) with the release key: the latest release's
     signed manifest verifies (or, before 0.6.2, "no in-game update files"), and api.github.com is blocked again
     afterwards; then the test key and the fake API are applied live
  2. refused downloads: bad manifest signature, bad zip hash, bad zip signature, truncated zip; nothing changes
  3. check -> download -> install: files swapped, backup kept, b4bcoop.ini (game folder + instance) and add-ons
     unchanged; restart: the new version runs and marks itself healthy
  4. go back: restart: the previous version runs again
  5. automatic revert: install again with update_never_healthy=1; two starts never marked healthy, the third start
     puts the previous version back and runs nothing; the next start is the previous version
Needs the build under test installed in the lane's game folder (launch/install.sh) and the lane's lock (taken here
unless --no-lock). Artifacts in /tmp/b4b-update-<time>/. Exit 1 on failure.
  B4B_LANE=2 B4B_STEAM=flatpak B4B_GPU=4090 tools/update-test.py [--no-lock] [--version 0.9.11-test]"""
import argparse, hashlib, http.server, json, os, re, shutil, socket, subprocess, sys, threading, time, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lane import GAME, ROOT, PORT_BASE, LANE

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(GAME, "Gobi", "Binaries", "Win64")
UPD = os.path.join(GAME, "b4bcoop-update")
OUT = f"/tmp/b4b-update-{time.strftime('%Y%m%d-%H%M%S')}"
T0 = time.time()
results = []


def log(msg):
    line = f"[{time.time() - T0:7.1f}s] {msg}"
    print(line, flush=True)
    with open(os.path.join(OUT, "update-test.log"), "a") as f: f.write(line + "\n")


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


def snapshot(prefix_ini):
    """sha256 of every file the updater may or must not touch"""
    files = {
        "X3DAudio1_7.dll": os.path.join(BIN, "X3DAudio1_7.dll"), "xinput1_3.dll": os.path.join(GAME, "xinput1_3.dll"),
        "game b4bcoop.ini": os.path.join(BIN, "b4bcoop.ini"), "instance b4bcoop.ini": prefix_ini,
        "b4bcoop-bans.txt": os.path.join(BIN, "b4bcoop-bans.txt"),
    }
    snap = {k: sha(p) for k, p in files.items()}
    addons = os.path.join(GAME, "b4bcoop-addons")
    for dp, _, fs in os.walk(addons):
        for f in fs: snap["addons/" + os.path.relpath(os.path.join(dp, f), addons)] = sha(os.path.join(dp, f))
    return snap


# ---------------------------------------------------------------- the fake release
class Release:
    def __init__(self, version, protocol):
        self.version, self.dir = version, os.path.join(OUT, "release")
        os.makedirs(self.dir)
        build = os.path.join(OUT, "build")
        log(f"building dev build {version} into {build}")
        env = dict(os.environ, B4B_BUILD_VERSION=version, B4B_BUILD_OUT=build)
        subprocess.run([os.path.join(REPO, "native", "build.sh")], env=env, check=True, stdout=subprocess.DEVNULL)
        self.dll, self.xinput = os.path.join(build, "X3DAudio1_7.dll"), os.path.join(build, "xinput1_3.dll")
        self.zip_name = f"b4bcoop-{version}.zip"
        zp = os.path.join(self.dir, self.zip_name)
        with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED) as z:
            for d in ("Gobi/", "Gobi/Binaries/", "Gobi/Binaries/Win64/"): z.writestr(d, b"")
            z.write(self.dll, "Gobi/Binaries/Win64/X3DAudio1_7.dll")
            z.writestr("Gobi/Binaries/Win64/b4bcoop.ini", b"; UPDATER TEST INI: must never be installed\r\nhost=0\r\n")
            z.writestr("b4bcoop-COMMANDS.txt", f"commands of {version}\r\n".encode())
            z.writestr("b4bcoop-LICENSE.txt", b"MIT\r\n")
            z.writestr("b4bcoop-README.txt", f"b4bcoop {version} test release\r\n".encode())
            z.write(self.xinput, "xinput1_3.dll")
        sr = os.path.join(REPO, "tools", "sign-release.sh")
        man = subprocess.run([sr, "manifest", zp, version, str(protocol)], check=True, capture_output=True, text=True).stdout
        open(os.path.join(self.dir, "b4bcoop-update.txt"), "w", newline="\n").write(man)
        key = os.path.join(OUT, "test-signing.key")
        subprocess.run(["openssl", "genpkey", "-algorithm", "ed25519", "-out", key], check=True, capture_output=True)
        self.pubhex = subprocess.run([sr, "pubhex", key], check=True, capture_output=True, text=True).stdout.strip()
        subprocess.run([sr, "sign", key, zp, os.path.join(self.dir, "b4bcoop-update.txt")], check=True, stdout=subprocess.DEVNULL)
        self.files = {n: open(os.path.join(self.dir, n), "rb").read() for n in os.listdir(self.dir)}
        log(f"release {version}: {len(self.files[self.zip_name])} bytes, test key {self.pubhex[:16]}...")


MODE = {"m": "ok"}


class Handler(http.server.BaseHTTPRequestHandler):
    rel: Release = None
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
        r, base = self.rel, f"http://127.0.0.1:{self.port}"
        if self.path in ("/repos/actuallydan/b4b-coop/releases/latest", f"/repos/actuallydan/b4b-coop/releases/tags/v{r.version}"):
            if not self.headers.get("User-Agent", "").startswith("b4bcoop-updater/"): return self.send(403, b"no UA")
            assets = [{"name": n, "size": len(b), "browser_download_url": f"{base}/dl/{n}", "uploader": {"login": "x"}}
                      for n, b in sorted(r.files.items())]
            body = {"tag_name": f"v{r.version}", "name": f"b4b-coop v{r.version}", "html_url": f"{base}/release",
                    "body": f"## New\n- updater test release {r.version}\n\n---\n\nprovenance text", "assets": assets}
            return self.send(200, json.dumps(body).encode(), "application/json")
        m = re.fullmatch(r"/dl/([\w.\-]+)", self.path)
        if m: return self.send(302, b"", headers=[("Location", f"{base}/assets/{m.group(1)}")])
        m = re.fullmatch(r"/assets/([\w.\-]+)", self.path)
        if not m or m.group(1) not in r.files: return self.send(404, b"not found")
        name, data, mode = m.group(1), r.files[m.group(1)], MODE["m"]
        if mode == "badmanifestsig" and name == "b4bcoop-update.txt.sig": data = bytes([data[0] ^ 1]) + data[1:]
        if mode == "badhash" and name == r.zip_name: data = data[:1000] + bytes([data[1000] ^ 0x20]) + data[1001:]
        if mode == "badzipsig" and name == r.zip_name + ".sig": data = data[:40] + bytes([data[40] ^ 4]) + data[41:]
        if mode == "truncated" and name == r.zip_name:
            self.send(200, data[: len(data) // 2], length=len(data))   # announces the full size, sends half, closes
            self.close_connection = True
            return
        self.send(200, data)


def serve(rel):
    Handler.rel = rel
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    Handler.port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    log(f"fake GitHub API on 127.0.0.1:{Handler.port}")
    return srv


# ---------------------------------------------------------------- instance control
def env_for(extra):
    return dict(os.environ, B4B_INI_EXTRA1=";".join(extra))


def start_instance(extra):
    log("multi.sh 1")
    r = subprocess.run([os.path.join(REPO, "launch", "multi.sh"), "1"], env=env_for(extra), capture_output=True, text=True, timeout=900)
    open(os.path.join(OUT, "multi.out"), "a").write(r.stdout + r.stderr)
    for _ in range(60):
        if "update:" in agent("update", "status"): return True
        time.sleep(2)
    return False


def stop_instances():
    subprocess.run([os.path.join(REPO, "launch", "multi-stop.sh")], capture_output=True, text=True)
    time.sleep(3)


def status():
    return agent("update", "status")


def wait_phase(want=("checked", "installed", "error"), t=120):
    end = time.time() + t
    while time.time() < end:
        s = status()
        m = re.search(r"phase=(\w+) busy=(\d)", s)
        if m and m.group(1) in want and m.group(2) == "0": return s
        time.sleep(1)
    return status()


def newest_log():
    import glob
    c = sorted(glob.glob(os.path.join(BIN, "b4bcoop-test1-*.log")), key=os.path.getmtime)
    return open(c[-1], errors="replace").read() if c else ""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-lock", action="store_true", help="the lane's gamelock is already held")
    ap.add_argument("--version", default="", help="the test release's version (default: VERSION's next patch -test)")
    ap.add_argument("--skip-github", action="store_true", help="skip the real api.github.com check")
    a = ap.parse_args()
    os.makedirs(OUT)
    log(f"lane {LANE}, game {GAME}, out {OUT}")
    lock = os.path.join(REPO, "launch", "gamelock.sh")
    if not a.no_lock: subprocess.run([lock, "acquire", "update-test"], check=True)
    try:
        run(a)
    finally:
        stop_instances()
        if not a.no_lock: subprocess.run([lock, "release", "update-test"])
    w = max(len(n) for n, _, _ in results) if results else 10
    print("\n" + "\n".join(f"{'PASS' if ok else 'FAIL'}  {n:<{w}}  {d}" for n, ok, d in results))
    bad = sum(not ok for _, ok, _ in results)
    print(f"\n{len(results) - bad}/{len(results)} passed; artifacts in {OUT}")
    sys.exit(1 if bad else 0)


def run(a):
    old = re.search(r'version=(\S+)', open(os.path.join(REPO, "VERSION")).read()).group(1)
    proto = int(re.search(r'protocol=(\d+)', open(os.path.join(REPO, "VERSION")).read()).group(1))
    if not a.version:   # must be newer than the build under test, or the tab offers "this older version"
        x, y, z = (int(n) for n in old.split("-")[0].split("."))
        a.version = f"{x}.{y}.{z + 1}-test"
    rel = Release(a.version, proto)
    srv = serve(rel)
    api = f"update_api=http://127.0.0.1:{Handler.port}"
    key = f"update_pubkey={rel.pubhex}"
    if os.path.isdir(UPD): shutil.rmtree(UPD)
    prefix_ini = os.path.join(ROOT, "test1", "b4bcoop.ini")
    old_dll = sha(os.path.join(BIN, "X3DAudio1_7.dll"))

    # 1. the real GitHub API over HTTPS with the release key (the real manifest verifies), then the test key + fake API
    if not start_instance([]): return check("instance 1 up", False)
    s = status()
    check("updater available", "unavailable=-" in s, s.strip().splitlines()[-1] if s else "no agent")
    if not a.skip_github:
        agent("update", "check")
        s = wait_phase()
        m = re.search(r"msg=(.*)", s)
        ok = "phase=checked" in s and ("has no in-game update files" in s or "manifest:" in s)
        check("real GitHub check over HTTPS", ok, m.group(1)[:140] if m else s[:140])
        lg = newest_log()
        check("netguard: api.github.com allowed only in the updater scope", "allow http api.github.com" in lg and "(updater)" in lg
              and "updater scope closed" in lg)
        out = agent("netguard")
        open(os.path.join(OUT, "netguard.txt"), "w").write(out)
        # a real release asset: github.com -> 302 -> release-assets.githubusercontent.com, inside and outside the scope
        import urllib.request
        url = "https://github.com/actuallydan/b4b-coop/releases/download/v0.6.1/SHA256SUMS"
        try: want = hashlib.sha256(urllib.request.urlopen(url, timeout=30).read()).hexdigest()
        except OSError as e: want = f"? ({e})"
        r = agent("update", "fetch", url, timeout=60)
        check("real asset download (github.com redirect) in the scope", f"sha256 {want}" in r, r.strip()[:120])
        r = agent("update", "fetch", url, "noscope", timeout=60)
        check("the same download outside the scope is blocked by netguard", "fetch failed" in r, r.strip()[:120])
    with open(prefix_ini, "a") as f: f.write(key + "\n" + api + "\n")
    time.sleep(4)
    s = status()
    check("dev update_api + update_pubkey applied live", f"api=http://127.0.0.1:{Handler.port}" in s and "key=test" in s)

    # 2. refused downloads; nothing may change
    before = snapshot(prefix_ini)
    for mode, want in (("badmanifestsig", "signature"), ("badhash", "SHA-256"), ("badzipsig", "signature"), ("truncated", "")):
        MODE["m"] = mode
        agent("update", "check")
        s = wait_phase()
        if mode != "badmanifestsig":
            if not check(f"{mode}: check ok", "phase=checked" in s and f"version={a.version}" in s, s[:160]): continue
            agent("update", "install")
            s = wait_phase(("installed", "error"), 180)
        msg = (re.search(r"msg=(.*)", s) or re.search("(.*)", s)).group(1)
        check(f"{mode}: refused", "phase=error" in s and want in msg and "installed=" in s, msg[:160])
        check(f"{mode}: nothing changed", snapshot(prefix_ini) == before and not os.path.isdir(os.path.join(UPD, "backup"))
              and not os.path.isdir(os.path.join(UPD, "staged")))
    MODE["m"] = "ok"

    # 3. the real thing
    agent("update", "check")
    s = wait_phase()
    check("check finds the new version", f"version={a.version}" in s and "available" in s, s[:160])
    agent("overlay", "open"); agent("overlay", "tab", "Updates")
    time.sleep(2)
    subprocess.run([os.path.join(REPO, "launch", "shot.sh"), "1", os.path.join(OUT, "updates-tab-checked.png")], capture_output=True)
    r = agent("overlay", "press", "Download and install on next start")
    s = wait_phase(("installed", "error"), 240)
    check("download + verify + install (the tab's button)", "phase=installed" in s, (re.search(r"msg=(.*)", s) or re.search("(.*)", s)).group(1)[:160])
    time.sleep(1)
    subprocess.run([os.path.join(REPO, "launch", "shot.sh"), "1", os.path.join(OUT, "updates-tab-installed.png")], capture_output=True)
    after = snapshot(prefix_ini)
    check("new X3DAudio1_7.dll in place", after["X3DAudio1_7.dll"] == sha(rel.dll))
    check("new xinput1_3.dll in place", after["xinput1_3.dll"] == sha(rel.xinput))
    check("previous DLL in the backup", sha(os.path.join(UPD, "backup", old, "Gobi", "Binaries", "Win64", "X3DAudio1_7.dll")) == old_dll)
    same = [k for k in before if k not in ("X3DAudio1_7.dll", "xinput1_3.dll") and before[k] != after.get(k)]
    check("b4bcoop.ini (game + instance), bans, add-ons unchanged", not same, ", ".join(same))
    gi = os.path.join(BIN, "b4bcoop.ini")
    check("the zip's ini was not installed", not os.path.exists(gi) or b"UPDATER TEST INI" not in open(gi, "rb").read())
    st = open(os.path.join(UPD, "state.txt")).read()
    check("state: pending install", f"installed={a.version}" in st and f"previous={old}" in st and "pending=1" in st)
    check("this game still runs the old version", "b4bcoop " + old in newest_log())
    stop_instances()
    if not start_instance([key, api]): return check("instance 1 up after the update", False)
    lg = newest_log()
    check(f"restart runs {a.version}", f"b4bcoop {a.version} (protocol" in lg, re.search(r"b4bcoop \S+ \(protocol \d+\)", lg).group(0) if re.search(r"b4bcoop \S+ \(protocol \d+\)", lg) else "")
    check("start counted", "start 1 of the freshly installed" in lg)
    time.sleep(25)
    lg = newest_log()
    check("marked healthy", f"{a.version} started fine" in lg and "pending=0" in open(os.path.join(UPD, "state.txt")).read())

    # 4. go back
    r = agent("update", "goback")
    check("go back", "phase=installed" in r and f"installed={old}" in r, r[:200])
    check("previous DLL back in place", sha(os.path.join(BIN, "X3DAudio1_7.dll")) == old_dll)
    stop_instances()
    if not start_instance([key, api]): return check("instance 1 up after going back", False)
    check(f"restart runs {old} again", f"b4bcoop {old} (protocol" in newest_log())

    # 5. a new version that never gets healthy is put back on its third start
    agent("update", "check")
    s = wait_phase()
    agent("update", "install")
    s = wait_phase(("installed", "error"), 240)
    if not check("install again", "phase=installed" in s, s[:120]): return
    stop_instances()
    sick = [key, api, "update_never_healthy=1"]
    for n in (1, 2):
        if not start_instance(sick): return check(f"sick start {n}", False)
        time.sleep(20)
        lg = newest_log()
        check(f"sick start {n}: runs {a.version}, not marked healthy", f"b4bcoop {a.version} (protocol" in lg and
              f"start {n} of the freshly installed" in lg and "not marking" in lg)
        stop_instances()
    up = start_instance(sick)
    lg = newest_log()
    check("third start: reverted, runs nothing", not up and "was put back" in lg and "running nothing this time" in lg,
          (re.search(r"update: .*put back.*", lg) or re.search("(.*)", "")).group(0)[:160])
    check("previous DLL back after the automatic revert", sha(os.path.join(BIN, "X3DAudio1_7.dll")) == old_dll)
    check("failed version kept aside", sha(os.path.join(UPD, "failed", a.version, "Gobi", "Binaries", "Win64", "X3DAudio1_7.dll")) == sha(rel.dll))
    stop_instances()
    if not start_instance([key, api]): return check("instance 1 up after the automatic revert", False)
    s = status()
    check(f"then {old} runs and remembers why", f"b4bcoop {old} (protocol" in newest_log() and "put back" in s, s[-200:])
    srv.shutdown()


if __name__ == "__main__":
    main()
