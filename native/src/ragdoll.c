// Ragdoll deaths for the Ridden (experimental, fork module). Local and cosmetic: every machine ragdolls the bodies it
// draws, nothing is sent over the network, so no protocol change (players without it see the game's own deaths).
//
// Step 1: physics falls instead of the canned death animations. Step 2: the killing hit throws the body (shotgun
// blasts, explosions), from the game's own record of the hit (see "step 2").
//
// Chat command /ragdoll (anyone, host or client; the setting is per machine and not saved yet):
//   /ragdoll                 status
//   /ragdoll on [delay]      ragdoll Ridden that die from now on; delay = seconds of the game's own death animation
//                            first (default 0 = physics at once)
//   /ragdoll off
//   /ragdoll mass [x]        every body's mass x the scale (0.1-10, default 1), for bodies that die from now on
//   /ragdoll gravity [x]     gravity on our ragdolls x the scale (0-5, default 1.3), at once, bodies in flight included
//   /ragdoll deadline [s]    every ragdoll still there s seconds after death is removed (12-120, default 15)
//   /ragdoll push [x]        the killing hit throws the body (step 2): overall strength x (0-10, default 4, off = 0);
//   /ragdoll push <kind> <n> one weapon kind's strength in kg cm/s (HG, SMG, AR, LMG, SG, Sni, FragGrenade, default, ...)
//   /ragdoll bloodbag [on|off] off (default): Ridden an explosion would burst stay bodies (ragdolled, pushed)
//   /ragdoll probe           the nearest living Ridden: its mesh, physics asset, collision, and the death-related
//                            functions/properties of its classes, into the log (discovery). Also (step 2 discovery):
//                            every setting that could push a body (mesh, movement, actor), all its components (hit,
//                            reaction and physics ones in full), and every loaded damage type's impulse settings
//   /ragdoll trace [secs]    log the game's function calls on Ridden (default 30 s): every new class.function once,
//                            plus each death's calls just before and after it (discovery: how the game kills a Ridden).
//                            Also logs native call stacks ("ragdoll stack" lines) of the corpse swap (the stand-in's
//                            construction) and of the deletion (UnPossessed / EndPlay / Destroyed on a dead Ridden).
//                            Calls named like hit/damage/impact/... are logged with their parameters ("ragdoll hit"
//                            on Ridden and corpses, "ragdoll hit-other" elsewhere: weapons, heroes, projectiles)
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
#include <ctype.h>
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
    static struct { const char *name; UClass *c; } cache[32];
    for (int i = 0; i < 32 && cache[i].name; i++) if (!strcmp(cache[i].name, name)) return cache[i].c;
    UClass *c = ue_find_class(name);
    for (int i = 0; i < 32 && c; i++) if (!cache[i].name) { cache[i].name = name; cache[i].c = c; break; }
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
    uint8_t bag;                           // an explosion's destroy (the blood bag) was refused for it
    uint8_t ended;                         // removed by the deadline (hidden, physics off, destroy asked)
    uint8_t fxwho, fxp;                    // build 23 arcs probe: 1 spared, 2 control; probes done (bit per time)
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
#define GRAV_DEFAULT 1.3f   // Joon's pick with the push on (build 17, 16 Oct); 2.5 before the push existed
static float mass_scale = 1.f, grav_scale = GRAV_DEFAULT;
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
typedef struct { UClass *oc; UFunction *fn; int dumps; } Pair;
static Pair pairs[4096];
static int npairs, n_calls;
// Hit discovery: calls whose name says hit/damage/impact... are logged with their parameters ("ragdoll hit" on a
// Ridden or corpse, "ragdoll hit-other" on anything else: the weapon, the hero, a projectile, the game state).
static const char *const HIT_KEYS[] = {"damage", "hit", "impact", "impulse", "react", "shot", "explo", "radial",
                                       "bullet", "projectile", "wound", "dismember", "gib", "stagger", "knock",
                                       "flinch", "force", "momentum", NULL};
#define HIT_LINES        400   // per trace, on Ridden / corpses
#define HIT_PER_FN       40    // per class.function (a calm test: 5 Ridden, a few shots each, killing hits included)
#define OTHER_LINES      200   // per trace, everything else
#define OTHER_PER_FN     6
static Pair other_pairs[1024];
static int n_other_pairs, n_hit_lines, n_other_lines;
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

static void fx_note(UClass *c, UObject *r, UObject *outer);   // build 23, below

static UObject *sco_detour(UClass *c, UObject *outer, FName name, uint32_t fl, uint32_t ifl, UObject *tmpl, uint8_t copy,
                           void *graph, uint8_t arch) {
    UObject *r = orig_sco(c, outer, name, fl, ifl, tmpl, copy, graph, arch);
    if (tracing && c && r && !(fl & LIVE_FLAGS) && is_corpse_class(c) && !own_call()) {
        char a[160], b[160], w[360];
        snprintf(w, sizeof w, "%s constructed (%s)", oname(r, a, sizeof a), oname(c, b, sizeof b));
        stack_log(ST_SPAWN, w);
    }
    if (tracing && c && r && !(fl & LIVE_FLAGS) && GetCurrentThreadId() == game_tid && !own_call()) fx_note(c, r, outer);
    return r;
}

static Tracked *dead_tracked(UObject *a) {   // game thread only: tr[] belongs to it
    for (int i = 0; i < ntr; i++) if (tr[i].a == a) return tr[i].died_at ? &tr[i] : NULL;
    return NULL;
}
static double last_blast_t(void);
static Tracked *any_tracked(UObject *a) {   // also the ones not yet seen dead (game thread only)
    for (int i = 0; i < ntr; i++) if (tr[i].a == a) return &tr[i];
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
    // Ridden we saw die, and (build 20) Ridden ended before we ever saw them dead: an explosion's blood bag destroys
    // them in the frame they die, so our tick never does; the stack names the code that does it
    Tracked *x = any_tracked(actor);
    // a ragdoll the settle hold released goes the known way (swap, delete 1 s later): only the others are of interest
    if (!x || x->released || (x->stacked & (1u << kind))) return;
    x->stacked |= 1u << kind;
    // build 22 log: the director despawns living Ridden far away the same way; those used up the stack budget before
    // any explosion. A blood bag is dead when it's destroyed: a Ridden still alive here is not one.
    if (!x->died_at && x->ls && ue_object_usable(x->ls, x->lsi) && call_bool(x->ls, "IsAlive") == 1) return;
    double lb = last_blast_t();
    char since[48];
    if (lb > 0) snprintf(since, sizeof since, "%.3f s after the last explosion", now_s() - lb);
    else snprintf(since, sizeof since, "no explosion yet");
    int n = x->died_at
        ? snprintf(w, sizeof w, "%s.%s on %s, %.2f s after death, %s", cname(o, a, sizeof a), ue_obj_name(fn, b, sizeof b),
                   oname(actor, c, sizeof c), now_s() - x->died_at, x->ragdolled ? "ragdolled" : "game's own death")
        : snprintf(w, sizeof w, "%s.%s on %s, NOT YET SEEN DEAD (a blood bag?), %s",
                   cname(o, a, sizeof a), ue_obj_name(fn, b, sizeof b), oname(actor, c, sizeof c), since);
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

// ---- hit discovery (build 14): does the game push bodies when it hits them, and what does a hit carry? ----
// Values of any reflected property, for function parameters (trace) and settings (probe). Layouts after FProperty
// (0x78 here, as tools/sdkdump.py reads subtypes at 0x78): StructProperty / ObjectProperty: UScriptStruct / class at
// 0x78; BoolProperty (stock 4.25 order): FieldSize 0x78, ByteOffset 0x79, ByteMask 0x7A, FieldMask 0x7B. A bool whose
// bytes don't look like that is printed raw with them, so a wrong guess shows in the log instead of a wrong value.
#define FP_SUB(f)       (*(UObject **)((char *)(f) + 0x78))
#define FBP_SIZE(f)     (*((uint8_t *)(f) + 0x78))
#define FBP_BYTEOFF(f)  (*((uint8_t *)(f) + 0x79))
#define FBP_MASK(f)     (*((uint8_t *)(f) + 0x7B))
#define CPF_PARM_       0x80ull
#define CPF_OUTPARM_    0x100ull
#define CPF_RETURNPARM_ 0x400ull
#define CPF_CONSTPARM_  0x2ull
static int name_has(const char *s, const char *const *keys);

static int bool_at(FField *p, const uint8_t *container) {   // -1: not a layout we know
    if (FBP_SIZE(p) != 1 || !FBP_MASK(p)) return -1;
    return (container[FP_OFFSET(p) + FBP_BYTEOFF(p)] & FBP_MASK(p)) != 0;
}

static void fmt_prop(FField *p, const uint8_t *container, char *out, size_t n, int depth) {
    char t[64], b[128];
    ue_name(*(FName *)FF_CLASS(p), t, sizeof t);
    const uint8_t *a = container + FP_OFFSET(p);
    out[0] = 0;
    if (!strcmp(t, "BoolProperty")) {
        int v = bool_at(p, container);
        if (v >= 0) snprintf(out, n, "%d", v);
        else snprintf(out, n, "byte %u (bool bytes %u/%u/%u?)", *a, FBP_SIZE(p), FBP_BYTEOFF(p), FBP_MASK(p));
    } else if (!strcmp(t, "FloatProperty")) snprintf(out, n, "%g", *(const float *)a);
    else if (!strcmp(t, "DoubleProperty")) snprintf(out, n, "%g", *(const double *)a);
    else if (!strcmp(t, "IntProperty")) snprintf(out, n, "%d", *(const int32_t *)a);
    else if (!strcmp(t, "Int64Property")) snprintf(out, n, "%lld", (long long)*(const int64_t *)a);
    else if (!strcmp(t, "ByteProperty") || !strcmp(t, "EnumProperty")) snprintf(out, n, "%u", *a);
    else if (!strcmp(t, "NameProperty")) snprintf(out, n, "%s", ue_name(*(const FName *)a, b, sizeof b));
    else if (!strcmp(t, "ObjectProperty") || !strcmp(t, "ClassProperty")) {
        UObject *o = *(UObject *const *)a;
        if (!o) snprintf(out, n, "None");
        else { char c[96]; snprintf(out, n, "%s (%s)", ue_obj_name(o, b, sizeof b), cname(o, c, sizeof c)); }
    } else if (!strcmp(t, "WeakObjectProperty")) {
        UObject *o = ue_weak_get(a);
        snprintf(out, n, "%s", o ? ue_obj_name(o, b, sizeof b) : "None");
    } else if (!strcmp(t, "StrProperty")) {
        const FString *s = (const FString *)a;
        size_t k = 0;
        for (int i = 0; s->data && i < s->num && s->data[i] && k + 2 < n && k < 80; i++)
            out[k++] = (s->data[i] >= 32 && s->data[i] < 127) ? (char)s->data[i] : '?';
        out[k] = 0;
    } else if (!strcmp(t, "ArrayProperty")) snprintf(out, n, "%d entries", ((const TArray *)a)->num);
    else if (!strcmp(t, "StructProperty")) {
        UObject *ss = FP_SUB(p);
        ue_obj_name(ss, b, sizeof b);
        // Vector, Vector_NetQuantize*, Rotator: 3 floats. Other structs: their fields, one level deep.
        if (!strncmp(b, "Vector", 6) && strcmp(b, "Vector2D") && strcmp(b, "Vector4"))
            snprintf(out, n, "(%.1f %.1f %.1f)", ((const float *)a)[0], ((const float *)a)[1], ((const float *)a)[2]);
        else if (!strcmp(b, "Rotator"))
            snprintf(out, n, "(p %.1f y %.1f r %.1f)", ((const float *)a)[0], ((const float *)a)[1], ((const float *)a)[2]);
        else if (depth <= 0) snprintf(out, n, "<%s>", b);
        else {
            size_t k = (size_t)snprintf(out, n, "%s{", b);
            for (UStruct *st = (UStruct *)ss; st && k + 8 < n; st = US_SUPER(st))
                for (FField *f = US_CHILDPROPS(st); f && k + 8 < n; f = FF_NEXT(f)) {
                    char fn[64], fv[160];
                    fmt_prop(f, a, fv, sizeof fv, depth - 1);
                    k += (size_t)snprintf(out + k, n - k, "%s=%s ", ue_name(FF_NAME(f), fn, sizeof fn), fv);
                }
            if (k + 2 < n) { if (out[k - 1] == ' ') k--; out[k++] = '}'; out[k] = 0; }
        }
    } else snprintf(out, n, "<%s>", t);
}

// One line per call: every input parameter with its value. Out parameters aren't filled in yet when the call starts
// (and may hold garbage pointers), so only their names are listed.
static void log_params(const char *tag, double t, UObject *actor, UObject *o, UFunction *fn, const uint8_t *params) {
    char line[3072], ab[128], cb[128], fb[128], pn[64], pv[1024];
    size_t k = (size_t)snprintf(line, sizeof line, "ragdoll %s +%.3fs %s %s.%s:", tag, t, actor ? ue_obj_name(actor, ab, sizeof ab) : "-",
                                cname(o, cb, sizeof cb), ue_obj_name(fn, fb, sizeof fb));
    for (FField *p = US_CHILDPROPS(fn); p && k + 16 < sizeof line; p = FF_NEXT(p)) {
        uint64_t fl = FP_FLAGS(p);
        if (!(fl & CPF_PARM_) || (fl & CPF_RETURNPARM_)) continue;
        ue_name(FF_NAME(p), pn, sizeof pn);
        // const T& inputs (e.g. a const FHitResult&) carry OutParm + ConstParm and are real inputs; other out
        // parameters (pure outs, non-const refs) are results, not filled in yet: name only.
        if ((fl & CPF_OUTPARM_) && !(fl & CPF_CONSTPARM_)) { k += (size_t)snprintf(line + k, sizeof line - k, " %s=(out)", pn); continue; }
        if (!params) { k += (size_t)snprintf(line + k, sizeof line - k, " %s=?", pn); continue; }
        fmt_prop(p, params, pv, sizeof pv, 1);
        k += (size_t)snprintf(line + k, sizeof line - k, " %s=%s", pn, pv);
    }
    LOG("%s", line);
}

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
    // build 23: + the facial animation and anim-graph calls every frame (60% of build 22's log, every dead body for 8 s)
    return !strncmp(fn, "ReceiveTick", 11) || !strncmp(fn, "BlueprintUpdateAnimation", 24) ||
           !strncmp(fn, "BlueprintThreadSafe", 19) || !strncmp(fn, "BlueprintPostEvaluate", 21) ||
           !strcmp(fn, "GetCurves") || !strcmp(fn, "GetBindingName") || !strncmp(fn, "EvaluateGraphExposedInputs", 26);
}

static void trace_other(UObject *o, UObject *actor, UFunction *fn, void *params) {   // not on a Ridden or corpse
    if (n_other_lines >= OTHER_LINES) return;
    char fb[128];
    ue_obj_name(fn, fb, sizeof fb);
    if (per_frame(fb) || !name_has(fb, HIT_KEYS)) return;
    UClass *oc = U_CLASS(o);
    EnterCriticalSection(&tcs);
    Pair *pr = NULL;
    for (int i = 0; i < n_other_pairs; i++) if (other_pairs[i].oc == oc && other_pairs[i].fn == fn) { pr = &other_pairs[i]; break; }
    if (!pr && n_other_pairs < (int)(sizeof other_pairs / sizeof other_pairs[0])) { pr = &other_pairs[n_other_pairs++]; *pr = (Pair){oc, fn, 0}; }
    if (pr && pr->dumps < OTHER_PER_FN && n_other_lines < OTHER_LINES) {
        pr->dumps++; n_other_lines++;
        log_params("hit-other", now_s() - trace_t0, actor, o, fn, params);
    }
    LeaveCriticalSection(&tcs);
}

static void trace_call(UObject *o, UFunction *fn, void *params) {
    UObject *actor = owner_actor(o);
    if (!actor || !(is_ridden_fast(actor) || (n_corpse_cls && is_corpse(actor)))) { trace_other(o, actor, fn, params); return; }
    stack_check(o, actor, fn, params);
    double t = now_s();
    char cb[128], fb[128], ab[128];
    EnterCriticalSection(&tcs);
    n_calls++;
    UClass *oc = U_CLASS(o);
    ring[ring_at] = (RingE){t, actor, oc, fn};
    ring_at = (ring_at + 1) % (int)(sizeof ring / sizeof ring[0]);
    Pair *pr = NULL;
    for (int i = 0; i < npairs; i++) if (pairs[i].oc == oc && pairs[i].fn == fn) { pr = &pairs[i]; break; }
    if (!pr && npairs < (int)(sizeof pairs / sizeof pairs[0])) {
        pr = &pairs[npairs++];
        *pr = (Pair){oc, fn, 0};
        LOG("ragdoll trace +%.2fs new %s.%s (on %s)", t - trace_t0, ue_obj_name(oc, cb, sizeof cb),
            ue_obj_name(fn, fb, sizeof fb), ue_obj_name(actor, ab, sizeof ab));
    }
    if (pr && pr->dumps < HIT_PER_FN && n_hit_lines < HIT_LINES) {
        ue_obj_name(fn, fb, sizeof fb);
        if (!per_frame(fb) && name_has(fb, HIT_KEYS)) { pr->dumps++; n_hit_lines++; log_params("hit", t - trace_t0, actor, o, fn, params); }
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

// ---- blast centres (for the blast push) ----
// An explosion's ExplosionComponent.Multicast_ExplodeFromSeed(Seed, PhysMat, VFXCharacters, VFXCharacterLocations,
// NumJuiced, LocationOverride) is its effects, sent to every machine after the damage. LocationOverride = the blast
// point (checked against a frag's ImpactPoint - ImpactDirection in build 14). The hit record's source is often the
// thrower, so blast pushes aim from the nearest recorded blast. The call is left as it is (builds 19-20 edited NumJuiced
// and the character lists to stop the blood bags: no effect on them, see "blood bags" below).
static UFunction *explode_fn;
static FField *blast_loc_f;
typedef struct { double t; float at[3]; } Blast;
static Blast blasts[8];
static int blast_at;
static double last_blast_t(void) { double t = 0; for (int i = 0; i < 8; i++) if (blasts[i].t > t) t = blasts[i].t; return t ? t : -1e9; }

// ---- what an explosion creates (discovery, build 23, while a trace runs) ----
// The red arcs on spared blood bags were no effect component of theirs at death (build 22: only an inactive
// CharacterEmitter). So: every object of an effect-like class (particles, decals, gore, gibs, dynamic materials...)
// constructed within 0.5 s before or 3 s after an explosion's effects call is logged, with what it was made in. The
// "before" ones matter: the blood bag code runs before that call, in the same frame (build 20 stacks), so they wait
// in a ring until the call comes. Gunfire's own decals and impacts outside those windows are never logged.
#define FX_NEW_LOGS 100
typedef struct { double t; char what[232]; } FxNew;
static FxNew fx_ring[32];
static int fx_ring_at, fx_new_logs;

static void fx_note(UClass *c, UObject *r, UObject *outer) {   // game thread, from the construction hook
    static const char *const NEW_KEYS[] = {"particle", "niagara", "fx", "decal", "emitter", "blood", "gore", "gib",
                                           "juice", "mutil", "limb", "dismember", "sever", "chunk",
                                           "materialinstancedynamic", NULL};
    if (fx_new_logs >= FX_NEW_LOGS) return;
    char cn[96], a[96], b[96], d[64], e[96];
    if (!name_has(ue_obj_name(c, cn, sizeof cn), NEW_KEYS)) return;
    UObject *own = outer ? owner_actor(outer) : NULL;
    double t = now_s(), lb = last_blast_t();
    char what[232];
    snprintf(what, sizeof what, "%s (%s) constructed in %s (%s), owner %s", oname(r, a, sizeof a), cn, oname(outer, b, sizeof b),
             cname(outer, d, sizeof d), oname(own, e, sizeof e));
    if (lb > 0 && t - lb < 3.0) {
        fx_new_logs++;
        LOG("ragdoll fx: +%.3fs after the explosion: %s", t - lb, what);
        return;
    }
    FxNew *f = &fx_ring[fx_ring_at++ % 32];
    f->t = t;
    memcpy(f->what, what, sizeof what);
}

static void fx_flush(double blast_t) {   // game thread: an explosion's effects call just came; its frame's objects
    for (int k = 0; k < 32; k++) {
        FxNew *f = &fx_ring[(fx_ring_at + k) % 32];   // oldest first
        if (!f->t || blast_t - f->t > 0.5 || f->t > blast_t) { f->t = 0; continue; }
        if (fx_new_logs < FX_NEW_LOGS) { fx_new_logs++; LOG("ragdoll fx: -%.3fs before the explosion's effects call: %s", blast_t - f->t, f->what); }
        f->t = 0;
    }
}
static int juice_state;   // blast lookup: 0 not looked up, 1 ok, -1 missing

static void juice_resolve(void) {
    char t[64], sn[64];
    if (juice_state) return;
    juice_state = -1;
    UClass *ec = cls("ExplosionComponent");
    UFunction *fn = ec ? ue_find_function(ec, "Multicast_ExplodeFromSeed") : NULL;
    FField *lf = fn ? ue_find_prop(fn, "LocationOverride") : NULL;
    if (!lf || strcmp(ue_name(*(FName *)FF_CLASS(lf), t, sizeof t), "StructProperty") || strncmp(ue_obj_name(FP_SUB(lf), sn, sizeof sn), "Vector", 6)) {
        LOG("ragdoll push: no ExplosionComponent.Multicast_ExplodeFromSeed(LocationOverride): blast pushes aim from the hit's source");
        return;
    }
    explode_fn = fn;
    blast_loc_f = lf;
    juice_state = 1;
}

// ---- blood bags: explosions keep the bodies (Joon, 16 Oct) ----
// The blood bag is not the effects call: build 20's traces show every one ending in the server's explosion code,
// before the effects are sent: BaseProjectile.OnHitCallback -> ExplosionComponent.Explode (exec 0x142060280) ->
// 0x141756980 +0x305 -> 0x141756C90 +0xF4C -> AActor::Destroy (0x143A00C10) on the Ridden, in the frame it dies. A hook
// on AActor::Destroy refuses exactly that call (its return address) for Ridden (not bosses) while /ragdoll is on and
// /ragdoll bloodbag is off: the body stays, our tick sees it dead and ragdolls and pushes it like any blast victim.
// Every other destroy passes untouched. The bytes of both functions and the call itself are checked before hooking.
#define ADDR_ACTOR_DESTROY  VA(0x143A00C10ull)
#define ADDR_JUICE_FN       VA(0x141756C90ull)
#define ADDR_JUICE_RET      VA(0x141757BDCull)   // 0x141756C90 +0xF4C: right after its call to AActor::Destroy
static const uint8_t SIG_ACTOR_DESTROY[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x40,0x33,
                                            0xff,0x0f,0xb6,0xf2,0xf6,0x41,0x64,0x04};
static const uint8_t SIG_JUICE_FN[] = {0x40,0x55,0x53,0x56,0x48,0x8d,0xac,0x24,0x70,0xfd,0xff,0xff,0x48,0x81,0xec,0x90,
                                       0x03,0x00,0x00,0x0f,0x29,0xb4,0x24,0x70};
typedef uint8_t (*ActorDestroyFn)(UObject *actor, uint8_t net_force, uint8_t modify_level);
static ActorDestroyFn orig_actor_destroy;
static int bloodbag_on;     // 1 = the game's blood bags
static int bag_state;       // 0 not tried, 1 hooked, -1 bytes don't match or hook failed
static int n_bags_spared, bag_logs;
static Tracked *any_tracked(UObject *a);

static uint8_t actor_destroy_detour(UObject *actor, uint8_t net_force, uint8_t modify_level) {   // game thread
    if ((uintptr_t)__builtin_return_address(0) == ADDR_JUICE_RET && mode_on && !bloodbag_on && actor &&
        is_ridden_fast(actor) && !is_boss(actor)) {
        n_bags_spared++;
        Tracked *x = any_tracked(actor);
        if (x) x->bag = 1;
        if (bag_logs < 20) {
            char a[160], c[160];
            bag_logs++;
            LOG("ragdoll bloodbag: %s (%s): the explosion's destroy refused, the body stays%s", oname(actor, a, sizeof a),
                cname(actor, c, sizeof c), x ? "" : " (not in our list: it won't be ragdolled)");
        }
        return 0;
    }
    return orig_actor_destroy(actor, net_force, modify_level);
}

static void bag_arm(void) {   // game thread, at /ragdoll on; the hook stays (it acts only while on and blood bags off)
    if (bag_state) return;
    bag_state = -1;
    const uint8_t *call = (const uint8_t *)(ADDR_JUICE_RET - 5);
    uintptr_t target = call[0] == 0xE8 ? ADDR_JUICE_RET + (uintptr_t)(intptr_t)*(const int32_t *)(call + 1) : 0;
    if (memcmp((void *)ADDR_ACTOR_DESTROY, SIG_ACTOR_DESTROY, sizeof SIG_ACTOR_DESTROY) ||
        memcmp((void *)ADDR_JUICE_FN, SIG_JUICE_FN, sizeof SIG_JUICE_FN) || target != ADDR_ACTOR_DESTROY) {
        LOG("ragdoll bloodbag: game code differs (AActor::Destroy bytes %s, explosion code bytes %s, the call %s): blood bags stay",
            memcmp((void *)ADDR_ACTOR_DESTROY, SIG_ACTOR_DESTROY, sizeof SIG_ACTOR_DESTROY) ? "differ" : "ok",
            memcmp((void *)ADDR_JUICE_FN, SIG_JUICE_FN, sizeof SIG_JUICE_FN) ? "differ" : "ok",
            target == ADDR_ACTOR_DESTROY ? "ok" : "not a call to AActor::Destroy");
        return;
    }
    if (MH_CreateHook((void *)ADDR_ACTOR_DESTROY, (void *)actor_destroy_detour, (void **)&orig_actor_destroy) != MH_OK ||
        MH_EnableHook((void *)ADDR_ACTOR_DESTROY) != MH_OK) {
        LOG("ragdoll bloodbag: hook on AActor::Destroy failed: blood bags stay");
        return;
    }
    bag_state = 1;
    LOG("ragdoll bloodbag: hooked AActor::Destroy; the explosion code's call (return 0x141757BDC) is refused for Ridden");
}

// A spared blood bag may have been hidden by the explosion code before its destroy: shown again before the ragdoll.
static void bag_unhide(Tracked *t, UObject *mesh) {
    char a[160];
    Call k;
    int vis = call_bool(mesh, "IsVisible");
    int32_t ho = ue_prop_offset(t->a, "bHidden");
    FField *hf = ho >= 0 ? ue_find_prop(U_CLASS(t->a), "bHidden") : NULL;
    int hidden = hf ? bool_at(hf, (const uint8_t *)t->a) : -1;
    if (call_prep(&k, t->a, "SetActorHiddenInGame")) { SET(&k, "bNewHidden", uint8_t, 0); call_go(&k); }
    if (call_prep(&k, mesh, "SetHiddenInGame")) { SET(&k, "NewHidden", uint8_t, 0); SET(&k, "bPropagateToChildren", uint8_t, 1); call_go(&k); }
    if (call_prep(&k, mesh, "SetVisibility")) { SET(&k, "bNewVisibility", uint8_t, 1); SET(&k, "bPropagateToChildren", uint8_t, 1); call_go(&k); }
    if (bag_logs < 40) {
        bag_logs++;
        LOG("ragdoll bloodbag: %s spared: actor hidden %d, mesh visible %d -> %d (shown before the ragdoll)", oname(t->a, a, sizeof a),
            hidden, vis, call_bool(mesh, "IsVisible"));
    }
}

// ---- the red arcs (discovery, build 23) ----
// Build 22: a spared body's only effect component at death is an inactive CharacterEmitter, no asset: the arcs are
// not a particle on the body at that moment. Seen until the swap to the stand-in (which has the mesh's own look), so
// they belong to the body: an effect made a moment later, or its look (materials, morph targets: a body swelling up
// before it bursts would draw as red spheres). Probed at death, +0.5 s and +2 s on the first 3 spared bodies and, to
// compare, the first 2 bodies that die near an explosion without being blood bags: effect components, the mesh's
// attached children (whoever owns them), its materials with their parameters, its morph targets, its scale.
static const float FXP_T[] = {0.f, 0.5f, 2.f};
static int fxp_spared, fxp_ctrl;

static int call_int(UObject *o, const char *fn) {
    Call c; if (!call_prep(&c, o, fn)) return -1;
    call_go(&c);
    int32_t *r = carg(&c, "ReturnValue");
    return r ? *r : -1;
}

static void log_array(const char *who, const char *what, UObject *o, const char *prop, int max) {   // array of structs
    FField *ap = o ? ue_find_prop(U_CLASS(o), prop) : NULL;
    if (!ap) return;
    FField *inner = *(FField **)((char *)ap + 0x78);   // FArrayProperty::Inner
    const TArray *arr = (const TArray *)((const uint8_t *)o + FP_OFFSET(ap));
    if (!inner || !arr) return;
    LOG("ragdoll fxprobe %s:   %s %s: %d", who, what, prop, arr->num);
    int32_t el = FP_ELSIZE(inner);
    for (int i = 0; arr->data && i < arr->num && i < max && el > 0; i++) {
        char v[600];
        fmt_prop(inner, (const uint8_t *)arr->data + (size_t)i * el, v, sizeof v, 2);
        LOG("ragdoll fxprobe %s:     [%d] %s", who, i, v);
    }
}

static void fx_probe(Tracked *t, double dt) {   // game thread
    char who[200], a[160], b[160], c[160];
    snprintf(who, sizeof who, "%s +%.1fs (%s)", oname(t->a, a, sizeof a), dt,
             t->fxwho == 1 ? "spared blood bag" : "near-blast death, not a blood bag");
    UObject *mesh = t->mesh;
    Call k;
    // effect components on the actor, as build 22 did, now also later
    static const char *const FX_KEYS[] = {"particle", "niagara", "fx", "decal", "emitter", "blood", "gore", "ribbon", "cable", NULL};
    UClass *ac = cls("ActorComponent");
    if (ac && call_prep(&k, t->a, "K2_GetComponentsByClass")) {
        SET(&k, "ComponentClass", UClass *, ac);
        call_go(&k);
        TArray *r = carg(&k, "ReturnValue");
        int n = 0;
        for (int i = 0; r && r->data && i < r->num && i < 128; i++) {
            UObject *comp = ((UObject **)r->data)[i];
            char cn[128];
            if (!comp || !name_has(cname(comp, cn, sizeof cn), FX_KEYS)) continue;
            UObject *asset = ue_get_ptr(comp, "Template");
            if (!asset) asset = ue_get_ptr(comp, "Asset");
            if (!asset) asset = ue_get_ptr(comp, "DecalMaterial");
            n++;
            LOG("ragdoll fxprobe %s: effect %s (%s), asset %s, active %d, visible %d", who, oname(comp, b, sizeof b), cn,
                oname(asset, c, sizeof c), call_bool(comp, "IsActive"), call_bool(comp, "IsVisible"));
        }
        LOG("ragdoll fxprobe %s: %d effect components of %d components", who, n, r ? r->num : -1);
    }
    if (!mesh) return;
    // whatever is attached to the mesh, also things other actors own (an effect spawned attached to a bone)
    FField *chp = ue_find_prop(U_CLASS(mesh), "AttachChildren");
    if (chp) {
        const TArray *ch = (const TArray *)((const uint8_t *)mesh + FP_OFFSET(chp));
        LOG("ragdoll fxprobe %s: mesh children: %d", who, ch->num);
        for (int i = 0; ch->data && i < ch->num && i < 24; i++) {
            UObject *x = ((UObject **)ch->data)[i];
            if (!x) continue;
            UObject *own = COMP_OWNER(x);
            LOG("ragdoll fxprobe %s:   child %s (%s), owner %s, visible %d", who, oname(x, a, sizeof a), cname(x, b, sizeof b),
                oname(own, c, sizeof c), call_bool(x, "IsVisible"));
        }
    }
    // materials: a dynamic one, or a swapped one, with its parameters
    int nm = call_int(mesh, "GetNumMaterials");
    FField *ovp = ue_find_prop(U_CLASS(mesh), "OverrideMaterials");
    LOG("ragdoll fxprobe %s: %d materials, %d overrides", who, nm,
        ovp ? ((const TArray *)((const uint8_t *)mesh + FP_OFFSET(ovp)))->num : -1);
    for (int i = 0; i < nm && i < 12; i++) {
        if (!call_prep(&k, mesh, "GetMaterial")) break;
        SET(&k, "ElementIndex", int32_t, i);
        call_go(&k);
        UObject **rm = carg(&k, "ReturnValue");
        UObject *m = rm ? *rm : NULL;
        if (!m) { LOG("ragdoll fxprobe %s:   material %d: none", who, i); continue; }
        UObject *par = ue_find_prop(U_CLASS(m), "Parent") ? ue_get_ptr(m, "Parent") : NULL;
        LOG("ragdoll fxprobe %s:   material %d: %s (%s), parent %s", who, i, oname(m, a, sizeof a), cname(m, b, sizeof b),
            oname(par, c, sizeof c));
        if (cname(m, b, sizeof b) && strstr(b, "Dynamic")) {   // its live parameters
            char what[32];
            snprintf(what, sizeof what, "material %d", i);
            log_array(who, what, m, "ScalarParameterValues", 16);
            log_array(who, what, m, "VectorParameterValues", 16);
        }
    }
    // morph targets with their weights now
    UObject *skm = ue_get_ptr(mesh, "SkeletalMesh");
    FField *mtp = skm ? ue_find_prop(U_CLASS(skm), "MorphTargets") : NULL;
    if (mtp) {
        const TArray *mt = (const TArray *)((const uint8_t *)skm + FP_OFFSET(mtp));
        int nonzero = 0;
        for (int i = 0; mt->data && i < mt->num && i < 64; i++) {
            UObject *m = ((UObject **)mt->data)[i];
            if (!m || !call_prep(&k, mesh, "GetMorphTarget")) continue;
            wchar_t wn[96];
            ue_obj_name(m, a, sizeof a);
            size_t j = 0;
            for (; a[j] && j + 1 < 96; j++) wn[j] = (wchar_t)(unsigned char)a[j];
            wn[j] = 0;
            SET(&k, "MorphTargetName", FName, fname_of(wn));
            call_go(&k);
            float *w = carg(&k, "ReturnValue");
            if (w && *w != 0.f) { nonzero++; LOG("ragdoll fxprobe %s:   morph target %s = %g", who, a, *w); }
        }
        LOG("ragdoll fxprobe %s: %d morph targets, %d not 0", who, mt->num, nonzero);
    } else LOG("ragdoll fxprobe %s: no morph target list (skeletal mesh %s)", who, oname(skm, a, sizeof a));
    // its size (a body blown up would show here)
    if (call_prep(&k, mesh, "K2_GetComponentScale") || call_prep(&k, mesh, "GetComponentScale")) {
        call_go(&k);
        float *sc = carg(&k, "ReturnValue");
        if (sc) LOG("ragdoll fxprobe %s: mesh scale (%.2f %.2f %.2f)", who, sc[0], sc[1], sc[2]);
    }
}

static void fx_probe_mark(Tracked *t) {   // from apply(): which bodies get the probe
    if (t->bag) { if (fxp_spared < 3) { fxp_spared++; t->fxwho = 1; } }
    else if (fxp_ctrl < 2 && last_blast_t() > 0 && now_s() - last_blast_t() < 1.0) { fxp_ctrl++; t->fxwho = 2; }
}

static void fx_probe_tick(Tracked *t, double now) {   // from the tick, bodies we ragdolled
    for (int i = 0; i < (int)(sizeof FXP_T / sizeof FXP_T[0]); i++) {
        if ((t->fxp & (1u << i)) || now - t->died_at < FXP_T[i]) continue;
        t->fxp |= 1u << i;
        fx_probe(t, now - t->died_at);
    }
}

static void pe_detour(UObject *o, UFunction *fn, void *params) {
    if (fn == explode_fn && explode_fn && params && mode_on && blast_loc_f) {   // game thread
        Blast *b = &blasts[blast_at++ % 8];
        b->t = now_s();
        memcpy(b->at, (uint8_t *)params + FP_OFFSET(blast_loc_f), sizeof b->at);
        if (tracing) fx_flush(b->t);
    }
    if (tracing && o && fn && !own_call()) trace_call(o, fn, params);
    orig_pe(o, fn, params);
}

// The ProcessEvent hook is on while a trace runs or while the ragdoll is on (one pointer compare per call).
static int pe_enabled;
static int pe_update(void) {
    juice_resolve();
    int want = tracing || (mode_on && explode_fn);   // with the ragdoll on: blast centres
    if (want && !hook_made) {
        InitializeCriticalSection(&tcs);
        if (MH_CreateHook((void *)ADDR_PROCESSEVENT, (void *)pe_detour, (void **)&orig_pe) != MH_OK) {
            LOG("ragdoll: ProcessEvent hook could not be made");
            return -1;
        }
        hook_made = 1;
    }
    if (want && !pe_enabled) {
        if (MH_EnableHook((void *)ADDR_PROCESSEVENT) != MH_OK) { LOG("ragdoll: ProcessEvent hook could not be enabled"); return -1; }
        pe_enabled = 1;
    } else if (!want && pe_enabled) {
        MH_DisableHook((void *)ADDR_PROCESSEVENT);
        pe_enabled = 0;
    }
    return 0;
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
    n_other_pairs = n_hit_lines = n_other_lines = 0;
    memset(ring, 0, sizeof ring);
    memset(focus, 0, sizeof focus);
    memset(fx_ring, 0, sizeof fx_ring);
    fx_new_logs = 0;
    trace_t0 = now_s();
    trace_end = trace_t0 + secs;
    LeaveCriticalSection(&tcs);
    game_tid = GetCurrentThreadId();   // chat commands run on the game thread
    InterlockedExchange(&tracing, 1);
    if (pe_update()) { InterlockedExchange(&tracing, 0); out_printf(o, "/ragdoll trace: hook failed\n"); return -1; }
    stacks_arm();
    LOG("ragdoll trace: started for %.0f s (Ridden tracked: %d)", secs, ntr);
    return 0;
}

static void trace_stop(const char *why) {
    if (!tracing) return;
    InterlockedExchange(&tracing, 0);
    pe_update();   // stays on for the blood bags while the ragdoll is on
    stacks_disarm();
    LOG("ragdoll trace: stopped (%s): %d calls on Ridden, %d distinct class.function, %d deaths seen; hit calls logged: %d "
        "on Ridden/corpses (cap %d), %d elsewhere (cap %d)", why, n_calls, npairs, n_deaths, n_hit_lines, HIT_LINES,
        n_other_lines, OTHER_LINES);
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
    char b[160], t[64], v[1024];
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
            if (inst) { v[0] = ' '; v[1] = '='; v[2] = ' '; fmt_prop(p, (const uint8_t *)inst, v + 3, sizeof v - 3, 1); }
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

// Settings that could push a body: every property in o's whole class chain (engine classes included) whose name
// says impulse/damage/momentum/..., with its value. On the mesh this shows the engine's bApplyImpulseOnDamage.
static const char *const PUSH_KEYS[] = {"impulse", "damage", "momentum", "knock", "stagger", "force", "push",
                                        "velocity", "strength", "ragdoll", NULL};
static void log_push_settings(UObject *o, const char *what) {
    char sn[128], pn[128], t[64], v[1024];
    int n = 0;
    if (!o) return;
    for (UStruct *s = U_CLASS(o); s; s = US_SUPER(s)) {
        ue_obj_name(s, sn, sizeof sn);
        for (FField *p = US_CHILDPROPS(s); p; p = FF_NEXT(p)) {
            ue_name(FF_NAME(p), pn, sizeof pn);
            if (!name_has(pn, PUSH_KEYS)) continue;
            ue_name(*(FName *)FF_CLASS(p), t, sizeof t);
            if (strstr(t, "Delegate")) continue;   // OnTakeAnyDamage etc.: events, not settings
            fmt_prop(p, (const uint8_t *)o, v, sizeof v, 1);
            LOG("ragdoll probe:   %s: %s.%s = %s", what, sn, pn, v);
            n++;
        }
    }
    if (!n) LOG("ragdoll probe:   %s: no impulse/damage/force settings", what);
}

// Every component of the actor (class and name); the hit- and physics-related ones are listed in full.
static void log_components(UObject *actor, UObject *const *done, int n_done) {
    static const char *const COMP_KEYS[] = {"hit", "react", "impact", "damage", "procedural", "physic", "ragdoll",
                                            "health", "dismember", "gib", "wound", "mutilat", NULL};   // mutilat: limbs off
    UClass *ac = cls("ActorComponent");
    Call k;
    if (!ac || !call_prep(&k, actor, "K2_GetComponentsByClass")) { LOG("ragdoll probe: no K2_GetComponentsByClass"); return; }
    SET(&k, "ComponentClass", UClass *, ac);
    call_go(&k);
    TArray *r = carg(&k, "ReturnValue");
    if (!r || !r->data) { LOG("ragdoll probe: components: none returned"); return; }
    char a[160], c[160];
    LOG("ragdoll probe: %d components:", r->num);
    for (int i = 0; i < r->num && i < 96; i++) {
        UObject *comp = ((UObject **)r->data)[i];
        if (!comp) continue;
        cname(comp, c, sizeof c);
        LOG("ragdoll probe:   component %s (%s)", oname(comp, a, sizeof a), c);
    }
    for (int i = 0; i < r->num && i < 96; i++) {
        UObject *comp = ((UObject **)r->data)[i];
        if (!comp) continue;
        int skip = 0;
        for (int j = 0; j < n_done; j++) if (done[j] == comp) skip = 1;
        if (skip || !name_has(cname(comp, c, sizeof c), COMP_KEYS)) continue;
        snprintf(a, sizeof a, "component %s (everything)", c);
        list_class(U_CLASS(comp), 1, 1, a, comp);
    }
}

// Every damage type class loaded right now (weapons load theirs with them: probe after shooting and throwing
// things), with the engine's impulse settings from its default object: DamageImpulse (point and radial damage push
// bodies by this much when the damage goes through the engine and the component allows it), bRadialDamageVelChange,
// bScaleMomentumByMass, DestructibleImpulse, plus any of the game's own settings with a push-like name.
static void log_damage_types(void) {
    UClass *dt = cls("DamageType"), *cc = cls("Class");
    if (!dt || !cc) { LOG("ragdoll probe: damage types: no DamageType class"); return; }
    int n = 0, n_push = 0;
    char b[160], pn[96], t[64], v[256];
    for (int32_t i = 0; i < ue_num_objects() && n < 200; i++) {
        UObject *o = ue_object_at(i);
        if (!o || !ue_is_a(o, cc)) continue;
        int is_dt = 0;
        for (UStruct *s = (UStruct *)o; s; s = US_SUPER(s)) if ((UClass *)s == dt) { is_dt = 1; break; }
        UObject *cdo = is_dt ? UC_CDO(o) : NULL;
        if (!cdo) continue;
        n++;
        char line[1536];
        size_t k = (size_t)snprintf(line, sizeof line, "ragdoll probe: damage type %s:", ue_obj_name(o, b, sizeof b));
        int pushes = 0;
        for (UStruct *s = (UStruct *)o; s && k + 16 < sizeof line; s = US_SUPER(s)) {
            for (FField *p = US_CHILDPROPS(s); p && k + 16 < sizeof line; p = FF_NEXT(p)) {
                ue_name(FF_NAME(p), pn, sizeof pn);
                if (!name_has(pn, PUSH_KEYS) && strcmp(pn, "bScaleMomentumByMass") && strcmp(pn, "bRadialDamageVelChange")) continue;
                ue_name(*(FName *)FF_CLASS(p), t, sizeof t);
                if (strstr(t, "Delegate")) continue;
                fmt_prop(p, (const uint8_t *)cdo, v, sizeof v, 1);
                if (!strcmp(pn, "DamageImpulse") && atof(v) != 0) pushes = 1;
                k += (size_t)snprintf(line + k, sizeof line - k, " %s=%s", pn, v);
            }
            if ((UClass *)s == dt) break;
        }
        n_push += pushes;
        LOG("%s", line);
    }
    LOG("ragdoll probe: %d damage types loaded%s, %d with a DamageImpulse other than 0", n, n >= 200 ? " (first 200)" : "", n_push);
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
    // Hit discovery (build 14): what could push a body, and from where.
    LOG("ragdoll probe: ---- push settings (build 14) ----");
    log_push_settings(mesh, "mesh");
    log_push_settings(ue_get_ptr(best, "CharacterMovement"), "movement");
    log_push_settings(best, "actor");
    UObject *done[3] = {dr, hr, rc};
    log_components(best, done, 3);
    log_damage_types();
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

// ---- step 2: the killing hit throws the body ----
// The game's damage never goes through the engine's damage path (build 14: no ReceivePointDamage / ReceiveAnyDamage,
// and its weapons' damage types aren't UDamageTypes), so the engine's impulse-on-damage never fires. Each Ridden's
// HealthComponent keeps its last hits in RecentDamageInfo, replicated (OnRep_RecentDamageInfo), so host and client
// both have it: TArray<FReplicatedDamageInfo> (build 16 log): Index (a running count), Damage, ServerWorldTime,
// BoneName, DamageType, Source, SourceName, SourceLocation, DamageContextFlags. No hit point, no direction (the
// OnDamage event's FDamageInfo has them, but only the server sees that). So, as the game's own client hit reactions
// must: the killing hit = the newest entry (highest Index) that isn't a stumble event (Stumble_* damage type, which
// comes with every hit); the hit point = the hit bone's place on the mesh now; the direction = from SourceLocation (or
// the Source's own place) to that point. Explosions: SourceLocation as the blast centre (to be checked in the log).
// The push is a real impulse (kg cm/s): the same hit moves a heavier body less, and /ragdoll mass counts.
//   bullets, melee: AddImpulseAtLocation(direction x strength, hit point) on the hit bone's body, or the nearest
//                   parent bone that has one (a Common's physics asset has 20 bodies, its skeleton more bones).
//   explosions:     AddRadialImpulse from the blast centre as a velocity change of strength / the body's mass, the
//                   same for every body part (a blast pushes by area, which goes roughly with mass), weaker with
//                   distance (x1 at the centre, x0.25 from BLAST_RANGE on).
// Strength per weapon kind, by the damage type's class: HG02_DamageType_C -> "HG" (the letters before the number),
// else a table key found in the name (FragGrenade_DamageType_C -> "FragGrenade"). An unknown kind gets "default" and is
// logged once by name, so it can be added: /ragdoll push <key> <n>. Every strength x the overall /ragdoll push scale.
#define BLAST_RANGE  800.f   // cm from the blast centre where an explosion's push bottoms out
#define PUSH_LOGS    40      // pushes logged per /ragdoll on
#define HIT_DUMPS    3       // deaths whose whole RecentDamageInfo is logged per /ragdoll on (array order, clients)
typedef struct { char key[24]; float n; uint8_t radial; } PushKind;
static PushKind push_tab[48] = {   // first guesses (x the scale, default 4); a Common weighs 133 kg by GetBoneMass
    {"HG", 6000, 0},  {"SMG", 7000, 0}, {"AR", 9000, 0}, {"LMG", 11000, 0}, {"SG", 25000, 0},
    {"FragGrenade", 60000, 1},   // frags and pipe bombs (both FragGrenade_DamageType_C); Joon's pick (16 Oct)
    {"PipeBomb", 40000, 1}, {"Grenade", 40000, 1}, {"Explosi", 40000, 1},   // (build 22 had these commented out by mistake)
    {"Propane", 40000, 1},
    // build 22 log: red barrels and Exploders were unknown (default 8000, aimed from the shooter, not the blast).
    // Blasts, as strong as a frag until Joon picks (first guess). Gas cans kill by fire over seconds, 5-8 m away:
    // a burning body collapses where it stands, so no push (first guess too; /ragdoll push GasCan <n> to change)
    {"ExplodingBarrel", 60000, 1}, {"Exploder", 60000, 1}, {"GasCan", 0, 0},
    {"Stumble", 0, 0}, {"default", 8000, 0},
    {"Sni", 25000, 0},   // snipers (Sni01_DamageType_C) as strong as the shotgun (Joon, 16 Oct)
};
static int n_push_tab = 16;   // the entries above (/ragdoll push <kind> <n> appends)
#define PUSH_DEFAULT 4.f   // Joon's pick (build 17, 16 Oct). GetBoneMass sums: Common 133 kg, Reeker family 710-790 kg
static float push_scale = PUSH_DEFAULT;
static int n_pushed, n_push_radial, n_push_nohit, n_push_fail, push_logs, hit_dumps;

// The stored hit record's fields, found by reflection once, by name and type, so another layout is logged instead of
// misread: the record's full layout goes into the log once. Usable: a hit point + direction (an FDamageInfo-like
// record), or a source (SourceLocation, or the Source object's place) to aim from. Everything else is optional.
static struct {
    int state; int32_t arr_off, elsize; UStruct *st;
    FField *point, *dir, *srcloc, *source;                               // aim: point + dir, or a source
    FField *type, *comp, *bone, *prev_hp, *stumble, *damage, *index, *time;   // optional
} dinfo;

static FField *field_of(UStruct *st, const char *const *names, const char *const *types, int vector) {
    char t[64], sn[96];
    for (int i = 0; names[i]; i++) {
        FField *f = ue_find_prop(st, names[i]);
        if (!f) continue;
        ue_name(*(FName *)FF_CLASS(f), t, sizeof t);
        for (int k = 0; types[k]; k++) {
            if (strcmp(t, types[k])) continue;
            if (vector && strncmp(ue_obj_name(FP_SUB(f), sn, sizeof sn), "Vector", 6)) continue;
            return f;
        }
    }
    return NULL;
}

static int dinfo_init(UObject *hc) {
    if (dinfo.state) return dinfo.state;
    char sn[96], fn[64], t[64], line[2048];
    dinfo.state = -1;
    FField *arr = ue_find_prop(U_CLASS(hc), "RecentDamageInfo");
    FField *inner = arr ? *(FField **)((char *)arr + 0x78) : NULL;   // FArrayProperty::Inner
    char it[64] = "";
    if (inner) ue_name(*(FName *)FF_CLASS(inner), it, sizeof it);
    UStruct *st = inner && !strcmp(it, "StructProperty") ? (UStruct *)FP_SUB(inner) : NULL;
    if (!st) { LOG("ragdoll push: HealthComponent.RecentDamageInfo %s: pushes off", !arr ? "missing" : "is not an array of structs"); return -1; }
    dinfo.st = st;
    dinfo.arr_off = FP_OFFSET(arr);
    dinfo.elsize = FP_ELSIZE(inner);
    size_t k = (size_t)snprintf(line, sizeof line, "ragdoll push: RecentDamageInfo = array of %s (%d bytes):", ue_obj_name(st, sn, sizeof sn), dinfo.elsize);
    for (UStruct *x = st; x && k + 64 < sizeof line; x = US_SUPER(x))
        for (FField *f = US_CHILDPROPS(x); f && k + 64 < sizeof line; f = FF_NEXT(f))
            k += (size_t)snprintf(line + k, sizeof line - k, " %s:%s@0x%x", ue_name(FF_NAME(f), fn, sizeof fn),
                                  ue_name(*(FName *)FF_CLASS(f), t, sizeof t), FP_OFFSET(f));
    LOG("%s", line);
    static const char *const VEC[] = {"StructProperty", NULL}, *const OBJ[] = {"ObjectProperty", "WeakObjectProperty", "ClassProperty", NULL},
                             *const NAM[] = {"NameProperty", NULL}, *const FLT[] = {"FloatProperty", NULL}, *const BOO[] = {"BoolProperty", NULL},
                             *const INT[] = {"IntProperty", NULL};
    static const char *const N_POINT[] = {"ImpactPoint", "HitResultLocation", "HitLocation", NULL},
                             *const N_DIR[] = {"ImpactDirection", "Direction", "ShotDirection", "HitDirection", "TraceDirection", NULL},
                             *const N_SRCLOC[] = {"SourceLocation", "InstigatorLocation", "OriginLocation", NULL},
                             *const N_SOURCE[] = {"Source", "DamageCauser", "Instigator", NULL},
                             *const N_TYPE[] = {"DamageType", "DamageTypeClass", NULL}, *const N_COMP[] = {"HitComponent", NULL},
                             *const N_BONE[] = {"BoneName", "HitBoneName", "Bone", NULL},
                             *const N_HP[] = {"PreviousTotalHealth", "PreviousHealth", NULL},
                             *const N_STUMBLE[] = {"bStumbleEvent", NULL}, *const N_DMG[] = {"Damage", NULL},
                             *const N_INDEX[] = {"Index", NULL}, *const N_TIME[] = {"ServerWorldTime", NULL};
    dinfo.point = field_of(st, N_POINT, VEC, 1);
    dinfo.dir = field_of(st, N_DIR, VEC, 1);
    dinfo.srcloc = field_of(st, N_SRCLOC, VEC, 1);
    dinfo.source = field_of(st, N_SOURCE, OBJ, 0);
    dinfo.type = field_of(st, N_TYPE, OBJ, 0);
    dinfo.comp = field_of(st, N_COMP, OBJ, 0);
    dinfo.bone = field_of(st, N_BONE, NAM, 0);
    dinfo.prev_hp = field_of(st, N_HP, FLT, 0);
    dinfo.stumble = field_of(st, N_STUMBLE, BOO, 0);
    dinfo.damage = field_of(st, N_DMG, FLT, 0);
    dinfo.index = field_of(st, N_INDEX, INT, 0);
    dinfo.time = field_of(st, N_TIME, FLT, 0);
    struct { const char *what; FField *f; } use[] = {
        {"hit point", dinfo.point}, {"direction", dinfo.dir}, {"source location", dinfo.srcloc}, {"source", dinfo.source},
        {"type", dinfo.type}, {"hit component", dinfo.comp}, {"bone", dinfo.bone}, {"health before", dinfo.prev_hp},
        {"stumble flag", dinfo.stumble}, {"index", dinfo.index}, {"time", dinfo.time}};
    k = (size_t)snprintf(line, sizeof line, "ragdoll push: fields used:");
    for (size_t i = 0; i < sizeof use / sizeof use[0]; i++)
        k += (size_t)snprintf(line + k, sizeof line - k, " %s=%s", use[i].what, use[i].f ? ue_name(FF_NAME(use[i].f), fn, sizeof fn) : "-");
    LOG("%s", line);
    if (!(dinfo.point && dinfo.dir) && !dinfo.srcloc && !dinfo.source) {
        LOG("ragdoll push: the record has no hit direction and no source to aim from (field list above): pushes off");
        return -1;
    }
    LOG("ragdoll push: aiming %s; newest hit by %s", dinfo.point && dinfo.dir ? "with the record's hit point and direction"
        : "from the source to the hit bone", dinfo.prev_hp ? "lowest health before" : dinfo.index ? "highest Index"
        : dinfo.time ? "latest ServerWorldTime" : "the last entry");
    return dinfo.state = 1;
}

// an object field of a record: object, weak object or class property
static UObject *obj_field(FField *f, const uint8_t *e) {
    char t[64];
    if (!f) return NULL;
    ue_name(*(FName *)FF_CLASS(f), t, sizeof t);
    return !strcmp(t, "WeakObjectProperty") ? ue_weak_get(e + FP_OFFSET(f)) : *(UObject *const *)(e + FP_OFFSET(f));
}

static PushKind *push_kind(const char *key, int add) {
    for (int i = 0; i < n_push_tab; i++) if (!_stricmp(push_tab[i].key, key)) return &push_tab[i];
    if (!add || n_push_tab == (int)(sizeof push_tab / sizeof push_tab[0])) return NULL;
    PushKind *k = &push_tab[n_push_tab++];
    snprintf(k->key, sizeof k->key, "%s", key);
    k->n = push_kind("default", 0)->n;
    k->radial = 0;
    return k;
}

// the damage type's class name -> the table entry
static PushKind *kind_of(UObject *dtype, char *name, size_t n) {
    if (!dtype) { snprintf(name, n, "None"); return push_kind("default", 0); }
    static UClass *class_cls;
    if (!class_cls) class_cls = cls("Class");
    if (class_cls && ue_is_a(dtype, class_cls)) ue_obj_name(dtype, name, n);   // a class (ClassProperty)
    else cname(dtype, name, n);                                               // its default object
    char code[24], low[160], kl[24];
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof code && ((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= 'a' && name[i] <= 'z')); i++) code[i] = name[i];
    code[i] = 0;
    PushKind *k = (name[i] >= '0' && name[i] <= '9') ? push_kind(code, 0) : NULL;   // HG02 -> HG
    if (k) return k;
    for (i = 0; name[i] && i + 1 < sizeof low; i++) low[i] = (char)tolower((unsigned char)name[i]);
    low[i] = 0;
    for (int j = 0; j < n_push_tab; j++) {   // a key in the name: FragGrenade_DamageType_C
        size_t m = 0;
        for (; push_tab[j].key[m] && m + 1 < sizeof kl; m++) kl[m] = (char)tolower((unsigned char)push_tab[j].key[m]);
        kl[m] = 0;
        if (m >= 4 && strcmp(kl, "default") && strstr(low, kl)) return &push_tab[j];
    }
    static char unknown[16][96];
    int seen = 0, slot = -1;
    for (int j = 0; j < 16; j++) { if (!strcmp(unknown[j], name)) seen = 1; if (slot < 0 && !unknown[j][0]) slot = j; }
    if (!seen && slot >= 0) {
        snprintf(unknown[slot], sizeof unknown[slot], "%s", name);
        LOG("ragdoll push: unknown damage type %s: default strength (add it with /ragdoll push %s <n>)", name, code[0] ? code : name);
    }
    return push_kind("default", 0);
}

static const BodyList *bodies_of(UObject *mesh);
static int has_body(const BodyList *bl, FName b) {
    for (int i = 0; bl && i < bl->n; i++) if (bl->bone[i].idx == b.idx && bl->bone[i].num == b.num) return 1;
    return 0;
}

// the whole body's mass in kg (every body, mass scale included), -1 unknown
static float body_mass(UObject *mesh, const BodyList *bl) {
    Call k;
    if (bl && call_prep(&k, mesh, "GetBoneMass")) {
        FName *bn = carg(&k, "BoneName");
        float *r = carg(&k, "ReturnValue"), sum = 0;
        SET(&k, "bScaleMass", uint8_t, 1);
        if (bn && r) {
            for (int i = 0; i < bl->n; i++) { *bn = bl->bone[i]; *r = 0; call_go(&k); sum += *r; }
            if (sum > 0) return sum;
        }
    }
    return mesh_mass(mesh);
}

static void log_hit(const char *tag, int idx, const uint8_t *e) {
    char line[2048], fn[64], fv[256];
    size_t k = (size_t)snprintf(line, sizeof line, "ragdoll push:   %s [%d]", tag, idx);
    for (UStruct *x = dinfo.st; x && k + 32 < sizeof line; x = US_SUPER(x))
        for (FField *f = US_CHILDPROPS(x); f && k + 32 < sizeof line; f = FF_NEXT(f)) {
            fmt_prop(f, e, fv, sizeof fv, 0);
            k += (size_t)snprintf(line + k, sizeof line - k, " %s=%s", ue_name(FF_NAME(f), fn, sizeof fn), fv);
        }
    LOG("%s", line);
}

// the place of a bone (or socket) of the mesh now, 0 = ok
static int bone_loc(UObject *mesh, FName bone, float out[3]) {
    Call k;
    if (!call_prep(&k, mesh, "GetSocketLocation")) return -1;
    SET(&k, "InSocketName", FName, bone);
    call_go(&k);
    float *r = carg(&k, "ReturnValue");
    if (!r) return -1;
    memcpy(out, r, 3 * sizeof(float));
    return 0;
}

static float entry_key(const uint8_t *e) {   // larger = the killing hit
    if (dinfo.prev_hp) return -*(const float *)(e + FP_OFFSET(dinfo.prev_hp));
    if (dinfo.index) return (float)*(const int32_t *)(e + FP_OFFSET(dinfo.index));
    if (dinfo.time) return *(const float *)(e + FP_OFFSET(dinfo.time));
    return 0;
}

static void push_body(Tracked *t, UObject *mesh) {
    char a[160], tn[96], bn[64], hb[64], sn[96], sc[96];
    if (push_scale <= 0.f) return;
    UObject *hc = call_obj(t->a, "GetHealthComponent");
    if (!hc || dinfo_init(hc) != 1) { n_push_fail++; return; }
    const TArray *arr = (const TArray *)((char *)hc + dinfo.arr_off);
    const uint8_t *best = NULL;
    int dump = hit_dumps < HIT_DUMPS;
    if (dump) { hit_dumps++; LOG("ragdoll push: %s: RecentDamageInfo, %d entries:", oname(t->a, a, sizeof a), arr->num); }
    for (int i = 0; arr->data && i < arr->num && i < 16; i++) {
        const uint8_t *e = (const uint8_t *)arr->data + (size_t)i * dinfo.elsize;
        if (dump) log_hit("entry", i, e);
        // the stumble event that comes with every hit: its flag, else no hit component, else a Stumble damage type
        if (dinfo.stumble && bool_at(dinfo.stumble, e) == 1) continue;
        if (dinfo.comp && !obj_field(dinfo.comp, e)) continue;
        if (dinfo.type) { PushKind *sk = kind_of(obj_field(dinfo.type, e), sn, sizeof sn); if (sk && !strcmp(sk->key, "Stumble")) continue; }
        if (!best || entry_key(e) >= entry_key(best)) best = e;   // ties: the later entry
    }
    if (!best) {
        n_push_nohit++;
        if (push_logs < PUSH_LOGS) { push_logs++; LOG("ragdoll push: %s: no usable hit in RecentDamageInfo (%d entries): not pushed", oname(t->a, a, sizeof a), arr->num); }
        return;
    }
    PushKind *kind = dinfo.type ? kind_of(obj_field(dinfo.type, best), tn, sizeof tn) : (snprintf(tn, sizeof tn, "unknown"), push_kind("default", 0));
    float strength = kind->n * push_scale;
    if (strength <= 0.f) return;
    const BodyList *bl = bodies_of(mesh);
    FName bone = dinfo.bone ? *(const FName *)(best + FP_OFFSET(dinfo.bone)) : (FName){0, 0};
    ue_name(bone, hb, sizeof hb);
    // the bone that takes the push: the hit bone, or its nearest parent with a body, else the first body (the root)
    Call k;
    for (int up = 0; bone.idx && bl && !has_body(bl, bone) && up < 8; up++) {
        if (!call_prep(&k, mesh, "GetParentBone")) { bone = (FName){0, 0}; break; }
        SET(&k, "BoneName", FName, bone);
        call_go(&k);
        FName *r = carg(&k, "ReturnValue");
        bone = r ? *r : (FName){0, 0};
    }
    if (bl && (!bone.idx || !has_body(bl, bone))) bone = bl->bone[0];
    // where it was hit, and where from
    float pt[3], org[3], dr[3];
    const char *from;
    if (dinfo.point && dinfo.dir) {
        memcpy(pt, best + FP_OFFSET(dinfo.point), sizeof pt);
        const float *d = (const float *)(best + FP_OFFSET(dinfo.dir));
        for (int c = 0; c < 3; c++) org[c] = pt[c] - d[c];
        from = "the record";
    } else {
        if (bone_loc(mesh, bone, pt) && actor_loc(t->a, pt)) {
            n_push_fail++;
            if (push_logs < PUSH_LOGS) { push_logs++; LOG("ragdoll push: %s: no place for bone %s: not pushed", oname(t->a, a, sizeof a), hb); }
            return;
        }
        const float *sl = dinfo.srcloc ? (const float *)(best + FP_OFFSET(dinfo.srcloc)) : NULL;
        UObject *src = dinfo.source ? obj_field(dinfo.source, best) : NULL;
        if (sl && (sl[0] != 0.f || sl[1] != 0.f || sl[2] != 0.f)) { memcpy(org, sl, sizeof org); from = "SourceLocation"; }
        else if (src && !actor_loc(owner_actor(src) ? owner_actor(src) : src, org)) from = "the source's place";
        else if (kind->radial) { memcpy(org, pt, sizeof org); org[2] -= 50.f; from = "below the body (no source)"; }   // replaced by a blast below
        else {
            n_push_fail++;
            if (push_logs < PUSH_LOGS) { push_logs++; LOG("ragdoll push: %s: killing hit (%s) has no source location: not pushed", oname(t->a, a, sizeof a), tn); }
            return;
        }
    }
    if (kind->radial) {   // the blast centre: the nearest explosion of the last second, if any within 20 m
        double now = now_s();
        float bd = 2000.f * 2000.f;
        int bi = -1;
        for (int i = 0; i < 8; i++) {
            if (!blasts[i].t || now - blasts[i].t > 1.0) continue;
            float d2 = 0;
            for (int c = 0; c < 3; c++) d2 += (pt[c] - blasts[i].at[c]) * (pt[c] - blasts[i].at[c]);
            if (d2 < bd) { bd = d2; bi = i; }
        }
        if (bi >= 0) { memcpy(org, blasts[bi].at, sizeof org); from = "the explosion"; }
    }
    for (int c = 0; c < 3; c++) dr[c] = pt[c] - org[c];
    float len = sqrtf(dr[0] * dr[0] + dr[1] * dr[1] + dr[2] * dr[2]);
    if (len < 1e-3f) {
        n_push_fail++;
        if (push_logs < PUSH_LOGS) { push_logs++; LOG("ragdoll push: %s: killing hit (%s) comes from the body's own place: not pushed", oname(t->a, a, sizeof a), tn); }
        return;
    }
    float mass = body_mass(mesh, bl);
    int log_it = push_logs < PUSH_LOGS;
    UObject *src = dinfo.source ? obj_field(dinfo.source, best) : NULL;
    if (kind->radial) {
        float f = 1.f - len / BLAST_RANGE;
        if (f < 0.25f) f = 0.25f;
        float dv = mass > 0 ? strength * f / mass : strength * f / 80.f;   // cm/s for every body part
        if (!call_prep(&k, mesh, "AddRadialImpulse")) { n_push_fail++; return; }
        float *o = carg(&k, "Origin");
        if (o) memcpy(o, org, sizeof org);
        SET(&k, "Radius", float, len + 300.f);   // reaches every part of the body
        SET(&k, "Strength", float, dv);
        SET(&k, "Falloff", uint8_t, 0);           // RIF_Constant: the distance is in dv already
        SET(&k, "bVelChange", uint8_t, 1);
        call_go(&k);
        n_pushed++; n_push_radial++;
        if (log_it) {
            push_logs++;
            LOG("ragdoll push: %s killed by %s (%s %.0f x%.2f) from %s (%s, %s): explosion at (%.0f %.0f %.0f), %.0f cm away, "
                "x%.2f for distance, body %.1f kg -> %.0f cm/s", oname(t->a, a, sizeof a), tn, kind->key, kind->n, push_scale,
                oname(src, sn, sizeof sn), cname(src, bn, sizeof bn), from, org[0], org[1], org[2], len, f, mass, dv);
        }
        return;
    }
    float imp[3] = {dr[0] / len * strength, dr[1] / len * strength, dr[2] / len * strength};
    if (!call_prep(&k, mesh, "AddImpulseAtLocation")) { n_push_fail++; return; }
    float *ip = carg(&k, "Impulse"), *lp = carg(&k, "Location");
    if (ip) memcpy(ip, imp, sizeof imp);
    if (lp) memcpy(lp, pt, sizeof pt);
    SET(&k, "BoneName", FName, bone);
    call_go(&k);
    n_pushed++;
    if (log_it) {
        push_logs++;
        LOG("ragdoll push: %s killed by %s (%s %.0f x%.2f) from %s (%s, %s, %.0f cm away): hit %s, pushed on %s, direction "
            "(%.2f %.2f %.2f), body %.1f kg (~%.0f cm/s for the whole body)", oname(t->a, a, sizeof a), tn, kind->key, kind->n,
            push_scale, oname(src, sn, sizeof sn), cname(src, sc, sizeof sc), from, len, hb, bone.idx ? ue_name(bone, bn, sizeof bn) : "the root body",
            dr[0] / len, dr[1] / len, dr[2] / len, mass, mass > 0 ? strength / mass : -1.f);
    }
}

// ---- deaths the game turns into an explosion: left to the game ----
// The Reeker family dies through a death montage that is the gameplay itself (build 17 trace, 16 Oct): Reeker
// (BloaterBasic) Bloater_DeathPop_AM = its gas pop, Exploder Exploder_Explode_Death_AM = its explosion, Retch (Vomiter)
// Bloater_DeathPop_AM = its body bursting (its blast comes earlier, from ExplosionComponent.OnDeath). apply() stops every
// montage, which cut the pop and the explosion (on the host: for everyone), and the game never starts a corpse
// timer for them, so they lay there until the deadline. So: at death, if the death montage the game just started is a
// pop or an explosion, the death is the game's: no montage stop, no ragdoll, no push. Per death, by the montage, so a
// Reeker that ever dies without popping still ragdolls, and other popping kinds need no list.
// Not "gib": the Zom_Death_Gib_* deaths of commons near a blast (build 18 trace) are ragdolled and pushed (Joon, 16 Oct).
static const char *const POP_KEYS[] = {"deathpop", "explode", NULL};
static int n_left, left_logs;

static int special_death(Tracked *t) {
    char a[160], c[160], m[160];
    UObject *mesh = ue_get_ptr(t->a, "Mesh"), *ai = mesh ? ue_get_ptr(mesh, "AnimScriptInstance") : NULL;
    UObject *mon = ai ? call_obj(ai, "GetCurrentActiveMontage") : NULL;
    if (!mon || !name_has(ue_obj_name(mon, m, sizeof m), POP_KEYS)) return 0;
    n_left++;
    if (left_logs < 60) {
        left_logs++;
        LOG("ragdoll: %s (%s) left to the game: its death montage %s pops or explodes", oname(t->a, a, sizeof a),
            cname(t->a, c, sizeof c), m);
    }
    return 1;
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
    if (t->bag) bag_unhide(t, mesh);
    fx_probe_mark(t);
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
    push_body(t, mesh);   // step 2: the killing hit throws the body
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
            if (x->ragdolled && x->fxwho && !x->ended) fx_probe_tick(x, t);
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
        if (is_boss(x->a) || special_death(x)) continue;
        if (apply(x) == 0) n_applied++; else n_failed++;
    }
}

void ragdoll_slash(char *rest, Out *o) {
    char *verb = rest ? strtok(rest, " ") : NULL, *arg = verb ? strtok(NULL, " ") : NULL;
    if (!verb || !strcmp(verb, "status")) {
        int released = n_at_rest + n_capped;
        out_printf(o, "/ragdoll: %s%s, %d Ridden tracked, %d deaths seen, %d ragdolled, %d left to the game (pop/explode), %d failed, %d came back; "
                      "settle hold %s: %d held, %d at rest + %d at the %.0f s cap (avg %.1f s, max %.1f s after death), "
                      "%d deleted while held; corpse budget %s: %d spared (%d again); deadline %.0f s: %d removed "
                      "(%d never got the game's settle check); mass x%.2f, gravity x%.2f%s; push x%.2f: %d pushed (%d explosions), "
                      "%d without a hit, %d failed; blood bags %s: %d spared; %ld stacks logged%s%s\n",
                   mode_on ? "on" : "off", mode_on && delay_s > 0 ? " (after the game's animation)" : "", ntr, n_deaths,
                   n_applied, n_left, n_failed, n_revived,
                   settle_state == 1 ? "on" : settle_state ? "OFF (see log)" : "not armed yet", n_held, n_at_rest, n_capped,
                   SETTLE_CAP, released ? settle_sum / released : 0.0, settle_max, n_held_gone,
                   corpse_state == 1 ? "hook on" : corpse_state ? "hook OFF (see log)" : "not armed yet", n_spared,
                   n_spared_again, deadline_s, n_ended, n_ended_unchecked, mass_scale, grav_scale, n_grav_fail ? " (NO EFFECT: see log)" : "",
                   push_scale, n_pushed, n_push_radial, n_push_nohit, n_push_fail,
                   bloodbag_on ? "on" : bag_state < 0 ? "off but NOT AVAILABLE (see log)" : bag_state ? "off" : "off (not armed yet)",
                   n_bags_spared, stack_id,
                   tracing ? ", tracing" : "", n_overflow ? ", tracking list full" : "");
    } else if (!strcmp(verb, "on")) {
        float d = arg ? (float)atof(arg) : 0.f;
        delay_s = d < 0 ? 0 : d > 10 ? 10 : d;
        mode_on = 1;
        detail_left = DETAIL_LOGS;
        settle_logs = 0;
        push_logs = hit_dumps = left_logs = bag_logs = fxp_spared = fxp_ctrl = 0;
        settle_arm();
        bag_arm();
        pe_update();
        LOG("ragdoll: on, delay %.2f s", delay_s);
        out_printf(o, "/ragdoll on: Ridden that die from now on fall as ragdolls%s and stay until they lie still (this machine only)%s\n",
                   delay_s > 0 ? " after the game's death animation starts" : "",
                   settle_state == 1 ? "" : "; settle hold OFF: they freeze at the game's time (see log)");
    } else if (!strcmp(verb, "off")) {
        mode_on = 0;
        pe_update();
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
        out_printf(o, "/ragdoll gravity: x%.2f (0-5, default %.1f) on ragdolls still falling, from now on%s\n", grav_scale, GRAV_DEFAULT,
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
    } else if (!strcmp(verb, "bloodbag")) {
        if (arg && (!strcmp(arg, "on") || !strcmp(arg, "off"))) {
            bloodbag_on = !strcmp(arg, "on");
            pe_update();
            LOG("ragdoll bloodbag: %s", bloodbag_on ? "on (the game's)" : "off (bodies kept)");
        }
        out_printf(o, "/ragdoll bloodbag: %s (default off) while /ragdoll is on: Ridden near a blast %s%s\n",
                   bloodbag_on ? "on" : "off", bloodbag_on ? "burst like a blood bag (the game's)" : "stay bodies, ragdolled and pushed",
                   bag_state < 0 ? "; NOT AVAILABLE (see log)" : !bag_state ? " (armed at /ragdoll on)" : "");
    } else if (!strcmp(verb, "push")) {
        char *arg2 = arg ? strtok(NULL, " ") : NULL;
        if (arg && !arg2 && (isdigit((unsigned char)arg[0]) || arg[0] == '.')) {   // /ragdoll push <x>: overall scale
            float x = (float)atof(arg);
            push_scale = x < 0.f ? 0.f : x > 10.f ? 10.f : x;
            LOG("ragdoll push: scale x%.2f", push_scale);
        } else if (arg && !strcmp(arg, "off")) {
            push_scale = 0.f;
            LOG("ragdoll push: off");
        } else if (arg && !strcmp(arg, "on")) {
            push_scale = PUSH_DEFAULT;
            LOG("ragdoll push: on (x%.2f, the default)", push_scale);
        } else if (arg && arg2) {   // /ragdoll push <key> <n>: one weapon kind
            float n = (float)atof(arg2);
            PushKind *k = push_kind(arg, 1);
            if (!k) { out_printf(o, "/ragdoll push: table full\n"); return; }
            k->n = n < 0.f ? 0.f : n > 500000.f ? 500000.f : n;
            LOG("ragdoll push: %s = %.0f%s", k->key, k->n, k->radial ? " (explosion)" : "");
        } else if (arg) {
            out_printf(o, "usage: /ragdoll push [<x> | on | off | <weapon kind> <strength>]\n");
            return;
        }
        char line[1024];
        size_t m = 0;
        for (int i = 0; i < n_push_tab && m + 40 < sizeof line; i++)
            m += (size_t)snprintf(line + m, sizeof line - m, "%s%s %.0f%s", i ? ", " : "", push_tab[i].key, push_tab[i].n,
                                  push_tab[i].radial ? " (blast)" : "");
        out_printf(o, "/ragdoll push: x%.2f (0-10, default %.0f, 0 = off) on bodies that die from now on; kg cm/s per weapon "
                      "kind: %s\n", push_scale, PUSH_DEFAULT, line);
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
        out_printf(o, "usage: /ragdoll [on [delay]|off|status|mass [x]|gravity [x]|deadline [s]|push [x|on|off|<kind> <n>]|bloodbag [on|off]|probe|trace [secs]]\n");
    }
}

int ragdoll_init(void) {
    LOG("ragdoll: experimental module loaded (fork build), off until /ragdoll on");
    return 0;
}
