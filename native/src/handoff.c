// Host hand-off: when the host leaves (or names someone else), another player hosts and the run goes on.
// docs/investigations/host-handoff.md.
//
// Host: while it hosts players, every client gets a plan as silent data lines (CHAT_DATA_TYPE): who hosts next (the
// player named with /handoff, else the first other player in the list) and how to reach them (steam:<id64> for a
// Steam P2P peer; 127.0.0.1:<our port + 1> for local test copies over loopback: their listen port is set for them).
// In a mission it also sends the campaign run as the host's profile holds it: the record the war table's Continue
// resumes (map, difficulty, retries, and the run's JSON: every slot's owner, hero, copper, loadout and cards), again
// whenever that record changes (chapter ends).
// Client: on `handoff go` (the host named a new host) or when the connection to the host ends without a kick or our
// own /leave (the host quit or crashed), the next host stops joining, loads its own Fort Hope as a listen server and,
// if the session was in a mission, writes the run into its own profile (the game's own offline upsert) and starts it
// the way the war table's Continue does (Matchmaking::JoinRun with the run id): the current chapter from its start,
// every player with what the run saved for their slot. Everyone else joins the new host and follows.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ue.h"
#include "log.h"
#include "cmds.h"
#include "overlay.h"
#include "MinHook.h"

#define ADDR_PROFILE_SETTINGS VA(0x141BC6930ull)  // UPlayerProfileSettings* (PPC*): the local player's profile save object
#define ADDR_UPSERT_RUN       VA(0x141BC1780ull)  // bool PPC::UpsertOfflineCampaignRun(PPC*, int32 Id, const FString& Map,
                                                  //   const FString& NextMapToUnlock, uint8 Difficulty, int32 Retries,
                                                  //   uint8 State, const FString& Data, int32 DataVersion): logs
                                                  //   "updating|creating campaign run %i [OFFLINE]", then SaveGame
#define ADDR_JOINRUN          VA(0x141AE9240ull)  // void UMatchmaking::JoinRun(this, const FString& Map, int32 RunId,
                                                  //   FString OwnerId (by value, callee frees), uint8 Difficulty, uint8 Pool,
                                                  //   uint8, uint8 bTraining, uint8* Voice): the war table's start/Continue
#define ADDR_RESIZE           VA(0x140BAF260ull)  // TArray<TCHAR>::ResizeForCopy (GMalloc storage)
#define ADDR_URL_PORT         VA(0x146972E60ull)  // FUrlConfig::DefaultPort (-Port= / [URL] Port): the listen port of a
                                                  //   map opened with ?listen
#define ADDR_MAX_RUNS         VA(0x1463FA838ull)  // cvar PlayerProfileManager.MaxCampaignRuns (16): the upsert refuses a
                                                  //   new run at the limit
static const uint8_t SIG_SETTINGS[] = {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x99,0xd8,0x00,0x00,0x00,0x48,0x85,0xdb,0x74,0x52};
static const uint8_t SIG_UPSERT[] = {0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8b,0xec,0x48,0x83,0xec,0x78};
static const uint8_t SIG_JOINRUN[] = {0x44,0x89,0x44,0x24,0x18,0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x41,0x54,0x41,0x55,0x41,0x56,0x48,0x8d,0xac,0x24,0xf0,0xfe};
static const uint8_t SIG_RESIZE[] = {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x20,0x48,0x63,0xda,0x48,0x8b,0xf9};

#define ADDR_REQUEST_EXIT     VA(0x1424CBAE0ull)  // void FWindowsPlatformMisc::RequestExit(bool Force): `quit`, the game's Quit,
                                                  //   closing the window; logs "FPlatformMisc::RequestExit(%i)"
static const uint8_t SIG_REQUEST_EXIT[] = {0x40,0x53,0x48,0x83,0xec,0x40,0x0f,0xb6,0x05};
typedef void (*RequestExitFn)(uint8_t force);
static RequestExitFn orig_request_exit;

typedef void *(*SettingsFn)(UObject *ppc);
typedef uint8_t (*UpsertFn)(UObject *ppc, int32_t id, const FString *map, const FString *next, uint8_t diff,
                            int32_t retries, uint8_t state, const FString *data, int32_t dver);
typedef void (*JoinRunFn)(UObject *mm, const FString *map, int32_t run, FString *owner, uint8_t diff, uint8_t pool,
                          uint8_t a7, uint8_t training, uint8_t *voice);
typedef void (*ResizeFn)(FString *s, int32_t new_max, int32_t prev_max);
static int funcs_ok;

// FPlayerProfileCampaignRun (0x60), one value of FPlayerProfileData.CampaignRuns (TMap<int32, run>, +0x178 in the
// offline data at UPlayerProfileSettings +0xE0; set elements 0x70: key +0, value +8, hash next +0x68)
typedef struct {
    int32_t id, pad0;
    FString map, next;
    uint8_t diff, pad1[3];
    int32_t retries, version;
    uint8_t state, pad2[3];
    FString data;
    int64_t ts;
    int32_t dver, tu, cl, pad3;
} RunRec;
_Static_assert(sizeof(RunRec) == 0x60, "FPlayerProfileCampaignRun");
_Static_assert(__builtin_offsetof(RunRec, data) == 0x38 && __builtin_offsetof(RunRec, ts) == 0x48, "run layout");
#define RUNS_OFF (0xE0 + 0x178)
#define CRM_RUN_ID 0x160   // UCampaignRunManager: the run JoinRun set (int32), owner FString at +0x168

static const char *DIFFS[] = {"Easy", "Normal", "Hard", "VeryHard"};

static int enabled = 1;   // ini host_handoff=0: no automatic take-over when the host is gone
int handoff_live(const char *key, const char *val) {
    if (strcmp(key, "host_handoff")) return 0;
    enabled = !val || atoi(val) != 0;
    LOG("handoff: automatic take-over %s", enabled ? "on" : "off");
    return 1;
}

static double clock_s;
static int handed_over_for_exit;   // host: we quit and told everyone to go on (handoff go quit)
static const char *pend_why;       // client: a hand-off to start on the next tick (handoff_request)

// ---- small helpers ----
static void w2a(const FString *s, char *buf, size_t n) {
    size_t k = 0;
    for (int i = 0; s && s->data && i < s->num && s->data[i] && k + 1 < n; i++) buf[k++] = s->data[i] < 128 ? (char)s->data[i] : '?';
    buf[k] = 0;
}
static int is_pc(UObject *o) { static UClass *c; if (!c) c = ue_find_class("PlayerController"); return o && c && ue_is_a(o, c); }
static int connected(void) {
    UObject *w = ue_world(), *nd = w ? ue_get_ptr(w, "NetDriver") : NULL;
    return nd && ue_get_ptr(nd, "ServerConnection");
}
static int in_camp(void) {
    UObject *w = ue_world();
    char pkg[300];
    return w && strstr(ue_world_package(w, pkg, sizeof pkg), "FortHope");
}
static int in_mission(char *pkg, size_t n) {
    UObject *w = ue_world();
    if (!w) return 0;
    ue_world_package(w, pkg, n);
    return strstr(pkg, "/Missions/") && !strstr(pkg, "FortHope");   // Fort Hope is under /Missions/ too
}
static UObject *local_ppc(void) { UObject *pc = ue_local_pc(); return pc ? ue_get_ptr(pc, "GobiPlayerProfileComponent") : NULL; }
static void *local_settings(void) {
    UObject *ppc = funcs_ok ? local_ppc() : NULL;
    return ppc ? ((SettingsFn)ADDR_PROFILE_SETTINGS)(ppc) : NULL;
}
static UObject *run_manager(void) {
    UObject *w = ue_world(), *gi = w ? ue_get_ptr(w, "OwningGameInstance") : NULL;
    return gi ? ue_get_ptr(gi, "CampaignRunManager") : NULL;
}
static UObject *find_live(const char *cls) {
    UClass *c = ue_find_class(cls);
    for (int32_t i = 0, n = ue_num_objects(); c && i < n; i++) {
        UObject *o = ue_object_at(i);
        if (o && !(U_FLAGS(o) & 0x30) && ue_is_a(o, c)) return o;
    }
    return NULL;
}

// The local profile's campaign runs (hash walk of the TMap; no allocation)
static int runs_list(void *settings, RunRec **out, int max) {
    if (!settings) return 0;
    char *m = (char *)settings + RUNS_OFF;
    uint8_t *data = *(uint8_t **)m;
    int32_t num = *(int32_t *)(m + 8), hs = *(int32_t *)(m + 0x48);
    int32_t *hash = *(int32_t **)(m + 0x40);
    if (!hash) hash = (int32_t *)(m + 0x38);
    if (!data || num <= 0 || hs <= 0 || hs > 65536) return 0;
    int n = 0;
    for (int b = 0; b < hs && n < max; b++)
        for (int32_t idx = hash[b], guard = 0; idx >= 0 && idx < num && n < max && guard < 4096; guard++) {
            uint8_t *e = data + (size_t)idx * 0x70;
            out[n++] = (RunRec *)(e + 8);
            idx = *(int32_t *)(e + 0x68);
        }
    return n;
}
static RunRec *run_by_id(int32_t id) {
    RunRec *r[64];
    int n = runs_list(local_settings(), r, 64);
    for (int i = 0; i < n; i++) if (r[i]->id == id) return r[i];
    return NULL;
}
// Host: the run being played. A resumed run's id is in the run manager (JoinRun puts it there); a new run has 0 there
// until the end and gets its record ("creating campaign run") at the end of its first chapter: the newest in-progress
// record written since we left the camp (run_base, unix seconds). Before that there is no record (NULL).
static int64_t run_base;
static RunRec *current_run(void) {
    UObject *crm = run_manager();
    int32_t id = crm ? *(int32_t *)((char *)crm + CRM_RUN_ID) : 0;
    if (id > 0) { RunRec *r = run_by_id(id); if (r) return r; }
    RunRec *r[64], *best = NULL;
    int n = runs_list(local_settings(), r, 64);
    for (int i = 0; i < n; i++)
        if (r[i]->state == 0 && run_base > 0 && r[i]->ts >= run_base - 2 && (!best || r[i]->ts > best->ts)) best = r[i];
    return best;
}
static int mission_difficulty(void) {
    UObject *w = ue_world(), *gs = w ? ue_get_ptr(w, "GameState") : NULL;
    int32_t off = gs ? ue_prop_offset(gs, "MissionDifficulty") : -1;
    return off >= 0 ? *((uint8_t *)gs + off) : 0;
}

static uint32_t fnv_w(const wchar_t *s, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (uint16_t)s[i]; h *= 16777619u; }
    return h;
}
static uint32_t fnv_a(const char *s) { uint32_t h = 2166136261u; for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; } return h; }

// ---- host side ----
typedef struct { UObject *pc; int32_t idx; uint32_t plan, map; int32_t run_id, run_dver; int64_t run_ts; } Sent;
static Sent sent[16];
static char designated[80];        // player key named with /handoff (steam:<id64> or name:<name>), "" = the rule
static int designated_idx = -1;    // its PlayerArray index when named (local test copies share one key)
static int go_pending;             // /handoff: send `go` once the plans are out
static double old_host_join_at;    // after `go`: when we (the old host) join the new host
static char old_host_target[120];
static int run_seq;

static int my_listen_port(void) {
    int l = steamnet_listen_port();
    return l > 0 ? l : *(volatile int *)ADDR_URL_PORT;
}

static int ps_steam_id(UObject *ps, uint64_t *id) {
    char key[80];
    admin_ps_key(ps, key, sizeof key);
    if (strncmp(key, "steam:", 6)) return 0;
    *id = strtoull(key + 6, NULL, 10);
    return *id != 0;
}

// The remote human players in PlayerArray order (their PlayerStates; pcs = their controllers)
static int humans(UObject **ps_out, UObject **pc_out, int *idx_out, int max) {
    UObject **pa;
    int n = admin_player_array(&pa), k = 0;
    UObject *me = ue_local_pc();
    for (int i = 0; i < n && k < max; i++) {
        UObject *pc = ue_get_ptr(pa[i], "Owner");
        if (!is_pc(pc) || pc == me) continue;
        ps_out[k] = pa[i]; pc_out[k] = pc; idx_out[k] = i; k++;
    }
    return k;
}

// How the others reach this player once it hosts: steam:<id64> over Steam P2P; over loopback (local test copies,
// host_ip=0 allows nothing else) 127.0.0.1 and a port we hand it. With host_ip=1 to another machine we know neither its
// address nor its forwarded port: no hand-off to that player.
static int successor_addr(UObject *ps, char *addr, size_t n, int *port) {
    uint64_t id = 0;
    *port = 0;
    if (ps_steam_id(ps, &id) && id != steamnet_local_id() && steamnet_peer_known(id)) { snprintf(addr, n, "steam:%llu", (unsigned long long)id); return 1; }
    if (coop_host_ip()) { addr[0] = 0; return 0; }
    *port = my_listen_port() + 1;
    snprintf(addr, n, "127.0.0.1:%d", *port);
    return 1;
}

static int pick_successor(UObject **ps, int nh, int *idxs) {
    if (designated[0]) {
        char key[80];
        for (int i = 0; i < nh; i++) {
            admin_ps_key(ps[i], key, sizeof key);
            if (!strcmp(key, designated) && (designated_idx < 0 || idxs[i] == designated_idx)) return i;
        }
        for (int i = 0; i < nh; i++) { admin_ps_key(ps[i], key, sizeof key); if (!strcmp(key, designated)) return i; }
    }
    return nh ? 0 : -1;
}

static Sent *sent_for(UObject *pc) {
    Sent *freeslot = NULL;
    for (int i = 0; i < 16; i++) {
        if (sent[i].pc && ue_object_at(sent[i].idx) != sent[i].pc) memset(&sent[i], 0, sizeof sent[i]);
        if (sent[i].pc == pc) return &sent[i];
        if (!sent[i].pc && !freeslot) freeslot = &sent[i];
    }
    if (!freeslot) return NULL;
    memset(freeslot, 0, sizeof *freeslot);
    freeslot->pc = pc; freeslot->idx = U_INDEX(pc); freeslot->run_id = -1; freeslot->run_ts = -1;
    return freeslot;
}

// Map path for the record: the current mission when the record resumes there, else the same folder's MAP_PERS_<map>
static void record_map_path(const RunRec *r, const char *world, char *out, size_t n) {
    char m[128];
    w2a(&r->map, m, sizeof m);
    size_t wl = strlen(world), ml = strlen(m);
    if (ml && wl > ml && !_stricmp(world + wl - ml, m) && world[wl - ml - 1] == '_') { snprintf(out, n, "%s", world); return; }
    const char *slash = strrchr(world, '/');
    snprintf(out, n, "%.*s/MAP_PERS_%s", slash ? (int)(slash - world) : 0, world, m);
}

static int send_run(UObject *pc, const RunRec *r, const char *mappath) {
    char m[128], nx[128], line[700];
    w2a(&r->map, m, sizeof m); w2a(&r->next, nx, sizeof nx);
    int len = r->data.num > 0 ? r->data.num - 1 : 0;
    ++run_seq;
    snprintf(line, sizeof line, "handoff run %d %d %u %d %u %d %d %s %s %s", run_seq, r->id, r->diff, r->retries, r->state,
             r->dver, len, m[0] ? m : "-", nx[0] ? nx : "-", mappath);
    if (admin_data_to(pc, line)) return -1;
    enum { CHUNK = 3000 };
    static wchar_t buf[CHUNK + 64];
    for (int off = 0; off < len; off += CHUNK) {
        int k = len - off < CHUNK ? len - off : CHUNK;
        int h = swprintf(buf, 64, L"handoff chunk %d %d ", run_seq, off);
        memcpy(buf + h, r->data.data + off, k * sizeof(wchar_t));
        buf[h + k] = 0;
        if (admin_data_to_w(pc, buf, h + k)) return -1;
    }
    snprintf(line, sizeof line, "handoff end %d %08x", run_seq, fnv_w(r->data.data, len));
    return admin_data_to(pc, line);
}

static void host_tick(void) {
    UObject *w = ue_world();
    if (!w || !ue_is_listen_server(w) || ue_num_clients(w) == 0) { memset(sent, 0, sizeof sent); return; }
    if (old_host_join_at > 0 || handed_over_for_exit) return;   // handed over: nothing more to plan
    UObject *ps[16], *pcs[16];
    int idxs[16], nh = humans(ps, pcs, idxs, 16);
    if (!nh) return;
    int s = pick_successor(ps, nh, idxs), port = 0;
    char addr[120], name[64], line[400], pkg[300], mappath[400] = "-";
    if (!successor_addr(ps[s], addr, sizeof addr, &port)) {
        static double told;
        if (clock_s - told > 60) { told = clock_s; LOG("handoff: no address for the next host (host_ip=1 to another machine): no hand-off"); }
        return;
    }
    admin_ps_name(ps[s], name, sizeof name);
    int mission = in_mission(pkg, sizeof pkg);
    RunRec *run = mission ? current_run() : NULL;
    if (run && run->state != 0) run = NULL;   // completed / failed: nothing to continue
    if (run) record_map_path(run, pkg, mappath, sizeof mappath);
    for (int i = 0; i < nh; i++) {
        Sent *st = sent_for(pcs[i]);
        if (!st) continue;
        if (i == s) snprintf(line, sizeof line, "handoff plan host %d %d %s", port, nh, name);
        else snprintf(line, sizeof line, "handoff plan join %s %d %s", addr, nh, name);
        uint32_t h = fnv_a(line);
        if (st->plan != h && !admin_data_to(pcs[i], line)) { st->plan = h; LOG("handoff: to #%d: %s", idxs[i], line); }
        if (run && (st->run_id != run->id || st->run_ts != run->ts || st->run_dver != run->dver)) {
            if (!send_run(pcs[i], run, mappath)) {
                st->run_id = run->id; st->run_ts = run->ts; st->run_dver = run->dver;
                LOG("handoff: run %d (%d chars, resumes at %s) sent to #%d", run->id, run->data.num, mappath, idxs[i]);
            }
        } else if (!run && mission && (st->run_id != 0 || st->run_ts != -2 || st->map != fnv_a(pkg))) {
            // nothing saved for this run (its first chapter, or the game refused to save it: a profile at the 16-run
            // limit): the next host starts a new run on the current map
            snprintf(line, sizeof line, "handoff newrun %d %s", mission_difficulty(), pkg);
            if (!admin_data_to(pcs[i], line)) { st->run_id = 0; st->run_ts = -2; st->map = fnv_a(pkg); LOG("handoff: to #%d: %s", idxs[i], line); }
        } else if (!run && in_camp() && (st->run_id != 0 || st->run_ts != 0)) {   // (not on a transition map)
            if (!admin_data_to(pcs[i], "handoff run none")) { st->run_id = 0; st->run_ts = 0; }
        }
    }
    if (go_pending) {
        go_pending = 0;
        for (int i = 0; i < nh; i++) admin_data_to(pcs[i], "handoff go");
        snprintf(old_host_target, sizeof old_host_target, "%s", addr);
        old_host_join_at = clock_s + 3;
        LOG("handoff: go: %s hosts next at %s%s; we join them in 3 s", name, addr, run ? " (run continues)" : "");
        chat_local("%s hosts the session now; you join them in a moment.", name);
    }
}

// The host quits (`quit`, the game's Quit, closing the window): its connections close without the clients noticing
// (they time out after 90 s, cmds.c's ConnectionTimeout). So tell them first: `handoff go` to everyone, and the exit
// itself a moment later, once the net driver has sent it.
static DWORD game_tid;
static double exit_at;
static uint8_t exit_force;
static void request_exit_detour(uint8_t force) {
    UObject *w = game_tid && GetCurrentThreadId() == game_tid ? ue_world() : NULL;
    if (!handed_over_for_exit && w && ue_is_listen_server(w) && ue_num_clients(w) > 0 && funcs_ok) {
        UObject *ps[16], *pcs[16];
        int idxs[16], nh = humans(ps, pcs, idxs, 16), sent_go = 0;
        handed_over_for_exit = 1;
        for (int i = 0; i < nh; i++) {
            Sent *st = sent_for(pcs[i]);
            if (st && st->plan && !admin_data_to(pcs[i], "handoff go quit")) sent_go++;   // only to players who have the plan
        }
        if (sent_go) {
            LOG("handoff: quitting: told %d player(s) to go on without us; exiting in 1 s", sent_go);
            exit_at = clock_s + 1.0; exit_force = force;
            return;
        }
    }
    orig_request_exit(force);
}

// ---- client side ----
static struct { int role; int port, n; char addr[120], name[64]; } plan;   // role 1 host, 2 join
typedef struct { int seq, id, diff, retries, state, dver, len, got; char map[128], next[128], path[400]; wchar_t *data; int ok; } RunIn;
static RunIn rin, rok;   // being received / last complete
static int was_connected;

enum { X_IDLE, X_HOST_CAMP, X_HOST_WAIT, X_JOIN };
static int xstate;
static double x_since, x_listen_at;
static int x_resume, x_expected;   // continue the run; players to wait for before that

static void run_free(RunIn *r) { free(r->data); memset(r, 0, sizeof *r); }

void handoff_host_data(const wchar_t *s, int n) {   // chat.c: a data line starting with "handoff "
    char head[600];
    int k = 0;
    for (; k < n && k < (int)sizeof head - 1 && s[k]; k++) head[k] = s[k] < 128 ? (char)s[k] : '?';
    head[k] = 0;
    if (!strncmp(head, "handoff chunk ", 14)) {
        int seq, off, used = 0;
        // exactly one space after the offset: the chunk itself may start with one
        if (sscanf(head + 14, "%d %d%n", &seq, &off, &used) < 2 || seq != rin.seq || !rin.data || head[14 + used] != ' ') return;
        int start = 14 + used + 1, cnt = n - start;
        while (cnt > 0 && !s[start + cnt - 1]) cnt--;
        if (off < 0 || off + cnt > rin.len) { LOG("handoff: bad chunk %d+%d of %d", off, cnt, rin.len); return; }
        memcpy(rin.data + off, s + start, cnt * sizeof(wchar_t));
        rin.got += cnt;
        return;
    }
    LOG("handoff: from the host: %s", head);
    char a[200], b[200];
    if (!strncmp(head, "handoff plan ", 13)) {
        if (pend_why || xstate != X_IDLE) return;   // already on our way: the old host's later plans don't count
        int port = 0, nn = 0, used = 0;
        memset(&plan, 0, sizeof plan);
        if (sscanf(head + 13, "host %d %d %n", &port, &nn, &used) >= 2) { plan.role = 1; plan.port = port; plan.n = nn; snprintf(plan.name, sizeof plan.name, "%s", head + 13 + used); }
        else if (sscanf(head + 13, "join %119s %d %n", a, &nn, &used) >= 2) { plan.role = 2; snprintf(plan.addr, sizeof plan.addr, "%s", a); plan.n = nn; snprintf(plan.name, sizeof plan.name, "%s", head + 13 + used); }
    } else if (!strcmp(head, "handoff run none")) {
        run_free(&rok);
    } else if (!strncmp(head, "handoff newrun ", 15)) {
        int d;
        char path[400];
        if (sscanf(head + 15, "%d %399s", &d, path) != 2) return;
        run_free(&rok);
        rok.diff = d; snprintf(rok.path, sizeof rok.path, "%s", path); rok.ok = 1;   // id 0: a new run
    } else if (!strncmp(head, "handoff run ", 12)) {
        RunIn r = {0};
        char path[400];
        if (sscanf(head + 12, "%d %d %d %d %d %d %d %127s %127s %399s", &r.seq, &r.id, &r.diff, &r.retries, &r.state, &r.dver,
                   &r.len, a, b, path) != 10 || r.len < 0 || r.len > (4 << 20)) return;
        snprintf(r.map, sizeof r.map, "%s", a); snprintf(r.next, sizeof r.next, "%s", b); snprintf(r.path, sizeof r.path, "%s", path);
        r.data = calloc(r.len + 1, sizeof(wchar_t));
        if (!r.data) return;
        run_free(&rin);
        rin = r;
    } else if (!strncmp(head, "handoff end ", 12)) {
        int seq; unsigned h;
        if (sscanf(head + 12, "%d %x", &seq, &h) != 2 || seq != rin.seq || !rin.data) return;
        if (rin.got != rin.len || fnv_w(rin.data, rin.len) != h) { LOG("handoff: run %d incomplete (%d of %d chars)", rin.id, rin.got, rin.len); run_free(&rin); return; }
        run_free(&rok);
        rok = rin; rok.ok = 1;
        memset(&rin, 0, sizeof rin);
        LOG("handoff: run %d received (%d chars, %s %s, resumes at %s)", rok.id, rok.len, rok.map, DIFFS[rok.diff & 3], rok.path);
    } else if (!strcmp(head, "handoff go")) {
        handoff_request("the host handed the session over", 1);
    } else if (!strcmp(head, "handoff go quit")) {
        handoff_request("the host quit and handed the session over", 0);
    }
}

// Act on the plan: we host next, or we join whoever does. Requested from the network receive path or a log line (the
// `go`, a lost connection), run on the next tick (handoff_tick); in a mission at the time of the request = continue.
static int pend_resume, pend_old_host;
void handoff_request(const char *why, int old_host_joins) {
    if (pend_why || xstate != X_IDLE || !plan.role) return;
    char pkg[300];
    pend_why = why;
    pend_resume = rok.ok && in_mission(pkg, sizeof pkg);
    pend_old_host = old_host_joins;
}
static void execute(const char *why) {
    if (xstate != X_IDLE || !plan.role) return;
    x_resume = pend_resume;
    x_since = clock_s;
    // the others: everyone the old host had but us, and the old host itself when it handed over (it joins us too)
    x_expected = plan.n - 1 + pend_old_host;
    LOG("handoff: %s: %s%s", why, plan.role == 1 ? "we host next" : "joining ", plan.role == 1 ? (x_resume ? " and continue the run" : "") : plan.addr);
    travel_end_follow("host hand-off");   // no more rejoins of the old host
    if (plan.role == 1) {
        if (plan.port > 0) {   // loopback copies: the port the others were told (listen ports come from FUrlConfig)
            int *p = (int *)ADDR_URL_PORT;
            LOG("handoff: listen port %d -> %d", *p, plan.port);
            *p = plan.port;
        }
        cmds_become_host();          // no join targets; host our camp (auto-host) from now on
        travel_set_host("");         // no rejoin of the old host
        if (connected()) game_exec("disconnect");
        xstate = X_HOST_CAMP;
        chat_local_soon(x_resume ? "You are the host now: the others join you, then the run continues from the start of this chapter."
                                 : "You are the host now: the others are joining you.");
    } else {
        travel_set_host("");
        cmds_set_session_join(plan.addr);   // tried once we are back in our camp (the old host leaving takes us there)
        xstate = X_JOIN;
        char msg[200];
        snprintf(msg, sizeof msg, "%s hosts the session now: joining them%s.", plan.name, x_resume ? " (the run continues)" : "");
        chat_local_soon(msg);
    }
}

// travel.c: the connection to our host ended (a `?closed`/`?failed` travel, not our own /leave or a kick)
void handoff_host_lost(const char *url) {
    if (xstate != X_IDLE || ue_is_listen_server(ue_world())) return;   // a host's own client connections time out too
    if (!plan.role || !was_connected) { LOG("handoff: host gone (%s), no plan", url); return; }
    if (!enabled) { LOG("handoff: host gone, take-over off (host_handoff=0)"); return; }
    handoff_request("the host is gone", 0);
}

static int resume_run(void) {
    void *settings = local_settings();
    UObject *ppc = local_ppc(), *mm = find_live("Matchmaking");
    if (!settings || !ppc || !mm) { LOG("handoff: resume: no %s", !settings ? "profile" : !ppc ? "profile component" : "Matchmaking"); return -1; }
    if (!rok.id) {   // nothing saved yet (first chapter): a new run on the same map, as the war table starts one
        static wchar_t wp0[400];
        FString path0 = {wp0, 0, 400}, none = {0};
        int k = 0;
        for (; k < 399 && rok.path[k]; k++) wp0[k] = rok.path[k];
        wp0[k] = 0; path0.num = k + 1;
        uint8_t voice0 = 0;
        LOG("handoff: new run: JoinRun(%s, %s)", rok.path, DIFFS[rok.diff & 3]);
        ((JoinRunFn)ADDR_JOINRUN)(mm, &path0, 0, &none, (uint8_t)rok.diff, 0, 0, 0, &voice0);
        return 0;
    }
    // the run into our profile: the game's own offline upsert (logs "creating campaign run %i [OFFLINE]", saves)
    static wchar_t wm[128], wn[128];
    FString map = {wm, 0, 128}, next = {wn, 0, 128}, data = {rok.data, rok.len + 1, rok.len + 1};
    for (int i = 0; i < 127 && (wm[i] = rok.map[i]); i++) map.num = i + 2;
    for (int i = 0; i < 127 && (wn[i] = rok.next[i]); i++) next.num = i + 2;
    RunRec *have[64];
    int nr = runs_list(settings, have, 64), exists = 0;
    for (int i = 0; i < nr; i++) if (have[i]->id == rok.id) exists = 1;
    int *maxp = (int *)ADDR_MAX_RUNS, max0 = *maxp;
    if (!exists && nr >= max0) { *maxp = nr + 1; LOG("handoff: profile has %d runs (limit %d): one more for this run", nr, max0); }
    uint8_t ok = ((UpsertFn)ADDR_UPSERT_RUN)(ppc, rok.id, &map, &next, (uint8_t)rok.diff, rok.retries, 0, &data, rok.dver);
    *maxp = max0;
    if (!ok) { LOG("handoff: resume: the profile refused run %d", rok.id); return -1; }
    // JoinRun as the war table's Continue: our profile's run, owner = our HydraPublicId (+0x1A8, unreflected)
    FString *hid = (FString *)((char *)ppc + 0x1A8);
    FString owner = {0};
    int on = hid->num > 0 ? hid->num : 1;
    ((ResizeFn)ADDR_RESIZE)(&owner, on, 0);
    if (!owner.data) return -1;
    for (int i = 0; i < on; i++) owner.data[i] = hid->data && i < hid->num ? hid->data[i] : 0;
    owner.num = on;
    static wchar_t wp[400];
    FString path = {wp, 0, 400};
    int i = 0;
    for (; i < 399 && rok.path[i]; i++) wp[i] = rok.path[i];
    wp[i] = 0; path.num = i + 1;
    uint8_t voice = 0;
    char ow[96]; w2a(hid, ow, sizeof ow);
    LOG("handoff: continuing run %d: JoinRun(%s, %s, owner %s)", rok.id, rok.path, DIFFS[rok.diff & 3], ow);
    ((JoinRunFn)ADDR_JOINRUN)(mm, &path, rok.id, &owner, (uint8_t)rok.diff, 0, 0, 0, &voice);
    return 0;
}

static void client_tick(void) {
    if (connected()) {
        was_connected = 1;
        if (xstate == X_JOIN && clock_s - x_since > 5) { LOG("handoff: joined the new host"); xstate = X_IDLE; memset(&plan, 0, sizeof plan); }
        return;
    }
    UObject *w = ue_world();
    if (xstate == X_HOST_CAMP) {
        if (!in_camp() || !ue_local_pc() || signin_pending() || signin_on_title()) return;
        if (!ue_is_listen_server(w)) { if (clock_s - x_since > 120) { LOG("handoff: never hosted, giving up"); xstate = X_IDLE; } return; }
        LOG("handoff: hosting our camp (%d client(s) so far)", ue_num_clients(w));
        memset(&plan, 0, sizeof plan);
        was_connected = 0;
        if (!x_resume) { xstate = X_IDLE; return; }
        xstate = X_HOST_WAIT; x_listen_at = clock_s;
    } else if (xstate == X_HOST_WAIT) {
        // continue once the others are here (they follow the mission start), at most 60 s
        UObject *ps[16], *pcs[16];
        int idxs[16], expected = x_expected, have = humans(ps, pcs, idxs, 16);   // logged in (a player state), not pending
        static int last = -1;
        static double changed;
        if (have != last) { last = have; changed = clock_s; LOG("handoff: %d of %d player(s) back", have, expected); }
        if (have < expected && clock_s - x_listen_at < 60) return;
        if (clock_s - changed < 5) return;   // let the last one finish loading the camp
        xstate = X_IDLE;
        if (resume_run()) chat_local("Could not continue the run here; start a new one at the war table.");
    } else if (xstate == X_JOIN) {
        if (clock_s - x_since > 200) { xstate = X_IDLE; memset(&plan, 0, sizeof plan); }
    }
}

void handoff_tick(float dt) {
    clock_s += dt;
    game_tid = GetCurrentThreadId();
    if (exit_at > 0 && clock_s >= exit_at) { exit_at = 0; LOG("handoff: exiting now"); orig_request_exit(exit_force); }
    if (pend_why) { const char *w = pend_why; execute(w); pend_why = NULL; }
    static double next;
    if (old_host_join_at > 0 && clock_s >= old_host_join_at) {
        old_host_join_at = 0;
        designated[0] = 0; designated_idx = -1;
        // from our own camp (the join loop retries there until the new host listens); from a mission the first failed
        // attempt would leave us alone in it
        LOG("handoff: the old host joins %s", old_host_target);
        cmds_set_session_join(old_host_target);
        game_exec("disconnect");
    }
    client_tick();
    if (clock_s < next) return;
    next = clock_s + 1;
    if (connected() || !funcs_ok) return;
    char pkg[300];   // our own run: since we left the camp (kept over the transition map between chapters)
    if (in_camp()) run_base = 0;
    else if (in_mission(pkg, sizeof pkg) && !run_base) run_base = (int64_t)time(NULL);
    host_tick();
}

// ---- commands ----
static void status(Out *o) {
    if (!admin_is_client() && ue_is_listen_server(ue_world())) {
        UObject *ps[16], *pcs[16];
        int idxs[16], nh = humans(ps, pcs, idxs, 16), port;
        if (!nh) { out_printf(o, "handoff: nobody else is here\n"); return; }
        int s = pick_successor(ps, nh, idxs);
        char name[64], addr[120];
        admin_ps_name(ps[s], name, sizeof name);
        int ok = successor_addr(ps[s], addr, sizeof addr, &port);
        out_printf(o, "if you leave, %s (#%d) hosts next%s%s%s\n", name, idxs[s], designated[0] ? " (named by you)" : " (first in the list)",
                   ok ? " at " : " -- but they can't be reached (host_ip=1)", ok ? addr : "");
        return;
    }
    if (!plan.role) out_printf(o, "handoff: no plan from the host\n");
    else if (plan.role == 1) out_printf(o, "handoff: you host next (%d player(s))%s\n", plan.n, enabled ? "" : " -- take-over off (host_handoff=0)");
    else out_printf(o, "handoff: %s hosts next (%s)\n", plan.name, plan.addr);
    if (rok.ok && rok.id) out_printf(o, "run %d: %s %s, %d retries, %d chars, resumes at %s\n", rok.id, rok.map, DIFFS[rok.diff & 3], rok.retries, rok.len, rok.path);
    else if (rok.ok) out_printf(o, "first chapter of a new run: a new run at %s (%s)\n", rok.path, DIFFS[rok.diff & 3]);
}

// /handoff [<name|#>]: name the next host (host); with a player, hand the session over now
void handoff_slash(const char *rest, Out *o) {
    while (rest && *rest == ' ') rest++;
    if (!rest || !*rest) { status(o); return; }
    if (!funcs_ok) { out_printf(o, "/handoff: not available in this game build\n"); return; }
    if (admin_is_client() || !ue_is_listen_server(ue_world())) { out_printf(o, "/handoff: only the host can hand the session over\n"); return; }
    UObject *ps = admin_find_player(rest, o);
    if (!ps) return;
    UObject *pc = ue_get_ptr(ps, "Owner");
    char name[64], key[80], addr[120];
    admin_ps_name(ps, name, sizeof name);
    if (!is_pc(pc) || pc == ue_local_pc()) { out_printf(o, "/handoff: %s can't host (a bot, or you)\n", name); return; }
    int port;
    if (!successor_addr(ps, addr, sizeof addr, &port)) { out_printf(o, "/handoff: %s can't be reached once they host (host_ip=1)\n", name); return; }
    admin_ps_key(ps, key, sizeof key);
    snprintf(designated, sizeof designated, "%s", key);
    UObject **pa; int n = admin_player_array(&pa);
    designated_idx = -1;
    for (int i = 0; i < n; i++) if (pa[i] == ps) designated_idx = i;
    go_pending = 1;
    char pkg[300];
    int run = in_mission(pkg, sizeof pkg) && current_run();
    LOG("handoff: /handoff %s (%s, #%d)", name, key, designated_idx);
    admin_notice("%s hosts the session now%s.", name, run ? "; the run continues from the start of this chapter" : "");
    out_printf(o, "handing the session to %s%s\n", name, run ? " (the run continues)" : "");
}

void handoff_players_panel(void) {   // admin.c's Players tab: who hosts next
    static Out o;
    out_reset(&o);
    status(&o);
    char *nl = strchr(o.buf, '\n');
    if (nl) *nl = 0;
    ov_text_dim("Host hand-off: %s", o.buf);
}

#ifndef B4B_RELEASE
// dev: `handoff` (status), `handoff runs` (this profile's campaign runs), `handoff run <id>` (one run's JSON start),
// `handoff resume <id>` (continue one of our own runs the way a take-over does: JoinRun), `handoff go` (client: act on
// the plan now), `handoff <name|#>` (= /handoff)
int handoff_cmd(const char *verb, char *rest, Out *o) {
    if (strcmp(verb, "handoff")) return 0;
    char *a = rest ? strtok(rest, " ") : NULL, *b = a ? strtok(NULL, "") : NULL;
    if (!a) { status(o); return 1; }
    if (!strcmp(a, "runs") || !strcmp(a, "run")) {
        RunRec *r[64];
        int n = runs_list(local_settings(), r, 64);
        UObject *crm = run_manager();
        out_printf(o, "run manager: %p run id %d; profile runs: %d (limit %d)\n", (void *)crm, crm ? *(int32_t *)((char *)crm + CRM_RUN_ID) : -1, n,
                   funcs_ok ? *(int *)ADDR_MAX_RUNS : -1);
        int want = b ? atoi(b) : 0;
        for (int i = 0; i < n; i++) {
            char m[128], nx[128];
            w2a(&r[i]->map, m, sizeof m); w2a(&r[i]->next, nx, sizeof nx);
            if (want && r[i]->id != want) continue;
            out_printf(o, "  %d map=%s next=%s %s retries=%d state=%d dver=%d ts=%lld len=%d\n", r[i]->id, m, nx, DIFFS[r[i]->diff & 3],
                       r[i]->retries, r[i]->state, r[i]->dver, (long long)r[i]->ts, r[i]->data.num);
            if (want) { char d[1200]; w2a(&r[i]->data, d, sizeof d); out_printf(o, "%s\n", d); }
        }
        RunRec *c = current_run();
        if (c) out_printf(o, "current: %d\n", c->id);
        return 1;
    }
    if (!strcmp(a, "resume") && b) {
        RunRec *r = run_by_id(atoi(b));
        if (!r) { out_printf(o, "no run %s\n", b); return 1; }
        run_free(&rok);
        rok.id = r->id; rok.diff = r->diff; rok.retries = r->retries; rok.dver = r->dver; rok.len = r->data.num - 1;
        w2a(&r->map, rok.map, sizeof rok.map); w2a(&r->next, rok.next, sizeof rok.next);
        rok.data = calloc(rok.len + 1, sizeof(wchar_t));
        memcpy(rok.data, r->data.data, rok.len * sizeof(wchar_t));
        snprintf(rok.path, sizeof rok.path, "/Game/Maps/Missions/Evansburgh/MAP_PERS_%s", rok.map);
        char *p = strtok(NULL, " ");
        if (p) snprintf(rok.path, sizeof rok.path, "%s", p);
        rok.ok = 1;
        out_printf(o, "resume %d at %s: %s\n", rok.id, rok.path, resume_run() ? "failed" : "JoinRun called");
        return 1;
    }
    if (!strcmp(a, "go")) { handoff_request("dev handoff go", 0); out_printf(o, "requested\n"); return 1; }
    char arg[160];
    snprintf(arg, sizeof arg, "%s%s%s", a, b ? " " : "", b ? b : "");
    handoff_slash(arg, o);
    return 1;
}
#endif

static void ini_key(const char *k, const char *v, void *ctx) { (void)ctx; handoff_live(k, v); }

int handoff_init(void) {
    cmds_ini_each(ini_key, NULL);
    funcs_ok = !memcmp((void *)ADDR_PROFILE_SETTINGS, SIG_SETTINGS, sizeof SIG_SETTINGS) &&
               !memcmp((void *)ADDR_UPSERT_RUN, SIG_UPSERT, sizeof SIG_UPSERT) &&
               !memcmp((void *)ADDR_JOINRUN, SIG_JOINRUN, sizeof SIG_JOINRUN) &&
               !memcmp((void *)ADDR_RESIZE, SIG_RESIZE, sizeof SIG_RESIZE);
    if (funcs_ok && !memcmp((void *)ADDR_REQUEST_EXIT, SIG_REQUEST_EXIT, sizeof SIG_REQUEST_EXIT) &&
        MH_CreateHook((void *)ADDR_REQUEST_EXIT, (void *)request_exit_detour, (void **)&orig_request_exit) == MH_OK &&
        MH_EnableHook((void *)ADDR_REQUEST_EXIT) == MH_OK) LOG("handoff: RequestExit hooked (the host tells its players before it quits)");
    else LOG("handoff: RequestExit not hooked: players notice a quitting host only by its timeout");
    LOG("handoff: %s (listen port %d)", funcs_ok ? "ready" : "signature mismatch: off", funcs_ok ? *(int *)ADDR_URL_PORT : -1);
    return 0;
}
