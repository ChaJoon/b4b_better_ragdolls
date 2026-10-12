#!/usr/bin/env python3
"""Character regression suite: build every test character with the modkit, then wear each one in real game instances.
The model-mods counterpart of tools/e2e.py (dev tool; the models themselves are never committed).

    tools/charsuite.py [--manifest FILE] [--only a,b] [--twice] [--no-preview] [--no-game] [--no-build]
                       [--vanilla] [--mission N] [--motion] [--poses] [--install] [--no-lock] [--out DIR] [--build-dir DIR]

Manifest (JSON, outside the repo; default ~/.local/share/b4b-coop/characters/suite.json or $B4B_CHARSUITE; format and
a CC0 example: tools/charsuite-example.json):
    {"root": "<dir the model paths are relative to; default the manifest's folder>",
     "characters": [{"name": "shino", "model": "src/sendagaya_shino/Sendagaya Shino.vrm", "outfit": "Holly/Elite_00",
                     "title": "Sendagaya Shino", "flags": [], "expect": "shino", "mission": true, "license": "CC0"}]}
    name     the outfit name (b4bmod survivor --as; in game /model <name>)
    outfit   base outfit: "Hero/Elite_NN", "TU11/Hero/Elite_NN" or a full /Game/... 3P_*_SKM path
    fp       first-person arms (default: the outfit's folder, 3P_ -> FP_; false = none)
    flags    extra b4bmod arguments this model needs (ideally none: the column shows how many)
    expect   outfit name the add-on must declare (default: name); mission: also worn in Evansburgh (see --mission)

Steps:
 1. build (sequential: one extract folder can't take parallel builds; its own folder by default, B4B_EXTRACT or
    ~/.local/share/b4b-coop/charsuite/extract, so builds of other agents/modders don't interfere):
    `modkit/b4bmod.sh survivor <model> --outfit .. --fp .. --as <name>` into --build-dir (default
    ~/.local/share/b4b-coop/charsuite/build, replaced per run); checks exit status, the add-on's outfit= name, size.
    --twice builds again into <build-dir>2 and compares the paks byte for byte (determinism).
 2. previews (blender/preview.py: 3P front/side/back with the game textures, the face, the FP arms in a gun hold
    seen from the FP camera), 4 at a time.
 3. game (lane from B4B_LANE; takes launch/gamelock.sh as "charsuite" unless --no-lock; --install runs
    launch/install.sh first): the add-ons go into <game>/b4bcoop-addons (the lock's backup restores the player's own on
    release), multi.sh 2 (host + client with the add-ons; --vanilla adds a 3rd instance with addons=0), Fort Hope.
    Per outfit: host `model <name>`, both logs must say `models: hero slot N wears outfit <name>` and both hero lists
    (dev `face`) must show the outfit's mesh on the host's hero; screenshots (presented frames, launch/shot.sh): the
    host stands in front of each client's hero (`face look` on the host; a client can't move itself), so the client's
    camera sees the host full body and face; host 3P (thirdperson; FP arms only show with a weapon, in the mission); the vanilla client's view (must show the
    survivor, no add-on mesh, no "wears" line); new mesh/material/cloth/add-on errors in every log since the outfit
    went on (lines not seen before the first swap), every instance still answering.
    --mission N (default 2, 0 = off): then Evansburgh (mission Easy, `ready` ends the character select) with the first
    N entries marked "mission": true (else the first N) worn again: host FP holding a weapon, host 3P, client view;
    --motion adds the face while talking (host's hero says a line, client view) and the outfit running (on a bot).
    --poses adds the host crouching, jogging, turning in place and aiming (client view + host 3P), where bad weights
    or stray cloth show.
Output: summary table + contact sheet (contact.png, one row per character) + logs, previews, screenshots, the game logs
and agent transcript in /tmp/b4b-charsuite-<time>/ (lane 2: /tmp/b4b-charsuite-l2-<time>/). Exit 1 on any failure.
Set B4B_GPU (e.g. 4090) as for multi.sh to keep the instances off the display GPU.
"""
import argparse, concurrent.futures, datetime, glob, hashlib, json, os, re, shutil, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
from lane import LANE, GAME   # B4B_LANE (tools/lane.py)
import e2e                    # agent, wait_for, Session, GameLog, screenshot, has_hero ... (they log into e2e.OUT)

DATA = os.path.expanduser("~/.local/share/b4b-coop")
KIT = os.path.join(REPO, "modkit")
ADDONS = os.path.join(GAME, "b4bcoop-addons")
T0 = time.time()
OUT = None
H, C, V = 1, 2, 3    # host, client with add-ons, client without (--vanilla)


def log(msg):
    line = f"[{time.time() - T0:7.1f}s] {msg}"
    print(line, flush=True)
    with open(os.path.join(OUT, "charsuite.log"), "a") as f: f.write(line + "\n")


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()


# ---------------------------------------------------------------- manifest
def template(ent):
    o = ent["outfit"]
    if o.startswith("/Game/"):
        p3 = o
    else:
        parts = o.strip("/").split("/")
        tu = parts[0] + "/" if len(parts) == 3 else ""
        hero, el = parts[-2], parts[-1]
        p3 = f"/Game/{tu}Characters/Heroes/{hero}/Meshes/Elite/{el}/3P_{hero}_{el}_SKM"
    fp = ent.get("fp", True)
    if fp is True:
        folder, n = p3.rsplit("/", 1)
        fp = f"{folder}/{re.sub(r'^3P_', 'FP_', n)}"
    return p3, fp or None


def load_manifest(path, only):
    m = json.load(open(path))
    root = os.path.expanduser(m.get("root") or os.path.dirname(os.path.abspath(path)))
    chars = []
    for e in m["characters"]:
        if only and e["name"] not in only: continue
        e = dict(e)
        e["model"] = os.path.join(root, os.path.expanduser(e["model"]))
        e["p3"], e["fp"] = template(e)
        e.setdefault("title", e["name"]); e.setdefault("flags", []); e.setdefault("expect", e["name"])
        chars.append(e)
    if only and len(chars) != len(only):
        sys.exit(f"--only: not in the manifest: {sorted(set(only) - {c['name'] for c in chars})}")
    return chars


# ---------------------------------------------------------------- build
class Row:
    """One character's results (the summary table)."""
    def __init__(self, e):
        self.e, self.name = e, e["name"]
        self.build = self.det = self.preview = self.host = self.client = self.vanilla = self.mission = None
        self.secs, self.size, self.sha, self.outfit, self.notes, self.logerr = 0, 0, "", "", [], []
        self.shots = {}   # column -> png
        self.why = []

    def fail(self, msg):
        self.why.append(msg); log(f"  FAIL {self.name}: {msg}")

    def ok(self):
        return not self.why


NOTE_RX = re.compile(r"(?i)\bwarning\b|\bhint\b|\bdropped\b|squeezed|not found|can't|cannot|\bfailed\b|\bmissing\b|\berror\b(?! max| [0-9])")


def build_one(e, bdir, tag):
    """b4bmod survivor for one entry; returns (rc, seconds, pak path, log path)."""
    mods, work = os.path.join(bdir, "mods"), os.path.join(bdir, "work")
    os.makedirs(mods, exist_ok=True); os.makedirs(work, exist_ok=True)
    out, wk = os.path.join(mods, e["name"]), os.path.join(work, e["name"])
    for p in (out, wk, out + ".pak"):
        if os.path.isdir(p): shutil.rmtree(p)
        elif os.path.exists(p): os.remove(p)
    cmd = ["python3", os.path.join(KIT, "b4bmod.py"), "survivor", e["model"],
           "--outfit", e["p3"]] + (["--fp", e["fp"]] if e["fp"] else []) + \
          ["--as", e["name"], "--title", e["title"], "-o", out, "--work", wk] + list(e["flags"])
    lp = os.path.join(OUT, "build", f"{e['name']}{tag}.log")
    os.makedirs(os.path.dirname(lp), exist_ok=True)
    t = time.time()
    with open(lp, "w") as f:
        f.write("$ " + " ".join(cmd) + "\n"); f.flush()
        rc = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, env=dict(os.environ, B4B_EXTRACT=EXTRACT)).returncode
    return rc, time.time() - t, out + ".pak", lp


def tree_diff(a, b):
    """Files that differ between two add-on folders (for a determinism failure)."""
    def files(r):
        return {os.path.relpath(os.path.join(d, n), r): os.path.join(d, n) for d, _, ns in os.walk(r) for n in ns}
    fa, fb = files(a), files(b)
    diff = sorted(k for k in set(fa) | set(fb) if k not in fa or k not in fb or sha256(fa[k]) != sha256(fb[k]))
    return diff


def builds(rows, a):
    for r in rows:
        e = r.e
        log(f"build {r.name}: {os.path.basename(e['model'])} on {e['p3'].rsplit('/', 1)[1]}"
            + (f" + {len(e['flags'])} flag(s)" if e["flags"] else ""))
        rc, r.secs, pak, lp = build_one(e, a.build_dir, "")
        text = open(lp, errors="replace").read()
        r.notes = list(dict.fromkeys(l.strip() for l in text.splitlines() if NOTE_RX.search(l) and not l.startswith("$ ")))
        if rc or not os.path.exists(pak):
            r.build = False
            tail = [l for l in text.splitlines() if l.strip()][-1:] or ["?"]
            r.fail(f"build exit {rc}: {tail[0][:120]}")
            continue
        r.build, r.size, r.sha = True, os.path.getsize(pak), sha256(pak)
        info = os.path.join(a.build_dir, "mods", r.name, "addoninfo.txt")
        m = re.search(r"^outfit=([^|\n]+)", open(info).read() if os.path.exists(info) else "", re.M)
        r.outfit = m.group(1) if m else ""
        if r.outfit != e["expect"]:
            r.fail(f"add-on declares outfit {r.outfit or '-'!r}, expected {e['expect']!r}")
        log(f"  ok {r.secs:.0f}s, {r.size / 1e6:.1f} MB, outfit={r.outfit}, {len(r.notes)} note line(s)")
        if a.twice:
            rc2, s2, pak2, _ = build_one(e, a.build_dir + "2", "-2")
            if rc2 or not os.path.exists(pak2):
                r.det = False; r.fail(f"second build exit {rc2}")
            elif sha256(pak2) == r.sha:
                r.det = True; log(f"  deterministic ({s2:.0f}s): pak sha256 {r.sha[:12]}")
            else:
                r.det = False
                d = tree_diff(os.path.join(a.build_dir, "mods", r.name), os.path.join(a.build_dir + "2", "mods", r.name))
                r.fail(f"second build differs: {len(d)} file(s), e.g. {', '.join(d[:3])}")
            shutil.rmtree(a.build_dir + "2", ignore_errors=True)


def previews(rows, a):
    """3P (front/side/back, the game textures) and face close-up, 4 renders at a time (Cycles CPU)."""
    os.makedirs(os.path.join(OUT, "preview"), exist_ok=True)
    blender = os.environ.get("B4B_BLENDER") or shutil.which("blender") or "blender"
    jobs = []
    for r in rows:
        if not r.build: continue
        wk = os.path.join(a.build_dir, "work", r.name)
        g = os.path.join(wk, "fit3p", "lod0.glb")
        tex = ["--textures", os.path.join(wk, "preview_textures_3p.json")] if os.path.exists(os.path.join(wk, "preview_textures_3p.json")) else []
        jobs.append((r, "prev3p", [g, os.path.join(OUT, "preview", f"{r.name}.png"), "--size", "360"] + tex))
        gf = os.path.join(wk, "fitfp", "lod0.glb")
        if os.path.exists(gf):                         # FP arms in a gun hold from the FP camera
            texf = os.path.join(wk, "preview_textures_fp.json")
            jobs.append((r, "prevfp", [gf, os.path.join(OUT, "preview", f"{r.name}_fp.png"), "--size", "360",
                                       "--fp", "hold"] + (["--textures", texf] if os.path.exists(texf) else [])))
        if os.path.exists(os.path.join(wk, "face_preview.json")):
            jobs.append((r, "prevface", [g, os.path.join(OUT, "preview", f"{r.name}_face.png"), "--size", "360", "--views", "front",
                                         "--face", os.path.join(wk, "face_preview.json")] + tex))

    def run(job):
        r, col, args = job
        lp = os.path.join(OUT, "preview", f"{os.path.basename(args[1])}.log")
        with open(lp, "w") as f:
            rc = subprocess.run([blender, "-b", "--factory-startup", "--python", os.path.join(KIT, "blender", "preview.py"),
                                 "--"] + args, stdout=f, stderr=subprocess.STDOUT, timeout=900).returncode
        return r, col, args[1], rc == 0 and os.path.exists(args[1])

    log(f"previews: {len(jobs)} render(s)")
    with concurrent.futures.ThreadPoolExecutor(4) as ex:
        for r, col, png, ok in ex.map(run, jobs):
            if ok: r.shots[col] = png
            if col == "prev3p": r.preview = ok
            if not ok: r.fail(f"preview {os.path.basename(png)} failed (see preview/*.log)")


# ---------------------------------------------------------------- game
RELEVANT = re.compile(
    r"Fatal|Assertion failed|Unhandled Exception"
    r"|(Error|Warning):.*b4bcoop/"
    r"|Log(SkeletalMesh|Clothing\w*|ChaosCloth|Material\w*|MeshMerge|Skin\w*) (Error|Warning):"
    r"|models: .*(failed to load|not usable|giving up|refused)"
    r"|addons: (conflict|.*line ignored|can't|bad )"
    r"|paks: .*(fail|refus|mismatch)", re.I)


def norm(line):
    return re.sub(r"\d+", "#", re.sub(r"^\[[^\]]*\]\s*", "", line)).strip()


# The game logs every synchronous hero-mesh load as an error, retail rows too; with #33 it loads add-on outfits
# itself (a real customization row), so that line names our paths and is no failure.
BENIGN = re.compile(r"LogCustomization Error: Forcing load for hero mesh definition")


def relevant(text):
    return [l for l in text.splitlines() if RELEVANT.search(l) and not BENIGN.search(l)]


def install_addons(rows):
    """The built add-ons (only these, all on) into the lane's game folder; the lock's backup keeps the player's own."""
    st = subprocess.run([os.path.join(REPO, "launch/lane-restore.sh"), "status"], capture_output=True, text=True).stdout
    if "backup present" not in st:
        raise RuntimeError(f"lane {LANE}: no player backup (hold launch/gamelock.sh first): {st.strip()}")
    if os.path.isdir(ADDONS): shutil.rmtree(ADDONS)
    os.makedirs(ADDONS)
    lines = []
    for r in rows:
        if not r.build: continue
        shutil.copy(os.path.join(ARGS.build_dir, "mods", r.name + ".pak"), ADDONS)
        lines.append(f"{r.name}.pak=1")
    open(os.path.join(ADDONS, "addonlist.txt"), "w").write("\n".join(lines) + "\n")
    log(f"add-ons: {len(lines)} installed in {ADDONS}")


def heroes(n):
    """Instance n's `face` hero list: [{idx, actor, mesh, at (x, y, z), you}] (dev `face`: position + local pawn)."""
    rx = r"^#(\d+) (\S+) mesh=(\S+)(?: at=\(([-\d.]+) ([-\d.]+) ([-\d.]+)\))?( \(you\))?"
    return [{"idx": int(m.group(1)), "actor": m.group(2), "mesh": m.group(3),
             "at": tuple(float(m.group(k) or 0) for k in (4, 5, 6)), "you": bool(m.group(7))}
            for m in re.finditer(rx, e2e.agent(n, "face"), re.M)]


def mine(n):
    return next((h for h in heroes(n) if h["you"]), None)


def nearest(n, pos):
    """Instance n's hero (not its own) standing closest to pos: pairs one hero across machines."""
    hs = [h for h in heroes(n) if not h["you"] and h["mesh"] != "-"]
    return min(hs, key=lambda h: (h["at"][0] - pos[0]) ** 2 + (h["at"][1] - pos[1]) ** 2, default=None) if pos else None


def wearer(n, name):
    """The hero wearing outfit <name> on instance n (its mesh is under /b4bcoop/outfits/<name>/), or None."""
    return next((h for h in heroes(n) if f"/b4bcoop/outfits/{name}/".lower() in h["mesh"].lower()), None)


def shot(r, n, col, label):
    png = os.path.join(OUT, "shots", f"{r.name}-{col}.png")
    os.makedirs(os.path.dirname(png), exist_ok=True)
    e2e.sh([os.path.join(REPO, "launch/shot.sh"), str(n), png], timeout=40, out=os.path.join(OUT, "shots.log"))
    if os.path.exists(png): r.shots[col] = png
    else: log(f"  no screenshot {label} from #{n}")


def wear(S, r, tag):
    """Host puts the outfit on; host and client (both with the add-on) must log it and show its mesh on the host's
    hero (the host: its own). Returns True when worn."""
    name = r.e["expect"]
    # already on (the campaign run's save keeps the host's looks from Fort Hope into the mission): nothing new is logged
    worn = bool((lambda h: h and h["you"])(wearer(H, name)) and wearer(C, name))
    for g in S.logs.values(): g.mark()
    out = e2e.agent(H, "model", name)
    rx = rf"models: hero slot \d+ wears outfit {re.escape(name)}\b"
    seen = {n: worn or e2e.wait_for(lambda n=n: S.logs[n].grep(rx), 30, 1) for n in (H, C)}
    w_h = e2e.wait_for(lambda: (lambda h: h if h and h["you"] else None)(wearer(H, name)), 10, 1)
    w_c = e2e.wait_for(lambda: wearer(C, name), 10, 1)
    ok = bool(all(seen.values()) and w_h and w_c)
    if not ok:
        r.fail(f"{tag}: not worn: host '{out.strip().splitlines()[0][:60] if out.strip() else '-'}', log host="
               f"{bool(seen[H])} client={bool(seen[C])}, mesh on the host's hero: host={bool(w_h)} client={bool(w_c)}")
    return ok


def face_to(n):
    """Host stands in front of instance n's hero, facing it (dev `face look` on the host; a client can't move itself):
    n's camera then looks at the host. Returns the host-side hero index or None."""
    me = mine(n)
    h = nearest(H, me["at"]) if me else None
    return h["idx"] if h else None


def look(idx, d, dz=3):
    return e2e.agent(H, "face", "look", idx, d, dz)


def motion(S, r, ic):
    """--motion (mission): the face while talking (the host's hero says a line, the client's camera 70 cm in front of
    it) and the outfit running (put on a bot, which runs 8 m to the side while the host looks at it)."""
    name = r.e["expect"]
    me = mine(H)
    if ic is not None and me:
        look(ic, 70); time.sleep(2)
        e2e.agent(H, "face", "say", me["idx"], "Ping_Affirmative")
        time.sleep(0.5); shot(r, C, "m_talk", "talking (client view)")
    # dev `players`: "[k] <class> name=<player> pawn=<class>"; a bot's player state has no name and a hero pawn
    bots = re.findall(r"^\s*\[(\d+)\] \S+ name= pawn=Hero_", e2e.agent(H, "players"), re.M)
    if not bots:
        log("  motion: no bot to run"); return
    e2e.agent(H, "model", "#" + bots[0], name)
    b = e2e.wait_for(lambda: next((h for h in heroes(H) if not h["you"] and
                                   f"/b4bcoop/outfits/{name}/".lower() in h["mesh"].lower()), None), 15, 1)
    if not b:
        log(f"  motion: bot #{bots[0]} doesn't wear {name}"); return
    look(b["idx"], 400, 0); time.sleep(2)
    x, y, z = b["at"]
    me = mine(H)
    dx, dy = (me["at"][0] - x, me["at"][1] - y) if me else (1.0, 0.0)
    n = max(1e-3, (dx * dx + dy * dy) ** 0.5)
    # across the host's view (perpendicular to host -> bot), 3 m: the bot stays in the picture while it runs
    e2e.agent(H, "face", "walk", b["idx"], round(x - dy / n * 300), round(y + dx / n * 300), round(z))
    time.sleep(0.8); shot(r, H, "m_run", "bot running in the outfit")
    e2e.agent(H, "model", "#" + bots[0], "reset")


VK_CTRL, VK_D = 17, 0x44    # the game's crouch (LeftControl) and move-right keys (DefaultInput.ini)


def poses(S, r, ic):
    """--poses (mission): the host's hero, in the outfit, where the game bends a survivor most (weights, stray cloth):
    crouching, jogging sideways, turning in place, aiming. Seen from the client (the host 3 m in front of it, facing
    it) and from the host's own third-person camera (the host's free look is off for this: the turn turns the hero)."""
    if ic is None:
        log("  poses: client's hero not found on the host"); return
    def z():
        me = mine(H)
        return me["at"][2] if me else None
    e2e.agent(H, "thirdperson", "on")
    look(ic, 300, 0); time.sleep(2)
    z0 = z()
    e2e.agent(H, "cheatprobe", "press", VK_CTRL, 6); time.sleep(1.5)
    zc = z()
    shot(r, C, "p_crouch", "crouching (client view)")
    shot(r, H, "p_crouch_3p", "crouching (host 3P)")
    time.sleep(5)
    if z0 is not None and zc is not None and z0 - zc > 5 and (z() or z0) < z0 - 5:   # crouch is a toggle: stand up
        e2e.agent(H, "cheatprobe", "press", VK_CTRL, 0.2); time.sleep(1.5)
    log(f"  poses: crouch {('%.0f cm lower' % (z0 - zc)) if z0 is not None and zc is not None else '?'}")
    look(ic, 300, 0); time.sleep(2)
    e2e.agent(H, "cheatprobe", "press", VK_D, 1.4); time.sleep(0.5)
    shot(r, C, "p_walk", "jogging sideways (client view)")
    shot(r, H, "p_walk_3p", "jogging (host 3P)")
    time.sleep(2)
    look(ic, 300, 0); time.sleep(2)
    e2e.agent(H, "thirdperson", "look", 0, 150, 2.5); time.sleep(0.6)
    shot(r, C, "p_turn", "turning in place (client view)")
    shot(r, H, "p_turn_3p", "turning in place (host 3P)")
    time.sleep(2.5)
    look(ic, 300, 0); time.sleep(2)
    e2e.agent(H, "cheatprobe", "input", "rmb", 5); time.sleep(1.5)
    shot(r, C, "p_aim", "aiming (client view)")
    time.sleep(4)
    e2e.agent(H, "thirdperson", "off")


def check_logs(S, r, insts, baseline, tag):
    new = []
    for n in insts:
        for l in relevant(S.logs[n].text()):
            if norm(l) not in baseline:
                new.append(f"#{n} {l.strip()[:200]}")
                baseline.add(norm(l))
    alive = [n for n in insts if "world:" in e2e.agent(n, "status")]
    if len(alive) != len(insts):
        new.append(f"instance(s) not answering: {sorted(set(insts) - set(alive))}")
    if new:
        r.logerr += [f"{tag}: {x}" for x in new]
        r.fail(f"{tag}: {len(new)} new log error(s), e.g. {new[0][:140]}")
    return not new


def game(rows, a):
    worn = [r for r in rows if r.build]
    if not worn:
        log("game: nothing built"); return
    n = 3 if a.vanilla else 2
    insts = list(range(1, n + 1))
    install_addons(worn)
    if a.vanilla: os.environ["B4B_INI_EXTRA3"] = "addons=0"
    # the host lets every add-on in, so a mis-classed add-on is reported (below) instead of blocking the whole session
    # --poses: the turn must turn the hero, not orbit the third-person camera (free look while standing still)
    os.environ["B4B_INI_EXTRA1"] = "addons_policy=any" + (";thirdperson_freelook=0" if a.poses else "")
    S = e2e.Session("chars", n, timeout=480)
    try:
        c0 = time.time()
        if not S.launch() or not e2e.wait_for(lambda: all(e2e.has_hero(i) for i in insts), 120, 3):
            for r in worn: r.fail("game: session didn't start (multi.sh)")
            return
        log(f"session up ({time.time() - c0:.0f}s): {n} instances in {S.world(H)}")
        for i in insts: e2e.agent(i, "model", "list", "outfits")
        time.sleep(2)
        reg = {i: set(S.logs[i].grep(r"models: add-on outfit (\S+) ", since_mark=False)) for i in insts}
        log("add-on outfits registered: " + ", ".join(f"#{i} {len(reg[i])}" for i in insts))
        # each add-on's content class as the game sees it: anything but cosmetic is refused by a host with the
        # addons_policy=cosmetic (not the default since #43, but a host's choice: a joiner with it couldn't play there)
        cls = dict(re.findall(r"addons: \d+\. (\S+)\.pak .*\n.*addons:    content: (.*)", S.logs[H].text(since_mark=False)))
        for r in worn:
            c = cls.get(r.name, "?")
            if not c.startswith("cosmetic"):
                r.fail(f"add-on content is '{c[:90]}': a host with addons_policy=cosmetic refuses a joiner with it")
        baseline = {norm(l) for i in insts for l in relevant(S.logs[i].text(since_mark=False))}
        for l in sorted(baseline): log(f"  baseline (ignored later): {l[:160]}")
        for r in worn:
            log(f"wear {r.name} (Fort Hope)")
            e2e.agent(H, "thirdperson", "off")
            r.host = r.client = wear(S, r, "Fort Hope")
            if r.host:
                ic = face_to(C)
                if ic is None:
                    r.fail("Fort Hope: client's hero not found on the host (face list)")
                else:
                    look(ic, 260, 0); time.sleep(3)
                    shot(r, C, "client_body", "client full body")
                    look(ic, 70); time.sleep(2)
                    shot(r, C, "client_face", "client face")
                    e2e.agent(H, "thirdperson", "on"); time.sleep(2)
                    shot(r, H, "host_3p", "host 3P")
                    e2e.agent(H, "thirdperson", "off")
                if a.vanilla:
                    iv = face_to(V)
                    if iv is not None:
                        look(iv, 70); time.sleep(3)
                        shot(r, V, "vanilla_face", "vanilla client face")
                    hm = mine(H)
                    hv = nearest(V, hm["at"]) if hm else None
                    vlog = S.logs[V].grep(rf"models: hero slot \d+ wears outfit {re.escape(r.e['expect'])}\b")
                    r.vanilla = bool(iv is not None and hv and "/b4bcoop/" not in hv["mesh"] and not vlog)
                    if not r.vanilla:
                        r.fail(f"vanilla client: " + ("host hero not found" if not hv or iv is None else
                                                      f"sees {hv['mesh'].rsplit('/', 1)[-1]}, log wears={bool(vlog)}"))
            check_logs(S, r, insts, baseline, "Fort Hope")
        # mission: a subset in Evansburgh (the character select ends with `ready`, then the saferoom)
        sub = ([r for r in worn if r.e.get("mission")] or worn)[:a.mission]
        if sub:
            log(f"mission Easy (Evansburgh) for {', '.join(r.name for r in sub)}")
            e2e.agent(H, "thirdperson", "off")
            e2e.agent(H, "mission", "Easy")
            ok = e2e.wait_for(lambda: all(e2e.MAP_B in S.world(i) for i in insts), 300, 5)
            ok = ok and e2e.wait_for(lambda: (e2e.agent(H, "ready") and False) or
                                     all(heroes(i) and all(h["mesh"] != "-" for h in heroes(i)) for i in insts), 90, 10)
            if not ok:
                for r in sub: r.mission = False; r.fail("mission: no heroes in Evansburgh_B")
                return
            time.sleep(8)
            for r in sub:
                log(f"wear {r.name} (Evansburgh)")
                r.mission = wear(S, r, "mission")
                if r.mission:
                    time.sleep(2)
                    shot(r, H, "m_host_fp", "mission host FP")
                    ic = face_to(C)
                    if ic is not None:
                        look(ic, 170, 0); time.sleep(3)
                        shot(r, C, "m_client", "mission client")
                    e2e.agent(H, "thirdperson", "on"); time.sleep(2)
                    shot(r, H, "m_host_3p", "mission host 3P")
                    e2e.agent(H, "thirdperson", "off")
                    if a.motion:
                        motion(S, r, ic)
                    if a.poses:
                        poses(S, r, ic)
                if not check_logs(S, r, insts, baseline, "mission"): r.mission = False
    finally:
        S.stop()


# ---------------------------------------------------------------- output
COLS = [("prev3p", "preview 3P"), ("prevface", "preview face"), ("prevfp", "preview FP"), ("host_3p", "host 3P"),
        ("client_face", "client face"), ("client_body", "client body"), ("vanilla_face", "no add-ons"),
        ("m_host_fp", "mission FP"), ("m_host_3p", "mission 3P"), ("m_client", "mission client"),
        ("m_talk", "talking"), ("m_run", "running"), ("p_crouch", "crouch"), ("p_crouch_3p", "crouch 3P"),
        ("p_walk", "jog sideways"), ("p_walk_3p", "jog 3P"), ("p_turn", "turn"), ("p_turn_3p", "turn 3P"), ("p_aim", "aim")]


def contact(rows):
    cols = [c for c in COLS if any(c[0] in r.shots for r in rows)]
    if not cols or not shutil.which("montage"): return None
    args = ["montage"]
    for r in rows:
        for i, (k, lbl) in enumerate(cols):
            p = r.shots.get(k)
            args += ["-label", f"{r.name}: {lbl}" if i == 0 else lbl, p if p else "xc:gray20"]
    out = os.path.join(OUT, "contact.png")
    args += ["-tile", f"{len(cols)}x", "-geometry", "320x180+4+4", "-background", "gray12", "-fill", "white",
             "-pointsize", "13", "-font", "DejaVu-Sans", "-depth", "8", out]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode: log(f"contact sheet: {r.stderr.strip()[:200]}"); return None
    return out


def mark(v):
    return "-" if v is None else "ok" if v else "FAIL"


def summary(rows, sheet):
    hdr = f"{'character':12} {'template':22} {'build':5} {'time':>5} {'MB':>6} {'flags':>5} {'det':4} {'notes':>5} " \
          f"{'prev':4} {'host':4} {'client':6} {'vanilla':7} {'mission':7} {'logerr':>6}  result"
    lines = [hdr]
    for r in rows:
        tpl = re.sub(r"^3P_|_SKM$", "", r.e["p3"].rsplit("/", 1)[1])
        lines.append(f"{r.name:12} {tpl:22} {mark(r.build):5} {r.secs:4.0f}s {r.size / 1e6:6.1f} {len(r.e['flags']):5d} "
                     f"{mark(r.det):4} {len(r.notes):5d} {mark(r.preview):4} {mark(r.host):4} {mark(r.client):6} "
                     f"{mark(r.vanilla):7} {mark(r.mission):7} {len(r.logerr):6d}  {'PASS' if r.ok() else 'FAIL'}")
    for r in rows:
        for w in r.why: lines.append(f"  {r.name}: {w}")
    bad = sum(not r.ok() for r in rows)
    lines.append(f"{len(rows) - bad}/{len(rows)} characters passed in {time.time() - T0:.0f}s; artifacts {OUT}"
                 + (f"; contact sheet {sheet}" if sheet else ""))
    return "\n".join(lines), bad


def main():
    global OUT, EXTRACT, ARGS
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", default=os.environ.get("B4B_CHARSUITE") or os.path.join(DATA, "characters", "suite.json"))
    ap.add_argument("--only", help="comma-separated names")
    ap.add_argument("--twice", action="store_true", help="build each twice, compare the paks (determinism)")
    ap.add_argument("--no-preview", action="store_true")
    ap.add_argument("--no-build", action="store_true", help="use the paks already in --build-dir")
    ap.add_argument("--no-game", action="store_true")
    ap.add_argument("--vanilla", action="store_true", help="a 3rd instance without add-ons (must see vanilla)")
    ap.add_argument("--mission", type=int, default=2, help="characters worn in Evansburgh (0 = no mission step)")
    ap.add_argument("--motion", action="store_true", help="mission: face while talking, the outfit running (on a bot)")
    ap.add_argument("--poses", action="store_true", help="mission: the host crouching, jogging, turning, aiming "
                    "(client view + host 3P)")
    ap.add_argument("--install", action="store_true", help="launch/install.sh (this checkout's dev build) first")
    ap.add_argument("--no-lock", action="store_true", help="don't take launch/gamelock.sh (caller holds it)")
    ap.add_argument("--out", help="artifacts (default /tmp/b4b-charsuite-<time>)")
    ap.add_argument("--build-dir", default=os.path.join(DATA, "charsuite", "build"))
    ARGS = a = ap.parse_args()
    EXTRACT = os.environ.get("B4B_EXTRACT") or os.path.join(DATA, "charsuite", "extract")
    OUT = a.out or f"/tmp/b4b-charsuite-{'' if LANE == '1' else 'l' + LANE + '-'}{datetime.datetime.now():%Y%m%d-%H%M%S}"
    os.makedirs(OUT, exist_ok=True)
    e2e.OUT, e2e.T0, e2e.log = OUT, T0, log   # e2e's helpers log into our charsuite.log
    chars = load_manifest(a.manifest, a.only.split(",") if a.only else None)
    rows = [Row(e) for e in chars]
    log(f"{len(rows)} character(s) from {a.manifest}; builds {a.build_dir}; extract {EXTRACT}; output {OUT}")
    lock = os.path.join(REPO, "launch/gamelock.sh")
    held = False
    try:
        if a.no_build:
            for r in rows:
                pak = os.path.join(a.build_dir, "mods", r.name + ".pak")
                r.build = os.path.exists(pak)
                if r.build:
                    r.size, r.sha = os.path.getsize(pak), sha256(pak)
                    m = re.search(r"^outfit=([^|\n]+)", open(os.path.join(a.build_dir, "mods", r.name, "addoninfo.txt")).read(), re.M)
                    r.outfit = m.group(1) if m else ""
                else: r.fail(f"--no-build: no {pak}")
        else:
            builds(rows, a)
        if not a.no_preview: previews(rows, a)
        if not a.no_game:
            if not a.no_lock:
                log("waiting for the game lock (launch/gamelock.sh acquire charsuite)")
                subprocess.run([lock, "acquire", "charsuite"], check=True); held = True
            if a.install:
                rc = e2e.sh([os.path.join(REPO, "launch/install.sh")], timeout=900, out=os.path.join(OUT, "install.log"))
                if rc: raise RuntimeError(f"launch/install.sh exit {rc} (install.log)")
            game(rows, a)
    except KeyboardInterrupt:
        log("interrupted")
        for r in rows: r.why.append("interrupted")
    except Exception as ex:
        log(f"harness error: {ex!r}")
        for r in rows: r.why.append(f"harness error {ex!r}")
    finally:
        if held:
            e2e.sh([os.path.join(REPO, "launch/multi-stop.sh")], timeout=60)
            subprocess.run([lock, "release", "charsuite"])
    with open(os.path.join(OUT, "notes.txt"), "w") as f:
        for r in rows:
            f.write(f"== {r.name}\n" + "".join(f"  {n}\n" for n in r.notes) + "".join(f"  LOG {x}\n" for x in r.logerr))
    sheet = contact(rows)
    text, bad = summary(rows, sheet)
    print("\n" + text)
    open(os.path.join(OUT, "summary.txt"), "w").write(text + "\n")
    json.dump([{"name": r.name, "build": r.build, "seconds": round(r.secs), "bytes": r.size, "sha256": r.sha,
                "outfit": r.outfit, "flags": r.e["flags"], "deterministic": r.det, "preview": r.preview,
                "host": r.host, "client": r.client, "vanilla": r.vanilla, "mission": r.mission,
                "log_errors": r.logerr, "failures": r.why, "shots": r.shots} for r in rows],
              open(os.path.join(OUT, "results.json"), "w"), indent=1)
    sys.exit(1 if bad or not rows else 0)


if __name__ == "__main__":
    main()
