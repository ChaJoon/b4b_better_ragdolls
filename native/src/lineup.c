// Character lineup with more than 4 heroes (issue #8, teamsize=5). The pre-round lock-in, post-round and
// character-select lineups don't show the real pawns: ACharacterLineupLayoutManager (placed in the lineup sublevel,
// GobiWorldSettings.CharacterLineupLevel) holds a fixed, level-placed set of CustomizationMannequin actors
// (Mannequins) and ATargetPoints (PreRoundLockInTargetPoints, PostRoundTargetPoints). SetLayoutType (0x141CBD120)
// shows hero-team slot i on Mannequins[i] at TargetPoints[i] and skips i >= Mannequins.Num, so a 5th hero is
// simply missing. This runs locally on every machine (UI-driven, the lineup level is streamed in per viewer).
//
// Fix, only when the hero team has more slots than there are mannequins: before SetLayoutType runs, spawn copies of
// the last mannequin (same class, owned by the manager so they live and die with the lineup sublevel) and append them
// to Mannequins, so the game dresses and shows them itself; after it, place the heroes whose index has no target
// point (slot_xform: back row). A 4-hero team never gets here. Kill switch: env B4BCOOP_NO_LINEUP=1.
// Commands: `lineup` (dump managers, mannequins, target points, camera), `lineup off <dx> <dy>` (placement of the
// 5th), `lineup fov <deg>` (0 = leave the camera alone), `lineup apply` (re-place a lineup that is showing).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MinHook.h"
#include "ue.h"
#include "log.h"
#include "cmds.h"

#define ADDR_SETLAYOUT VA(0x141CBD120ull)   // void ACharacterLineupLayoutManager::SetLayoutType(this, ELayoutType)
static const uint8_t SIG_SETLAYOUT[] = {0x48,0x89,0x5c,0x24,0x18,0x57,0x48,0x83,0xec,0x30,0x0f,0xb6,0x81,0xa4,0x02,0x00,
                                        0x00,0x48,0x8b,0xd9,0x0f,0xb6,0xfa,0x40,0x3a,0xf8,0x0f,0x84,0x20,0x01,0x00,0x00};
#define ADDR_GMALLOC VA(0x1469E59F0ull)     // FMalloc* GMalloc; vtable +0x20 Realloc(this, ptr, size, align)
enum { LT_CharacterSelect = 1, LT_PreRound = 2, LT_PostRound = 3 };
#define TEAMSLOTS_SZ 0x20

typedef void (*SetLayoutFn)(UObject *mgr, uint8_t type);
static SetLayoutFn orig_setlayout;
static float fov_want = 0;              // lineup camera FOV override (0 = the level's, 30)
static float off_x = 211, off_y = -11;  // offset of the first hero without a target point from the last point
static int n_spawned, n_placed;

typedef struct { float q[4]; float t[3], pad0; float s[3], pad1; } Xform;   // FTransform (float, 0x30)

static TArray *arr(UObject *o, const char *prop) { int32_t off = ue_prop_offset(o, prop); return off >= 0 ? (TArray *)((char *)o + off) : NULL; }

// hero team size from the (replicated) slot manager
static int hero_slots(void) {
    UObject *w = ue_world(), *gs = w ? ue_get_ptr(w, "GameState") : NULL, *psm = gs ? ue_get_ptr(gs, "PlayerSlotManager") : NULL;
    TArray *teams = psm ? arr(psm, "TeamSlots") : NULL;
    if (!teams || !teams->num) return 0;
    for (int t = 0; t < teams->num; t++) {
        uint8_t *ts = (uint8_t *)teams->data + t * TEAMSLOTS_SZ;
        if (ts[0] == 0) return ((TArray *)(ts + 8))->num;   // EGobiTeam 0 = heroes
    }
    return ((TArray *)((uint8_t *)teams->data + 8))->num;
}

static int call_parms(UObject *o, const char *fn, void *p, size_t cap) {
    UFunction *f = o ? ue_find_function(U_CLASS(o), fn) : NULL;
    if (!f || UFN_PARMSSIZE(f) > cap) return -1;
    ue_process_event(o, f, p);
    return 0;
}
static int32_t poff(UObject *o, const char *fn, const char *prm) {
    UFunction *f = o ? ue_find_function(U_CLASS(o), fn) : NULL;
    FField *p = f ? ue_find_prop(f, prm) : NULL;
    return p ? FP_OFFSET(p) : -1;
}

static int get_xform(UObject *a, Xform *x) {
    uint8_t p[128] = {0};
    int32_t o = poff(a, "GetTransform", "ReturnValue");
    if (o < 0 || call_parms(a, "GetTransform", p, sizeof p)) return -1;
    memcpy(x, p + o, sizeof *x);
    return 0;
}

static void set_xform(UObject *a, const Xform *x) {
    static uint8_t p[1024];
    memset(p, 0, sizeof p);
    int32_t o = poff(a, "K2_SetActorTransform", "NewTransform"), t = poff(a, "K2_SetActorTransform", "bTeleport");
    if (o < 0) return;
    memcpy(p + o, x, sizeof *x);
    if (t >= 0) p[t] = 1;
    call_parms(a, "K2_SetActorTransform", p, sizeof p);
}

static int grow(TArray *a, int want) {
    if (a->max >= want) return 0;
    void **gm = *(void ***)ADDR_GMALLOC;   // FMalloc*
    if (!gm) return -1;
    typedef void *(*ReallocFn)(void *self, void *ptr, size_t n, uint32_t align);
    void *d = ((ReallocFn)(*(void ***)gm)[4])(gm, a->data, (size_t)want * sizeof(void *), 0);
    if (!d) return -1;
    a->data = d; a->max = want;
    return 0;
}

// Spawn a copy of the last mannequin (deferred spawn, not replicated) and append it.
static UObject *spawn_mannequin(UObject *mgr, TArray *mans, const Xform *at) {
    UObject *last = ((UObject **)mans->data)[mans->num - 1];
    UClass *gsc = ue_find_class("GameplayStatics");
    UObject *cdo = gsc ? UC_CDO(gsc) : NULL;
    if (!last || !cdo) return NULL;
    static uint8_t p[256];
    memset(p, 0, sizeof p);
    int32_t ow = poff(cdo, "BeginDeferredActorSpawnFromClass", "WorldContextObject"),
            oc = poff(cdo, "BeginDeferredActorSpawnFromClass", "ActorClass"),
            ot = poff(cdo, "BeginDeferredActorSpawnFromClass", "SpawnTransform"),
            oh = poff(cdo, "BeginDeferredActorSpawnFromClass", "CollisionHandlingOverride"),
            oo = poff(cdo, "BeginDeferredActorSpawnFromClass", "Owner"),
            orv = poff(cdo, "BeginDeferredActorSpawnFromClass", "ReturnValue");
    if (ow < 0 || oc < 0 || ot < 0 || oh < 0 || oo < 0 || orv < 0) return NULL;
    *(UObject **)(p + ow) = mgr;
    *(UObject **)(p + oc) = U_CLASS(last);
    memcpy(p + ot, at, sizeof *at);
    p[oh] = 1;   // AlwaysSpawn
    *(UObject **)(p + oo) = mgr;   // spawned into the manager's level (the lineup sublevel)
    if (call_parms(cdo, "BeginDeferredActorSpawnFromClass", p, sizeof p)) return NULL;
    UObject *a = *(UObject **)(p + orv);
    if (!a) return NULL;
    int32_t rep = ue_prop_offset(a, "bReplicates");
    if (rep >= 0 && (*((uint8_t *)a + rep) & 1)) {   // a host must not replicate a local lineup prop to clients
        uint8_t q[16] = {0};
        call_parms(a, "SetReplicates", q, sizeof q);
    }
    memset(p, 0, sizeof p);
    int32_t fa = poff(cdo, "FinishSpawningActor", "Actor"), ft = poff(cdo, "FinishSpawningActor", "SpawnTransform");
    if (fa < 0 || ft < 0) return NULL;
    *(UObject **)(p + fa) = a;
    memcpy(p + ft, at, sizeof *at);
    call_parms(cdo, "FinishSpawningActor", p, sizeof p);
    if (grow(mans, mans->num + 1)) return NULL;
    ((UObject **)mans->data)[mans->num++] = a;
    return a;
}

static TArray *points_for(UObject *mgr, int type) {
    return type == LT_PostRound ? arr(mgr, "PostRoundTargetPoints") : type == LT_PreRound ? arr(mgr, "PreRoundLockInTargetPoints") : NULL;
}

// Where hero i goes: the level's target point i, or, past the last of the k points, the last point + (i-k+1) x the
// offset (rotation/scale of the last point). Default offset: the back row, in the gap between the 3rd and 4th hero
// seen from the lineup camera (MAP_CharacterPreRound: camera at x 9758 looking +x, points at x 10119..10264, the
// 5th lands at about (10330, 10058)). A 6th+ would continue further back; untested.
static int slot_xform(TArray *tps, int i, Xform *out) {
    int k = tps ? tps->num : 0;
    if (k < 1) return -1;
    if (i < k) return get_xform(((UObject **)tps->data)[i], out);
    if (get_xform(((UObject **)tps->data)[k - 1], out)) return -1;
    out->t[0] += off_x * (i - k + 1);
    out->t[1] += off_y * (i - k + 1);
    return 0;
}

static void fov(UObject *mgr) {
    if (fov_want <= 0) return;
    UObject *cam = ue_get_ptr(mgr, "Camera"), *cc = cam ? ue_get_ptr(cam, "CameraComponent") : NULL;
    int32_t o = cc ? ue_prop_offset(cc, "FieldOfView") : -1;
    if (o >= 0) *(float *)((char *)cc + o) = fov_want;
}

static void place(UObject *mgr, int type, int n);
static void setlayout_detour(UObject *mgr, uint8_t type) {
    int n = hero_slots();
    TArray *mans = arr(mgr, "Mannequins");
    int extra = mans && mans->num > 0 && n > mans->num && n <= 8;
    if (extra) {
        TArray *tps = points_for(mgr, type) ? points_for(mgr, type) : arr(mgr, "PostRoundTargetPoints");
        for (int i = mans->num; i < n; i++) {
            Xform x;
            if (slot_xform(tps, i, &x) && get_xform(((UObject **)mans->data)[mans->num - 1], &x)) break;
            UObject *m = spawn_mannequin(mgr, mans, &x);
            char b[128];
            LOG("lineup: %d hero slots, %d mannequins: spawned %s", n, mans->num - (m ? 1 : 0), m ? ue_obj_name(m, b, sizeof b) : "nothing");
            if (!m) break;
            n_spawned++;
        }
    }
    orig_setlayout(mgr, type);
    place(mgr, type, n);
}

static void place(UObject *mgr, int type, int n) {
    TArray *mans = arr(mgr, "Mannequins"), *tps = points_for(mgr, type);
    if (!mans || !tps || n <= tps->num || n > mans->num) return;
    for (int i = tps->num; i < n; i++) {
        Xform x;
        if (slot_xform(tps, i, &x)) break;
        set_xform(((UObject **)mans->data)[i], &x);
        n_placed++;
    }
    fov(mgr);
    LOG("lineup: layout %d, placed %d hero(es) beyond %d target points", type, n - tps->num, tps->num);
}

// ---- the mission-start player list with 5+ heroes (#28) ----
// The list at the left of character select / loadout (MissionLoadoutScreen_WBP.PlayerLoadouts, a
// PlayerLoadouts_WBP_C : UPlayerLoadoutsUserWidget) is a VerticalBox (widget +0x468) with 4 PlayerLoadoutsEntry
// rows placed in the designer. OnSlotsUpdated (0x141E3D670) gives hero slot i the row i (PlayerSlot +0x2a0) and skips
// slots without a row, so the 5th player is not listed on any machine. Before it runs, rows of the same class are
// added to the box (WidgetBlueprintLibrary::Create + PanelWidget::AddChild, the new slot gets the last row's
// size/padding/alignment), so the game fills them like the others. They start Hidden like the designer's rows: the
// game shows a human's row, a bot's stays hidden (it showed a stale row before). Two columns: hud.c. A 4-hero team
// never gets here.
#define ADDR_LOADOUTS_SLOTS VA(0x141E3D670ull)   // void UPlayerLoadoutsUserWidget::OnSlotsUpdated (native body)
static const uint8_t SIG_LOADOUTS_SLOTS[] = {0x40,0x55,0x53,0x41,0x54,0x48,0x8d,0x6c,0x24,0xb9,0x48,0x81,0xec,0xc0,0x00,0x00,
                                             0x00,0x48,0x8b,0x05};
#define LOADOUTS_PANEL 0x468                      // the row box (UPanelWidget*), as OnSlotsUpdated reads it
typedef void (*LoadoutsSlotsFn)(UObject *w);
static LoadoutsSlotsFn orig_loadouts;
static int n_rows_added;

static UObject *create_widget(UObject *ctx, UClass *cls) {
    UClass *wbl = ue_find_class("WidgetBlueprintLibrary");
    UObject *cdo = wbl ? UC_CDO(wbl) : NULL;
    static uint8_t p[128];
    memset(p, 0, sizeof p);
    int32_t ow = poff(cdo, "Create", "WorldContextObject"), ot = poff(cdo, "Create", "WidgetType"),
            op = poff(cdo, "Create", "OwningPlayer"), orv = poff(cdo, "Create", "ReturnValue");
    if (!cdo || ow < 0 || ot < 0 || op < 0 || orv < 0) return NULL;
    *(UObject **)(p + ow) = ctx;
    *(UClass **)(p + ot) = cls;
    *(UObject **)(p + op) = ue_local_pc();
    if (call_parms(cdo, "Create", p, sizeof p)) return NULL;
    return *(UObject **)(p + orv);
}

// the new row's slot gets the old one's layout through the slot's own setters (they also update the Slate slot)
static void copy_slot(UObject *from, UObject *to) {
    static const char *props[][3] = {{"Size", "SetSize", "InSize"}, {"Padding", "SetPadding", "InPadding"},
        {"HorizontalAlignment", "SetHorizontalAlignment", "InHorizontalAlignment"},
        {"VerticalAlignment", "SetVerticalAlignment", "InVerticalAlignment"}};
    for (size_t i = 0; i < sizeof props / sizeof *props; i++) {
        FField *f = ue_find_prop((UStruct *)U_CLASS(from), props[i][0]);
        int32_t a = poff(to, props[i][1], props[i][2]);
        if (!f || a < 0 || FP_ELSIZE(f) > 32) continue;
        uint8_t p[64] = {0};
        memcpy(p + a, (char *)from + FP_OFFSET(f), FP_ELSIZE(f));
        call_parms(to, props[i][1], p, sizeof p);
    }
}

static void loadouts_detour(UObject *w) {
    int n = hero_slots();
    UObject *panel = *(UObject **)((char *)w + LOADOUTS_PANEL);
    TArray *slots = panel ? arr(panel, "Slots") : NULL;
    int32_t oc = -1;
    while (slots && slots->num > 0 && slots->num < n && n <= 8) {
        UObject *last_slot = ((UObject **)slots->data)[slots->num - 1];
        if (oc < 0 && last_slot) oc = ue_prop_offset(last_slot, "Content");
        UObject *last = last_slot && oc >= 0 ? *(UObject **)((char *)last_slot + oc) : NULL;
        UObject *row = last ? create_widget(w, U_CLASS(last)) : NULL;
        uint8_t p[32] = {0};
        int32_t pc = poff(panel, "AddChild", "Content"), pr = poff(panel, "AddChild", "ReturnValue");
        if (!row || pc < 0 || pr < 0) { LOG("lineup: could not add a player list row (%d hero slots)", n); break; }
        *(UObject **)(p + pc) = row;
        call_parms(panel, "AddChild", p, sizeof p);
        UObject *s = *(UObject **)(p + pr);
        if (!s) { LOG("lineup: player list AddChild failed"); break; }
        copy_slot(last_slot, s);
        uint8_t v[16] = {2};   // Hidden, like the designer's rows: the game shows a human's row, a bot's stays hidden
        call_parms(row, "SetVisibility", v, sizeof v);
        n_rows_added++;
        char b[128];
        LOG("lineup: %d hero slots: player list row %d added (%s)", n, slots->num, ue_obj_name(row, b, sizeof b));
    }
    orig_loadouts(w);
}

#ifndef B4B_RELEASE
// ---- dev command: lineup [off <dx> <dy> | fov <deg> | apply] ----
static void dump(Out *o) {
    UClass *c = ue_find_class("CharacterLineupLayoutManager");
    UObject *mgr = c ? ue_find_first_of("CharacterLineupLayoutManager") : NULL;
    char b[256];
    out_printf(o, "lineup: hook=%d list_hook=%d list_rows_added=%d off=(%.0f,%.0f) fov=%.0f spawned=%d placed=%d hero_slots=%d\n", orig_setlayout != NULL, orig_loadouts != NULL, n_rows_added,
               off_x, off_y, fov_want, n_spawned, n_placed, hero_slots());
    for (int32_t i = 0, k = ue_num_objects(); c && i < k; i++) {
        UObject *m = ue_object_at(i);
        if (!m || (U_FLAGS(m) & 0x30) || !ue_is_a(m, c)) continue;
        out_printf(o, "manager %s layout=%d\n", ue_full_path(m, b, sizeof b), *((uint8_t *)m + 0x2a4));
        const char *names[] = {"Mannequins", "PreRoundLockInTargetPoints", "PostRoundTargetPoints"};
        for (int a = 0; a < 3; a++) {
            TArray *t = arr(m, names[a]);
            out_printf(o, "  %s %d\n", names[a], t ? t->num : -1);
            for (int j = 0; t && j < t->num; j++) {
                UObject *x = ((UObject **)t->data)[j];
                Xform f = {0};
                if (x) get_xform(x, &f);
                int32_t h = x ? ue_prop_offset(x, "bHidden") : -1;
                out_printf(o, "    [%d] %s at=(%.0f,%.0f,%.0f) q=(%.2f,%.2f,%.2f,%.2f) hidden=%d\n", j, x ? ue_obj_name(x, b, sizeof b) : "null",
                           f.t[0], f.t[1], f.t[2], f.q[0], f.q[1], f.q[2], f.q[3], h >= 0 ? *((uint8_t *)x + h) & 1 : -1);
            }
        }
        UObject *cam = ue_get_ptr(m, "Camera"), *cc = cam ? ue_get_ptr(cam, "CameraComponent") : NULL;
        Xform f = {0};
        if (cam) get_xform(cam, &f);
        int32_t fo = cc ? ue_prop_offset(cc, "FieldOfView") : -1;
        out_printf(o, "  camera at=(%.0f,%.0f,%.0f) fov=%.1f\n", f.t[0], f.t[1], f.t[2], fo >= 0 ? *(float *)((char *)cc + fo) : -1.f);
    }
    (void)mgr;
}

int lineup_cmd(const char *verb, char *rest, Out *o) {
    if (strcmp(verb, "lineup")) return 0;
    char *a = rest ? strtok(rest, " ") : NULL, *v = a ? strtok(NULL, " ") : NULL;
    if (a && !strcmp(a, "fov") && v) fov_want = (float)atof(v);
    else if (a && !strcmp(a, "off") && v) { char *w2 = strtok(NULL, " "); off_x = (float)atof(v); off_y = w2 ? (float)atof(w2) : 0; }
    if (a && strcmp(a, "fov") && strcmp(a, "off") && strcmp(a, "apply")) {
        out_printf(o, "usage: lineup [off <dx> <dy> | fov <deg> | apply]\n");
        return 1;
    }
    if (a) {   // re-place the heroes of every lineup that is showing (layout 2/3) with the new settings
        UClass *c = ue_find_class("CharacterLineupLayoutManager");
        for (int32_t i = 0, k = ue_num_objects(); c && i < k; i++) {
            UObject *m = ue_object_at(i);
            if (!m || (U_FLAGS(m) & 0x30) || !ue_is_a(m, c)) continue;
            uint8_t t = *((uint8_t *)m + 0x2a4);
            if (t == LT_PreRound || t == LT_PostRound) place(m, t, hero_slots());
        }
    }
    dump(o);
    return 1;
}
#endif  // !B4B_RELEASE

int lineup_init(void) {
    const char *e = getenv("B4BCOOP_NO_LINEUP");
    if (e && *e && *e != '0') { LOG("lineup: disabled by B4BCOOP_NO_LINEUP"); return 0; }
    if (memcmp((void *)ADDR_SETLAYOUT, SIG_SETLAYOUT, sizeof SIG_SETLAYOUT)) { LOG("lineup: signature mismatch"); return -1; }
    if (MH_CreateHook((void *)ADDR_SETLAYOUT, (void *)setlayout_detour, (void **)&orig_setlayout) != MH_OK ||
        MH_EnableHook((void *)ADDR_SETLAYOUT) != MH_OK) { LOG("lineup: hook failed"); orig_setlayout = NULL; return -1; }
    LOG("lineup: SetLayoutType hooked");
    if (memcmp((void *)ADDR_LOADOUTS_SLOTS, SIG_LOADOUTS_SLOTS, sizeof SIG_LOADOUTS_SLOTS)) LOG("lineup: player list signature mismatch");
    else if (MH_CreateHook((void *)ADDR_LOADOUTS_SLOTS, (void *)loadouts_detour, (void **)&orig_loadouts) != MH_OK ||
             MH_EnableHook((void *)ADDR_LOADOUTS_SLOTS) != MH_OK) LOG("lineup: player list hook failed");
    return 0;
}
