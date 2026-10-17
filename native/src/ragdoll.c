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
//   /ragdoll blood [on|off]  on (default): those bodies get the blood the game paints on the blast victims it keeps
//   /ragdoll dismember [on|off|limbs <min> [max]|big <0-100>|head <0-100|on|off>|pieces on|off|type <n|auto>|
//                      impulse <n>]  off by default: blast victims lose min-max limbs (default 2-3, a different limb
//                            each, big cuts big% of the time) through the game's MutilationState, each thrown as a piece;
//                            head: the share of victims that lose their head (default 50; see "dismemberment")
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
// own_traced (build 29): the game's calls made inside our one call at depth 1 are traced like its own (what
// OnRep_MutilationState does); our nested calls (depth 2+, e.g. the trace's own checks) stay out
static int own_traced;
static int own_call(void) { return own_depth && own_tid == GetCurrentThreadId() && !(own_traced && own_depth == 1); }
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
    uint8_t blast_ok;                      // build 27: pushed by a recorded explosion, from blast_at
    uint8_t blast_i;                       // build 32: that explosion's slot in blasts[] (its tally)
    uint32_t hid;                          // build 32: physics bodies under a hidden bone (bit per body): out of the rest test
    float blast_at[3];
    uint8_t dism, velp;                    // build 29: limbs taken off by /ragdoll dismember; speed probes done (bits)
    uint8_t limbp;                         // build 31: limb probe: 1 at the dismember, 2 also at +0.5 s
    uint8_t painted;                       // build 33: the blood paint was looked at for it (spared blood bags)
    float push_dv;                         // build 29: cm/s the blast push gave every body (0 = no blast push)
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

static void pieces_reset(void);
static int world_check(void) {   // new map: every old pointer is gone
    static UObject *last_w;
    UObject *w = ue_world();
    if (w != last_w) { last_w = w; ntr = 0; scan_at = 0; n_body_lists = 0; world_gz = 0; pieces_reset(); }
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
                                       "flinch", "force", "momentum", "mutil", NULL};   // mutil: build 27
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

// build 29: objects the game constructs inside our OnRep_MutilationState call (gib pieces, sprays), while tracing
static int dism_watch, dism_built_logs;
#define DISM_BUILT_LOGS 80
static void dism_built(UClass *c, UObject *r, UObject *outer) {
    if (dism_built_logs >= DISM_BUILT_LOGS) return;
    dism_built_logs++;
    char a[96], b[96], d[96];
    LOG("ragdoll dismember:   constructed %s (%s) in %s", ue_obj_name(r, a, sizeof a), ue_obj_name(c, b, sizeof b),
        outer ? ue_obj_name(outer, d, sizeof d) : "-");
}

static UObject *sco_detour(UClass *c, UObject *outer, FName name, uint32_t fl, uint32_t ifl, UObject *tmpl, uint8_t copy,
                           void *graph, uint8_t arch) {
    UObject *r = orig_sco(c, outer, name, fl, ifl, tmpl, copy, graph, arch);
    if (dism_watch && c && r && GetCurrentThreadId() == game_tid) dism_built(c, r, outer);
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
        // (build 25: exact names; build 23 printed the material VectorParameterValue structs as 3 floats)
        if (!strcmp(b, "Vector") || !strncmp(b, "Vector_NetQuantize", 18))
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
typedef struct {
    double t; float at[3];
    int bodies, asked, off, refused, game_pc, our_pc, pc_fail;   // build 32: /ragdoll dismember's tally of this explosion
    int big, small, heads, heads_off;                            // build 33: cut sizes; head test
    char refused_s[160];
    uint8_t told, painted;                                       // build 33: painted = the blood paint was done
} Blast;
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
                                           "materialinstancedynamic", "staticmesh", NULL};   // staticmesh: gib pieces (build 27)
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
// Build 26: only what is actually hidden, and never its children. Builds 21-25 unhid the mesh with
// bPropagateToChildren, which also turned on Damage_Capsule (a CapsuleComponent attached to the mesh, hidden in game
// like every collision shape): the engine then draws its outline, the "dark red/pink spherical arcs" that stuck to
// spared bodies until the swap (Joon, 16 Oct; they started with build 21). Every log since build 21 says the explosion
// code never hides the body (actor hidden 0, mesh visible 1), so normally nothing is changed here at all.
static int comp_hidden_in_game(UObject *c) {
    FField *f = c ? ue_find_prop(U_CLASS(c), "bHiddenInGame") : NULL;
    return f ? bool_at(f, (const uint8_t *)c) : -1;
}

static void bag_unhide(Tracked *t, UObject *mesh) {
    char a[160];
    Call k;
    int vis = call_bool(mesh, "IsVisible"), mhid = comp_hidden_in_game(mesh);
    int32_t ho = ue_prop_offset(t->a, "bHidden");
    FField *hf = ho >= 0 ? ue_find_prop(U_CLASS(t->a), "bHidden") : NULL;
    int hidden = hf ? bool_at(hf, (const uint8_t *)t->a) : -1;
    if (hidden == 1 && call_prep(&k, t->a, "SetActorHiddenInGame")) { SET(&k, "bNewHidden", uint8_t, 0); call_go(&k); }
    if (mhid == 1 && call_prep(&k, mesh, "SetHiddenInGame")) { SET(&k, "NewHidden", uint8_t, 0); SET(&k, "bPropagateToChildren", uint8_t, 0); call_go(&k); }
    if (vis == 0 && call_prep(&k, mesh, "SetVisibility")) { SET(&k, "bNewVisibility", uint8_t, 1); SET(&k, "bPropagateToChildren", uint8_t, 0); call_go(&k); }
    if (bag_logs < 40) {
        bag_logs++;
        LOG("ragdoll bloodbag: %s spared: actor hidden %d, mesh hidden in game %d, mesh visible %d -> %d%s", oname(t->a, a, sizeof a),
            hidden, mhid, vis, call_bool(mesh, "IsVisible"), hidden == 1 || mhid == 1 || vis == 0 ? " (shown before the ragdoll)" : " (nothing to show)");
    }
}

// a component's (or object's) own properties with values, its class chain down to ActorComponent left out (build 25;
// used for the flying piece and the mutilation config)
static void log_props(const char *who, const char *what, UObject *o) {
    char b[128], pn[96], v[700];
    for (UStruct *s = o ? (UStruct *)U_CLASS(o) : NULL; s; s = US_SUPER(s)) {
        ue_obj_name(s, b, sizeof b);
        if (!strcmp(b, "ActorComponent") || !strcmp(b, "SceneComponent") || !strcmp(b, "Object") || !strcmp(b, "Actor")) break;
        for (FField *p = US_CHILDPROPS(s); p; p = FF_NEXT(p)) {
            fmt_prop(p, (const uint8_t *)o, v, sizeof v, 2);
            LOG("ragdoll fxprobe %s:   %s %s.%s = %s", who, what, b, ue_name(FF_NAME(p), pn, sizeof pn), v);
        }
    }
}

static void pe_detour(UObject *o, UFunction *fn, void *params) {
    if (fn == explode_fn && explode_fn && params && mode_on && blast_loc_f) {   // game thread
        Blast *b = &blasts[blast_at++ % 8];
        memset(b, 0, sizeof *b);
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
static void dismember(Tracked *t, UObject *mesh, float dv, Blast *bl);
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
        if (bi >= 0) {
            memcpy(org, blasts[bi].at, sizeof org); from = "the explosion";
            memcpy(t->blast_at, org, sizeof org); t->blast_ok = 1; t->blast_i = (uint8_t)bi;
        }
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
        // build 29: limbs off first (/ragdoll dismember on), then the push. Build 28 pushed first, and bodies that
        // lost limbs barely moved (Joon, 16 Oct 23:40): whatever the game's limb code does to the body, it is done
        // before the impulse now, and our physics settings are put back after it.
        dismember(t, mesh, dv, t->blast_ok ? &blasts[t->blast_i % 8] : NULL);
        if (!call_prep(&k, mesh, "AddRadialImpulse")) { n_push_fail++; return; }
        float *o = carg(&k, "Origin");
        if (o) memcpy(o, org, sizeof org);
        SET(&k, "Radius", float, len + 300.f);   // reaches every part of the body
        SET(&k, "Strength", float, dv);
        SET(&k, "Falloff", uint8_t, 0);           // RIF_Constant: the distance is in dv already
        SET(&k, "bVelChange", uint8_t, 1);
        call_go(&k);
        t->push_dv = dv;
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

// the game's animation off the body (montages stopped, physical animation motors at 0) / every body simulating, full
// physics in the pose: apply(), and again after the game's limb code (build 29)
static void anim_off(UObject *ai, UObject *pac) {
    Call k;
    if (ai && call_prep(&k, ai, "Montage_Stop")) { SET(&k, "InBlendOutTime", float, 0.f); call_go(&k); }
    if (pac && call_prep(&k, pac, "SetStrengthMultiplyer")) { SET(&k, "InStrengthMultiplyer", float, 0.f); call_go(&k); }
}
static void sim_on(UObject *mesh) {
    Call k;
    if (call_prep(&k, mesh, "SetAllBodiesSimulatePhysics")) { SET(&k, "bNewSimulate", uint8_t, 1); call_go(&k); }
    if (call_prep(&k, mesh, "SetSimulatePhysics")) { SET(&k, "bSimulate", uint8_t, 1); call_go(&k); }
    if (call_prep(&k, mesh, "SetPhysicsBlendWeight")) { SET(&k, "PhysicsBlendWeight", float, 1.f); call_go(&k); }
}

// ---- dismemberment (build 27 discovery; build 32: several limbs, varied, a piece thrown for each) ----
// MutilationComponent (builds 23-26 probes): `MutilationState`, a replicated array of MutilationInfo {Region, Type,
// Impulse, ImpactPoint, SourceActor} that the game acts on in OnRep_MutilationState (clients too), and
// `DismembermentMeshPieces`, one per limb region: a static mesh (Common_Gib_*_SM) at a bone, with a blood spray. There
// is no callable "dismember" function, so with /ragdoll dismember on a blast victim gets entries appended (Type
// BreakOff, looked up by name in the game's EMutilationType; Impulse away from the blast) and OnRep_MutilationState is
// called here, once per limb, before the push (build 29: limbs taken off after the push took the push away). On the
// host the array also replicates, so clients see the same limbs go. What builds 28-31 showed:
//   - the game takes each limb off (IsRegionDismembered 1, the bones from the cut down hidden, a gibcap on the stump)
//     and appends its own copy of each entry it applies;
//   - it refuses a second arm: (LeftLowerArm, RightLowerArm) gave the second 0 in every run (builds 28, 30, 31);
//   - it throws one flying piece (Common_DismemberedPieceStaticMesh_BP) per explosion at most, for the first limb of
//     the frame; every other limb just vanished behind its gibcap: 31 of 34 limbs came off in build 31's run, and Joon
//     could see only "some";
//   - the hidden part's physics bodies keep moving, which kept the settle test holding (one body to the 10 s cap).
// Build 32 (Joon, 17 Oct: more than one limb, and variety): per body dism_min..dism_max distinct limbs (left/right
// arm/leg, from the region enum's names), each at a random depth among the body's own pieces (upper arm or forearm;
// thigh, calf or foot); a refused region tries the limb's other depths, then the next limb; every limb off whose piece
// the game didn't throw gets one from us (the game's piece class, the piece's mesh and spray, thrown away from the
// blast, PIECE_LIFE s, at most PIECES_MAX at once); the settle test leaves out bodies under hidden bones; one tally
// line per explosion.
// Build 32's run (Joon, 17 Oct 11:01-11:10, 40 bodies logged): the game cuts an "Upper" region at the elbow / knee
// (forearm and hand / shin and foot off) and every other limb region at the wrist / ankle; nothing at the shoulder or
// hip. It refused only second arms, never a big cut. The stand-in already keeps the cuts (12 of 12), so build 32's
// carry-over is gone. Build 33 (Joon: mostly big cuts): each limb gets its big cut first dism_big% of the time (default
// 75), else its small one (the other depth stays the fallback when refused). The flying pieces: the config names two
// classes (DismemberedPieceSkelMeshActor, DismemberedPieceStaticMeshActor) and build 32 took the skeletal one, so none
// of ours flew; now the static one, for pieces that have a static mesh (all of a Common's). Head test (Joon: vanilla
// explosions take some heads): with `head` set, that share of victims also gets a Head entry (Head is in the config's
// ExplodeRegions; a Common has no head piece), BreakOff, then Rupture if refused, logged. Build 33's run: the game took
// every head (BreakOff accepted, cut at neck_02, no piece thrown) and Joon saw them explode; build 34: half by default.
// Build 33's blood paint took (spared bodies' Drench Bias 0.20 -> 0.42, as the kept victims'); its pieces hung in the
// air (see piece_learn).
#define ADDR_GMALLOC VA(0x1469E59F0ull)     // FMalloc* GMalloc; vtable +0x20 Realloc(this, ptr, size, align) (as models.c)
typedef void *(*GmReallocFn)(void *self, void *p, size_t n, uint32_t align);
static void *rd_realloc(void *p, size_t n) {
    void *gm = *(void **)ADDR_GMALLOC;
    return gm ? ((GmReallocFn)(*(void ***)gm)[4])(gm, p, n, 16) : NULL;
}
// an array the engine filled for us (a call's out parameter or return value): freed the way TArray frees, Realloc to 0
static void rd_free_array(TArray *a) {
    if (a && a->data) rd_realloc(a->data, 0);
    if (a) { a->data = NULL; a->num = a->max = 0; }
}

static UObject *gs_cdo(void) { UClass *c = cls("GameplayStatics"); return c ? UC_CDO(c) : NULL; }
// build 33: our own random numbers (xorshift64*, seeded from the clock at first use) instead of the C runtime's rand():
// its state may be shared with the game's own FMath::Rand (UE's dynamic CRT), and our draws shouldn't shift the game's
static uint64_t rng_s;
static uint32_t rnd(void) {
    if (!rng_s) {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        rng_s = ((uint64_t)c.QuadPart * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)GetCurrentProcessId() << 32) ^ GetTickCount64();
        if (!rng_s) rng_s = 1;
    }
    rng_s ^= rng_s >> 12; rng_s ^= rng_s << 25; rng_s ^= rng_s >> 27;
    return (uint32_t)((rng_s * 0x2545F4914F6CDD1Dull) >> 32);
}
static int rnd_below(int n) { return n > 1 ? (int)(rnd() % (uint32_t)n) : 0; }   // 0 .. n-1
static float frand(void) { return (float)(rnd() >> 8) / 16777216.f; }           // 0 .. <1

// UGameplayStatics::GetAllActorsOfClass: the live actors of a class (the engine's class hash, no object-array scan).
// `out` (may be NULL) gets up to `max` of them; returns how many there are, -1 if the call is missing
static int actors_of(UClass *c, UObject **out, int max) {
    UObject *gs = gs_cdo();
    Call k;
    if (!c || !gs || !call_prep(&k, gs, "GetAllActorsOfClass")) return -1;
    SET(&k, "WorldContextObject", UObject *, ue_world());
    SET(&k, "ActorClass", UObject *, (UObject *)c);
    call_go(&k);
    TArray *r = carg(&k, "OutActors");
    if (!r) return -1;
    int n = r->num;
    for (int i = 0; out && r->data && i < n && i < max; i++) out[i] = ((UObject **)r->data)[i];
    rd_free_array(r);
    return n;
}

static int dism_on;                  // /ragdoll dismember on|off
static int dism_min = 2, dism_max = 3;   // build 32: limbs per blast victim (/ragdoll dismember limbs <min> [max])
static int dism_pieces = 1;          // build 32: our piece for every limb the game throws none for
static int dism_big = 75;            // build 33: % of limbs whose big cut (an "Upper" region) is tried first
static int dism_head = 50;           // % of blast victims that also lose the head (build 33 test; build 34: Joon, half)
static int dism_type = -1;           // MutilationInfo.Type set by hand (/ragdoll dismember type <n>), -1 = BreakOff
static int dism_type_set;            // set by hand
static float dism_imp;               // |Impulse| to write: last seen, or set by hand (0 = 5000)
static int dism_imp_set, dism_logs, mstate_logs, n_dism, n_dism_fail, dism_traced;
static int n_limbs_off, n_limbs_refused, n_pieces_game, n_pieces_ours, n_pieces_fail;
static int n_big, n_small, n_heads, n_heads_off, head_logs, piece_fail_logs;
static int dism_probed;              // build 31: bodies given the limb probe since /ragdoll dismember on|off
static struct {
    int state;                       // 0 not looked up, 1 ok, -1 missing (logged)
    FField *arr, *pieces;
    int32_t el, pel;
    FField *region, *type, *impulse, *point, *source, *p_region, *p_bone;
    FField *p_sm, *p_ps, *p_sock;    // a piece's StaticMesh, ParticleSystem, ParticleAttachSocketName (build 32, optional)
    int breakoff;                    // EMutilationType::BreakOff (a limb off, as the Tallboy's arm: build 28), -1 unknown
    int rupture, head;               // build 33: EMutilationType::Rupture, EMutilationRegion::Head; -1 unknown
} mi;
enum { LIMB_LARM, LIMB_RARM, LIMB_LLEG, LIMB_RLEG, N_LIMBS };
static int8_t region_limb[256];      // EMutilationRegion value -> limb, -1 none (Head, Torso, Legs ...): from the names
static uint8_t region_big[256];      // build 33: 1 = the limb's big cut (an "Upper" region: elbow / knee, build 32 log)

// the names of an enum property's values, so the log says what Type 2 means. Layout as tools/sdkdump.py (which dumped
// all 1442 enums) and docs/NOTES.md: FByteProperty::Enum and FEnumProperty::UnderlyingProp at +0x78, FEnumProperty::Enum
// at +0x80; UEnum::Names (TArray<TPair<FName, int64>>, 16-byte entries) at +0x48, stock 0x40 + 8 for this engine's
// bigger UObject. (Build 27 read +0x40, CppType's length fields, as the array and crashed the game at the first body.)
static UObject *prop_enum(FField *p) {   // the property's UEnum, NULL if it isn't one we can check
    char t[64], b[96];
    ue_name(*(FName *)FF_CLASS(p), t, sizeof t);
    UObject *e = !strcmp(t, "EnumProperty") ? *(UObject **)((char *)p + 0x80) : !strcmp(t, "ByteProperty") ? FP_SUB(p) : NULL;
    if (!e || U_INDEX(e) < 0 || U_INDEX(e) >= ue_num_objects() || ue_object_at(U_INDEX(e)) != e) return NULL;
    cname(e, b, sizeof b);
    return !strcmp(b, "Enum") || !strcmp(b, "UserDefinedEnum") ? e : NULL;
}
static const TArray *enum_list(UObject *e) {   // its Names, NULL if the size looks wrong
    const TArray *names = e ? (const TArray *)((const char *)e + 0x48) : NULL;
    return names && names->data && names->num > 0 && names->num <= 256 && names->max >= names->num ? names : NULL;
}
static const char *enum_entry(const TArray *names, int i, char *b, size_t n, int64_t *v) {   // name after "Enum::"
    const uint8_t *pr = (const uint8_t *)names->data + (size_t)i * 16;
    ue_name(*(const FName *)pr, b, n);
    if (v) *v = *(const int64_t *)(pr + 8);
    const char *sh = strstr(b, "::");
    return sh ? sh + 2 : b;
}
static void enum_names(FField *p, char *out, size_t n) {
    char b[96];
    UObject *e = prop_enum(p);
    const TArray *names = enum_list(e);
    if (!names) { snprintf(out, n, "?"); return; }
    size_t k = (size_t)snprintf(out, n, "%s:", ue_obj_name(e, b, sizeof b));
    for (int i = 0; i < names->num && i < 40 && k + 8 < n; i++) {
        int64_t v;
        const char *s = enum_entry(names, i, b, sizeof b, &v);
        k += (size_t)snprintf(out + k, n - k, " %s=%lld", s, (long long)v);
    }
}
static int enum_value(FField *p, const char *name) {   // the value named `name` (after "Enum::"), -1 if none
    char b[96];
    const TArray *names = enum_list(prop_enum(p));
    for (int i = 0; names && i < names->num; i++) {
        int64_t v;
        if (!strcmp(enum_entry(names, i, b, sizeof b, &v), name)) return (int)v;
    }
    return -1;
}
static const char *region_name(int r, char *out, size_t n) {   // "LeftLowerArm", or the number
    char b[96];
    const TArray *names = mi.region ? enum_list(prop_enum(mi.region)) : NULL;
    for (int i = 0; names && i < names->num; i++) {
        int64_t v;
        const char *s = enum_entry(names, i, b, sizeof b, &v);
        if (v == r) { snprintf(out, n, "%s", s); return out; }
    }
    snprintf(out, n, "%d", r);
    return out;
}

static void mi_init(UClass *mc) {   // MutilationComponent's class
    if (mi.state || !mc) return;
    mi.state = -1;
    mi.breakoff = mi.rupture = mi.head = -1;
    memset(region_limb, -1, sizeof region_limb);
    memset(region_big, 0, sizeof region_big);
    mi.arr = ue_find_prop(mc, "MutilationState");
    mi.pieces = ue_find_prop(mc, "DismembermentMeshPieces");
    FField *in = mi.arr ? *(FField **)((char *)mi.arr + 0x78) : NULL, *pin = mi.pieces ? *(FField **)((char *)mi.pieces + 0x78) : NULL;
    UStruct *st = in ? (UStruct *)FP_SUB(in) : NULL, *pst = pin ? (UStruct *)FP_SUB(pin) : NULL;
    if (!st || !pst) { LOG("ragdoll dismember: MutilationState / DismembermentMeshPieces not found: off"); return; }
    mi.el = FP_ELSIZE(in); mi.pel = FP_ELSIZE(pin);
    mi.region = ue_find_prop(st, "Region"); mi.type = ue_find_prop(st, "Type"); mi.impulse = ue_find_prop(st, "Impulse");
    mi.point = ue_find_prop(st, "ImpactPoint"); mi.source = ue_find_prop(st, "SourceActor");
    mi.p_region = ue_find_prop(pst, "Region"); mi.p_bone = ue_find_prop(pst, "StaticMeshAttachName");
    mi.p_sm = ue_find_prop(pst, "StaticMesh"); mi.p_ps = ue_find_prop(pst, "ParticleSystem");
    mi.p_sock = ue_find_prop(pst, "ParticleAttachSocketName");
    if (!mi.region || !mi.type || !mi.impulse || !mi.point || !mi.p_region || !mi.p_bone || FP_ELSIZE(mi.region) != 1 ||
        FP_ELSIZE(mi.type) != 1 || FP_ELSIZE(mi.p_region) != 1 || mi.el <= 0 || mi.el > 256 || mi.pel <= 0) {
        LOG("ragdoll dismember: unexpected MutilationInfo / DismembermentMeshPiece layout: off");
        return;
    }
    mi.state = 1;
    LOG("ragdoll dismember: MutilationInfo %d bytes: Region@0x%x Type@0x%x Impulse@0x%x ImpactPoint@0x%x SourceActor@0x%x; "
        "piece %d bytes: Region@0x%x bone@0x%x mesh@0x%x spray@0x%x socket@0x%x", mi.el, FP_OFFSET(mi.region), FP_OFFSET(mi.type),
        FP_OFFSET(mi.impulse), FP_OFFSET(mi.point), mi.source ? FP_OFFSET(mi.source) : -1, mi.pel, FP_OFFSET(mi.p_region),
        FP_OFFSET(mi.p_bone), mi.p_sm ? FP_OFFSET(mi.p_sm) : -1, mi.p_ps ? FP_OFFSET(mi.p_ps) : -1, mi.p_sock ? FP_OFFSET(mi.p_sock) : -1);
    char en[1024], lm[400], b[96];
    enum_names(mi.region, en, sizeof en);
    LOG("ragdoll dismember: Region %s", en);
    // limbs by name: Left/Right + Arm/Hand or Leg/Foot (Legs, Head, Torso... are none)
    static const char *const LIMB_NAME[N_LIMBS] = {"left arm", "right arm", "left leg", "right leg"};
    const TArray *names = enum_list(prop_enum(mi.region));
    size_t q = 0;
    lm[0] = 0;
    for (int i = 0; names && i < names->num; i++) {
        int64_t v;
        const char *s = enum_entry(names, i, b, sizeof b, &v);
        int left = !strncmp(s, "Left", 4), right = !strncmp(s, "Right", 5);
        int arm = strstr(s, "Arm") || strstr(s, "Hand"), leg = strstr(s, "Leg") || strstr(s, "Foot");
        if (v < 0 || v > 255 || !(left || right) || !(arm || leg)) continue;
        region_limb[v] = (int8_t)(arm ? (left ? LIMB_LARM : LIMB_RARM) : (left ? LIMB_LLEG : LIMB_RLEG));
        region_big[v] = strstr(s, "Upper") != NULL;
        q += (size_t)snprintf(lm + q, sizeof lm - q, "%s%s=%s%s", q ? ", " : "", s, LIMB_NAME[region_limb[v]],
                              region_big[v] ? " (big)" : "");
    }
    LOG("ragdoll dismember: limbs: %s", q ? lm : "NONE (no Left/Right Arm/Leg region names): nothing will be taken off");
    mi.head = enum_value(mi.region, "Head");
    enum_names(mi.type, en, sizeof en);
    mi.breakoff = enum_value(mi.type, "BreakOff");
    mi.rupture = enum_value(mi.type, "Rupture");
    LOG("ragdoll dismember: Type %s; limbs off = BreakOff = %d (Rupture %d); Head = %d", en, mi.breakoff, mi.rupture, mi.head);
}

// at ragdoll time, every body: what the game wrote (learns the impulse size from real entries; the Tallboy's arm: 0)
static void mstate_note(Tracked *t) {
    UObject *mut = comp_of(t->a, "MutilationComponent");
    if (!mut) return;
    mi_init(U_CLASS(mut));
    if (mi.state != 1) return;
    const TArray *arr = (const TArray *)((const uint8_t *)mut + FP_OFFSET(mi.arr));
    if (!arr->data || arr->num <= 0) return;
    char a[160], c[96], v[400];
    for (int i = 0; i < arr->num && i < 8; i++) {
        const uint8_t *e = (const uint8_t *)arr->data + (size_t)i * mi.el;
        const float *im = (const float *)(e + FP_OFFSET(mi.impulse));
        float len = sqrtf(im[0] * im[0] + im[1] * im[1] + im[2] * im[2]);
        if (!dism_imp_set && len > 0.f) dism_imp = len;
        if (mstate_logs < 30) {
            fmt_prop(*(FField **)((char *)mi.arr + 0x78), e, v, sizeof v, 1);
            LOG("ragdoll dismember: %s (%s) at death: MutilationState[%d] %s, |Impulse| %.0f", oname(t->a, a, sizeof a),
                cname(t->a, c, sizeof c), i, v, len);
        }
    }
    mstate_logs++;
}

// a new zeroed entry at the end of MutilationState, the array grown with the engine's allocator if full (the game
// appends its own copy of each entry inside OnRep_MutilationState: build 30), NULL if out of memory
static uint8_t *mstate_add(TArray *arr) {
    if (arr->num + 1 > arr->max) {
        int32_t nmax = arr->num + 4;
        void *d = rd_realloc(arr->data, (size_t)nmax * (size_t)mi.el);
        if (!d) return NULL;
        arr->data = d;
        arr->max = nmax;
    }
    uint8_t *e = (uint8_t *)arr->data + (size_t)arr->num * (size_t)mi.el;
    memset(e, 0, (size_t)mi.el);
    arr->num++;
    return e;
}

// the game's limb code on this machine; while `watch`, its calls and constructions go into the log
static int mstate_onrep(UObject *mut, int watch) {
    if (watch) { own_traced = 1; dism_watch = 1; }
    int called = call_void(mut, "OnRep_MutilationState");
    own_traced = 0;
    dism_watch = 0;
    return called;
}

static int region_off(UObject *mut, uint8_t region) {   // IsRegionDismembered: 1/0, -1 unknown
    Call k;
    if (!call_prep(&k, mut, "IsRegionDismembered")) return -1;
    void *pa = carg(&k, "Region");
    if (!pa) return -1;
    *(uint8_t *)pa = region;
    call_go(&k);
    uint8_t *rv = carg(&k, "ReturnValue");
    return rv ? *rv != 0 : -1;
}

// ---- the cut: which bones the game hid ----
static int bone_hidden(UObject *mesh, FName bone) {   // 1/0, -1 unknown
    Call k;
    if (!call_prep(&k, mesh, "IsBoneHiddenByName")) return -1;
    SET(&k, "BoneName", FName, bone);
    call_go(&k);
    uint8_t *r = carg(&k, "ReturnValue");
    return r ? *r != 0 : -1;
}
// the hidden bones whose parent isn't hidden: where each limb was cut (children of a hidden bone are hidden too)
static int hidden_cuts(UObject *mesh, FName *out, int max) {
    Call k, p;
    if (!mesh || !call_prep(&k, mesh, "GetBoneName") || !call_prep(&p, mesh, "GetParentBone")) return 0;
    int32_t *bi = carg(&k, "BoneIndex");
    FName *bn = carg(&k, "ReturnValue"), *pin = carg(&p, "BoneName"), *pout = carg(&p, "ReturnValue");
    int nb = 0, n = 0;
    Call c;
    if (call_prep(&c, mesh, "GetNumBones")) { call_go(&c); int32_t *r = carg(&c, "ReturnValue"); nb = r ? *r : 0; }
    if (!bi || !bn || !pin || !pout) return 0;
    for (int i = 0; i < nb && i < 512 && n < max; i++) {
        *bi = i; bn->idx = bn->num = 0;
        call_go(&k);
        FName b = *bn;
        if (!b.idx || bone_hidden(mesh, b) != 1) continue;
        *pin = b; pout->idx = pout->num = 0;
        call_go(&p);
        if (pout->idx && bone_hidden(mesh, *pout) == 1) continue;
        out[n++] = b;
    }
    return n;
}
static void names_list(const FName *b, int n, char *out, size_t m) {
    char t[64];
    size_t q = 0;
    out[0] = 0;
    for (int i = 0; i < n && q + 8 < m; i++) q += (size_t)snprintf(out + q, m - q, "%s%s", i ? " " : "", ue_name(b[i], t, sizeof t));
    if (!n) snprintf(out, m, "none");
}

// the physics bodies under a hidden bone (bit per body of the physics asset's list): the parts that came off still
// simulate, unseen, and flop about; the settle test leaves them out (build 31: a body held to the 10 s cap by its
// hidden foot)
static uint32_t hidden_bodies(UObject *mesh) {
    const BodyList *bl = bodies_of(mesh);
    uint32_t m = 0;
    for (int i = 0; bl && i < bl->n && i < 32; i++) if (bone_hidden(mesh, bl->bone[i]) == 1) m |= 1u << i;
    return m;
}

// build 31: what the game did to a dismembered body: where it was cut, the static-mesh components on the body (its
// gibcaps), and the flying pieces in the world. First 3 bodies per /ragdoll dismember on|off, at once and 0.5 s later.
static void limb_probe(Tracked *t, const char *when, const char *regs) {
    char a[160], b[96], c[96], d[96], sk[96], cuts[400];
    UObject *mesh = t->mesh;
    FName cut[16];
    names_list(cut, hidden_cuts(mesh, cut, 16), cuts, sizeof cuts);
    LOG("ragdoll limbs: %s %s (regions %s): cut at %s", oname(t->a, a, sizeof a), when, regs, cuts);
    UClass *smc = cls("StaticMeshComponent");
    Call k;
    if (!smc || !call_prep(&k, t->a, "K2_GetComponentsByClass")) return;
    SET(&k, "ComponentClass", UClass *, smc);
    call_go(&k);
    TArray *r = carg(&k, "ReturnValue");
    for (int i = 0; r && r->data && i < r->num && i < 16; i++) {
        UObject *comp = ((UObject **)r->data)[i];
        if (!comp) continue;
        FName sock = {0, 0};
        Call s2;
        if (call_prep(&s2, comp, "GetAttachSocketName")) { call_go(&s2); FName *sr = carg(&s2, "ReturnValue"); if (sr) sock = *sr; }
        UObject *par = ue_get_ptr(comp, "AttachParent");
        LOG("ragdoll limbs: %s %s:   static mesh component %s: mesh %s, attached to %s at %s, visible %d, hidden in game %d",
            oname(t->a, a, sizeof a), when, oname(comp, b, sizeof b), oname(ue_get_ptr(comp, "StaticMesh"), c, sizeof c),
            oname(par, d, sizeof d), sock.idx ? ue_name(sock, sk, sizeof sk) : "-", call_bool(comp, "IsVisible"), comp_hidden_in_game(comp));
    }
    rd_free_array(r);
}

// ---- pieces: the game's flying-piece actor, ours for the limbs it throws none for ----
// The class: the body's MutilationConfig names two (build 32 log, its variables logged once): DismemberedPieceStaticMeshActor
// (Common_DismemberedPieceStaticMesh_BP_C: the one the game throws for a static-mesh piece, every piece of a Common) and
// DismemberedPieceSkelMeshActor (skeletal pieces: not thrown by us). Build 32 took the first class named like
// *DismemberedPiece*, the skeletal one, so every piece of ours failed. Without that variable:
// Common_DismemberedPieceStaticMesh_BP_C by name (an object-array scan, at most once every 5 s while not found).
// Blueprint classes go with their map.
#define PIECE_CLASS  "Common_DismemberedPieceStaticMesh_BP_C"
#define PIECE_LIFE   30.f   // s: our pieces' life span
#define PIECES_MAX   48     // our pieces at once: the oldest goes first
static struct { UObject *cfg; int32_t ci; UClass *c; int32_t cli; } pcls[8];
static int n_pcls;
static UClass *pcls_byname;
static int32_t pcls_byname_i;
static double pcls_try;
static struct { UObject *a; int32_t ai; } pieces[PIECES_MAX];
static int piece_at;

static void pieces_reset(void) {   // new map (world_check)
    n_pcls = 0; pcls_byname = NULL; pcls_try = 0; piece_at = 0;
    memset(pieces, 0, sizeof pieces);
}

static UClass *piece_class(UObject *mut) {
    char b[160], t[64];
    UObject *cfg = ue_get_ptr(mut, "Config");
    for (int i = 0; cfg && i < n_pcls; i++)
        if (pcls[i].cfg == cfg && ue_object_usable(cfg, pcls[i].ci)) {
            if (pcls[i].c && ue_object_usable(pcls[i].c, pcls[i].cli)) return pcls[i].c;
            if (!pcls[i].c) goto by_name;   // this config names no piece class
        }
    if (cfg && n_pcls < 8) {
        UClass *found = NULL;
        static UClass *class_cls;
        if (!class_cls) class_cls = cls("Class");
        FField *p = ue_find_prop(U_CLASS(cfg), "DismemberedPieceStaticMeshActor");
        if (p) ue_name(*(FName *)FF_CLASS(p), t, sizeof t);
        if (p && (!strcmp(t, "ClassProperty") || !strcmp(t, "ObjectProperty"))) {   // a plain class pointer
            UObject *v = *(UObject **)((char *)cfg + FP_OFFSET(p));
            if (v && class_cls && ue_is_a(v, class_cls)) found = (UClass *)v;
        }
        pcls[n_pcls].cfg = cfg; pcls[n_pcls].ci = U_INDEX(cfg); pcls[n_pcls].c = found; pcls[n_pcls].cli = found ? U_INDEX(found) : -1;
        n_pcls++;
        static int cfg_logged;
        if (!cfg_logged++) { LOG("ragdoll dismember: the body's MutilationConfig %s:", oname(cfg, b, sizeof b)); log_props("dismember", "config", cfg); }
        LOG("ragdoll dismember: piece class from %s.DismemberedPieceStaticMeshActor: %s", oname(cfg, b, sizeof b),
            found ? ue_obj_name(found, t, sizeof t) : "none there");
        if (found) return found;
    }
by_name:
    if (pcls_byname && ue_object_usable(pcls_byname, pcls_byname_i)) return pcls_byname;
    pcls_byname = NULL;
    double now = now_s();
    if (now - pcls_try < 5.0) return NULL;
    pcls_try = now;
    pcls_byname = ue_find_class(PIECE_CLASS);
    pcls_byname_i = pcls_byname ? U_INDEX(pcls_byname) : -1;
    static int logged;
    if (pcls_byname && !logged++) LOG("ragdoll dismember: piece class %s found by name", PIECE_CLASS);
    return pcls_byname;
}

// one piece of `pe` (a DismembermentMeshPiece) thrown from where its bone is now, away from the blast at `speed`
// cm/s give or take, spinning, with its blood spray; 0 = thrown, else a PF_* reason (piece_why)
enum { PF_CLASS = 1, PF_CALLS, PF_SPAWN, PF_COMP, PF_MESH, PF_PHYS };
static const char *piece_why(int r) {
    static const char *const W[] = {"thrown", "no piece class loaded", "a call is missing", "the spawn failed",
                                    "the piece has no static mesh component", "the component refused the mesh",
                                    "its physics wouldn't start (removed)"};
    return r >= 0 && r <= PF_PHYS ? W[r] : "?";
}

// Build 33's pieces all hung where the limb had been (Joon, 17 Oct 11:41: "left frozen where it was"; log: simulating 0
// on every one): the piece actor comes inert, and the game sets its component up when it throws one. So ours get what
// the game's own thrown piece has: its collision profile (read once off a simulating piece of the game's; the corpses'
// "Ragdoll" until one is seen), physics collision on, mobility Movable; a piece whose physics still won't start is
// removed rather than left hanging, and the first ones log the component's state.
static int comp_byte(UObject *c, const char *fn) {   // a uint8 return value (GetCollisionEnabled), -1 = none
    Call k;
    if (!c || !call_prep(&k, c, fn)) return -1;
    call_go(&k);
    uint8_t *r = carg(&k, "ReturnValue");
    return r ? *r : -1;
}
static int comp_mobility(UObject *c) { int32_t o = c ? ue_prop_offset(c, "Mobility") : -1; return o < 0 ? -1 : *((uint8_t *)c + o); }
static int is_our_piece(UObject *a) {
    for (int i = 0; i < PIECES_MAX; i++) if (pieces[i].a == a) return 1;
    return 0;
}
static FName piece_prof;            // the game's thrown piece's collision profile, once seen
static void piece_learn(UClass *pc) {   // from a piece of the game's that simulates (one is thrown per explosion)
    if (piece_prof.idx || !pc) return;
    UObject *list[64];
    int n = actors_of(pc, list, 64);
    for (int i = 0; i < n && i < 64; i++) {
        UObject *comp = is_our_piece(list[i]) ? NULL : ue_get_ptr(list[i], "StaticMesh");
        if (!comp || call_bool(comp, "IsSimulatingPhysics") != 1) continue;
        char a[160], b[96];
        piece_prof = collision_profile(comp);
        LOG("ragdoll dismember: the game's thrown piece %s: collision profile %s, collision enabled %d, mobility %d (2 = movable)",
            oname(list[i], a, sizeof a), ue_name(piece_prof, b, sizeof b), comp_byte(comp, "GetCollisionEnabled"), comp_mobility(comp));
        return;
    }
}
static int throw_piece(UClass *pc, UObject *mesh, const uint8_t *pe, const float blast[3], float speed) {
    UObject *sm = mi.p_sm ? *(UObject *const *)(pe + FP_OFFSET(mi.p_sm)) : NULL;
    UObject *ps = mi.p_ps ? *(UObject *const *)(pe + FP_OFFSET(mi.p_ps)) : NULL;
    FName bone = *(const FName *)(pe + FP_OFFSET(mi.p_bone));
    FName sock = mi.p_sock ? *(const FName *)(pe + FP_OFFSET(mi.p_sock)) : (FName){0, 0};
    UObject *gs = gs_cdo();
    Call k, fin;
    if (!pc) return PF_CLASS;
    if (!sm || !gs || !call_prep(&k, mesh, "GetSocketTransform") || !call_prep(&fin, gs, "FinishSpawningActor")) return PF_CALLS;
    SET(&k, "InSocketName", FName, bone);
    SET(&k, "TransformSpace", uint8_t, 0);   // RTS_World
    call_go(&k);
    float *xr = carg(&k, "ReturnValue");
    if (!xr) return PF_CALLS;
    float xf[12];   // FTransform: quat, translation (+pad), scale (+pad)
    memcpy(xf, xr, sizeof xf);
    if (xf[0] * xf[0] + xf[1] * xf[1] + xf[2] * xf[2] + xf[3] * xf[3] < 0.5f) { xf[0] = xf[1] = xf[2] = 0.f; xf[3] = 1.f; }
    xf[7] = 0.f; xf[8] = xf[9] = xf[10] = 1.f; xf[11] = 0.f;
    if (!call_prep(&k, gs, "BeginDeferredActorSpawnFromClass")) return PF_CALLS;
    SET(&k, "WorldContextObject", UObject *, ue_world());
    SET(&k, "ActorClass", UObject *, (UObject *)pc);
    void *st = carg(&k, "SpawnTransform");
    if (st) memcpy(st, xf, sizeof xf);
    SET(&k, "CollisionHandlingOverride", uint8_t, 1);   // AlwaysSpawn
    call_go(&k);
    UObject **rv = carg(&k, "ReturnValue"), *act = rv ? *rv : NULL;
    if (!act) return PF_SPAWN;
    if (call_prep(&k, act, "SetReplicates")) { SET(&k, "bInReplicates", uint8_t, 0); call_go(&k); }   // ours stay on this machine
    SET(&fin, "Actor", UObject *, act);
    st = carg(&fin, "SpawnTransform");
    if (st) memcpy(st, xf, sizeof xf);
    call_go(&fin);
    UObject *comp = ue_get_ptr(act, "StaticMesh");
    if (!comp) comp = comp_of(act, "StaticMeshComponent");
    if (!comp) { call_void(act, "K2_DestroyActor"); return PF_COMP; }
    static int state_logs;
    char b0[96], b1[96];
    int mob0 = comp_mobility(comp), cen0 = comp_byte(comp, "GetCollisionEnabled");
    FName prof0 = collision_profile(comp);
    if (mob0 != 2 && call_prep(&k, comp, "SetMobility")) { SET(&k, "NewMobility", uint8_t, 2); call_go(&k); }   // Movable
    if (!call_prep(&k, comp, "SetStaticMesh")) { call_void(act, "K2_DestroyActor"); return PF_COMP; }
    SET(&k, "NewMesh", UObject *, sm);
    call_go(&k);
    if (ue_get_ptr(comp, "StaticMesh") != sm) { call_void(act, "K2_DestroyActor"); return PF_MESH; }
    static FName ragdoll_prof;
    if (!ragdoll_prof.idx) ragdoll_prof = fname_of(L"Ragdoll");
    FName prof = piece_prof.idx ? piece_prof : ragdoll_prof;
    if (call_prep(&k, comp, "SetCollisionProfileName")) { SET(&k, "InCollisionProfileName", FName, prof); call_go(&k); }
    int cen = comp_byte(comp, "GetCollisionEnabled");
    if (cen >= 0 && cen < 2 && call_prep(&k, comp, "SetCollisionEnabled")) {   // physics collision: needed to simulate
        SET(&k, "NewType", uint8_t, 3);   // QueryAndPhysics
        call_go(&k);
    }
    if (call_prep(&k, comp, "SetSimulatePhysics")) { SET(&k, "bSimulate", uint8_t, 1); call_go(&k); }
    int sim = call_bool(comp, "IsSimulatingPhysics");
    if (state_logs < 3 || sim != 1) {
        if (state_logs < 8) {
            state_logs++;
            LOG("ragdoll dismember: our piece: spawned with mobility %d, collision enabled %d, profile %s; now mobility %d, "
                "collision enabled %d, profile %s, simulating %d", mob0, cen0, ue_name(prof0, b0, sizeof b0), comp_mobility(comp),
                comp_byte(comp, "GetCollisionEnabled"), ue_name(collision_profile(comp), b1, sizeof b1), sim);
        }
    }
    if (sim != 1) { call_void(act, "K2_DestroyActor"); return PF_PHYS; }   // never left hanging in the air
    // away from the blast, spread out and a little up; 0.6-1.2 x the body's own push
    float d[3], len = 0.f;
    for (int c = 0; c < 3; c++) { d[c] = xf[4 + c] - blast[c]; len += d[c] * d[c]; }
    len = sqrtf(len);
    if (len < 1.f) { d[0] = d[1] = 0.f; d[2] = 1.f; len = 1.f; }
    for (int c = 0; c < 3; c++) d[c] = d[c] / len + (frand() * 2.f - 1.f) * 0.4f;
    d[2] += 0.35f;
    len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float v = speed * (0.6f + 0.6f * frand());
    if (call_prep(&k, comp, "SetPhysicsLinearVelocity")) {
        float *nv = carg(&k, "NewVel");
        if (nv) for (int c = 0; c < 3; c++) nv[c] = d[c] / len * v;
        call_go(&k);
    }
    if (call_prep(&k, comp, "SetPhysicsAngularVelocityInDegrees")) {
        float *av = carg(&k, "NewAngVel");
        if (av) for (int c = 0; c < 3; c++) av[c] = (frand() * 2.f - 1.f) * 720.f;
        call_go(&k);
    }
    if (ps && call_prep(&k, gs, "SpawnEmitterAttached")) {   // the piece's blood spray, at its socket if the mesh has one
        int has = 0;
        Call s;
        if (sock.idx && call_prep(&s, comp, "DoesSocketExist")) { SET(&s, "InSocketName", FName, sock); call_go(&s); uint8_t *r = carg(&s, "ReturnValue"); has = r && *r; }
        SET(&k, "EmitterTemplate", UObject *, ps);
        SET(&k, "AttachToComponent", UObject *, comp);
        if (has) SET(&k, "AttachPointName", FName, sock);
        float *sc = carg(&k, "Scale");
        if (sc) sc[0] = sc[1] = sc[2] = 1.f;
        SET(&k, "LocationType", uint8_t, 0);   // KeepRelativeOffset
        SET(&k, "bAutoDestroy", uint8_t, 1);
        SET(&k, "bAutoActivateSystem", uint8_t, 1);
        call_go(&k);
    }
    if (call_prep(&k, act, "SetLifeSpan")) { SET(&k, "InLifespan", float, PIECE_LIFE); call_go(&k); }
    // at most PIECES_MAX of ours at once: the oldest goes (its life span may have ended it already)
    int slot = piece_at++ % PIECES_MAX;
    if (pieces[slot].a && ue_object_usable(pieces[slot].a, pieces[slot].ai)) call_void(pieces[slot].a, "K2_DestroyActor");
    pieces[slot].a = act; pieces[slot].ai = U_INDEX(act);
    return 0;
}

static void pieces_probe(void) {   // every flying piece in the world (Common_DismemberedPieceStaticMesh_BP and kin)
    static double last;
    static int props_logged;
    static const char *const PIECE[] = {"dismemberedpiece", NULL};
    double now = now_s();
    if (now - last < 1.0) return;
    last = now;
    char a[160], b[96], c[96];
    int n = 0, shown = 0;
    int32_t no = ue_num_objects();
    for (int32_t i = 0; i < no; i++) {
        UObject *o = ue_object_at(i);
        if (!o || (U_FLAGS(o) & LIVE_FLAGS) || !U_CLASS(o) || !ue_object_usable(o, i)) continue;
        if (!name_has(cname(o, b, sizeof b), PIECE) || strstr(b, "Component")) continue;
        n++;
        if (shown >= 12) continue;
        shown++;
        float at[3] = {0, 0, 0};
        actor_loc(o, at);
        UObject *sm = ue_get_ptr(o, "StaticMesh");
        FField *hp = ue_find_prop(U_CLASS(o), "bHidden");
        float v[3] = {0, 0, 0};
        Call k;
        if (sm && call_prep(&k, sm, "GetPhysicsLinearVelocity")) { call_go(&k); float *r = carg(&k, "ReturnValue"); if (r) memcpy(v, r, sizeof v); }
        LOG("ragdoll limbs: piece %s (%s) at (%.0f %.0f %.0f): mesh %s, hidden %d, simulating %d, %.0f cm/s", oname(o, a, sizeof a), b,
            at[0], at[1], at[2], sm ? oname(ue_get_ptr(sm, "StaticMesh"), c, sizeof c) : "-", hp ? bool_at(hp, (const uint8_t *)o) : -1,
            sm ? call_bool(sm, "IsSimulatingPhysics") : -1, sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
        if (!props_logged++) log_props("limbs", "piece", o);
    }
    LOG("ragdoll limbs: %d flying pieces in the world", n);
}

static void limb_probe_tick(Tracked *t, double now) {   // from the tick: the probed bodies 0.5 s later
    if (t->limbp != 1 || now - t->died_at < 0.5) return;
    t->limbp = 2;
    limb_probe(t, "+0.5s", "as before");
    pieces_probe();
}

// ---- taking limbs off a blast victim ----
static void str_add(char *s, size_t n, const char *add) {
    size_t q = strlen(s);
    if (q + 3 < n) snprintf(s + q, n - q, "%s%s", q ? ", " : "", add);
}

static uint8_t piece_region(const TArray *pcs, int i) { return ((const uint8_t *)pcs->data + (size_t)i * mi.pel)[FP_OFFSET(mi.p_region)]; }

// one MutilationInfo entry for `region` (Impulse away from the blast at `mag`, ImpactPoint the blast), then the game's
// limb code; 1 = the region is off now (IsRegionDismembered, or the game appended its own copy), 0 = refused, -1 = no memory
static int mstate_apply(Tracked *t, UObject *mut, TArray *arr, uint8_t region, int type, const float at[3], float mag, int watch, int *called) {
    float d[3];
    for (int c = 0; c < 3; c++) d[c] = at[c] - t->blast_at[c];
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1.f) { d[0] = 0; d[1] = 0; d[2] = 1; len = 1.f; }
    int32_t n0 = arr->num;
    uint8_t *e = mstate_add(arr);
    if (!e) return -1;
    e[FP_OFFSET(mi.region)] = region;
    e[FP_OFFSET(mi.type)] = (uint8_t)type;
    float *im = (float *)(e + FP_OFFSET(mi.impulse)), *ip = (float *)(e + FP_OFFSET(mi.point));
    for (int c = 0; c < 3; c++) { im[c] = d[c] / len * mag; ip[c] = t->blast_at[c]; }
    *called = mstate_onrep(mut, watch);
    int off = region_off(mut, region);
    return off >= 0 ? off : arr->num > n0 + 1;   // no IsRegionDismembered: the game appends its copy of what it applies
}

// build 33: the head test (Joon: vanilla explosions take some heads off). Head is in the config's ExplodeRegions but a
// Common has no head piece, so: a Head entry with the limbs' Type (BreakOff), then Rupture if that is refused; what
// the game did (dismembered, its own copy appended, new cuts, a piece thrown) is logged for the first 20 bodies.
// 1 = the head is off (new hidden bones: something visibly gone)
static int head_try(Tracked *t, UObject *mut, TArray *arr, UObject *mesh, int type, float mag, UClass *pc) {
    char a[160], cut_s[300], res[200] = "";
    if (mi.head < 0 || mi.head > 255) return 0;
    FName before[24], after[24], fresh[24];
    int nb = hidden_cuts(mesh, before, 24), nf = 0, called = 0, off = 0, done = 0;
    int seen = pc ? actors_of(pc, NULL, 0) : -1;
    float at[3];
    static FName head_bone;
    if (!head_bone.idx) head_bone = fname_of(L"head");
    if (bone_loc(mesh, head_bone, at) && actor_loc(t->a, at)) return 0;
    const int types[2] = {type, mi.rupture};
    const char *const tn[2] = {"BreakOff", "Rupture"};
    for (int i = 0; i < 2 && !done; i++) {   // Rupture only if BreakOff was refused
        if (types[i] < 0 || (i && types[i] == type)) continue;
        int32_t n0 = arr->num;
        int r = mstate_apply(t, mut, arr, (uint8_t)mi.head, types[i], at, mag, 0, &called);
        if (r < 0) return 0;
        int na = hidden_cuts(mesh, after, 24);
        nf = 0;
        for (int j = 0; j < na; j++) {
            int old = 0;
            for (int k = 0; k < nb; k++) if (after[j].idx == before[k].idx && after[j].num == before[k].num) old = 1;
            if (!old && nf < 24) fresh[nf++] = after[j];
        }
        off = nf > 0;   // off = something visibly gone, not just the flag
        done = r == 1 || off;
        size_t q = strlen(res);
        snprintf(res + q, sizeof res - q, "%s%s: dismembered %d, the game's copy %s, %d new cuts", q ? "; " : "",
                 i ? tn[1] : (type == mi.breakoff ? tn[0] : "Type set"), region_off(mut, (uint8_t)mi.head),
                 arr->num > n0 + 1 ? "appended" : "not appended", nf);
    }
    int now_n = pc ? actors_of(pc, NULL, 0) : -1;
    n_heads++;
    if (off) n_heads_off++;
    if (head_logs < 20) {
        head_logs++;
        names_list(fresh, nf, cut_s, sizeof cut_s);
        LOG("ragdoll head: %s: %s; head %s, cut at %s; a piece by the game: %s", oname(t->a, a, sizeof a), res,
            off ? "OFF" : "still on", cut_s, seen < 0 || now_n < 0 ? "?" : now_n > seen ? "yes" : "no");
    }
    return off;
}

static void dismember(Tracked *t, UObject *mesh, float dv, Blast *bl) {
    char a[160], rn[48], tmp[96];
    if (!dism_on || !t->blast_ok || dism_max <= 0) return;
    UObject *mut = comp_of(t->a, "MutilationComponent");
    if (!mut) return;
    mi_init(U_CLASS(mut));
    if (mi.state != 1) return;
    int type = dism_type_set ? dism_type : mi.breakoff;
    if (type < 0) {
        if (dism_logs++ < 3) LOG("ragdoll dismember: the game's EMutilationType has no BreakOff: /ragdoll dismember type <n>");
        return;
    }
    TArray *arr = (TArray *)((uint8_t *)mut + FP_OFFSET(mi.arr));
    const TArray *pcs = (const TArray *)((const uint8_t *)mut + FP_OFFSET(mi.pieces));
    if (!pcs->data || pcs->num <= 0) return;
    // the body's pieces by limb, the limbs in a random order
    int by[N_LIMBS][8] = {{0}}, nb[N_LIMBS] = {0}, order[N_LIMBS], no = 0;
    for (int i = 0; i < pcs->num && i < 64; i++) {
        int l = region_limb[piece_region(pcs, i)];
        if (l >= 0 && nb[l] < 8) by[l][nb[l]++] = i;
    }
    for (int l = 0; l < N_LIMBS; l++) if (nb[l]) order[no++] = l;
    if (!no) return;
    for (int i = no - 1; i > 0; i--) { int j = rnd_below(i + 1), x = order[i]; order[i] = order[j]; order[j] = x; }
    int lo = dism_min < 1 ? 1 : dism_min, hi = dism_max < lo ? lo : dism_max;
    int want = lo + rnd_below(hi - lo + 1);
    if (want > no) want = no;
    float mag = dism_imp > 0.f ? dism_imp : 5000.f;
    UClass *pc = dism_pieces || dism_head ? piece_class(mut) : NULL;
    int seen = pc ? actors_of(pc, NULL, 0) : -1;   // pieces in the world: one more after a limb = the game threw it
    // the first 3 per /ragdoll dismember on, while a trace runs: the game's calls inside OnRep_MutilationState (and
    // what it constructs) go into the log as if the game had made the call (build 29: what takes the push away)
    int watch = tracing && dism_traced < 3, called = 0;
    if (watch) {
        dism_traced++;
        LOG("ragdoll dismember: %s: the game's calls inside OnRep_MutilationState follow (one call per region)", oname(t->a, a, sizeof a));
    }
    char offs[160] = "", refs[160] = "", regs[64] = "";
    int got = 0, refused = 0, big = 0, small = 0, game_pc = 0, our_pc = 0, pc_fail = 0, pc_skel = 0;
    int throw_ix[N_LIMBS], n_throw = 0;
    for (int oi = 0; oi < no && got < want; oi++) {
        // the limb's depths: its big cut first dism_big% of the time, else its small one; each size in a random
        // order, the other size after it (a refused depth: the limb's next one)
        int l = order[oi], tries[8], nt = 0, big_first = rnd_below(100) < dism_big;
        for (int pass = 0; pass < 2; pass++) {
            int want_big = pass ? !big_first : big_first, s0 = nt;
            for (int i = 0; i < nb[l]; i++) if (region_big[piece_region(pcs, by[l][i])] == want_big) tries[nt++] = by[l][i];
            for (int i = nt - 1; i > s0; i--) { int j = s0 + rnd_below(i - s0 + 1), x = tries[i]; tries[i] = tries[j]; tries[j] = x; }
        }
        for (int j = 0; j < nt; j++) {
            const uint8_t *pe = (const uint8_t *)pcs->data + (size_t)tries[j] * mi.pel;
            uint8_t region = pe[FP_OFFSET(mi.p_region)];
            FName bone = *(const FName *)(pe + FP_OFFSET(mi.p_bone));
            float at[3];
            if (bone_loc(mesh, bone, at) && actor_loc(t->a, at)) continue;
            int off = mstate_apply(t, mut, arr, region, type, at, mag, watch, &called);
            if (off < 0) { LOG("ragdoll dismember: allocation failed"); oi = no; break; }
            region_name(region, rn, sizeof rn);
            if (!off) { refused++; str_add(refs, sizeof refs, rn); continue; }   // (our entry stays: harmless, see build 31)
            got++;
            if (region_big[region]) big++; else small++;
            str_add(offs, sizeof offs, rn);
            snprintf(tmp, sizeof tmp, "%u", region);
            str_add(regs, sizeof regs, tmp);
            int now_n = pc ? actors_of(pc, NULL, 0) : -1;
            UObject *sm = mi.p_sm ? *(UObject *const *)(pe + FP_OFFSET(mi.p_sm)) : NULL;
            if (pc && seen >= 0 && now_n > seen) game_pc++;
            else if (!sm) pc_skel++;                        // a skeletal piece: not one of ours to throw
            else if (dism_pieces) throw_ix[n_throw++] = tries[j];
            if (now_n >= 0) seen = now_n;
            break;
        }
    }
    int head = dism_head > 0 && rnd_below(100) < dism_head ? 1 + head_try(t, mut, arr, mesh, type, mag, pc) : 0;   // 0 untried, 1 on, 2 off
    n_limbs_off += got;
    n_limbs_refused += refused;
    n_big += big; n_small += small;
    if (bl) {
        bl->bodies++; bl->asked += want; bl->off += got; bl->refused += refused; bl->big += big; bl->small += small;
        bl->heads += head > 0; bl->heads_off += head == 2;
        if (refused) str_add(bl->refused_s, sizeof bl->refused_s, refs);
    }
    if (!got && head != 2) {
        n_dism_fail++;
        if (dism_logs < 40) { dism_logs++; LOG("ragdoll dismember: %s: no limb came off (refused: %s)", oname(t->a, a, sizeof a), refs[0] ? refs : "none tried"); }
        return;
    }
    t->dism = 1;
    n_dism++;
    // whatever the limb code did to the body's physics: our settings again, before the push
    anim_off(ue_get_ptr(mesh, "AnimScriptInstance"), comp_of(t->a, "PhysicalAnimationComponent"));
    sim_on(mesh);
    call_void(mesh, "WakeAllRigidBodies");
    t->hid = hidden_bodies(mesh);
    // a piece for every limb the game threw none for (it throws one per explosion at most)
    if (!pc && n_throw) pc = piece_class(mut);   // the game may have loaded it inside its limb code just now
    if (n_throw) piece_learn(pc);
    for (int i = 0; i < n_throw; i++) {
        const uint8_t *pe = (const uint8_t *)pcs->data + (size_t)throw_ix[i] * mi.pel;
        int r = throw_piece(pc, mesh, pe, t->blast_at, dv > 0.f ? dv : 1000.f);
        if (!r) { our_pc++; continue; }
        pc_fail++;
        if (piece_fail_logs < 5) {
            piece_fail_logs++;
            region_name(pe[FP_OFFSET(mi.p_region)], rn, sizeof rn);
            LOG("ragdoll dismember: %s: our %s piece not thrown: %s (class %s)", oname(t->a, a, sizeof a), rn, piece_why(r),
                pc ? ue_obj_name(pc, tmp, sizeof tmp) : "none");
        }
    }
    n_pieces_game += game_pc; n_pieces_ours += our_pc; n_pieces_fail += pc_fail;
    if (bl) { bl->game_pc += game_pc; bl->our_pc += our_pc; bl->pc_fail += pc_fail; }
    if (watch) {
        char v[400];
        for (int i = 0; i < arr->num && i < 12; i++) {
            fmt_prop(*(FField **)((char *)mi.arr + 0x78), (const uint8_t *)arr->data + (size_t)i * mi.el, v, sizeof v, 1);
            LOG("ragdoll dismember: %s after OnRep: MutilationState[%d] %s", oname(t->a, a, sizeof a), i, v);
        }
    }
    if (dism_probed < 3) { dism_probed++; t->limbp = 1; limb_probe(t, "at the dismember", regs); }
    if (dism_logs < 40) {
        dism_logs++;
        LOG("ragdoll dismember: %s: %d of %d limbs off (%s; %d big, %d small)%s%s%s; pieces: %d by the game, %d by us%s%s; "
            "OnRep_MutilationState %s, state now %d entries, %d bodies under the cuts", oname(t->a, a, sizeof a), got, want, offs,
            big, small, refused ? ", refused " : "", refs, head == 2 ? ", head OFF" : head ? ", head still on" : "", game_pc,
            our_pc, pc_fail ? " (some failed)" : "", pc_skel ? " (skeletal pieces skipped)" : "", called ? "called" : "NOT FOUND",
            arr->num, __builtin_popcount(t->hid));
    }
}

// one line per explosion, half a second after it (all its victims are handled in the tick that sees them die)
static void blast_tally_tick(double now) {
    for (int i = 0; i < 8; i++) {
        Blast *b = &blasts[i];
        if (!b->bodies || b->told || now - b->t < 0.5) continue;
        b->told = 1;
        char hd[64] = "";
        if (b->heads) snprintf(hd, sizeof hd, "; heads off %d of %d tried", b->heads_off, b->heads);
        LOG("ragdoll dismember: explosion at (%.0f %.0f %.0f): %d bodies, %d of %d limbs off (%d big cuts, %d small)%s%s%s; "
            "pieces thrown: %d by the game, %d by us%s", b->at[0], b->at[1], b->at[2], b->bodies, b->off, b->asked, b->big,
            b->small, hd, b->refused ? "; refused: " : "", b->refused_s, b->game_pc, b->our_pc,
            b->pc_fail ? " (some of ours failed: see the log)" : "");
    }
}

// ---- blood paint on spared blood bags (build 33) ----
// Joon (17 Oct): "the vanilla game paints the surviving flying zombies with blood". Build 23: an explosion gives every
// victim Drench_Blood_Explosion_GE, and the ones it keeps as bodies (not juiced) also Drench_Blood_Juice_GE, right after
// its effects call; at +0.5 s their materials' "Drench Bias" was 0.38-0.41, against 0.20-0.22 on the bodies we spare
// from the blood bag (the game meant those gone, so they never get the juice one). DrenchComponent keeps IncomingDrench
// (DrenchReplicatedData {Type, InitialStrength, Parity}, replicated; Parity flips so an equal value still arrives). So,
// PAINT_AFTER s after an explosion that spared bodies: the strongest IncomingDrench among its kept victims (the game's
// own juice paint) is copied onto every spared body that has less, its Parity flipped, and OnRep_IncomingDrench called
// here (on a host the value also replicates to clients). No kept victim in that explosion: the last one seen (none yet:
// nothing). The first 3 explosions log both bodies' drench variables and their "Drench Bias" at the copy and 0.5 s
// later, which says whether it took. The drench component's and GameplayEffectsComponent's functions are logged once
// (the other way in: applying Drench_Blood_Juice_GE itself). No OnRep_IncomingDrench or another layout: off.
#define PAINT_AFTER 0.25
static int blood_on = 1;                 // /ragdoll blood on|off
static int paint_state;                  // 0 not looked up, 1 ok, -1 not possible (logged)
static int n_painted, n_paint_explosions, paint_logs, paint_probes;
static FField *dr_in, *dr_type, *dr_str, *dr_par;
static int32_t dr_size;
static uint8_t dr_last[64];              // the last kept victim's IncomingDrench (an explosion without one uses it)
static int dr_have;
static struct { UObject *a; int32_t ai; double at; char label[24]; } paint_watch[6];

static void log_funcs(UClass *c, const char *what) {   // every function of the class chain, with its parameters
    char b[96], sn[96], pn[64], pt[64], line[700];
    for (UStruct *s = c; s; s = US_SUPER(s)) {
        ue_obj_name(s, sn, sizeof sn);
        if (!strcmp(sn, "ActorComponent") || !strcmp(sn, "Object")) break;
        for (UObject *f = US_CHILDREN(s); f; f = UF_NEXT(f)) {
            size_t q = (size_t)snprintf(line, sizeof line, "ragdoll blood: %s fn %s.%s(", what, sn, ue_obj_name(f, b, sizeof b));
            for (FField *p = US_CHILDPROPS((UStruct *)f); p && q + 96 < sizeof line; p = FF_NEXT(p))
                q += (size_t)snprintf(line + q, sizeof line - q, "%s%s %s", p == US_CHILDPROPS((UStruct *)f) ? "" : ", ",
                                      ue_name(*(FName *)FF_CLASS(p), pt, sizeof pt), ue_name(FF_NAME(p), pn, sizeof pn));
            snprintf(line + q, sizeof line - q, ")");
            LOG("%s", line);
        }
    }
}

static int paint_resolve(UObject *dc) {
    char t[64], b[96], c[96];
    if (paint_state) return paint_state > 0;
    paint_state = -1;
    dr_in = ue_find_prop(U_CLASS(dc), "IncomingDrench");
    UStruct *st = dr_in && !strcmp(ue_name(*(FName *)FF_CLASS(dr_in), t, sizeof t), "StructProperty") ? (UStruct *)FP_SUB(dr_in) : NULL;
    dr_size = st ? FP_ELSIZE(dr_in) : 0;
    dr_str = st ? ue_find_prop(st, "InitialStrength") : NULL;
    dr_type = st ? ue_find_prop(st, "Type") : NULL;
    dr_par = st ? ue_find_prop(st, "Parity") : NULL;
    UFunction *rep = ue_find_function(U_CLASS(dc), "OnRep_IncomingDrench");
    // both components' functions, once either way: the other way in if the copy doesn't take (applying the GE itself)
    log_funcs(U_CLASS(dc), "drench");
    UClass *gec = cls("GameplayEffectsComponent");
    if (gec) log_funcs(gec, "effects");
    if (!st || dr_size <= 0 || dr_size > (int32_t)sizeof dr_last || !dr_str || FP_ELSIZE(dr_str) != 4 || !dr_par || !rep) {
        LOG("ragdoll blood: OFF: %s (%s): IncomingDrench %s, InitialStrength %s, Parity %s, OnRep_IncomingDrench %s",
            oname(dc, b, sizeof b), cname(dc, c, sizeof c), st ? "ok" : "not a struct", dr_str ? "ok" : "missing",
            dr_par ? "ok" : "missing", rep ? "ok" : "missing");
        return 0;
    }
    paint_state = 1;
    LOG("ragdoll blood: %s.IncomingDrench %s (%d bytes): Type@0x%x InitialStrength@0x%x Parity %s@0x%x", cname(dc, c, sizeof c),
        ue_obj_name(st, b, sizeof b), dr_size, dr_type ? FP_OFFSET(dr_type) : -1, FP_OFFSET(dr_str),
        ue_name(*(FName *)FF_CLASS(dr_par), t, sizeof t), FP_OFFSET(dr_par));
    return 1;
}

static void parity_from(uint8_t *s, const uint8_t *own) {   // s gets own's Parity, flipped
    char t[64];
    ue_name(*(FName *)FF_CLASS(dr_par), t, sizeof t);
    int32_t o = FP_OFFSET(dr_par);
    if (!strcmp(t, "BoolProperty") && FBP_SIZE(dr_par) == 1 && FBP_MASK(dr_par)) {
        uint8_t m = FBP_MASK(dr_par), *p = s + o + FBP_BYTEOFF(dr_par);
        *p = (uint8_t)(((*p & ~m) | (own[o + FBP_BYTEOFF(dr_par)] & m)) ^ m);
    } else if (!strcmp(t, "IntProperty")) {
        *(int32_t *)(s + o) = *(const int32_t *)(own + o) + 1;
    } else {
        s[o] = (uint8_t)(own[o] + 1);
    }
}

// the body materials' "Drench Bias" (build 23: slot 0 is Invisible_M, then the body's dynamic instances); -1 = none
static float drench_bias(UObject *mesh) {
    static FName nm;
    if (!nm.idx) nm = fname_of(L"Drench Bias");
    Call g, k;
    if (!mesh || !call_prep(&g, mesh, "GetMaterial")) return -1.f;
    for (int i = 1; i < 3; i++) {
        SET(&g, "ElementIndex", int32_t, i);
        call_go(&g);
        UObject **m = carg(&g, "ReturnValue");
        if (!m || !*m || !call_prep(&k, *m, "K2_GetScalarParameterValue")) continue;
        SET(&k, "ParameterName", FName, nm);
        call_go(&k);
        float *r = carg(&k, "ReturnValue");
        if (r) return *r;
    }
    return -1.f;
}

static void paint_probe(Tracked *x, const char *label, double now) {   // drench variables now, Drench Bias now and +0.5 s
    char a[160], who[200];
    snprintf(who, sizeof who, "%s (%s)", oname(x->a, a, sizeof a), label);
    UObject *dc = comp_of(x->a, "DrenchComponent");
    if (dc) log_props(who, "drench", dc);
    LOG("ragdoll blood: %s: Drench Bias %.2f", who, drench_bias(x->mesh));
    for (int i = 0; i < 6; i++)
        if (!paint_watch[i].a) {
            paint_watch[i].a = x->a; paint_watch[i].ai = x->ai; paint_watch[i].at = now + 0.5;
            snprintf(paint_watch[i].label, sizeof paint_watch[i].label, "%s", label);
            break;
        }
}

static void paint_tick(double now) {
    char a[160], b[160];
    for (int i = 0; i < 6; i++) {   // the +0.5 s look of the probed bodies
        if (!paint_watch[i].a || now < paint_watch[i].at) continue;
        UObject *pa = paint_watch[i].a;
        paint_watch[i].a = NULL;
        if (!ue_object_usable(pa, paint_watch[i].ai)) continue;
        Tracked *x = any_tracked(pa);
        LOG("ragdoll blood: %s (%s) +0.5 s: Drench Bias %.2f", oname(pa, a, sizeof a), paint_watch[i].label, x ? drench_bias(x->mesh) : -1.f);
    }
    if (!blood_on || !mode_on || paint_state < 0) return;
    if (!cls("DrenchComponent")) {   // (cls() caches only classes it finds: a missing one would be looked for every tick)
        paint_state = -1;
        LOG("ragdoll blood: OFF: no DrenchComponent class in this game");
        return;
    }
    for (int bi = 0; bi < 8; bi++) {
        Blast *bl = &blasts[bi];
        if (!bl->t || bl->painted || now - bl->t < PAINT_AFTER) continue;
        bl->painted = 1;
        if (now - bl->t > 3.0) continue;   // a stale slot (the ragdoll was off)
        Tracked *spared[64], *donor = NULL;
        int ns = 0;
        float dstr = 0.f;
        uint8_t don[64];
        for (int k = 0; k < ntr; k++) {
            Tracked *x = &tr[k];
            if (!x->blast_ok || x->blast_i % 8 != bi || !x->ragdolled || x->died_at < bl->t - 0.2 || !ue_object_usable(x->a, x->ai)) continue;
            UObject *dc = comp_of(x->a, "DrenchComponent");
            if (!dc || !paint_resolve(dc)) continue;
            const uint8_t *in = (const uint8_t *)dc + FP_OFFSET(dr_in);
            float s = *(const float *)(in + FP_OFFSET(dr_str));
            if (x->bag) { if (!x->painted && ns < 64) spared[ns++] = x; }
            else if (s > dstr) { dstr = s; memcpy(don, in, (size_t)dr_size); donor = x; }
        }
        if (paint_state < 0 || !ns) continue;
        const char *from = "a kept victim of this explosion";
        if (donor) { memcpy(dr_last, don, (size_t)dr_size); dr_have = 1; }
        else if (dr_have) { memcpy(don, dr_last, (size_t)dr_size); dstr = *(const float *)(don + FP_OFFSET(dr_str)); from = "the last kept victim seen"; }
        else {
            if (paint_logs < 12) { paint_logs++; LOG("ragdoll blood: explosion at (%.0f %.0f %.0f): %d spared bodies, no kept victim seen yet: not painted", bl->at[0], bl->at[1], bl->at[2], ns); }
            continue;
        }
        int probe = paint_probes < 3, done = 0, probed = 0;
        if (probe && donor) paint_probe(donor, "kept victim", now);
        float lo = 1e9f, hi = -1e9f;
        for (int k = 0; k < ns; k++) {
            Tracked *x = spared[k];
            x->painted = 1;
            UObject *dc = comp_of(x->a, "DrenchComponent");
            if (!dc) continue;
            uint8_t *in = (uint8_t *)dc + FP_OFFSET(dr_in), buf[64];
            float own = *(const float *)(in + FP_OFFSET(dr_str));
            if (own < lo) lo = own;
            if (own > hi) hi = own;
            if (own >= dstr) continue;   // as bloody as the kept ones already
            if (probe && !probed) LOG("ragdoll blood: %s (spared, before the copy): Drench Bias %.2f", oname(x->a, a, sizeof a), drench_bias(x->mesh));
            memcpy(buf, don, (size_t)dr_size);
            parity_from(buf, in);
            memcpy(in, buf, (size_t)dr_size);
            call_void(dc, "OnRep_IncomingDrench");
            done++;
            if (probe && !probed) { probed = 1; paint_probe(x, "spared, painted", now); }
        }
        if (probe) paint_probes++;
        n_painted += done;
        n_paint_explosions++;
        if (paint_logs < 12) {
            paint_logs++;
            LOG("ragdoll blood: explosion at (%.0f %.0f %.0f): %d of %d spared bodies painted like %s%s%s (Type %d, strength %.2f; "
                "theirs %.2f-%.2f)", bl->at[0], bl->at[1], bl->at[2], done, ns, from, donor ? " " : "",
                donor ? oname(donor->a, b, sizeof b) : "", dr_type ? don[FP_OFFSET(dr_type)] : -1, dstr, lo, hi);
        }
    }
}

// build 29: how fast blast-pushed bodies really travel, dismembered or not: the mean velocity of all their bodies
// (flailing limbs cancel out) 0.1 and 0.4 s after death against the push they got, and what could hold them back
// (a montage, physical-animation motors, physics off). 16 bodies of each kind, counted again at /ragdoll dismember.
#define VEL_LOGS 16
static int vel_logs_d, vel_logs_c;
static const double VEL_T[] = {0.1, 0.4};
static void vel_probe_tick(Tracked *x, double now) {
    for (int i = 0; i < (int)(sizeof VEL_T / sizeof VEL_T[0]); i++) {
        if ((x->velp & (1u << i)) || now - x->died_at < VEL_T[i]) continue;
        x->velp |= 1u << i;
        int *n = x->dism ? &vel_logs_d : &vel_logs_c;
        if (!i) { if (*n >= VEL_LOGS) { x->velp = 0x80; return; } (*n)++; }
        if (i == (int)(sizeof VEL_T / sizeof VEL_T[0]) - 1) x->velp |= 0x80;   // the last one: done with this body
        Call k;
        if (!x->mesh || !call_prep(&k, x->mesh, "GetPhysicsLinearVelocity")) return;
        FName *bn = carg(&k, "BoneName");
        float *v = carg(&k, "ReturnValue");
        if (!v) return;
        const BodyList *bl = bodies_of(x->mesh);
        int nb = bl && bn ? bl->n : 0, cnt = 0;
        float sum[3] = {0, 0, 0}, top = 0;
        for (int b = nb ? 0 : -1; b < nb; b++) {   // no list: the root body only
            if (b >= 0 && b < 32 && ((x->hid >> b) & 1)) continue;   // parts that came off
            if (b >= 0) *bn = bl->bone[b];
            call_go(&k);
            for (int c = 0; c < 3; c++) sum[c] += v[c];
            float sp = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (sp > top) top = sp;
            cnt++;
        }
        for (int c = 0; c < 3; c++) sum[c] /= (float)(cnt ? cnt : 1);
        UObject *ai = ue_get_ptr(x->mesh, "AnimScriptInstance"), *pac = comp_of(x->a, "PhysicalAnimationComponent");
        int32_t so = pac ? ue_prop_offset(pac, "StrengthMultiplyer") : -1;
        char a[160], pm[24];
        if (so >= 0) snprintf(pm, sizeof pm, "%.2f", *(float *)((char *)pac + so)); else snprintf(pm, sizeof pm, "?");
        LOG("ragdoll velocity: %s (%s) +%.1fs: bodies move %.0f cm/s on average (%.0f up), fastest %.0f; push %.0f cm/s; "
            "simulating %d, montage %d, physical animation x%s", oname(x->a, a, sizeof a), x->dism ? "dismembered" : "not dismembered",
            VEL_T[i], sqrtf(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]), sum[2], top, x->push_dv,
            call_bool(x->mesh, "IsSimulatingPhysics"), ai ? call_bool(ai, "IsAnyMontagePlaying") : -1, pm);
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
    anim_off(ai, pac);
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
    sim_on(mesh);
    if (mass_scale != 1.f) set_mass_scale(t, mesh, mass_scale);
    call_void(mesh, "WakeAllRigidBodies");

    int sim = call_bool(mesh, "IsSimulatingPhysics");
    if (detail) {
        if (detail_left >= DETAIL_LOGS - 2) watch_start(t->a, mesh);
        log_mesh(mesh, "after");
        LOG("ragdoll after:   montage playing %d", ai ? call_bool(ai, "IsAnyMontagePlaying") : -1);
    }
    if (sim != 1) { LOG("ragdoll: %s is not simulating after the calls", oname(t->a, a, sizeof a)); return -1; }
    mstate_note(t);       // build 27: what the game wrote on the body (limbs it took off)
    push_body(t, mesh);   // step 2: the killing hit throws the body (blast victims: /ragdoll dismember first)
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
    t->dism = 0; t->hid = 0; t->blast_ok = 0; t->painted = 0;
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
        if (i >= 0 && i < 32 && ((x->hid >> i) & 1)) continue;   // build 32: a part that came off (unseen, flopping)
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
    blast_tally_tick(t);
    paint_tick(t);
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
            if (x->ragdolled && x->push_dv > 0.f && !(x->velp & 0x80) && !x->ended) vel_probe_tick(x, t);
            if (x->ragdolled && x->limbp == 1 && !x->ended) limb_probe_tick(x, t);
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
                      "%d without a hit, %d failed; blood bags %s: %d spared, blood %s: %d painted; %ld stacks logged%s%s\n",
                   mode_on ? "on" : "off", mode_on && delay_s > 0 ? " (after the game's animation)" : "", ntr, n_deaths,
                   n_applied, n_left, n_failed, n_revived,
                   settle_state == 1 ? "on" : settle_state ? "OFF (see log)" : "not armed yet", n_held, n_at_rest, n_capped,
                   SETTLE_CAP, released ? settle_sum / released : 0.0, settle_max, n_held_gone,
                   corpse_state == 1 ? "hook on" : corpse_state ? "hook OFF (see log)" : "not armed yet", n_spared,
                   n_spared_again, deadline_s, n_ended, n_ended_unchecked, mass_scale, grav_scale, n_grav_fail ? " (NO EFFECT: see log)" : "",
                   push_scale, n_pushed, n_push_radial, n_push_nohit, n_push_fail,
                   bloodbag_on ? "on" : bag_state < 0 ? "off but NOT AVAILABLE (see log)" : bag_state ? "off" : "off (not armed yet)",
                   n_bags_spared, blood_on ? (paint_state < 0 ? "NOT AVAILABLE" : "on") : "off", n_painted, stack_id,
                   tracing ? ", tracing" : "", n_overflow ? ", tracking list full" : "");
    } else if (!strcmp(verb, "on")) {
        float d = arg ? (float)atof(arg) : 0.f;
        delay_s = d < 0 ? 0 : d > 10 ? 10 : d;
        mode_on = 1;
        detail_left = DETAIL_LOGS;
        settle_logs = 0;
        push_logs = hit_dumps = left_logs = bag_logs = 0;
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
    } else if (!strcmp(verb, "dismember")) {
        char *arg2 = arg ? strtok(NULL, " ") : NULL, *arg3 = arg2 ? strtok(NULL, " ") : NULL;
        if (arg && (!strcmp(arg, "on") || !strcmp(arg, "off"))) {
            dism_on = !strcmp(arg, "on");
            dism_logs = dism_traced = dism_built_logs = vel_logs_d = vel_logs_c = dism_probed = head_logs = piece_fail_logs = 0;
        }
        else if (arg && !strcmp(arg, "limbs") && arg2 && isdigit((unsigned char)arg2[0])) {   // limbs <min> [max], 1-4
            int lo = atoi(arg2), hi = arg3 ? atoi(arg3) : lo;
            lo = lo < 1 ? 1 : lo > N_LIMBS ? N_LIMBS : lo;
            hi = hi < 1 ? 1 : hi > N_LIMBS ? N_LIMBS : hi;
            if (hi < lo) { int x = lo; lo = hi; hi = x; }
            dism_min = lo; dism_max = hi;
        }
        else if (arg && !strcmp(arg, "pieces") && arg2 && (!strcmp(arg2, "on") || !strcmp(arg2, "off"))) dism_pieces = !strcmp(arg2, "on");
        else if (arg && !strcmp(arg, "big") && arg2 && isdigit((unsigned char)arg2[0])) {   // % of limbs cut big first
            int v = atoi(arg2);
            dism_big = v < 0 ? 0 : v > 100 ? 100 : v;
        }
        else if (arg && !strcmp(arg, "head") && arg2 && (isdigit((unsigned char)arg2[0]) || !strcmp(arg2, "on") || !strcmp(arg2, "off"))) {
            int v = !strcmp(arg2, "on") ? 50 : !strcmp(arg2, "off") ? 0 : atoi(arg2);   // % of victims
            dism_head = v < 0 ? 0 : v > 100 ? 100 : v;
            head_logs = 0;
        }
        else if (arg && !strcmp(arg, "type") && arg2) {   // a number, or "auto" for BreakOff again
            int v = strcmp(arg2, "auto") ? atoi(arg2) : -1;
            dism_type = v < 0 ? -1 : v > 255 ? 255 : v;
            dism_type_set = dism_type >= 0;
        }
        else if (arg && !strcmp(arg, "impulse") && arg2) { float v = (float)atof(arg2); dism_imp = v < 0 ? 0 : v; dism_imp_set = v > 0; }
        else if (arg) {
            out_printf(o, "usage: /ragdoll dismember [on|off|limbs <min> [max]|big <0-100>|head <0-100|on|off>|pieces on|off|"
                          "type <n|auto>|impulse <n>]\n");
            return;
        }
        mi_init(cls("MutilationComponent"));   // the BreakOff value and the limbs, from the game's enums (once)
        int type = dism_type_set ? dism_type : mi.breakoff;
        const char *src = dism_type_set ? " (set)" : type >= 0 ? " (BreakOff, from the game)" : " (BreakOff not found: /ragdoll dismember type <n>)";
        if (arg) LOG("ragdoll dismember: %s, %d-%d limbs, big cuts %d%%, head %d%%, pieces of ours %s, Type %d%s, impulse %.0f%s",
                     dism_on ? "on" : "off", dism_min, dism_max, dism_big, dism_head, dism_pieces ? "on" : "off", type, src,
                     dism_imp > 0 ? dism_imp : 5000.f, dism_imp_set ? " (set)" : dism_imp > 0 ? " (seen)" : " (default)");
        out_printf(o, "/ragdoll dismember: %s (default off): blast victims lose %d-%d limbs (limbs <min> [max], 1-4; one arm at "
                      "most, the game refuses a second), a different limb each; big cuts (elbow, knee) %d%% of the time, else "
                      "wrist or ankle (big <0-100>, default 75); heads off on %d%% of victims (head <0-100|on|off>, default 50); "
                      "pieces of ours %s (a flying piece for each limb the game throws none for); Type %d%s, impulse %.0f; so far "
                      "%d bodies, %d limbs off (%d big, %d small), %d refused, heads off %d of %d tried, pieces %d by the game + "
                      "%d by us (%d failed)\n", dism_on ? "on" : "off", dism_min, dism_max, dism_big, dism_head,
                   dism_pieces ? "on" : "off", type, src, dism_imp > 0 ? dism_imp : 5000.f, n_dism, n_limbs_off, n_big, n_small,
                   n_limbs_refused, n_heads_off, n_heads, n_pieces_game, n_pieces_ours, n_pieces_fail);
    } else if (!strcmp(verb, "blood")) {
        if (arg && (!strcmp(arg, "on") || !strcmp(arg, "off"))) {
            blood_on = !strcmp(arg, "on");
            paint_logs = paint_probes = 0;
            LOG("ragdoll blood: %s", blood_on ? "on" : "off");
        } else if (arg) { out_printf(o, "usage: /ragdoll blood [on|off]\n"); return; }
        out_printf(o, "/ragdoll blood: %s (default on): bodies spared from the blood bag get the blood the game paints on the "
                      "blast victims it keeps%s; so far %d bodies painted in %d explosions\n", blood_on ? "on" : "off",
                   paint_state < 0 ? "; NOT AVAILABLE in this game (see log)" : "", n_painted, n_paint_explosions);
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
        out_printf(o, "usage: /ragdoll [on [delay]|off|status|mass [x]|gravity [x]|deadline [s]|push [x|on|off|<kind> <n>]|bloodbag [on|off]|blood [on|off]|dismember [on|off|limbs <min> [max]|big <0-100>|head <0-100|on|off>|pieces on|off|type <n|auto>|impulse <n>]|probe|trace [secs]]\n");
    }
}

int ragdoll_init(void) {
    LOG("ragdoll: experimental module loaded (fork build), off until /ragdoll on");
    return 0;
}
