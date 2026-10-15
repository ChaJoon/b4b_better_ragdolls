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
//   /ragdoll probe           the nearest living Ridden: its mesh, physics asset, collision, and the death-related
//                            functions/properties of its classes, into the log (discovery)
//   /ragdoll trace [secs]    log the game's function calls on Ridden (default 30 s): every new class.function once,
//                            plus each death's calls just before and after it (discovery: how the game kills a Ridden)
//
// Detection: an incremental scan of the object array finds living Ridden (GobiCharacters that aren't heroes; see
// cheats.c cmd_killall), then their LifeStateComponent's IsAlive is polled each tick. Bosses are left alone.
// Ragdoll: capsule collision off; mesh collision profile "Ragdoll", all bodies simulating physics (which also detaches
// the mesh from the capsule), bodies woken. The first few are logged before/after in detail.
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
static void call_go(Call *c) { ue_process_event(c->obj, c->fn, c->p); }
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
    FName prof;
    float rel_loc[3], rel_rot[3];
} Tracked;
static Tracked tr[MAX_TRACKED];
static int ntr, scan_at, n_overflow, n_revived;
static int mode_on;
static float delay_s;
static int n_applied, n_failed, n_deaths, detail_left;

static int world_check(void) {   // new map: every old pointer is gone
    static UObject *last_w;
    UObject *w = ue_world();
    if (w != last_w) { last_w = w; ntr = 0; scan_at = 0; }
    return w != NULL;
}

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
typedef struct { UObject *actor; double until; int lines; } Focus;
static Focus focus[8];

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

static void trace_call(UObject *o, UFunction *fn) {
    UObject *actor = owner_actor(o);
    if (!actor || !is_ridden_fast(actor)) return;
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
        if (f->actor != actor || t > f->until || f->lines >= 300) continue;
        ue_obj_name(fn, fb, sizeof fb);
        if (per_frame(fb)) break;
        f->lines++;
        LOG("ragdoll death %s +%.3fs %s.%s", ue_obj_name(actor, ab, sizeof ab), t - (f->until - 4.0),
            ue_obj_name(oc, cb, sizeof cb), fb);
        break;
    }
    LeaveCriticalSection(&tcs);
}

static void pe_detour(UObject *o, UFunction *fn, void *params) {
    if (tracing && o && fn) trace_call(o, fn);
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
    focus[slot] = (Focus){actor, t + 4.0, 0};
    LOG("ragdoll death %s: calls in the next 4 s follow (\"+s\" after we saw it)", ab);
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
    if (MH_EnableHook((void *)ADDR_PROCESSEVENT) != MH_OK) { out_printf(o, "/ragdoll trace: hook failed\n"); return -1; }
    InterlockedExchange(&tracing, 1);
    LOG("ragdoll trace: started for %.0f s (Ridden tracked: %d)", secs, ntr);
    return 0;
}

static void trace_stop(const char *why) {
    if (!tracing) return;
    InterlockedExchange(&tracing, 0);
    MH_DisableHook((void *)ADDR_PROCESSEVENT);
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

// ---- the ragdoll ----
static int apply(Tracked *t) {
    char a[160], c[160];
    UObject *mesh = ue_get_ptr(t->a, "Mesh"), *cap = ue_get_ptr(t->a, "CapsuleComponent");
    if (!mesh) { LOG("ragdoll: %s has no mesh", oname(t->a, a, sizeof a)); return -1; }
    int detail = detail_left > 0;
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
    if (cap && call_prep(&k, cap, "SetCollisionEnabled")) { SET(&k, "NewType", uint8_t, 0); call_go(&k); }   // NoCollision
    static FName ragdoll_profile;
    if (!ragdoll_profile.idx) ragdoll_profile = fname_of(L"Ragdoll");
    if (call_prep(&k, mesh, "SetCollisionProfileName")) { SET(&k, "InCollisionProfileName", FName, ragdoll_profile); call_go(&k); }
    if (call_prep(&k, mesh, "SetAllBodiesSimulatePhysics")) { SET(&k, "bNewSimulate", uint8_t, 1); call_go(&k); }
    if (call_prep(&k, mesh, "SetSimulatePhysics")) { SET(&k, "bSimulate", uint8_t, 1); call_go(&k); }
    if (call_prep(&k, mesh, "SetPhysicsBlendWeight")) { SET(&k, "PhysicsBlendWeight", float, 1.f); call_go(&k); }
    call_void(mesh, "WakeAllRigidBodies");

    int sim = call_bool(mesh, "IsSimulatingPhysics");
    if (detail) {
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
    n_revived++;
    LOG("ragdoll: WARNING %s came back alive after its ragdoll (the game reuses Ridden actors): physics undone", oname(t->a, a, sizeof a));
    t->ragdolled = 0; t->done = 0; t->died_at = 0;
}

void ragdoll_tick(float dt) {
    (void)dt;
    tick_n++;
    if (!mode_on && !tracing && !any_ragdolled()) return;
    if (tracing && now_s() > trace_end) trace_stop("time up");
    if (!world_check()) return;
    scan_step();
    double t = now_s();
    for (int i = 0; i < ntr; i++) {
        Tracked *x = &tr[i];
        if (!ue_object_usable(x->a, x->ai) || !ue_object_usable(x->ls, x->lsi)) { tr[i--] = tr[--ntr]; continue; }
        if (x->done) {
            if (x->ragdolled && (tick_n % 15) == 0 && call_bool(x->ls, "IsAlive") == 1) undo(x);
            continue;
        }
        if (!x->died_at) {
            if (call_bool(x->ls, "IsAlive") == 1) continue;
            x->died_at = t;
            n_deaths++;
            if (tracing) trace_on_death(x->a);
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
        out_printf(o, "/ragdoll: %s%s, %d Ridden tracked, %d deaths seen, %d ragdolled, %d failed, %d came back%s%s\n",
                   mode_on ? "on" : "off", mode_on && delay_s > 0 ? " (after the game's animation)" : "", ntr, n_deaths,
                   n_applied, n_failed, n_revived, tracing ? ", tracing" : "", n_overflow ? ", tracking list full" : "");
    } else if (!strcmp(verb, "on")) {
        float d = arg ? (float)atof(arg) : 0.f;
        delay_s = d < 0 ? 0 : d > 10 ? 10 : d;
        mode_on = 1;
        detail_left = DETAIL_LOGS;
        LOG("ragdoll: on, delay %.2f s", delay_s);
        out_printf(o, "/ragdoll on: Ridden that die from now on fall as ragdolls%s (this machine only)\n",
                   delay_s > 0 ? " after the game's death animation starts" : "");
    } else if (!strcmp(verb, "off")) {
        mode_on = 0;
        LOG("ragdoll: off");
        out_printf(o, "/ragdoll off\n");
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
        out_printf(o, "usage: /ragdoll [on [delay]|off|status|probe|trace [secs]]\n");
    }
}

int ragdoll_init(void) {
    LOG("ragdoll: experimental module loaded (fork build), off until /ragdoll on");
    return 0;
}
