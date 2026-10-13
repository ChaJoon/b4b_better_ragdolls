#!/usr/bin/env python3
"""End-to-end regression suite: real game instances on the test prefixes (launch/multi.sh), driven by the agent.

    tools/e2e.py [--quick | --full] [--no-lock] [--out DIR]

--quick (default, ~6 min): 2 instances (host + client), Evansburgh Easy:
    join, mission follow, client flashlight replicated to the host, a host and a client burn card (charged to their
    own profiles), chat `/players` typed on the client, ready + endmission success with the client's SP forwarded,
    seamless chapter transition, profile diffs after the deferred save, the 0.3.0 defaults (host with no host= line,
    Steam-only presence with the protocol, protocol in the login options), add-on privacy (#35: the host announces its
    addons_policy, the client checks itself and logs in with only "checked", no add-on list anywhere in the host's
    log, /addons players gone), and (ss, all along) no public TCP/UDP peer and no game socket bound off loopback.
--full (~15 min in all): also 5 instances without teamsize (the 5th is refused with "Server full.", host survives) and a
    teamsize=5 round (5 humans follow into the mission, rewards forwarded to all 4 clients).

Uses the installed DLL as is (launch/install.sh first). Holds launch/gamelock.sh as "e2e" unless --no-lock (the
caller already holds it). Everything (logs, agent outputs, profile snapshots and diffs, ss samples, screenshots) goes
to a timestamped directory, default /tmp/b4b-e2e-<time>. Exit status 1 if any check failed.
Before each session every prefix it uses gets its golden profile back (tools/testprefix.py --restore; logged as
"profile testN: ..."), so a profile the game wiped or a run left behind never breaks the next run; the profile checks
still diff this run's before/after. --keep-profiles skips the restore. B4B_STEAM=flatpak restores that account's own
set (profile-golden-flatpak/: the native account's saves don't load under it).
B4B_LANE=2 runs it on the second live-test lane (launch/lane.sh: Flatpak game copy, its own lock, prefixes, ports and
window names; artifacts /tmp/b4b-e2e-l2-<time>); both lanes can run at the same time.
"""
import argparse, datetime, glob, ipaddress, json, os, re, shutil, socket, subprocess, sys, threading, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
from lane import LANE, GAME, ROOT, PORT_BASE, WIN   # B4B_LANE=2: second live-test lane (tools/lane.py)
import testprefix
BIN = os.path.join(GAME, "Gobi/Binaries/Win64")
PROFILE = "pfx/drive_c/users/steamuser/AppData/Local/Back4Blood/Steam/Saved/SaveGames/PlayerProfileSettings.json"
MAP_B, MAP_C = "Evansburgh_B", "Evansburgh_C"

OUT = None           # run directory
KEEP_PROFILES = False  # --keep-profiles: don't restore the golden profiles
RESULTS = []         # (session, check, ok, seconds, detail)
T0 = time.time()


def log(msg):
    line = f"[{time.time() - T0:7.1f}s] {msg}"
    print(line, flush=True)
    with open(os.path.join(OUT, "e2e.log"), "a") as f: f.write(line + "\n")


# ---------------------------------------------------------------- agent / instances
def agent(n, *args, timeout=20):
    """Send one command to instance n's agent (1-based); '' if it is not reachable."""
    cmd = " ".join(str(a) for a in args)
    try:
        s = socket.create_connection(("127.0.0.1", PORT_BASE + n - 1), timeout=timeout)
        s.settimeout(timeout)
        s.sendall((cmd + "\n").encode())
        chunks = []
        while True:
            d = s.recv(65536)
            if not d: break
            chunks.append(d)
        s.close()
        out = b"".join(chunks).decode(errors="replace")
    except OSError as e:
        out = ""
        log(f"  agent {n} '{cmd}': {e}")
    with open(os.path.join(OUT, "agent.log"), "a") as f:
        f.write(f"--- [{time.time() - T0:.1f}s] #{n} {cmd}\n{out}")
    return out


def wait_for(fn, timeout, interval=3.0):
    """Poll fn() until it returns something truthy; returns that value, or None on timeout."""
    end = time.time() + timeout
    while True:
        v = fn()
        if v: return v
        if time.time() >= end: return None
        time.sleep(interval)


def sh(args, timeout=None, env=None, out=None):
    e = dict(os.environ, **(env or {}))
    with open(out or os.devnull, "a") as f:
        return subprocess.run(args, stdout=f, stderr=subprocess.STDOUT, env=e, timeout=timeout).returncode


def game_pids():
    """PIDs of our own processes on a test prefix (games and their wineservers)."""
    pids = set()
    for p in os.listdir("/proc"):
        if not p.isdigit(): continue
        try:
            env = open(f"/proc/{p}/environ", "rb").read()
        except OSError:
            continue
        if f"B4B_PREFIX={ROOT}/test".encode() in env: pids.add(int(p))
    return pids


def window_id(n):
    try:
        out = subprocess.run(["wmctrl", "-l"], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    for line in out.splitlines():
        if f"{WIN} #{n}" in line and (n != 1 or "HOST" in line) and not re.search(rf"{WIN} #{n}\d", line):
            return line.split()[0]
    return None


def screenshot(n, name):
    sh([os.path.join(REPO, "launch/shot.sh"), str(n), os.path.join(OUT, name)], timeout=30)


def log_snapshot(n):
    """mtime of every existing b4bcoop-test<n>-*.log, taken before a launch (GameLog: written since = this launch's)."""
    out = {}
    for p in glob.glob(os.path.join(BIN, f"b4bcoop-test{n}-*.log")):
        try: out[p] = os.path.getmtime(p)
        except OSError: pass
    return out


class GameLog:
    """The agent log of instance n from this launch (b4bcoop-test<n>-<winpid>.log, rewritten per process).
    Wine hands out the same few Windows PIDs, so the previous session's log can have this session's name, or another
    name with an mtime just after this launch's start (its game was stopped a moment before). Only a log written after
    the launch counts: a new file, or one whose mtime moved past its snapshot (the new process truncated it)."""
    def __init__(self, n, since, before=None):
        self.n, self.since, self.path, self.mark_pos = n, since, None, 0
        self.before = before if before is not None else log_snapshot(n)

    def find(self):
        if not self.path:
            c = []
            for p in glob.glob(os.path.join(BIN, f"b4bcoop-test{self.n}-*.log")):
                try: m = os.path.getmtime(p)
                except OSError: continue
                if m >= self.since and (p not in self.before or m > self.before[p]): c.append((m, p))
            if c: self.path = max(c)[1]
        return self.path

    def text(self, since_mark=True):
        if not self.find(): return ""
        with open(self.path, "rb") as f:
            if since_mark: f.seek(self.mark_pos)
            return f.read().decode(errors="replace")

    def mark(self):
        self.mark_pos = os.path.getsize(self.path) if self.find() else 0

    def grep(self, pattern, since_mark=True):
        return re.findall(pattern, self.text(since_mark), re.M)


# ---------------------------------------------------------------- checks
class Check:
    def __init__(self, session, name):
        self.session, self.name, self.t = session, name, time.time()
        log(f"CHECK {session}: {name}")

    def done(self, ok, detail=""):
        dt = time.time() - self.t
        RESULTS.append((self.session, self.name, bool(ok), dt, detail))
        log(f"  {'PASS' if ok else 'FAIL'} {self.name} ({dt:.0f}s) {detail}")
        return bool(ok)


# ---------------------------------------------------------------- netguard: sample sockets while the games run
def public(addr):
    host = addr.rsplit(":", 1)[0].strip("[]")
    if host in ("*", "0.0.0.0", "::", ""): return False
    host = host.split("%")[0]
    try:
        ip = ipaddress.ip_address(host)
    except ValueError:
        return False
    if getattr(ip, "ipv4_mapped", None): ip = ip.ipv4_mapped
    return not (ip.is_private or ip.is_loopback or ip.is_link_local or ip.is_unspecified)


def loopback(addr):
    host = addr.rsplit(":", 1)[0].strip("[]").split("%")[0]
    try:
        ip = ipaddress.ip_address(host)
    except ValueError:
        return False
    if getattr(ip, "ipv4_mapped", None): ip = ip.ipv4_mapped
    return ip.is_loopback


def steam_native(line):
    """A socket of a Proton game that Wine's wineserver does not co-hold was not made through Wine's ws2_32 (every
    Windows module's socket is a wineserver object, so wineserver keeps an fd of it): it is native Linux code in the
    game process, and the only native networking code there is Steam's own client library (steamclient.so, loaded by
    Proton's lsteamclient). Its relay pings (27 UDP sockets on 0.0.0.0 that stay open, whenever Steam initialises or
    refreshes relay network access; dev `steamnet ping 0` triggers them) are Steam's, outside our loopback promise and
    outside netguard (docs/investigations/steam-p2p.md "Loopback binding")."""
    return "wineserver" not in line and '"Back4Blood.exe"' in line


# The add-on shop's list (shop.c) is fetched once per game start in Fort Hope from raw.githubusercontent.com (GitHub's
# Fastly addresses), inside netguard's updater scope: HTTPS to those addresses is expected, listed apart (.shop).
SHOP_NETS = [ipaddress.ip_network(n) for n in ("185.199.108.0/22", "2606:50c0::/32")]


def shop_peer(addr):
    host, _, port = addr.rpartition(":")
    try:
        ip = ipaddress.ip_address(host.strip("[]").split("%")[0])
    except ValueError:
        return False
    if getattr(ip, "ipv4_mapped", None): ip = ip.ipv4_mapped
    return port == "443" and any(ip in n for n in SHOP_NETS if n.version == ip.version)


class SocketSampler(threading.Thread):
    """ss -tunapH every second; any socket of a test-prefix process with a public peer is a finding, and so is (host_ip=0,
    the default) any UDP socket or TCP listener of theirs that is not bound to loopback. Sockets of Steam's native
    client library (steam_native) are listed apart (.steam) and are no finding, and so are HTTPS connections to GitHub's
    raw.githubusercontent.com addresses (.shop: the add-on shop's list, shop_peer)."""
    def __init__(self):
        super().__init__(daemon=True)
        self.stop_ev, self.samples, self.public, self.seen, self.exposed = threading.Event(), 0, [], set(), []
        self.steam = []   # (key, line): Steam's own (native steamclient.so) sockets off loopback or with a public peer
        self.shop = []    # (key, line): the add-on shop's list from GitHub (shop_peer)

    def run(self):
        f = open(os.path.join(OUT, "ss-samples.txt"), "a")
        while not self.stop_ev.is_set():
            pids = game_pids()
            if pids:
                try:
                    out = subprocess.run(["ss", "-tunapH"], capture_output=True, text=True, timeout=10).stdout
                except (OSError, subprocess.TimeoutExpired):
                    out = ""
                self.samples += 1
                for line in out.splitlines():
                    m = re.findall(r"pid=(\d+)", line)
                    if not m or not any(int(p) in pids for p in m): continue
                    cols = line.split()
                    if len(cols) < 6: continue
                    key = (cols[0], cols[4], cols[5])
                    native = steam_native(line)
                    if key not in self.seen:
                        self.seen.add(key)
                        f.write(f"{datetime.datetime.now():%H:%M:%S} {'[steam native] ' if native else ''}{line}\n"); f.flush()
                    # reachable from the network: a UDP socket or TCP listener not bound to loopback (host_ip=0)
                    exposed = (cols[0] == "udp" or cols[1] == "LISTEN") and not loopback(cols[4])
                    if native:
                        if (exposed or public(cols[5])) and key not in [k for k, _ in self.steam]:
                            self.steam.append((key, line))
                        continue
                    if cols[0] == "tcp" and shop_peer(cols[5]):
                        if key not in [k for k, _ in self.shop]: self.shop.append((key, line))
                        continue
                    if public(cols[5]) and key not in [k for k, _ in self.public]:
                        self.public.append((key, line))
                    if exposed and key not in [k for k, _ in self.exposed]:
                        self.exposed.append((key, line))
            self.stop_ev.wait(1)
        f.close()


# ---------------------------------------------------------------- profiles
def profile_path(n):
    return os.path.join(ROOT, f"test{n}", PROFILE)


def load_profile(path):
    try:
        with open(path) as f: return json.load(f)
    except (OSError, ValueError):
        return None


def flat(d, p=""):
    out = {}
    if isinstance(d, dict):
        for k, v in d.items(): out.update(flat(v, f"{p}.{k}" if p else k))
    elif isinstance(d, list):
        out[p] = json.dumps(d, sort_keys=True)
    else:
        out[p] = d
    return out


def consumable_spent(prof, row):
    for k, v in (prof or {}).get("offlineData", {}).get("consumables", {}).items():
        if f'RowDisplayName="{row}"' in k: return v.get("spent", 0)
    return 0


def sp(prof):
    return (prof or {}).get("offlineData", {}).get("supplyPoints", {}).get("acquired", 0)


def stp(prof):
    return (prof or {}).get("offlineData", {}).get("skullTotemPoints", {}).get("acquired", 0)


def write_diff(before, after, name):
    a, b = flat(before or {}), flat(after or {})
    lines = []
    for k in sorted(set(a) | set(b)):
        if a.get(k) == b.get(k): continue
        if k.startswith("offlineData.campaignRuns.") and len(lines) > 400: continue
        va, vb = a.get(k), b.get(k)
        if isinstance(va, str) and len(va) > 200: va = va[:200] + "..."
        if isinstance(vb, str) and len(vb) > 200: vb = vb[:200] + "..."
        lines.append(f"{k}: {va} -> {vb}")
    with open(os.path.join(OUT, name), "w") as f: f.write("\n".join(lines) + "\n")
    return lines


# ---------------------------------------------------------------- sessions
class Session:
    def __init__(self, name, n, ini_extra="", timeout=420):
        self.name, self.n, self.ini_extra, self.timeout = name, n, ini_extra, timeout
        self.logs, self.proc, self.start, self.prepared = {}, None, None, False

    def prepare(self):
        """Stop leftover test instances, then restore each prefix's golden profile (once per session)."""
        if self.prepared: return
        self.prepared = True
        if subprocess.run([os.path.join(REPO, "launch/multi-stop.sh"), "--list"], capture_output=True, text=True).stdout.strip():
            log("test instances already running: stopping them first")
            sh([os.path.join(REPO, "launch/multi-stop.sh")], timeout=60)
            time.sleep(3)
        for i in range(1, self.n + 1):
            dst = os.path.join(ROOT, f"test{i}")
            if not os.path.isdir(os.path.join(dst, "pfx")):   # a lane's first run: create the prefix now
                sh([sys.executable, os.path.join(REPO, "tools/testprefix.py"), str(i)], timeout=300)
            if KEEP_PROFILES:
                log(f"profile test{i}: kept as is (--keep-profiles): {testprefix.profile_health(os.path.join(dst, testprefix.SAVES))[1]}")
                continue
            try:
                log(f"profile test{i}: {testprefix.restore_golden(dst)}")
            except SystemExit as e:
                log(f"profile test{i}: NOT restored: {e}")

    def launch(self, wait=True):
        """multi.sh n. wait=True: returns multi.sh's success (host sees n players)."""
        self.prepare()
        self.start = time.time() - 2
        self.logs = {i: GameLog(i, self.start, log_snapshot(i)) for i in range(1, self.n + 1)}
        env = {"B4B_INI_EXTRA": self.ini_extra, "B4B_TIMEOUT": str(self.timeout)}
        out = os.path.join(OUT, f"{self.name}-multi.out")
        log(f"launch {self.name}: multi.sh {self.n} {('(' + self.ini_extra + ')') if self.ini_extra else ''}")
        self.proc = subprocess.Popen([os.path.join(REPO, "launch/multi.sh"), str(self.n)], stdout=open(out, "w"),
                                     stderr=subprocess.STDOUT, env=dict(os.environ, **env), start_new_session=True)
        if not wait: return None
        try:
            return self.proc.wait(timeout=self.timeout + 200) == 0
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return False

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()   # multi.sh only; the games run in their own sessions
        for i in range(1, self.n + 1):
            if window_id(i) or os.environ.get("B4B_GPU"): screenshot(i, f"{self.name}-end-{i}.png")
        sh([os.path.join(REPO, "launch/multi-stop.sh")], timeout=60, out=os.path.join(OUT, f"{self.name}-multi.out"))
        for i, g in self.logs.items():
            if g.find(): shutil.copy(g.path, os.path.join(OUT, f"{self.name}-{os.path.basename(g.path)}"))

    def world(self, i):
        m = re.search(r"^world: (\S+)", agent(i, "status"), re.M)
        return m.group(1) if m else ""


def humans(slots_out):
    """Hero-team slots (team[0]) owned by a human PlayerController in a `slots` listing."""
    team0 = slots_out.split("team[1]")[0]
    return [l for l in team0.splitlines() if re.search(r"owner='[^']*'\(\w*PlayerController\w*\)", l)]


def has_hero(i):
    return "light=" in agent(i, "flashlight", "status")


def pick_card(cards, avoid=()):
    rows = [l.split()[-1] for l in cards.splitlines() if l.startswith("  ")]
    for pref in (r"^Burn_RollGun", r"^Burn_Roll", r"^Burn_(?!Team)"):
        for r in rows:
            if re.match(pref, r) and r not in avoid: return r
    return None


def duo(args):
    """The 2-instance regression: everything in --quick."""
    # addons_policy=cosmetic: the default (any, #43) lets every login in, so the announce/check path below needs a rule
    S = Session("duo", 2, ini_extra="addons_policy=cosmetic")
    S.prepare()   # golden profiles back first, so "before" is a known-good profile
    before = {i: load_profile(profile_path(i)) for i in (1, 2)}
    for i in (1, 2):
        if before[i]: shutil.copy(profile_path(i), os.path.join(OUT, f"profile{i}-before.json"))
    H, C = 1, 2
    sampler = SocketSampler(); sampler.start()
    try:
        c = Check("duo", "join (host sees 2 players, client connected)")
        ok = S.launch()
        st_h, st_c = agent(H, "status"), agent(C, "status")
        ok = ok and "client_conns=1" in st_h and "server_conn=yes" in st_c
        c.done(ok, re.sub(r"\s+", " ", " / ".join(re.findall(r"netdriver: .*", st_h + st_c))))
        if not ok: return
        agent(H, "steamnet"); agent(H, "netguard"); agent(C, "netguard")
        hl, cl = S.logs[H], S.logs[C]

        # defaults: the host's ini has no host= line; Steam-only presence; the client's login carries its protocol
        c = Check("duo", "defaults: host by default, presence steam: without addr:, protocol in the login")
        adv = wait_for(lambda: hl.grep(r'presence: advertising connect="([^"]*)"', since_mark=False), 30, 3) or []
        dflt = hl.grep(r"config: .* host=1 \(default\)", since_mark=False)
        login = hl.grep(r"admin: PreLogin options: .*\?b4bcoop=\d+\?b4bcoopver=", since_mark=False)
        ok = bool(dflt) and bool(adv) and "steam:" in adv[-1] and "proto:" in adv[-1] and "addr:" not in adv[-1]
        c.done(ok and bool(login), f"connect={adv[-1] if adv else '-'}; default host={bool(dflt)}; login={bool(login)}")

        # add-ons (#22, private since #35): the host announces addons_policy (presence addons:, and its first refusal
        # of the loopback join, which knows no presence); the client checks itself and logs in with only
        # ?b4bcoopaddonsok=<policy>. Nothing about the client's add-ons in the host's log; /addons players is gone.
        c = Check("duo", "add-ons: host announces its policy, client checks itself, nothing sent")
        pol = hl.grep(r"addons: policy for joiners: (\w+)", since_mark=False)
        asked = cl.grep(r"addons: host \S+ asks us to check our add-ons against its policy (\S+)", since_mark=False)
        ok_login = hl.grep(r"admin: PreLogin options: .*\?b4bcoopaddonsok=(\w+)", since_mark=False)
        checked = hl.grep(r"addons: login \S+: checked its add-ons against addons_policy=(\w+)", since_mark=False)
        leaks = hl.grep(r"b4bcoopaddons=[^-?\s'\"]", since_mark=False) + hl.grep(r"addons: login \S+: \d+c\d+g", since_mark=False)
        who = agent(H, "slash", "/addons players")
        pol_adv = bool(adv) and pol and f"addons:{pol[-1]}" in adv[-1]
        ok = bool(pol) and pol_adv and bool(asked) and bool(ok_login) and bool(checked) and not leaks and "gone" in who
        c.done(ok, f"policy={pol[-1] if pol else '-'}; presence addons:={pol_adv}; client asked={asked[-1] if asked else '-'}; "
                   f"login ok={ok_login[-1] if ok_login else '-'}; host checked={bool(checked)}; leaks={len(leaks)}; "
                   f"players={who.strip()[:60]}")

        c = Check("duo", "mission follow (client hero in Evansburgh_B)")
        agent(H, "mission", "Easy")
        ok = wait_for(lambda: MAP_B in S.world(H) and MAP_B in S.world(C) and has_hero(H) and has_hero(C), 240, 5)
        slots = agent(H, "slots")
        ok = ok and len(humans(slots)) == 2
        c.done(ok, f"{len(humans(slots))} human slot(s)")
        if not ok: return

        c = Check("duo", "client flashlight replicated to the host")
        agent(C, "flashlight", "on")
        def remote_light_on():
            lst = agent(H, "flashlight", "list")
            return [l for l in lst.splitlines() if "(mine)" not in l and "light=on" in l and "manual=yes" in l
                    and "PlayerController" in l]
        on = wait_for(remote_light_on, 20, 2)
        agent(C, "flashlight", "off")
        c.done(on, on[0].split(" volume_")[0] if on else "host never saw the client's light on")

        c = Check("duo", "burn cards: host and client play one each")
        hl.mark(); cl.mark()
        card_c = pick_card(agent(C, "burncard", "list"))
        card_h = pick_card(agent(H, "burncard", "list"), avoid=(card_c,))
        ok = bool(card_c and card_h)
        if ok:
            agent(C, "burncard", card_c)
            time.sleep(2)
            agent(H, "burncard", card_h)
            status = wait_for(lambda: (lambda s: s if s.count("played this map 1") >= 2 and s.count("key 'b4bcoop.burn.") >= 2
                                       else None)(agent(H, "burncard", "status")), 20, 2)
            ok = bool(status) and bool(hl.grep(rf"remote player played {card_c}")) and bool(hl.grep(rf"local player played {card_h}"))
        c.done(ok, f"host {card_h}, client {card_c}")

        agent(H, "ready")
        time.sleep(3)
        c = Check("duo", "burn cards: charged once each, client's forwarded")
        hl.mark(); cl.mark()
        agent(H, "burncard", "charge")
        got = wait_for(lambda: cl.grep(rf"CLIENT RPC.*consumable {card_c} by -1"), 20, 2)
        ok = (got and len(hl.grep(r"burncards: charging b4bcoop\.burn\.\d+ -> remote")) == 1
              and len(hl.grep(r"burncards: charging b4bcoop\.burn\.\d+ -> local")) == 1
              and len(hl.grep(r"rewards: forwarding AdjustConsumableQuantity \(-1\)")) == 1)
        c.done(ok, "host: 1 local + 1 remote charge, 1 forward; client: [CLIENT RPC] -1" if ok else "see logs")

        c = Check("duo", "chat: /players typed on the client gets a reply")
        c.done(*chat_players(S, C))

        c = Check("duo", "~ overlay: opens, draws, a Players control runs /players, Add-ons: list fetched by itself + Refresh, closes (host and client)")
        c.done(*overlay_smoke(S, (H, C)))

        c = Check("duo", "endmission success: client's SP forwarded")
        hl.mark(); cl.mark()
        agent(H, "endmission", "1")
        fwd = wait_for(lambda: hl.grep(r"rewards: forwarding AdjustSupplyPoints \((-?\d+)\)"), 30, 2)
        rpc = wait_for(lambda: cl.grep(r"\[CLIENT RPC\] adjusting SP by (-?\d+)"), 30, 2)
        n_fwd = int(fwd[0]) if fwd else None
        host_sp = host_applied(hl.text(), "SP")
        ok = fwd and rpc and len(fwd) == 1 and len(rpc) == 1 and int(rpc[0]) == n_fwd
        c.done(ok, f"forwarded {fwd}, client applied {rpc}, host own {host_sp}")
        stp_fwd = [int(x) for x in hl.grep(r"rewards: forwarding AdjustSkullTotemPoints \((-?\d+)\)")]
        host_stp = host_applied(hl.text(), "STP")
        others = sorted(set(hl.grep(r"rewards: not forwarded: command type (\d+)")))
        if others: log(f"  note: not forwarded command types {others}")

        c = Check("duo", "seamless chapter transition (both in Evansburgh_C)")
        hl.mark()
        time.sleep(8)
        agent(H, "ready", "vote")
        ok = wait_for(lambda: MAP_C in S.world(H) and MAP_C in S.world(C) and has_hero(H) and has_hero(C), 300, 5)
        seamless = hl.grep(r"HandleSeamlessTravelPlayer.*found previous slot")
        ok = ok and len(humans(agent(H, "slots"))) == 2
        c.done(ok and bool(seamless), f"{len(seamless)} 'found previous slot' on the host")

        c = Check("duo", "profiles after the deferred save: each charged/credited exactly once")
        exp_c = sp(before[C]) + (n_fwd or 0)
        wait_for(lambda: sp(load_profile(profile_path(C))) >= exp_c and consumable_spent(load_profile(profile_path(H)), card_h)
                 > consumable_spent(before[H], card_h), 90, 5)
        time.sleep(5)
        after = {i: load_profile(profile_path(i)) for i in (1, 2)}
        for i in (1, 2):
            if after[i]: shutil.copy(profile_path(i), os.path.join(OUT, f"profile{i}-after.json"))
            write_diff(before[i], after[i], f"profile{i}.diff")
        d = {
            "client SP": (sp(after[C]) - sp(before[C]), n_fwd),
            "host SP": (sp(after[H]) - sp(before[H]), host_sp),
            "client STP": (stp(after[C]) - stp(before[C]), sum(stp_fwd)),
            "host STP": (stp(after[H]) - stp(before[H]), host_stp),
            f"client {card_c}.spent": (consumable_spent(after[C], card_c) - consumable_spent(before[C], card_c), 1),
            f"host {card_h}.spent": (consumable_spent(after[H], card_h) - consumable_spent(before[H], card_h), 1),
            f"host {card_c}.spent": (consumable_spent(after[H], card_c) - consumable_spent(before[H], card_c), 0),
            f"client {card_h}.spent": (consumable_spent(after[C], card_h) - consumable_spent(before[C], card_h), 0),
        }
        bad = {k: v for k, v in d.items() if v[0] != v[1]}
        c.done(not bad and all(after.values()), "; ".join(f"{k} {v[0]:+d} (want {v[1]:+d})" for k, v in d.items()))
    finally:
        sampler.stop_ev.set(); sampler.join(10)
        agent(H, "netguard"); agent(C, "netguard")
        S.stop()
        c = Check("duo", "netguard: no public peer on any game socket (ss)")
        c.done(sampler.samples > 0 and not sampler.public,
               f"{sampler.samples} samples, {len(sampler.seen)} distinct sockets"
               + (f", {len(sampler.shop)} to GitHub for the add-on shop's list (expected)" if sampler.shop else "")
               + (": " + " | ".join(l for _, l in sampler.public[:3]) if sampler.public else ""))
        c = Check("duo", "nothing reachable from the network (game UDP on 127.0.0.1, ss)")
        c.done(sampler.samples > 0 and not sampler.exposed,
               f"{len(sampler.seen)} distinct sockets"
               + (f", {len(sampler.steam)} of Steam's native client off loopback (not ours, ss-samples.txt)"
                  if sampler.steam else "")
               + (": " + " | ".join(l for _, l in sampler.exposed[:3]) if sampler.exposed else ""))


def host_applied(text, what):
    """Sum of the host's own rewards: an 'adjusting SP|STP by N' line followed by the host's offline apply."""
    total, pending = 0, None
    cmd = "AdjustSupplyPoints" if what == "SP" else "AdjustSkullTotemPoints"
    for line in text.splitlines():
        m = re.search(rf"\]: adjusting {what} by (-?\d+)", line)
        if m: pending = int(m.group(1)); continue
        if pending is not None and f"ApplyCommandToOfflineData:{cmd}" in line:
            total += pending; pending = None
        elif pending is not None and "adjusting" in line:
            pending = None
    return total


def chat_players(S, n):
    """/players through the real chat box: key presses (`type`) to the focused game window. The window is raised
    first (wmctrl); without focus the input may be ignored, so a second try re-raises it, and as a last resort the
    chat box's own send (`chat /players`, same Say hook) is used, reported as such."""
    g = S.logs[n]
    for attempt, how in enumerate(("type", "type", "chat")):
        g.mark()
        wid = window_id(n)
        if wid: sh(["wmctrl", "-i", "-a", wid], timeout=10); time.sleep(1.5)
        agent(n, how, "/players")
        if wait_for(lambda: g.grep(r'chat: intercepted Say(Team)? "/players"'), 10, 1):
            lines = wait_for(lambda: g.grep(r"chat: \[local coop\] (#\d+ .*)"), 10, 1) or []
            ok = sum("(you)" in l for l in lines) == 1 and len(lines) >= 2
            via = "typed" if how == "type" else "chat box send (typing got no input)"
            return ok, f"{via}, attempt {attempt + 1}: {len(lines)} line(s): " + " | ".join(lines[:5])
        if how == "type": agent(n, "popup", "close")
    return False, "no 'chat: intercepted' after typing or chat-box send"


def overlay_smoke(S, insts):
    """The ~ window (dev `overlay` command): open, frames drawn over the game, the Players tab's list button runs the
    /players chat command (reply in the window's log), the Add-ons tab's Refresh gets the add-on shop's list again
    (it was fetched by itself on arrival in Fort Hope), close; the game keeps answering."""
    details = []
    for n in insts:
        agent(n, "overlay", "open")
        st = wait_for(lambda: (lambda s: s if re.search(r"renderer=ready .*drawn=[1-9]", s) else None)(agent(n, "overlay", "status")), 20, 1)
        if not st: return False, f"instance {n}: no frames drawn: {agent(n, 'overlay', 'status').strip()}"
        agent(n, "overlay", "tab", "Players")
        time.sleep(1)
        agent(n, "overlay", "press", "List in the log (/players)")
        lg = wait_for(lambda: (lambda t: t if re.search(r"^#\d+ .*\(you\)", t, re.M) else None)(agent(n, "overlay", "log")), 10, 1)
        screenshot(n, f"overlay-{n}.png")
        auto = wait_for(lambda: S.logs[n].grep(r"shop: in Fort Hope for the first time this game", False), 10, 1)
        g = S.logs[n]
        g.mark()
        agent(n, "overlay", "tab", "Add-ons")
        time.sleep(1)
        agent(n, "overlay", "press", "Refresh##shop")
        lk = auto and wait_for(lambda: g.grep(r"shop: getting the add-on list"), 15, 1)
        screenshot(n, f"overlay-addons-{n}.png")
        agent(n, "overlay", "close")
        st2 = agent(n, "overlay", "status")
        if not lg or not lk or "open=0" not in st2:
            return False, (f"instance {n}: log {'ok' if lg else 'missing /players reply'}, add-ons tab "
                           f"{'ok' if lk else 'no automatic list fetch or Refresh'}, {st2.strip()[:120]}")
        details.append(f"#{n} " + re.search(r"frames built=\d+ drawn=\d+", st2).group(0))
    return True, "; ".join(details)


def slotguard(args):
    """5 instances, vanilla 4 slots: the 5th is refused with "Server full." and the host keeps 4 humans."""
    S = Session("slotguard", 5, timeout=150)
    try:
        c = Check("slotguard", "5th joiner refused (Server full.), host fine")
        S.launch(wait=False)
        rej = wait_for(lambda: S.logs[1].grep(r"slotguard: login REJECTED.*") if S.logs[1].find() else None, 360, 5)
        full = wait_for(lambda: S.logs[5].grep(r"PendingConnectionFailure, Error: 'Server full.'") if S.logs[5].find() else None, 60, 3)
        time.sleep(5)
        who = agent(1, "who")
        n_h = len([l for l in who.splitlines() if l.startswith("#") and "[bot]" not in l])
        alive = "pong" in agent(1, "ping")
        c.done(rej and full and n_h == 4 and alive, f"{(rej or ['no REJECTED'])[0][:90]}; host humans={n_h} alive={alive}")
    finally:
        S.stop()


def five(args):
    """teamsize=5: 5 humans follow into the mission, each gets a hero, rewards forwarded to all 4 clients."""
    S = Session("five", 5, ini_extra="teamsize=5", timeout=480)
    try:
        c = Check("five", "teamsize=5: join 5")
        ok = S.launch()
        c.done(ok and "client_conns=4" in agent(1, "status"), "")
        if not ok: return
        c = Check("five", "teamsize=5: all 5 follow into Evansburgh_B with a hero")
        agent(1, "mission", "Easy")
        ok = wait_for(lambda: all(MAP_B in S.world(i) and has_hero(i) for i in range(1, 6)), 300, 5)
        slots = agent(1, "slots")
        c.done(ok and len(humans(slots)) == 5 and "TeamSize=5" in slots, f"{len(humans(slots))} human slot(s)")
        if not ok: return
        c = Check("five", "teamsize=5: endmission, SP forwarded to 4 clients")
        agent(1, "ready"); time.sleep(3)
        for g in S.logs.values(): g.mark()
        agent(1, "endmission", "1")
        fwd = wait_for(lambda: (lambda f: f if len(f) >= 4 else None)(S.logs[1].grep(r"rewards: forwarding AdjustSupplyPoints")), 40, 2)
        got = [i for i in range(2, 6) if wait_for(lambda: S.logs[i].grep(r"\[CLIENT RPC\] adjusting SP by"), 20, 2)]
        c.done(fwd and len(fwd) == 4 and got == [2, 3, 4, 5], f"{len(fwd or [])} forward(s), clients with RPC {got}")
        time.sleep(20)
        screenshot(1, "five-postround-1.png")
    finally:
        S.stop()


def main():
    global OUT, KEEP_PROFILES
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--quick", action="store_true", help="2-instance regression (default)")
    g.add_argument("--full", action="store_true", help="--quick + slot guard (5 vanilla) + teamsize=5 round")
    ap.add_argument("--no-lock", action="store_true", help="don't take launch/gamelock.sh (caller holds it)")
    ap.add_argument("--out", help="output directory (default /tmp/b4b-e2e-<time>)")
    ap.add_argument("--keep-profiles", action="store_true", help="don't restore the golden test profiles first")
    a = ap.parse_args()
    KEEP_PROFILES = a.keep_profiles
    OUT = a.out or f"/tmp/b4b-e2e-{'' if LANE == '1' else 'l' + LANE + '-'}{datetime.datetime.now():%Y%m%d-%H%M%S}"
    os.makedirs(OUT, exist_ok=True)
    lock = os.path.join(REPO, "launch/gamelock.sh")
    if not a.no_lock:
        log("waiting for the game lock (launch/gamelock.sh acquire e2e)")
        subprocess.run([lock, "acquire", "e2e"], check=True)
    dlls = [os.path.join(BIN, n) for n in ("X3DAudio1_7.dll", "dwmapi.dll") if os.path.exists(os.path.join(BIN, n))]
    log(f"lane {LANE} ({GAME}); output {OUT}; installed DLL " +
        (", ".join(f"{os.path.basename(d)} {time.ctime(os.path.getmtime(d))}" for d in dlls) or "MISSING"))
    try:
        duo(a)
        if a.full:
            slotguard(a)
            five(a)
    except KeyboardInterrupt:
        log("interrupted")
        RESULTS.append(("-", "interrupted", False, 0, ""))
    except Exception as e:  # keep the summary and the cleanup
        log(f"harness error: {e!r}")
        RESULTS.append(("-", f"harness error {e!r}", False, 0, ""))
    finally:
        sh([os.path.join(REPO, "launch/multi-stop.sh")], timeout=60)
        if not a.no_lock: subprocess.run([lock, "release", "e2e"])
    w = max(len(r[1]) for r in RESULTS) if RESULTS else 10
    lines = [f"{'session':10} {'check':{w}} result  time  detail"]
    for s, name, ok, dt, detail in RESULTS:
        lines.append(f"{s:10} {name:{w}} {'PASS' if ok else 'FAIL':6} {dt:4.0f}s  {detail[:160]}")
    failed = sum(not r[2] for r in RESULTS)
    lines.append(f"{len(RESULTS) - failed}/{len(RESULTS)} passed in {time.time() - T0:.0f}s; artifacts: {OUT}")
    summary = "\n".join(lines)
    print("\n" + summary)
    open(os.path.join(OUT, "summary.txt"), "w").write(summary + "\n")
    sys.exit(1 if failed or not RESULTS else 0)


if __name__ == "__main__":
    main()
