// Ragdoll deaths for the Ridden (experimental, fork module). Local and cosmetic: every machine ragdolls the bodies it
// draws, nothing is sent over the network, so no protocol change (players without it see the game's own deaths).
//
// Step 1 (this file): physics falls instead of the canned death animations. Step 2 (later): impulse from the killing
// hit (shotgun blasts, explosions).
//
// Chat command /ragdoll (anyone, host or client; the setting is per machine and not saved yet):
//   /ragdoll                 status
//   /ragdoll on [delay]      ragdoll Ridden that die from now on; delay = seconds of the game's own death animation
//                            first (default 0 = physics at once)
//   /ragdoll off
//   /ragdoll mass [x]        every body's mass x the scale (0.1-10, default 1), for bodies that die from now on
//   /ragdoll gravity [x]     gravity on our ragdolls x the scale (0-5, default 1), at once, bodies in flight included
//   /ragdoll deadline [s]    every ragdoll still there s seconds after death is removed (12-120, default 15)
//   /ragdoll probe           the nearest living Ridden: its mesh, physics asset, collision, and the death-related
//                            functions/properties of its classes, into the log (discovery)
//   /ragdoll trace [secs]    log the game's function calls on Ridden (default 30 s): every new class.function once,
//                            plus each death's calls just before and after it (discovery: how the game kills a Ridden).
//                            Also logs native call stacks ("ragdoll stack" lines) of the corpse swap (the stand-in's
//                            construction) and of the deletion (UnPossessed / EndPlay / Destroyed on a dead Ridden)
//
// Detection: an incremental scan of the object array finds living Ridden (GobiCharacters that aren't heroes; see
// cheats.c cmd_killall), then their LifeStateComponent's IsAlive is polled each tick. Bosses are left alone.
// Ragdoll: capsule collision off; mesh collision profile "Ragdoll", all bodies simulating physics (which also detaches
// the mesh from the capsule), bodies woken. The first few are logged before/after in detail.
// Settling: the game swaps a dead body for a frozen stand-in once its own settle check passes, and deletes the real
// one 1 s later. A hook on that check holds it for our ragdolls until the body is at rest (see "settle hold").
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "MinHook.h"
#include "ue.h"
#include "log.h"
#include "cmds.h"

typedef FName *(*FNameCtorFn)(FName *self, const wchar_t *name, int find_type);
#define ADDR_FNAME_CTOR VA(0x1424BC8E0ull)
typedef void (*PeFn)(UObject *o, UFunction *fn, void *params);
#define COMP_OWNER(c)   (*(UObject **)((char *)(c) + 0xD8))   // UActorComponent::OwnerPrivate (unreflected)
#define LIVE_FLAGS      0x30                                  // RF_ClassDefaultObject | RF_ArchetypeObject

#define MAX_TRACKED   768
#define SCAN_STEP     32768   // objects looked at per tick by the Ridden scan
#define DETAIL_LOGS   5       // ragdolls logged in full detail per /ragdoll on

// ---- small helpers (same patterns as cheats.c) ----
static double now_s(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static UClass *cls(const char *name) {
    static struct { const char *name; UClass *c; } cache[16];
    for (int i = 0; i < 16 && cache[i].name; i++) if (!strcmp(cache[i].name, name)) return cache[i].c;
    UClass *c = ue_find_class(name);
    for (int i = 0; i < 16 && c; i++) if (!cache[i].name) { cache[i].name = name; cache[i].c = c; break; }
    return c;
}

typedef struct { UObject *obj; UFunction *fn; uint8_t p[512]; } Call;
static int call_prep(Call *c, UObject *obj, const char *fname) {
    c->obj = obj;
    c->fn = obj && U_CLASS(obj) ? ue_find_function(U_CLASS(obj), fname) : NULL;
    if (!c->fn || UFN_PARMSSIZE(c->fn) > sizeof c->p) return 0;
    memset(c->p, 0, UFN_PARMSSIZE(c->fn));
    return 1;
}
static void *carg(Call *c, const char *name) { FField *f = ue_find_prop(c->fn, name); return f ? c->p + FP_OFFSET(f) : NULL; }
// Our own calls (and whatever the game runs inside them) are left out of /ragdoll trace and its stacks: only the game
// thread makes them, so a depth counter plus that thread's id tells the ProcessEvent hook which calls are ours.
static volatile DWORD own_tid;
static volatile int own_depth;
static void call_go(Call *c) {
    if (!own_depth++) own_tid = GetCurrentThreadId();
    ue_process_event(c->obj, c->fn, c->p);
    own_depth--;
}
static int own_call(void) { return own_depth && own_tid == GetCurrentThreadId(); }
#define SET(c, name, type, v) do { void *_a = carg(c, name); if (_a) *(type *)_a = (v); } while (0)
static UObject *call_obj(UObject *o, const char *fn) {
    Call c; if (!call_prep(&c, o, fn)) return NULL;
    call_go(&c);
    void *r = carg(&c, "ReturnValue");
    return r ? *(UObject **)r : NULL;
}
static int call_bool(UObject *o, const char *fn) {
    Call c; if (!call_prep(&c, o, fn)) return -1;
    call_go(&c);
    uint8_t *r = carg(&c, "ReturnValue");
    return r ? *r != 0 : -1;
}
static int call_void(UObject *o, const char *fn) { Call c; if (!call_prep(&c, o, fn)) return 0; call_go(&c); return 1; }

static const char *oname(UObject *o, char *b, size_t n) { return o ? ue_obj_name(o, b, n) : "-"; }
static const char *cname(UObject *o, char *b, size_t n) { return o && U_CLASS(o) ? ue_obj_name(U_CLASS(o), b, n) : "-"; }

static FName fname_of(const wchar_t *w) { FName n = {0, 0}; ((FNameCtorFn)ADDR_FNAME_CTOR)(&n, w, 1); return n; }

static int actor_loc(UObject *a, float v[3]) {
    Call c;
    if (!a || !call_prep(&c, a, "K2_GetActorLocation")) return -1;
    call_go(&c);
    float *r = carg(&c, "ReturnValue");
    if (!r) return -1;
    v[0] = r[0]; v[1] = r[1]; v[2] = r[2];
    return 0;
}

static int tarray_num(UObject *o, const char *prop) {
    int32_t off = o ? ue_prop_offset(o, prop) : -1;
    return off < 0 ? -1 : ((TArray *)((char *)o + off))->num;
}

// ---- which actors are Ridden ----
static int is_ridden_fast(UObject *a) {   // no property lookups: safe and cheap inside the ProcessEvent hook
    static UClass *gc, *hc;
    if (!gc) { gc = cls("GobiCharacter"); hc = cls("HeroCharacter"); }
    return a && gc && ue_is_a(a, gc) && !(hc && ue_is_a(a, hc));
}
static int is_ridden(UObject *a) {
    if (!is_ridden_fast(a)) return 0;
    UClass *pcc = cls("PlayerController");
    UObject *ctrl = ue_get_ptr(a, "Controller");
    return !(ctrl && pcc && ue_is_a(ctrl, pcc));   // never a player-controlled character
}
static int is_boss(UObject *a) {   // own death sequences / huge bodies: leave them to the game
    static const char *const BOSS[] = {"Brute_", "Hag_", "Breaker", "Titan", "Sentinel", "Sleeper", "Mother", "Ogre", NULL};
    char b[160];
    cname(a, b, sizeof b);
    for (int i = 0; BOSS[i]; i++) if (strstr(b, BOSS[i])) return 1;
    return 0;
}

// ---- tracking ----
typedef struct {
    UObject *a; int32_t ai;
    UObject *ls; int32_t lsi;
    double died_at;   // 0 = alive
    uint8_t done;     // ragdolled, or left alone
    uint8_t ragdolled, cap_coll;           // what apply() changed, to undo it if the actor comes back (reuse)
    uint8_t stacked;                       // deletion stacks already logged for this body (1 << kind)
    uint8_t rest, released;                // settle hold: checks at rest in a row; the game's own check runs again
    uint16_t checks;                       // settle checks of the game seen on this body
    UObject *mesh, *rc;                    // its mesh and RagdollComponent (the settle check's `this`)
    const struct BodyList *bodies;         // its physics bodies' bone names (settle test), NULL = root body only
    float last_v;                          // fastest body at the last settle check, cm/s
    uint8_t spared;                        // times the corpse budget's delete was skipped for it
    uint8_t mass_set;                      // apply() changed its mass scale (undone if the actor comes back)
    uint8_t ended;                         // removed by the deadline (hidden, physics off, destroy asked)
    FName prof;
    float rel_loc[3], rel_rot[3];
} Tracked;
static Tracked tr[MAX_TRACKED];
static int ntr, scan_at, n_overflow, n_revived;
static int mode_on;
static float delay_s;
static int n_applied, n_failed, n_deaths, detail_left;

// The bone names of a physics asset's bodies (USkeletalBodySetup.BoneName), for the settle test: one list per asset
#define MAX_BODIES 32
typedef struct BodyList { UObject *pa; int n; FName bone[MAX_BODIES]; } BodyList;
static BodyList body_lists[8];
static int n_body_lists;

// ---- tuning: mass and gravity of our ragdolls (this machine only, not saved, like the rest of /ragdoll) ----
// Mass: USkeletalMeshComponent::SetAllMassScale at ragdoll time, every body's mass x the scale (ratios between bodies
// kept). Under gravity alone it changes nothing (every mass falls alike); it decides how far a push moves a body:
// the killing hit's impulse (step 2), other bodies, things walking into it.
// Gravity: Unreal has only gravity on/off per body, and the world's gravity also moves players and projectiles, so the
// difference is added as a force every frame: AddForceToAllBodiesBelow from the root body (BoneName None) down,
// (scale - 1) x the world's gravity, as an acceleration (bAccelChange: mass has no effect, like gravity itself). Only
// while the settle hold holds the body; the game swaps it for its frozen stand-in within ~1 s of the release.
static float mass_scale = 1.f, grav_scale = 1.f;
static float world_gz;   // the world's gravity, cm/s^2, read once per map; 0 = not read yet
static int n_grav_fail;

static int world_check(void) {   // new map: every old pointer is gone
    static UObject *last_w;
    UObject *w = ue_world();
    if (w != last_w) { last_w = w; ntr = 0; scan_at = 0; n_body_lists = 0; world_gz = 0; }
    return w != NULL;
}

// The game's corpse stand-in: when the settle check passes it hides the real Ridden and puts an actor of the class in
// its RagdollComponent.VFXCorpseActorType in the same pose (the body players see from then on; the Ridden is deleted).
// Known here only for /ragdoll trace (spawn and corpse stacks, calls on stand-ins).
static UClass *corpse_cls[8];
static int n_corpse_cls;
static void note_corpse_class(UObject *ridden);
static int is_corpse(UObject *o) { for (int k = 0; k < n_corpse_cls; k++) if (ue_is_a(o, corpse_cls[k])) return 1; return 0; }

static int tracked_has(UObject *a) { for (int i = 0; i < ntr; i++) if (tr[i].a == a) return 1; return 0; }

static void scan_step(void) {
    int32_t n = ue_num_objects();
    if (scan_at >= n) scan_at = 0;
    int32_t end = scan_at + SCAN_STEP < n ? scan_at + SCAN_STEP : n;
    for (int32_t i = scan_at; i < end; i++) {
        UObject *o = ue_object_at(i);
        if (!o || (U_FLAGS(o) & LIVE_FLAGS) || !is_ridden_fast(o) || !ue_object_usable(o, i) || tracked_has(o)) continue;
        if (!is_ridden(o)) continue;
        UObject *ls = call_obj(o, "GetLifeStateComponent");
        if (!ls || call_bool(ls, "IsAlive") != 1) continue;   // only bodies we see die
        if (ntr == MAX_TRACKED) { n_overflow++; continue; }
        memset(&tr[ntr], 0, sizeof tr[ntr]);
        tr[ntr].a = o; tr[ntr].ai = i; tr[ntr].ls = ls; tr[ntr].lsi = U_INDEX(ls);
        ntr++;
    }
    scan_at = end;
}

// ---- trace (ProcessEvent hook, only while /ragdoll trace runs) ----
static PeFn orig_pe;
static int hook_made;
static volatile LONG tracing;
static double trace_t0, trace_end;
static CRITICAL_SECTION tcs;
typedef struct { UClass *oc; UFunction *fn; } Pair;
static Pair pairs[4096];
static int npairs, n_calls;
typedef struct { double t; UObject *actor; UClass *oc; UFunction *fn; } RingE;
static RingE ring[512];
static int ring_at;
typedef struct { UObject *actor; double from, until; int lines; } Focus;
#define FOCUS_SECS 8.0
static Focus focus[8];

// ---- stacks (discovery, part of /ragdoll trace): which native code swaps and deletes a dead Ridden ----
// Neither the corpse swap nor the deletion is a ProcessEvent call of its own, so they are found from their effects:
//   spawn      the stand-in (RagdollComponent.VFXCorpseActorType) is constructed: StaticConstructObject_Internal hook
//   unpossess  GameplayActionsComponent.OnUnPossessed on a Ridden we saw die (the first sign of its deletion)
//   endplay    the first ReceiveEndPlay on that Ridden or one of its components
//   destroyed  ReceiveDestroyed on it
//   corpse     the stand-in's first event (UserConstructionScript / ReceiveBeginPlay): a second view of the spawn
// Each logs the call stack (RtlCaptureStackBackTrace: the x64 unwind data in the exe) as static VAs, every frame with
// its function's start (RtlLookupFunctionEntry, chained entries followed) and, once per function, that function's
// first 24 bytes, for a signature check like every other hook here. Frames are labelled when the function is a
// UFunction's native exec thunk ("exec Actor.K2_DestroyActor": a Blueprint called it) or the Blueprint VM
// (UObject::ProcessInternal, the Func of every script function), so a Blueprint-driven swap shows up as such.
enum { ST_SPAWN, ST_UNPOSSESS, ST_ENDPLAY, ST_DESTROYED, ST_CORPSE, ST_N };
static const char *const ST_TAG[ST_N] = {"spawn", "unpossess", "endplay", "destroyed", "corpse"};
#define STACKS_PER_KIND 3
#define STACK_FRAMES    48
#define STACK_BYTES     24
static volatile LONG stack_budget[ST_N], stack_id;
static volatile DWORD game_tid;
static FName nm_unpossessed, nm_endplay, nm_destroyed, nm_ucs, nm_beginplay;

// StaticConstructObject_Internal (as in cheats.c; 4.25 argument list). Hooked only while a trace runs. cheats.c checks
// these bytes before it creates the engine CheatManager, so /size /freecam /slomo /freeze, used for the first time
// during a trace, answer "not available" (logged "signature mismatch"); they work again once the trace ends.
#define ADDR_SCO VA(0x1426E4840ull)
static const uint8_t SIG_SCO[] = {0x4c,0x89,0x44,0x24,0x18,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,
                                  0x57,0x48,0x81,0xec,0xa8,0x01,0x00,0x00};
typedef UObject *(*ScoFn)(UClass *, UObject *, FName, uint32_t, uint32_t, UObject *, uint8_t, void *, uint8_t);
static ScoFn orig_sco;
static int sco_state;   // 0 not tried, 1 hooked, -1 signature mismatch or hook failed

#define UFN_FUNC(o) (*(uintptr_t *)((char *)(o) + 0xE0))   // UFunction::Func (tools/sdkdump.py)
typedef struct { uintptr_t at; UFunction *fn; } Native;
static Native *natives;
static int n_natives;
static uintptr_t bp_vm;   // UObject::ProcessInternal

static uintptr_t exe_lo, exe_hi, self_lo, self_hi;

static int native_cmp(const void *a, const void *b) {
    uintptr_t x = ((const Native *)a)->at, y = ((const Native *)b)->at;
    return x < y ? -1 : x > y;
}

static void module_range(HMODULE m, uintptr_t *lo, uintptr_t *hi) {
    IMAGE_DOS_HEADER *d = (IMAGE_DOS_HEADER *)m;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)((char *)m + d->e_lfanew);
    *lo = (uintptr_t)m;
    *hi = *lo + nt->OptionalHeader.SizeOfImage;
}

// game thread, once: every native UFunction's exec thunk, and the Blueprint VM entry (the Func most script
// functions share; a majority vote, since script functions all point at UObject::ProcessInternal)
static void natives_build(void) {
    if (natives) return;
    HMODULE self = NULL;
    module_range(GetModuleHandleW(NULL), &exe_lo, &exe_hi);
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(void *)natives_build, &self)) module_range(self, &self_lo, &self_hi);
    UClass *fc = ue_find_class("Function");
    int cap = 1 << 15;
    natives = malloc(cap * sizeof *natives);
    if (!fc || !natives) { LOG("ragdoll stack: can't index native functions (%s)", fc ? "no memory" : "no Function class"); return; }
    uintptr_t cand = 0;
    int votes = 0;
    for (int32_t i = 0, n = ue_num_objects(); i < n; i++) {
        UObject *o = ue_object_at(i);
        if (!o || U_CLASS(o) != fc) continue;
        uintptr_t f = UFN_FUNC(o);
        if (f < exe_lo || f >= exe_hi) continue;
        if (!(UFN_FLAGS(o) & 0x400)) {   // FUNC_Native not set: a script function
            if (!votes) { cand = f; votes = 1; } else votes += cand == f ? 1 : -1;
            continue;
        }
        if (n_natives == cap) {
            Native *bigger = realloc(natives, 2 * cap * sizeof *natives);
            if (!bigger) break;
            natives = bigger; cap *= 2;
        }
        natives[n_natives++] = (Native){f, o};
    }
    bp_vm = cand;
    qsort(natives, n_natives, sizeof *natives, native_cmp);
    LOG("ragdoll stack: %d native functions indexed, Blueprint VM (ProcessInternal) at 0x%llx", n_natives,
        bp_vm ? (unsigned long long)(bp_vm - g_base_delta) : 0ull);
}

static uintptr_t fn_start(uintptr_t pc) {   // start of the function holding pc, following chained unwind entries
    DWORD64 base = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)pc, &base, NULL);
    for (int guard = 0; rf && guard < 8; guard++) {
        if (rf->UnwindData & 1) { rf = (PRUNTIME_FUNCTION)(base + (rf->UnwindData & ~1u)); continue; }   // indirect entry
        const uint8_t *ui = (const uint8_t *)(base + rf->UnwindData);
        if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
        rf = (PRUNTIME_FUNCTION)(ui + 4 + 2 * ((ui[2] + 1u) & ~1u));   // the parent entry, after the unwind codes
    }
    return rf ? (uintptr_t)(base + rf->BeginAddress) : 0;
}

static const char *fn_label(uintptr_t f, char *out, size_t n) {
    char a[96], b[96];
    out[0] = 0;
    if (!f) return out;
    if (f == ADDR_PROCESSEVENT) snprintf(out, n, "  [UObject::ProcessEvent]");
    else if (f == ADDR_SCO) snprintf(out, n, "  [StaticConstructObject_Internal]");
    else if (f == ADDR_GAMEENGINETICK) snprintf(out, n, "  [UGameEngine::Tick]");
    else if (bp_vm && f == bp_vm) snprintf(out, n, "  [UObject::ProcessInternal: Blueprint VM]");
    else if (n_natives) {
        Native key = {f, NULL}, *hit = bsearch(&key, natives, n_natives, sizeof *natives, native_cmp);
        if (hit) snprintf(out, n, "  [exec %s.%s]", oname(U_OUTER(hit->fn), a, sizeof a), ue_obj_name(hit->fn, b, sizeof b));
    }
    return out;
}

static void stack_log(int kind, const char *what) {
    if (InterlockedDecrement(&stack_budget[kind]) < 0) return;
    void *fr[STACK_FRAMES];
    USHORT n = RtlCaptureStackBackTrace(1, STACK_FRAMES, fr, NULL);   // skip this function
    LONG id = InterlockedIncrement(&stack_id);
    LOG("ragdoll stack #%ld %s: %s (thread %lu%s, +%.2fs, %u frames)", id, ST_TAG[kind], what, GetCurrentThreadId(),
        GetCurrentThreadId() == game_tid ? ", game thread" : "", now_s() - trace_t0, n);
    uintptr_t shown[STACK_FRAMES];
    int n_shown = 0;
    for (int i = 0; i < n; i++) {
        uintptr_t pc = (uintptr_t)fr[i];
        if (!pc) break;   // Wine ends the walk at the stack base with zero entries
        char lab[200], hex[STACK_BYTES * 3 + 8], mod[MAX_PATH];
        if (pc >= exe_lo && pc < exe_hi) {
            uintptr_t f = fn_start(pc - 1);   // pc - 1: a call that ends a function returns past its end
            hex[0] = 0;
            int seen = 0;
            for (int k = 0; k < n_shown; k++) if (shown[k] == f) seen = 1;
            if (f && !seen) {
                shown[n_shown++] = f;
                char *p = hex + sprintf(hex, "  bytes");
                for (int k = 0; k < STACK_BYTES; k++) p += sprintf(p, " %02x", ((const uint8_t *)f)[k]);
            }
            if (f) LOG("ragdoll stack #%ld %2d 0x%llx = fn 0x%llx+0x%llx%s%s", id, i, (unsigned long long)(pc - g_base_delta),
                       (unsigned long long)(f - g_base_delta), (unsigned long long)(pc - f), fn_label(f, lab, sizeof lab), hex);
            else LOG("ragdoll stack #%ld %2d 0x%llx = fn ? (no unwind entry: leaf)", id, i, (unsigned long long)(pc - g_base_delta));
        } else if (pc >= self_lo && pc < self_hi) {
            LOG("ragdoll stack #%ld %2d b4bcoop+0x%llx", id, i, (unsigned long long)(pc - self_lo));
        } else {
            HMODULE m = NULL;
            mod[0] = 0;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)pc, &m) && m) GetModuleFileNameA(m, mod, sizeof mod);
            const char *base = strrchr(mod, '\\');
            LOG("ragdoll stack #%ld %2d %s+0x%llx", id, i, mod[0] ? (base ? base + 1 : mod) : "?",
                (unsigned long long)(m ? pc - (uintptr_t)m : pc));
        }
    }
    if (n < 4) LOG("ragdoll stack #%ld: the walk stopped early (a frame without unwind data)", id);
}

static int is_corpse_class(UClass *c) { for (int k = 0; k < n_corpse_cls; k++) if (c == corpse_cls[k]) return 1; return 0; }

static UObject *sco_detour(UClass *c, UObject *outer, FName name, uint32_t fl, uint32_t ifl, UObject *tmpl, uint8_t copy,
                           void *graph, uint8_t arch) {
    UObject *r = orig_sco(c, outer, name, fl, ifl, tmpl, copy, graph, arch);
    if (tracing && c && r && !(fl & LIVE_FLAGS) && is_corpse_class(c) && !own_call()) {
        char a[160], b[160], w[360];
        snprintf(w, sizeof w, "%s constructed (%s)", oname(r, a, sizeof a), oname(c, b, sizeof b));
        stack_log(ST_SPAWN, w);
    }
    return r;
}

static Tracked *dead_tracked(UObject *a) {   // game thread only: tr[] belongs to it
    for (int i = 0; i < ntr; i++) if (tr[i].a == a) return tr[i].died_at ? &tr[i] : NULL;
    return NULL;
}

static int same_name(UFunction *fn, FName n) { return n.idx && U_NAME(fn).idx == n.idx && U_NAME(fn).num == n.num; }

// from trace_call, for calls on a Ridden or a stand-in that aren't ours
static void stack_check(UObject *o, UObject *actor, UFunction *fn, void *params) {
    char a[160], b[160], c[160], w[360];
    if (n_corpse_cls && is_corpse(actor)) {
        if (same_name(fn, nm_ucs) || same_name(fn, nm_beginplay)) {
            static UObject *last;
            if (actor == last) return;   // one stack per stand-in
            last = actor;
            snprintf(w, sizeof w, "%s.%s on %s", cname(o, a, sizeof a), ue_obj_name(fn, b, sizeof b), oname(actor, c, sizeof c));
            stack_log(ST_CORPSE, w);
        }
        return;
    }
    int kind = same_name(fn, nm_unpossessed) ? ST_UNPOSSESS : same_name(fn, nm_endplay) ? ST_ENDPLAY :
               same_name(fn, nm_destroyed) ? ST_DESTROYED : -1;
    if (kind < 0 || GetCurrentThreadId() != game_tid) return;
    Tracked *x = dead_tracked(actor);
    // a ragdoll the settle hold released goes the known way (swap, delete 1 s later): only the others are of interest
    if (!x || x->released || (x->stacked & (1u << kind))) return;
    x->stacked |= 1u << kind;
    int n = snprintf(w, sizeof w, "%s.%s on %s, %.2f s after death, %s", cname(o, a, sizeof a), ue_obj_name(fn, b, sizeof b),
                     oname(actor, c, sizeof c), now_s() - x->died_at, x->ragdolled ? "ragdolled" : "game's own death");
    if (kind == ST_ENDPLAY && params && n > 0 && n < (int)sizeof w)
        snprintf(w + n, sizeof w - n, ", reason %u", *(uint8_t *)params);   // 0 Destroyed, 1 LevelTransition, 3 RemovedFromWorld
    stack_log(kind, w);
}

static void stacks_arm(void) {   // game thread, from trace_start
    natives_build();
    if (!nm_unpossessed.idx) {
        nm_unpossessed = fname_of(L"OnUnPossessed"); nm_endplay = fname_of(L"ReceiveEndPlay");
        nm_destroyed = fname_of(L"ReceiveDestroyed"); nm_ucs = fname_of(L"UserConstructionScript");
        nm_beginplay = fname_of(L"ReceiveBeginPlay");
    }
    for (int k = 0; k < ST_N; k++) InterlockedExchange(&stack_budget[k], STACKS_PER_KIND);
    if (!sco_state) {
        if (memcmp((void *)ADDR_SCO, SIG_SCO, sizeof SIG_SCO)) { sco_state = -1; LOG("ragdoll stack: StaticConstructObject signature mismatch, no spawn stacks"); }
        else if (MH_CreateHook((void *)ADDR_SCO, (void *)sco_detour, (void **)&orig_sco) != MH_OK) { sco_state = -1; LOG("ragdoll stack: StaticConstructObject hook failed"); }
        else sco_state = 1;
    }
    if (sco_state == 1 && MH_EnableHook((void *)ADDR_SCO) != MH_OK) LOG("ragdoll stack: StaticConstructObject hook couldn't be enabled");
    LOG("ragdoll stack: armed (%d each: spawn%s, unpossess, endplay, destroyed, corpse); corpse classes known: %d",
        STACKS_PER_KIND, sco_state == 1 ? "" : " OFF", n_corpse_cls);
}

static void stacks_disarm(void) { if (sco_state == 1) MH_DisableHook((void *)ADDR_SCO); }

static UObject *owner_actor(UObject *o) {
    static UClass *ac, *cc, *ai;
    if (!ac) { ac = cls("Actor"); cc = cls("ActorComponent"); ai = cls("AnimInstance"); }
    if (!ac || !cc) return NULL;
    if (ue_is_a(o, ac)) return o;
    if (ue_is_a(o, cc)) return COMP_OWNER(o);
    if (ai && ue_is_a(o, ai)) { UObject *m = U_OUTER(o); return m && ue_is_a(m, cc) ? COMP_OWNER(m) : NULL; }
    return NULL;
}

static int per_frame(const char *fn) {   // left out of the death sequences (still listed once)
    return !strncmp(fn, "ReceiveTick", 11) || !strncmp(fn, "BlueprintUpdateAnimation", 24) ||
           !strncmp(fn, "BlueprintThreadSafe", 19) || !strncmp(fn, "BlueprintPostEvaluate", 21);
}

static void trace_call(UObject *o, UFunction *fn, void *params) {
    UObject *actor = owner_actor(o);
    if (!actor || !(is_ridden_fast(actor) || (n_corpse_cls && is_corpse(actor)))) return;
    stack_check(o, actor, fn, params);
    double t = now_s();
    char cb[128], fb[128], ab[128];
    EnterCriticalSection(&tcs);
    n_calls++;
    UClass *oc = U_CLASS(o);
    ring[ring_at] = (RingE){t, actor, oc, fn};
    ring_at = (ring_at + 1) % (int)(sizeof ring / sizeof ring[0]);
    int seen = 0;
    for (int i = 0; i < npairs; i++) if (pairs[i].oc == oc && pairs[i].fn == fn) { seen = 1; break; }
    if (!seen && npairs < (int)(sizeof pairs / sizeof pairs[0])) {
        pairs[npairs++] = (Pair){oc, fn};
        LOG("ragdoll trace +%.2fs new %s.%s (on %s)", t - trace_t0, ue_obj_name(oc, cb, sizeof cb),
            ue_obj_name(fn, fb, sizeof fb), ue_obj_name(actor, ab, sizeof ab));
    }
    for (int i = 0; i < 8; i++) {
        Focus *f = &focus[i];
        if (f->actor != actor || t > f->until || f->lines >= 600) continue;
        ue_obj_name(fn, fb, sizeof fb);
        if (per_frame(fb)) break;
        f->lines++;
        if (!strcmp(fb, "ReceiveEndPlay") && params)   // EEndPlayReason: 0 Destroyed, 1 LevelTransition, 3 RemovedFromWorld
            LOG("ragdoll death %s +%.3fs %s.%s (reason %u)", ue_obj_name(actor, ab, sizeof ab), t - f->from,
                ue_obj_name(oc, cb, sizeof cb), fb, *(uint8_t *)params);
        else
            LOG("ragdoll death %s +%.3fs %s.%s", ue_obj_name(actor, ab, sizeof ab), t - f->from, ue_obj_name(oc, cb, sizeof cb), fb);
        break;
    }
    LeaveCriticalSection(&tcs);
}

static void pe_detour(UObject *o, UFunction *fn, void *params) {
    if (tracing && o && fn && !own_call()) trace_call(o, fn, params);
    orig_pe(o, fn, params);
}

static void trace_on_death(UObject *actor) {   // game thread, from the tick that saw it die
    char ab[128], cb[128], fb[128];
    double t = now_s();
    EnterCriticalSection(&tcs);
    LOG("ragdoll death %s (%s) at +%.2fs: its calls in the last second before we saw it:", ue_obj_name(actor, ab, sizeof ab),
        cname(actor, cb, sizeof cb), t - trace_t0);
    int n = (int)(sizeof ring / sizeof ring[0]);
    for (int k = 0; k < n; k++) {
        RingE *e = &ring[(ring_at + k) % n];
        if (e->actor != actor || t - e->t > 1.0) continue;
        ue_obj_name(e->fn, fb, sizeof fb);
        if (per_frame(fb)) continue;
        LOG("ragdoll death %s -%.3fs %s.%s", ab, t - e->t, ue_obj_name(e->oc, cb, sizeof cb), fb);
    }
    int slot = 0;
    for (int i = 0; i < 8; i++) if (focus[i].until < focus[slot].until) slot = i;
    focus[slot] = (Focus){actor, t, t + FOCUS_SECS, 0};
    LOG("ragdoll death %s: calls in the next %.0f s follow (\"+s\" after we saw it)", ab, FOCUS_SECS);
    LeaveCriticalSection(&tcs);
}

static int trace_start(double secs, Out *o) {
    is_ridden_fast(NULL); owner_actor(ue_engine());   // look the classes up here, not first inside the hook
    if (!hook_made) {
        InitializeCriticalSection(&tcs);
        if (MH_CreateHook((void *)ADDR_PROCESSEVENT, (void *)pe_detour, (void **)&orig_pe) != MH_OK) {
            out_printf(o, "/ragdoll trace: hook failed\n");
            return -1;
        }
        hook_made = 1;
    }
    EnterCriticalSection(&tcs);
    npairs = n_calls = 0;
    memset(ring, 0, sizeof ring);
    memset(focus, 0, sizeof focus);
    trace_t0 = now_s();
    trace_end = trace_t0 + secs;
    LeaveCriticalSection(&tcs);
    game_tid = GetCurrentThreadId();   // chat commands run on the game thread
    if (MH_EnableHook((void *)ADDR_PROCESSEVENT) != MH_OK) { out_printf(o, "/ragdoll trace: hook failed\n"); return -1; }
    stacks_arm();
    InterlockedExchange(&tracing, 1);
    LOG("ragdoll trace: started for %.0f s (Ridden tracked: %d)", secs, ntr);
    return 0;
}

static void trace_stop(const char *why) {
    if (!tracing) return;
    InterlockedExchange(&tracing, 0);
    MH_DisableHook((void *)ADDR_PROCESSEVENT);
    stacks_disarm();
    LOG("ragdoll trace: stopped (%s): %d calls on Ridden, %d distinct class.function, %d deaths seen", why, n_calls, npairs, n_deaths);
}

// ---- probe ----
static int name_has(const char *s, const char *const *keys) {
    char low[256];
    size_t i = 0;
    for (; s[i] && i + 1 < sizeof low; i++) low[i] = (char)((s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i]);
    low[i] = 0;
    for (int k = 0; keys[k]; k++) if (strstr(low, keys[k])) return 1;
    return 0;
}
static const char *const KEYS[] = {"death", "dead", "die", "dying", "kill", "ragdoll", "physic", "corpse", "gib",
                                   "dismember", "impulse", "lifestate", "lifespan", "simulat", "collision", "hitreact", NULL};

static UObject *comp_of(UObject *actor, const char *class_name) {   // AActor::GetComponentByClass
    UClass *c = cls(class_name);
    Call k;
    if (!c || !actor || !call_prep(&k, actor, "GetComponentByClass")) return NULL;
    SET(&k, "ComponentClass", UClass *, c);
    call_go(&k);
    UObject **r = carg(&k, "ReturnValue");
    return r ? *r : NULL;
}

static void list_class(UClass *c, int all_funcs, int all_props, const char *what, UObject *inst) {
    char b[160], t[64], v[48];
    LOG("ragdoll probe: %s class chain:", what);
    for (UStruct *s = c; s; s = US_SUPER(s)) {
        LOG("ragdoll probe:   %s", ue_obj_name(s, b, sizeof b));
        if (!strcmp(b, "Object") || !strcmp(b, "ActorComponent") || !strcmp(b, "Actor")) break;
    }
    for (UStruct *s = c; s; s = US_SUPER(s)) {
        ue_obj_name(s, b, sizeof b);
        int top = !strcmp(b, "Object") || !strcmp(b, "Actor") || !strcmp(b, "ActorComponent");
        char sn[160];
        snprintf(sn, sizeof sn, "%s", b);
        for (UObject *f = US_CHILDREN(s); f; f = UF_NEXT(f)) {
            ue_obj_name(f, b, sizeof b);
            if (all_funcs || name_has(b, KEYS)) LOG("ragdoll probe:   fn %s.%s (flags %08x)", sn, b, UFN_FLAGS(f));
        }
        for (FField *p = US_CHILDPROPS(s); p; p = FF_NEXT(p)) {
            ue_name(FF_NAME(p), b, sizeof b);
            if (!all_props && !name_has(b, KEYS)) continue;
            ue_name(*(FName *)FF_CLASS(p), t, sizeof t);
            v[0] = 0;
            if (inst) {
                char *a = (char *)inst + FP_OFFSET(p);
                if (!strcmp(t, "FloatProperty")) snprintf(v, sizeof v, " = %g", *(float *)a);
                else if (!strcmp(t, "IntProperty")) snprintf(v, sizeof v, " = %d", *(int32_t *)a);
                else if (!strcmp(t, "ByteProperty") || !strcmp(t, "EnumProperty") || !strcmp(t, "BoolProperty"))
                    snprintf(v, sizeof v, " = byte %u", *(uint8_t *)a);
                else if (!strcmp(t, "ObjectProperty")) { char on[96]; snprintf(v, sizeof v, " = %s", oname(*(UObject **)a, on, sizeof on)); }
                else if (!strcmp(t, "ArrayProperty")) snprintf(v, sizeof v, " = %d entries", ((TArray *)a)->num);
            }
            LOG("ragdoll probe:   prop %s.%s %s @0x%x%s", sn, b, t, FP_OFFSET(p), v);
        }
        if (top) break;
    }
}

static FName collision_profile(UObject *comp) {
    FName n = {0, 0};
    Call c;
    if (comp && call_prep(&c, comp, "GetCollisionProfileName")) {
        call_go(&c);
        FName *r = carg(&c, "ReturnValue");
        if (r) n = *r;
    }
    return n;
}

static void log_mesh(UObject *mesh, const char *tag) {
    char a[160], b[160], c[96];
    UObject *sk = ue_get_ptr(mesh, "SkeletalMesh"), *pa = sk ? ue_get_ptr(sk, "PhysicsAsset") : NULL;
    UObject *pao = ue_get_ptr(mesh, "PhysicsAssetOverride");
    UObject *use = pao ? pao : pa;
    LOG("ragdoll %s: mesh %s (%s), skeletal mesh %s, physics asset %s%s: %d bodies, %d constraints", tag,
        oname(mesh, a, sizeof a), cname(mesh, c, sizeof c), oname(sk, b, sizeof b), oname(use, a, sizeof a),
        pao ? " (override)" : "", tarray_num(use, "SkeletalBodySetups"), tarray_num(use, "ConstraintSetup"));
    Call k;
    int sim = -1;
    if (call_prep(&k, mesh, "IsSimulatingPhysics")) { call_go(&k); uint8_t *r = carg(&k, "ReturnValue"); sim = r ? *r : -1; }
    int cen = -1;
    if (call_prep(&k, mesh, "GetCollisionEnabled")) { call_go(&k); uint8_t *r = carg(&k, "ReturnValue"); cen = r ? *r : -1; }
    LOG("ragdoll %s:   collision profile %s, collision enabled %d (0 none, 1 query, 2 physics, 3 both), simulating %d, anim %s",
        tag, ue_name(collision_profile(mesh), b, sizeof b), cen, sim, cname(ue_get_ptr(mesh, "AnimScriptInstance"), a, sizeof a));
}

static void cmd_probe(Out *o) {
    UObject *me = ue_local_pc() ? ue_get_ptr(ue_local_pc(), "Pawn") : NULL;
    float at[3] = {0}, v[3];
    int have_me = actor_loc(me, at) == 0;
    UObject *best = NULL;
    float bd = 1e30f;
    for (int i = 0; i < ntr; i++) {
        Tracked *t = &tr[i];
        if (t->died_at || !ue_object_usable(t->a, t->ai) || actor_loc(t->a, v)) continue;
        float d = have_me ? (v[0] - at[0]) * (v[0] - at[0]) + (v[1] - at[1]) * (v[1] - at[1]) + (v[2] - at[2]) * (v[2] - at[2]) : 0;
        if (d < bd) { bd = d; best = t->a; }
    }
    if (!best) { out_printf(o, "/ragdoll probe: no living Ridden found yet (in a mission, wait a second near some)\n"); return; }
    char a[160], c[160];
    LOG("ragdoll probe: ---- %s (%s), %.0f m away ----", oname(best, a, sizeof a), cname(best, c, sizeof c),
        have_me ? sqrtf(bd) / 100.f : -1.f);
    list_class(U_CLASS(best), 0, 0, "actor", best);
    UObject *ls = call_obj(best, "GetLifeStateComponent"), *hc = call_obj(best, "GetHealthComponent");
    if (ls) list_class(U_CLASS(ls), 0, 0, "life state component", ls);
    if (hc) list_class(U_CLASS(hc), 0, 0, "health component", hc);
    UObject *dr = comp_of(best, "PlayDeathReactionComponent"), *hr = comp_of(best, "PlayHitReactionComponent");
    UObject *pac = comp_of(best, "PhysicalAnimationComponent");
    if (dr) list_class(U_CLASS(dr), 1, 1, "death reaction component (everything)", dr);
    if (hr) list_class(U_CLASS(hr), 1, 1, "hit reaction component (everything)", hr);
    LOG("ragdoll probe: physical animation component: %s", pac ? oname(pac, a, sizeof a) : "none");
    note_corpse_class(best);
    UObject *rc = ue_get_ptr(best, "RagdollComponent");
    if (rc) list_class(U_CLASS(rc), 1, 1, "ragdoll component (everything)", rc);
    else LOG("ragdoll probe: no RagdollComponent");
    UObject *mesh = ue_get_ptr(best, "Mesh"), *cap = ue_get_ptr(best, "CapsuleComponent");
    if (mesh) {
        log_mesh(mesh, "probe");
        UObject *ai = ue_get_ptr(mesh, "AnimScriptInstance");
        if (ai) list_class(U_CLASS(ai), 1, 0, "anim instance (all functions)", ai);
    }
    if (cap) LOG("ragdoll probe: capsule %s, collision profile %s", oname(cap, a, sizeof a), ue_name(collision_profile(cap), c, sizeof c));
    LOG("ragdoll probe: ---- end ----");
    out_printf(o, "/ragdoll probe: %s, %.0f m away: details in the b4bcoop log\n", cname(best, c, sizeof c),
               have_me ? sqrtf(bd) / 100.f : -1.f);
}

// ---- watch (discovery): what changes on a ragdolled body over its first seconds ----
// Snapshots of a few components at death; at each sample time, every simple property whose bytes changed is logged
// (old -> new) together with the body's physics state. Finds what the game flips when it freezes a corpse.
typedef struct { UObject *o; int32_t oi; uint8_t *snap; int32_t size; char tag[24]; } Snap;
typedef struct { UObject *actor, *mesh; int32_t ai; double t0; int next; Snap s[4]; } Watch;
static Watch watches[6];
static const float SAMPLE_T[] = {0.25f, 0.5f, 1.f, 1.5f, 2.f, 2.5f, 3.f, 4.f, 6.f, 8.f};
#define N_SAMPLES ((int)(sizeof SAMPLE_T / sizeof SAMPLE_T[0]))

static void snap_take(Snap *sn, UObject *o, const char *tag) {
    memset(sn, 0, sizeof *sn);
    if (!o || !U_CLASS(o)) return;
    int32_t size = US_PROPSSIZE(U_CLASS(o));
    if (size <= 0 || size > 0x10000 || !(sn->snap = malloc(size))) return;
    memcpy(sn->snap, o, size);
    sn->o = o; sn->oi = U_INDEX(o); sn->size = size;
    snprintf(sn->tag, sizeof sn->tag, "%s", tag);
}

static int simple_type(const char *t) {
    return !strcmp(t, "BoolProperty") || !strcmp(t, "ByteProperty") || !strcmp(t, "EnumProperty") ||
           !strcmp(t, "FloatProperty") || !strcmp(t, "IntProperty") || !strcmp(t, "ObjectProperty") || !strcmp(t, "NameProperty");
}

static void fmt_val(const char *t, const uint8_t *a, char *out, size_t n) {
    if (!strcmp(t, "FloatProperty")) snprintf(out, n, "%g", *(const float *)a);
    else if (!strcmp(t, "IntProperty")) snprintf(out, n, "%d", *(const int32_t *)a);
    else if (!strcmp(t, "ObjectProperty")) { char b[96]; snprintf(out, n, "%s", *(UObject *const *)a ? ue_obj_name(*(UObject *const *)a, b, sizeof b) : "None"); }
    else if (!strcmp(t, "NameProperty")) { char b[96]; snprintf(out, n, "%s", ue_name(*(const FName *)a, b, sizeof b)); }
    else snprintf(out, n, "byte %u", *a);
}

static void snap_diff(Snap *sn, const char *who, double dt) {
    if (!sn->o || !ue_object_usable(sn->o, sn->oi)) return;
    char pn[128], t[64], ov[112], nv[112];
    int lines = 0;
    for (UStruct *st = U_CLASS(sn->o); st; st = US_SUPER(st)) {
        for (FField *p = US_CHILDPROPS(st); p; p = FF_NEXT(p)) {
            int32_t off = FP_OFFSET(p), sz = FP_ELSIZE(p);
            if (off < 0 || sz <= 0 || off + sz > sn->size) continue;
            ue_name(*(FName *)FF_CLASS(p), t, sizeof t);
            if (!simple_type(t)) continue;
            uint8_t *now = (uint8_t *)sn->o + off, *was = sn->snap + off;
            if (!memcmp(now, was, sz)) continue;
            ue_name(FF_NAME(p), pn, sizeof pn);
            if (!strcmp(pn, "LastRenderTime") || !strcmp(pn, "LastRenderTimeOnScreen") || strstr(pn, "Time") == pn + strlen(pn) - 4) {
                continue;   // clocks, not state
            }
            fmt_val(t, was, ov, sizeof ov); fmt_val(t, now, nv, sizeof nv);
            // bools share bytes: name every one in a changed byte, with its bit (FBoolProperty ByteMask, +0x7A,
            // assumed from the FProperty layout in NOTES.md: base ends at 0x78) so the right one can be told apart
            if (!strcmp(t, "BoolProperty")) {
                uint8_t mask = *((uint8_t *)p + 0x7A);
                if (!mask || (mask & (mask - 1))) mask = 0xFF;   // not a single bit: the layout guess is wrong, show the byte
                if (!((*now ^ *was) & mask)) continue;   // another bool in this byte changed
                snprintf(ov, sizeof ov, "%d", (*was & mask) != 0); snprintf(nv, sizeof nv, "%d", (*now & mask) != 0);
            }
            if (lines++ < 40) LOG("ragdoll watch %s +%.2fs   %s.%s: %s -> %s", who, dt, sn->tag, pn, ov, nv);
        }
        char b[64];
        ue_obj_name(st, b, sizeof b);
        if (!strcmp(b, "ActorComponent") || !strcmp(b, "Object")) break;
    }
    memcpy(sn->snap, sn->o, sn->size);   // the next sample compares against now
}

static void watch_start(UObject *actor, UObject *mesh) {
    for (int i = 0; i < 6; i++) {
        Watch *w = &watches[i];
        if (w->actor) continue;
        w->actor = actor; w->ai = U_INDEX(actor); w->mesh = mesh; w->t0 = now_s(); w->next = 0;
        snap_take(&w->s[0], ue_get_ptr(actor, "RagdollComponent"), "RagdollComponent");
        snap_take(&w->s[1], call_obj(actor, "GetLifeStateComponent"), "LifeState");
        snap_take(&w->s[2], comp_of(actor, "PhysicalAnimationComponent"), "PhysicalAnimation");
        snap_take(&w->s[3], mesh, "Mesh");
        return;
    }
}

static void watch_end(Watch *w) { for (int k = 0; k < 4; k++) free(w->s[k].snap); memset(w, 0, sizeof *w); }

static void watch_tick(void) {
    char a[160];
    double t = now_s();
    for (int i = 0; i < 6; i++) {
        Watch *w = &watches[i];
        if (!w->actor || t - w->t0 < SAMPLE_T[w->next]) continue;
        if (!ue_object_usable(w->actor, w->ai)) { LOG("ragdoll watch: body gone (despawned) before +%.2fs", SAMPLE_T[w->next]); watch_end(w); continue; }
        oname(w->actor, a, sizeof a);
        int sim = call_bool(w->mesh, "IsSimulatingPhysics"), awake = call_bool(w->mesh, "IsAnyRigidBodyAwake");
        int tick = call_bool(w->mesh, "IsComponentTickEnabled"), rag = call_bool(w->actor, "IsInRagdoll");
        int phys = call_bool(w->actor, "IsInPhysicalizedDeath");
        LOG("ragdoll watch %s +%.2fs: simulating %d, awake %d, mesh tick %d, IsInRagdoll %d, IsInPhysicalizedDeath %d",
            a, t - w->t0, sim, awake, tick, rag, phys);
        for (int k = 0; k < 4; k++) snap_diff(&w->s[k], a, t - w->t0);
        if (++w->next == N_SAMPLES) { LOG("ragdoll watch %s: done", a); watch_end(w); }
    }
}

// ---- corpses ----
static void note_corpse_class(UObject *ridden) {
    char a[160];
    UObject *rc = ue_get_ptr(ridden, "RagdollComponent");
    int32_t off = rc ? ue_prop_offset(rc, "VFXCorpseActorType") : -1;
    UClass *cc = off >= 0 ? *(UClass **)((char *)rc + off) : NULL;
    if (!cc) return;
    for (int k = 0; k < n_corpse_cls; k++) if (corpse_cls[k] == cc) return;
    if (n_corpse_cls < 8) corpse_cls[n_corpse_cls++] = cc;
    LOG("ragdoll: corpse stand-in class for %s: %s", cname(ridden, a, sizeof a), ue_obj_name(cc, a, sizeof a));
}

// ---- tuning (see "tuning" above) ----
// kg, UPrimitiveComponent::GetMass, -1 unknown. On a skeletal mesh it may be every body or the root body only (not
// checked for this engine); either way before/after shows whether the scale took.
static float mesh_mass(UObject *mesh) {
    Call k;
    if (!call_prep(&k, mesh, "GetMass")) return -1;
    call_go(&k);
    float *r = carg(&k, "ReturnValue");
    return r ? *r : -1;
}

static int mass_logs;   // bodies logged with their mass before/after, per /ragdoll mass

static void set_mass_scale(Tracked *t, UObject *mesh, float s) {
    char a[160];
    Call k;
    int log_it = mass_logs < 3 && s != 1.f;
    float before = log_it ? mesh_mass(mesh) : 0;
    if (!call_prep(&k, mesh, "SetAllMassScale")) {
        static int warned;
        if (!warned++) LOG("ragdoll tune: no SetAllMassScale on %s: mass unchanged", cname(mesh, a, sizeof a));
        return;
    }
    SET(&k, "InMassScale", float, s);
    call_go(&k);
    t->mass_set = s != 1.f;
    if (log_it) {
        mass_logs++;
        LOG("ragdoll tune: %s mass x%.2f: %.1f kg -> %.1f kg", oname(t->a, a, sizeof a), s, before, mesh_mass(mesh));
    }
}

// The world's gravity (AWorldSettings.WorldGravityZ, the value in use: the project default unless the map sets its
// own), read once per map from a body's world settings
static float gravity_z(UObject *actor) {
    if (world_gz) return world_gz;
    UObject *ws = call_obj(actor, "GetWorldSettings");
    int32_t off = ws ? ue_prop_offset(ws, "WorldGravityZ") : -1;
    float g = off >= 0 ? *(float *)((char *)ws + off) : 0.f;
    world_gz = g < 0 ? g : -980.f;
    LOG("ragdoll tune: world gravity %.0f cm/s^2 (%s)", world_gz,
        !ws ? "no world settings: Unreal's default" : off < 0 ? "no WorldGravityZ: Unreal's default" :
        g < 0 ? "WorldGravityZ" : "WorldGravityZ not set yet: Unreal's default");
    return world_gz;
}

// every frame for each body the settle hold holds, when gravity isn't x1: the difference to scale x gravity
static void extra_gravity(Tracked *x) {
    Call k;
    if (!call_prep(&k, x->mesh, "AddForceToAllBodiesBelow")) {
        if (!n_grav_fail++) LOG("ragdoll tune: no AddForceToAllBodiesBelow on the mesh: gravity scale has no effect");
        return;
    }
    float *f = carg(&k, "Force");
    if (!f) return;
    f[2] = (grav_scale - 1.f) * gravity_z(x->a);   // BoneName stays None: the root body and every body below it
    SET(&k, "bAccelChange", uint8_t, 1);
    SET(&k, "bIncludeSelf", uint8_t, 1);
    call_go(&k);
}

// ---- the ragdoll ----
static int apply(Tracked *t) {
    char a[160], c[160];
    UObject *mesh = ue_get_ptr(t->a, "Mesh"), *cap = ue_get_ptr(t->a, "CapsuleComponent");
    if (!mesh) { LOG("ragdoll: %s has no mesh", oname(t->a, a, sizeof a)); return -1; }
    int detail = detail_left > 0;
    note_corpse_class(t->a);
    static UClass *kinds[16];   // each kind of Ridden once: its body, and whether the game's corpse path applies to it
    UClass *kc = U_CLASS(t->a);
    int k_seen = 0, k_n = 0;
    for (; k_n < 16 && kinds[k_n]; k_n++) if (kinds[k_n] == kc) k_seen = 1;
    if (!k_seen && k_n < 16) {
        kinds[k_n] = kc;
        UObject *rc0 = ue_get_ptr(t->a, "RagdollComponent");
        LOG("ragdoll kind: %s, RagdollComponent %s", cname(t->a, c, sizeof c), rc0 ? oname(rc0, a, sizeof a) : "NONE");
        log_mesh(mesh, "kind");
    }
    UObject *ai = ue_get_ptr(mesh, "AnimScriptInstance"), *pac = comp_of(t->a, "PhysicalAnimationComponent");
    if (detail) {
        detail_left--;
        LOG("ragdoll: ---- %s (%s), %.3f s after death ----", oname(t->a, a, sizeof a), cname(t->a, c, sizeof c), now_s() - t->died_at);
        log_mesh(mesh, "before");
        LOG("ragdoll before:   montage playing %d, physical animation component %s", ai ? call_bool(ai, "IsAnyMontagePlaying") : -1,
            pac ? oname(pac, a, sizeof a) : "none");
        static int dr_logged;
        UObject *dr = comp_of(t->a, "PlayDeathReactionComponent");
        if (dr && !dr_logged++) list_class(U_CLASS(dr), 0, 1, "death reaction component at death (values)", dr);
    }

    Call k;
    // The game's own death: physics already on, but a death-reaction montage steers the bodies for ~0.5 s (the
    // "forward lunge") until PlayDeathReactionComponent's ragdoll point. Stop every montage at once (no blend out)
    // and give physics the full say.
    if (ai && call_prep(&k, ai, "Montage_Stop")) { SET(&k, "InBlendOutTime", float, 0.f); call_go(&k); }
    if (pac && call_prep(&k, pac, "SetStrengthMultiplyer")) { SET(&k, "InStrengthMultiplyer", float, 0.f); call_go(&k); }
    t->prof = collision_profile(mesh);
    t->cap_coll = 3;
    if (cap && call_prep(&k, cap, "GetCollisionEnabled")) { call_go(&k); uint8_t *r = carg(&k, "ReturnValue"); if (r) t->cap_coll = *r; }
    int32_t lo = ue_prop_offset(mesh, "RelativeLocation"), ro = ue_prop_offset(mesh, "RelativeRotation");
    if (lo >= 0) memcpy(t->rel_loc, (char *)mesh + lo, sizeof t->rel_loc);
    if (ro >= 0) memcpy(t->rel_rot, (char *)mesh + ro, sizeof t->rel_rot);
    t->ragdolled = 1;
    t->mesh = mesh;
    t->rc = ue_get_ptr(t->a, "RagdollComponent");   // the settle check's `this`: the hold finds the body by it
    if (cap && call_prep(&k, cap, "SetCollisionEnabled")) { SET(&k, "NewType", uint8_t, 0); call_go(&k); }   // NoCollision
    static FName ragdoll_profile;
    if (!ragdoll_profile.idx) ragdoll_profile = fname_of(L"Ragdoll");
    if (call_prep(&k, mesh, "SetCollisionProfileName")) { SET(&k, "InCollisionProfileName", FName, ragdoll_profile); call_go(&k); }
    if (call_prep(&k, mesh, "SetAllBodiesSimulatePhysics")) { SET(&k, "bNewSimulate", uint8_t, 1); call_go(&k); }
    if (call_prep(&k, mesh, "SetSimulatePhysics")) { SET(&k, "bSimulate", uint8_t, 1); call_go(&k); }
    if (call_prep(&k, mesh, "SetPhysicsBlendWeight")) { SET(&k, "PhysicsBlendWeight", float, 1.f); call_go(&k); }
    if (mass_scale != 1.f) set_mass_scale(t, mesh, mass_scale);
    call_void(mesh, "WakeAllRigidBodies");

    int sim = call_bool(mesh, "IsSimulatingPhysics");
    if (detail) {
        if (detail_left >= DETAIL_LOGS - 2) watch_start(t->a, mesh);
        log_mesh(mesh, "after");
        LOG("ragdoll after:   montage playing %d", ai ? call_bool(ai, "IsAnyMontagePlaying") : -1);
    }
    if (sim != 1) { LOG("ragdoll: %s is not simulating after the calls", oname(t->a, a, sizeof a)); return -1; }
    return 0;
}

static unsigned tick_n;
static int any_ragdolled(void) { for (int i = 0; i < ntr; i++) if (tr[i].ragdolled) return 1; return 0; }

// The game brought a body we ragdolled back to life (actor reuse): put it back the way it was. The mesh was detached
// from the capsule by the physics, so it is attached again at its old relative place.
static void undo(Tracked *t) {
    char a[160];
    UObject *mesh = ue_get_ptr(t->a, "Mesh"), *cap = ue_get_ptr(t->a, "CapsuleComponent");
    Call k;
    if (mesh) {
        if (call_prep(&k, mesh, "SetSimulatePhysics")) { SET(&k, "bSimulate", uint8_t, 0); call_go(&k); }
        if (call_prep(&k, mesh, "SetAllBodiesSimulatePhysics")) { SET(&k, "bNewSimulate", uint8_t, 0); call_go(&k); }
        if (call_prep(&k, mesh, "SetCollisionProfileName")) { SET(&k, "InCollisionProfileName", FName, t->prof); call_go(&k); }
        if (t->mass_set) set_mass_scale(t, mesh, 1.f);
        if (cap && call_prep(&k, mesh, "K2_AttachToComponent")) {
            SET(&k, "Parent", UObject *, cap);
            SET(&k, "LocationRule", uint8_t, 0); SET(&k, "RotationRule", uint8_t, 0); SET(&k, "ScaleRule", uint8_t, 0);   // KeepRelative
            call_go(&k);
        }
        if (call_prep(&k, mesh, "K2_SetRelativeLocationAndRotation")) {
            float *l = carg(&k, "NewLocation"), *r = carg(&k, "NewRotation");
            if (l) memcpy(l, t->rel_loc, sizeof t->rel_loc);
            if (r) memcpy(r, t->rel_rot, sizeof t->rel_rot);
            call_go(&k);
        }
    }
    if (cap && call_prep(&k, cap, "SetCollisionEnabled")) { SET(&k, "NewType", uint8_t, t->cap_coll); call_go(&k); }
    if (t->ended) {   // the deadline hid it and its destroy was refused (client): show it again
        if (call_prep(&k, t->a, "SetActorHiddenInGame")) { SET(&k, "bNewHidden", uint8_t, 0); call_go(&k); }
        if (call_prep(&k, t->a, "SetActorEnableCollision")) { SET(&k, "bNewActorEnableCollision", uint8_t, 1); call_go(&k); }
    }
    n_revived++;
    LOG("ragdoll: WARNING %s came back alive after its ragdoll (the game reuses Ridden actors): physics undone", oname(t->a, a, sizeof a));
    t->ragdolled = 0; t->done = 0; t->died_at = 0; t->rest = 0; t->released = 0; t->checks = 0; t->spared = 0; t->ended = 0;
}

// ---- settle hold: the game swaps a ragdoll for its stand-in only once the body is at rest ----
// From the code dump of Steam build 14216215 (ragdoll-findings.md, "Build 8"). URagdollComponent (the character's
// RagdollComponent, `this` below) runs both steps as timers on the world's timer manager, handle at +0x268:
//   settle check 0x141B6FD00   looping, every 0.5 s, the first 1-2 s after the physical death (1 s + a random 0-1 s).
//                              The root body's speed is compared with a limit that grows with time since death, from 0
//                              to 500 cm/s at the component's time limits (from the game's settings); 2 checks in a row
//                              under the limit (or both time limits passed) call the swap.
//   swap 0x141B6F030           sets "settled" (+0x1D2), clears the settle timer, spawns the stand-in (VFXCorpse_BP,
//                              a PoseableMesh copy of the pose), hides the real body, and sets a 1.0 s one-shot timer:
//   delete 0x141B6F380         AActor::Destroy on the Ridden.
// The growing limit passes a body still sliding at a few m/s, so ragdolls froze mid-fall. For bodies we ragdolled the
// hook skips the game's check until our own test says the body is at rest (or the cap): every body under the speed
// limit (build 8 tested the pelvis only, which passed a kneeling body whose torso was still tipping over). From then on
// the game's check, swap, stand-in and deletion run unchanged, in the body's final pose. The deletion needs no hook:
// only the swap starts its timer.
#define ADDR_SETTLE_CHECK VA(0x141B6FD00ull)
static const uint8_t SIG_SETTLE[] = {0x40,0x55,0x53,0x57,0x48,0x8d,0x6c,0x24,0xb9,0x48,0x81,0xec,0x90,0x00,0x00,0x00,
                                     0x48,0x8b,0xb9,0xd8,0x00,0x00,0x00,0x48};
#define SETTLE_SPEED   15.f   // cm/s: every body slower than this (the fastest one under it), or all asleep, is at rest
#define SETTLE_CHECKS  2      // settle checks at rest in a row (0.5 s apart) before the game's own check runs
#define SETTLE_CAP     10.0   // s after death: the game's check runs however the body moves
#define SETTLE_LOGS    40     // "hold" lines logged per /ragdoll on (releases and deletions are always logged)
typedef void (*SettleFn)(UObject *ragdoll_component);
static SettleFn orig_settle;
static int settle_state;   // 0 not tried, 1 hooked, -1 signature mismatch or hook failed
static int n_held, n_at_rest, n_capped, n_held_gone, settle_logs;
static double settle_sum, settle_max;

// the mesh's physics asset's bodies, read once per asset (Common Ridden: 3P_Common_PA, 20 bodies)
static const BodyList *bodies_of(UObject *mesh) {
    char a[160];
    UObject *pa = ue_get_ptr(mesh, "PhysicsAssetOverride");
    if (!pa) { UObject *sk = ue_get_ptr(mesh, "SkeletalMesh"); pa = sk ? ue_get_ptr(sk, "PhysicsAsset") : NULL; }
    if (!pa) return NULL;
    for (int i = 0; i < n_body_lists; i++) if (body_lists[i].pa == pa) return body_lists[i].n ? &body_lists[i] : NULL;
    if (n_body_lists == (int)(sizeof body_lists / sizeof body_lists[0])) return NULL;   // 8 kinds of body in one map
    BodyList *bl = &body_lists[n_body_lists++];
    memset(bl, 0, sizeof *bl);
    bl->pa = pa;
    int32_t off = ue_prop_offset(pa, "SkeletalBodySetups");
    TArray *arr = off >= 0 ? (TArray *)((char *)pa + off) : NULL;
    for (int i = 0; arr && i < arr->num && bl->n < MAX_BODIES; i++) {
        UObject *bs = ((UObject **)arr->data)[i];
        int32_t bo = bs ? ue_prop_offset(bs, "BoneName") : -1;
        if (bo >= 0) bl->bone[bl->n++] = *(FName *)((char *)bs + bo);
    }
    LOG("ragdoll settle: physics asset %s: %d bodies in the rest test%s", oname(pa, a, sizeof a), bl->n,
        bl->n ? "" : " (none readable: the root body only)");
    return bl->n ? bl : NULL;
}

// cm/s of the fastest body (bone name into `bone`), -1 if unknown
static float max_speed(Tracked *x, char *bone, size_t n) {
    Call k;
    snprintf(bone, n, "root");
    if (!call_prep(&k, x->mesh, "GetPhysicsLinearVelocity")) return -1;
    FName *bn = carg(&k, "BoneName");
    float *v = carg(&k, "ReturnValue");
    if (!v) return -1;
    int nb = x->bodies && bn ? x->bodies->n : 0;
    float best = -1;
    int at = -1;
    for (int i = nb ? 0 : -1; i < nb; i++) {   // no list: once, BoneName None = the root body
        if (i >= 0) *bn = x->bodies->bone[i];
        call_go(&k);
        float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (s > best) { best = s; at = i; }
    }
    if (at >= 0) ue_name(x->bodies->bone[at], bone, n);
    return best;
}

// 1 = hold (skip the game's check this time), 0 = let it run, from now on for this body
static int settle_hold(Tracked *x) {
    char a[160], bone[64];
    double age = now_s() - x->died_at;
    if (!x->checks) x->bodies = bodies_of(x->mesh);
    float v = max_speed(x, bone, sizeof bone);
    int awake = call_bool(x->mesh, "IsAnyRigidBodyAwake");
    x->last_v = v;
    x->rest = (awake == 0 || (v >= 0 && v < SETTLE_SPEED)) ? x->rest + 1 : 0;
    if (!x->checks++) n_held++;
    const char *why = x->rest >= SETTLE_CHECKS ? "at rest" : age >= SETTLE_CAP ? "time cap" : NULL;
    if (why || settle_logs < SETTLE_LOGS) {
        if (!why) settle_logs++;
        LOG("ragdoll settle %s +%.2fs: fastest %s %.0f cm/s, awake %d -> %s", oname(x->a, a, sizeof a), age, bone, v,
            awake, why ? why : "hold");
    }
    if (!why) return 1;
    x->released = 1;
    if (x->rest >= SETTLE_CHECKS) n_at_rest++; else n_capped++;
    settle_sum += age;
    if (age > settle_max) settle_max = age;
    return 0;
}

static void settle_detour(UObject *rc) {   // game thread: timers run inside the world tick, like ragdoll_tick
    static int first = 1;
    if (first && rc) {
        char a[160], c[160];
        first = 0;
        LOG("ragdoll settle: the game's settle check runs on %s (%s)", oname(rc, a, sizeof a), cname(rc, c, sizeof c));
    }
    Tracked *x = NULL;
    for (int i = 0; rc && i < ntr; i++) if (tr[i].rc == rc) { x = &tr[i]; break; }
    if (x && x->ragdolled && !x->released && x->mesh && settle_hold(x)) return;   // the timer fires again in 0.5 s
    orig_settle(rc);
}

// ---- corpse budget: the game's UCorpseManager may not delete a body the settle hold still holds ----
// From the second code dump (ragdoll-findings.md, "Corpse budget"): UCorpseManager::RegisterCorpse (0x14142FAA0)
// appends each dead Ridden to a list (+0x38, 20-byte entries: weak pointer, registered at, last hit at, flag) and binds
// its OnCorpseHit / OnCorpseDestroyed. Every frame its tick (0x1414306E0, an engine "tickable") removes entries past
// both time limits and then, while the list is longer than the maximum (game settings, per quality level), the
// entries at its front (not by age: removals swap the last entry in). Both go through 0x141430490 (unbind the two
// delegates, then AActor::Destroy), and both drop the entry from the list whatever that function does. A body the
// hold keeps alive longer counts against that maximum for longer, so hordes lost falling bodies (build 9: 30 of 164).
// The hook skips the destroy for bodies still held: the manager's list loses them as usual (its budget stays right
// for the rest), and the game's own swap and 1 s delete end them once they are at rest. Their two delegates stay
// bound; the handlers look the body up in the list, where it no longer is (not seen in code: logged if it comes back).
#define ADDR_CORPSE_DESTROY VA(0x141430490ull)
static const uint8_t SIG_CORPSE_DESTROY[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x18,0x55,0x57,0x41,0x54,
                                             0x41,0x56,0x41,0x57,0x48,0x8b,0xec,0x48,0x83,0xec};
typedef void (*CorpseDestroyFn)(UObject *manager, UObject *corpse, uint8_t net_force);
static CorpseDestroyFn orig_corpse_destroy;
static int corpse_state, n_spared, n_spared_again;

static void corpse_destroy_detour(UObject *manager, UObject *corpse, uint8_t net_force) {   // game thread (its tick)
    Tracked *x = NULL;
    for (int i = 0; corpse && i < ntr; i++) if (tr[i].a == corpse) { x = &tr[i]; break; }
    if (x && x->ragdolled && !x->released) {
        char a[160];
        if (x->spared++) n_spared_again++;   // its entry should be gone after the first time: the game re-added it
        n_spared++;
        if (n_spared <= 40 || x->spared > 1)
            LOG("ragdoll budget: spared %s from the corpse budget %.1f s after death (fastest %.0f cm/s at the last check)%s",
                oname(corpse, a, sizeof a), now_s() - x->died_at, x->last_v, x->spared > 1 ? ", AGAIN: re-registered" : "");
        return;
    }
    orig_corpse_destroy(manager, corpse, net_force);
}

static void corpse_arm(void) {
    if (corpse_state) return;
    if (memcmp((void *)ADDR_CORPSE_DESTROY, SIG_CORPSE_DESTROY, sizeof SIG_CORPSE_DESTROY)) {
        corpse_state = -1;
        LOG("ragdoll budget: signature mismatch at the corpse manager's delete: falling bodies can still be deleted");
        return;
    }
    if (MH_CreateHook((void *)ADDR_CORPSE_DESTROY, (void *)corpse_destroy_detour, (void **)&orig_corpse_destroy) != MH_OK ||
        MH_EnableHook((void *)ADDR_CORPSE_DESTROY) != MH_OK) {
        corpse_state = -1;
        LOG("ragdoll budget: hook failed: falling bodies can still be deleted");
        return;
    }
    corpse_state = 1;
    LOG("ragdoll budget: hooked; the corpse manager no longer deletes bodies the settle hold holds");
}

static void settle_arm(void) {   // game thread, at /ragdoll on; the hook stays (it only acts on bodies we ragdolled)
    if (settle_state) return;
    if (memcmp((void *)ADDR_SETTLE_CHECK, SIG_SETTLE, sizeof SIG_SETTLE)) {
        settle_state = -1;
        LOG("ragdoll settle: signature mismatch at the game's settle check: no hold, bodies freeze at the game's time");
        return;
    }
    if (MH_CreateHook((void *)ADDR_SETTLE_CHECK, (void *)settle_detour, (void **)&orig_settle) != MH_OK ||
        MH_EnableHook((void *)ADDR_SETTLE_CHECK) != MH_OK) {
        settle_state = -1;
        LOG("ragdoll settle: hook failed: no hold, bodies freeze at the game's time");
        return;
    }
    settle_state = 1;
    LOG("ragdoll settle: hooked; at rest = every body under %.0f cm/s or all asleep for %d checks, cap %.0f s",
        SETTLE_SPEED, SETTLE_CHECKS, SETTLE_CAP);
    corpse_arm();   // only with the hold: without it bodies leave the budget the game's way
}

// ---- deadline: every ragdoll is gone a fixed time after death, whatever the game does ----
// The 10 s cap above is only looked at inside the game's settle check, so a body whose RagdollComponent never starts
// that timer (seen: Retches, 16 Oct, never removed at any gravity) was held forever, and so was its extra gravity.
// This one runs in our own tick from the moment of death. At the deadline the body is hidden, its physics stopped
// (no more cost, no jitter) and the actor destroyed, as the game's own delete does (AActor::Destroy). A client may
// not be allowed to destroy a replicated actor; it stays hidden until the host's copy goes. Bodies the game handled
// the normal way (swap + delete about 1-2 s after the release) are long gone by then. The log line says what the
// game had done with the body, to find out why it never got there.
static float deadline_s = 15.f;   // s after death; above SETTLE_CAP plus the game's swap and 1 s delete
static int n_ended, n_ended_unchecked, deadline_logs;

static void end_body(Tracked *x, double age) {
    char a[160], c[160], s[160];
    Call k;
    n_ended++;
    if (!x->checks) n_ended_unchecked++;
    if (deadline_logs < 40) {
        deadline_logs++;
        UObject *rc = x->rc;
        int32_t off = rc ? ue_prop_offset(rc, "VFXCorpseActorType") : -1;
        UClass *sc = off >= 0 ? *(UClass **)((char *)rc + off) : NULL;
        LOG("ragdoll deadline: %s (%s) removed %.1f s after death: %d settle checks of the game seen%s, %s, last fastest "
            "%.0f cm/s, RagdollComponent %s, stand-in class %s, IsInPhysicalizedDeath %d",
            oname(x->a, a, sizeof a), cname(x->a, c, sizeof c), age, x->checks,
            x->checks ? "" : " (the game never started its settle check)",
            x->released ? "released by the hold" : "still held", x->last_v, rc ? "yes" : "NONE",
            sc ? ue_obj_name(sc, s, sizeof s) : "none", call_bool(x->a, "IsInPhysicalizedDeath"));
    }
    x->ended = 1;
    if (!x->released) { x->released = 1; x->rest = 0; }   // the game's own check and the budget may act on it now
    if (call_prep(&k, x->a, "SetActorHiddenInGame")) { SET(&k, "bNewHidden", uint8_t, 1); call_go(&k); }
    if (x->mesh) {
        if (call_prep(&k, x->mesh, "SetAllBodiesSimulatePhysics")) { SET(&k, "bNewSimulate", uint8_t, 0); call_go(&k); }
        if (call_prep(&k, x->mesh, "SetSimulatePhysics")) { SET(&k, "bSimulate", uint8_t, 0); call_go(&k); }
    }
    if (call_prep(&k, x->a, "SetActorEnableCollision")) { SET(&k, "bNewActorEnableCollision", uint8_t, 0); call_go(&k); }
    call_void(x->a, "K2_DestroyActor");
}

void ragdoll_tick(float dt) {
    (void)dt;
    tick_n++;
    if (!mode_on && !tracing && !any_ragdolled()) return;
    if (tracing && now_s() > trace_end) trace_stop("time up");
    if (!world_check()) { for (int i = 0; i < 6; i++) if (watches[i].actor) watch_end(&watches[i]); return; }
    watch_tick();
    scan_step();
    double t = now_s();
    for (int i = 0; i < ntr; i++) {
        Tracked *x = &tr[i];
        if (!ue_object_usable(x->a, x->ai) || !ue_object_usable(x->ls, x->lsi)) {
            // gone before the hold let the game swap it: not the game's swap + delete (on a client: the host's delete?)
            if (x->ragdolled && x->checks && !x->released) {
                // a budget or population cleanup would show as many bodies in flight / many Ridden alive at the time
                int flying = 0, alive = 0;
                for (int k = 0; k < ntr; k++) { flying += tr[k].ragdolled && !tr[k].released; alive += !tr[k].died_at; }
                n_held_gone++;
                LOG("ragdoll settle: a held body was deleted %.1f s after death, before it was at rest (last check: "
                    "fastest %.0f cm/s after %d checks; now %d bodies in flight incl. this one, %d Ridden alive)",
                    t - x->died_at, x->last_v, x->checks, flying, alive);
            }
            tr[i--] = tr[--ntr];
            continue;
        }
        if (x->done) {
            if (x->ragdolled && !x->ended && t - x->died_at >= deadline_s) { end_body(x, t - x->died_at); continue; }
            if (x->ragdolled && !x->released && x->mesh && grav_scale != 1.f && !n_grav_fail) extra_gravity(x);
            if (x->ragdolled && (tick_n % 15) == 0 && call_bool(x->ls, "IsAlive") == 1) undo(x);
            continue;
        }
        if (!x->died_at) {
            if (call_bool(x->ls, "IsAlive") == 1) continue;
            x->died_at = t;
            n_deaths++;
            if (tracing) { note_corpse_class(x->a); trace_on_death(x->a); }   // the stand-in class, for spawn stacks
        }
        if (!mode_on) { x->done = 1; continue; }
        if (t - x->died_at < delay_s) continue;
        x->done = 1;
        if (is_boss(x->a)) continue;
        if (apply(x) == 0) n_applied++; else n_failed++;
    }
}

void ragdoll_slash(char *rest, Out *o) {
    char *verb = rest ? strtok(rest, " ") : NULL, *arg = verb ? strtok(NULL, " ") : NULL;
    if (!verb || !strcmp(verb, "status")) {
        int released = n_at_rest + n_capped;
        out_printf(o, "/ragdoll: %s%s, %d Ridden tracked, %d deaths seen, %d ragdolled, %d failed, %d came back; "
                      "settle hold %s: %d held, %d at rest + %d at the %.0f s cap (avg %.1f s, max %.1f s after death), "
                      "%d deleted while held; corpse budget %s: %d spared (%d again); deadline %.0f s: %d removed "
                      "(%d never got the game's settle check); mass x%.2f, gravity x%.2f%s; %ld stacks logged%s%s\n",
                   mode_on ? "on" : "off", mode_on && delay_s > 0 ? " (after the game's animation)" : "", ntr, n_deaths,
                   n_applied, n_failed, n_revived,
                   settle_state == 1 ? "on" : settle_state ? "OFF (see log)" : "not armed yet", n_held, n_at_rest, n_capped,
                   SETTLE_CAP, released ? settle_sum / released : 0.0, settle_max, n_held_gone,
                   corpse_state == 1 ? "hook on" : corpse_state ? "hook OFF (see log)" : "not armed yet", n_spared,
                   n_spared_again, deadline_s, n_ended, n_ended_unchecked, mass_scale, grav_scale, n_grav_fail ? " (NO EFFECT: see log)" : "", stack_id,
                   tracing ? ", tracing" : "", n_overflow ? ", tracking list full" : "");
    } else if (!strcmp(verb, "on")) {
        float d = arg ? (float)atof(arg) : 0.f;
        delay_s = d < 0 ? 0 : d > 10 ? 10 : d;
        mode_on = 1;
        detail_left = DETAIL_LOGS;
        settle_logs = 0;
        settle_arm();
        LOG("ragdoll: on, delay %.2f s", delay_s);
        out_printf(o, "/ragdoll on: Ridden that die from now on fall as ragdolls%s and stay until they lie still (this machine only)%s\n",
                   delay_s > 0 ? " after the game's death animation starts" : "",
                   settle_state == 1 ? "" : "; settle hold OFF: they freeze at the game's time (see log)");
    } else if (!strcmp(verb, "off")) {
        mode_on = 0;
        LOG("ragdoll: off");
        out_printf(o, "/ragdoll off\n");
    } else if (!strcmp(verb, "mass")) {
        if (arg) {
            float m = (float)atof(arg);
            mass_scale = m < 0.1f ? 0.1f : m > 10.f ? 10.f : m;
            mass_logs = 0;
            LOG("ragdoll tune: mass x%.2f", mass_scale);
        }
        out_printf(o, "/ragdoll mass: x%.2f (0.1-10, default 1) for bodies that die from now on; under gravity alone "
                      "weight changes nothing, it sets how far a push moves the body\n", mass_scale);
    } else if (!strcmp(verb, "gravity")) {
        if (arg) {
            float g = (float)atof(arg);
            grav_scale = g < 0.f ? 0.f : g > 5.f ? 5.f : g;
            LOG("ragdoll tune: gravity x%.2f", grav_scale);
        }
        out_printf(o, "/ragdoll gravity: x%.2f (0-5, default 1) on ragdolls still falling, from now on%s\n", grav_scale,
                   n_grav_fail ? "; NO EFFECT in this game (see log)" : "");
    } else if (!strcmp(verb, "deadline")) {
        if (arg) {
            float s = (float)atof(arg);
            deadline_s = s < 12.f ? 12.f : s > 120.f ? 120.f : s;
            deadline_logs = 0;
            LOG("ragdoll deadline: %.0f s", deadline_s);
        }
        out_printf(o, "/ragdoll deadline: every ragdoll still there %.0f s after death is removed (12-120, default 15)\n",
                   deadline_s);
    } else if (!strcmp(verb, "probe")) {
        world_check();
        if (!mode_on && !tracing) for (int k = 0; k < 64; k++) scan_step();   // not scanning in the background: a full pass now
        cmd_probe(o);
    } else if (!strcmp(verb, "trace")) {
        double s = arg ? atof(arg) : 30;
        if (s < 5) s = 5;
        if (s > 120) s = 120;
        if (tracing) trace_stop("restarted");
        if (trace_start(s, o) == 0) out_printf(o, "/ragdoll trace: %.0f s, kill a few Ridden now (log: \"ragdoll trace\" lines)\n", s);
    } else {
        out_printf(o, "usage: /ragdoll [on [delay]|off|status|mass [x]|gravity [x]|deadline [s]|probe|trace [secs]]\n");
    }
}

int ragdoll_init(void) {
    LOG("ragdoll: experimental module loaded (fork build), off until /ragdoll on");
    return 0;
}
