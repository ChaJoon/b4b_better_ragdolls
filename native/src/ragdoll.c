// Ragdoll deaths for the Ridden (experimental, fork module). Local and cosmetic: every machine ragdolls the bodies it
// draws, nothing is sent over the network, so no protocol change (players without it see the game's own deaths).
//
// Step 1: physics falls instead of the canned death animations. Step 2: the killing hit throws the body (shotgun
// blasts, explosions), from the game's own record of the hit (see "step 2").
//
// Chat command /ragdoll (anyone, host or client; per machine). Build 36: every setting below is saved in b4bcoop.ini
// when changed (see "saved settings" at the end), /ragdoll on included: it is on again at the next game start.
//   /ragdoll                 status
//   /ragdoll on [delay]      ragdoll Ridden that die from now on; delay = seconds of the game's own death animation
//                            first (default 0 = physics at once)
//   /ragdoll off
//   /ragdoll mass [x]        every body's mass x the scale (0.1-10, default 1), for bodies that die from now on
//   /ragdoll gravity [x]     gravity on our ragdolls x the scale (0-5, default 1.3), at once, bodies in flight included
//   /ragdoll deadline [s]    every ragdoll still there s seconds after death is removed (12-120, default 15)
//   /ragdoll push [x]        the killing hit throws the body (step 2): overall strength x (0-10, default 4, off = 0);
//   /ragdoll push <kind> <n> one weapon kind's strength in kg cm/s (HG, SMG, AR, LMG, SG, Sni, FragGrenade, default, ...)
//   /ragdoll shot [x]        shots on a body that is still a ragdoll shove it: the shooter's weapon kind's strength from
//                            the push table x this (0-10, default 2, off = 0; see "shots on dead bodies")
//   /ragdoll carry [x]       build 38: a Ridden that dies running keeps its speed: x its running speed (0-2, default 1,
//                            off = 0; see "momentum carry")
//   /ragdoll carry keep <s>  build 39: how long a run's speed survives a stagger before the kill (0-3, default 1)
//   /ragdoll wounds [on|off|kinds <K,K>|small <K,K>|heads <K,K>|living off|arms|all|blast on|off]   build 40, on by
//                            default: shots by SG, Sni, LMG, AR take off or open what they hit; HG, SMG only heads; blast
//                            victims get 1-2 open wounds (see "wounds by weapon")
//   /ragdoll bloodbag [on|off] off (default): Ridden an explosion would burst stay bodies (ragdolled, pushed)
//   /ragdoll blood [on|off]  on (default): those bodies get the blood the game paints on the blast victims it keeps
//   /ragdoll dismember [on|off|limbs <min> [max]|big <0-100>|head <0-100|on|off>|pieces on|off|type <n|auto>|
//                      impulse <n>]  on by default (build 36): blast victims lose min-max limbs (default 2-3, a different limb
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
    float mass;                            // build 35: the whole body's kg for the shot shoves (0 = not read yet)
    uint16_t shots;                        // build 35: shoves from shots after death
    uint8_t relcount;                      // build 35: settle-hold releases (a shot can hold a body again)
    // build 38: momentum carry (the muscle-tone experiment of build 38 was removed in build 40)
    UObject *mov;                          // its CharacterMovement (at scan time): the running speed while alive
    int32_t movi;                          // its object index (still the same object?), as ai / lsi
    float run_v[3];                        // the movement's velocity at the last tick it was alive, cm/s
    double run_t;                          // when that was read (0 = never)
    float mem_v[3];                        // build 39: the fastest recent speed (fades over /ragdoll carry keep)
    double mem_t;                          // when it was read (0 = never)
    float carry;                           // cm/s of running speed given to the ragdoll (0 = none)
    char kind[24];                         // the killing hit's kind (a push table key), "" = not known
    UObject *mut;                          // build 41: its MutilationComponent (looked up at its first OnDamage)
    int32_t muti;                          // its object index
    double dmg_t;                          // build 41: its last OnDamage event (host), 0 = none seen
    char dmg_kind[24];                     // that hit's kind
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
static void shot_reset(void);
static int world_check(void) {   // new map: every old pointer is gone
    static UObject *last_w;
    UObject *w = ue_world();
    if (w != last_w) { last_w = w; ntr = 0; scan_at = 0; n_body_lists = 0; world_gz = 0; pieces_reset(); shot_reset(); }
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
static void dmg_learn(UObject *actor);                                // build 40: wounds on living Ridden (below)
static void cap_living(Tracked *x);                                   // build 42: the game's own open wounds off (below)
static void wound_hook(UObject *o, UFunction *fn, void *params);
static int wound_hook_wanted(void);

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
        tr[ntr].mov = ue_get_ptr(o, "CharacterMovement");   // build 38: its running speed is read every tick
        tr[ntr].movi = tr[ntr].mov ? U_INDEX(tr[ntr].mov) : -1;
        ntr++;
        dmg_learn(o);   // build 40: its class's OnDamage event (living hits)
        cap_living(&tr[ntr - 1]);   // build 42: before its first hit
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
enum { ST_SPAWN, ST_UNPOSSESS, ST_ENDPLAY, ST_DESTROYED, ST_CORPSE, ST_MUTIL, ST_N };   // ST_MUTIL: build 41
static const char *const ST_TAG[ST_N] = {"spawn", "unpossess", "endplay", "destroyed", "corpse", "mutilate"};
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

// ---- freeze watchdog (build 43) ----
// Joon's build 42 session froze (25 Oct 08:17:50): the picture stopped, the last sounds looped, the log's last line the
// engine's "Collecting garbage", nothing of ours around it. A stuck game thread can't log, so a thread of ours watches
// the tick count (ragdoll_tick runs every engine tick): no tick for FREEZE_S seconds, and again at FREEZE_S2, and the
// game thread is suspended just long enough to copy its registers and walk its stack, then resumed, then the frames go
// into the log as "ragdoll freeze #n" lines: static VAs with each function's start and first bytes (as the other
// stacks), ours as b4bcoop+offset, anything else as its image base + offset (names in a last line). No loader or heap
// lock is taken while the thread is suspended (it may hold one): the unwind entries come from each image's own .pdata,
// found from VirtualQuery's AllocationBase, and RtlVirtualUnwind only reads the stack. A loading screen can also stop
// the tick for that long: then the stack shows the loading, and "ticks again" follows.
#define FREEZE_S      10
#define FREEZE_S2     40
#define FREEZE_FRAMES 64
static unsigned tick_n;   // ragdoll_tick
static volatile LONG freeze_id;

static int mem_ok(uintptr_t p, size_t n) {   // committed, readable memory
    MEMORY_BASIC_INFORMATION mb;
    if (p < 0x10000 || !VirtualQuery((const void *)p, &mb, sizeof mb) || mb.State != MEM_COMMIT ||
        (mb.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return p + n <= (uintptr_t)mb.BaseAddress + mb.RegionSize;
}
// the RUNTIME_FUNCTION covering pc in its image's .pdata (indirect entries followed), *img its image base; NULL = none
// (a leaf, or not an image). No loader lock: VirtualQuery gives the image, the headers give the table.
static PRUNTIME_FUNCTION pdata_find(uintptr_t pc, uintptr_t *img) {
    MEMORY_BASIC_INFORMATION mb;
    if (!VirtualQuery((const void *)pc, &mb, sizeof mb) || mb.Type != MEM_IMAGE || !mb.AllocationBase) return NULL;
    uintptr_t base = (uintptr_t)mb.AllocationBase;
    const IMAGE_DOS_HEADER *d = (const IMAGE_DOS_HEADER *)base;
    if (!mem_ok(base, sizeof *d) || d->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + (uint32_t)d->e_lfanew);
    if (!mem_ok((uintptr_t)nt, sizeof *nt) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION) return NULL;
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!dd.VirtualAddress || dd.Size < sizeof(RUNTIME_FUNCTION) || !mem_ok(base + dd.VirtualAddress, dd.Size)) return NULL;
    PRUNTIME_FUNCTION rf = (PRUNTIME_FUNCTION)(base + dd.VirtualAddress);
    DWORD rva = (DWORD)(pc - base), lo = 0, hi = dd.Size / sizeof *rf;
    while (lo < hi) {
        DWORD m = lo + (hi - lo) / 2;
        if (rva < rf[m].BeginAddress) hi = m;
        else if (rva >= rf[m].EndAddress) lo = m + 1;
        else {
            PRUNTIME_FUNCTION e = &rf[m];
            for (int g = 0; g < 4 && (e->UnwindData & 1); g++) e = (PRUNTIME_FUNCTION)(base + (e->UnwindData & ~1u));
            *img = base;
            return e;
        }
    }
    return NULL;
}
static uintptr_t pdata_fn_start(uintptr_t pc) {   // its function's start, chained entries followed (as fn_start)
    uintptr_t base = 0;
    PRUNTIME_FUNCTION rf = pdata_find(pc, &base);
    for (int guard = 0; rf && guard < 8; guard++) {
        const uint8_t *ui = (const uint8_t *)(base + rf->UnwindData);
        if (!mem_ok((uintptr_t)ui, 4) || !((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
        rf = (PRUNTIME_FUNCTION)(ui + 4 + 2 * ((ui[2] + 1u) & ~1u));
    }
    return rf ? base + rf->BeginAddress : 0;
}
static void freeze_stack(DWORD tid, int secs) {
    uintptr_t pc[FREEZE_FRAMES], img[FREEZE_FRAMES];
    int n = 0;
    const char *why = NULL;
    if (!exe_lo) module_range(GetModuleHandleW(NULL), &exe_lo, &exe_hi);
    if (!self_lo) {
        HMODULE self = NULL;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)(void *)freeze_stack, &self)) module_range(self, &self_lo, &self_hi);
    }
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) { LOG("ragdoll freeze: no game tick for %d s; the game thread %lu can't be opened (%lu)", secs, tid, GetLastError()); return; }
    if (SuspendThread(h) == (DWORD)-1) {
        LOG("ragdoll freeze: no game tick for %d s; the game thread %lu can't be suspended (%lu)", secs, tid, GetLastError());
        CloseHandle(h);
        return;
    }
    CONTEXT c;   // suspended: copy and walk, nothing that may lock
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(h, &c)) {
        while (n < FREEZE_FRAMES && c.Rip) {
            uintptr_t base = 0;
            pc[n] = (uintptr_t)c.Rip;
            PRUNTIME_FUNCTION rf = pdata_find((uintptr_t)c.Rip, &base);
            img[n++] = base;
            if (rf) {
                void *hd = NULL;
                DWORD64 ef = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &hd, &ef, NULL);
            } else {   // a leaf: the return address is at rsp
                if ((c.Rsp & 7) || !mem_ok((uintptr_t)c.Rsp, 8)) break;
                c.Rip = *(const DWORD64 *)c.Rsp;
                c.Rsp += 8;
            }
        }
    } else why = "GetThreadContext failed";
    ResumeThread(h);
    CloseHandle(h);
    LONG id = InterlockedIncrement(&freeze_id);
    LOG("ragdoll freeze #%ld: no game tick for %d s (last tick %u): the game thread %lu, %d frames%s%s (a loading screen can "
        "do this too)", id, secs, tick_n, tid, n, why ? ": " : "", why ? why : "");
    uintptr_t bases[16];
    int nb = 0;
    for (int i = 0; i < n; i++) {
        char hex[STACK_BYTES * 3 + 8], lab[200];
        uintptr_t p = pc[i];
        if (p >= exe_lo && p < exe_hi) {
            uintptr_t f = pdata_fn_start(i ? p - 1 : p);   // a return address may sit past its function's end
            hex[0] = 0;
            if (f && mem_ok(f, STACK_BYTES)) {
                char *q = hex + sprintf(hex, "  bytes");
                for (int k = 0; k < STACK_BYTES; k++) q += sprintf(q, " %02x", ((const uint8_t *)f)[k]);
            }
            if (f) LOG("ragdoll freeze #%ld %2d 0x%llx = fn 0x%llx+0x%llx%s%s", id, i, (unsigned long long)(p - g_base_delta),
                       (unsigned long long)(f - g_base_delta), (unsigned long long)(p - f), fn_label(f, lab, sizeof lab), hex);
            else LOG("ragdoll freeze #%ld %2d 0x%llx = fn ? (no unwind entry: leaf)", id, i, (unsigned long long)(p - g_base_delta));
        } else if (p >= self_lo && p < self_hi) {
            LOG("ragdoll freeze #%ld %2d b4bcoop+0x%llx", id, i, (unsigned long long)(p - self_lo));
        } else {
            LOG("ragdoll freeze #%ld %2d image 0x%llx+0x%llx", id, i, (unsigned long long)img[i],
                (unsigned long long)(img[i] ? p - img[i] : p));
            int seen = 0;
            for (int k = 0; k < nb; k++) if (bases[k] == img[i]) seen = 1;
            if (img[i] && !seen && nb < 16) bases[nb++] = img[i];
        }
    }
    for (int k = 0; k < nb; k++) {   // names last: GetModuleFileName may wait for the loader lock
        char mod[MAX_PATH];
        mod[0] = 0;
        GetModuleFileNameA((HMODULE)bases[k], mod, sizeof mod);
        const char *b = strrchr(mod, '\\');
        LOG("ragdoll freeze #%ld image 0x%llx = %s", id, (unsigned long long)bases[k], mod[0] ? (b ? b + 1 : mod) : "?");
    }
}
static DWORD WINAPI freeze_watch(void *arg) {
    (void)arg;
    unsigned last = 0;
    int still = 0;
    for (;;) {
        Sleep(1000);
        unsigned t = *(volatile unsigned *)&tick_n;
        DWORD tid = game_tid;
        if (!t || !tid) continue;   // no tick yet
        if (t != last) {
            if (still >= FREEZE_S) LOG("ragdoll freeze: the game thread ticks again after %d s", still);
            last = t;
            still = 0;
            continue;
        }
        if (++still == FREEZE_S || still == FREEZE_S2) freeze_stack(tid, still);
    }
    return 0;
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
    } else if (!strcmp(t, "ArrayProperty")) {
        // build 41: the first entries too when there is depth left (build 40's dumps said "3 entries" and no more)
        const TArray *arr = (const TArray *)a;
        FField *in = *(FField **)((char *)p + 0x78);   // FArrayProperty::Inner
        if (depth <= 0 || !in || !arr->data || arr->num <= 0 || arr->num > 100000) { snprintf(out, n, "%d entries", arr->num); return; }
        size_t k = (size_t)snprintf(out, n, "%d entries [", arr->num);
        int32_t es = FP_ELSIZE(in);
        for (int i = 0; i < arr->num && i < 8 && es > 0 && k + 16 < n; i++) {
            char ev[1200];
            fmt_prop(in, (const uint8_t *)arr->data + (size_t)i * (size_t)es - FP_OFFSET(in), ev, sizeof ev, depth - 1);
            k += (size_t)snprintf(out + k, n - k, "%s%s", i ? ", " : "", ev);
        }
        if (k + 6 < n) snprintf(out + k, n - k, "%s]", arr->num > 8 ? ", ..." : "");
    }
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
                    char fn[64], fv[1200];
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
    uint8_t own;                                                 // build 41: one we worked out (no game announcement)
} Blast;
static Blast blasts[8];
static int blast_at;
static double last_blast_t(void) { double t = 0; for (int i = 0; i < 8; i++) if (blasts[i].t > t) t = blasts[i].t; return t ? t : -1e9; }

// ---- shots on dead bodies (build 35): the hit, from the corpse manager (see "shots on dead bodies" below) ----
static UFunction *corpse_hit_fn;   // UCorpseManager::OnCorpseHit(HitActor): bound to every registered corpse's hits
static FField *corpse_hit_actor_f;
static void shot_note(UObject *o, UFunction *fn, void *params);
static void shot_resolve(void);

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

static FName nm_mutsnd;   // build 41: HandleMutilationSounds (the game's own mutilations, discovery)
static void game_mut_note(UObject *o, UFunction *fn, const uint8_t *params);
static void pe_detour(UObject *o, UFunction *fn, void *params) {
    if (fov_pe_match(o, fn)) { orig_pe(o, fn, params); fov_pe_apply(params); return; }   // build 44: fov.c's camera modifier
    if (fn == explode_fn && explode_fn && params && mode_on && blast_loc_f) {   // game thread
        Blast *b = &blasts[blast_at++ % 8];
        memset(b, 0, sizeof *b);
        b->t = now_s();
        memcpy(b->at, (uint8_t *)params + FP_OFFSET(blast_loc_f), sizeof b->at);
        if (tracing) fx_flush(b->t);
    }
    if (fn == corpse_hit_fn && corpse_hit_fn && params && mode_on) shot_note(o, fn, params);   // build 35
    if (params && mode_on) wound_hook(o, fn, params);                                          // build 40
    if (params && mode_on && nm_mutsnd.idx && fn && same_name(fn, nm_mutsnd) && !own_call()) game_mut_note(o, fn, params);   // build 41
    if (tracing && o && fn && !own_call()) trace_call(o, fn, params);
    orig_pe(o, fn, params);
}

// The ProcessEvent hook is on while a trace runs, while the ragdoll is on (one pointer compare per call), or (build 44)
// while fov.c's first-person FOV has its camera modifier in.
static int pe_enabled;
static int pe_update(void) {
    if (mode_on || tracing) { juice_resolve(); shot_resolve(); }   // build 44: not for fov.c alone
    int want = tracing || (mode_on && (explode_fn || corpse_hit_fn || wound_hook_wanted())) ||   // blast centres, shots, living hits
               fov_pe_wanted();                                                                  // build 44: the FOV
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

void ragdoll_pe_refresh(void) { pe_update(); }   // build 44: fov.c (game thread)

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
static const PushKind PUSH_DEF[] = {   // x the scale (default 4); a Common weighs 133 kg by GetBoneMass
    // build 36: the guns are Joon's (17 Oct, after build 35: HG 6000, SMG 7000, AR 9000, LMG 11000, SG 25000 before)
    {"HG", 3000, 0},  {"SMG", 3500, 0}, {"AR", 5000, 0}, {"LMG", 10000, 0}, {"SG", 22500, 0},
    {"Sni", 25000, 0},   // snipers (Sni01_DamageType_C) (Joon, 16 Oct; kept 17 Oct); listed with the guns since build 36
    {"FragGrenade", 60000, 1},   // frags and pipe bombs (both FragGrenade_DamageType_C); Joon's pick (16 Oct)
    {"PipeBomb", 40000, 1}, {"Grenade", 40000, 1}, {"Explosi", 40000, 1},   // (build 22 had these commented out by mistake)
    {"Propane", 40000, 1},
    // build 22 log: red barrels and Exploders were unknown (default 8000, aimed from the shooter, not the blast).
    // Blasts, as strong as a frag until Joon picks (first guess). Gas cans kill by fire over seconds, 5-8 m away:
    // a burning body collapses where it stands, so no push (first guess too; /ragdoll push GasCan <n> to change)
    {"ExplodingBarrel", 60000, 1}, {"Exploder", 60000, 1}, {"GasCan", 0, 0},
    {"Molotov", 0, 0},   // build 40: fire, like GasCan (build 38 log: Molotov_DamageType_C kills got the default 8000)
    // build 41: the Volatile Ridden corruption card's blast (Challenge_Volatile_Explosion_DamageType_C, build 40 log:
    // Explosi 40000 from the exploding Ridden, never dismembered) and the Broadside card's (a precision kill makes the
    // Ridden explode; its damage type not seen yet): as strong as a frag (Joon: "like a frag")
    {"Volatile", 60000, 1}, {"Broadside", 60000, 1},
    {"Arrow", 8000, 0},  // build 41: the bow (Arrow01_Hero_DamageType_C), the default's strength; named so it has a kind
    {"Stumble", 0, 0}, {"default", 8000, 0},   // default: melee and any weapon not named here
};
#define N_PUSH_DEF ((int)(sizeof PUSH_DEF / sizeof PUSH_DEF[0]))
static PushKind push_tab[48];   // PUSH_DEF, then b4bcoop.ini's ragdoll_kinds and /ragdoll push <kind> <n> (which appends)
static int n_push_tab;          // 0 = not filled yet (push_kind fills it)
static void kinds_reset(void) { memcpy(push_tab, PUSH_DEF, sizeof PUSH_DEF); n_push_tab = N_PUSH_DEF; }
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
    if (!n_push_tab) kinds_reset();
    for (int i = 0; i < n_push_tab; i++) if (!_stricmp(push_tab[i].key, key)) return &push_tab[i];
    if (!add || n_push_tab == (int)(sizeof push_tab / sizeof push_tab[0])) return NULL;
    PushKind *k = &push_tab[n_push_tab++];
    snprintf(k->key, sizeof k->key, "%s", key);
    k->n = push_kind("default", 0)->n;
    k->radial = 0;
    return k;
}

// the damage type's class name -> the table entry
// a damage type's (or weapon's) class name -> the table entry (build 41: split from kind_of, for the harness)
static PushKind *kind_by_name(const char *name) {
    char code[24], low[160], kl[24];
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof code && ((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= 'a' && name[i] <= 'z')); i++) code[i] = name[i];
    code[i] = 0;
    PushKind *k = (name[i] >= '0' && name[i] <= '9') ? push_kind(code, 0) : NULL;   // HG02 -> HG
    if (k) return k;
    for (i = 0; name[i] && i + 1 < sizeof low; i++) low[i] = (char)tolower((unsigned char)name[i]);
    low[i] = 0;
    // a key in the name: FragGrenade_DamageType_C. The longest key found wins (build 41: Challenge_Volatile_Explosion_
    // DamageType_C holds both "Explosi" and "Volatile"; before, the first key in the table won)
    PushKind *best = NULL;
    size_t best_m = 0;
    if (!n_push_tab) kinds_reset();
    for (int j = 0; j < n_push_tab; j++) {
        size_t m = 0;
        for (; push_tab[j].key[m] && m + 1 < sizeof kl; m++) kl[m] = (char)tolower((unsigned char)push_tab[j].key[m]);
        kl[m] = 0;
        if (m >= 4 && m > best_m && strcmp(kl, "default") && strstr(low, kl)) { best = &push_tab[j]; best_m = m; }
    }
    if (best) return best;
    static char unknown[16][96];
    int seen = 0, slot = -1;
    for (int j = 0; j < 16; j++) { if (!strcmp(unknown[j], name)) seen = 1; if (slot < 0 && !unknown[j][0]) slot = j; }
    if (!seen && slot >= 0) {
        snprintf(unknown[slot], sizeof unknown[slot], "%s", name);
        LOG("ragdoll push: unknown kind %s (a damage type, or a weapon for shots on bodies): default strength (add it with "
            "/ragdoll push %s <n>)", name, code[0] ? code : name);
    }
    return push_kind("default", 0);
}
static PushKind *kind_of(UObject *dtype, char *name, size_t n) {
    if (!dtype) { snprintf(name, n, "None"); return push_kind("default", 0); }
    static UClass *class_cls;
    if (!class_cls) class_cls = cls("Class");
    if (class_cls && ue_is_a(dtype, class_cls)) ue_obj_name(dtype, name, n);   // a class (ClassProperty)
    else cname(dtype, name, n);                                               // its default object
    return kind_by_name(name);
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

// the bone that takes a push: the hit bone, or its nearest parent with a body (8 steps up at most), else the first
// body (the root); with no body list the bone stays as it is (None = the engine's root body)
static FName body_bone(UObject *mesh, const BodyList *bl, FName bone) {
    Call k;
    for (int up = 0; bone.idx && bl && !has_body(bl, bone) && up < 8; up++) {
        if (!call_prep(&k, mesh, "GetParentBone")) { bone = (FName){0, 0}; break; }
        SET(&k, "BoneName", FName, bone);
        call_go(&k);
        FName *r = carg(&k, "ReturnValue");
        bone = r ? *r : (FName){0, 0};
    }
    if (bl && (!bone.idx || !has_body(bl, bone))) bone = bl->bone[0];
    return bone;
}

static float entry_key(const uint8_t *e) {   // larger = the killing hit
    if (dinfo.prev_hp) return -*(const float *)(e + FP_OFFSET(dinfo.prev_hp));
    if (dinfo.index) return (float)*(const int32_t *)(e + FP_OFFSET(dinfo.index));
    if (dinfo.time) return *(const float *)(e + FP_OFFSET(dinfo.time));
    return 0;
}


// build 41: where a blast came from, for its push and for /ragdoll dismember (t->blast_ok). In this order:
//   1. the hit's source is another Ridden: it blew up (the Volatile Ridden card; an Exploder): its place. Build 40 log:
//      the volatile blast's victims named the exploding Ridden as the source, and the game announced no explosion
//      (no Multicast_ExplodeFromSeed), so they were pushed but never dismembered;
//   2. the nearest explosion the game announced (Multicast_ExplodeFromSeed: frags, pipe bombs, barrels, propane) of the
//      last second, within 20 m (their source is often the thrower, so the announcement is the better centre);
//   3. a Ridden that died in the last 0.5 s within 6 m of this one: a card's explosion credited to the hero (Broadside:
//      a precision kill makes the Ridden explode; not seen in a log yet);
//   else the place worked out before (the source's place, or under the body): pushed, not dismembered (as build 40).
// 1 and 3 are recorded as explosions of our own (the same one within 1 m and 0.5 s), so the tally line covers them; the
// search in 2 skips ours (a volatile chain: each body's own source is the better answer).
static void blast_take(Tracked *t, const float at[3], double now) {
    int bi = -1;
    for (int i = 0; i < 8 && bi < 0; i++) {
        if (!blasts[i].own || now - blasts[i].t > 0.5) continue;
        float d2 = 0;
        for (int c = 0; c < 3; c++) d2 += (at[c] - blasts[i].at[c]) * (at[c] - blasts[i].at[c]);
        if (d2 < 100.f * 100.f) bi = i;
    }
    if (bi < 0) {
        bi = blast_at++ % 8;
        memset(&blasts[bi], 0, sizeof blasts[bi]);
        blasts[bi].t = now;
        blasts[bi].own = 1;
        memcpy(blasts[bi].at, at, sizeof blasts[bi].at);
    }
    memcpy(t->blast_at, blasts[bi].at, sizeof t->blast_at);
    t->blast_ok = 1;
    t->blast_i = (uint8_t)bi;
}
static void blast_centre(Tracked *t, const float pt[3], UObject *srca, float org[3], const char **from) {
    double now = now_s();
    float at[3];
    if (srca && srca != t->a && is_ridden_fast(srca) && !actor_loc(srca, at)) {
        memcpy(org, at, sizeof at);
        *from = "the Ridden that blew up";
        blast_take(t, org, now);
        return;
    }
    float bd = 2000.f * 2000.f;
    int bi = -1;
    for (int i = 0; i < 8; i++) {
        if (!blasts[i].t || blasts[i].own || now - blasts[i].t > 1.0) continue;
        float d2 = 0;
        for (int c = 0; c < 3; c++) d2 += (pt[c] - blasts[i].at[c]) * (pt[c] - blasts[i].at[c]);
        if (d2 < bd) { bd = d2; bi = i; }
    }
    if (bi >= 0) {
        memcpy(org, blasts[bi].at, 3 * sizeof(float));
        *from = "the explosion";
        memcpy(t->blast_at, org, sizeof t->blast_at); t->blast_ok = 1; t->blast_i = (uint8_t)bi;
        return;
    }
    Tracked *closest = NULL;
    float nd = 600.f * 600.f, nat[3] = {0, 0, 0};
    for (int i = 0; i < ntr; i++) {
        Tracked *x = &tr[i];
        if (x == t || !x->died_at || now - x->died_at > 0.5 || !ue_object_usable(x->a, x->ai) || actor_loc(x->a, at)) continue;
        float d2 = 0;
        for (int c = 0; c < 3; c++) d2 += (pt[c] - at[c]) * (pt[c] - at[c]);
        if (d2 < nd) { nd = d2; closest = x; memcpy(nat, at, sizeof nat); }
    }
    if (closest) {
        memcpy(org, nat, sizeof nat);
        *from = "a Ridden that had just died nearby (a card's explosion?)";
        blast_take(t, org, now);
    }
}

static int wounds_on = 1;   // build 40: /ragdoll wounds (see "wounds by weapon"); needs the killing hit even with no push

// the killing hit in a body's RecentDamageInfo: the newest entry that isn't a stumble event (build 40: shared with the
// wounds); NULL = none. `dump`: every entry into the log (the first deaths per /ragdoll on)
static const uint8_t *killing_entry(Tracked *t, const TArray *arr, int dump) {
    char a[160], sn[96];
    const uint8_t *best = NULL;
    if (dump) LOG("ragdoll push: %s: RecentDamageInfo, %d entries:", oname(t->a, a, sizeof a), arr->num);
    for (int i = 0; arr->data && i < arr->num && i < 16; i++) {
        const uint8_t *e = (const uint8_t *)arr->data + (size_t)i * dinfo.elsize;
        if (dump) log_hit("entry", i, e);
        // the stumble event that comes with every hit: its flag, else no hit component, else a Stumble damage type
        if (dinfo.stumble && bool_at(dinfo.stumble, e) == 1) continue;
        if (dinfo.comp && !obj_field(dinfo.comp, e)) continue;
        if (dinfo.type) { PushKind *sk = kind_of(obj_field(dinfo.type, e), sn, sizeof sn); if (sk && !strcmp(sk->key, "Stumble")) continue; }
        if (!best || entry_key(e) >= entry_key(best)) best = e;   // ties: the later entry
    }
    return best;
}

static void push_body(Tracked *t, UObject *mesh) {
    char a[160], tn[96], bn[64], hb[64], sn[96], sc[96];
    if (push_scale <= 0.f) return;
    UObject *hc = call_obj(t->a, "GetHealthComponent");
    if (!hc || dinfo_init(hc) != 1) { n_push_fail++; return; }
    const TArray *arr = (const TArray *)((char *)hc + dinfo.arr_off);
    int dump = hit_dumps < HIT_DUMPS;
    if (dump) hit_dumps++;
    const uint8_t *best = killing_entry(t, arr, dump);
    if (!best) {
        n_push_nohit++;
        if (push_logs < PUSH_LOGS) { push_logs++; LOG("ragdoll push: %s: no usable hit in RecentDamageInfo (%d entries): not pushed", oname(t->a, a, sizeof a), arr->num); }
        return;
    }
    PushKind *kind = dinfo.type ? kind_of(obj_field(dinfo.type, best), tn, sizeof tn) : (snprintf(tn, sizeof tn, "unknown"), push_kind("default", 0));
    snprintf(t->kind, sizeof t->kind, "%s", kind->key);   // the killing hit's kind, kept with the body
    float strength = kind->n * push_scale;
    if (strength <= 0.f) return;
    const BodyList *bl = bodies_of(mesh);
    FName bone = dinfo.bone ? *(const FName *)(best + FP_OFFSET(dinfo.bone)) : (FName){0, 0};
    ue_name(bone, hb, sizeof hb);
    Call k;
    bone = body_bone(mesh, bl, bone);
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
    if (kind->radial) {   // the blast centre (build 41: also for explosions the game doesn't announce)
        UObject *s0 = dinfo.source ? obj_field(dinfo.source, best) : NULL;
        blast_centre(t, pt, s0 ? (owner_actor(s0) ? owner_actor(s0) : s0) : NULL, org, &from);
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

#define DISM_DEFAULT 1               // build 36: on by default (Joon, 17 Oct: everything on at a game start)
static int dism_on = DISM_DEFAULT;   // /ragdoll dismember on|off
#define DISM_MIN_DEF 2
#define DISM_MAX_DEF 3
static int dism_min = DISM_MIN_DEF, dism_max = DISM_MAX_DEF;   // build 32: limbs per blast victim (/ragdoll dismember limbs <min> [max])
static int dism_pieces = 1;          // build 32: our piece for every limb the game throws none for
#define DISM_BIG_DEF 75
static int dism_big = DISM_BIG_DEF;  // build 33: % of limbs whose big cut (an "Upper" region) is tried first
#define DISM_HEAD_DEF 50
static int dism_head = DISM_HEAD_DEF; // % of blast victims that also lose the head (build 33 test; build 34: Joon, half)
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

// the names of an enum property's values, so the log says what Type 2 means (ue.c: ue_prop_enum and friends, the layout)
static void enum_names(FField *p, char *out, size_t n) {
    char b[96];
    UObject *e = ue_prop_enum(p);
    const TArray *names = ue_enum_names(e);
    if (!names) { snprintf(out, n, "?"); return; }
    size_t k = (size_t)snprintf(out, n, "%s:", ue_obj_name(e, b, sizeof b));
    for (int i = 0; i < names->num && i < 40 && k + 8 < n; i++) {
        int64_t v;
        const char *s = ue_enum_entry(names, i, b, sizeof b, &v);
        k += (size_t)snprintf(out + k, n - k, " %s=%lld", s, (long long)v);
    }
}
static int enum_value(FField *p, const char *name) {   // the value named `name` (after "Enum::"), -1 if none
    char b[96];
    const TArray *names = ue_enum_names(ue_prop_enum(p));
    for (int i = 0; names && i < names->num; i++) {
        int64_t v;
        if (!strcmp(ue_enum_entry(names, i, b, sizeof b, &v), name)) return (int)v;
    }
    return -1;
}
static const char *region_name(int r, char *out, size_t n) {   // "LeftLowerArm", or the number
    char b[96];
    const TArray *names = mi.region ? ue_enum_names(ue_prop_enum(mi.region)) : NULL;
    for (int i = 0; names && i < names->num; i++) {
        int64_t v;
        const char *s = ue_enum_entry(names, i, b, sizeof b, &v);
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
    const TArray *names = ue_enum_names(ue_prop_enum(mi.region));
    size_t q = 0;
    lm[0] = 0;
    for (int i = 0; names && i < names->num; i++) {
        int64_t v;
        const char *s = ue_enum_entry(names, i, b, sizeof b, &v);
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

// build 41: the game's own wounds (see "wounds by weapon"): MaxWounds / MaxRuptureWounds and each class's own values
static int cap_state;                 // 0 not looked up, 1 ok, -1 not in this game (logged)
static FField *cap_w_f, *cap_r_f;
static struct { UClass *c; int32_t w, r; } cap_cls[8];
static int n_cap_cls;
static int cap_orig(UObject *mut, int32_t *w, int32_t *r);

// the game's limb code on this machine; while `watch`, its calls and constructions go into the log. Build 41: with
// the component's MaxWounds / MaxRuptureWounds at 0 (the game's own wounds stopped), the game's values for our call
static int mstate_onrep(UObject *mut, int watch) {
    int32_t w, r, *pw = NULL, *pr = NULL, sw = 0, sr = 0;
    if (cap_state == 1 && cap_orig(mut, &w, &r)) {
        pw = (int32_t *)((uint8_t *)mut + FP_OFFSET(cap_w_f));
        pr = (int32_t *)((uint8_t *)mut + FP_OFFSET(cap_r_f));
        sw = *pw; sr = *pr;
        *pw = w; *pr = r;
    }
    if (watch) { own_traced = 1; dism_watch = 1; }
    int called = call_void(mut, "OnRep_MutilationState");
    own_traced = 0;
    dism_watch = 0;
    if (pw) { *pw = sw; *pr = sr; }
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

// one MutilationInfo entry for `region` (Impulse from `from` toward `at` at `mag`, ImpactPoint `point`), then the game's
// limb code; 1 = the region is off now (IsRegionDismembered, or the game appended its own copy), 0 = refused, -1 = no memory.
// Build 40: a Rupture (an open wound) doesn't make the region "dismembered": judged by the game's appended copy.
static int mstate_apply(Tracked *t, UObject *mut, TArray *arr, uint8_t region, int type, const float at[3], const float from[3],
                        const float point[3], float mag, int watch, int *called) {
    (void)t;
    float d[3];
    for (int c = 0; c < 3; c++) d[c] = at[c] - from[c];
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1.f) { d[0] = 0; d[1] = 0; d[2] = 1; len = 1.f; }
    int32_t n0 = arr->num;
    uint8_t *e = mstate_add(arr);
    if (!e) return -1;
    e[FP_OFFSET(mi.region)] = region;
    e[FP_OFFSET(mi.type)] = (uint8_t)type;
    float *im = (float *)(e + FP_OFFSET(mi.impulse)), *ip = (float *)(e + FP_OFFSET(mi.point));
    for (int c = 0; c < 3; c++) { im[c] = d[c] / len * mag; ip[c] = point[c]; }
    *called = mstate_onrep(mut, watch);
    if (type == mi.rupture) return arr->num > n0 + 1;
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
        int r = mstate_apply(t, mut, arr, (uint8_t)mi.head, types[i], at, t->blast_at, t->blast_at, mag, 0, &called);
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

// ---- wounds by weapon (build 40) ----
// Joon (24 Oct, build 39): shotguns took nothing off, not even heads; snipers took limbs and heads but never opened a
// body hit; a headshot (sniper, rifle) took the head AND opened the chest; explosions left the body whole apart from
// the limbs. Wanted: what comes off or opens is where the shot lands. The game's own entries (build 38 log, rifle kills):
// headshot = Head BreakOff (|Impulse| 2000) + Chest Rupture (20000) + often an upper arm Rupture; a thigh hit =
// LeftLowerLeg BreakOff; a spine_03 hit = Stomach Rupture. So a Rupture is the open wound and a BreakOff a part off; the
// game allows 2 ruptures per body (MaxRuptureWounds) and one arm off per Common (build 28).
// For the wound kinds (push table keys, default SG,Sni,LMG,AR; "small" ones, default LMG,AR, get a weaker Impulse: a
// guess at a smaller wound) the hit bone decides:
//   head, neck              -> Head BreakOff                    (a dead body only: the killing shot, or one already down)
//   hand                    -> <side>LowerArm BreakOff (wrist)
//   forearm, upper arm      -> <side>UpperArm BreakOff (elbow: a Common has no shoulder cut)
//   foot                    -> <side>LowerLeg BreakOff (ankle)
//   calf, thigh             -> <side>UpperLeg BreakOff (knee: no hip cut)
//   clavicle, spine_02/03   -> Chest Rupture
//   spine_01, pelvis        -> Stomach Rupture
// Head-only kinds (default HG,SMG, Joon 24 Oct): a killing headshot takes the head, nothing else.
// Where the hits come from: the killing shot (RecentDamageInfo at the ragdoll, host and client, before the push: limbs
// taken after a push took the push away, build 28), shots on bodies already down (build 35's aim trace: the bone, the
// shooter's weapon), and hits on living Ridden (host only: the game's OnDamage event on the Ridden; /ragdoll wounds
// living off | arms (arms and open wounds, default) | all (legs too: a living Ridden without shins walks on stumps)).
// Heads never come off a living Ridden. A part already off is left alone (also a head the game's head-explosion card
// took). Blast victims (/ragdoll dismember) also get 1 or 2 open wounds, chest and/or stomach (/ragdoll wounds blast).
// On a host every change replicates (MutilationState), so clients see the same wounds.
//
// The game's own wounds (build 41). Build 40 log: on a killing headshot the game itself adds Head BreakOff (|Impulse|
// 2000) + Chest Rupture (20000) + one upper arm Rupture (20000), at the same point, its SourceActor the hero; an arm kill
// adds a Chest Rupture to the arm BreakOff; a torso kill a Stomach or Chest Rupture. That is the "head and the body,
// sometimes an arm" Joon saw for every gun and the bow, and it happens on the host at damage time, before the Ridden
// is seen dead (its native code: no UFunction to stop it). Never more than 2 ruptures per body in any log, which fits
// the component's MaxRuptureWounds = 2 being a limit its choice respects. So, host only, /ragdoll wounds game off
// (default): at every OnDamage event of a weapon kind we handle (the wound kinds, the head-only kinds and the no-wound
// kinds; the event comes before the mutilation: build 26 trace, a Tallboy's arm 5 ms after its OnDamage), the
// Ridden's MutilationComponent gets MaxRuptureWounds 0 and MaxWounds 0; any other kind (melee, blasts, the game's own)
// puts the class's values back, so their gore stays the game's. Our own entries raise both again around our
// OnRep_MutilationState call, and at the ragdoll (dead: the game makes no more of its own) both go back to the game's
// values for good (caps_restore_one), so the 0 only ever sits on living Ridden. MaxWounds' meaning is unknown: maybe a cap on the BreakOff the game picks (the head),
// maybe nothing; with it at 0 a gun's headshot head comes from our own wound (the wound and head-only kinds take it),
// and the bow's (a no-wound kind) not at all. Whether it worked shows at death: "the game's own wounds: ..." per body
// killed by a handled kind (entries with a SourceActor; ours have none). Also logged (discovery for a native hook if
// this isn't enough): the first game-made mutilations (EnemyAudioComponent's HandleMutilationSounds, which the game
// calls per region from inside its mutilation code) with their time after the Ridden's last OnDamage and 3 call stacks;
// each weapon damage type's settings (its gameplay tags) and the wound config in full, once.
// Build 41's cap at the OnDamage event came too late (the game picks a hit's wounds just before that hit's OnDamage
// event) and MaxWounds 0 didn't stop the head pop; build 42 caps open wounds from the scan and decides hit by hit in the
// game's own wound pick (see "the game's own wounds, hit by hit" below).
// No-wound kinds (/ragdoll wounds none, default Arrow,Bow: Joon wants no dismemberment from the bow): nothing of ours,
// and (build 42) none of the game's either.
#define WOUND_NONE_DEF   "Arrow,Bow"
#define WOUND_KINDS_DEF  "SG,Sni,LMG,AR"
#define WOUND_SMALL_DEF  "LMG,AR"
#define WOUND_HEADS_DEF  "HG,SMG"
#define WOUND_LIVING_DEF 1
#define WOUND_IMP_FULL   20000.f   // the game's own Rupture entries (build 38 log)
#define WOUND_IMP_SMALL  5000.f    // a guess: if the Impulse sizes the wound, a smaller one
#define WOUND_IMP_LIMB   2000.f    // the game's own BreakOff entries
#define WOUND_LOGS       40
#define WOUNDQ           64
static const char *const LIVING_NAME[3] = {"off", "arms", "all"};
static char wound_kinds[64] = WOUND_KINDS_DEF, wound_small[64] = WOUND_SMALL_DEF, wound_heads[64] = WOUND_HEADS_DEF;
static char wound_none[64] = WOUND_NONE_DEF;   // build 41
static int wound_living = WOUND_LIVING_DEF, wound_blast = 1;
static int wound_game;                          // build 41: 1 = the game's own wounds stay (vanilla), 0 = stopped
static int n_wound_hits, n_wound_off, n_wound_rupt, n_wound_refused, n_wound_already, n_wound_skip, n_wound_nomap,
           n_wound_pieces, n_blast_rupt, wound_logs, wound_nomap_logs;
static int n_cap_scan, n_cap_hits, n_cap_fail, cap_logs, n_ctl_kills, n_ctl_game, ctl_logs, n_game_mut, game_mut_logs;

static int kind_in(const char *list, const char *kind) {   // the kind is in the list (any case; "all" = every kind)
    if (!kind || !kind[0]) return 0;
    size_t kl = strlen(kind);
    for (const char *p = list; *p; ) {
        while (*p == ',' || *p == ' ') p++;
        size_t n = 0;
        while (p[n] && p[n] != ',' && p[n] != ' ') n++;
        if (n && ((n == 3 && !_strnicmp(p, "all", 3)) || (n == kl && !_strnicmp(p, kind, n)))) return 1;
        p += n;
    }
    return 0;
}

enum { WC_NONE, WC_HEAD, WC_ARM, WC_LEG, WC_BODY };
// the region a hit on this bone opens or takes off ("RightUpperArm"), *rupture = an open wound; WC_NONE = nothing
static int wound_class(const char *bone, char *region, size_t n, int *rupture) {
    char low[64];
    size_t i = 0;
    for (; bone[i] && i + 1 < sizeof low; i++) low[i] = (char)tolower((unsigned char)bone[i]);
    low[i] = 0;
    const char *us = strrchr(low, '_');
    const char *side = us && us[1] && !us[2] ? (us[1] == 'l' ? "Left" : us[1] == 'r' ? "Right" : NULL) : NULL;
    *rupture = 0;
    if (strstr(low, "head") || strstr(low, "neck")) { snprintf(region, n, "Head"); return WC_HEAD; }
    if (strstr(low, "hand") && side) { snprintf(region, n, "%sLowerArm", side); return WC_ARM; }
    if ((strstr(low, "lowerarm") || strstr(low, "upperarm") || strstr(low, "shoulder")) && side) { snprintf(region, n, "%sUpperArm", side); return WC_ARM; }
    if ((strstr(low, "foot") || strstr(low, "ball") || strstr(low, "toe")) && side) { snprintf(region, n, "%sLowerLeg", side); return WC_LEG; }
    if ((strstr(low, "calf") || strstr(low, "thigh")) && side) { snprintf(region, n, "%sUpperLeg", side); return WC_LEG; }
    *rupture = 1;
    if (strstr(low, "clavicle") || strstr(low, "spine_02") || strstr(low, "spine_03") || strstr(low, "chest")) { snprintf(region, n, "Chest"); return WC_BODY; }
    if (strstr(low, "spine") || strstr(low, "pelvis") || strstr(low, "hip")) { snprintf(region, n, "Stomach"); return WC_BODY; }
    *rupture = 0;
    return WC_NONE;
}

static int mstate_has(const TArray *arr, uint8_t region, int type) {   // an entry for this region and type already
    for (int i = 0; arr->data && i < arr->num; i++) {
        const uint8_t *e = (const uint8_t *)arr->data + (size_t)i * mi.el;
        if (e[FP_OFFSET(mi.region)] == region && e[FP_OFFSET(mi.type)] == (uint8_t)type) return 1;
    }
    return 0;
}

// ---- the game's own wounds (build 41, see "wounds by weapon" above) ----
// the OnDamage event's DamageInfo fields (living hits, below; build 40), shared with cap_hit
static struct { int state; FField *info, *bone, *type, *stumble, *point, *dir; } dmgi;
static int kind_handled(const char *key) {   // a kind whose wounds are ours: the wound, head-only and no-wound kinds
    return key && key[0] && (kind_in(wound_kinds, key) || kind_in(wound_heads, key) || kind_in(wound_none, key));
}
static int is_common_class(UObject *a) {    // Common_AICharacterBP_C, CommonArmored_AICharacterBP_C (Joon's reports):
    char b[96];                             // the specials (a Tallboy's weak-spot arm) keep the game's own
    return a && U_CLASS(a) && strstr(ue_obj_name(U_CLASS(a), b, sizeof b), "Common") != NULL;
}
static int cap_init(UObject *mut) {   // MutilationComponent.MaxWounds / MaxRuptureWounds (ints): 1 ok, -1 not here
    char t[64], u[64];
    if (cap_state || !mut) return cap_state;
    cap_state = -1;
    FField *w = ue_find_prop(U_CLASS(mut), "MaxWounds"), *r = ue_find_prop(U_CLASS(mut), "MaxRuptureWounds");
    if (!w || !r || strcmp(ue_name(*(FName *)FF_CLASS(w), t, sizeof t), "IntProperty") ||
        strcmp(ue_name(*(FName *)FF_CLASS(r), u, sizeof u), "IntProperty")) {
        LOG("ragdoll wounds: MutilationComponent has no MaxWounds / MaxRuptureWounds ints: the game's own wounds stay");
        return -1;
    }
    cap_w_f = w; cap_r_f = r;
    cap_state = 1;
    return 1;
}
// the class's own values, read off the first component of that class we touch (before any change of ours)
static int cap_orig(UObject *mut, int32_t *w, int32_t *r) {
    char b[96];
    if (cap_init(mut) != 1) return 0;
    UClass *c = U_CLASS(mut);
    for (int i = 0; i < n_cap_cls; i++) if (cap_cls[i].c == c) { *w = cap_cls[i].w; *r = cap_cls[i].r; return 1; }
    if (n_cap_cls == (int)(sizeof cap_cls / sizeof cap_cls[0])) return 0;
    *w = *(const int32_t *)((const uint8_t *)mut + FP_OFFSET(cap_w_f));
    *r = *(const int32_t *)((const uint8_t *)mut + FP_OFFSET(cap_r_f));
    cap_cls[n_cap_cls].c = c; cap_cls[n_cap_cls].w = *w; cap_cls[n_cap_cls].r = *r;
    n_cap_cls++;
    LOG("ragdoll wounds: %s: the game's MaxWounds %d, MaxRuptureWounds %d", ue_obj_name(c, b, sizeof b), *w, *r);
    return 1;
}
static void cap_read(UObject *mut, int32_t *w, int32_t *r) {
    *w = *r = -1;
    if (!mut || cap_state != 1) return;
    *w = *(const int32_t *)((const uint8_t *)mut + FP_OFFSET(cap_w_f));
    *r = *(const int32_t *)((const uint8_t *)mut + FP_OFFSET(cap_r_f));
}

// Build 42: the cap goes on before the first hit. Build 41 set it at a Common's OnDamage event, but the build 41 log
// shows the game's mutilation for a hit running just before that hit's OnDamage event, in the same frame: 28 of the
// game's 30 mutilations were on a Common's first hit, cap not set yet; the 2 on Commons hit before (cap already 0) were
// the Head BreakOff alone, no Chest / UpperArm Rupture. So: a living Common's MaxRuptureWounds is 0 from the scan that
// first lists it (cap_living: alive, so before any hit but the very first frame's), and the OnDamage event (cap_hit) only
// catches one hit before the scan reached it. MaxWounds stays the game's: at 0 it didn't stop the game's head pop
// (those 2 Commons) and nothing says what it caps, so build 42 leaves it alone. Every kind of hit now gets the game's
// BreakOffs (the head, an arm or leg where the shot lands) but none of its open wounds; ours come from wound_apply.
static UObject *cap_comp(Tracked *x, UObject *o) {   // the Common's MutilationComponent (kept on its entry)
    UObject *mut = x && x->mut && ue_object_usable(x->mut, x->muti) ? x->mut : NULL;
    if (!mut) {
        mut = comp_of(o, "MutilationComponent");
        if (x) { x->mut = mut; x->muti = mut ? U_INDEX(mut) : -1; }
    }
    return mut;
}
// MaxRuptureWounds to 0 on a living Common: 1 changed (*r0 = before), 0 already 0, -1 can't
static int cap_zero(UObject *o, Tracked *x, int32_t *r0) {
    int32_t w, r, w0;
    UObject *mut = cap_comp(x, o);
    if (!mut || !cap_orig(mut, &w, &r)) { n_cap_fail++; return -1; }
    cap_read(mut, &w0, r0);
    if (!*r0) return 0;
    *(int32_t *)((uint8_t *)mut + FP_OFFSET(cap_r_f)) = 0;
    return 1;
}
static int mutil_state;   // the hit-by-hit hooks (below): 0 not tried, 1 hooked, -1 not (logged)
static int caps_wanted(void) { return mode_on && wounds_on && !wound_game && cap_state >= 0 && !admin_is_client(); }
// Build 43: the cap that stays on a living Common (scan, OnDamage) only without the hit-by-hit hooks. Joon's build 42
// session froze (25 Oct 08:17:50, the game thread stuck as a garbage collection began, Ridden fighting over a bile jar
// and taking Blighted acid hits: kinds build 42 left with that cap at 0 except inside the game's pick). Not proven to
// be the cause, but with the hooks working (build 42 log: every bow pick skipped) the cap is only needed inside the
// game's wound pick, so with the hooks the rest of the game never sees anything but its own value.
static int caps_fallback(void) { return caps_wanted() && mutil_state != 1; }
static void cap_living(Tracked *x) {   // the scan, a living Ridden just listed (game thread)
    char a[160];
    int32_t r0;
    if (GetCurrentThreadId() != game_tid || !caps_fallback() || !is_common_class(x->a)) return;
    if (cap_zero(x->a, x, &r0) != 1) return;
    n_cap_scan++;
    if (cap_logs < 4) {
        cap_logs++;
        LOG("ragdoll wounds: %s listed alive: the game's own open wounds off (MaxRuptureWounds %d -> 0)", oname(x->a, a, sizeof a), r0);
    }
}
// In the ProcessEvent hook (host), at a living Common's OnDamage event: one the scan hadn't listed yet gets the cap for
// its next hits (this hit's own mutilation is already done). Nothing heavy: a lookup, an int write.
static void cap_hit(UObject *o, const void *params) {
    char kn[96], a[160], bn[64];
    if (GetCurrentThreadId() != game_tid || !caps_fallback() || !is_common_class(o)) return;
    const uint8_t *di = (const uint8_t *)params + FP_OFFSET(dmgi.info);
    if (dmgi.stumble && bool_at(dmgi.stumble, di) == 1) return;
    PushKind *k = kind_of(obj_field(dmgi.type, di), kn, sizeof kn);
    if (!strcmp(k->key, "Stumble")) return;
    Tracked *x = any_tracked(o);
    if (x) { x->dmg_t = now_s(); snprintf(x->dmg_kind, sizeof x->dmg_kind, "%s", k->key); }
    int32_t r0;
    if (cap_zero(o, x, &r0) != 1) return;
    n_cap_hits++;
    if (cap_logs < 12) {
        cap_logs++;
        FName bone = dmgi.bone ? *(const FName *)(di + FP_OFFSET(dmgi.bone)) : (FName){0, 0};
        LOG("ragdoll wounds: %s hit by %s (%s) on %s before the scan listed it: the game's own open wounds off from its next "
            "hit (MaxRuptureWounds %d -> 0)", oname(o, a, sizeof a), k->key, kn, bone.idx ? ue_name(bone, bn, sizeof bn) : "-", r0);
    }
}
// /ragdoll wounds on, game off: every living Common already listed gets the cap (game thread)
static int caps_living_all(void) {
    int n = 0;
    for (int i = 0; i < ntr; i++) {
        Tracked *x = &tr[i];
        int32_t r0;
        if (x->died_at || x->done || !ue_object_usable(x->a, x->ai) || !is_common_class(x->a)) continue;
        if (caps_fallback() && cap_zero(x->a, x, &r0) == 1) n++;
    }
    return n;
}

// ---- the game's own wounds, hit by hit (build 42, from Joon's code dump of 25 Oct) ----
// The dump (b4b-codedump: 0x14172b4b0 two calls deep and the mutilation code 0x141a20000-0x141a30000, exe checked
// against the build): the game applies a hit in 0x14172b4b0 (rcx = the hit character's damage component, rdx = the
// FDamageInfo, 0xC0 bytes: its default constructor 0x1413e1c50 zeroes 0xC0 and puts the damage type at +0x18; copied
// whole and broadcast to two delegates first), then calls 0x141a24a30 with the character's MutilationComponent as its
// only argument (void; its one caller ignores rax): the game's wound pick for that hit (its rule list and tag queries,
// then BreakOff 0x141a23290 / Rupture 0x141a25720, the 3 call stacks of the build 41 log). Our own wounds never go
// through it (they write MutilationState and call OnRep_MutilationState). So, host, game thread, Commons only:
//   - at 0x14172b4b0 the hit's damage type is noted for the length of the call (a stack: a hit can cause hits);
//   - at 0x141a24a30, by that hit's kind: a no-wound kind (/ragdoll wounds none, default the bow): the pick is skipped,
//     no wound of the game's at all for that hit, the head pop included (MaxWounds 0 didn't stop it, build 41); a kind
//     whose wounds are ours (the wound and head-only kinds): MaxRuptureWounds 0 for this pick, so only the game's
//     BreakOffs (the head on a killing headshot, a limb where the shot lands); any other kind (melee, blasts, fire):
//     the game's own values; a hit whose damage type can't be read: 0 for this pick. Build 43: after the pick the
//     value it had goes back (the game's own: with the hooks in, nothing else sets it).
// The damage type is read through the OnDamage event's DamageInfo struct (dmgi), checked to be the same 0xC0 bytes.
// Without the hooks (signature mismatch) the scan's cap stays instead (caps_fallback): every kind without the game's
// open wounds.
#define ADDR_HIT_APPLY  VA(0x14172b4b0ull)
#define ADDR_MUT_PICK   VA(0x141a24a30ull)
static const uint8_t SIG_HIT_APPLY[] = {0x40,0x53,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x40,0x01,0x00,0x00,0x83,0xba,0xb4,
                                        0x00,0x00,0x00,0xff,0x4c,0x8b,0xfa,0x4c};
static const uint8_t SIG_MUT_PICK[] = {0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x48,0x8d,0xac,0x24,0x18,0xff,0xff,0xff,0x48,
                                       0x81,0xec,0xe8,0x01,0x00,0x00,0x48,0x83};
typedef void (*HitApplyFn)(UObject *comp, const void *dinfo, void *r8, void *r9);
typedef void (*MutPickFn)(UObject *mut, void *rdx, void *r8, void *r9);
static HitApplyFn orig_hit_apply;
static MutPickFn orig_mut_pick;
static int dinfo_ok;                  // 0 not checked, 1 the DamageInfo struct is the 0xC0 bytes, -1 not (logged)
#define HIT_STACK 8
static UObject *hit_dtype[HIT_STACK];
static int hit_depth;
static int n_pick_skip, n_pick_ours, n_pick_game, n_pick_unknown, pick_logs;

// what the game's wound pick does for a hit of kind k (NULL: its damage type wasn't read)
enum { PICK_OURS, PICK_SKIP, PICK_GAME };
static int pick_rule(const PushKind *k) {
    if (!k) return PICK_OURS;                                       // unread: the cap stays
    if (!k->radial && kind_in(wound_none, k->key)) return PICK_SKIP; // the bow: none of the game's wounds
    if (k->radial || !kind_handled(k->key)) return PICK_GAME;        // melee, blasts, fire: the game's own
    return PICK_OURS;                                                // our wound kinds: its BreakOffs, no open wounds
}
static UObject *live_obj(UObject *o) {   // o, if it is an object in the object array
    int32_t i = o ? U_INDEX(o) : -1;
    return i >= 0 && i < ue_num_objects() && ue_object_at(i) == o ? o : NULL;
}
static void hit_apply_detour(UObject *comp, const void *di, void *r8, void *r9) {
    UObject *dt = NULL;
    if (di && dmgi.state == 1 && GetCurrentThreadId() == game_tid) {
        if (!dinfo_ok) {
            UStruct *st = (UStruct *)FP_SUB(dmgi.info);
            int sz = st ? US_PROPSSIZE(st) : -1;
            dinfo_ok = sz == 0xC0 && dmgi.type && FP_OFFSET(dmgi.type) == 0x18 ? 1 : -1;   // as the code: 0xC0, +0x18
            LOG("ragdoll wounds: the game's hits: DamageInfo %d bytes, its DamageType at +0x%x: %s", sz,
                dmgi.type ? FP_OFFSET(dmgi.type) : -1, dinfo_ok == 1 ? "read at each hit" : "NOT the code's layout (0xC0 bytes, +0x18): not read");
        }
        if (dinfo_ok == 1) dt = live_obj(obj_field(dmgi.type, (const uint8_t *)di));
    }
    if (hit_depth < HIT_STACK) hit_dtype[hit_depth] = dt;
    hit_depth++;
    orig_hit_apply(comp, di, r8, r9);
    hit_depth--;
}
static void mut_pick_detour(UObject *mut, void *rdx, void *r8, void *r9) {
    char kn[96], a[160];
    UObject *owner = mut && GetCurrentThreadId() == game_tid && caps_wanted() ? U_OUTER(mut) : NULL;
    if (!owner || !live_obj(owner) || !is_common_class(owner) || cap_init(mut) != 1) { orig_mut_pick(mut, rdx, r8, r9); return; }
    UObject *dt = hit_depth > 0 && hit_depth <= HIT_STACK ? hit_dtype[hit_depth - 1] : NULL;
    PushKind *k = dt ? kind_of(dt, kn, sizeof kn) : NULL;
    if (!dt) snprintf(kn, sizeof kn, "-");
    int32_t w, r;
    if (!cap_orig(mut, &w, &r)) { orig_mut_pick(mut, rdx, r8, r9); return; }
    int32_t *pr = (int32_t *)((uint8_t *)mut + FP_OFFSET(cap_r_f)), was = *pr;   // build 43: put back after the pick
    const char *what;
    int rule = pick_rule(k);
    if (rule == PICK_SKIP) {
        n_pick_skip++;
        what = "skipped: no wound of the game's (a no-wound kind)";
    } else if (rule == PICK_GAME) {
        n_pick_game++;
        *pr = r;
        orig_mut_pick(mut, rdx, r8, r9);
        *pr = was;
        what = "the game's own (its open wounds for this hit)";
    } else {
        if (k) n_pick_ours++; else n_pick_unknown++;
        *pr = 0;
        orig_mut_pick(mut, rdx, r8, r9);
        *pr = was;
        what = k ? "no open wounds (MaxRuptureWounds 0 for this pick): the game's head or limb only" :
                   "damage type not read: no open wounds";
    }
    if (pick_logs < 16) {
        pick_logs++;
        LOG("ragdoll wounds: the game's wound pick for %s hit by %s (%s): %s", oname(owner, a, sizeof a), k ? k->key : "?", kn, what);
    }
}
static void mutil_arm(void) {   // game thread, at /ragdoll on; the hooks stay (they act only while caps_wanted())
    if (mutil_state) return;
    if (memcmp((void *)ADDR_HIT_APPLY, SIG_HIT_APPLY, sizeof SIG_HIT_APPLY) ||
        memcmp((void *)ADDR_MUT_PICK, SIG_MUT_PICK, sizeof SIG_MUT_PICK)) {
        mutil_state = -1;
        LOG("ragdoll wounds: signature mismatch at the game's hit / wound pick: per-hit control off (the scan's cap stays)");
        return;
    }
    if (MH_CreateHook((void *)ADDR_HIT_APPLY, (void *)hit_apply_detour, (void **)&orig_hit_apply) != MH_OK ||
        MH_CreateHook((void *)ADDR_MUT_PICK, (void *)mut_pick_detour, (void **)&orig_mut_pick) != MH_OK ||
        MH_EnableHook((void *)ADDR_HIT_APPLY) != MH_OK || MH_EnableHook((void *)ADDR_MUT_PICK) != MH_OK) {
        MH_DisableHook((void *)ADDR_HIT_APPLY);
        MH_DisableHook((void *)ADDR_MUT_PICK);
        mutil_state = -1;
        LOG("ragdoll wounds: the game's hit / wound pick could not be hooked: per-hit control off (the scan's cap stays)");
        return;
    }
    mutil_state = 1;
    LOG("ragdoll wounds: hooked the game's hit and wound pick: no-wound kinds (%s) get none of the game's wounds, ours no "
        "open wounds (only during the pick), the rest the game's own", wound_none);
}

// /ragdoll wounds game on: every Ridden we changed gets the game's values back (game thread)
static int caps_restore_all(void) {
    int n = 0;
    int32_t w, r;
    for (int i = 0; cap_state == 1 && i < ntr; i++) {
        Tracked *x = &tr[i];
        if (!x->mut || !ue_object_usable(x->mut, x->muti) || !cap_orig(x->mut, &w, &r)) continue;
        *(int32_t *)((uint8_t *)x->mut + FP_OFFSET(cap_w_f)) = w;
        *(int32_t *)((uint8_t *)x->mut + FP_OFFSET(cap_r_f)) = r;
        n++;
    }
    return n;
}
// at the ragdoll (build 41): the body is dead, so the game makes no more wounds of its own on it (its killing hit's
// are in MutilationState by the time we see the death: build 40 log, "at death"); the caps only ever need to be 0 on
// living Ridden. Back to the game's values, so nothing later (the stand-in swap, the game's own code reading them) sees
// the 0 we set. Our own wounds on the body raise them around their call anyway (mstate_onrep).
static void caps_restore_one(Tracked *t) {
    int32_t w, r;
    if (cap_state != 1 || !is_common_class(t->a)) return;
    // hit before the scan listed it: cap_hit found the component itself and couldn't keep it on the entry
    UObject *mut = t->mut && ue_object_usable(t->mut, t->muti) ? t->mut : comp_of(t->a, "MutilationComponent");
    if (!mut || !cap_orig(mut, &w, &r)) return;
    *(int32_t *)((uint8_t *)mut + FP_OFFSET(cap_w_f)) = w;
    *(int32_t *)((uint8_t *)mut + FP_OFFSET(cap_r_f)) = r;
}

// discovery (host): a mutilation the game made itself (HandleMutilationSounds outside our own calls); 3 call stacks
static int game_mut_stacks;
static void stack_log(int kind, const char *what);
static void natives_build(void);
static void game_mut_note(UObject *o, UFunction *fn, const uint8_t *params) {
    char a[160], c[96], pv[200], line[700], pn[64];
    if (GetCurrentThreadId() != game_tid || admin_is_client()) return;
    UObject *actor = owner_actor(o);
    Tracked *x = actor ? any_tracked(actor) : NULL;
    n_game_mut++;
    if (game_mut_logs >= 30) return;
    game_mut_logs++;
    size_t k = 0;
    line[0] = 0;
    for (FField *p = US_CHILDPROPS(fn); p && k + 64 < sizeof line; p = FF_NEXT(p)) {
        if (!(FP_FLAGS(p) & CPF_PARM_) || (FP_FLAGS(p) & (CPF_OUTPARM_ | CPF_RETURNPARM_))) continue;
        fmt_prop(p, params, pv, sizeof pv, 1);
        k += (size_t)snprintf(line + k, sizeof line - k, " %s=%s", ue_name(FF_NAME(p), pn, sizeof pn), pv);
    }
    int32_t w = -1, r = -1;
    if (x && x->mut && ue_object_usable(x->mut, x->muti)) cap_read(x->mut, &w, &r);
    char when[96];
    if (x && x->dmg_t) snprintf(when, sizeof when, "%.3f s after its last OnDamage (%s)", now_s() - x->dmg_t, x->dmg_kind);
    else snprintf(when, sizeof when, "no OnDamage of it seen");
    LOG("ragdoll wounds: the game's own mutilation on %s (%s):%s; %s, %s; its MaxWounds %d, MaxRuptureWounds %d now",
        oname(actor, a, sizeof a), cname(actor, c, sizeof c), line, when, x && x->died_at ? "seen dead" : "not seen dead yet", w, r);
    if (game_mut_stacks < 3) {
        game_mut_stacks++;
        natives_build();
        snprintf(line, sizeof line, "the game's own mutilation on %s", oname(actor, a, sizeof a));
        stack_log(ST_MUTIL, line);
    }
}

// at the ragdoll: what the game itself put on a Common killed by a kind we handle (entries with a SourceActor)
static void game_wounds_at_death(Tracked *t, const char *kind, const char *bone) {
    char a[160], rn[48], list[300] = "";
    if (admin_is_client() || !kind_handled(kind) || !is_common_class(t->a) || mi.state != 1 || !mi.source) return;
    UObject *mut = t->mut && ue_object_usable(t->mut, t->muti) ? t->mut : comp_of(t->a, "MutilationComponent");
    if (!mut) return;
    const TArray *arr = (const TArray *)((const uint8_t *)mut + FP_OFFSET(mi.arr));
    int n = 0;
    for (int i = 0; arr->data && i < arr->num && i < 32; i++) {
        const uint8_t *e = (const uint8_t *)arr->data + (size_t)i * mi.el;
        if (!*(UObject *const *)(e + FP_OFFSET(mi.source))) continue;   // ours (and the game's copies of ours): none
        n++;
        char one[64];
        snprintf(one, sizeof one, "%s %s", region_name(e[FP_OFFSET(mi.region)], rn, sizeof rn),
                 e[FP_OFFSET(mi.type)] == (uint8_t)mi.rupture ? "Rupture" : e[FP_OFFSET(mi.type)] == (uint8_t)mi.breakoff ? "BreakOff" : "?");
        str_add(list, sizeof list, one);
    }
    n_ctl_kills++;
    if (n) n_ctl_game++;
    int32_t w, r;
    cap_read(mut, &w, &r);
    if (ctl_logs < 40) {
        ctl_logs++;
        LOG("ragdoll wounds: %s killed by %s on %s: the game's own wounds: %s (MaxWounds %d, MaxRuptureWounds %d%s)",
            oname(t->a, a, sizeof a), kind, bone, n ? list : "none", w, r, wound_game ? "; /ragdoll wounds game on" : "");
    }
}

// discovery: every property of an object (its class chain down to Object), with values; `tag` names it in the log
static void dump_obj(const char *tag, UObject *o) {
    char b[128], pn[96], v[8000], a[160];
    if (!o) return;
    LOG("ragdoll wounds: %s %s (%s):", tag, oname(o, a, sizeof a), cname(o, b, sizeof b));
    for (UStruct *s = (UStruct *)U_CLASS(o); s; s = US_SUPER(s)) {
        ue_obj_name(s, b, sizeof b);
        if (!strcmp(b, "Object")) break;
        for (FField *p = US_CHILDPROPS(s); p; p = FF_NEXT(p)) {
            fmt_prop(p, (const uint8_t *)o, v, sizeof v, 4);   // build 41: 4 (arrays and their structs; tags)
            LOG("ragdoll wounds:   %s %s.%s = %s", tag, b, ue_name(FF_NAME(p), pn, sizeof pn), v);
        }
    }
}
static void dtype_probe(UObject *dtype) {   // each damage type once: what could decide the game's own wounds
    static UClass *seen[32];
    static int n_seen;
    if (!dtype || n_seen >= 32) return;
    UClass *c = U_CLASS(dtype);
    for (int i = 0; i < n_seen; i++) if (seen[i] == c) return;
    seen[n_seen++] = c;
    dump_obj("damage type", dtype);
}

// What a hit by `kind` on `bone` does to the body (see above). alive: a living Ridden. from: where the shot came from.
// 2 = a part came off, 1 = a wound opened, 0 = nothing
static int wound_apply(Tracked *t, UObject *mesh, const char *kind, FName bone, const float from[3], int alive, const char *when) {
    char a[160], c[96], bn[64], region[32], piece_s[64] = "";
    if (!wounds_on || !mesh || !kind || !kind[0]) return 0;
    if (kind_in(wound_none, kind)) return 0;   // build 41: no wounds at all (the bow)
    int heads_only = kind_in(wound_heads, kind);
    if (!heads_only && !kind_in(wound_kinds, kind)) return 0;
    if (alive && !wound_living) return 0;
    int rup = 0, wc = wound_class(ue_name(bone, bn, sizeof bn), region, sizeof region, &rup);
    n_wound_hits++;
    if (wc == WC_NONE) {
        n_wound_nomap++;
        if (wound_nomap_logs < 10) { wound_nomap_logs++; LOG("ragdoll wounds: %s hit on bone %s: no wound for it", when, bn); }
        return 0;
    }
    if (heads_only && wc != WC_HEAD) return 0;
    if (alive && (wc == WC_HEAD || (wc == WC_LEG && wound_living < 2))) { n_wound_skip++; return 0; }
    UObject *mut = comp_of(t->a, "MutilationComponent");
    if (!mut) return 0;
    mi_init(U_CLASS(mut));
    if (mi.state != 1) return 0;
    static int cfg_dumped;
    if (!cfg_dumped++) dump_obj("wound config", ue_get_ptr(mut, "WoundConfigsData"));
    int type = rup ? mi.rupture : mi.breakoff, r = mi.region ? enum_value(mi.region, region) : -1;
    if (type < 0 || r < 0 || r > 255) { n_wound_nomap++; return 0; }
    TArray *arr = (TArray *)((uint8_t *)mut + FP_OFFSET(mi.arr));
    if (rup ? mstate_has(arr, (uint8_t)r, type) : region_off(mut, (uint8_t)r) == 1) { n_wound_already++; return 0; }
    float at[3];
    if (bone_loc(mesh, bone, at) && actor_loc(t->a, at)) return 0;
    int small = kind_in(wound_small, kind);
    float mag = rup ? (small ? WOUND_IMP_SMALL : WOUND_IMP_FULL) : WOUND_IMP_LIMB;
    UClass *pc = rup ? NULL : piece_class(mut);
    int seen = pc ? actors_of(pc, NULL, 0) : -1, called = 0;
    int32_t n0 = arr->num;
    int off = mstate_apply(t, mut, arr, (uint8_t)r, type, at, from, at, mag, 0, &called);
    if (off < 0) return 0;
    if (!off) n_wound_refused++;
    else if (rup) n_wound_rupt++;
    else {
        n_wound_off++;
        int now_n = pc ? actors_of(pc, NULL, 0) : -1;
        if (pc && seen >= 0 && now_n > seen) snprintf(piece_s, sizeof piece_s, ", a piece thrown by the game");
        else if (dism_pieces && pc) {   // ours: the body's piece for this region, thrown away from the shot
            const TArray *pcs = (const TArray *)((const uint8_t *)mut + FP_OFFSET(mi.pieces));
            for (int i = 0; pcs->data && i < pcs->num && i < 64; i++) {
                if (piece_region(pcs, i) != (uint8_t)r) continue;
                const uint8_t *pe = (const uint8_t *)pcs->data + (size_t)i * mi.pel;
                int pr = throw_piece(pc, mesh, pe, from, 600.f);
                if (!pr) n_wound_pieces++;
                snprintf(piece_s, sizeof piece_s, ", our piece: %s", piece_why(pr));
                break;
            }
        }
    }
    if (off && t->ragdolled) {   // the limb code may have changed the body's physics: ours again (as dismember)
        anim_off(ue_get_ptr(mesh, "AnimScriptInstance"), comp_of(t->a, "PhysicalAnimationComponent"));
        sim_on(mesh);
        call_void(mesh, "WakeAllRigidBodies");
        t->hid = hidden_bodies(mesh);
    }
    if (wound_logs < WOUND_LOGS) {
        wound_logs++;
        LOG("ragdoll wounds: %s (%s) %s, %s on %s: %s %s%s -> %s (state %d -> %d entries, dismembered %d)%s",
            oname(t->a, a, sizeof a), cname(t->a, c, sizeof c), when, kind, bn, region, rup ? "Rupture" : "BreakOff",
            rup ? (small ? " (small)" : " (full)") : "", off ? (rup ? "opened" : "off") : "REFUSED", n0, arr->num,
            region_off(mut, (uint8_t)r), piece_s);
    }
    return off ? (rup ? 1 : 2) : 0;
}

// the killing shot (at the ragdoll, before the push): its kind and bone from the body's RecentDamageInfo
static void wound_kill(Tracked *t, UObject *mesh) {
    char tn[96];
    if (!wounds_on) return;
    UObject *hc = call_obj(t->a, "GetHealthComponent");
    if (!hc || dinfo_init(hc) != 1 || !dinfo.bone) return;
    const uint8_t *best = killing_entry(t, (const TArray *)((char *)hc + dinfo.arr_off), 0);
    if (!best) return;
    UObject *dt = dinfo.type ? obj_field(dinfo.type, best) : NULL;
    PushKind *kind = dinfo.type ? kind_of(dt, tn, sizeof tn) : push_kind("default", 0);
    if (kind->radial) return;   // blasts: /ragdoll dismember
    dtype_probe(dt);
    FName bone = *(const FName *)(best + FP_OFFSET(dinfo.bone));
    char bn[64];
    game_wounds_at_death(t, kind->key, ue_name(bone, bn, sizeof bn));   // build 41: did the game add any of its own?
    float from[3];
    const float *sl = dinfo.srcloc ? (const float *)(best + FP_OFFSET(dinfo.srcloc)) : NULL;
    UObject *src = dinfo.source ? obj_field(dinfo.source, best) : NULL;
    if (sl && (sl[0] != 0.f || sl[1] != 0.f || sl[2] != 0.f)) memcpy(from, sl, sizeof from);
    else if (!src || actor_loc(owner_actor(src) ? owner_actor(src) : src, from)) {
        if (actor_loc(t->a, from)) return;
        from[0] -= 100.f;   // no source: somewhere off the body
    }
    wound_apply(t, mesh, kind->key, bone, from, 0, "killing shot");
}

// blast victims: 1 or 2 open wounds (chest, stomach in a random order), the number of wounds opened; names into `out`
static int blast_wounds(Tracked *t, UObject *mut, TArray *arr, UObject *mesh, char *out, size_t n) {
    char rn[48];
    if (!wounds_on || !wound_blast || mi.rupture < 0 || !mi.region) return 0;
    int regs[2] = {enum_value(mi.region, "Chest"), enum_value(mi.region, "Stomach")}, want = 1 + rnd_below(2), got = 0, called = 0;
    if (rnd_below(2)) { int x = regs[0]; regs[0] = regs[1]; regs[1] = x; }
    static FName spine;
    if (!spine.idx) spine = fname_of(L"spine_02");
    float at[3];
    if (bone_loc(mesh, spine, at) && actor_loc(t->a, at)) return 0;
    for (int i = 0; i < 2 && got < want; i++) {
        if (regs[i] < 0 || regs[i] > 255 || mstate_has(arr, (uint8_t)regs[i], mi.rupture)) continue;
        if (mstate_apply(t, mut, arr, (uint8_t)regs[i], mi.rupture, at, t->blast_at, t->blast_at, WOUND_IMP_FULL, 0, &called) == 1) {
            got++;
            str_add(out, n, region_name(regs[i], rn, sizeof rn));
        }
    }
    n_blast_rupt += got;
    return got;
}

// ---- living hits (host): the game's OnDamage event on a Ridden (BndEvt__Health_..._OnDamageSignature, build 14) ----
// Its DamageInfo has the bone and the damage type (server only: clients get the host's wounds by replication).
// Learned per Ridden class as the scan finds them; the ProcessEvent hook compares the function pointer only and
// queues the hit; the tick handles it (Ridden still alive only: a killing hit is the ragdoll's).
static UFunction *dmg_fns[32];
static int n_dmg_fns;
static UClass *dmg_seen[48];
static int n_dmg_seen;
typedef struct { UObject *a, *dtype; FName bone; float from[3]; } WoundHit;
static WoundHit woundq[WOUNDQ];
static int n_woundq, n_woundq_drop, n_living_hits;

static void dmg_fields(UFunction *fn) {
    char t[64], sn[96], fnm[64], line[1024];
    if (dmgi.state) return;
    dmgi.state = -1;
    FField *p = ue_find_prop(fn, "DamageInfo");
    UStruct *st = p && !strcmp(ue_name(*(FName *)FF_CLASS(p), t, sizeof t), "StructProperty") ? (UStruct *)FP_SUB(p) : NULL;
    if (!st) { LOG("ragdoll wounds: the OnDamage event has no DamageInfo struct: no wounds on living Ridden"); return; }
    dmgi.info = p;
    dmgi.bone = ue_find_prop(st, "BoneName");
    dmgi.type = ue_find_prop(st, "DamageType");
    dmgi.stumble = ue_find_prop(st, "bStumbleEvent");
    dmgi.point = ue_find_prop(st, "ImpactPoint");
    dmgi.dir = ue_find_prop(st, "ImpactDirection");
    size_t k = (size_t)snprintf(line, sizeof line, "ragdoll wounds: living hits from %s(DamageInfo: %s):", ue_obj_name(fn, fnm, sizeof fnm),
                                ue_obj_name(st, sn, sizeof sn));
    FField *f[5] = {dmgi.bone, dmgi.type, dmgi.stumble, dmgi.point, dmgi.dir};
    const char *nm[5] = {"BoneName", "DamageType", "bStumbleEvent", "ImpactPoint", "ImpactDirection"};
    for (int i = 0; i < 5; i++) k += (size_t)snprintf(line + k, sizeof line - k, " %s%s", nm[i], f[i] ? "" : " MISSING");
    LOG("%s", line);
    if (dmgi.bone && dmgi.type) dmgi.state = 1;
}

static void dmg_learn(UObject *actor) {   // game thread (the scan): this Ridden class's OnDamage events
    char b[160];
    UClass *c = actor ? U_CLASS(actor) : NULL;
    if (!c) return;
    for (int i = 0; i < n_dmg_seen; i++) if (dmg_seen[i] == c) return;
    if (n_dmg_seen < 48) dmg_seen[n_dmg_seen++] = c;
    for (UStruct *s = (UStruct *)c; s; s = US_SUPER(s)) {
        ue_obj_name(s, b, sizeof b);
        if (!strcmp(b, "GobiCharacter") || !strcmp(b, "Character") || !strcmp(b, "Object")) break;
        for (UObject *f = US_CHILDREN(s); f; f = UF_NEXT(f)) {
            ue_obj_name(f, b, sizeof b);
            if (strncmp(b, "BndEvt__", 8) || !strstr(b, "OnDamageSignature")) continue;
            int dup = 0;
            for (int i = 0; i < n_dmg_fns; i++) if (dmg_fns[i] == (UFunction *)f) dup = 1;
            if (dup || n_dmg_fns >= 32) continue;
            dmg_fns[n_dmg_fns++] = (UFunction *)f;
            dmg_fields((UFunction *)f);
            if (n_dmg_fns == 1) pe_update();   // the hook may be off (no blast or corpse-hit function found)
        }
    }
}

static void wound_note(UObject *o, void *params) {   // in the ProcessEvent hook: queue it, nothing heavy here
    if (GetCurrentThreadId() != game_tid) return;
    const uint8_t *di = (const uint8_t *)params + FP_OFFSET(dmgi.info);
    if (dmgi.stumble && bool_at(dmgi.stumble, di) == 1) return;
    FName bone = *(const FName *)(di + FP_OFFSET(dmgi.bone));
    if (!bone.idx) return;
    if (n_woundq == WOUNDQ) { n_woundq_drop++; return; }
    WoundHit *h = &woundq[n_woundq++];
    h->a = o;
    h->dtype = obj_field(dmgi.type, di);
    h->bone = bone;
    const float *pt = dmgi.point ? (const float *)(di + FP_OFFSET(dmgi.point)) : NULL;
    const float *dr = dmgi.dir ? (const float *)(di + FP_OFFSET(dmgi.dir)) : NULL;
    for (int c = 0; c < 3; c++) h->from[c] = (pt ? pt[c] : 0.f) - (dr ? dr[c] * 100.f : 0.f);
}

static void wound_tick(void) {   // game thread, after this tick's deaths are ragdolled
    char kn[96];
    for (int i = 0; i < n_woundq; i++) {
        WoundHit *h = &woundq[i];
        Tracked *x = any_tracked(h->a);
        if (!x || x->died_at || x->done || !ue_object_usable(x->a, x->ai) || call_bool(x->ls, "IsAlive") != 1) continue;
        PushKind *k = kind_of(h->dtype, kn, sizeof kn);
        if (k->radial || !strcmp(k->key, "Stumble")) continue;
        n_living_hits++;
        wound_apply(x, ue_get_ptr(x->a, "Mesh"), k->key, h->bone, h->from, 1, "alive");
    }
    n_woundq = 0;
}

// build 41: also for the game's own wounds (cap_hit), with /ragdoll wounds living off
static int wound_hook_wanted(void) { return wounds_on && (wound_living || !wound_game) && n_dmg_fns > 0; }
static void wound_hook(UObject *o, UFunction *fn, void *params) {   // in the ProcessEvent hook (any call while on)
    if (!wounds_on || dmgi.state != 1) return;
    for (int i = 0; i < n_dmg_fns; i++) if (fn == dmg_fns[i]) {
        if (!wound_game) cap_hit(o, params);   // before the game's own mutilation for this hit
        if (wound_living) wound_note(o, params);
        return;
    }
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
            int off = mstate_apply(t, mut, arr, region, type, at, t->blast_at, t->blast_at, mag, watch, &called);
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
    char wnames[48] = "";
    int wn = blast_wounds(t, mut, arr, mesh, wnames, sizeof wnames);   // build 40: open wounds (chest, stomach)
    n_limbs_off += got;
    n_limbs_refused += refused;
    n_big += big; n_small += small;
    if (bl) {
        bl->bodies++; bl->asked += want; bl->off += got; bl->refused += refused; bl->big += big; bl->small += small;
        bl->heads += head > 0; bl->heads_off += head == 2;
        if (refused) str_add(bl->refused_s, sizeof bl->refused_s, refs);
    }
    if (!got && head != 2 && !wn) {
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
        LOG("ragdoll dismember: %s: %d of %d limbs off (%s; %d big, %d small)%s%s%s, open wounds: %s; pieces: %d by the game, %d "
            "by us%s%s; OnRep_MutilationState %s, state now %d entries, %d bodies under the cuts", oname(t->a, a, sizeof a), got, want,
            offs, big, small, refused ? ", refused " : "", refs, head == 2 ? ", head OFF" : head ? ", head still on" : "",
            wn ? wnames : "none", game_pc,
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
// Build 38: also bodies that died running (momentum carry), 16 of them per /ragdoll carry change, against the speed
// they were given: whether the engine keeps it, or adds the body's own (animated) speed on top (double speed).
#define VEL_LOGS 16
static int vel_logs_d, vel_logs_c, vel_logs_r;
static const double VEL_T[] = {0.1, 0.4};
static void vel_probe_tick(Tracked *x, double now) {
    for (int i = 0; i < (int)(sizeof VEL_T / sizeof VEL_T[0]); i++) {
        if ((x->velp & (1u << i)) || now - x->died_at < VEL_T[i]) continue;
        x->velp |= 1u << i;
        int *n = x->push_dv > 0.f ? (x->dism ? &vel_logs_d : &vel_logs_c) : &vel_logs_r;
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
        LOG("ragdoll velocity: %s (%s) +%.1fs: bodies move %.0f cm/s on average (%.0f up, %.0f along the ground), fastest "
            "%.0f; push %.0f cm/s, running speed carried %.0f cm/s%s; simulating %d, montage %d, physical animation x%s",
            oname(x->a, a, sizeof a), x->push_dv > 0.f ? (x->dism ? "dismembered" : "not dismembered") : "died running",
            VEL_T[i], sqrtf(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]), sum[2], sqrtf(sum[0] * sum[0] + sum[1] * sum[1]),
            top, x->push_dv, x->carry, "",
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

// ---- momentum carry (build 38; build 39: through staggers, never slowing a body down) ----
// Joon (24 Oct): in L4D2 a common killed mid-run tumbles on; here it dropped on the spot (burning Ridden "just go
// limp", weak-gun kills "go down"). While a Ridden is alive the tick reads its CharacterMovement's Velocity (a memory
// read, no call); at the ragdoll the bodies' mean velocity along the running direction is raised to that running speed
// x /ragdoll carry: the difference is added to every body (SetAllPhysicsLinearVelocity, bAddToCurrent: the limbs keep
// their own swing), then the killing hit's push comes on top. Slower than CARRY_MIN = standing: nothing done.
// Build 38 run (Joon, 24 Oct, 240 deaths): it works (SetAllPhysicsLinearVelocity reaches every body; bodies keep about
// 65-70% along the ground at +0.1 s, 35-40% at +0.4 s: the feet drag), but
//   - SMG kills dropped on the spot: the first bullet staggers the Ridden (the game's stumble), its movement falls to
//     ~100 cm/s (the stagger animation), and the second bullet, ~0.1 s later, kills it with that speed. Waiting for the
//     stagger to end, then killing, carried the run (Joon). 23 of the 40 logged deaths were under 200 cm/s.
//     Build 39: the fastest recent speed is remembered and fades out over /ragdoll carry keep (default 1 s); at death
//     the larger of the speed now and the remembered one, faded by its age, is used. A body in flight doesn't stop
//     dead because its legs stopped a moment ago.
//   - build 38 set the mean to the running velocity, so a body already moving faster was slowed (two armored commons,
//     157 -> 37 cm/s and 107 -> 91). Build 39 only adds, along the running direction, up to the running speed.
#define CARRY_DEFAULT  1.f
#define CARRY_MIN      30.f    // cm/s: slower is standing (nothing carried)
#define CARRY_MAX      1500.f  // cm/s at most (a launch, a teleport)
#define CARRY_STALE    0.25    // s between the last speed read alive and the death: older isn't used
#define CARRY_KEEP_DEF 1.f     // s a remembered speed takes to fade to nothing (build 39)
#define CARRY_LOGS     40
static float carry_scale = CARRY_DEFAULT, carry_keep = CARRY_KEEP_DEF;
static int32_t mov_vel_off = -2;   // MovementComponent.Velocity's offset: -2 not looked up, -1 not there
static int carry_mode;             // 0 SetAllPhysicsLinearVelocity, 1 every body one by one (the first reached only the root)
static int n_carried, n_carry_still, n_carry_nomov, n_carry_fail, n_carry_mem, n_carry_faster, carry_logs;

static int mov_ok(Tracked *x) { return x->mov && ue_object_usable(x->mov, x->movi); }
static float len3(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float carry_fade(double age) {   // a remembered speed's share left after `age` s
    return carry_keep <= 0.f || age >= carry_keep ? 0.f : age <= 0 ? 1.f : (float)(1.0 - age / carry_keep);
}

static void run_note(Tracked *x, double t) {   // every tick while it is alive
    if (!mov_ok(x)) return;
    if (mov_vel_off == -2) {
        char tt[64], sn[64];
        FField *f = ue_find_prop(U_CLASS(x->mov), "Velocity");
        mov_vel_off = f && !strcmp(ue_name(*(FName *)FF_CLASS(f), tt, sizeof tt), "StructProperty") &&
                      !strcmp(ue_obj_name(FP_SUB(f), sn, sizeof sn), "Vector") ? FP_OFFSET(f) : -1;
        LOG("ragdoll carry: %s", mov_vel_off >= 0 ? "running speeds from MovementComponent.Velocity"
                                                  : "no MovementComponent.Velocity (a Vector): no running speed carried");
    }
    if (mov_vel_off < 0) return;
    memcpy(x->run_v, (char *)x->mov + mov_vel_off, sizeof x->run_v);
    x->run_t = t;
    // build 39: the fastest recent speed, faded by its age; a faster one now replaces it
    if (!x->mem_t || len3(x->run_v) >= len3(x->mem_v) * carry_fade(t - x->mem_t)) {
        memcpy(x->mem_v, x->run_v, sizeof x->mem_v);
        x->mem_t = t;
    }
}

// the mean velocity of the body's simulated bodies (parts cut off left out), cm/s; how many bodies were read
static int mean_velocity(Tracked *t, UObject *mesh, float out[3]) {
    Call k;
    out[0] = out[1] = out[2] = 0;
    if (!call_prep(&k, mesh, "GetPhysicsLinearVelocity")) return 0;
    FName *bn = carg(&k, "BoneName");
    float *v = carg(&k, "ReturnValue");
    if (!v) return 0;
    const BodyList *bl = bodies_of(mesh);
    int nb = bl && bn ? bl->n : 0, cnt = 0;
    for (int b = nb ? 0 : -1; b < nb; b++) {   // no list: the root body only
        if (b >= 0 && b < 32 && ((t->hid >> b) & 1)) continue;
        if (b >= 0) *bn = bl->bone[b];
        call_go(&k);
        for (int c = 0; c < 3; c++) out[c] += v[c];
        cnt++;
    }
    for (int c = 0; c < 3; c++) out[c] /= (float)(cnt ? cnt : 1);
    return cnt;
}

// d (cm/s) added to every body's velocity; 0 = done
static int add_velocity(UObject *mesh, const BodyList *bl, const float d[3]) {
    Call k;
    if (carry_mode == 0 && call_prep(&k, mesh, "SetAllPhysicsLinearVelocity")) {
        float *nv = carg(&k, "NewVel");
        if (nv) { memcpy(nv, d, 3 * sizeof(float)); SET(&k, "bAddToCurrent", uint8_t, 1); call_go(&k); return 0; }
    }
    if (!bl || !call_prep(&k, mesh, "SetPhysicsLinearVelocity")) return -1;
    float *nv = carg(&k, "NewVel");
    FName *bn = carg(&k, "BoneName");
    if (!nv || !bn) return -1;
    for (int i = 0; i < bl->n; i++) {
        memcpy(nv, d, 3 * sizeof(float));
        SET(&k, "bAddToCurrent", uint8_t, 1);
        *bn = bl->bone[i];
        call_go(&k);
    }
    return 0;
}

static void carry_body(Tracked *t, UObject *mesh) {
    char a[160], c[160], from[96];
    if (carry_scale <= 0.f) return;
    if (!mov_ok(t) || mov_vel_off < 0 || !t->run_t || t->died_at - t->run_t > CARRY_STALE) { n_carry_nomov++; return; }
    // the speed at death, or the remembered one faded by its age if that is more (a stagger stopped the legs)
    double mem_age = t->mem_t ? t->died_at - t->mem_t : 1e9;
    float fade = carry_fade(mem_age), run[3];
    int mem = t->mem_t && len3(t->mem_v) * fade > len3(t->run_v);
    for (int i = 0; i < 3; i++) run[i] = mem ? t->mem_v[i] * fade : t->run_v[i];
    float rs = len3(run);
    if (rs < CARRY_MIN) { n_carry_still++; return; }
    if (mem) snprintf(from, sizeof from, "%.0f cm/s %.2f s before, x%.2f", len3(t->mem_v), mem_age, fade);
    else snprintf(from, sizeof from, "the speed at death");
    float k = carry_scale;
    if (rs * k > CARRY_MAX) k = CARRY_MAX / rs;
    float dir[3] = {run[0] / rs, run[1] / rs, run[2] / rs}, want = rs * k;
    const BodyList *bl = bodies_of(mesh);
    float before[3], after[3], d[3];
    if (!mean_velocity(t, mesh, before)) { n_carry_fail++; return; }
    float par = dot3(before, dir);   // how fast the body already goes that way
    if (par >= want) {               // faster already (the hit, the stagger): left as it is
        n_carry_faster++;
        if (carry_logs < CARRY_LOGS) {
            carry_logs++;
            LOG("ragdoll carry: %s (%s) ran %.0f cm/s (%s): its bodies already go %.0f cm/s that way: left as they are",
                oname(t->a, a, sizeof a), cname(t->a, c, sizeof c), rs, from, par);
        }
        return;
    }
    for (int i = 0; i < 3; i++) d[i] = dir[i] * (want - par);
    if (add_velocity(mesh, bl, d)) {
        if (!n_carry_fail++) LOG("ragdoll carry: no SetAllPhysicsLinearVelocity / SetPhysicsLinearVelocity(NewVel, bAddToCurrent, BoneName): nothing carried");
        return;
    }
    mean_velocity(t, mesh, after);
    const char *how = "";
    float got = dot3(after, dir) - par;
    if (carry_mode == 0 && want - par > 50.f && got < 0.5f * (want - par)) {   // it moved the root body only, it seems
        carry_mode = 1;
        LOG("ragdoll carry: SetAllPhysicsLinearVelocity moved the mean by %.0f of %.0f cm/s (the root body only?): every body "
            "is set one by one from now on", got, want - par);
        float rest[3];
        for (int i = 0; i < 3; i++) rest[i] = dir[i] * (want - par - got);
        add_velocity(mesh, bl, rest);
        mean_velocity(t, mesh, after);
        how = ", every body set one by one";
    }
    t->carry = want;
    n_carried++;
    if (mem) n_carry_mem++;
    if (carry_logs < CARRY_LOGS) {
        carry_logs++;
        LOG("ragdoll carry: %s (%s) ran %.0f cm/s (%s; at death %.0f, read %.3f s before we saw it dead; its movement reads %.0f "
            "now): bodies %.0f cm/s that way (%.0f in all) -> %.0f (%.0f in all), asked %.0f (x%.2f)%s", oname(t->a, a, sizeof a),
            cname(t->a, c, sizeof c), rs, from, len3(t->run_v), t->died_at - t->run_t,
            len3((const float *)((char *)t->mov + mov_vel_off)), par, len3(before), dot3(after, dir), len3(after), want, k, how);
    }
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
    if (sim != 1) { LOG("ragdoll: %s is not simulating after the calls", oname(t->a, a, sizeof a)); caps_restore_one(t); return -1; }
    mstate_note(t);       // build 27: what the game wrote on the body (limbs it took off)
    t->hid = hidden_bodies(mesh);   // build 40: parts lost while alive stay out of the rest test
    wound_kill(t, mesh);  // build 40: the killing shot opens a wound or takes a part off where it hit, before the push
    carry_body(t, mesh);  // build 38: it keeps its running speed, before the push
    push_body(t, mesh);   // step 2: the killing hit throws the body (blast victims: /ragdoll dismember first)
    caps_restore_one(t);  // build 41: dead now, the game adds no more wounds of its own: its caps back (see cap_hit)
    return 0;
}

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
    t->mass = 0; t->shots = 0; t->relcount = 0;
    t->carry = 0; t->kind[0] = 0; t->run_t = 0; t->mem_t = 0; t->velp = 0; t->push_dv = 0;
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
    if (x->relcount < 255 && x->relcount++) return 0;   // build 35: released again after a shot held it: counted once
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

// ---- shots on dead bodies (build 35): every hit on a ragdoll that still simulates shoves it ----
// Dead bodies take no damage event (build 14 trace): the game tells only its corpse manager, through the corpse's
// ProceduralHitReactionComponent: UCorpseManager::OnCorpseHit(HitActor), no direction, bone or point. The game's own
// nudge on a simulating body is tiny (Joon, 16 Oct: the same at /ragdoll push 0). Joon's choice (17 Oct): our own
// shove, not finding and scaling the game's nudge first. The hit is queued in the ProcessEvent hook; the tick finds
// who fired:
//   aim trace     a hero whose aim (GetActorEyesViewPoint: where shots start and go, measured with bullet holes in
//                 thirdperson.c; bots and other players too) runs through the body (K2_LineTraceComponent on its
//                 mesh, i.e. its physics bodies): that bone, at that point;
//   closest bone  else the hero whose aim passes closest to one of the body's bones, within SHOT_CONE degrees (or
//                 SHOT_NEAR cm): shotgun pellets off the centre, recoil, a body that slid since the shot.
// The shove is a real impulse along that aim (AddImpulseAtLocation, kg cm/s, as the killing hit's), on that bone or its
// nearest parent with a body. Strength = the shooter's weapon kind (InventoryComponent.SelectedItemActor's class:
// AR02_1_BP_C -> "AR", the push table's keys) x /ragdoll shot. All hits on one body in one frame (a shotgun's pellets)
// make one shove. Left out: hits within SHOT_AFTER_DEATH of the death (the killing hit's, already pushed) and hits near
// an explosion of the last SHOT_BLAST_S (the explosion's own, not anyone's aim). A body the settle hold had released
// (at rest, the game's swap coming) is held again, so steady fire keeps it physical up to the SETTLE_CAP; one already
// swapped for the stand-in can't move (a PoseableMesh), and its hidden real body has no collision to be hit.
// Co-op: whether a client's game calls OnCorpseHit at all is unknown (the status line counts the hits).
#define SHOT_DEFAULT      2.f      // x the push table per shove: about half a killing hit's throw per shot (first guess)
#define SHOT_RANGE        10000.f  // cm along a hero's aim
#define SHOT_CONE         10.f     // degrees off the aim, for the closest bone
#define SHOT_NEAR         40.f     // cm off the aim, always close enough (short range)
#define SHOT_AFTER_DEATH  0.05     // s: hits this close to the death are the killing hit's
#define SHOT_BLAST_S      0.25     // s around an explosion: hits within BLAST_RANGE of it are its own
#define SHOT_LOGS         40       // shoves logged per /ragdoll on
#define SHOT_MISS_LOGS    20       // hits with no shooter found, logged per /ragdoll on
#define SHOT_DUMPS        3        // OnCorpseHit calls logged with their parameters per /ragdoll on
#define RC_SETTLED        0x1D2    // URagdollComponent: set by the swap (code dump; build 32 saw it on 12 of 12 swaps)
#define SHOTQ             64
#define DEG               57.29578f
static float shot_scale = SHOT_DEFAULT;
static int shot_state;   // OnCorpseHit: 0 not looked up, 1 ok, -1 missing (looked up again at /ragdoll on)
static int lt_state;     // K2_LineTraceComponent: 0 not tried, 1 ok, -1 missing (closest bone only)
static struct { UObject *a; double t; } shotq[SHOTQ];
static int n_shotq;
static int n_shot_hits, n_shoves, n_shove_trace, n_shove_near, n_shot_reheld, n_shot_noshooter, n_shot_early, n_shot_frozen,
           n_shot_blast, n_shot_notours, n_shot_fail, n_shot_dropped, shot_logs, shot_miss_logs, shot_dumps;
typedef struct { UObject *h; float eye[3], aim[3]; UObject *item; } Shooter;

static void shot_reset(void) { n_shotq = 0; }

static void shot_resolve(void) {
    static const char *const MGR[] = {"CorpseManager", "GobiCorpseManager", NULL};
    char cn[96], pn[64], t[64], line[512];
    if (shot_state) return;
    shot_state = -1;
    UFunction *fn = NULL;
    for (int i = 0; MGR[i] && !fn; i++) { UClass *c = cls(MGR[i]); fn = c ? ue_find_function(c, "OnCorpseHit") : NULL; }
    FField *af = fn ? ue_find_prop(fn, "HitActor") : NULL;
    for (FField *p = fn && !af ? US_CHILDPROPS(fn) : NULL; p; p = FF_NEXT(p))   // else its first object parameter
        if ((FP_FLAGS(p) & CPF_PARM_) && !strcmp(ue_name(*(FName *)FF_CLASS(p), t, sizeof t), "ObjectProperty")) { af = p; break; }
    if (!fn || !af) {
        LOG("ragdoll shot: %s: shots on dead bodies off", fn ? "OnCorpseHit has no object parameter" : "no CorpseManager.OnCorpseHit");
        return;
    }
    size_t k = (size_t)snprintf(line, sizeof line, "ragdoll shot: hits on bodies from %s.OnCorpseHit(", ue_obj_name(U_OUTER(fn), cn, sizeof cn));
    for (FField *p = US_CHILDPROPS(fn); p && k + 64 < sizeof line; p = FF_NEXT(p))
        if (FP_FLAGS(p) & CPF_PARM_)
            k += (size_t)snprintf(line + k, sizeof line - k, "%s%s:%s@0x%x", p == US_CHILDPROPS(fn) ? "" : " ",
                                  ue_name(FF_NAME(p), pn, sizeof pn), ue_name(*(FName *)FF_CLASS(p), t, sizeof t), FP_OFFSET(p));
    LOG("%s), the body = %s", line, ue_name(FF_NAME(af), pn, sizeof pn));
    corpse_hit_fn = fn;
    corpse_hit_actor_f = af;
    shot_state = 1;
}

// in the ProcessEvent hook: queue the hit for the tick (nothing heavy inside someone else's call)
static void shot_note(UObject *o, UFunction *fn, void *params) {
    n_shot_hits++;
    UObject *a = corpse_hit_actor_f ? *(UObject **)((uint8_t *)params + FP_OFFSET(corpse_hit_actor_f)) : NULL;
    if (!a || GetCurrentThreadId() != game_tid) { n_shot_dropped++; return; }
    if (shot_dumps < SHOT_DUMPS) {
        Tracked *x = dead_tracked(a);
        shot_dumps++;
        log_params("corpsehit", x ? now_s() - x->died_at : 0, a, o, fn, params);
    }
    if (shot_scale <= 0.f) return;
    if (n_shotq == SHOTQ) { n_shot_dropped++; return; }
    shotq[n_shotq].a = a;
    shotq[n_shotq].t = now_s();
    n_shotq++;
}

// every hero (players and bots): where its shots start and go, and what it holds
static int shooters(Shooter *s, int max) {
    UObject *hs[8];
    int n = actors_of(cls("HeroCharacter"), hs, 8), k = 0;
    for (int i = 0; i < n && i < 8 && k < max; i++) {
        Call c;
        if (!hs[i] || !call_prep(&c, hs[i], "GetActorEyesViewPoint")) continue;
        call_go(&c);
        float *l = carg(&c, "OutLocation"), *r = carg(&c, "OutRotation");
        if (!l || !r) continue;
        Shooter *x = &s[k++];
        x->h = hs[i];
        memcpy(x->eye, l, sizeof x->eye);
        float p = r[0] / DEG, y = r[1] / DEG;   // FRotator (pitch, yaw, roll) in degrees
        x->aim[0] = cosf(p) * cosf(y); x->aim[1] = cosf(p) * sinf(y); x->aim[2] = sinf(p);
        UObject *inv = call_obj(hs[i], "GetInventoryComponent");
        x->item = inv ? ue_get_ptr(inv, "SelectedItemActor") : NULL;
    }
    return k;
}

static float d3(const float a[3], const float b[3]) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}

// along a hero's aim through the body's mesh: 1 = hit (point, bone, distance), 0 = missed, -1 = can't trace
static int aim_trace(UObject *mesh, const Shooter *s, float hit[3], FName *bone, float *dist) {
    Call k;
    if (lt_state < 0) return -1;
    float *st = NULL, *en = NULL;
    if (call_prep(&k, mesh, "K2_LineTraceComponent")) { st = carg(&k, "TraceStart"); en = carg(&k, "TraceEnd"); }
    if (!st || !en) { lt_state = -1; LOG("ragdoll shot: no PrimitiveComponent.K2_LineTraceComponent(TraceStart, TraceEnd): the closest bone only"); return -1; }
    for (int c = 0; c < 3; c++) { st[c] = s->eye[c]; en[c] = s->eye[c] + s->aim[c] * SHOT_RANGE; }
    SET(&k, "bTraceComplex", uint8_t, 0);
    SET(&k, "bShowTrace", uint8_t, 0);
    SET(&k, "bPersistentShowTrace", uint8_t, 0);
    call_go(&k);
    uint8_t *r = carg(&k, "ReturnValue");
    float *hl = carg(&k, "HitLocation");
    FName *bn = carg(&k, "BoneName");
    if (!lt_state) { lt_state = 1; LOG("ragdoll shot: aim traces with K2_LineTraceComponent (HitLocation %s, BoneName %s)", hl ? "yes" : "NO", bn ? "yes" : "NO"); }
    if (!r || !*r || !hl) return 0;
    memcpy(hit, hl, 3 * sizeof(float));
    *bone = bn ? *bn : (FName){0, 0};
    *dist = d3(hit, s->eye);
    return 1;
}

static void shove(UObject *a, int hits, double first, double now, Shooter *sh, int *nsh) {
    char ab[160], hb[160], kb[96], bb[64], pb[64], how[200];
    Tracked *x = dead_tracked(a);
    if (!x || !x->ragdolled || x->ended || !x->mesh) { n_shot_notours++; return; }   // bosses, pops, not seen dying
    if (first < x->died_at + SHOT_AFTER_DEATH) { n_shot_early++; return; }
    if ((x->rc && *((const uint8_t *)x->rc + RC_SETTLED)) || call_bool(x->mesh, "IsSimulatingPhysics") != 1) { n_shot_frozen++; return; }
    const BodyList *bl = x->bodies ? x->bodies : bodies_of(x->mesh);
    float at[3];   // where the body is: its first body (the root), else the actor
    int have = bl && !bone_loc(x->mesh, bl->bone[0], at);
    if (!have && actor_loc(x->a, at)) { n_shot_fail++; return; }
    for (int i = 0; i < 8; i++)
        if (blasts[i].t && fabs(first - blasts[i].t) <= SHOT_BLAST_S && d3(at, blasts[i].at) < BLAST_RANGE) { n_shot_blast++; return; }
    if (*nsh < 0) *nsh = shooters(sh, 8);
    int best = -1, mode = 0;
    float pt[3] = {0}, along = 0, off_cm = 0, off_deg = 0;
    FName bone = {0, 0};
    for (int i = 0; i < *nsh; i++) {   // 1. an aim that runs through the body; the nearest shooter if several
        float h[3], d;
        FName b;
        if (aim_trace(x->mesh, &sh[i], h, &b, &d) == 1 && (best < 0 || d < along)) {
            best = i; mode = 1; along = d; bone = b;
            memcpy(pt, h, sizeof pt);
        }
    }
    if (best < 0 && bl) {   // 2. the aim that passes closest to one of its bones
        float bp[MAX_BODIES][3], ang_best = 1e30f;
        uint32_t ok = 0;
        for (int j = 0; j < bl->n && j < MAX_BODIES; j++)
            if (!((x->hid >> j) & 1) && !bone_loc(x->mesh, bl->bone[j], bp[j])) ok |= 1u << j;   // parts cut off: not aimed at
        for (int i = 0; i < *nsh; i++)
            for (int j = 0; j < bl->n && j < MAX_BODIES; j++) {
                if (!((ok >> j) & 1)) continue;
                float v[3] = {bp[j][0] - sh[i].eye[0], bp[j][1] - sh[i].eye[1], bp[j][2] - sh[i].eye[2]};
                float t = v[0] * sh[i].aim[0] + v[1] * sh[i].aim[1] + v[2] * sh[i].aim[2];
                if (t <= 0.f || t > SHOT_RANGE) continue;
                float q[3] = {v[0] - t * sh[i].aim[0], v[1] - t * sh[i].aim[1], v[2] - t * sh[i].aim[2]};
                float perp = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]), ang = atan2f(perp, t) * DEG;
                if ((perp > SHOT_NEAR && ang > SHOT_CONE) || ang >= ang_best) continue;
                ang_best = ang; best = i; mode = 2; bone = bl->bone[j]; along = t; off_cm = perp; off_deg = ang;
                memcpy(pt, bp[j], sizeof pt);
            }
    }
    if (best < 0) {
        n_shot_noshooter++;
        if (shot_miss_logs < SHOT_MISS_LOGS) {
            char ws[480];
            size_t m = 0;
            ws[0] = 0;
            for (int i = 0; i < *nsh && m + 120 < sizeof ws; i++) {
                float v[3] = {at[0] - sh[i].eye[0], at[1] - sh[i].eye[1], at[2] - sh[i].eye[2]}, d = d3(at, sh[i].eye);
                float c = d > 0 ? (v[0] * sh[i].aim[0] + v[1] * sh[i].aim[1] + v[2] * sh[i].aim[2]) / d : 1;
                m += (size_t)snprintf(ws + m, sizeof ws - m, "%s%s %.0f deg off, %.0f cm", i ? "; " : "", oname(sh[i].h, hb, sizeof hb),
                                      acosf(c > 1 ? 1 : c < -1 ? -1 : c) * DEG, d);
            }
            shot_miss_logs++;
            LOG("ragdoll shot: %s +%.2fs after death: %d hit(s), no hero aims at it (%d heroes: %s): not shoved",
                oname(x->a, ab, sizeof ab), now - x->died_at, hits, *nsh, *nsh ? ws : "none found");
        }
        return;
    }
    PushKind *kind = kind_of(sh[best].item, kb, sizeof kb);   // nothing held: "None", the default strength
    if (kind->radial) kind = push_kind("default", 0);         // a weapon is no blast
    // build 40: the shot opens a wound or takes a part off where it lands (wound kinds), before the push
    int w = wound_apply(x, x->mesh, kind->key, bone, sh[best].eye, 0, "downed");
    float strength = kind->n * shot_scale;
    if (strength <= 0.f) return;
    FName on = body_bone(x->mesh, bl, bone);
    for (int j = 0; w == 2 && bl && j < bl->n && j < 32; j++)   // that part came off (its body simulates on, unseen): push the rest
        if (bl->bone[j].idx == on.idx && bl->bone[j].num == on.num && ((x->hid >> j) & 1)) { on = bl->bone[0]; break; }
    Call k;
    float *ip = NULL, *lp = NULL;
    if (call_prep(&k, x->mesh, "AddImpulseAtLocation")) { ip = carg(&k, "Impulse"); lp = carg(&k, "Location"); }
    if (!ip || !lp) { n_shot_fail++; return; }
    for (int c = 0; c < 3; c++) ip[c] = sh[best].aim[c] * strength;
    memcpy(lp, pt, sizeof pt);
    SET(&k, "BoneName", FName, on);
    call_go(&k);
    call_void(x->mesh, "WakeAllRigidBodies");
    int reheld = x->released && now - x->died_at < SETTLE_CAP;   // at rest, about to be frozen: held again
    if (reheld) { x->released = 0; x->rest = 0; n_shot_reheld++; }
    if (!x->mass) x->mass = body_mass(x->mesh, bl);
    n_shoves++; x->shots++;
    if (mode == 1) n_shove_trace++; else n_shove_near++;
    if (shot_logs < SHOT_LOGS) {
        shot_logs++;
        if (mode == 1) snprintf(how, sizeof how, "its aim runs through %s, %.0f cm away", bone.idx ? ue_name(bone, bb, sizeof bb) : "the body", along);
        else snprintf(how, sizeof how, "its aim passes %.0f cm from %s (%.1f deg off), %.0f cm away", off_cm, ue_name(bone, bb, sizeof bb), off_deg, along);
        LOG("ragdoll shot: %s +%.2fs after death: %d hit(s) this frame; shooter %s holding %s (%s %.0f x%.2f): %s; pushed on %s, "
            "body %.1f kg (~%.0f cm/s for the whole body), shove #%d on it%s", oname(x->a, ab, sizeof ab), now - x->died_at, hits,
            oname(sh[best].h, hb, sizeof hb), kb, kind->key, kind->n, shot_scale, how, on.idx ? ue_name(on, pb, sizeof pb) : "the root body",
            x->mass, x->mass > 0 ? strength / x->mass : -1.f, x->shots, reheld ? "; it was at rest: held again" : "");
    }
}

static void shot_tick(double now) {   // game thread: this frame's hits, one shove per body
    if (!n_shotq) return;
    Shooter sh[8];
    int nsh = -1;   // the heroes, looked at once per tick when a hit needs them
    for (int i = 0; i < n_shotq; i++) {
        UObject *a = shotq[i].a;
        if (!a) continue;
        int hits = 0;
        double first = shotq[i].t;
        for (int j = i; j < n_shotq; j++)
            if (shotq[j].a == a) { hits++; if (shotq[j].t < first) first = shotq[j].t; shotq[j].a = NULL; }
        shove(a, hits, first, now, sh, &nsh);
    }
    n_shotq = 0;
}

// ---- deadline: every ragdoll is gone a fixed time after death, whatever the game does ----
// The 10 s cap above is only looked at inside the game's settle check, so a body whose RagdollComponent never starts
// that timer (seen: Retches, 16 Oct, never removed at any gravity) was held forever, and so was its extra gravity.
// This one runs in our own tick from the moment of death. At the deadline the body is hidden, its physics stopped
// (no more cost, no jitter) and the actor destroyed, as the game's own delete does (AActor::Destroy). A client may
// not be allowed to destroy a replicated actor; it stays hidden until the host's copy goes. Bodies the game handled
// the normal way (swap + delete about 1-2 s after the release) are long gone by then. The log line says what the
// game had done with the body, to find out why it never got there.
#define DEADLINE_DEFAULT 15.f
static float deadline_s = DEADLINE_DEFAULT;   // s after death; above SETTLE_CAP plus the game's swap and 1 s delete
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

// ---- saved settings (build 36): b4bcoop.ini ----
// Joon (17 Oct): everything should work after a restart. Every /ragdoll setting is written to b4bcoop.ini (next to the
// DLL) by the command that changes it, through the agent's own writer (cmds_ini_set: that one key, the file's
// comments and order kept). A value equal to the default is written as a commented-out line, so a default changed in a
// later build still applies. Read at start (ragdoll_init) and when the file is edited while the game runs
// (ragdoll_live, from cmds.c's reload poll). Keys:
//   ragdoll=1                          /ragdoll on: on at every start (the tick turns it on once a world exists)
//   ragdoll_delay, ragdoll_mass, ragdoll_gravity, ragdoll_deadline, ragdoll_push, ragdoll_shot   numbers
//   ragdoll_kinds=SG:30000,Axe:6000    the push table's kinds that differ from the defaults, or were added
//   ragdoll_bloodbag, ragdoll_blood, ragdoll_dismember, ragdoll_pieces   1 / 0 (on / off read too)
//   ragdoll_limbs=2-3, ragdoll_big=75, ragdoll_head=50   as /ragdoll dismember limbs|big|head
//   ragdoll_carry                      build 38: x the running speed (0-2)
//   ragdoll_carry_keep                 build 39: s a remembered running speed takes to fade (0-3)
//   ragdoll_wounds, ragdoll_wounds_kinds|small|heads=K,K, ragdoll_wounds_living=off|arms|all, ragdoll_wounds_blast   build 40
// Not saved: trace, probe, dismember type|impulse (the dismember experiment's test knobs).
static int start_on;     // ragdoll=1 read: the tick turns the ragdoll on, once
static int start_done;   // that happened, or the player turned it on or off himself
static int save_fail;    // a write failed during this command

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int onoff(const char *v, int def) {
    if (!v) return def;
    if (!_stricmp(v, "on") || !_stricmp(v, "true")) return 1;
    if (!_stricmp(v, "off") || !_stricmp(v, "false")) return 0;
    return atoi(v) != 0;
}
// a list of weapon kinds (push table keys): letters, digits and _, comma-separated (spaces dropped); build 40 (wounds)
static void set_kinds(char *dst, size_t size, const char *v) {
    size_t m = 0;
    for (; *v && m + 1 < size; v++) {
        char ch = *v;
        if (isalnum((unsigned char)ch) || ch == '_') dst[m++] = ch;
        else if ((ch == ',' || ch == ' ') && m && dst[m - 1] != ',') dst[m++] = ',';
    }
    while (m && dst[m - 1] == ',') m--;
    dst[m] = 0;
}

static void set_limbs(int lo, int hi) {   // 1-4 each, in order
    lo = clampi(lo, 1, N_LIMBS);
    hi = clampi(hi, 1, N_LIMBS);
    if (hi < lo) { int x = lo; lo = hi; hi = x; }
    dism_min = lo; dism_max = hi;
}

static void save(const char *key, const char *val) {   // val NULL: back to the default (the line commented out)
    if (cmds_ini_set(key, val)) { save_fail = 1; LOG("ragdoll: could not write %s to %s", key, cmds_config_path()); }
}
static void save_f(const char *key, float v, float def) {
    char b[32];
    if (v == def) { save(key, NULL); return; }
    snprintf(b, sizeof b, "%g", v);
    save(key, b);
}
static void save_i(const char *key, int v, int def) {
    char b[16];
    if (v == def) { save(key, NULL); return; }
    snprintf(b, sizeof b, "%d", v);
    save(key, b);
}
// b4bcoop.ini's ragdoll_kinds: the kinds that differ from PUSH_DEF, or aren't in it, as KIND:n pairs (":blast" /
// ":bullet" when a kind's explosion flag differs from its default, build 41); "" = none
static void kinds_format(char *b, size_t size) {
    size_t m = 0;
    b[0] = 0;
    for (int i = 0; i < n_push_tab && m + 48 < size; i++) {
        int same = 0, def_radial = 0;
        for (int j = 0; j < N_PUSH_DEF; j++)
            if (!_stricmp(PUSH_DEF[j].key, push_tab[i].key)) {
                same = PUSH_DEF[j].n == push_tab[i].n && PUSH_DEF[j].radial == push_tab[i].radial;
                def_radial = PUSH_DEF[j].radial;
                break;
            }
        if (!same) m += (size_t)snprintf(b + m, size - m, "%s%s:%.0f%s", m ? "," : "", push_tab[i].key, push_tab[i].n,
                                         push_tab[i].radial == def_radial ? "" : push_tab[i].radial ? ":blast" : ":bullet");
    }
}
// its reader (no strtok: the chat handler uses it); v NULL = the defaults
static void kinds_parse(const char *v) {
    kinds_reset();
    for (const char *p = v ? v : ""; *p; ) {
        char key[24];
        size_t n = 0;
        while (*p == ' ' || *p == ',') p++;
        while (*p && *p != ':' && *p != ',' && *p != ' ' && n + 1 < sizeof key) key[n++] = *p++;
        key[n] = 0;
        while (*p && *p != ':' && *p != ',') p++;   // spaces before ':', or the rest of a name too long (cut to 23
                                                    // letters, as /ragdoll push <kind> cuts it)
        if (*p != ':') continue;
        float x = (float)atof(++p);
        const char *e = p;
        while (*e && *e != ',') e++;
        int rad = -1;   // build 41: "Key:n:blast" / ":bullet"
        for (const char *q = p; q < e; q++)
            if (*q == ':') { rad = !_strnicmp(q + 1, "blast", 5) ? 1 : !_strnicmp(q + 1, "bullet", 6) ? 0 : -1; break; }
        p = e;
        PushKind *pk = n ? push_kind(key, 1) : NULL;
        if (pk) { pk->n = clampf(x, 0.f, 500000.f); if (rad >= 0) pk->radial = (uint8_t)rad; }
    }
}
static void save_kinds(void) {
    char b[600];
    kinds_format(b, sizeof b);
    save("ragdoll_kinds", b[0] ? b : NULL);
}

// one b4bcoop.ini key into the settings (val NULL = removed: the default); 1 = a key of ours
static int setting(const char *k, const char *v) {
    if (!strcmp(k, "ragdoll")) start_on = onoff(v, 0);
    else if (!strcmp(k, "ragdoll_delay")) delay_s = v ? clampf((float)atof(v), 0.f, 10.f) : 0.f;
    else if (!strcmp(k, "ragdoll_mass")) mass_scale = v ? clampf((float)atof(v), 0.1f, 10.f) : 1.f;
    else if (!strcmp(k, "ragdoll_gravity")) grav_scale = v ? clampf((float)atof(v), 0.f, 5.f) : GRAV_DEFAULT;
    else if (!strcmp(k, "ragdoll_deadline")) deadline_s = v ? clampf((float)atof(v), 12.f, 120.f) : DEADLINE_DEFAULT;
    else if (!strcmp(k, "ragdoll_push")) push_scale = v ? clampf((float)atof(v), 0.f, 10.f) : PUSH_DEFAULT;
    else if (!strcmp(k, "ragdoll_shot")) shot_scale = v ? clampf((float)atof(v), 0.f, 10.f) : SHOT_DEFAULT;
    else if (!strcmp(k, "ragdoll_kinds")) kinds_parse(v);   // KIND:n[:blast|:bullet] pairs
    else if (!strcmp(k, "ragdoll_bloodbag")) bloodbag_on = onoff(v, 0);
    else if (!strcmp(k, "ragdoll_blood")) blood_on = onoff(v, 1);
    else if (!strcmp(k, "ragdoll_dismember")) dism_on = onoff(v, DISM_DEFAULT);
    else if (!strcmp(k, "ragdoll_pieces")) dism_pieces = onoff(v, 1);
    else if (!strcmp(k, "ragdoll_limbs")) {   // "2-4", or one number
        const char *d = v ? strchr(v, '-') : NULL;
        if (v) set_limbs(atoi(v), d ? atoi(d + 1) : atoi(v)); else set_limbs(DISM_MIN_DEF, DISM_MAX_DEF);
    }
    else if (!strcmp(k, "ragdoll_big")) dism_big = v ? clampi(atoi(v), 0, 100) : DISM_BIG_DEF;
    else if (!strcmp(k, "ragdoll_head")) dism_head = v ? clampi(atoi(v), 0, 100) : DISM_HEAD_DEF;
    else if (!strcmp(k, "ragdoll_carry")) carry_scale = v ? clampf((float)atof(v), 0.f, 2.f) : CARRY_DEFAULT;
    else if (!strcmp(k, "ragdoll_carry_keep")) carry_keep = v ? clampf((float)atof(v), 0.f, 3.f) : CARRY_KEEP_DEF;
    else if (!strcmp(k, "ragdoll_wounds")) wounds_on = onoff(v, 1);
    else if (!strcmp(k, "ragdoll_wounds_kinds")) set_kinds(wound_kinds, sizeof wound_kinds, v ? v : WOUND_KINDS_DEF);
    else if (!strcmp(k, "ragdoll_wounds_small")) set_kinds(wound_small, sizeof wound_small, v ? v : WOUND_SMALL_DEF);
    else if (!strcmp(k, "ragdoll_wounds_heads")) set_kinds(wound_heads, sizeof wound_heads, v ? v : WOUND_HEADS_DEF);
    else if (!strcmp(k, "ragdoll_wounds_living")) wound_living = !v ? WOUND_LIVING_DEF : !_stricmp(v, "off") ? 0 : !_stricmp(v, "arms") ? 1 :
                                                  !_stricmp(v, "all") ? 2 : clampi(atoi(v), 0, 2);
    else if (!strcmp(k, "ragdoll_wounds_blast")) wound_blast = onoff(v, 1);
    else if (!strcmp(k, "ragdoll_wounds_game")) wound_game = onoff(v, 0);                                    // build 41
    else if (!strcmp(k, "ragdoll_wounds_none")) set_kinds(wound_none, sizeof wound_none, v ? v : WOUND_NONE_DEF);   // build 41
    else return 0;
    return 1;
}

static void turn_on(float delay) {   // game thread: /ragdoll on, or ragdoll=1 at start
    delay_s = clampf(delay, 0.f, 10.f);
    mode_on = 1;
    start_done = 1;
    detail_left = DETAIL_LOGS;
    settle_logs = 0;
    push_logs = hit_dumps = left_logs = bag_logs = 0;
    shot_logs = shot_miss_logs = shot_dumps = 0;
    carry_logs = vel_logs_r = 0;   // build 38
    wound_logs = wound_nomap_logs = 0;   // build 40
    cap_logs = ctl_logs = game_mut_logs = pick_logs = 0;   // build 41, 42
    if (!nm_mutsnd.idx) nm_mutsnd = fname_of(L"HandleMutilationSounds");
    if (!game_mut_stacks) InterlockedExchange(&stack_budget[ST_MUTIL], 3);
    if (shot_state < 0) shot_state = 0;   // look OnCorpseHit up again
    shot_reset();
    game_tid = GetCurrentThreadId();
    settle_arm();
    bag_arm();
    mutil_arm();   // build 42: the game's own wounds, hit by hit
    pe_update();
    LOG("ragdoll: on, delay %.2f s", delay_s);
}
static void turn_off(void) {
    mode_on = 0;
    start_done = 1;
    pe_update();
    LOG("ragdoll: off (the game's own wounds back on %d Ridden)", caps_restore_all());
}

// b4bcoop.ini edited while the game runs (cmds.c's reload poll, game thread): 1 = our key, applied
int ragdoll_live(const char *key, const char *val) {
    if (strncmp(key, "ragdoll", 7) || !setting(key, val)) return 0;
    if (!strcmp(key, "ragdoll")) { if (start_on && !mode_on) turn_on(delay_s); else if (!start_on && mode_on) turn_off(); }
    return 1;
}

void ragdoll_tick(float dt) {
    (void)dt;
    tick_n++;
    game_tid = GetCurrentThreadId();   // the tick runs on the game thread (shot_note checks it)
    if (start_on && !start_done && !mode_on && ue_world()) {   // build 36: b4bcoop.ini ragdoll=1
        turn_on(delay_s);
        LOG("ragdoll: on at start (b4bcoop.ini ragdoll=1)");
    }
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
            if (x->ragdolled && (x->push_dv > 0.f || x->carry > 0.f) && !(x->velp & 0x80) && !x->ended) vel_probe_tick(x, t);
            if (x->ragdolled && x->limbp == 1 && !x->ended) limb_probe_tick(x, t);
            if (x->ragdolled && !x->ended && t - x->died_at >= deadline_s) { end_body(x, t - x->died_at); continue; }
            if (x->ragdolled && !x->released && x->mesh && grav_scale != 1.f && !n_grav_fail) extra_gravity(x);
            if (x->ragdolled && (tick_n % 15) == 0 && call_bool(x->ls, "IsAlive") == 1) undo(x);
            continue;
        }
        if (!x->died_at) {
            if (call_bool(x->ls, "IsAlive") == 1) { run_note(x, t); continue; }   // build 38: its running speed
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
    shot_tick(t);   // build 35: after the deaths of this tick are ragdolled
    wound_tick();   // build 40: hits on living Ridden (host)
}

void ragdoll_slash(char *rest, Out *o) {
    save_fail = 0;
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
        out_printf(o, "/ragdoll shots on bodies x%.2f%s: %d shoves from %d hits (%d by aim trace, %d by the closest bone; %d held "
                      "again), %d with no shooter found, %d with the killing hit, %d already frozen, %d near an explosion, %d not "
                      "ours, %d failed, %d dropped\n", shot_scale, shot_state < 0 ? " (NOT AVAILABLE: see log)" : "", n_shoves,
                   n_shot_hits, n_shove_trace, n_shove_near, n_shot_reheld, n_shot_noshooter, n_shot_early, n_shot_frozen,
                   n_shot_blast, n_shot_notours, n_shot_fail, n_shot_dropped);
        out_printf(o, "/ragdoll carry x%.2f keep %.2f s%s: %d died running and kept it (%d from before a stagger), %d already faster, "
                      "%d standing still, %d with no speed read, %d failed\n", carry_scale,
                   carry_keep, mov_vel_off == -1 ? " (NOT AVAILABLE: see log)" : "", n_carried, n_carry_mem, n_carry_faster,
                   n_carry_still, n_carry_nomov, n_carry_fail);
        out_printf(o, "/ragdoll wounds %s (%s; small %s; heads only %s; none %s; living %s; blasts %s): %d hits looked at, %d parts off, %d open "
                      "wounds (+%d on blast victims), %d refused, %d already done, %d left alone on living Ridden, %d with no wound "
                      "for the bone; %d living hits (%d dropped, %d OnDamage events known%s); our pieces %d; the game's own open wounds %s: "
                      "%d Commons capped before their first hit, %d at a hit, %d failed; the game's wound pick%s: %d skipped (no-wound kinds), "
                      "%d without open wounds, %d the game's own (other kinds), %d with the damage type unread; %d Commons killed by "
                      "those kinds, %d still with the game's own; %d of the game's own mutilations seen\n",
                   wounds_on ? "on" : "off", wound_kinds, wound_small, wound_heads, wound_none, LIVING_NAME[wound_living], wound_blast ? "on" : "off",
                   n_wound_hits, n_wound_off, n_wound_rupt, n_blast_rupt, n_wound_refused, n_wound_already, n_wound_skip, n_wound_nomap,
                   n_living_hits, n_woundq_drop, n_dmg_fns, dmgi.state < 0 ? ", NOT AVAILABLE: see log" : "", n_wound_pieces,
                   wound_game ? "on" : cap_state < 0 ? "off but NOT AVAILABLE" : "off", n_cap_scan, n_cap_hits, n_cap_fail,
                   mutil_state == 1 ? "" : mutil_state ? " NOT HOOKED (see log)" : " not hooked yet", n_pick_skip, n_pick_ours,
                   n_pick_game, n_pick_unknown, n_ctl_kills,
                   n_ctl_game, n_game_mut);
    } else if (!strcmp(verb, "on")) {
        turn_on(arg ? (float)atof(arg) : 0.f);
        start_on = 1;
        save("ragdoll", "1");
        save_f("ragdoll_delay", delay_s, 0.f);
        out_printf(o, "/ragdoll on: Ridden that die from now on fall as ragdolls%s and stay until they lie still (this machine "
                      "only); on at every game start until /ragdoll off%s\n",
                   delay_s > 0 ? " after the game's death animation starts" : "",
                   settle_state == 1 ? "" : "; settle hold OFF: they freeze at the game's time (see log)");
    } else if (!strcmp(verb, "off")) {
        turn_off();
        start_on = 0;
        save("ragdoll", NULL);
        out_printf(o, "/ragdoll off (also at the next game start)\n");
    } else if (!strcmp(verb, "mass")) {
        if (arg) {
            float m = (float)atof(arg);
            mass_scale = m < 0.1f ? 0.1f : m > 10.f ? 10.f : m;
            mass_logs = 0;
            save_f("ragdoll_mass", mass_scale, 1.f);
            LOG("ragdoll tune: mass x%.2f", mass_scale);
        }
        out_printf(o, "/ragdoll mass: x%.2f (0.1-10, default 1) for bodies that die from now on; under gravity alone "
                      "weight changes nothing, it sets how far a push moves the body\n", mass_scale);
    } else if (!strcmp(verb, "gravity")) {
        if (arg) {
            float g = (float)atof(arg);
            grav_scale = g < 0.f ? 0.f : g > 5.f ? 5.f : g;
            LOG("ragdoll tune: gravity x%.2f", grav_scale);
            save_f("ragdoll_gravity", grav_scale, GRAV_DEFAULT);
        }
        out_printf(o, "/ragdoll gravity: x%.2f (0-5, default %.1f) on ragdolls still falling, from now on%s\n", grav_scale, GRAV_DEFAULT,
                   n_grav_fail ? "; NO EFFECT in this game (see log)" : "");
    } else if (!strcmp(verb, "deadline")) {
        if (arg) {
            float s = (float)atof(arg);
            deadline_s = s < 12.f ? 12.f : s > 120.f ? 120.f : s;
            deadline_logs = 0;
            save_f("ragdoll_deadline", deadline_s, DEADLINE_DEFAULT);
            LOG("ragdoll deadline: %.0f s", deadline_s);
        }
        out_printf(o, "/ragdoll deadline: every ragdoll still there %.0f s after death is removed (12-120, default 15)\n",
                   deadline_s);
    } else if (!strcmp(verb, "dismember")) {
        char *arg2 = arg ? strtok(NULL, " ") : NULL, *arg3 = arg2 ? strtok(NULL, " ") : NULL;
        if (arg && (!strcmp(arg, "on") || !strcmp(arg, "off"))) {
            dism_on = !strcmp(arg, "on");
            dism_logs = dism_traced = dism_built_logs = vel_logs_d = vel_logs_c = dism_probed = head_logs = piece_fail_logs = 0;
            save_i("ragdoll_dismember", dism_on, DISM_DEFAULT);
        }
        else if (arg && !strcmp(arg, "limbs") && arg2 && isdigit((unsigned char)arg2[0])) {   // limbs <min> [max], 1-4
            set_limbs(atoi(arg2), arg3 ? atoi(arg3) : atoi(arg2));
            char b[16];
            snprintf(b, sizeof b, "%d-%d", dism_min, dism_max);
            save("ragdoll_limbs", dism_min == DISM_MIN_DEF && dism_max == DISM_MAX_DEF ? NULL : b);
        }
        else if (arg && !strcmp(arg, "pieces") && arg2 && (!strcmp(arg2, "on") || !strcmp(arg2, "off"))) {
            dism_pieces = !strcmp(arg2, "on");
            save_i("ragdoll_pieces", dism_pieces, 1);
        }
        else if (arg && !strcmp(arg, "big") && arg2 && isdigit((unsigned char)arg2[0])) {   // % of limbs cut big first
            dism_big = clampi(atoi(arg2), 0, 100);
            save_i("ragdoll_big", dism_big, DISM_BIG_DEF);
        }
        else if (arg && !strcmp(arg, "head") && arg2 && (isdigit((unsigned char)arg2[0]) || !strcmp(arg2, "on") || !strcmp(arg2, "off"))) {
            int v = !strcmp(arg2, "on") ? 50 : !strcmp(arg2, "off") ? 0 : atoi(arg2);   // % of victims
            dism_head = v < 0 ? 0 : v > 100 ? 100 : v;
            head_logs = 0;
            save_i("ragdoll_head", dism_head, DISM_HEAD_DEF);
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
        out_printf(o, "/ragdoll dismember: %s (default on): blast victims lose %d-%d limbs (limbs <min> [max], 1-4; one arm at "
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
            save_i("ragdoll_blood", blood_on, 1);
            LOG("ragdoll blood: %s", blood_on ? "on" : "off");
        } else if (arg) { out_printf(o, "usage: /ragdoll blood [on|off]\n"); return; }
        out_printf(o, "/ragdoll blood: %s (default on): bodies spared from the blood bag get the blood the game paints on the "
                      "blast victims it keeps%s; so far %d bodies painted in %d explosions\n", blood_on ? "on" : "off",
                   paint_state < 0 ? "; NOT AVAILABLE in this game (see log)" : "", n_painted, n_paint_explosions);
    } else if (!strcmp(verb, "bloodbag")) {
        if (arg && (!strcmp(arg, "on") || !strcmp(arg, "off"))) {
            bloodbag_on = !strcmp(arg, "on");
            pe_update();
            save_i("ragdoll_bloodbag", bloodbag_on, 0);
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
            save_f("ragdoll_push", push_scale, PUSH_DEFAULT);
        } else if (arg && !strcmp(arg, "off")) {
            push_scale = 0.f;
            LOG("ragdoll push: off");
            save_f("ragdoll_push", push_scale, PUSH_DEFAULT);
        } else if (arg && !strcmp(arg, "on")) {
            push_scale = PUSH_DEFAULT;
            LOG("ragdoll push: on (x%.2f, the default)", push_scale);
            save_f("ragdoll_push", push_scale, PUSH_DEFAULT);
        } else if (arg && arg2) {   // /ragdoll push <key> <n> [blast|bullet]: one weapon kind
            float n = (float)atof(arg2);
            char *arg3 = strtok(NULL, " ");
            PushKind *k = push_kind(arg, 1);
            if (!k) { out_printf(o, "/ragdoll push: table full\n"); return; }
            k->n = n < 0.f ? 0.f : n > 500000.f ? 500000.f : n;
            if (arg3 && !_stricmp(arg3, "blast")) k->radial = 1;        // build 41: an explosion's kind (pushed from its
            else if (arg3 && !_stricmp(arg3, "bullet")) k->radial = 0;  // centre, dismembered)
            LOG("ragdoll push: %s = %.0f%s", k->key, k->n, k->radial ? " (explosion)" : "");
            save_kinds();
        } else if (arg) {
            out_printf(o, "usage: /ragdoll push [<x> | on | off | <weapon kind> <strength> [blast|bullet]]\n");
            return;
        }
        char line[1024];
        size_t m = 0;
        for (int i = 0; i < n_push_tab && m + 40 < sizeof line; i++)
            m += (size_t)snprintf(line + m, sizeof line - m, "%s%s %.0f%s", i ? ", " : "", push_tab[i].key, push_tab[i].n,
                                  push_tab[i].radial ? " (blast)" : "");
        out_printf(o, "/ragdoll push: x%.2f (0-10, default %.0f, 0 = off) on bodies that die from now on; kg cm/s per weapon "
                      "kind: %s\n", push_scale, PUSH_DEFAULT, line);
    } else if (!strcmp(verb, "shot")) {   // build 35: shots on bodies that are still ragdolls
        if (arg && (isdigit((unsigned char)arg[0]) || arg[0] == '.')) {
            float x = (float)atof(arg);
            shot_scale = x < 0.f ? 0.f : x > 10.f ? 10.f : x;
        } else if (arg && !strcmp(arg, "off")) shot_scale = 0.f;
        else if (arg && !strcmp(arg, "on")) shot_scale = SHOT_DEFAULT;
        else if (arg) { out_printf(o, "usage: /ragdoll shot [<x> | on | off]\n"); return; }
        if (arg) { shot_logs = shot_miss_logs = 0; LOG("ragdoll shot: x%.2f", shot_scale); save_f("ragdoll_shot", shot_scale, SHOT_DEFAULT); }
        PushKind *hg = push_kind("HG", 0), *ar = push_kind("AR", 0), *sg = push_kind("SG", 0);
        const float common = 133.f;   // kg, a Common by GetBoneMass (build 18 log)
        out_printf(o, "/ragdoll shot: x%.2f (0-10, default %.0f, 0 = off): a shot on a body that is still a ragdoll shoves it "
                      "with the shooter's weapon strength from /ragdoll push's table x this; per shot a Common gets about %.0f "
                      "cm/s from a pistol, %.0f from a rifle, %.0f from a shotgun%s; so far %d shoves\n", shot_scale, SHOT_DEFAULT,
                   hg ? hg->n * shot_scale / common : 0.f, ar ? ar->n * shot_scale / common : 0.f, sg ? sg->n * shot_scale / common : 0.f,
                   shot_state < 0 ? "; NOT AVAILABLE in this game (see log)" : "", n_shoves);
    } else if (!strcmp(verb, "carry")) {   // build 38: running speed kept; build 39: keep <s>
        char *arg2 = arg ? strtok(NULL, " ") : NULL;
        if (arg && !strcmp(arg, "keep") && arg2 && (isdigit((unsigned char)arg2[0]) || arg2[0] == '.')) {
            carry_keep = clampf((float)atof(arg2), 0.f, 3.f);
            save_f("ragdoll_carry_keep", carry_keep, CARRY_KEEP_DEF);
        }
        else if (arg && (isdigit((unsigned char)arg[0]) || arg[0] == '.')) { carry_scale = clampf((float)atof(arg), 0.f, 2.f); save_f("ragdoll_carry", carry_scale, CARRY_DEFAULT); }
        else if (arg && !strcmp(arg, "off")) { carry_scale = 0.f; save_f("ragdoll_carry", carry_scale, CARRY_DEFAULT); }
        else if (arg && !strcmp(arg, "on")) { carry_scale = CARRY_DEFAULT; save_f("ragdoll_carry", carry_scale, CARRY_DEFAULT); }
        else if (arg) { out_printf(o, "usage: /ragdoll carry [<x> | on | off | keep <s>]\n"); return; }
        if (arg) { carry_logs = vel_logs_r = 0; LOG("ragdoll carry: x%.2f, keep %.2f s", carry_scale, carry_keep); }
        out_printf(o, "/ragdoll carry: x%.2f (0-2, default %.0f, 0 = off): a Ridden that dies running keeps that speed x this "
                      "(%.0f cm/s at most); keep %.2f s (0-3, default %.0f): a stagger before the kill keeps the run's speed, "
                      "fading to nothing over this time; so far %d did (%d from before a stagger), %d were already faster%s\n",
                   carry_scale, CARRY_DEFAULT, CARRY_MAX, carry_keep, CARRY_KEEP_DEF, n_carried, n_carry_mem, n_carry_faster,
                   mov_vel_off == -1 ? "; NOT AVAILABLE in this game (see log)" : "");
    } else if (!strcmp(verb, "wounds")) {   // build 40: wounds by weapon
        char *arg2 = arg ? strtok(NULL, " ") : NULL;
        if (arg && (!strcmp(arg, "on") || !strcmp(arg, "off"))) {
            wounds_on = !strcmp(arg, "on");
            save_i("ragdoll_wounds", wounds_on, 1);
            if (!wounds_on) LOG("ragdoll wounds: off: the game's own wounds back on %d Ridden", caps_restore_all());
            else LOG("ragdoll wounds: on: the game's own open wounds off on %d living Commons", caps_living_all());
            pe_update();
        }
        else if (arg && !strcmp(arg, "kinds") && arg2) { set_kinds(wound_kinds, sizeof wound_kinds, arg2); save("ragdoll_wounds_kinds", strcmp(wound_kinds, WOUND_KINDS_DEF) ? wound_kinds : NULL); }
        else if (arg && !strcmp(arg, "small") && arg2) { set_kinds(wound_small, sizeof wound_small, arg2); save("ragdoll_wounds_small", strcmp(wound_small, WOUND_SMALL_DEF) ? wound_small : NULL); }
        else if (arg && !strcmp(arg, "heads") && arg2) { set_kinds(wound_heads, sizeof wound_heads, arg2); save("ragdoll_wounds_heads", strcmp(wound_heads, WOUND_HEADS_DEF) ? wound_heads : NULL); }
        else if (arg && !strcmp(arg, "living") && arg2 && (!strcmp(arg2, "off") || !strcmp(arg2, "arms") || !strcmp(arg2, "all"))) {
            wound_living = !strcmp(arg2, "off") ? 0 : !strcmp(arg2, "arms") ? 1 : 2;
            save("ragdoll_wounds_living", wound_living == WOUND_LIVING_DEF ? NULL : LIVING_NAME[wound_living]);
            pe_update();
        }
        else if (arg && !strcmp(arg, "blast") && arg2 && (!strcmp(arg2, "on") || !strcmp(arg2, "off"))) { wound_blast = !strcmp(arg2, "on"); save_i("ragdoll_wounds_blast", wound_blast, 1); }
        else if (arg && !strcmp(arg, "game") && arg2 && (!strcmp(arg2, "on") || !strcmp(arg2, "off"))) {   // build 41
            wound_game = !strcmp(arg2, "on");
            save_i("ragdoll_wounds_game", wound_game, 0);
            if (wound_game) LOG("ragdoll wounds: the game's own wounds back on %d Ridden", caps_restore_all());
            else LOG("ragdoll wounds: the game's own open wounds off on %d living Commons", caps_living_all());
            pe_update();
        }
        else if (arg && !strcmp(arg, "none") && arg2) { set_kinds(wound_none, sizeof wound_none, arg2); save("ragdoll_wounds_none", strcmp(wound_none, WOUND_NONE_DEF) ? wound_none : NULL); }
        else if (arg) {
            out_printf(o, "usage: /ragdoll wounds [on|off|kinds <K,K>|small <K,K>|heads <K,K>|none <K,K>|living off|arms|all|blast on|off|game on|off]\n");
            return;
        }
        if (arg) {
            wound_logs = wound_nomap_logs = cap_logs = ctl_logs = 0;
            LOG("ragdoll wounds: %s, kinds %s, small %s, heads only %s, none %s, living %s, blasts %s, the game's own %s", wounds_on ? "on" : "off",
                wound_kinds, wound_small, wound_heads, wound_none, LIVING_NAME[wound_living], wound_blast ? "on" : "off",
                wound_game ? "on" : "off");
        }
        out_printf(o, "/ragdoll wounds: %s (default on): shots by %s take off or open what they hit (head, arm, leg, chest, "
                      "stomach); small wounds for %s; %s only take heads (a killing headshot); %s get none; on living Ridden: %s "
                      "(off | arms: arms and open wounds | all: legs too; never heads); blast victims get 1-2 open wounds: %s; the "
                      "game's own open wounds on Commons: %s (game on|off, default off; host; its head pop and limbs stay). Lists: kinds|small|heads|"
                      "none <K,K> (none = empty); so far %d parts off, %d wounds opened\n", wounds_on ? "on" : "off", wound_kinds,
                   wound_small, wound_heads, wound_none, LIVING_NAME[wound_living], wound_blast ? "on" : "off",
                   wound_game ? "on" : cap_state < 0 ? "off but NOT AVAILABLE (see log)" : "off", n_wound_off, n_wound_rupt + n_blast_rupt);
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
        out_printf(o, "usage: /ragdoll [on [delay]|off|status|mass [x]|gravity [x]|deadline [s]|push [x|on|off|<kind> <n> [blast]]|shot [x|on|off]|carry [x|on|off|keep <s>]|wounds [on|off|kinds <K,K>|small <K,K>|heads <K,K>|none <K,K>|living off|arms|all|blast on|off|game on|off]|bloodbag [on|off]|blood [on|off]|dismember [on|off|limbs <min> [max]|big <0-100>|head <0-100|on|off>|pieces on|off|type <n|auto>|impulse <n>]|probe|trace [secs]]\n");
    }
    if (save_fail) out_printf(o, "/ragdoll: could not write b4bcoop.ini (see log): this change lasts until the game closes\n");
}

static void ini_pair(const char *k, const char *v, void *ctx) { (void)ctx; if (!strncmp(k, "ragdoll", 7)) setting(k, v); }

int ragdoll_init(void) {
    kinds_reset();
    HANDLE wd = CreateThread(NULL, 0, freeze_watch, NULL, 0, NULL);   // build 43: logs the game thread's stack if it stops
    if (wd) CloseHandle(wd); else LOG("ragdoll freeze: the watchdog thread could not start (%lu)", GetLastError());
    cmds_ini_each(ini_pair, NULL);   // b4bcoop.ini (build 36)
    char kinds[600];
    size_t m = 0;
    kinds[0] = 0;
    for (int i = 0; i < n_push_tab && m + 40 < sizeof kinds; i++)
        m += (size_t)snprintf(kinds + m, sizeof kinds - m, "%s%s %.0f", i ? ", " : "", push_tab[i].key, push_tab[i].n);
    LOG("ragdoll: experimental module loaded (fork build), %s; from b4bcoop.ini and the defaults: delay %.2f, mass x%.2f, "
        "gravity x%.2f, deadline %.0f s, push x%.2f, shots on bodies x%.2f, blood bags %s, blood %s, dismember %s (%d-%d "
        "limbs, big %d%%, head %d%%, pieces %s), carry x%.2f keep %.2f s, wounds %s (%s; small %s; heads only %s; none %s; living %s; "
        "blasts %s; the game's own %s); kinds: %s",
        start_on ? "on at the first world (ragdoll=1)" : "off until /ragdoll on",
        delay_s, mass_scale, grav_scale, deadline_s, push_scale, shot_scale, bloodbag_on ? "on" : "off", blood_on ? "on" : "off",
        dism_on ? "on" : "off", dism_min, dism_max, dism_big, dism_head, dism_pieces ? "on" : "off", carry_scale, carry_keep,
        wounds_on ? "on" : "off", wound_kinds, wound_small, wound_heads, wound_none, LIVING_NAME[wound_living], wound_blast ? "on" : "off",
        wound_game ? "on" : "off", kinds);
    return 0;
}
