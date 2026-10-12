// Added weapon looks (add-ons with `weapon=` lines, modkit `b4bmod weapon --as <name>`): a new model/look for a retail
// weapon that only the players who pick it (/model <name>) carry, instead of replacing the weapon for everyone.
// Design and results: docs/investigations/new-assets.md §9.
//
// How a weapon's look travels: every weapon actor (Item) has an ItemMeshManagementComponent whose replicated
// CustomizationRow (+0x258, an FDataTableRowHandle of the weapon's <Code>_Customization_DT) is the equipped skin. The
// owning client sets it with the server RPC ServerCustomizationRow, whose implementation (0x1418BBE90) copies it
// without any check and applies it (0x1418BA5D0). A row the table doesn't have takes the "no skin" path: the
// components' material overrides are emptied, meshes stay as they are (the weapon's default look).
// So the look is a made-up row "b4bcoop.weapon.<name>" in that component: the game on every machine shows the default
// weapon for it (players without the add-on or without b4bcoop), and every b4bcoop machine that has the add-on puts
// the add-on's meshes on the weapon's first/third-person mesh components (paired with the retail meshes by object
// name: the add-on's copies keep the template's names). Nothing is written to any profile or campaign save (the
// campaign run keeps items as {row, quantity, attachments, ammo}, no skin).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "MinHook.h"
#include "ue.h"
#include "log.h"
#include "cmds.h"
#include "overlay.h"

typedef FName *(*FNameCtorFn)(FName *self, const wchar_t *name, int find_type);
#define ADDR_FNAME_CTOR VA(0x1424BC8E0ull)
// UItemMeshManagementComponent::ApplyCustomization(this): the row's skin (or the no-skin path); called by the
// ServerCustomizationRow implementation, OnRep_CustomizationRow and the first-person mesh init
#define ADDR_APPLY VA(0x1418BA5D0ull)
static const uint8_t SIG_APPLY[] = {0x48,0x89,0x4c,0x24,0x08,0x55,0x57,0x41,0x56,0x48,0x8d,0x6c,0x24,0xf0,0x48,0x81,
                                    0xec,0x10,0x01,0x00,0x00,0x4c,0x8b,0xf1};
// ServerCustomizationRow_Implementation(this, const FDataTableRowHandle *row) (vtable +0x500; _Validate: true)
#define ADDR_SETROW VA(0x1418BBE90ull)
static const uint8_t SIG_SETROW[] = {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x02,0x48,0x8b,0xda,0x48,0x89,0x81,0x58,
                                     0x02,0x00,0x00,0x48,0x8b,0x42,0x08,0x48};

typedef struct { UObject *table; FName row; FString display; } RowHandle;   // FDataTableRowHandle in this build (0x20)
#define WROW_PREFIX "b4bcoop.weapon."
#define REFUSED "The host turned weapon looks off"

static int fname_eq(FName a, FName b) { return a.idx == b.idx && a.num == b.num; }
static FName make_name(const char *s) {
    wchar_t w[256]; int k = 0;
    for (; s[k] && k < 255; k++) w[k] = (wchar_t)(unsigned char)s[k];
    w[k] = 0;
    FName n = {0};
    ((FNameCtorFn)ADDR_FNAME_CTOR)(&n, w, 1);
    return n;
}
static int is_live(UObject *o) { return o && !(U_FLAGS(o) & 0x30); }
static int alive(UObject *o, int32_t idx) { return o && ue_object_at(idx) == o && is_live(o); }
static UFunction *fn_of(UObject *o, const char *name) { return o ? ue_find_function(U_CLASS(o), name) : NULL; }
static int32_t parm_off(UFunction *f, const char *p) { FField *x = f ? ue_find_prop(f, p) : NULL; return x ? FP_OFFSET(x) : -1; }
static int is_client(void) {
    UObject *w = ue_world(), *nd = w ? ue_get_ptr(w, "NetDriver") : NULL;
    return nd && ue_get_ptr(nd, "ServerConnection");
}

// ---- the add-ons' looks ----
enum { M_FP, M_SM, M_SKM, NM };
typedef struct {
    char name[33], code[16], title[64], addon[96], path[NM][200];
    UObject *mesh[NM]; int32_t idx[NM]; int tried[NM];
} Look;
#define MAX_LOOKS 512
static Look looks[MAX_LOOKS];
static int n_looks = -1;
static void build_looks(void) {
    AddonWeapon a[MAX_LOOKS];
    n_looks = addons_weapons(a, MAX_LOOKS);
    for (int i = 0; i < n_looks; i++) {
        Look *l = &looks[i];
        memset(l, 0, sizeof *l);
        snprintf(l->name, sizeof l->name, "%s", a[i].name); snprintf(l->code, sizeof l->code, "%s", a[i].code);
        snprintf(l->title, sizeof l->title, "%s", a[i].title); snprintf(l->addon, sizeof l->addon, "%s", a[i].addon);
        snprintf(l->path[M_FP], 200, "%s", a[i].fp); snprintf(l->path[M_SM], 200, "%s", a[i].sm3p);
        snprintf(l->path[M_SKM], 200, "%s", a[i].skm3p);
        LOG("wlooks: add-on weapon look %s for %s (\"%s\", %s): %s %s %s", l->name, l->code, l->title, l->addon,
            l->path[0], l->path[1], l->path[2]);
    }
}
// An add-on loaded at runtime (the ~ window's Get more / Installed, /addons on): append its weapon looks; a look whose add-on was updated
// (addons_update_runtime: none of its old meshes was loaded) takes the new file's meshes and title.
void wlooks_refresh(void) {
    if (n_looks < 0) return;
    AddonWeapon a[MAX_LOOKS];
    int n = addons_weapons(a, MAX_LOOKS);
    for (int i = 0; i < n; i++) {
        int k = 0;
        while (k < n_looks && _stricmp(looks[k].name, a[i].name)) k++;
        if (k < n_looks) {
            Look *l = &looks[k];
            const char *np[NM] = {a[i].fp, a[i].sm3p, a[i].skm3p};
            int ch = strcmp(l->title, a[i].title) || strcmp(l->addon, a[i].addon) || strcmp(l->code, a[i].code);
            for (int m = 0; m < NM; m++) if (strcmp(l->path[m], np[m])) {
                snprintf(l->path[m], 200, "%s", np[m]);
                l->mesh[m] = NULL; l->idx[m] = 0; l->tried[m] = 0;
                ch = 1;
            }
            if (!ch) continue;
            snprintf(l->code, sizeof l->code, "%s", a[i].code);
            snprintf(l->title, sizeof l->title, "%s", a[i].title); snprintf(l->addon, sizeof l->addon, "%s", a[i].addon);
            LOG("wlooks: add-on weapon look %s for %s (\"%s\", %s) updated", l->name, l->code, l->title, l->addon);
            continue;
        }
        if (n_looks == MAX_LOOKS) break;
        Look *l = &looks[n_looks++];
        memset(l, 0, sizeof *l);
        snprintf(l->name, sizeof l->name, "%s", a[i].name); snprintf(l->code, sizeof l->code, "%s", a[i].code);
        snprintf(l->title, sizeof l->title, "%s", a[i].title); snprintf(l->addon, sizeof l->addon, "%s", a[i].addon);
        snprintf(l->path[M_FP], 200, "%s", a[i].fp); snprintf(l->path[M_SM], 200, "%s", a[i].sm3p);
        snprintf(l->path[M_SKM], 200, "%s", a[i].skm3p);
        LOG("wlooks: add-on weapon look %s for %s (\"%s\", %s, added now)", l->name, l->code, l->title, l->addon);
    }
}
static Look *look_by_name(const char *name) {
    if (n_looks < 0) build_looks();
    for (int i = 0; i < n_looks; i++) if (!_stricmp(looks[i].name, name)) return &looks[i];
    return NULL;
}
// "b4bcoop.weapon.<name>" with a well-formed name: 1, name copied to out
static int wrow_name(FName row, char *out, size_t n) {
    char b[80];
    ue_name(row, b, sizeof b);
    if (_strnicmp(b, WROW_PREFIX, sizeof WROW_PREFIX - 1)) return 0;
    const char *nm = b + sizeof WROW_PREFIX - 1;
    size_t l = strlen(nm);
    if (!l || l > 32 || !islower((unsigned char)*nm)) return 0;
    for (const char *c = nm; *c; c++) if (!islower((unsigned char)*c) && !isdigit((unsigned char)*c) && *c != '_') return 0;
    if (out) snprintf(out, n, "%s", nm);
    return 1;
}
static int is_ours(FName row) { char b[80]; return !strncmp(ue_name(row, b, sizeof b), "b4bcoop.", 8); }
static UObject *look_mesh(Look *l, int k) {   // loaded on first use (blocking), kept alive by the components using it
    if (!l->path[k][0]) return NULL;
    if (l->mesh[k] && alive(l->mesh[k], l->idx[k])) return l->mesh[k];
    if (l->tried[k] > 2) return NULL;   // failed three times: not in this add-on / broken
    l->mesh[k] = models_load_asset(l->path[k]);   // again after a map change (garbage-collected when unused)
    l->idx[k] = l->mesh[k] ? U_INDEX(l->mesh[k]) : 0;
    if (!l->mesh[k] && ++l->tried[k] > 2) LOG("wlooks: %s: %s failed to load", l->name, l->path[k]);
    return l->mesh[k];
}

// ---- weapons ----
// An Item of weapon <code>: its class is <Code>_<n>_BP_C / <Code>_BP_C (pickups are separate actors)
static int item_code(UObject *item, const char *code) {
    char b[128];
    size_t n = strlen(code);
    ue_obj_name(U_CLASS(item), b, sizeof b);
    return !_strnicmp(b, code, n) && b[n] == '_' && !strstr(b, "Pickup");
}
static UObject *mm_of(UObject *item) { return item ? ue_get_ptr(item, "MeshManagementComponent") : NULL; }
static RowHandle *row_of(UObject *mm) {
    int32_t off = mm ? ue_prop_offset(mm, "CustomizationRow") : -1;
    return off >= 0 ? (RowHandle *)((char *)mm + off) : NULL;
}
static UObject *cust_table(UObject *item) {   // ItemRow.WeaponCustomizationTable (ItemRow +0x70)
    int32_t off = ue_prop_offset(item, "ItemRow");
    return off >= 0 ? *(UObject **)((char *)item + off + 0x70) : NULL;
}
static int items_of(UObject *pawn, UObject ***out) {   // Inventory.EquipmentSlots
    UObject *inv = pawn ? ue_get_ptr(pawn, "Inventory") : NULL;
    int32_t off = inv ? ue_prop_offset(inv, "EquipmentSlots") : -1;
    if (off < 0) return 0;
    TArray *a = (TArray *)((char *)inv + off);
    *out = (UObject **)a->data;
    return a->data ? a->num : 0;
}

// ---- putting the meshes on ----
static UClass *c_skc, *c_smc;
static void classes(void) {
    if (!c_skc) c_skc = ue_find_class("SkeletalMeshComponent");
    if (!c_smc) c_smc = ue_find_class("StaticMeshComponent");
}
static UObject *comp_mesh(UObject *c) { return ue_get_ptr(c, ue_is_a(c, c_skc) ? "SkeletalMesh" : "StaticMesh"); }
static void set_comp_mesh(UObject *c, UObject *mesh, int clear_overrides) {
    int sk = ue_is_a(c, c_skc);
    UFunction *f = fn_of(c, sk ? "SetSkeletalMesh" : "SetStaticMesh");
    int32_t pm = parm_off(f, "NewMesh"), pr = parm_off(f, "bReinitPose");
    if (!f || pm < 0 || UFN_PARMSSIZE(f) > 32) return;
    int32_t om = clear_overrides ? ue_prop_offset(c, "OverrideMaterials") : -1;   // a skin's don't fit our mesh
    if (om >= 0) ((TArray *)((char *)c + om))->num = 0;
    uint8_t p[32] = {0};
    *(UObject **)(p + pm) = mesh;
    if (pr >= 0) p[pr] = 1;
    ue_process_event(c, f, p);
}
// Components we changed, with what they had: put back when the weapon's row is no longer a look of ours.
// The first-person skin's materials are set once when the weapon's FP mesh is set up (ApplyCustomization leaves them),
// so they are kept here and put back with the mesh.
#define MAX_MATS 16
// floor: item is a weapon pickup lying on the floor (a dropped weapon, §9 "Floor"), kept until it's gone or models go off;
// conf: the host said so (else a client's own guess, taken back when the host names another pickup), t: when put on
typedef struct { UObject *comp, *orig, *item; int32_t ci, oi, ii; char look[33]; UObject *mat[MAX_MATS]; int32_t mi[MAX_MATS]; int nmat, floor, conf; float t; } Swap;
#define MAX_SWAPS 96
static Swap swaps[MAX_SWAPS];
static Swap *swap_of(UObject *c) {
    for (int i = 0; i < MAX_SWAPS; i++) if (swaps[i].comp == c && alive(c, swaps[i].ci)) return &swaps[i];
    return NULL;
}
static void note_swap(UObject *c, UObject *orig, UObject *item, const char *look) {
    Swap *s = swap_of(c);
    if (!s) for (int i = 0; i < MAX_SWAPS && !s; i++) if (!swaps[i].comp || !alive(swaps[i].comp, swaps[i].ci)) s = &swaps[i];
    if (!s) return;
    if (s->comp != c) {   // first swap keeps the retail mesh and its material overrides
        s->orig = orig; s->oi = orig ? U_INDEX(orig) : 0;
        int32_t om = ue_prop_offset(c, "OverrideMaterials");
        TArray *a = om >= 0 ? (TArray *)((char *)c + om) : NULL;
        s->nmat = 0;
        for (int i = 0; a && i < a->num && i < MAX_MATS; i++) {
            UObject *m = ((UObject **)a->data)[i];
            s->mat[i] = m; s->mi[i] = m ? U_INDEX(m) : 0; s->nmat = i + 1;
        }
    }
    s->comp = c; s->ci = U_INDEX(c); s->item = item; s->ii = U_INDEX(item); s->floor = 0; s->conf = 0;
    snprintf(s->look, sizeof s->look, "%s", look);
}
static int n_applied;
// Every mesh component of the item's first/third-person sets whose mesh has the name of one of the look's meshes
// (the retail one it was copied from, or ours already) gets ours.
static void apply_look(UObject *item, UObject *mm, Look *l) {
    classes();
    static const char *arrs[2] = {"FirstPersonMeshComponents", "ThirdPersonMeshComponents"};
    int changed = 0;
    for (int a = 0; a < 2; a++) {
        int32_t off = ue_prop_offset(mm, arrs[a]);
        TArray *arr = off >= 0 ? (TArray *)((char *)mm + off) : NULL;
        for (int i = 0; arr && i < arr->num; i++) {
            UObject *c = ((UObject **)arr->data)[i];
            if (!is_live(c) || (!ue_is_a(c, c_skc) && !ue_is_a(c, c_smc))) continue;
            UObject *cur = comp_mesh(c);
            if (!cur) continue;
            char cn[128], mn[128];
            ue_obj_name(cur, cn, sizeof cn);
            for (int k = 0; k < NM; k++) {
                if (!l->path[k][0]) continue;
                const char *base = strrchr(l->path[k], '.');
                if (!base || _stricmp(base + 1, cn)) continue;
                UObject *m = look_mesh(l, k);
                if (!m || m == cur) break;
                if (strcmp(ue_obj_name(U_CLASS(m), mn, sizeof mn), ue_is_a(c, c_skc) ? "SkeletalMesh" : "StaticMesh")) break;
                Swap *s = swap_of(c);
                note_swap(c, s ? s->orig : cur, item, l->name);
                set_comp_mesh(c, m, 1);
                changed++;
                break;
            }
        }
    }
    if (changed) {
        n_applied++;
        char b[96];
        LOG("wlooks: %s wears %s (%d mesh(es))", ue_obj_name(item, b, sizeof b), l->name, changed);
    }
}
// The component's retail mesh and the material overrides it had back (those the new skin didn't set); the entry freed
static void put_back(Swap *s) {
    if (s->orig && alive(s->orig, s->oi)) {
        classes();
        set_comp_mesh(s->comp, s->orig, 0);
        UFunction *sm = s->nmat ? fn_of(s->comp, "SetMaterial") : NULL;
        int32_t pe = parm_off(sm, "ElementIndex"), pm = parm_off(sm, "Material");
        int32_t om = ue_prop_offset(s->comp, "OverrideMaterials");
        TArray *cur = om >= 0 ? (TArray *)((char *)s->comp + om) : NULL;
        for (int k = 0; sm && pe >= 0 && pm >= 0 && k < s->nmat && UFN_PARMSSIZE(sm) <= 32; k++) {
            if (cur && k < cur->num && ((UObject **)cur->data)[k]) continue;   // the new skin set this one
            if (!s->mat[k] || !alive(s->mat[k], s->mi[k])) continue;
            uint8_t p[32] = {0};
            *(int32_t *)(p + pe) = k; *(UObject **)(p + pm) = s->mat[k];
            ue_process_event(s->comp, sm, p);
        }
        char b[96], c[64];
        LOG("wlooks: %s %s back to its own mesh", ue_obj_name(s->item, b, sizeof b), ue_obj_name(s->comp, c, sizeof c));
    }
    memset(s, 0, sizeof *s);
}
// Components whose weapon no longer carries that look (only == item: that weapon's): their retail mesh back, before
// the game applies the new skin (ApplyCustomization hook) so its material overrides stay.
static void restore_stale(UObject *only) {
    for (int i = 0; i < MAX_SWAPS; i++) {
        Swap *s = &swaps[i];
        if (!s->comp || (only && s->item != only)) continue;
        if (!alive(s->comp, s->ci) || !alive(s->item, s->ii)) { memset(s, 0, sizeof *s); continue; }
        char nm[40];
        if (s->floor) { if (!models_off()) continue; }
        else {
            RowHandle *r = row_of(mm_of(s->item));
            if (r && wrow_name(r->row, nm, sizeof nm) && !strcmp(nm, s->look) && look_by_name(nm)) continue;
        }
        put_back(s);
    }
}
static float now;   // seconds since load (wlooks_tick)
static void carried_note(UObject *item, UObject *pawn, const char *name, const char *code);
static void code_of(UObject *item, char *out, size_t n) {   // AR02_1_BP_C -> AR02
    char b[128];
    ue_obj_name(U_CLASS(item), b, sizeof b);
    char *u = strchr(b, '_');
    if (u) *u = 0;
    snprintf(out, n, "%s", b);
}
static void check_item(UObject *item, UObject *pawn) {
    UObject *mm = mm_of(item);
    RowHandle *r = row_of(mm);
    char nm[40], code[16];
    if (!r || !wrow_name(r->row, nm, sizeof nm)) return;
    Look *l = look_by_name(nm);
    if (l && !item_code(item, l->code)) return;
    if (l) apply_look(item, mm, l);
    // the host remembers every weapon wearing a look, add-on or not: it tells the clients where their pickups lie
    if (l || !is_client()) { code_of(item, code, sizeof code); carried_note(item, pawn, nm, code); }
}

// ---- weapons on the floor ----
// A dropped weapon is not the weapon actor: the host destroys the Item and spawns an ItemPickup (<Code>_N_Pickup_BP,
// replicated) whose ItemRowsAndQuantities {row, quantity, attachments, ammo} carry no skin, showing 3P_<Code>_SM on its
// StaticMeshComponent (so retail skins don't show on the floor either). Every machine keeps a list of the heroes'
// weapons wearing a look (clients: only looks they have); when one leaves its hero's inventory, the new pickup of that
// weapon type gets the look's 3P static mesh.
// - Host (add-on or not): the pickup whose PreviousOwner (weak pointer +0x320, GetPreviousOwner 0x142199930, not
//   replicated) is that hero, else the nearest "dropped from player" one within 3 m. Once it lies still (or after 5 s)
//   the host sends every client "wlook floor <look> <code> <x> <y> <z>" (a data line, chat.c CHAT_DATA_TYPE), and a
//   player who joins later gets "wlook hello" plus every such pickup still lying there; "wlook gone ..." when it's
//   picked up (a client still waiting for that pickup forgets it).
// - Client with the add-on: right away its own guess (nearest pickup of that type within 3 m of where the hero stood);
//   the host's line then puts the look on the pickup lying at that spot (±1 m, it may arrive before the pickup does),
//   and takes it off a guessed pickup nearby. A guess the host never confirms goes back after 10 s once the host is
//   known to send these (its "hello"); older hosts: the guess stays.
// Machines without the add-on show the pickup as the game does. Picking it up makes a new Item for the new owner with
// no skin: their own choice (their skin, or their /model look) applies, like retail skins. World pickups nobody dropped
// (loot, context 1) stay retail.
typedef struct { UObject *item, *pawn; int32_t ii, pi; char name[33], code[16]; float loc[3]; } Carried;
#define MAX_CARRIED 32
static Carried carried[MAX_CARRIED];
typedef struct { UObject *pawn; int32_t pi; char name[33], code[16]; float loc[3], left, next; } Drop;
#define MAX_DROPS 8
static Drop drops[MAX_DROPS];
static int n_floor;
// host: dropped weapons wearing a look, told to the clients once they lie still
typedef struct { UObject *pk; int32_t pi; char name[33], code[16]; float loc[3], age; int still, sent; } FloorRec;
#define MAX_RECS 32
static FloorRec recs[MAX_RECS];
static int n_sent;
// client: the host's lines not matched to a pickup yet (it isn't replicated yet / still in the air here)
typedef struct { int on; char name[33], code[16]; float loc[3], age; UObject *world; } Pending;
#define MAX_PENDING 32
static Pending pending[MAX_PENDING];
static int host_speaks, n_host_lines;   // the host sends floor lines (it said hello / sent one) on this connection
static UObject *host_conn;

static int actor_loc(UObject *a, float out[3]) {
    UFunction *f = fn_of(a, "K2_GetActorLocation");
    int32_t pr = parm_off(f, "ReturnValue");
    if (!f || pr < 0 || UFN_PARMSSIZE(f) > 32) return 0;
    uint8_t p[32] = {0};
    ue_process_event(a, f, p);
    memcpy(out, p + pr, 12);
    return 1;
}
static float dist2(const float *a, const float *b) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}
static int in_inventory(UObject *pawn, UObject *item);
static void carried_note(UObject *item, UObject *pawn, const char *name, const char *code) {
    if (!pawn) pawn = ue_get_ptr(item, "Owner");
    // a weapon not (yet) in its hero's replicated inventory (e.g. just joined) would look dropped the next frame
    if (!is_live(pawn) || !in_inventory(pawn, item)) return;
    Carried *c = NULL;
    for (int i = 0; i < MAX_CARRIED && !c; i++) if (carried[i].item == item && alive(item, carried[i].ii)) c = &carried[i];
    for (int i = 0; i < MAX_CARRIED && !c; i++) if (!carried[i].item) c = &carried[i];
    if (!c) return;
    c->item = item; c->ii = U_INDEX(item); c->pawn = pawn; c->pi = U_INDEX(pawn);
    snprintf(c->name, sizeof c->name, "%s", name); snprintf(c->code, sizeof c->code, "%s", code);
    actor_loc(pawn, c->loc);   // where it stood last (a drop is looked for around the hero)
}
static int in_inventory(UObject *pawn, UObject *item) {
    UObject **it;
    int n = items_of(pawn, &it);
    for (int i = 0; i < n; i++) if (it[i] == item) return 1;
    return 0;
}
// Every frame: a weapon with a look that left its hero (dropped, thrown, swapped for a pickup, destroyed) = a drop to
// look for. The row changing (/model reset, /models off) is not a drop.
static void tick_carried(void) {
    for (int i = 0; i < MAX_CARRIED; i++) {
        Carried *c = &carried[i];
        if (!c->item) continue;
        int item_ok = alive(c->item, c->ii), pawn_ok = alive(c->pawn, c->pi);
        if (item_ok && pawn_ok && in_inventory(c->pawn, c->item)) {
            char nm[40];
            RowHandle *r = row_of(mm_of(c->item));
            if (!r || !wrow_name(r->row, nm, sizeof nm) || strcmp(nm, c->name)) memset(c, 0, sizeof *c);
            continue;
        }
        Drop *d = NULL;
        for (int k = 0; k < MAX_DROPS && !d; k++) if (drops[k].left <= 0) d = &drops[k];
        if (d && pawn_ok && !models_off()) {
            d->pawn = c->pawn; d->pi = c->pi; d->left = 4; d->next = 0;
            snprintf(d->name, sizeof d->name, "%s", c->name); snprintf(d->code, sizeof d->code, "%s", c->code);
            if (!actor_loc(c->pawn, d->loc)) memcpy(d->loc, c->loc, sizeof d->loc);
            char b[96];
            LOG("wlooks: %s (%s, %s) left %s, looking for its pickup", c->name, c->code, item_ok ? "kept" : "gone",
                ue_obj_name(c->pawn, b, sizeof b));
        }
        memset(c, 0, sizeof *c);
    }
}

static UClass *c_pickup;
static const char *const pk_comps[2] = {"StaticMeshComponent", "InterpolatedStaticMeshComponent"};
static UObject *pickup_mesh(UObject *pk, char *name, size_t n) {   // the pickup's shown static mesh and its name
    UObject *c = ue_get_ptr(pk, "StaticMeshComponent"), *m = is_live(c) ? ue_get_ptr(c, "StaticMesh") : NULL;
    if (name) { name[0] = 0; if (m) ue_obj_name(m, name, n); }
    return m;
}
static int pickup_code(UObject *pk, const char *code) {   // <Code>_N_Pickup_BP_C
    char b[128];
    size_t n = strlen(code);
    ue_obj_name(U_CLASS(pk), b, sizeof b);
    return n && !_strnicmp(b, code, n) && b[n] == '_' && strstr(b, "Pickup");
}
static Swap *floor_of(UObject *pk) {
    for (int i = 0; i < MAX_SWAPS; i++)
        if (swaps[i].floor && swaps[i].item == pk && alive(pk, swaps[i].ii) && alive(swaps[i].comp, swaps[i].ci)) return &swaps[i];
    return NULL;
}
static FloorRec *rec_of(UObject *pk) {
    for (int i = 0; i < MAX_RECS; i++) if (recs[i].pk == pk && alive(pk, recs[i].pi)) return &recs[i];
    return NULL;
}
// The look's 3P static mesh on the pickup's mesh components that show the weapon's (by name, as on the weapon);
// the number of components now showing it (0: nothing to put it on)
static int floor_apply(UObject *pk, Look *l, int conf) {
    const char *base = strrchr(l->path[M_SM], '.');
    if (!base) return 0;
    classes();
    int on = 0;
    for (int k = 0; k < 2; k++) {
        UObject *c = ue_get_ptr(pk, pk_comps[k]), *cur = is_live(c) && ue_is_a(c, c_smc) ? comp_mesh(c) : NULL;
        char cn[128];
        if (!cur || _stricmp(ue_obj_name(cur, cn, sizeof cn), base + 1)) continue;
        UObject *m = look_mesh(l, M_SM);
        if (!m) continue;
        Swap *s = swap_of(c);
        if (m != cur || !s || strcmp(s->look, l->name)) {
            note_swap(c, s ? s->orig : cur, pk, l->name);
            if (m != cur) set_comp_mesh(c, m, 1);
            if ((s = swap_of(c))) s->t = now;
        }
        if (s) { s->floor = 1; s->conf |= conf; }
        on++;
    }
    return on;
}
static void floor_put_back(UObject *pk) {
    for (int i = 0; i < MAX_SWAPS; i++) if (swaps[i].floor && swaps[i].item == pk) put_back(&swaps[i]);
}
static int ctx_off = -2;
static int pickup_ctx(UObject *pk) {   // EItemPickupCreationContext (replicated): 0 player, 1 loot, 2 kill, 3 player item
    if (ctx_off == -2) ctx_off = ue_prop_offset(pk, "CreationContext");
    return ctx_off >= 0 ? *((uint8_t *)pk + ctx_off) : -1;
}
static UObject *pickup_prev_owner(UObject *pk) { return ue_weak_get((char *)pk + 0x320); }
// A looted pickup isn't destroyed: it stays (hidden, at 0,0,0) with quantity 0 (seen live), maybe to be used again
static int pickup_looted(UObject *pk) {
    static int32_t off = -2;
    if (off == -2) off = ue_prop_offset(pk, "ItemRowsAndQuantities");
    if (off < 0) return 0;
    TArray *a = (TArray *)((char *)pk + off);
    int q = 0;
    for (int i = 0; a->data && i < a->num && i < 16; i++) q += *(int32_t *)((char *)a->data + i * 0x48 + 0x20);   // ItemRowAndQuantity.Quantity
    return q <= 0;
}
// Pending drops: find their pickup (scanned 5x a second while one is pending, for 4 s)
static void tick_drops(float dt) {
    int any = 0, host = !is_client();
    for (int k = 0; k < MAX_DROPS; k++) {
        Drop *d = &drops[k];
        if (d->left <= 0) continue;
        d->left -= dt; d->next -= dt;
        if (d->next <= 0 && d->left > 0) { any = 1; d->next = 0.2f; }
    }
    if (!any) return;
    if (!c_pickup) c_pickup = ue_find_class("ItemPickup");
    if (!c_pickup) return;
    UObject *best[MAX_DROPS] = {0};
    float bd[MAX_DROPS];
    for (int k = 0; k < MAX_DROPS; k++) bd[k] = 300.f * 300.f;
    for (int32_t i = 0, n = ue_num_objects(); i < n; i++) {
        UObject *o = ue_object_at(i);
        if (!is_live(o) || !ue_is_a(o, c_pickup) || floor_of(o) || rec_of(o) || pickup_looted(o)) continue;
        UObject *own = NULL, *ins = NULL, *prev = NULL;
        int got_prev = 0, ctx = -1, have_loc = 0;
        float loc[3];
        for (int k = 0; k < MAX_DROPS; k++) {
            Drop *d = &drops[k];
            if (d->left <= 0 || !pickup_code(o, d->code)) continue;
            if (!got_prev) {
                own = ue_get_ptr(o, "Owner"); ins = ue_get_ptr(o, "Instigator"); prev = pickup_prev_owner(o);
                ctx = pickup_ctx(o); got_prev = 1;
            }
            if (own == d->pawn || ins == d->pawn || prev == d->pawn) { best[k] = o; bd[k] = -1; continue; }
            if (bd[k] < 0 || (ctx != 0 && ctx != 3)) continue;
            if (!have_loc) have_loc = actor_loc(o, loc);
            float q = have_loc ? dist2(loc, d->loc) : 1e30f;
            if (q < bd[k]) { bd[k] = q; best[k] = o; }
        }
    }
    for (int k = 0; k < MAX_DROPS; k++) {
        Drop *d = &drops[k];
        if (d->left <= 0 || !best[k] || floor_of(best[k]) || rec_of(best[k])) continue;
        Look *l = look_by_name(d->name);
        int n = l ? floor_apply(best[k], l, host) : 0;
        if (host) {
            FloorRec *r = NULL;
            for (int i = 0; i < MAX_RECS && !r; i++) if (!recs[i].pk || !alive(recs[i].pk, recs[i].pi)) r = &recs[i];
            if (r) {
                memset(r, 0, sizeof *r);
                r->pk = best[k]; r->pi = U_INDEX(best[k]);
                snprintf(r->name, sizeof r->name, "%s", d->name); snprintf(r->code, sizeof r->code, "%s", d->code);
                actor_loc(best[k], r->loc);
            }
        }
        char b[96];
        LOG("wlooks: %s on the floor: %s (%s, %d mesh(es)%s)", d->name, ue_obj_name(best[k], b, sizeof b),
            bd[k] < 0 ? "its dropper" : "nearest", n, host ? ", told once it lies still" : ", a guess until the host says");
        if (n) n_floor++;
        d->left = 0;
    }
}

// ---- host: telling the clients ----
static UClass *c_pc;
static int remote_pcs(UObject **out, int max) {   // every remote player's controller
    if (!c_pc) c_pc = ue_find_class("PlayerController");
    UObject **pa, *me = ue_local_pc();
    int n = admin_player_array(&pa), k = 0;
    for (int i = 0; i < n && k < max; i++) {
        UObject *pc = is_live(pa[i]) ? ue_get_ptr(pa[i], "Owner") : NULL;
        if (pc && pc != me && is_live(pc) && c_pc && ue_is_a(pc, c_pc)) out[k++] = pc;
    }
    return k;
}
static void rec_line(FloorRec *r, const char *verb, char *b, size_t n) {
    snprintf(b, n, "wlook %s %s %s %d %d %d", verb, r->name, r->code, (int)lroundf(r->loc[0]), (int)lroundf(r->loc[1]),
             (int)lroundf(r->loc[2]));
}
typedef struct { UObject *pc; int32_t i; float wait; int done; } Peer;
#define MAX_PEERS 16
static Peer peers[MAX_PEERS];
static float host_acc;
// Every 0.25 s on the host: a pickup told once it lies still (moved < 2 cm twice) or after 5 s; a player first seen
// gets "hello" and every pickup told so far after 2 s (joined late: those pickups were dropped before it came).
static void tick_host(float dt) {
    if ((host_acc += dt) < 0.25f) return;
    float step = host_acc;
    host_acc = 0;
    UObject *pcs[MAX_PEERS];
    int np = remote_pcs(pcs, MAX_PEERS);
    for (int i = 0; i < MAX_PEERS; i++) if (peers[i].pc && !alive(peers[i].pc, peers[i].i)) memset(&peers[i], 0, sizeof peers[i]);
    for (int k = 0; k < np; k++) {
        Peer *p = NULL;
        for (int i = 0; i < MAX_PEERS && !p; i++) if (peers[i].pc == pcs[k]) p = &peers[i];
        for (int i = 0; i < MAX_PEERS && !p; i++) if (!peers[i].pc) { p = &peers[i]; p->pc = pcs[k]; p->i = U_INDEX(pcs[k]); p->wait = 2; p->done = 0; }
        if (!p || p->done || (p->wait -= step) > 0) continue;
        p->done = 1;
        admin_data_to(p->pc, "wlook hello");
        int sent = 0;
        char b[160];
        for (int i = 0; i < MAX_RECS; i++) {
            FloorRec *r = &recs[i];
            if (!r->pk || !r->sent || !alive(r->pk, r->pi) || pickup_looted(r->pk) || models_off()) continue;
            actor_loc(r->pk, r->loc);   // where it lies now
            rec_line(r, "floor", b, sizeof b);
            admin_data_to(p->pc, b);
            sent++;
        }
        char a[64] = "?";
        UObject *ps = ue_get_ptr(p->pc, "PlayerState");
        if (ps) admin_ps_name(ps, a, sizeof a);
        LOG("wlooks: told %s the weapon looks on the floor (%d)", a, sent);
    }
    for (int i = 0; i < MAX_RECS; i++) {
        FloorRec *r = &recs[i];
        if (!r->pk) continue;
        int gone = !alive(r->pk, r->pi) || pickup_looted(r->pk);
        if (gone || models_off()) {   // picked up / gone: clients still waiting for it forget it
            if (r->sent && !models_off()) {
                char b[160];
                rec_line(r, "gone", b, sizeof b);
                for (int k = 0; k < np; k++) admin_data_to(pcs[k], b);
            }
            memset(r, 0, sizeof *r);
            continue;
        }
        float cur[3];
        if (!actor_loc(r->pk, cur)) continue;
        if (r->sent) {   // it moved after it was told (seen: lay still 0.5 s, then fell 1.4 m): told again where it is
            if (dist2(cur, r->loc) < 30.f * 30.f) continue;
            char b[160];
            rec_line(r, "gone", b, sizeof b);
            for (int k = 0; k < np; k++) admin_data_to(pcs[k], b);
            memcpy(r->loc, cur, sizeof cur);
            r->sent = 0; r->still = 0; r->age = 0;
            continue;
        }
        r->still = dist2(cur, r->loc) < 2.f * 2.f ? r->still + 1 : 0;
        memcpy(r->loc, cur, sizeof cur);
        if (r->still < 2 && (r->age += step) < 5) continue;
        r->sent = 1;
        char b[160];
        rec_line(r, "floor", b, sizeof b);
        for (int k = 0; k < np; k++) admin_data_to(pcs[k], b);
        n_sent++;
        LOG("wlooks: told %d player(s): %s", np, b);
    }
}

// ---- client: the host's lines ----
static int valid_name(const char *nm) {   // as wrow_name
    size_t l = strlen(nm);
    if (!l || l > 32 || !islower((unsigned char)*nm)) return 0;
    for (const char *c = nm; *c; c++) if (!islower((unsigned char)*c) && !isdigit((unsigned char)*c) && *c != '_') return 0;
    return 1;
}
void wlooks_host_data(const char *text) {
    if (strncmp(text, "wlook ", 6)) return;
    host_speaks = 1; n_host_lines++;
    char name[40] = "", code[20] = "";
    int x, y, z;
    int gone = !strncmp(text, "wlook gone", 10);
    if (sscanf(text + (gone ? 10 : 11), " %39s %19s %d %d %d", name, code, &x, &y, &z) != 5 || (!gone && strncmp(text, "wlook floor", 11)))
        return;
    if (!valid_name(name) || strlen(code) > 15 || models_off()) return;
    Look *l = look_by_name(name);
    if (!l || _stricmp(l->code, code)) return;   // not a look this machine has
    float at[3] = {(float)x, (float)y, (float)z};
    for (int i = 0; i < MAX_PENDING; i++)   // the same line twice (sent as it landed and again on joining)
        if (pending[i].on && !strcmp(pending[i].name, name) && dist2(pending[i].loc, at) < 10.f * 10.f) {
            if (gone) memset(&pending[i], 0, sizeof pending[i]);
            return;
        }
    if (gone) return;
    for (int i = 0; i < MAX_SWAPS; i++) {   // ... or already on its pickup
        float a[3];
        if (swaps[i].floor && swaps[i].conf && !strcmp(swaps[i].look, name) && alive(swaps[i].item, swaps[i].ii) &&
            actor_loc(swaps[i].item, a) && dist2(a, at) < 100.f * 100.f) return;
    }
    Pending *p = NULL;
    for (int i = 0; i < MAX_PENDING && !p; i++) if (!pending[i].on) p = &pending[i];
    if (!p) { p = &pending[0]; for (int i = 1; i < MAX_PENDING; i++) if (pending[i].age > p->age) p = &pending[i]; }
    memset(p, 0, sizeof *p);
    p->on = 1; p->world = ue_world();
    snprintf(p->name, sizeof p->name, "%s", name); snprintf(p->code, sizeof p->code, "%s", l->code);
    memcpy(p->loc, at, sizeof at);
}
static float client_acc;
// Every 0.25 s on a client: the host's lines onto the pickups lying there; guesses the host didn't confirm go back
static void tick_client(float dt) {
    UObject *w = ue_world(), *nd = w ? ue_get_ptr(w, "NetDriver") : NULL, *conn = nd ? ue_get_ptr(nd, "ServerConnection") : NULL;
    if (conn && conn != host_conn) { host_conn = conn; host_speaks = 0; }   // another host: not known to send lines yet
    if ((client_acc += dt) < 0.25f) return;
    float step = client_acc;
    client_acc = 0;
    int any = 0;
    for (int i = 0; i < MAX_PENDING; i++) {
        Pending *p = &pending[i];
        if (!p->on) continue;
        if (p->world != w || (p->age += step) > 600 || models_off()) { memset(p, 0, sizeof *p); continue; }
        any = 1;
    }
    if (any) {
        if (!c_pickup) c_pickup = ue_find_class("ItemPickup");
        UObject *best[MAX_PENDING] = {0};
        float bd[MAX_PENDING];
        for (int k = 0; k < MAX_PENDING; k++) bd[k] = 100.f * 100.f;
        for (int32_t i = 0, n = c_pickup ? ue_num_objects() : 0; i < n; i++) {
            UObject *o = ue_object_at(i);
            if (!is_live(o) || !ue_is_a(o, c_pickup)) continue;
            Swap *f = floor_of(o);
            if ((f && f->conf) || pickup_looted(o)) continue;   // the host named it already / picked up
            int have = 0, ctx = -2;
            float loc[3];
            for (int k = 0; k < MAX_PENDING; k++) {
                Pending *p = &pending[k];
                if (!p->on || !pickup_code(o, p->code)) continue;
                if (ctx == -2) ctx = pickup_ctx(o);
                if (ctx != 0 && ctx != 3) break;   // world loot
                if (!have && !(have = actor_loc(o, loc))) break;
                float q = dist2(loc, p->loc);
                if (q < bd[k]) { bd[k] = q; best[k] = o; }
            }
        }
        for (int k = 0; k < MAX_PENDING; k++) {
            Pending *p = &pending[k];
            if (!p->on || !best[k]) continue;
            Swap *f = floor_of(best[k]);
            if (f && f->conf) continue;   // two lines, one pickup: the first wins, the other waits for its own
            Look *l = look_by_name(p->name);
            int n = l ? floor_apply(best[k], l, 1) : 0;
            char b[96];
            LOG("wlooks: %s on the floor: %s (the host's, %.0f cm off, %d mesh(es))", p->name, ue_obj_name(best[k], b, sizeof b),
                sqrtf(bd[k]), n);
            if (n) n_floor++;
            // the guess for this drop may sit on another pickup nearby (two heroes dropping the same type at once)
            for (int i = 0; i < MAX_SWAPS; i++) {
                Swap *s = &swaps[i];
                if (!s->floor || s->conf || s->item == best[k] || strcmp(s->look, p->name) || !alive(s->item, s->ii)) continue;
                float a[3], c[3];
                int other = 0;   // another line of the same look (e.g. both dropped theirs) is for this guess: it stays
                for (int j = 0; j < MAX_PENDING && !other; j++)
                    other = j != k && pending[j].on && !strcmp(pending[j].name, p->name) && actor_loc(s->item, a) &&
                            dist2(a, pending[j].loc) < 100.f * 100.f;
                if (!other && actor_loc(s->item, a) && actor_loc(best[k], c) && dist2(a, c) < 500.f * 500.f) {
                    LOG("wlooks: guess %s taken back (the host named another pickup)", ue_obj_name(s->item, b, sizeof b));
                    floor_put_back(s->item);
                }
            }
            memset(p, 0, sizeof *p);
        }
    }
    if (!host_speaks) return;   // an older host: guesses stay
    for (int i = 0; i < MAX_SWAPS; i++) {
        Swap *s = &swaps[i];
        if (!s->floor || s->conf || now - s->t < 10 || !alive(s->item, s->ii)) continue;
        char b[96];
        LOG("wlooks: guess %s not confirmed by the host, taken back", ue_obj_name(s->item, b, sizeof b));
        floor_put_back(s->item);
    }
}
// Every second: the floor looks stay on (a pickup's mesh set again by the game gets ours again)
static void tick_floor(void) {
    if (models_off()) return;   // restore_stale puts them back
    for (int i = 0; i < MAX_SWAPS; i++) {
        Swap *s = &swaps[i];
        if (!s->floor || !alive(s->item, s->ii) || !alive(s->comp, s->ci)) continue;
        if (pickup_looted(s->item)) { put_back(s); continue; }   // picked up: clean if the game uses it again
        Look *l = look_by_name(s->look);
        UObject *m = l ? look_mesh(l, M_SM) : NULL;
        if (m && comp_mesh(s->comp) != m) {
            char cn[128];
            UObject *cur = comp_mesh(s->comp);
            const char *base = strrchr(l->path[M_SM], '.');
            if (cur && base && !_stricmp(ue_obj_name(cur, cn, sizeof cn), base + 1)) set_comp_mesh(s->comp, m, 1);
        }
    }
}

// ApplyCustomization hook: re-apply right after the game (skin change, first-person mesh init, OnRep)
typedef void (*ApplyFn)(UObject *mm);
static ApplyFn orig_apply;
static void apply_detour(UObject *mm) {
    UObject *it = U_OUTER(mm);
    if (is_live(it)) restore_stale(it);
    orig_apply(mm);
    RowHandle *r = row_of(mm);
    if (r && is_ours(r->row) && (n_looks > 0 || !is_client())) {
        UObject *item = U_OUTER(mm);
        if (is_live(item)) check_item(item, NULL);
    }
}

// ---- this player's choices ----
typedef struct {
    int on; char name[33], code[16];
    UObject *item; int32_t ii;   // the weapon we last set (retry counters / previous row per weapon)
    RowHandle prev;              // its row before ours (its skin), sent back by /model reset
    int tries, gave_up; float wait;
} Wish;
#define MAX_WISH 8
static Wish wish[MAX_WISH];

static UObject *my_pawn(void) { UObject *pc = ue_local_pc(); return pc ? ue_get_ptr(pc, "Pawn") : NULL; }
static int send_row(UObject *mm, UObject *table, FName row) {
    UFunction *f = fn_of(mm, "ServerCustomizationRow");
    int32_t po = parm_off(f, "ClientCustomizationRow");
    if (!f || po < 0 || UFN_PARMSSIZE(f) > 64) return -1;
    uint8_t p[64] = {0};
    RowHandle *h = (RowHandle *)(p + po);
    h->table = table; h->row = row;
    ue_process_event(mm, f, p);   // host: runs here; client: to the host (our weapon, our connection)
    // The row replicates to everyone but the owner (seen live): a client sets its own copy, like the game does
    RowHandle *r = is_client() ? row_of(mm) : NULL;
    UFunction *rep = r ? fn_of(mm, "OnRep_CustomizationRow") : NULL;
    if (r) {
        r->table = table; r->row = row;
        uint8_t q[16] = {0};
        if (rep) ue_process_event(mm, rep, q);   // applies it (when the weapon's meshes are set up)
    }
    return 0;
}
static void tick_wishes(float dt) {
    UObject **it, *pawn = my_pawn();
    int n = items_of(pawn, &it);
    for (int w = 0; w < MAX_WISH; w++) {
        Wish *x = &wish[w];
        if (!x->on) continue;
        for (int i = 0; i < n; i++) {
            UObject *item = it[i];
            if (!is_live(item) || !item_code(item, x->code)) continue;
            UObject *mm = mm_of(item);
            RowHandle *r = row_of(mm);
            if (!r) continue;
            char nm[40];
            if (wrow_name(r->row, nm, sizeof nm) && !strcmp(nm, x->name)) { x->tries = 0; continue; }
            if (x->item != item || !alive(item, x->ii)) {   // a new weapon (pickup, respawn, new map)
                x->item = item; x->ii = U_INDEX(item); x->tries = 0; x->gave_up = 0; x->wait = 1;
                memset(&x->prev, 0, sizeof x->prev);
                if (!is_ours(r->row)) { x->prev.table = r->table; x->prev.row = r->row; }
            }
            if (x->gave_up || (x->wait -= dt) > 0) continue;
            if (x->tries >= 10) { x->gave_up = 1; LOG("wlooks: %s not applied after %d tries, giving up for this weapon", x->name, x->tries); continue; }
            if (!is_ours(r->row) && !(r->table == x->prev.table && fname_eq(r->row, x->prev.row))) { x->prev.table = r->table; x->prev.row = r->row; }
            char row[80];
            snprintf(row, sizeof row, WROW_PREFIX "%s", x->name);
            UObject *t = cust_table(item);
            if (send_row(mm, t ? t : r->table, make_name(row))) continue;
            x->tries++; x->wait = x->tries < 3 ? 3 : 10;
            char b[96];
            LOG("wlooks: sent %s for %s (try %d)", x->name, ue_obj_name(item, b, sizeof b), x->tries);
        }
    }
}

static float acc;
void wlooks_tick(float dt) {
    if (n_looks < 0) build_looks();
    now += dt;
    int host = !is_client();
    if (n_looks > 0 || host) { tick_carried(); tick_drops(dt); }
    if (host) tick_host(dt); else tick_client(dt);
    int any = 0;
    for (int w = 0; w < MAX_WISH; w++) any |= wish[w].on;
    if (any) tick_wishes(dt);
    if ((acc += dt) < 1.0f) return;   // safety net; the ApplyCustomization hook does it at once
    acc = 0;
    restore_stale(NULL);
    if (!n_looks && !host) return;
    tick_floor();
    UObject *p[32];
    int np = models_hero_pawns(p, 32);
    for (int k = 0; k < np; k++) {
        UObject **it;
        int n = items_of(p[k], &it);
        for (int i = 0; i < n; i++) if (is_live(it[i])) check_item(it[i], p[k]);
    }
}

// ---- host: who may use them ----
typedef void (*SetRowFn)(UObject *mm, RowHandle *row);
static SetRowFn orig_setrow;
static UObject *owner_pc(UObject *mm, UObject **pawn_out) {   // component -> item -> owner pawn -> controller
    UObject *item = U_OUTER(mm), *pawn = item ? ue_get_ptr(item, "Owner") : NULL;
    static UClass *pcc;
    if (!pcc) pcc = ue_find_class("PlayerController");
    if (pawn_out) *pawn_out = pawn;
    for (int d = 0; pawn && d < 3; d++) {   // owner chains: pawn, or controller directly
        if (pcc && ue_is_a(pawn, pcc)) return pawn;
        UObject *c = ue_get_ptr(pawn, "Controller");
        if (c && pcc && ue_is_a(c, pcc)) return c;
        pawn = ue_get_ptr(pawn, "Owner");
    }
    return NULL;
}
static void setrow_detour(UObject *mm, RowHandle *row) {
    char nm[40], rn[96];
    if (row && is_ours(row->row)) {
        UObject *pawn, *pc = owner_pc(mm, &pawn);
        int mine = pc && pc == ue_local_pc();
        const char *why = NULL, *tell = REFUSED " (/models).";
        if (!wrow_name(row->row, nm, sizeof nm)) why = "not a weapon look";
        else if (!mine && models_locked()) why = "models are off";
        else if (!mine && !strcmp(addons_policy_name(), "none"))
            why = "addons_policy=none", tell = REFUSED " (addons_policy=none).";
        UObject *ps = pc ? ue_get_ptr(pc, "PlayerState") : NULL;
        char who[64] = "?";
        if (ps) admin_ps_name(ps, who, sizeof who);
        if (why) {
            LOG("wlooks: refused %s from %s: %s", ue_name(row->row, rn, sizeof rn), who, why);
            static ULONGLONG last; static UObject *last_pc;
            if (pc && !mine && (pc != last_pc || GetTickCount64() - last > 3000)) { admin_notice_to(pc, tell); last_pc = pc; last = GetTickCount64(); }
            return;
        }
        char b[96];
        LOG("wlooks: %s puts %s on %s", who, nm, ue_obj_name(U_OUTER(mm), b, sizeof b));
    }
    orig_setrow(mm, row);
}

// /models off: every weapon of the hero team with a look of ours back to no skin (the owner's skin comes back with
// their next weapon); clients' agents put the retail meshes back when the row replicates.
int wlooks_lock_reset(void) {
    if (!orig_setrow || is_client()) return 0;
    UObject *p[32];
    int np = models_hero_pawns(p, 32), done = 0;
    for (int k = 0; k < np; k++) {
        UObject **it;
        int n = items_of(p[k], &it);
        for (int i = 0; i < n; i++) {
            UObject *mm = is_live(it[i]) ? mm_of(it[i]) : NULL;
            RowHandle *r = row_of(mm);
            if (!r || !wrow_name(r->row, NULL, 0)) continue;
            RowHandle none = {0};
            none.table = r->table;
            orig_setrow(mm, &none);
            done++;
            // the row doesn't replicate to the weapon's owner: tell their agent, which puts its own skin back
            UObject *pc = owner_pc(mm, NULL);
            if (pc && pc != ue_local_pc()) admin_notice_to(pc, REFUSED " (/models).");
        }
    }
    for (int w = 0; w < MAX_WISH; w++) wish[w].on = 0;
    restore_stale(NULL);
    if (done) LOG("wlooks: /models off, %d weapon look(s) reset", done);
    return done;
}

void wlooks_host_notice(const char *text) {
    if (strncmp(text, REFUSED, sizeof REFUSED - 1)) return;
    LOG("wlooks: refused by the host");
    wlooks_reset(NULL);   // our own copy of the row (the host's reset doesn't reach the owner): back to our skins
}

// ---- /model ----
int wlooks_pick(const char *name, Out *o) {
    Look *l = look_by_name(name);
    if (!l) return 0;
    int k = 0;
    for (int w = 0; w < MAX_WISH; w++) if (wish[w].on && !_stricmp(wish[w].code, l->code)) { k = w; goto have; }
    for (k = 0; k < MAX_WISH && wish[k].on; k++) {}
    if (k == MAX_WISH) { out_printf(o, "too many weapon looks (/model reset)\n"); return 1; }
    memset(&wish[k], 0, sizeof wish[k]);
have:
    wish[k].on = 1; wish[k].tries = 0; wish[k].gave_up = 0; wish[k].wait = 0;
    snprintf(wish[k].name, sizeof wish[k].name, "%s", l->name);
    snprintf(wish[k].code, sizeof wish[k].code, "%s", l->code);
    out_printf(o, "your %s now looks like %s (/model reset to undo; players without that add-on see the %s)\n",
               l->code, l->title, l->code);
    LOG("wlooks: /model %s (%s)", l->name, l->code);
    return 1;
}

// code NULL: every weapon type (/model reset); else only that weapon type (the ~ window's Wear, per weapon)
void wlooks_reset_code(const char *code, Out *o) {
    UObject **it;
    int n = items_of(my_pawn(), &it), k = 0;
    for (int w = 0; w < MAX_WISH; w++) {
        Wish *x = &wish[w];
        if (!x->on || (code && _stricmp(x->code, code))) continue;
        x->on = 0; k++;
        for (int i = 0; i < n; i++) {
            if (!is_live(it[i]) || !item_code(it[i], x->code)) continue;
            UObject *mm = mm_of(it[i]);
            RowHandle *r = row_of(mm);
            if (!r || !is_ours(r->row)) continue;
            int same = x->item == it[i] && alive(it[i], x->ii);
            send_row(mm, same && x->prev.table ? x->prev.table : r->table, same ? x->prev.row : (FName){0, 0});
        }
    }
    if (k && o) {
        if (code) out_printf(o, "your %s back to your own skin\n", code);
        else out_printf(o, "weapons back to your own skins\n");
    }
    if (!k && o && code) out_printf(o, "your %s has no add-on look\n", code);
}
void wlooks_reset(Out *o) { wlooks_reset_code(NULL, o); }

void wlooks_list(Out *o) {
    if (n_looks < 0) build_looks();
    if (!n_looks) { out_printf(o, "no add-on weapon looks (add-ons with weapons: /addons)\n"); return; }
    out_printf(o, "add-on weapon looks (seen by players who have the add-on; others see the normal weapon):\n");
    for (int i = 0; i < n_looks; i++)
        out_printf(o, " %s: %s for the %s%s%s%s\n", looks[i].name, looks[i].title, looks[i].code,
                   strcmp(looks[i].addon, looks[i].title) ? " (" : "", strcmp(looks[i].addon, looks[i].title) ? looks[i].addon : "",
                   strcmp(looks[i].addon, looks[i].title) ? ")" : "");
}
void wlooks_overview(Out *o) {
    if (n_looks < 0) build_looks();
    if (!n_looks) return;
    char line[200];
    size_t k = snprintf(line, sizeof line, "add-on weapon looks (/model list weapons):");
    for (int i = 0; i < n_looks; i++) {
        if (k + strlen(looks[i].name) + 2 > 90) { out_printf(o, "%s\n", line); k = snprintf(line, sizeof line, " "); }
        k += snprintf(line + k, sizeof line - k, " %s", looks[i].name);
    }
    out_printf(o, "%s\n", line);
}
void wlooks_status(Out *o) {
    for (int w = 0; w < MAX_WISH; w++)
        if (wish[w].on) out_printf(o, "your %s: %s%s\n", wish[w].code, wish[w].name, wish[w].gave_up ? " (refused by the host)" : "");
}

// ~ window, "Looks & add-ons" tab, Wear (models.c draws it): the add-ons' weapon looks by weapon type, through /model.
// search: only looks whose name, title, add-on or weapon code has it; hdr: a section heading for a search across every
// list (drawn only when something matches, no "nothing" lines). Returns the number of looks shown.
static int carries(const char *code) {   // this player's hero holds a weapon of that type now
    UObject **it;
    int n = items_of(my_pawn(), &it);
    for (int i = 0; i < n; i++) if (is_live(it[i]) && item_code(it[i], code)) return 1;
    return 0;
}
int wlooks_count(void) {
    if (n_looks < 0) build_looks();
    return n_looks;
}
static int look_match(const Look *l, const char *s) {
    const char *f[4] = {l->name, l->title, l->addon, l->code};
    size_t n = strlen(s);
    if (!n) return 1;
    for (int k = 0; k < 4; k++)
        for (const char *h = f[k]; *h; h++) if (!_strnicmp(h, s, n)) return 1;
    return 0;
}
int wlooks_panel(int blocked, const char *why, const char *search, const char *hdr) {
    if (n_looks < 0) build_looks();
    if (!n_looks) {
        if (!hdr) ov_text_dim("No add-on weapon looks yet: Get more (above) lists free ones; mod makers: b4bmod weapon --as <name>.");
        return 0;
    }
    int done[MAX_LOOKS] = {0}, shown = 0;
    for (int i = 0; i < n_looks; i++) {
        if (done[i]) continue;
        const char *code = looks[i].code;
        int any = 0;
        for (int j = i; j < n_looks; j++) if (!_stricmp(looks[j].code, code) && look_match(&looks[j], search)) any++;
        if (!any) { for (int j = i; j < n_looks; j++) if (!_stricmp(looks[j].code, code)) done[j] = 1; continue; }
        if (!shown) {
            if (hdr) ov_heading(hdr);
            else ov_text_dim("Your weapon of that type shows the add-on's model, also every one you pick up later. Players "
                             "without the add-on see the normal weapon.");
        }
        shown += any;
        Wish *mine = NULL;
        for (int w = 0; w < MAX_WISH; w++) if (wish[w].on && !_stricmp(wish[w].code, code)) mine = &wish[w];
        ov_push_id(i);
        ov_text("%s", code);
        ov_same_line();
        if (mine && mine->gave_up) ov_text_warn("%s: refused by the host", mine->name);
        else if (mine) ov_text_dim("yours: %s%s", mine->name, carries(code) ? "" : " (applies when you carry one)");
        else ov_text_dim("your own skin");
        ov_same_line();
        ov_begin_disabled(!mine, "No add-on look on this weapon type.");
        char rl[40];
        snprintf(rl, sizeof rl, "Reset##w:%s", code);
        if (ov_button(rl)) {
            static Out o;
            out_reset(&o);
            wlooks_reset_code(code, &o);
            if (o.len) overlay_note(o.buf);
        }
        ov_tooltip("This weapon type back to your own skin (/model reset does all weapons and your survivor).");
        ov_end_disabled();
        if (ov_table_begin("wl", 3)) {
            for (int j = i; j < n_looks; j++) {
                if (_stricmp(looks[j].code, code)) continue;
                done[j] = 1;
                if (!look_match(&looks[j], search)) continue;
                ov_push_id(j);
                ov_table_next();
                ov_begin_disabled(blocked, why);
                char lab[48];
                snprintf(lab, sizeof lab, "Use##%s", looks[j].name);
                if (ov_button(lab)) ov_run("model %s", looks[j].name);
                ov_tooltip("/model <name>");
                ov_end_disabled();
                ov_same_line();
                ov_text("%s%s", looks[j].name, mine && !strcmp(mine->name, looks[j].name) ? "  (yours)" : "");
                ov_table_next(); ov_text("%s", looks[j].title);
                ov_table_next(); ov_text_dim("add-on: %s", looks[j].addon);
                ov_pop_id();
            }
            ov_table_end();
        }
        ov_pop_id();
    }
    if (!shown && !hdr) ov_text_dim("Nothing matches.");
    return shown;
}

int wlooks_init(void) {
    if (memcmp((void *)ADDR_APPLY, SIG_APPLY, sizeof SIG_APPLY) || memcmp((void *)ADDR_SETROW, SIG_SETROW, sizeof SIG_SETROW)) {
        LOG("wlooks: signature mismatch, no weapon looks");
        return -1;
    }
    if (MH_CreateHook((void *)ADDR_APPLY, (void *)apply_detour, (void **)&orig_apply) != MH_OK ||
        MH_EnableHook((void *)ADDR_APPLY) != MH_OK) { orig_apply = NULL; LOG("wlooks: apply hook failed"); return -1; }
    if (MH_CreateHook((void *)ADDR_SETROW, (void *)setrow_detour, (void **)&orig_setrow) != MH_OK ||
        MH_EnableHook((void *)ADDR_SETROW) != MH_OK) { orig_setrow = NULL; LOG("wlooks: row hook failed (no host rules)"); }
    return 0;
}

#ifndef B4B_RELEASE
// wlook dump: every hero's weapons, their skin row and mesh components
static UObject *my_inv(void) { UObject *p = my_pawn(); return p ? ue_get_ptr(p, "Inventory") : NULL; }
int wlooks_cmd(const char *verb, char *rest, Out *o) {
    (void)verb;
    classes();
    char *sub = rest ? strtok(rest, " ") : NULL, *arg = sub ? strtok(NULL, " ") : NULL;
    UObject *inv = sub ? my_inv() : NULL;
    if (sub && !strcmp(sub, "swap")) {   // the quick-swap key (last weapon)
        UFunction *f = fn_of(inv, "OnInputWeaponQuickSwap");
        uint8_t p[16] = {0};
        if (f) ue_process_event(inv, f, p);
        out_printf(o, "quick swap: %s\n", f ? "done" : "no inventory");
        return 1;
    }
    if (sub && !strcmp(sub, "select") && arg) {   // SelectEquipmentSlot(slot, false): 0 primary, 1 secondary
        UFunction *f = fn_of(inv, "SelectEquipmentSlot");
        int32_t ps = parm_off(f, "EquipmentSlot"), pr = parm_off(f, "bInRequest");
        uint8_t p[16] = {0};
        if (f && ps >= 0) { p[ps] = (uint8_t)atoi(arg); if (pr >= 0) p[pr] = 0; ue_process_event(inv, f, p); }
        out_printf(o, "select slot %s: %s\n", arg, f ? "done" : "no inventory");
        return 1;
    }
    if (sub && !strcmp(sub, "row") && arg) {   // row <item#> <row name|none>: ServerCustomizationRow on my own weapon
        UObject **it; int n = items_of(my_pawn(), &it), k = atoi(arg);
        char *rn = strtok(NULL, " ");
        UObject *mm = k >= 0 && k < n && it[k] ? mm_of(it[k]) : NULL;
        if (!mm || !rn) { out_printf(o, "usage: wlook row <item#> <row|none>\n"); return 1; }
        FName nm = {0};
        if (_stricmp(rn, "none")) nm = make_name(rn);
        UObject *t = cust_table(it[k]);
        out_printf(o, "ServerCustomizationRow: %d\n", send_row(mm, t ? t : row_of(mm)->table, nm));
        return 1;
    }
    if (sub && !strcmp(sub, "drop") && arg) {   // ServerDropItem(the item in EquipmentSlots[n], manually dropped)
        UObject **it; int n = items_of(my_pawn(), &it), k = atoi(arg);
        UFunction *f = fn_of(inv, "ServerDropItem");
        int32_t pi = parm_off(f, "Item"), pm = parm_off(f, "bManuallyDropped"), pf = parm_off(f, "bForce");
        if (!f || pi < 0 || k < 0 || k >= n || !it[k] || UFN_PARMSSIZE(f) > 64) { out_printf(o, "no item %d\n", k); return 1; }
        uint8_t p[64] = {0};
        *(UObject **)(p + pi) = it[k];
        if (pm >= 0) p[pm] = 1;
        if (pf >= 0) p[pf] = 1;
        ue_process_event(inv, f, p);
        out_printf(o, "dropped item %d\n", k);
        return 1;
    }
    if (sub && !strcmp(sub, "drophero") && arg) {   // drophero <item#> <hero#>...: host, those heroes drop in one frame
        int k = atoi(arg), done = 0;
        UObject *p[32];
        int np = models_hero_pawns(p, 32);
        for (char *h = strtok(NULL, " "); h; h = strtok(NULL, " ")) {
            int hi = atoi(h);
            UObject **it, *hinv = hi >= 0 && hi < np ? ue_get_ptr(p[hi], "Inventory") : NULL;
            int n = hi >= 0 && hi < np ? items_of(p[hi], &it) : 0;
            UFunction *f = fn_of(hinv, "ServerDropItem");
            int32_t pi = parm_off(f, "Item"), pm = parm_off(f, "bManuallyDropped"), pf = parm_off(f, "bForce");
            if (!f || pi < 0 || k < 0 || k >= n || !it[k] || UFN_PARMSSIZE(f) > 64) { out_printf(o, "hero %d: no item %d\n", hi, k); continue; }
            uint8_t q[64] = {0};
            *(UObject **)(q + pi) = it[k];
            if (pm >= 0) q[pm] = 1;
            if (pf >= 0) q[pf] = 1;
            ue_process_event(hinv, f, q);
            done++;
        }
        out_printf(o, "%d hero(es) dropped item %d in this frame\n", done, k);
        return 1;
    }
    if (sub && !strcmp(sub, "guess") && arg) {   // guess <x> <y> <look>: this client's guess on the pickup nearest x,y (tests)
        char *ys = strtok(NULL, " "), *ln = ys ? strtok(NULL, " ") : NULL;
        Look *l = ln ? look_by_name(ln) : NULL;
        float at[3] = {(float)atof(arg), ys ? (float)atof(ys) : 0, 0}, bd = 1e30f;
        UObject *best = NULL;
        if (!c_pickup) c_pickup = ue_find_class("ItemPickup");
        for (int32_t i = 0, n = l && c_pickup ? ue_num_objects() : 0; i < n; i++) {
            UObject *x = ue_object_at(i);
            float q[3];
            if (!is_live(x) || !ue_is_a(x, c_pickup) || !pickup_code(x, l->code) || !actor_loc(x, q)) continue;
            q[2] = 0;
            if (dist2(q, at) < bd) { bd = dist2(q, at); best = x; }
        }
        char a[96];
        if (!best) { out_printf(o, "usage: wlook guess <x> <y> <look> (a pickup of that look's weapon type)\n"); return 1; }
        out_printf(o, "guess %s on %s: %d mesh(es)\n", l->name, ue_obj_name(best, a, sizeof a), floor_apply(best, l, 0));
        return 1;
    }
    if (sub && !strcmp(sub, "use")) {   // use [mesh substr]: press Use on the nearest weapon pickup (ForcePressUse)
        UObject *pawn = my_pawn(), *best = NULL;
        float me[3], bd = 1e30f;
        if (!c_pickup) c_pickup = ue_find_class("ItemPickup");
        if (!pawn || !actor_loc(pawn, me) || !c_pickup) { out_printf(o, "no pawn\n"); return 1; }
        for (int32_t i = 0, n = ue_num_objects(); i < n; i++) {
            UObject *x = ue_object_at(i);
            char m[96];
            float l[3];
            if (!is_live(x) || !ue_is_a(x, c_pickup) || !pickup_mesh(x, m, sizeof m) || (arg && !strstr(m, arg))) continue;
            if (actor_loc(x, l) && dist2(l, me) < bd) { bd = dist2(l, me); best = x; }
        }
        UFunction *g = fn_of(pawn, "GetHeroUseComponent");
        uint8_t q[16] = {0};
        if (g) ue_process_event(pawn, g, q);
        UObject *huc = *(UObject **)q, *uc = best ? ue_get_ptr(best, "UsableComponent") : NULL;
        UFunction *f = fn_of(huc, "ForcePressUse");
        int32_t pa = parm_off(f, "Actor"), pu = parm_off(f, "UsableComponent");
        if (!best || !uc || !f || pa < 0 || pu < 0 || UFN_PARMSSIZE(f) > 32) { out_printf(o, "no pickup / use component\n"); return 1; }
        uint8_t p[32] = {0};
        *(UObject **)(p + pa) = best; *(UObject **)(p + pu) = uc;
        ue_process_event(huc, f, p);
        char a[96];
        out_printf(o, "ForcePressUse %s (%.0f cm away)\n", ue_obj_name(best, a, sizeof a), sqrtf(bd));
        return 1;
    }
    if (sub && !strcmp(sub, "pickups")) {   // weapon pickups: mesh, who dropped it (Owner/Instigator/PreviousOwner), floor look
        if (!c_pickup) c_pickup = ue_find_class("ItemPickup");
        int k = 0;
        for (int32_t i = 0, n = ue_num_objects(); c_pickup && i < n; i++) {
            UObject *x = ue_object_at(i);
            if (!is_live(x) || !ue_is_a(x, c_pickup)) continue;
            char a[96], m[96], w[3][64];
            UObject *mesh = pickup_mesh(x, m, sizeof m), *who[3] = {ue_get_ptr(x, "Owner"), ue_get_ptr(x, "Instigator"), pickup_prev_owner(x)};
            if (!mesh || (arg && !strstr(m, arg))) continue;
            for (int j = 0; j < 3; j++) { if (who[j]) ue_obj_name(who[j], w[j], sizeof w[j]); else strcpy(w[j], "-"); }
            float loc[3] = {0};
            actor_loc(x, loc);
            Swap *f = floor_of(x);
            FloorRec *r = rec_of(x);
            out_printf(o, "%s mesh=%s ctx=%d owner=%s instigator=%s prev=%s at %.0f %.0f %.0f look=%s%s%s\n", ue_obj_name(x, a, sizeof a),
                       m, pickup_ctx(x), w[0], w[1], w[2], loc[0], loc[1], loc[2], f ? f->look : (r ? r->name : "-"),
                       f ? (f->conf ? " (host)" : " (guess)") : "", r ? (r->sent ? " told" : " not told yet") : "");
            if (++k >= 40) break;
        }
        int np = 0, nr = 0;
        for (int i = 0; i < MAX_PENDING; i++) np += pending[i].on;
        for (int i = 0; i < MAX_RECS; i++) nr += recs[i].pk && alive(recs[i].pk, recs[i].pi);
        out_printf(o, "%d pickup(s) shown, %d floor look(s) put on; host: %d dropped with a look, %d told; client: host sends "
                   "lines %s (%d received), %d waiting for their pickup\n", k, n_floor, nr, n_sent, host_speaks ? "yes" : "no",
                   n_host_lines, np);
        return 1;
    }
    UObject *p[32];
    int np = models_hero_pawns(p, 32);
    char a[128], b[300], c[128];
    for (int k = 0; k < np; k++) {
        UObject **it;
        int n = items_of(p[k], &it);
        out_printf(o, "#%d %s: %d item(s)\n", k, ue_obj_name(p[k], a, sizeof a), n);
        for (int i = 0; i < n; i++) {
            UObject *item = it[i];
            if (!is_live(item)) { out_printf(o, "  [%d] -\n", i); continue; }
            UObject *mm = mm_of(item), *t = cust_table(item), *own = ue_get_ptr(item, "Owner");
            RowHandle *r = row_of(mm);
            out_printf(o, "  [%d] %s class=%s owner=%s custtable=%s row=%s/%s\n", i, ue_obj_name(item, a, sizeof a),
                       ue_obj_name(U_CLASS(item), c, sizeof c), own ? ue_obj_name(own, b, 64) : "-",
                       t ? ue_obj_name(t, b + 64, 64) : "-", r && r->table ? ue_obj_name(r->table, b + 128, 64) : "-",
                       r ? ue_name(r->row, b + 192, 100) : "-");
            static const char *arrs[2] = {"FirstPersonMeshComponents", "ThirdPersonMeshComponents"};
            for (int q = 0; mm && q < 2; q++) {
                int32_t off = ue_prop_offset(mm, arrs[q]);
                TArray *arr = off >= 0 ? (TArray *)((char *)mm + off) : NULL;
                for (int j = 0; arr && j < arr->num; j++) {
                    UObject *x = ((UObject **)arr->data)[j];
                    if (!is_live(x)) continue;
                    int32_t om = ue_prop_offset(x, "OverrideMaterials");
                    UObject *m = (ue_is_a(x, c_skc) || ue_is_a(x, c_smc)) ? comp_mesh(x) : NULL;
                    out_printf(o, "     %s %s [%s] mesh=%s overrides=%d\n", q ? "3P" : "FP", ue_obj_name(x, a, sizeof a),
                               ue_obj_name(U_CLASS(x), c, sizeof c), m ? ue_full_path(m, b, sizeof b) : "-",
                               om >= 0 ? ((TArray *)((char *)x + om))->num : -1);
                }
            }
        }
    }
    out_printf(o, "looks: %d, applied %d time(s)\n", n_looks, n_applied);
    for (int w = 0; w < MAX_WISH; w++) if (wish[w].on) out_printf(o, "wish %s (%s) tries=%d gave_up=%d\n", wish[w].name, wish[w].code, wish[w].tries, wish[w].gave_up);
    return 1;
}
#else
int wlooks_cmd(const char *verb, char *rest, Out *o) { (void)verb; (void)rest; (void)o; return 0; }
#endif
