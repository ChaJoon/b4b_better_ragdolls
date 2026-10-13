// Runtime model swaps between assets the game ships (issue #19, Tier 0). Findings: docs/investigations/model-swap.md.
//
// How a survivor's look is built: FCharacterCustomizationSet = 4 FDataTableRowHandles (Head, Torso, Legs, Outfit) +
// LastEquipSlot, each a row of the hero's CharacterCustomizationTable (CharacterDefinitionRow +0x88), whose
// FCharacterCustomizationRow holds the first- and third-person HeroMeshDefinition (soft SkeletalMesh + material slot
// overrides). The set lives in the replicated APlayerSlot::CurrentCustomizationSet (+0x318). Its OnRep (0x141A123C0,
// also run by the server) hands it to the slot's HeroCharacter (0x141BE5E20), which async-loads the meshes and puts
// them on its body/head/legs/first-person components. Clients push their own set with the server RPC
// AGobiPlayerState::ServerSelectCustomizationSet, whose implementation copies it into their slot WITHOUT validation
// (_Validate returns true) and runs the OnRep. Rows are looked up by (table, row) only, so a row of ANOTHER hero's
// table works: that is a cross-survivor outfit swap, applied by every machine's own game code from the replicated
// slot. Everyone sees it, even players without b4bcoop. Nothing is written to the profile (the profile side is a
// separate EquipCharacterCustomizationSet command).
//
// What we do:
//   - /model <name> (anyone, for yourself): send ServerSelectCustomizationSet(current set + the chosen row(s)) for our
//     own player state (host: runs locally). The wish is kept for the session and re-sent whenever our slot's set no
//     longer matches it (new map, respawned slot, the game re-sending our profile set).
//   - /model <player> <name> (host): write that slot's set directly + run its OnRep (replicates to everyone); kept
//     and re-applied the same way, dropped when a human takes over a bot's slot.
//   - /model reset: the game's own re-init from the profile (ClientInitCustomizationRowForSelectedCharacter), or for a
//     bot its hero's default skin.
//   - /models off|on (host): hook on ServerSelectCustomizationSet_Implementation refuses sets with another survivor's
//     rows; turning it off resets every such slot.
//   - Added outfits (add-ons with `outfit=` lines, addons.c; docs/investigations/new-assets.md): a made-up outfit row
//     "b4bcoop.outfit.<name>" like the NPC bodies; every machine with that add-on puts its 3P mesh and FP arms on.
//   - Made-up rows on everything that wears a set (cutscene stand-ins, lineup mannequins, #37): applyslot_detour.
//   - The game's customization screen (#33): real rows of that name for the add-on outfits; the profile never stores
//     them (equip_detour / getprof_detour, b4bcoop-outfits.txt).
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

// AGobiPlayerState::ServerSelectCustomizationSet_Implementation (vtable +0x8c0)
#define ADDR_SELECTSET VA(0x141BD6DC0ull)
static const uint8_t SIG_SELECTSET[] = {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8d,0x99,0x4c,0x05,0x00,0x00,0x4c,0x8b,0xda,
                                        0x48,0x8b,0xcb};

// FDataTableRowHandle in this build: {UDataTable*, FName RowName, FString (unreflected display name)} = 0x20
typedef struct { UObject *table; FName row; FString display; } RowHandle;
typedef struct { RowHandle slot[4]; uint8_t last, pad[7]; } CustSet;   // FCharacterCustomizationSet (0x88)
enum { SLOT_HEAD, SLOT_TORSO, SLOT_LEGS, SLOT_OUTFIT };
static const char *SLOTN[] = {"head", "torso", "legs", "outfit"};

#define DT_ROWSTRUCT(t)   (*(UObject **)((char *)(t) + 0x30))
#define DT_ROWMAP(t)      ((void *)((char *)(t) + 0x38))
// CharacterDefinitionRow
#define CD_SLUG(r)        (*(FName *)((char *)(r) + 0x08))
#define CD_CUSTTABLE(r)   (*(UObject **)((char *)(r) + 0x88))
#define CD_DEFSKIN(r)     ((RowHandle *)((char *)(r) + 0x90))
// CharacterCustomizationRow
#define CC_SLOT(r)        (*(uint8_t *)((char *)(r) + 0x48))
#define CC_FP(r)          ((char *)(r) + 0x50)    // HeroMeshDefinition
#define CC_3P(r)          ((char *)(r) + 0x118)
#define HMD_MESHPATH(d)   (*(FName *)((char *)(d) + 0xa0 + 0x10))  // SoftObjectPtr -> FSoftObjectPath.AssetPathName
#define HMD_MATS(d)       ((TArray *)((char *)(d) + 0x00))        // TMap<FName, TSoftObjectPtr<MaterialInterface>>

static int fname_eq(FName a, FName b) { return a.idx == b.idx && a.num == b.num; }
static FName make_name(const char *s) {
    wchar_t w[512]; int k = 0;
    for (; s[k] && k < 511; k++) w[k] = (wchar_t)(unsigned char)s[k];
    w[k] = 0;
    FName n = {0};
    ((FNameCtorFn)ADDR_FNAME_CTOR)(&n, w, 1);
    return n;
}
static void *sparse_at(const void *set, int i, int stride) {
    const TArray *a = set;
    if (!a->data || i < 0 || i >= a->num || i >= *(const int32_t *)((const char *)set + 0x28)) return NULL;
    const uint32_t *bits = *(uint32_t *const *)((const char *)set + 0x20);
    if (!bits) bits = (const uint32_t *)((const char *)set + 0x10);
    return (bits[i >> 5] >> (i & 31)) & 1 ? (char *)a->data + (size_t)i * stride : NULL;
}
static uint8_t *dt_row(UObject *table, FName row) {
    if (!table) return NULL;
    TArray *m = DT_ROWMAP(table);
    for (int i = 0; i < m->num; i++) {
        char *e = sparse_at(m, i, 0x18);
        if (e && fname_eq(*(FName *)e, row)) return *(uint8_t **)(e + 8);
    }
    return NULL;
}
static int is_rowstruct(UObject *t, const char *name) {
    char b[128];
    UObject *rs = t ? DT_ROWSTRUCT(t) : NULL;
    return rs && !strcmp(ue_obj_name(rs, b, sizeof b), name);
}
static int is_live(UObject *o) { return o && !(U_FLAGS(o) & 0x30); }
static int is_ours(FName row) { char b[16]; return !strncmp(ue_name(row, b, sizeof b), "b4bcoop.", 8); }   // b4bcoop.npc/outfit.*
static UFunction *fn_of(UObject *o, const char *name) { return o ? ue_find_function(U_CLASS(o), name) : NULL; }
static int32_t parm_off(UFunction *f, const char *p) { FField *x = f ? ue_find_prop(f, p) : NULL; return x ? FP_OFFSET(x) : -1; }

// ---- catalogue: every row of every hero's customization table (loaded with the hero definitions) ----
typedef struct {
    char name[48];       // friendly: third-person mesh name without 3P_/_SKM, lower case ("holly_elite_04")
    int hero;            // index into heroes[]
    UObject *table;      // customization table
    FName row;
    int slot;
    char mesh3p[160];
} Entry;
#define MAX_ENTRIES 768
static Entry cat[MAX_ENTRIES];
static int n_cat;
typedef struct { char slug[32]; UObject *deftable, *custtable; FName defrow; RowHandle defskin; int first, count; } Hero;
static Hero heroes[32];
static int n_heroes;
static UObject *cat_world;

static void friendly(const char *path, char *out, size_t n) {
    const char *b = strrchr(path, '.');
    b = b ? b + 1 : path;
    if (!_strnicmp(b, "3P_", 3)) b += 3;
    size_t k = 0;
    for (; *b && k + 1 < n; b++) out[k++] = (char)tolower((unsigned char)*b);
    out[k] = 0;
    if (k > 4 && !strcmp(out + k - 4, "_skm")) out[k - 4] = 0;
}

static void build_catalogue(void) {
    n_cat = n_heroes = 0;
    cat_world = ue_world();
    int32_t n = ue_num_objects();
    UClass *dtc = ue_find_class("DataTable");
    char b[64];
    for (int32_t i = 0; dtc && i < n; i++) {
        UObject *t = ue_object_at(i);
        if (!is_live(t) || !ue_is_a(t, dtc) || !is_rowstruct(t, "CharacterDefinitionRow")) continue;
        TArray *m = DT_ROWMAP(t);
        for (int k = 0; k < m->num && n_heroes < 32; k++) {
            char *e = sparse_at(m, k, 0x18);
            uint8_t *r = e ? *(uint8_t **)(e + 8) : NULL;
            if (!r) continue;
            Hero *h = &heroes[n_heroes];
            memset(h, 0, sizeof *h);
            // slugs are hero_1..hero_12: name the survivor after its table (Holly_Customization_DT -> holly)
            if (CD_CUSTTABLE(r)) ue_obj_name(CD_CUSTTABLE(r), h->slug, sizeof h->slug); else ue_name(CD_SLUG(r), h->slug, sizeof h->slug);
            char *us = strstr(h->slug, "_Customization");
            if (us) *us = 0;
            for (char *c = h->slug; *c; c++) *c = (char)tolower((unsigned char)*c);
            h->deftable = t; h->defrow = *(FName *)e; h->custtable = CD_CUSTTABLE(r); h->defskin = *CD_DEFSKIN(r);
            h->defskin.display.data = NULL; h->defskin.display.num = h->defskin.display.max = 0;
            h->first = n_cat;
            UObject *ct = h->custtable;
            if (ct && is_rowstruct(ct, "CharacterCustomizationRow")) {
                TArray *cm = DT_ROWMAP(ct);
                for (int j = 0; j < cm->num && n_cat < MAX_ENTRIES; j++) {
                    char *ce = sparse_at(cm, j, 0x18);
                    uint8_t *cr = ce ? *(uint8_t **)(ce + 8) : NULL;
                    if (!cr || is_ours(*(FName *)ce)) continue;   // our add-on outfit rows (screen_rows) are not the game's
                    Entry *en = &cat[n_cat++];
                    memset(en, 0, sizeof *en);
                    en->hero = n_heroes; en->table = ct; en->row = *(FName *)ce; en->slot = CC_SLOT(cr) & 3;
                    ue_name(HMD_MESHPATH(CC_3P(cr)), en->mesh3p, sizeof en->mesh3p);
                    if (strcmp(en->mesh3p, "None") && en->mesh3p[0]) friendly(en->mesh3p, en->name, sizeof en->name);
                    else snprintf(en->name, sizeof en->name, "%s_%s_%s", h->slug, SLOTN[en->slot], ue_name(en->row, b, 9));
                }
            }
            h->count = n_cat - h->first;
            n_heroes++;
        }
    }
    // colour variants reuse a mesh with other materials: same name, so number them in table order (_v2, _v3 ...)
    static int dupn[MAX_ENTRIES];
    for (int i = 0; i < n_cat; i++) {
        dupn[i] = 1;
        for (int j = 0; j < i; j++) if (!strcmp(cat[i].name, cat[j].name)) dupn[i]++;
    }
    for (int i = 0; i < n_cat; i++)
        if (dupn[i] > 1) { size_t l = strlen(cat[i].name); snprintf(cat[i].name + (l > 40 ? 40 : l), 8, "_v%d", dupn[i]); }
    LOG("models: catalogue %d heroes, %d customization rows", n_heroes, n_cat);
}
static void need_catalogue(void) { if (!n_cat) build_catalogue(); }

static const Entry *entry_by_row(UObject *table, FName row) {
    for (int i = 0; i < n_cat; i++) if (cat[i].table == table && fname_eq(cat[i].row, row)) return &cat[i];
    return NULL;
}
static int hero_by_table(UObject *custtable) {
    for (int i = 0; i < n_heroes; i++) if (heroes[i].custtable == custtable) return i;
    return -1;
}
static int hero_by_slug(const char *s) {
    for (int i = 0; i < n_heroes; i++) if (!_stricmp(heroes[i].slug, s)) return i;
    return -1;
}

static void handle_str(const RowHandle *h, char *buf, size_t n) {
    char t[128], r[128];
    const Entry *e = entry_by_row(h->table, h->row);
    if (!h->table) { snprintf(buf, n, "-"); return; }
    if (e) snprintf(buf, n, "%s", e->name);
    else if (!strncmp(ue_name(h->row, r, sizeof r), "b4bcoop.", 8)) snprintf(buf, n, "%s", r);
    else snprintf(buf, n, "%s/%s", ue_obj_name(h->table, t, sizeof t), r);
}

static UObject *load_asset(const char *path);
UObject *models_load_asset(const char *path) { return load_asset(path); }
static UObject *load_asset(const char *path) {
    UClass *ksl = ue_find_class("KismetSystemLibrary");
    UObject *cdo = ksl ? UC_CDO(ksl) : NULL;
    UFunction *f = fn_of(cdo, "LoadAsset_Blocking");
    int32_t pa = parm_off(f, "Asset"), rv = parm_off(f, "ReturnValue");
    if (!f || pa < 0 || rv < 0 || UFN_PARMSSIZE(f) > 128) return NULL;
    uint8_t p[128] = {0};
    *(int32_t *)(p + pa) = -1;   // FWeakObjectPtr: none
    *(FName *)(p + pa + 0x10) = make_name(path);
    ue_process_event(cdo, f, p);
    return *(UObject **)(p + rv);
}

static UObject *comp_named(UObject *actor, const char *name) {
    int32_t n = ue_num_objects(); char a[128];
    for (int32_t i = 0; i < n; i++) {
        UObject *x = ue_object_at(i);
        if (is_live(x) && U_OUTER(x) == actor && !_stricmp(ue_obj_name(x, a, sizeof a), name)) return x;
    }
    return NULL;
}

// ---- NPC bodies: skeletal meshes on the heroes' 3P_Biped_SK skeleton that are not survivor outfits ----
// No customization row exists for them, so the look travels as a made-up outfit row name "b4bcoop.npc.<name>" in the
// hero's own customization table: the replicated set carries it to everyone, the game itself finds no such row
// (IsValid fails, nothing is applied by the game), and every machine running b4bcoop puts the NPC mesh on that hero
// (tick_npc). Players without b4bcoop keep seeing the survivor. Ridden use 3P_Common_SK (and the Hag, Sleeper, Titan
// their own): not offered.
#define NPC_PREFIX "b4bcoop.npc."
typedef struct { char name[40]; char path[160]; int bad; UObject *mesh; int32_t mesh_idx; } Npc;
static Npc npcs[128];
static int n_npcs = -1;
static void build_npcs(void) {
    n_npcs = 0;
    UObject *ar = ue_find_first_of("AssetRegistryImpl");
    if (!ar) { UClass *c = ue_find_class("AssetRegistryImpl"); ar = c ? UC_CDO(c) : NULL; }
    UClass *ic = ue_find_class("AssetRegistry");
    UFunction *f = ic ? ue_find_function(ic, "GetAssetsByClass") : NULL;   // IAssetRegistry (interface) on the impl
    int32_t pc = parm_off(f, "ClassName"), pa = parm_off(f, "OutAssetData"), ps = parm_off(f, "bSearchSubClasses");
    if (!ar || !f || pc < 0 || pa < 0 || UFN_PARMSSIZE(f) > 64) { LOG("models: no asset registry, no NPC bodies"); return; }
    uint8_t p[64] = {0};
    *(FName *)(p + pc) = make_name("SkeletalMesh");
    if (ps >= 0) p[ps] = 1;
    ue_process_event(ar, f, p);
    TArray *arr = (TArray *)(p + pa);   // FAssetData (0x50) array from the game's allocator; left as is (once)
    char b[200];
    for (int i = 0; i < arr->num && n_npcs < 128; i++) {
        ue_name(*(FName *)((char *)arr->data + i * 0x50), b, sizeof b);   // ObjectPath
        if (!strstr(b, "/Characters/NPC/") && !strstr(b, "/Characters/NPCs/") && !strstr(b, "/Characters/Cultists/") &&
            !strstr(b, "/BaseHero/Meshes/3P_Survivor")) continue;
        if (strstr(b, "CultistPet")) continue;   // a Tallboy
        Npc *n = &npcs[n_npcs++];
        memset(n, 0, sizeof *n);
        snprintf(n->path, sizeof n->path, "%s", b);
        const char *base = strrchr(b, '.');
        base = base ? base + 1 : b;
        if (!_strnicmp(base, "3P_", 3)) base += 3;
        size_t k = 0;
        for (; *base && k + 1 < sizeof n->name; base++) {
            if (!_strnicmp(base, "_SKM", 4)) { base += 3; continue; }
            n->name[k++] = (char)tolower((unsigned char)*base);
        }
        n->name[k] = 0;
    }
    for (int i = 1; i < n_npcs; i++)   // by name
        for (int j = i; j > 0 && strcmp(npcs[j - 1].name, npcs[j].name) > 0; j--) { Npc t = npcs[j]; npcs[j] = npcs[j - 1]; npcs[j - 1] = t; }
    LOG("models: %d NPC bodies in the asset registry", n_npcs);
}
static Npc *npc_by_name(const char *name) {
    if (n_npcs < 0) build_npcs();
    for (int i = 0; i < n_npcs; i++) if (!_stricmp(npcs[i].name, name)) return &npcs[i];
    return NULL;
}
// "b4bcoop.npc.<name>" row -> its NPC entry (NULL: not ours / unknown)
static Npc *npc_of_row(FName row) {
    char b[80];
    ue_name(row, b, sizeof b);
    return _strnicmp(b, NPC_PREFIX, sizeof NPC_PREFIX - 1) ? NULL : npc_by_name(b + sizeof NPC_PREFIX - 1);
}
// A skeletal mesh on the given skeleton, loaded on first use (blocking) and kept alive by the components using it;
// *bad once it failed to load or has another skeleton.
static UObject *mesh_on(const char *path, const char *skel, UObject **m, int32_t *mi, int *bad, const char *what) {
    if (*bad) return NULL;
    if (*m && ue_object_at(*mi) == *m) return *m;
    *m = load_asset(path);
    *mi = *m ? U_INDEX(*m) : 0;
    char b[64];
    UObject *sk = *m ? ue_get_ptr(*m, "Skeleton") : NULL;
    if (!sk || strcmp(ue_obj_name(sk, b, sizeof b), skel)) {
        *bad = 1;
        LOG("models: %s %s: %s", what, path, *m ? "other skeleton, not usable" : "failed to load");
        *m = NULL;
    }
    return *m;
}
static UObject *npc_mesh(Npc *n) { return mesh_on(n->path, "3P_Biped_SK", &n->mesh, &n->mesh_idx, &n->bad, "NPC"); }

// ---- added outfits: add-ons with `outfit=` lines (addons.c; made with modkit `b4bmod survivor --as <name>`) ----
// Same made-up-row path as the NPC bodies, row "b4bcoop.outfit.<name>": every machine with that add-on puts its 3P
// mesh on the body and its FP mesh on the first-person arms; machines without it (or without b4bcoop) show the
// set's pieces, i.e. the wearer's own survivor. The host relays names it doesn't have itself.
#define OUTFIT_PREFIX "b4bcoop.outfit."
typedef struct {
    char name[33], hero[24], title[64], addon[96], p3[200], pf[200];
    UObject *m3, *mf; int32_t i3, i_f; int bad, fpbad, rowfail, inpak, stale;   // stale: rows still name old meshes
} Outf;
#define MAX_OUTFS 512
static Outf outfs[MAX_OUTFS];
static int n_outfs = -1;
static int outfs_gen = 1;          // bumped whenever outfs[] changes (screen_rows redoes the tables)
static float rows_acc;
static void build_outfits(void) {
    AddonOutfit a[MAX_OUTFS];
    n_outfs = addons_outfits(a, MAX_OUTFS);
    for (int i = 0; i < n_outfs; i++) {
        Outf *o = &outfs[i];
        memset(o, 0, sizeof *o);
        snprintf(o->name, sizeof o->name, "%s", a[i].name); snprintf(o->hero, sizeof o->hero, "%s", a[i].hero);
        snprintf(o->title, sizeof o->title, "%s", a[i].title); snprintf(o->addon, sizeof o->addon, "%s", a[i].addon);
        snprintf(o->p3, sizeof o->p3, "%s", a[i].mesh3p); snprintf(o->pf, sizeof o->pf, "%s", a[i].meshfp);
        o->fpbad = !o->pf[0];
        LOG("models: add-on outfit %s (\"%s\", %s): %s%s%s", o->name, o->title, o->addon, o->p3, o->pf[0] ? " + " : "", o->pf);
    }
}
// An add-on loaded at runtime (the ~ window's Add-ons tab, /addons on): append its outfits; one whose add-on was updated (another
// file now, addons_update_runtime) gets the new meshes and title (its old meshes were not loaded: addons.c checked).
// The customization screen's rows follow at the next frame.
void models_outfits_refresh(void) {
    if (n_outfs < 0) return;   // not built yet: the first use builds the whole list
    AddonOutfit a[MAX_OUTFS];
    int n = addons_outfits(a, MAX_OUTFS);
    for (int i = 0; i < n; i++) {
        int k = 0;
        while (k < n_outfs && _stricmp(outfs[k].name, a[i].name)) k++;
        if (k < n_outfs) {
            Outf *o = &outfs[k];
            int paths = strcmp(o->p3, a[i].mesh3p) || strcmp(o->pf, a[i].meshfp);
            if (!paths && !strcmp(o->title, a[i].title) && !strcmp(o->addon, a[i].addon)) continue;
            snprintf(o->hero, sizeof o->hero, "%s", a[i].hero);
            snprintf(o->title, sizeof o->title, "%s", a[i].title); snprintf(o->addon, sizeof o->addon, "%s", a[i].addon);
            if (paths) {
                snprintf(o->p3, sizeof o->p3, "%s", a[i].mesh3p); snprintf(o->pf, sizeof o->pf, "%s", a[i].meshfp);
                o->m3 = o->mf = NULL; o->bad = o->rowfail = o->inpak = 0; o->fpbad = !o->pf[0]; o->stale = 1;
            }
            outfs_gen++;
            LOG("models: add-on outfit %s (\"%s\", %s) updated%s%s", o->name, o->title, o->addon, paths ? ": " : "", paths ? o->p3 : "");
            continue;
        }
        if (n_outfs == MAX_OUTFS) break;
        Outf *o = &outfs[n_outfs++];
        memset(o, 0, sizeof *o);
        snprintf(o->name, sizeof o->name, "%s", a[i].name); snprintf(o->hero, sizeof o->hero, "%s", a[i].hero);
        snprintf(o->title, sizeof o->title, "%s", a[i].title); snprintf(o->addon, sizeof o->addon, "%s", a[i].addon);
        snprintf(o->p3, sizeof o->p3, "%s", a[i].mesh3p); snprintf(o->pf, sizeof o->pf, "%s", a[i].meshfp);
        o->fpbad = !o->pf[0];
        outfs_gen++;
        LOG("models: add-on outfit %s (\"%s\", %s, added now): %s%s%s", o->name, o->title, o->addon, o->p3, o->pf[0] ? " + " : "", o->pf);
    }
    rows_acc = 0;   // its customization-screen rows now, not within 5 s
}
static Outf *outfit_by_name(const char *name) {
    if (n_outfs < 0) build_outfits();
    for (int i = 0; i < n_outfs; i++) if (!_stricmp(outfs[i].name, name)) return &outfs[i];
    return NULL;
}
// "b4bcoop.outfit.<name>" with a well-formed name (any add-on's, known here or not): 1, name copied to out
static int outfit_row(FName row, char *out, size_t n) {
    char b[80];
    ue_name(row, b, sizeof b);
    if (_strnicmp(b, OUTFIT_PREFIX, sizeof OUTFIT_PREFIX - 1)) return 0;
    const char *nm = b + sizeof OUTFIT_PREFIX - 1;
    size_t l = strlen(nm);
    if (!l || l > 32) return 0;
    for (const char *c = nm; *c; c++) if (!islower((unsigned char)*c) && !isdigit((unsigned char)*c) && *c != '_') return 0;
    if (out) snprintf(out, n, "%s", nm);
    return 1;
}
static Outf *outfit_of_row(FName row) {
    char nm[40];
    return outfit_row(row, nm, sizeof nm) ? outfit_by_name(nm) : NULL;
}

// ---- players, slots, sets ----
static UObject *my_ps(void) { UObject *pc = ue_local_pc(); return pc ? ue_get_ptr(pc, "PlayerState") : NULL; }
static int is_client(void) {
    UObject *w = ue_world(), *nd = w ? ue_get_ptr(w, "NetDriver") : NULL;
    return nd && ue_get_ptr(nd, "ServerConnection");
}
static UObject *ps_slot(UObject *ps) {   // GobiPlayerState.OwnedPlayerSlot (weak)
    int32_t off = ps ? ue_prop_offset(ps, "OwnedPlayerSlot") : -1;
    return off >= 0 ? ue_weak_get((char *)ps + off) : NULL;
}
static CustSet *slot_set(UObject *slot) {
    int32_t off = slot ? ue_prop_offset(slot, "CurrentCustomizationSet") : -1;
    return off >= 0 ? (CustSet *)((char *)slot + off) : NULL;
}
static int slot_hero(UObject *slot) {   // index into heroes[] of the slot's current hero, -1 unknown
    int32_t off = slot ? ue_prop_offset(slot, "CurrentHeroRowHandle") : -1;
    if (off < 0) return -1;
    RowHandle *h = (RowHandle *)((char *)slot + off);
    for (int i = 0; i < n_heroes; i++) if (heroes[i].deftable == h->table && fname_eq(heroes[i].defrow, h->row)) return i;
    return -1;
}
static UObject *slot_pawn(UObject *slot) { return slot ? ue_get_ptr(slot, "AssignedPawn") : NULL; }
static UObject *slot_owner(UObject *slot) { return slot ? ue_get_ptr(slot, "OwningPlayer") : NULL; }

// A handle names a customization row of a hero other than `hero` (or of no hero table we know).
static int foreign(const RowHandle *h, int hero) {
    if (!h->table) return 0;
    int t = hero_by_table(h->table);
    // not a real row: an NPC body; ours: an add-on outfit, also where screen_rows put a real row for it in the table
    return t < 0 || t != hero || !dt_row(h->table, h->row) || is_ours(h->row);
}
static int set_foreign(const CustSet *s, int hero) {
    for (int k = 0; k < 4; k++) if (foreign(&s->slot[k], hero)) return 1;
    return 0;
}
// Every non-empty handle is a row of a CharacterCustomizationRow table, or (outfit only) one of our NPC names or an
// add-on outfit name (the host relays those without having the add-on).
static int set_sane(const CustSet *s) {
    for (int k = 0; k < 4; k++) {
        const RowHandle *h = &s->slot[k];
        if (!h->table) continue;
        if (!is_rowstruct(h->table, "CharacterCustomizationRow")) return 0;
        if (!dt_row(h->table, h->row) && !(k == SLOT_OUTFIT && (npc_of_row(h->row) || outfit_row(h->row, NULL, 0)))) return 0;
    }
    return 1;
}

// ---- wishes ----
typedef struct {
    int on;
    char key[80];              // host: player key (steam:<id>/name:<n>) or "slot:<n>" for a bot's slot
    char label[48];            // what was asked for (entry or hero name)
    RowHandle pick[4]; uint8_t has[4];
    int npc;                   // pick[OUTFIT].row is made up: an NPC or add-on outfit (table: the hero's own, see compose)
    int32_t slot_idx;          // U_INDEX of the slot last seen (new map/slot: retry counters reset)
    int tries; float wait; int gave_up;
} Want;
static Want me;                // this machine's player
#define MAX_OTHERS 16
static Want others[MAX_OTHERS];   // host: forced on other players / bots
static int locked;             // host: /models off
static float tick_acc;
static int n_refused;

static void want_pick(Want *w, const Entry *e) {
    w->pick[e->slot].table = e->table; w->pick[e->slot].row = e->row; w->has[e->slot] = 1;
}
// A whole survivor: its default skin set (a handle to a customization row), else every outfit-slot row's first.
static int want_hero(Want *w, int hi) {
    const Hero *h = &heroes[hi];
    const Entry *e = h->defskin.table ? entry_by_row(h->defskin.table, h->defskin.row) : NULL;
    if (!e) for (int i = h->first; i < h->first + h->count; i++) if (cat[i].slot == SLOT_OUTFIT) { e = &cat[i]; break; }
    if (!e) return -1;
    want_pick(w, e);
    return 0;
}

// A set wearing a made-up outfit row with pieces missing (the customization screen sends outfit-only sets): the
// hero's first row per piece, which is what machines without the look show (the game needs all three pieces then).
static int complete_pieces(CustSet *s, int hero) {
    int done = 0;
    if (hero < 0 || s->last != SLOT_OUTFIT || !s->slot[SLOT_OUTFIT].table || !is_ours(s->slot[SLOT_OUTFIT].row)) return 0;
    for (int k = 0; k < 3; k++) {
        if (s->slot[k].table && dt_row(s->slot[k].table, s->slot[k].row)) continue;
        for (int i = heroes[hero].first; i < heroes[hero].first + heroes[hero].count; i++)
            if (cat[i].slot == k) { s->slot[k].table = cat[i].table; s->slot[k].row = cat[i].row; done++; break; }
    }
    return done;
}

// cur + picks. A head/torso/legs pick clears the outfit (the game shows the outfit when LastEquipSlot is Outfit, the
// pieces otherwise, and needs all three pieces then). A piece the set lacks is taken from the hero's first row for
// that slot (hero: index into heroes[], -1 unknown).
static void compose(const CustSet *cur, const Want *w, int hero, CustSet *out) {
    memset(out, 0, sizeof *out);
    for (int k = 0; k < 4; k++) { out->slot[k].table = cur->slot[k].table; out->slot[k].row = cur->slot[k].row; }
    out->last = cur->last;
    int pieces = 0;
    for (int k = 0; k < 4; k++) if (w->has[k]) { out->slot[k].table = w->pick[k].table; out->slot[k].row = w->pick[k].row; out->last = (uint8_t)k; pieces |= k != SLOT_OUTFIT; }
    if (w->npc) {
        out->slot[SLOT_OUTFIT].table = hero >= 0 ? heroes[hero].custtable : NULL;
        // the game finds no such row: it shows the pieces (players without b4bcoop or the add-on). Make them complete.
        for (int k = 0; k < 3 && hero >= 0; k++) {
            if (out->slot[k].table && dt_row(out->slot[k].table, out->slot[k].row)) continue;
            for (int i = heroes[hero].first; i < heroes[hero].first + heroes[hero].count; i++)
                if (cat[i].slot == k) { out->slot[k].table = cat[i].table; out->slot[k].row = cat[i].row; break; }
        }
    }
    if (!pieces || w->has[SLOT_OUTFIT]) return;
    out->slot[SLOT_OUTFIT].table = NULL; out->slot[SLOT_OUTFIT].row.idx = out->slot[SLOT_OUTFIT].row.num = 0;
    for (int k = 0; k < 3; k++) {
        if (out->slot[k].table && dt_row(out->slot[k].table, out->slot[k].row)) continue;
        for (int i = hero >= 0 ? heroes[hero].first : 0; hero >= 0 && i < heroes[hero].first + heroes[hero].count; i++)
            if (cat[i].slot == k) { out->slot[k].table = cat[i].table; out->slot[k].row = cat[i].row; break; }
    }
}
static int matches(const CustSet *cur, const Want *w) {
    int pieces = 0;
    for (int k = 0; k < 4; k++) {
        if (!w->has[k]) continue;
        if (k == SLOT_OUTFIT && w->npc) { if (!cur->slot[k].table || !fname_eq(cur->slot[k].row, w->pick[k].row) || cur->last != SLOT_OUTFIT) return 0; continue; }
        if (cur->slot[k].table != w->pick[k].table || !fname_eq(cur->slot[k].row, w->pick[k].row)) return 0;
        pieces |= k != SLOT_OUTFIT;
    }
    return !(pieces && !w->has[SLOT_OUTFIT] && cur->slot[SLOT_OUTFIT].table);
}

// Our own player: the game's server RPC (host: runs locally).
static int send_select(UObject *ps, const CustSet *set) {
    UFunction *f = fn_of(ps, "ServerSelectCustomizationSet");
    int32_t po = parm_off(f, "InCustomizationSet");
    if (!f || po < 0 || UFN_PARMSSIZE(f) > 256) return -1;
    uint8_t p[256] = {0};
    memcpy(p + po, set, sizeof *set);
    ue_process_event(ps, f, p);
    return 0;
}
// Host: any slot. Same as the RPC implementation: copy (table, row) x4 + last, then the slot's OnRep applies it here;
// clients get the replicated property and apply it in their OnRep.
static int host_write(UObject *slot, const CustSet *set) {
    CustSet *cur = slot_set(slot);
    UFunction *f = fn_of(slot, "OnRep_CurrentCustomizationSet");
    if (!cur || !f) return -1;
    for (int k = 0; k < 4; k++) { cur->slot[k].table = set->slot[k].table; cur->slot[k].row = set->slot[k].row; }
    cur->last = set->last;
    uint8_t p[16] = {0};
    ue_process_event(slot, f, p);
    return 0;
}
// The game's own re-init of a player's look from their profile: ClientInitCustomizationRowForSelectedCharacter is a
// client RPC (runs on that player's machine, even without b4bcoop), which answers with ServerSelectCustomizationSet.
static int reinit_from_profile(UObject *ps, UObject *slot) {
    UFunction *f = fn_of(ps, "ClientInitCustomizationRowForSelectedCharacter");
    int32_t po = parm_off(f, "CurrentHeroRowHandle"), ho = slot ? ue_prop_offset(slot, "CurrentHeroRowHandle") : -1;
    if (!f || po < 0 || ho < 0 || UFN_PARMSSIZE(f) > 64) return -1;
    uint8_t p[64] = {0};
    RowHandle *h = (RowHandle *)(p + po), *src = (RowHandle *)((char *)slot + ho);
    h->table = src->table; h->row = src->row;
    ue_process_event(ps, f, p);
    return 0;
}
static int default_set(UObject *slot, CustSet *out) {   // a bot's look: its hero's default skin
    int hi = slot_hero(slot);
    Want w = {0};
    CustSet *cur = slot_set(slot);
    if (hi < 0 || !cur || want_hero(&w, hi)) return -1;
    CustSet clean = {0};
    compose(&clean, &w, hi, out);
    return 0;
}

static int is_bot_slot(UObject *slot) {
    static UClass *pcc;
    if (!pcc) pcc = ue_find_class("PlayerController");
    UObject *ps = slot_owner(slot), *owner = ps ? ue_get_ptr(ps, "Owner") : NULL;
    return !ps || !owner || !pcc || !ue_is_a(owner, pcc);
}

// The game's own look per slot (host): the last set without another survivor's rows, seen before we changed it or
// accepted from the player. Put back while the campaign run is saved (runrefresh_detour).
static struct { UObject *slot; int32_t idx; CustSet set; } cleans[16];
static void note_clean(UObject *slot, const CustSet *set) {
    if (!slot || !set || set_foreign(set, slot_hero(slot))) return;
    int free_i = -1;
    for (int i = 0; i < 16; i++) {
        int live = cleans[i].slot && ue_object_at(cleans[i].idx) == cleans[i].slot;
        if (live && cleans[i].slot == slot) { free_i = i; break; }
        if (!live && free_i < 0) free_i = i;
    }
    if (free_i < 0) return;
    cleans[free_i].slot = slot; cleans[free_i].idx = U_INDEX(slot);
    memset(&cleans[free_i].set, 0, sizeof cleans[free_i].set);
    for (int k = 0; k < 4; k++) { cleans[free_i].set.slot[k].table = set->slot[k].table; cleans[free_i].set.slot[k].row = set->slot[k].row; }
    cleans[free_i].set.last = set->last;
}
static int clean_of(UObject *slot, CustSet *out) {
    for (int i = 0; i < 16; i++)
        if (cleans[i].slot == slot && ue_object_at(cleans[i].idx) == slot) { *out = cleans[i].set; return 0; }
    return default_set(slot, out);
}

// Hero-team slots (PlayerSlotManager.TeamSlots[team 0].Slots), in slot order.
static int hero_slots(UObject ***out) {
    UObject *w = ue_world(), *gs = w ? ue_get_ptr(w, "GameState") : NULL, *psm = gs ? ue_get_ptr(gs, "PlayerSlotManager") : NULL;
    int32_t off = psm ? ue_prop_offset(psm, "TeamSlots") : -1;
    if (off < 0) return 0;
    TArray *teams = (TArray *)((char *)psm + off);
    for (int t = 0; t < teams->num; t++) {
        uint8_t *ts = (uint8_t *)teams->data + t * 0x20;
        if (ts[0] == 0) { TArray *s = (TArray *)(ts + 8); *out = (UObject **)s->data; return s->num; }
    }
    return 0;
}

int models_hero_pawns(UObject **out, int max) {
    UObject **s; int n = hero_slots(&s), k = 0;
    for (int i = 0; i < n && k < max; i++) { UObject *p = slot_pawn(s[i]); if (p) out[k++] = p; }
    return k;
}
int models_locked(void) { return locked; }

// Slot a host wish targets now (NULL: not in this map / not yet).
static UObject *want_slot(const Want *w) {
    if (!strncmp(w->key, "slot:", 5)) {
        UObject **s; int n = hero_slots(&s), i = atoi(w->key + 5);
        return i >= 0 && i < n ? s[i] : NULL;
    }
    UObject *gs = ue_world() ? ue_get_ptr(ue_world(), "GameState") : NULL;
    int32_t off = gs ? ue_prop_offset(gs, "PlayerArray") : -1;
    if (off < 0) return NULL;
    TArray *pa = (TArray *)((char *)gs + off);
    char k[80];
    UObject *mine = my_ps();
    for (int i = 0; i < pa->num; i++) {
        UObject *ps = ((UObject **)pa->data)[i];
        if (!ps || ps == mine) continue;   // the host's own look is `me` (local copies share one Steam id)
        admin_ps_key(ps, k, sizeof k);
        if (!strcmp(k, w->key)) return ps_slot(ps);
    }
    return NULL;
}

// ---- tick: keep wishes applied ----
// A slot we can dress: it has a pawn, a chosen hero, and a complete look from the game (an outfit or all 3 pieces).
static int ready_slot(UObject *slot) {
    CustSet *cur = slot_set(slot);
    if (!cur || !slot_pawn(slot) || slot_hero(slot) < 0) return 0;
    return cur->slot[SLOT_OUTFIT].table || (cur->slot[SLOT_HEAD].table && cur->slot[SLOT_TORSO].table && cur->slot[SLOT_LEGS].table);
}
static void tick_me(float dt) {
    if (!me.on) return;
    UObject *ps = my_ps(), *slot = ps_slot(ps);
    CustSet *cur = slot_set(slot);
    // loading, character select (no hero yet: the game sends our profile look once one is picked), dead: later
    if (!ready_slot(slot)) return;
    int32_t key = U_INDEX(slot) * 64 + slot_hero(slot);
    if (key != me.slot_idx) { me.slot_idx = key; me.tries = 0; me.gave_up = 0; me.wait = 3; }   // let the game's own sends settle
    if (matches(cur, &me)) { me.tries = 0; me.gave_up = 0; return; }
    // A host with b4bcoop and /models off answers with a notice (models_host_notice); otherwise the host accepts, but
    // the slot's replication can lag seconds behind (e.g. right after a checkpoint restart): resend slowly, then stop.
    if (me.gave_up || (me.wait -= dt) > 0) return;
    if (me.tries >= 10) {
        me.gave_up = 1;
        LOG("models: %s still not applied after %d tries, giving up for this map", me.label, me.tries);
        return;
    }
    CustSet s;
    if (!is_client()) note_clean(slot, cur);
    compose(cur, &me, slot_hero(slot), &s);
    if (send_select(ps, &s)) return;
    me.tries++; me.wait = me.tries < 3 ? 5 : 10;
    LOG("models: sent %s (try %d)", me.label, me.tries);
}

static void tick_others(float dt) {
    if (is_client()) return;
    for (int i = 0; i < MAX_OTHERS; i++) {
        Want *w = &others[i];
        if (!w->on) continue;
        UObject *slot = want_slot(w);
        CustSet *cur = slot_set(slot);
        if (!ready_slot(slot)) continue;
        if (!strncmp(w->key, "slot:", 5) && !is_bot_slot(slot)) {   // a human took over this bot: their own choice now
            LOG("models: %s taken over by a player, dropping the forced model", w->key);
            w->on = 0;
            continue;
        }
        int32_t key = U_INDEX(slot) * 64 + slot_hero(slot);
        if (key != w->slot_idx) { w->slot_idx = key; w->wait = 3; }
        if (matches(cur, w) || (w->wait -= dt) > 0) continue;
        CustSet s;
        note_clean(slot, cur);
        compose(cur, w, slot_hero(slot), &s);
        if (!host_write(slot, &s)) LOG("models: applied %s to %s", w->label, w->key);
        w->wait = 2;
    }
}

// Every machine: heroes whose replicated set names an NPC body wear it (the game applies nothing for that row).
static UObject *comp_of(UObject *actor, const char *name) {   // cached per actor: object scans are slow
    static struct { UObject *actor, *comp; int32_t ai, ci; char name[24]; } cache[48];
    for (int i = 0; i < 48; i++)
        if (cache[i].actor == actor && !strcmp(cache[i].name, name) && ue_object_at(cache[i].ai) == actor && ue_object_at(cache[i].ci) == cache[i].comp)
            return cache[i].comp;
    UObject *c = comp_named(actor, name);
    static int next;
    if (c) { int i = next++ % 48; cache[i].actor = actor; cache[i].comp = c; cache[i].ai = U_INDEX(actor); cache[i].ci = U_INDEX(c); snprintf(cache[i].name, sizeof cache[i].name, "%s", name); }
    return c;
}
static void set_mesh(UObject *comp, UObject *mesh) {
    static UFunction *f; static int32_t pm, pr;
    if (!f) { f = fn_of(comp, "SetSkeletalMesh"); pm = parm_off(f, "NewMesh"); pr = parm_off(f, "bReinitPose"); }
    if (!f || pm < 0) return;
    int32_t om = ue_prop_offset(comp, "OverrideMaterials");   // the survivor outfit's material overrides don't fit
    if (om >= 0) ((TArray *)((char *)comp + om))->num = 0;
    uint8_t p[32] = {0};
    *(UObject **)(p + pm) = mesh;
    if (pr >= 0) p[pr] = 1;
    ue_process_event(comp, f, p);
}
// Hitboxes stay the wearer's: a body mesh brings its own PhysicsAsset (an add-on outfit: its template survivor's, e.g.
// a female survivor's smaller bodies on Walker; an NPC body: the NPC's). The wearer's own is the PhysicsAsset of the
// set's torso piece (what the game shows for the set without the look: compose keeps the pieces complete); it is put
// on the body component as its override (SetPhysicsAsset -> PhysicsAssetOverride, which later SetSkeletalMesh calls
// keep), so a look never changes how the hero is hit.
static struct { UObject *table, *pa; FName row; int32_t pai; } torso_pas[24];
static UObject *wearer_pa(const CustSet *cur, int hero) {
    const RowHandle *t = &cur->slot[SLOT_TORSO];
    uint8_t *r = t->table && !is_ours(t->row) ? dt_row(t->table, t->row) : NULL;
    if (!r && hero >= 0) { t = &heroes[hero].defskin; r = t->table ? dt_row(t->table, t->row) : NULL; }   // no pieces: the default outfit
    if (!r) return NULL;
    static int next;
    for (int i = 0; i < 24; i++)
        if (torso_pas[i].table == t->table && fname_eq(torso_pas[i].row, t->row) && torso_pas[i].pa && ue_object_at(torso_pas[i].pai) == torso_pas[i].pa)
            return torso_pas[i].pa;
    char p[200];
    ue_name(HMD_MESHPATH(CC_3P(r)), p, sizeof p);
    UObject *m = strcmp(p, "None") ? load_asset(p) : NULL, *pa = m ? ue_get_ptr(m, "PhysicsAsset") : NULL;
    if (!pa) return NULL;
    int i = next++ % 24;
    torso_pas[i].table = t->table; torso_pas[i].row = t->row; torso_pas[i].pa = pa; torso_pas[i].pai = U_INDEX(pa);
    return pa;
}
static int keep_wearer_pa(UObject *body, UObject *pa) {   // 1 if it was changed
    static UFunction *f; static int32_t pp, pr;
    if (!f) { f = fn_of(body, "SetPhysicsAsset"); pp = parm_off(f, "NewPhysicsAsset"); pr = parm_off(f, "bForceReInit"); }
    if (!f || pp < 0 || !pa) return 0;
    int32_t ov = ue_prop_offset(body, "PhysicsAssetOverride");
    if (ov >= 0 && *(UObject **)((char *)body + ov) == pa) return 0;
    uint8_t p[32] = {0};
    *(UObject **)(p + pp) = pa;
    if (pr >= 0) p[pr] = 1;
    ue_process_event(body, f, p);
    return 1;
}

// The 3P body mesh of a made-up outfit row (NPC body or add-on outfit) and, for add-on outfits, the FP arms; NULL when
// the row is not ours or the look isn't here (add-on not mounted, mesh failed to load).
static UObject *look_meshes(FName row, UObject **fp, Npc **npc, Outf **outf) {
    Npc *np = npc_of_row(row);
    Outf *of = np ? NULL : outfit_of_row(row);
    UObject *mesh = np && !np->bad ? npc_mesh(np) : of ? mesh_on(of->p3, "3P_Biped_SK", &of->m3, &of->i3, &of->bad, "outfit") : NULL;
    if (fp) *fp = mesh && of && !of->fpbad ? mesh_on(of->pf, "FP_Biped_SK", &of->mf, &of->i_f, &of->fpbad, "outfit arms") : NULL;
    if (npc) *npc = np;
    if (outf) *outf = of;
    return mesh;
}

// The game's per-slot apply of a customization set (FCharacterCustomizationSet -> mesh components): one function for
// everything that wears a set: heroes (0x141BE5E20 -> 0x141B75AD0), cutscene stand-ins (APlayerStandIn, e.g. the
// Act 3 escape: SetAppearanceToMatchPlayerSlot copies the slot's set), the lineup / character-select / customization
// mannequins. bool (ctx {set*, comps*}, slot, first-person comp, third-person comp): false when the row isn't in its
// table; for the outfit slot the caller then applies the three pieces instead, and on true with LastEquipSlot Outfit
// it empties the head and legs components. So for a made-up outfit row whose look is here (NPC body, add-on outfit
// not given a real row by screen_rows), we put the look on and answer true: every such actor shows it exactly like a
// real outfit (#37: stand-ins showed the survivor). Synchronous like the game's own (blocking loads).
#define ADDR_APPLYSLOT VA(0x141B75B90ull)
static const uint8_t SIG_APPLYSLOT[] = {0x40,0x55,0x53,0x56,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,
                                        0x10,0xfc,0xff,0xff,0x48,0x81,0xec,0xf0};
typedef uint8_t (*ApplySlotFn)(void **ctx, uint8_t slot, UObject *fp, UObject *body);
static ApplySlotFn orig_applyslot;
static int n_slot_applied;
static uint8_t applyslot_detour(void **ctx, uint8_t slot, UObject *fp, UObject *body) {
    uint8_t r = orig_applyslot(ctx, slot, fp, body);
    if (r || slot != SLOT_OUTFIT || !ctx || !ctx[0]) return r;
    const RowHandle *h = &((CustSet *)ctx[0])->slot[SLOT_OUTFIT];
    if (!h->table || !is_ours(h->row)) return r;
    UObject *fpm = NULL, *mesh = look_meshes(h->row, &fpm, NULL, NULL);
    if (!mesh) return r;
    if (body) set_mesh(body, mesh);
    if (fp) {
        if (fpm) set_mesh(fp, fpm);
        else orig_applyslot(ctx, SLOT_TORSO, fp, NULL);   // no arms of its own (NPC bodies): the torso piece's
    }
    if (n_slot_applied++ < 50 || !(n_slot_applied % 100)) {
        char a[96], b[80], c[96];
        UObject *owner = body ? U_OUTER(body) : NULL;
        LOG("models: %s on %s (%s)", ue_name(h->row, b, sizeof b), owner ? ue_obj_name(owner, a, sizeof a) : "?",
            owner ? ue_obj_name(U_CLASS(owner), c, sizeof c) : "?");
    }
    return 1;
}

// Every machine, heroes whose replicated set names a made-up outfit row (NPC body, add-on outfit): the look and the
// wearer's hitboxes. The apply hook above (or the game, for a real row) normally put the look on already; this keeps
// it (a later re-apply, a pawn the hook missed) and logs each new wearer once.
static struct { UObject *pawn; int32_t pi; FName row; } worn[16];
static int n_npc_applied;
static void tick_npc(void) {
    UObject **s; int n = hero_slots(&s);
    for (int i = 0; i < n; i++) {
        CustSet *cur = slot_set(s[i]);
        UObject *pawn = slot_pawn(s[i]);
        if (!cur || !pawn || cur->last != SLOT_OUTFIT || !cur->slot[SLOT_OUTFIT].table) continue;
        FName row = cur->slot[SLOT_OUTFIT].row;
        if (!is_ours(row)) continue;
        Npc *np; Outf *of; UObject *fpm;
        UObject *mesh = look_meshes(row, &fpm, &np, &of);
        UObject *body = mesh ? ue_get_ptr(pawn, "Mesh") : NULL;
        if (!body) continue;
        if (fpm) {   // first-person arms (NPC bodies have none: the survivor's stay)
            UObject *arms = comp_of(pawn, "FirstPersonArms");
            if (arms && ue_get_ptr(arms, "SkeletalMesh") != fpm) set_mesh(arms, fpm);
        }
        int changed = 0;
        if (ue_get_ptr(body, "SkeletalMesh") != mesh) { set_mesh(body, mesh); changed = 1; }
        if (!n_cat) need_catalogue();
        UObject *pa = wearer_pa(cur, slot_hero(s[i])), *mpa = ue_get_ptr(mesh, "PhysicsAsset");
        if (pa && pa != mpa) changed |= keep_wearer_pa(body, pa);
        UObject *head = comp_of(pawn, "ThirdPersonHeadMesh"), *legs = comp_of(pawn, "ThirdPersonLegsMesh");
        if (head && ue_get_ptr(head, "SkeletalMesh")) { set_mesh(head, NULL); changed = 1; }
        if (legs && ue_get_ptr(legs, "SkeletalMesh")) { set_mesh(legs, NULL); changed = 1; }
        int seen = i < 16 && worn[i].pawn == pawn && ue_object_at(worn[i].pi) == pawn && fname_eq(worn[i].row, row);
        if (changed) n_npc_applied++;
        if (seen && !changed) continue;
        if (i < 16) { worn[i].pawn = pawn; worn[i].pi = U_INDEX(pawn); worn[i].row = row; }
        char pb[96];
        LOG("models: hero slot %d wears %s %s (hitboxes: %s)", i, np ? "NPC" : "outfit", np ? np->name : of->name,
            pa ? ue_obj_name(pa, pb, sizeof pb) : mpa ? "the look's own" : "none");
    }
}
// ---- added outfits on the game's customization screen (#33; docs/investigations/new-assets.md §11) ----
// Every mounted add-on outfit gets a real row "b4bcoop.outfit.<name>" in every survivor's <Hero>_Customization_DT,
// on this machine only (same name as the made-up row of /model, so everything above treats both alike). The screen
// lists the table's rows, so the outfit shows up there for every survivor: unlocked (the lock check only knows rows a
// Products_DT product unlocks), the title as its name, the game's generic skin icon (no product, no icon). The game
// itself then applies the row like any outfit, on every actor that wears a set.
// The profile never holds such a row: equipping it saves the profile's previous look for that survivor instead
// (equip_detour) and remembers the add-on outfit in b4bcoop-outfits.txt next to the config ("<survivor> <outfit>");
// reading the profile's look (FCharacterCustomizationUtils::GetProfileCustomization, getprof_detour) answers with the
// remembered outfit while its add-on is mounted. So the game without b4bcoop or without the add-on loads a profile it
// knows, and shows the survivor's previous look.
#define ADDR_GMALLOC VA(0x1469E59F0ull)     // FMalloc* GMalloc; vtable +0x20 Realloc(this, ptr, size, align)
typedef void *(*ReallocFn)(void *self, void *p, size_t n, uint32_t align);
static void *gm_realloc(void *p, size_t n) {
    void *gm = *(void **)ADDR_GMALLOC;
    return gm ? ((ReallocFn)(*(void ***)gm)[4])(gm, p, n, 16) : NULL;
}
// TMap<FName, uint8*> (UDataTable::RowMap) in this build: TSparseArray {TArray elements 0x18 {key, value, HashNextId,
// HashIndex}; TBitArray inline 4 dwords +0x10, secondary +0x20, NumBits +0x28, MaxBits +0x2c; FirstFreeIndex +0x30,
// NumFreeIndices +0x34}, hash {inline +0x38, secondary +0x40}, HashSize +0x48. Hash of an FName as the set's FindId
// (0x140BCE720) computes it.
static uint32_t fname_hash(FName n) {
    uint32_t blk = n.idx >> 18, off = n.idx & 0xffff;
    return (blk << 21) + off + (off << 16) + (off >> 4) + n.num + blk;
}
static int rowmap_add(void *map, FName key, void *row) {
    char *m = map;
    TArray *el = map;
    int32_t *nbits = (int32_t *)(m + 0x28), maxbits = *(int32_t *)(m + 0x2c), nfree = *(int32_t *)(m + 0x34);
    int32_t hsize = *(int32_t *)(m + 0x48);
    uint32_t *bits = *(uint32_t **)(m + 0x20) ? *(uint32_t **)(m + 0x20) : (uint32_t *)(m + 0x10);
    int32_t *hash = *(int32_t **)(m + 0x40) ? *(int32_t **)(m + 0x40) : (int32_t *)(m + 0x38);
    if (hsize <= 0 || (hsize & (hsize - 1))) return -1;
    if (nfree || *nbits != el->num) return -2;   // tables are loaded without holes; anything else: leave it alone
    if (el->num + 1 > maxbits) {   // allocation flags full: grow them (inline 4 dwords -> heap, as TBitArray does)
        int32_t nmax = (maxbits + 128 + 31) & ~31;
        uint32_t *nb = gm_realloc(NULL, (size_t)nmax / 8);
        if (!nb) return -3;
        memset(nb, 0, (size_t)nmax / 8);
        memcpy(nb, bits, (size_t)((maxbits + 31) / 32) * 4);
        uint32_t *old = *(uint32_t **)(m + 0x20);
        *(uint32_t **)(m + 0x20) = nb; *(int32_t *)(m + 0x2c) = nmax;
        if (old) gm_realloc(old, 0);   // Realloc to 0 = free
        bits = nb;
    }
    for (int i = 0; i < el->num; i++) {   // our hash must be the game's
        char *e = (char *)el->data + (size_t)i * 0x18;
        if (*(int32_t *)(e + 0x14) != (int32_t)(fname_hash(*(FName *)e) & (uint32_t)(hsize - 1))) return -4;
    }
    if (el->num >= el->max) {
        void *d = gm_realloc(el->data, (size_t)(el->max + 16) * 0x18);
        if (!d) return -5;
        el->data = d; el->max += 16;
    }
    int idx = el->num;
    char *e = (char *)el->data + (size_t)idx * 0x18;
    *(FName *)e = key; *(void **)(e + 8) = row;
    int32_t hi = (int32_t)(fname_hash(key) & (uint32_t)(hsize - 1));
    *(int32_t *)(e + 0x14) = hi; *(int32_t *)(e + 0x10) = hash[hi];
    bits[idx >> 5] |= 1u << (idx & 31);
    el->num++; (*nbits)++;
    hash[hi] = idx;
    return idx;
}
static int text_of(const char *s, void *ftext) {   // FText (0x18) owning a new text: KismetTextLibrary.Conv_StringToText
    UClass *k = ue_find_class("KismetTextLibrary");
    UObject *cdo = k ? UC_CDO(k) : NULL;
    UFunction *f = fn_of(cdo, "Conv_StringToText");
    int32_t pi = parm_off(f, "inString"), pr = parm_off(f, "ReturnValue");   // sic: "inString"
    if (!f || pi < 0 || pr < 0 || UFN_PARMSSIZE(f) > 64) return !cdo ? -2 : !f ? -3 : -4;
    wchar_t w[128]; int n = 0;
    for (; s[n] && n < 127; n++) w[n] = (wchar_t)(unsigned char)s[n];
    w[n] = 0;
    uint8_t p[64] = {0};
    FString *in = (FString *)(p + pi);
    in->data = w; in->num = in->max = n + 1;
    ue_process_event(cdo, f, p);
    memcpy(ftext, p + pr, 0x18);   // the reference moves into the row (never released: rows live as long as the table)
    return *(void **)ftext ? 0 : -1;
}
// A CharacterCustomizationRow (0x320) for an add-on outfit, built field by field (no shallow copies of the template's
// maps/texts): the template row (the survivor's default outfit) gives the vtable, quality and equip sound.
static uint8_t *outfit_row_new(const uint8_t *tmpl, const Outf *o) {
    uint8_t *r = gm_realloc(NULL, 0x320);
    if (!r) { LOG("models: row for %s: no memory", o->name); return NULL; }
    memset(r, 0, 0x320);
    memcpy(r, tmpl, 8);                                     // FTableRowBase vtable
    char desc[160];
    snprintf(desc, sizeof desc, "Add-on outfit (%s). Players without this add-on see your survivor.", o->addon);
    char up[64];   // the game's outfit names are upper case
    snprintf(up, sizeof up, "%s", o->title[0] ? o->title : o->name);
    for (char *c = up; *c; c++) *c = (char)toupper((unsigned char)*c);
    int te = text_of(up, r + 0x08);
    if (!te) te = text_of(desc, r + 0x20);
    if (te) { LOG("models: row for %s: no text (%d)", o->name, te); return NULL; }   // leaks 0x320
    r[0x38] = tmpl[0x38];                                   // Quality
    memcpy(r + 0x40, tmpl + 0x40, 8);                       // EquipSound
    r[0x48] = SLOT_OUTFIT;
    char *fp = CC_FP(r), *tp = CC_3P(r);
    *(int32_t *)(fp + 0xa0) = *(int32_t *)(tp + 0xa0) = -1;   // weak pointers: none (the soft path resolves)
    *(int32_t *)(r + 0x1e0 + 0xa0) = -1;                    // ThirdPersonMeshDefinition_XB1PS4_Override: empty
    for (int k = 0; k < 3; k++) *(int32_t *)(r + 0x2a8 + k * 0x28) = -1;   // animation overrides: none
    HMD_MESHPATH(tp) = make_name(o->p3);
    // no arms of its own (converted mods may lack them): the survivor's default outfit's, never an empty FP mesh
    HMD_MESHPATH(fp) = o->pf[0] ? make_name(o->pf) : HMD_MESHPATH(CC_FP(tmpl));
    return r;
}
static int screen_off;            // ini outfits_screen=0: no rows (then /model only)
static struct { int32_t ti; int n; } done[32];   // per survivor: table checked with that many outfits
static int n_rows_added, n_rows_failed, n_rows_updated;
static int mesh_in_paks(const char *objpath) {   // "/Game/A/B.B" -> Gobi/Content/A/B.uasset is in a mounted pak
    char key[260];
    const char *dot = strrchr(objpath, '.');
    if (_strnicmp(objpath, "/Game/", 6) || !dot) return 0;
    snprintf(key, sizeof key, "Gobi/Content/%.*s.uasset", (int)(dot - objpath - 6), objpath + 6);
    return paks_file_exists(key);
}
static void screen_rows(float dt) {
    static int read_ini;
    if (!read_ini) {   // ini outfits_screen=0 (read at the first tick: b4bcoop.ini is read after models_init)
        const char *v = cmds_ini_value("outfits_screen");
        read_ini = 1;
        if (v && !strcmp(v, "0")) { screen_off = 1; LOG("models: add-on outfits on the customization screen: off (outfits_screen=0)"); }
    }
    if (screen_off || (rows_acc -= dt) > 0) return;
    rows_acc = 5;
    if (n_outfs < 0) build_outfits();
    if (!n_outfs) return;
    need_catalogue();
    int added = n_rows_added;
    for (int h = 0; h < n_heroes && h < 32; h++) {
        UObject *t = heroes[h].custtable;
        uint8_t *tmpl = heroes[h].defskin.table ? dt_row(heroes[h].defskin.table, heroes[h].defskin.row) : NULL;
        if (!t || !is_live(t) || !tmpl || !is_rowstruct(t, "CharacterCustomizationRow")) continue;
        if (done[h].ti == U_INDEX(t) && done[h].n == outfs_gen) continue;
        done[h].ti = U_INDEX(t); done[h].n = outfs_gen;
        for (int i = 0; i < n_outfs; i++) {
            Outf *o = &outfs[i];
            if (o->bad || o->rowfail) continue;
            if (!o->inpak) {   // never a row whose mesh isn't there: the game would put an empty mesh on
                o->inpak = mesh_in_paks(o->p3) ? 1 : -1;
                if (o->inpak < 0) LOG("models: outfit %s: %s not in the mounted add-ons, /model only", o->name, o->p3);
            }
            if (o->inpak < 0) continue;
            char rn[64];
            snprintf(rn, sizeof rn, OUTFIT_PREFIX "%s", o->name);
            FName key = make_name(rn);
            uint8_t *er = dt_row(t, key);
            if (er) {
                if (o->stale) {   // its add-on was updated with other mesh paths (none of the old ones loaded)
                    char *fp = CC_FP(er), *tp = CC_3P(er);
                    *(int32_t *)(fp + 0xa0) = *(int32_t *)(tp + 0xa0) = -1;
                    HMD_MESHPATH(tp) = make_name(o->p3);
                    HMD_MESHPATH(fp) = o->pf[0] ? make_name(o->pf) : HMD_MESHPATH(CC_FP(tmpl));
                    n_rows_updated++;
                }
                continue;
            }
            uint8_t *r = outfit_row_new(tmpl, o);
            int idx = r ? rowmap_add(DT_ROWMAP(t), key, r) : -6;
            char tn[64];
            if (idx < 0) {
                o->rowfail = 1; n_rows_failed++;
                LOG("models: no customization-screen row for outfit %s in %s (%d): /model only", o->name, ue_obj_name(t, tn, sizeof tn), idx);
                continue;
            }
            n_rows_added++;
        }
    }
    if (n_rows_added > added) LOG("models: %d customization-screen row(s) added (add-on outfits for every survivor, %d in all)", n_rows_added - added, n_rows_added);
    if (n_rows_updated) LOG("models: %d customization-screen row(s) now name an updated add-on's meshes", n_rows_updated);
    n_rows_updated = 0;
    for (int i = 0; i < n_outfs; i++) outfs[i].stale = 0;
}

// b4bcoop-outfits.txt: the add-on outfit each survivor wears, picked on the customization screen
typedef struct { char hero[24], outfit[33]; } Pick;
static Pick picks[32];
static int n_picks = -1;
static void picks_path(char *path, size_t n) {
    const char *cfg = cmds_config_path(), *s1 = strrchr(cfg, '\\'), *s2 = strrchr(cfg, '/');
    const char *sl = s1 > s2 ? s1 : s2;
    int dir = sl ? (int)(sl - cfg + 1) : 0;
    snprintf(path, n, "%.*sb4bcoop-outfits.txt", dir, cfg);
}
static void picks_load(void) {
    n_picks = 0;
    char path[MAX_PATH], line[128];
    picks_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f) && n_picks < 32) {
        char h[32], o[48];
        if (line[0] == '#' || sscanf(line, "%31s %47s", h, o) != 2) continue;
        snprintf(picks[n_picks].hero, sizeof picks[n_picks].hero, "%s", h);
        snprintf(picks[n_picks].outfit, sizeof picks[n_picks].outfit, "%s", o);
        n_picks++;
    }
    fclose(f);
}
static const char *pick_of(const char *hero) {
    if (n_picks < 0) picks_load();
    for (int i = 0; i < n_picks; i++) if (!_stricmp(picks[i].hero, hero)) return picks[i].outfit;
    return NULL;
}
static void pick_set(const char *hero, const char *outfit) {   // outfit NULL: none
    if (n_picks < 0) picks_load();
    const char *cur = pick_of(hero);
    if (outfit ? cur && !strcmp(cur, outfit) : !cur) return;
    int i = 0;
    while (i < n_picks && _stricmp(picks[i].hero, hero)) i++;
    if (!outfit) { if (i < n_picks) picks[i] = picks[--n_picks]; }
    else {
        if (i == n_picks && n_picks < 32) n_picks++;
        if (i < n_picks) { snprintf(picks[i].hero, sizeof picks[i].hero, "%s", hero); snprintf(picks[i].outfit, sizeof picks[i].outfit, "%s", outfit); }
    }
    char path[MAX_PATH];
    picks_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) { LOG("models: can't write %s", path); return; }
    fprintf(f, "# b4bcoop: add-on outfits picked on the customization screen (survivor outfit). Your profile keeps the\n"
               "# survivor's previous look, which the game shows without b4bcoop or without the add-on.\n");
    for (int k = 0; k < n_picks; k++) fprintf(f, "%s %s\n", picks[k].hero, picks[k].outfit);
    fclose(f);
    LOG("models: %s now wears %s on the customization screen", hero, outfit ? outfit : "a game outfit");
}
static int hero_by_def(const RowHandle *h) {   // a CharacterDefinitionRow handle (HeroDefinitions_DT, hero_N)
    if (!h || !h->table) return -1;
    need_catalogue();
    for (int i = 0; i < n_heroes; i++) if (heroes[i].deftable == h->table && fname_eq(heroes[i].defrow, h->row)) return i;
    return -1;
}

// FCharacterCustomizationUtils::GetProfileCustomization(PlayerController, hero row handle, out set): the profile's
// look for that survivor (default skin in the outfit slot first, then the saved set if valid and unlocked). Used by
// AGobiPlayerState::InitCustomizationSet (-> ServerSelectCustomizationSet), the customization screen and the
// mannequins. Our own controller only: the remembered add-on outfit replaces the outfit slot while its row exists.
#define ADDR_GETPROF VA(0x141B769C0ull)
static const uint8_t SIG_GETPROF[] = {0x40,0x53,0x57,0x41,0x55,0x41,0x56,0x48,0x83,0xec,0x78,0x4d,0x8b,0xe8,0x4c,0x8b,
                                      0xf2,0x48,0x8b,0xd9,0x48,0x85,0xc9,0x0f};
typedef void (*GetProfFn)(UObject *pc, RowHandle *hero, CustSet *out);
static GetProfFn orig_getprof;
static int no_subst;              // the host refused our add-on outfit: the profile's own look until the next map
static int n_subst, reinit_due;
static void getprof_detour(UObject *pc, RowHandle *hero, CustSet *out) {
    orig_getprof(pc, hero, out);
    if (!pc || !out || pc != ue_local_pc() || screen_off || no_subst || models_off()) return;
    int hi = hero_by_def(hero);
    const char *nm = hi >= 0 ? pick_of(heroes[hi].slug) : NULL;
    Outf *of = nm ? outfit_by_name(nm) : NULL;
    char rn[64];
    snprintf(rn, sizeof rn, OUTFIT_PREFIX "%s", nm ? nm : "");
    FName row = of ? make_name(rn) : (FName){0};
    if (!of || !dt_row(heroes[hi].custtable, row)) return;   // add-on not mounted here (or no row): the profile's look
    if (!is_client()) {   // host: the look the campaign run saves for us (runrefresh_detour) is the profile's
        UObject *slot = ps_slot(my_ps());
        if (slot) note_clean(slot, out);
    }
    for (int k = 0; k < 3; k++) {   // complete pieces: what players without the add-on see
        if (out->slot[k].table && dt_row(out->slot[k].table, out->slot[k].row)) continue;
        for (int i = heroes[hi].first; i < heroes[hi].first + heroes[hi].count; i++)
            if (cat[i].slot == k) { out->slot[k].table = cat[i].table; out->slot[k].row = cat[i].row; break; }
    }
    out->slot[SLOT_OUTFIT].table = heroes[hi].custtable; out->slot[SLOT_OUTFIT].row = row;
    out->last = SLOT_OUTFIT;
    if (n_subst++ < 20) LOG("models: %s wears add-on outfit %s (customization screen)", heroes[hi].slug, nm);
}

// The profile component's EquipCharacterCustomizationSet(hero row handle, set) (logs "applying customization set to
// %s"; only caller: the customization screen when it closes with a changed look). Our rows never reach the profile.
#define ADDR_EQUIP VA(0x141BC3C00ull)
static const uint8_t SIG_EQUIP[] = {0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x57,0x48,0x8d,0x6c,0x24,0xe0,
                                    0x48,0x81,0xec,0x20,0x01,0x00,0x00,0x48};
typedef void (*EquipFn)(UObject *prof, RowHandle *hero, CustSet *set);
static EquipFn orig_equip;
static void equip_detour(UObject *prof, RowHandle *hero, CustSet *set) {
    int hi = set ? hero_by_def(hero) : -1;
    if (hi < 0) { orig_equip(prof, hero, set); return; }
    char nm[40];
    int wears = set->last == SLOT_OUTFIT && set->slot[SLOT_OUTFIT].table && outfit_row(set->slot[SLOT_OUTFIT].row, nm, sizeof nm);
    int any = 0;
    for (int k = 0; k < 4; k++) any |= set->slot[k].table && is_ours(set->slot[k].row);
    pick_set(heroes[hi].slug, wears ? nm : NULL);
    if (!any) { orig_equip(prof, hero, set); return; }
    // the profile keeps the previous look in every slot that names one of our rows (the screen's own set, which it
    // sends to the server next, is left as it is)
    CustSet prev, copy = *set;
    memset(&prev, 0, sizeof prev);
    UObject *pc = ue_local_pc();
    if (pc && orig_getprof) orig_getprof(pc, hero, &prev);
    uint8_t prev_last = prev.slot[prev.last & 3].table ? prev.last : SLOT_OUTFIT;   // no saved set: the default skin
    for (int k = 0; k < 4; k++) {
        if (!copy.slot[k].table || !is_ours(copy.slot[k].row)) continue;
        int keep = prev.slot[k].table && !is_ours(prev.slot[k].row) && dt_row(prev.slot[k].table, prev.slot[k].row);
        copy.slot[k].table = keep ? prev.slot[k].table : (k == SLOT_OUTFIT ? heroes[hi].defskin.table : NULL);
        copy.slot[k].row = keep ? prev.slot[k].row : (k == SLOT_OUTFIT ? heroes[hi].defskin.row : (FName){0});
        copy.slot[k].display.data = NULL; copy.slot[k].display.num = copy.slot[k].display.max = 0;
        if (copy.last == k) copy.last = keep ? prev_last : SLOT_OUTFIT;
    }
    char b[64];
    LOG("models: profile keeps %s's look (%s) instead of add-on outfit %s", heroes[hi].slug,
        (handle_str(&copy.slot[copy.last & 3], b, sizeof b), b), wears ? nm : "-");
    orig_equip(prof, hero, &copy);
}
static void tick_screen(void);
void models_tick(float dt) {
    if ((tick_acc += dt) < 0.5f) return;
    dt = tick_acc; tick_acc = 0;
    UObject *w = ue_world();
    if (!w || !ue_local_pc()) return;
    tick_npc();
    tick_screen();
    wlooks_tick(dt);
    screen_rows(dt);
    if (!is_client()) no_subst = 0;
    if (reinit_due) {   // the host refused our add-on outfit (or allows it again): send the look the profile path gives
        UObject *ps = my_ps(), *slot = ps_slot(ps);
        reinit_due = 0;
        if (!me.on && slot && !reinit_from_profile(ps, slot))
            LOG("models: sent %s", no_subst ? "the profile's own look instead" : "our look again (host allows model swaps)");
    }
    if (!me.on && !others[0].on) {   // cheap path: nothing wished (others[] is compacted on use)
        int any = 0;
        for (int i = 0; i < MAX_OTHERS; i++) any |= others[i].on;
        if (!any) return;
    }
    need_catalogue();
    tick_me(dt);
    tick_others(dt);
}

// Client: a notice from the host (chat.c). The host refused our look: stop resending until the next /model.
#define REFUSED_NOTICE "The host turned model swaps off"
#define WREFUSED_NOTICE "The host turned weapon looks off"
static int host_off = -1;          // client: the host's /models state as its notices told it (-1 unknown)
static char refusal[160];          // client: the host's last refusal of our look, until our next /model (shown in Wear)
void models_host_notice(const char *text) {
    if (me.on && !strncmp(text, REFUSED_NOTICE, sizeof REFUSED_NOTICE - 1)) { me.gave_up = 1; LOG("models: refused by the host"); }
    if (n_subst && !no_subst && !strncmp(text, REFUSED_NOTICE, sizeof REFUSED_NOTICE - 1)) { no_subst = 1; reinit_due = 1; }
    if (!strncmp(text, REFUSED_NOTICE, sizeof REFUSED_NOTICE - 1) || !strncmp(text, WREFUSED_NOTICE, sizeof WREFUSED_NOTICE - 1)) {
        snprintf(refusal, sizeof refusal, "%s", text);
        if (strstr(text, "(/models)")) host_off = 1;
    }
    if (!strncmp(text, "[host] model swaps are off", 26)) host_off = 1;
    if (!strncmp(text, "[host] model swaps are on", 25)) {
        // our add-on outfit from the customization screen was held back: put it on again now, not at the next map
        if (host_off == 1 || no_subst) reinit_due = 1;
        host_off = 0; refusal[0] = 0; no_subst = 0;
    }
    wlooks_host_notice(text);
}
int models_off(void) { return is_client() ? host_off == 1 : locked; }

// ---- host: lock ----
typedef void (*SelectSetFn)(UObject *ps, const CustSet *set);
static SelectSetFn orig_selectset;
static void selectset_detour(UObject *ps, const CustSet *set) {
    UObject *slot = ps_slot(ps);
    CustSet full;
    if (set && slot) {   // made-up outfit row without pieces: complete them before the set replicates
        need_catalogue();
        full = *set;
        for (int k = 0; k < 4; k++) full.slot[k].display.data = NULL, full.slot[k].display.num = full.slot[k].display.max = 0;
        if (complete_pieces(&full, slot_hero(slot))) set = &full;
    }
    if (set && slot && ps != my_ps()) {
        need_catalogue();
        char n[64];
        admin_ps_name(ps, n, sizeof n);
        const char *why = NULL, *tell = REFUSED_NOTICE " (/models).";
        if (!set_sane(set)) why = "not a customization row";
        else if (locked && set_foreign(set, slot_hero(slot))) why = "models are off";
        else if (set->slot[SLOT_OUTFIT].table && outfit_row(set->slot[SLOT_OUTFIT].row, NULL, 0) && !strcmp(addons_policy_name(), "none"))
            why = "add-on outfit, addons_policy=none", tell = REFUSED_NOTICE " for add-on outfits (addons_policy=none).";
        if (why) {
            n_refused++;
            LOG("models: refused a look from %s: %s", n, why);
            UObject *pc = ue_get_ptr(ps, "Owner");
            static ULONGLONG last_told; static UObject *last_pc;
            if (pc && (pc != last_pc || GetTickCount64() - last_told > 3000)) {
                admin_notice_to(pc, tell);
                last_pc = pc; last_told = GetTickCount64();
            }
            return;
        }
        char b[4][64];
        for (int k = 0; k < 4; k++) handle_str(&set->slot[k], b[k], sizeof b[k]);
        LOG("models: %s selects head=%s torso=%s legs=%s outfit=%s", n, b[0], b[1], b[2], b[3]);
    }
    if (set && slot && ps != my_ps()) note_clean(slot, set);
    orig_selectset(ps, set);
}

// FCampaignRunData::RefreshFromGameState(this, GameState, bool, bool) (host; logs "Refreshing campaign run from game
// state"): copies every slot's hero, CurrentCustomizationSet, respawn snapshot ... into the run the host's profile
// saves. For the duration of the call, slots wearing another survivor's rows get their own look back, so no swapped
// look is saved (fields written directly, no OnRep: nothing is shown or replicated in between).
#define ADDR_RUNREFRESH VA(0x1416E84D0ull)
static const uint8_t SIG_RUNREFRESH[] = {0x44,0x88,0x4c,0x24,0x20,0x44,0x88,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x55,
                                         0x41,0x54,0x41,0x57,0x48,0x8d,0x6c,0x24,0xb9};
typedef uint64_t (*RunRefreshFn)(void *run, void *gs, uint64_t a, uint64_t b);
static RunRefreshFn orig_runrefresh;
static void swap_fields(CustSet *cur, CustSet *other) {   // (table, row) x4 + last only; display strings stay put
    for (int k = 0; k < 4; k++) {
        RowHandle t = cur->slot[k];
        cur->slot[k].table = other->slot[k].table; cur->slot[k].row = other->slot[k].row;
        other->slot[k].table = t.table; other->slot[k].row = t.row;
    }
    uint8_t l = cur->last; cur->last = other->last; other->last = l;
}
static uint64_t runrefresh_detour(void *run, void *gs, uint64_t a, uint64_t b) {
    UObject **s; int n = hero_slots(&s), k = 0;
    struct { CustSet *cur; CustSet other; } swapped[16];
    if (n_cat) {
        for (int i = 0; i < n && k < 16; i++) {
            CustSet *cur = slot_set(s[i]);
            if (!cur || !set_foreign(cur, slot_hero(s[i])) || clean_of(s[i], &swapped[k].other)) continue;
            swapped[k].cur = cur;
            swap_fields(cur, &swapped[k].other);
            k++;
        }
    }
    uint64_t r = orig_runrefresh(run, gs, a, b);
    for (int i = 0; i < k; i++) swap_fields(swapped[i].cur, &swapped[i].other);
    if (k) LOG("models: campaign run saved with %d survivor(s) in their own look", k);
    return r;
}

// Undo every look with another survivor's rows (host, /models off).
static int reset_foreign_all(void) {
    UObject **s; int n = hero_slots(&s), done = 0;
    for (int i = 0; i < n; i++) {
        CustSet *cur = slot_set(s[i]);
        if (!cur || !set_foreign(cur, slot_hero(s[i]))) continue;
        UObject *ps = slot_owner(s[i]);
        CustSet d;
        if (!is_bot_slot(s[i]) && ps) { if (!reinit_from_profile(ps, s[i])) done++; }
        else if (!default_set(s[i], &d) && !host_write(s[i], &d)) done++;
    }
    return done;
}

// ---- commands ----
// Resolve a name: an entry ("holly_elite_04"), or a survivor ("holly": its default skin).
static int resolve(const char *name, Want *w, char *label, size_t n) {
    need_catalogue();
    memset(w->pick, 0, sizeof w->pick); memset(w->has, 0, sizeof w->has); w->npc = 0;
    for (int i = 0; i < n_cat; i++)
        if (!_stricmp(cat[i].name, name)) { want_pick(w, &cat[i]); snprintf(label, n, "%s", cat[i].name); return 0; }
    int hi = hero_by_slug(name);
    if (hi >= 0 && !want_hero(w, hi)) { snprintf(label, n, "%s", heroes[hi].slug); return 0; }
    Outf *of = outfit_by_name(name);
    if (of && !of->bad) {
        char row[80];
        snprintf(row, sizeof row, OUTFIT_PREFIX "%s", of->name);
        w->pick[SLOT_OUTFIT].table = NULL; w->pick[SLOT_OUTFIT].row = make_name(row); w->has[SLOT_OUTFIT] = 1; w->npc = 1;
        snprintf(label, n, "%s", of->name);
        return 0;
    }
    Npc *np = npc_by_name(name);
    if (np && !np->bad) {
        char row[80];
        snprintf(row, sizeof row, NPC_PREFIX "%s", np->name);
        w->pick[SLOT_OUTFIT].table = NULL; w->pick[SLOT_OUTFIT].row = make_name(row); w->has[SLOT_OUTFIT] = 1; w->npc = 1;
        snprintf(label, n, "%s", np->name);
        return 0;
    }
    return -1;
}

static void list(const char *cat_arg, Out *o) {
    need_catalogue();
    if (!n_heroes) { out_printf(o, "no survivor data loaded yet\n"); return; }
    if (cat_arg && (!_stricmp(cat_arg, "outfits") || !_stricmp(cat_arg, "outfit") || !_stricmp(cat_arg, "addons"))) {
        if (n_outfs < 0) build_outfits();
        if (!n_outfs) { out_printf(o, "no add-on outfits (add-ons with outfits: /addons)\n"); return; }
        out_printf(o, "add-on outfits (seen by players who have the add-on; others see your survivor):\n");
        for (int i = 0; i < n_outfs; i++)
            out_printf(o, " %s: %s (%s)%s\n", outfs[i].name, outfs[i].title, outfs[i].addon, outfs[i].bad ? " NOT USABLE" : "");
        return;
    }
    if (cat_arg && (!_stricmp(cat_arg, "npc") || !_stricmp(cat_arg, "npcs"))) {
        if (n_npcs < 0) build_npcs();
        char line[200]; size_t k = snprintf(line, sizeof line, "NPC bodies (seen by players with b4bcoop):");
        for (int i = 0; i < n_npcs; i++) {
            if (npcs[i].bad) continue;
            if (k + strlen(npcs[i].name) + 2 > 90) { out_printf(o, "%s\n", line); k = snprintf(line, sizeof line, " "); }
            k += snprintf(line + k, sizeof line - k, " %s", npcs[i].name);
        }
        out_printf(o, "%s\n", line);
        return;
    }
    if (cat_arg && (!_stricmp(cat_arg, "weapons") || !_stricmp(cat_arg, "weapon"))) { wlooks_list(o); return; }
    int hi = cat_arg && *cat_arg ? hero_by_slug(cat_arg) : -1;
    if (hi < 0 && cat_arg && *cat_arg && strcmp(cat_arg, "all")) {
        out_printf(o, "no survivor '%s'. /model list shows them\n", cat_arg);
        return;
    }
    if (hi < 0 && !(cat_arg && !strcmp(cat_arg, "all"))) {   // overview
        char line[200]; size_t k = 0;
        out_printf(o, "survivors (/model <name> = their look, /model list <name> = their outfits):\n");
        for (int i = 0; i < n_heroes; i++) {
            if (!heroes[i].count) continue;
            k += snprintf(line + k, sizeof line - k, "%s%s(%d)", k ? " " : "", heroes[i].slug, heroes[i].count);
            if (k > 70) { out_printf(o, "%s\n", line); k = 0; }
        }
        if (k) out_printf(o, "%s\n", line);
        out_printf(o, "/model list npc: Fort Hope NPCs, survivors, cultists\n");
        if (n_outfs < 0) build_outfits();
        if (n_outfs) {
            k = snprintf(line, sizeof line, "add-on outfits (/model list outfits):");
            for (int i = 0; i < n_outfs; i++) {
                if (k + strlen(outfs[i].name) + 2 > 90) { out_printf(o, "%s\n", line); k = snprintf(line, sizeof line, " "); }
                k += snprintf(line + k, sizeof line - k, " %s", outfs[i].name);
            }
            out_printf(o, "%s\n", line);
        }
        wlooks_overview(o);
        return;
    }
    static const int order[4] = {SLOT_OUTFIT, SLOT_HEAD, SLOT_TORSO, SLOT_LEGS};
    static const char *group[4] = {"outfits", "heads", "torsos", "legs"};
    for (int h = 0; h < n_heroes; h++) {
        if (hi >= 0 && h != hi) continue;
        if (hi < 0) out_printf(o, "%s:\n", heroes[h].slug);
        size_t sl = strlen(heroes[h].slug);
        for (int g = 0; g < 4; g++) {
            // this group's names, sorted, without the "<hero>_" prefix
            const char *nm[96]; int n = 0;
            for (int i = heroes[h].first; i < heroes[h].first + heroes[h].count && n < 96; i++) {
                if (cat[i].slot != order[g]) continue;
                const char *x = cat[i].name;
                if (!strncmp(x, heroes[h].slug, sl) && x[sl] == '_') x += sl + 1;
                int j = n++;
                for (; j > 0 && strcmp(nm[j - 1], x) > 0; j--) nm[j] = nm[j - 1];
                nm[j] = x;
            }
            if (!n) continue;
            char line[200]; size_t k = snprintf(line, sizeof line, " %s:", group[g]);
            for (int i = 0; i < n; i++) {
                if (k + strlen(nm[i]) + 2 > 90) { out_printf(o, "%s\n", line); k = snprintf(line, sizeof line, "   "); }
                k += snprintf(line + k, sizeof line - k, " %s", nm[i]);
            }
            out_printf(o, "%s\n", line);
        }
    }
    if (hi >= 0) out_printf(o, "use it as /model %s_<name>\n", heroes[hi].slug);
}

static void status(Out *o) {
    UObject *slot = ps_slot(my_ps());
    CustSet *cur = slot_set(slot);
    need_catalogue();
    if (cur) {
        char b[4][64];
        for (int k = 0; k < 4; k++) handle_str(&cur->slot[k], b[k], sizeof b[k]);
        out_printf(o, "you: outfit=%s head=%s torso=%s legs=%s\n", b[3], b[0], b[1], b[2]);
    }
    out_printf(o, "your model: %s%s\n", me.on ? me.label : "(game's own)", me.on && me.gave_up ? " (refused by the host)" : "");
    wlooks_status(o);
    if (!is_client()) {
        for (int i = 0; i < MAX_OTHERS; i++) if (others[i].on) out_printf(o, "forced: %s = %s\n", others[i].key, others[i].label);
        out_printf(o, "models are %s for players (/models on|off)\n", locked ? "OFF" : "on");
    }
}

static void reset_me(Out *o) {
    UObject *ps = my_ps(), *slot = ps_slot(ps);
    me.on = 0;
    refusal[0] = 0;
    wlooks_reset(o);
    if (!slot) { out_printf(o, "model reset (applies when you have a survivor)\n"); return; }
    if (reinit_from_profile(ps, slot)) { out_printf(o, "reset failed\n"); return; }
    out_printf(o, "back to your own look\n");
}

static void notify_all(const char *text) {
    UObject *gs = ue_world() ? ue_get_ptr(ue_world(), "GameState") : NULL, *me_pc = ue_local_pc();
    int32_t off = gs ? ue_prop_offset(gs, "PlayerArray") : -1;
    if (off < 0) return;
    TArray *pa = (TArray *)((char *)gs + off);
    for (int i = 0; i < pa->num; i++) {
        UObject *ps = ((UObject **)pa->data)[i], *pc = ps ? ue_get_ptr(ps, "Owner") : NULL;
        if (pc && pc != me_pc && !is_bot_slot(ps_slot(ps))) admin_notice_to(pc, text);
    }
}

// Host: the key of a wish on that player (their player key) or bot (its hero slot).
static void want_key(UObject *ps, UObject *slot, char *key, size_t n) {
    if (is_bot_slot(slot)) {
        UObject **s; int k = hero_slots(&s), idx = -1;
        for (int i = 0; i < k; i++) if (s[i] == slot) idx = i;
        snprintf(key, n, "slot:%d", idx);
    } else admin_ps_key(ps, key, n);
}

// /model <player> <name|reset> (host)
static void model_other(const char *who, const char *what, Out *o) {
    if (is_client()) { out_printf(o, "/model <player> <name>: host only\n"); return; }
    UObject *ps = admin_find_player(who, o);
    if (!ps) return;
    UObject *slot = ps_slot(ps);
    if (!slot) { out_printf(o, "that player has no survivor yet\n"); return; }
    char key[80], nm[64];
    admin_ps_name(ps, nm, sizeof nm);
    if (is_bot_slot(slot) && slot_hero(slot) >= 0) snprintf(nm, sizeof nm, "%s (bot)", heroes[slot_hero(slot)].slug);
    want_key(ps, slot, key, sizeof key);
    Want *w = NULL, *free_w = NULL;
    for (int i = 0; i < MAX_OTHERS; i++) {
        if (others[i].on && !strcmp(others[i].key, key)) w = &others[i];
        if (!others[i].on && !free_w) free_w = &others[i];
    }
    if (ps == my_ps()) { out_printf(o, "that's you: /model <name>\n"); return; }
    if (!_stricmp(what, "reset")) {
        if (w) w->on = 0;
        CustSet d;
        if (!is_bot_slot(slot)) reinit_from_profile(ps, slot);
        else if (!default_set(slot, &d)) host_write(slot, &d);
        out_printf(o, "%s: back to their own look\n", nm);
        return;
    }
    if (locked) { out_printf(o, "models are off (/models on)\n"); return; }
    Want nw = {0};
    char label[48];
    if (resolve(what, &nw, label, sizeof label)) { out_printf(o, "no model '%s' (/model list)\n", what); return; }
    if (!w) w = free_w;
    if (!w) { out_printf(o, "too many forced models\n"); return; }
    *w = nw;
    w->on = 1;
    snprintf(w->key, sizeof w->key, "%s", key);
    snprintf(w->label, sizeof w->label, "%s", label);
    char line[160];
    snprintf(line, sizeof line, "[host] %s now looks like %s", nm, label);
    notify_all(line);
    out_printf(o, "%s now looks like %s\n", nm, label);
    LOG("models: host forces %s on %s (%s)", label, nm, key);
}

// Chat /model and /models, dev CLI `model` / `models` (same code).
void models_slash(const char *verb, char *rest, Out *o) {
    char *a = rest ? strtok(rest, " ") : NULL, *b = a ? strtok(NULL, " ") : NULL;
    if (!strcmp(verb, "models")) {
        if (!a) { status(o); return; }
        if (is_client()) { out_printf(o, "/models on|off: host only\n"); return; }
        if (!_stricmp(a, "off")) {
            locked = 1;
            for (int i = 0; i < MAX_OTHERS; i++) others[i].on = 0;
            int n = reset_foreign_all() + wlooks_lock_reset();
            if (me.on) reset_me(o);
            notify_all("[host] model swaps are off");
            out_printf(o, "model swaps off for everyone (%d look(s) reset)\n", n);
            LOG("models: locked, %d reset", n);
        } else if (!_stricmp(a, "on")) {
            locked = 0;
            notify_all("[host] model swaps are on (/model list)");
            out_printf(o, "model swaps on\n");
            LOG("models: unlocked");
        } else out_printf(o, "usage: /models [on|off]\n");
        return;
    }
    if (!a || !_stricmp(a, "status")) { status(o); return; }
    if (!_stricmp(a, "list")) { list(b, o); return; }
    if (!_stricmp(a, "reset") && !b) { reset_me(o); return; }
    if (b) { model_other(a, b, o); return; }
    if (locked && !is_client()) { out_printf(o, "models are off (/models on)\n"); return; }
    Want nw = {0};
    char label[48];
    if (resolve(a, &nw, label, sizeof label)) {
        if (!wlooks_pick(a, o)) out_printf(o, "no model '%s' (/model list)\n", a);
        else refusal[0] = 0;
        return;
    }
    refusal[0] = 0;
    // pieces add up (/model holly_head_03 then /model walker_legs_01); an outfit or a whole survivor replaces the wish
    if (me.on && !nw.has[SLOT_OUTFIT] && !me.has[SLOT_OUTFIT]) {
        for (int k = 0; k < 3; k++) if (nw.has[k]) { me.pick[k] = nw.pick[k]; me.has[k] = 1; }
        size_t l = strlen(me.label);
        if (l + strlen(label) + 2 < sizeof me.label) snprintf(me.label + l, sizeof me.label - l, "+%s", label);
        me.tries = 0; me.gave_up = 0; me.wait = 0;
        out_printf(o, "you now look like %s (/model reset to undo)\n", me.label);
        LOG("models: /model %s", me.label);
        tick_acc = 1;
        return;
    }
    me = nw;
    me.on = 1;
    // Same slot and hero as now: no settle wait (tick_me waits 3 s on a new slot/hero only, for the game's own sends;
    // a fresh wish used to look like a new slot, so every /model and every Models-tab click took 2-3 s to show).
    UObject *cur_slot = ps_slot(my_ps());
    if (ready_slot(cur_slot)) me.slot_idx = U_INDEX(cur_slot) * 64 + slot_hero(cur_slot);
    snprintf(me.label, sizeof me.label, "%s", label);
    out_printf(o, "you now look like %s (/model reset to undo)\n", label);
    if (nw.npc && outfit_by_name(label)) out_printf(o, "(an add-on outfit: players without that add-on see your survivor)\n");
    LOG("models: /model %s", label);
    tick_acc = 1;   // apply on the next tick
}

// ---- ~ window, Add-ons tab (looks.c): an outfit row's Wear opens the game's customization screen with the outfit on
// (the #33 rows), and the model swaps part of Host rules. ----
static UObject *live_at(const char *cls, int32_t *idx) {   // the newest live object of that class (not its CDO)
    UClass *c = ue_find_class(cls);
    for (int32_t i = ue_num_objects() - 1; c && i >= 0; i--) {
        UObject *x = ue_object_at(i);
        if (is_live(x) && ue_is_a(x, c) && x != UC_CDO(U_CLASS(x))) { if (idx) *idx = i; return x; }
    }
    return NULL;
}
static UObject *live_of(const char *cls) { return live_at(cls, NULL); }
// The screen opened by models_open_screen: the game's own way there (a usable in Fort Hope) also leaves it by itself;
// ours enters the customization view (EnterCharacterCustomization: the camera on the screen's mannequin), so we leave
// it once the screen is closed (Back / Esc), else the camera stays there.
static UObject *our_screen, *our_mgr, *our_ps;
static int32_t our_screen_idx, our_mgr_idx;
static void tick_screen(void) {
    if (!our_screen) return;
    int open = 0;
    if (ue_object_at(our_screen_idx) == our_screen && is_live(our_screen)) {
        UFunction *f = fn_of(our_screen, "IsOpen");
        uint8_t p[16] = {0};
        if (f) { ue_process_event(our_screen, f, p); open = p[0] & 1; }
    }
    if (open) return;
    UFunction *x = ue_object_at(our_mgr_idx) == our_mgr && is_live(our_mgr) ? fn_of(our_mgr, "ExitCharacterCustomization") : NULL;
    int32_t xo = parm_off(x, "GobiPlayerState");
    if (x && xo >= 0 && our_ps == my_ps()) {
        uint8_t p[64] = {0};
        *(UObject **)(p + xo) = our_ps;
        ue_process_event(our_mgr, x, p);
    }
    // the view: the screen's camera (Character_CineCameraActor) stays the view target; back to our hero (the usable
    // that normally leads there does this when its screen closes)
    UObject *pc = ue_local_pc(), *pawn = pc ? ue_get_ptr(pc, "Pawn") : NULL, *vt = NULL;
    UFunction *gv = fn_of(pc, "GetViewTarget"), *sv = fn_of(pc, "SetViewTargetWithBlend");
    int32_t gr = parm_off(gv, "ReturnValue"), st = parm_off(sv, "NewViewTarget");
    static UClass *cam;
    if (!cam) cam = ue_find_class("CameraActor");
    if (gv && gr >= 0) { uint8_t p[32] = {0}; ue_process_event(pc, gv, p); vt = *(UObject **)(p + gr); }
    int view = pawn && sv && st >= 0 && UFN_PARMSSIZE(sv) <= 64 && vt && vt != pawn && cam && ue_is_a(vt, cam);
    if (view) { uint8_t p[64] = {0}; *(UObject **)(p + st) = pawn; ue_process_event(pc, sv, p); }
    LOG("models: customization screen closed: %s%s", x ? "left the customization view" : "its manager is gone",
        view ? ", view back on the hero" : "");
    our_screen = our_mgr = our_ps = NULL;
}
// Fort Hope's customization manager (the Wear button asks every frame: looked up once a second)
static UObject *screen_mgr(void) {
    static ULONGLONG t;
    static UObject *mgr, *world;
    UObject *w = ue_world();
    if (w != world || GetTickCount64() - t > 1000) {
        char pkg[256];
        world = w; t = GetTickCount64();
        mgr = w && strstr(ue_world_package(w, pkg, sizeof pkg), "FortHope") ? live_of("CharacterCustomizationManager") : NULL;
    }
    return mgr;
}
const char *models_screen_why_not(void) {
    if (screen_off) return "The customization screen has no add-on outfits here (outfits_screen=0 in b4bcoop.ini).";
    UObject *slot = ps_slot(my_ps());
    if (!screen_mgr() || !slot || slot_hero(slot) < 0)
        return "Wear it from the character customization screen in Fort Hope.";
    return NULL;
}
const char *models_open_screen(const char *outfit) {
    const char *why = models_screen_why_not();
    if (why) return why;
    UObject *ps = my_ps(), *slot = ps_slot(ps), *mgr = screen_mgr();
    int32_t ho = ue_prop_offset(slot, "CurrentHeroRowHandle");
    UFunction *f = fn_of(mgr, "EnterCharacterCustomization");
    int32_t fo = parm_off(f, "GobiPlayerState");
    if (!f || fo < 0 || ho < 0) return "The customization screen could not be opened (see the log).";
    uint8_t p[64] = {0};
    *(UObject **)(p + fo) = ps;
    ue_process_event(mgr, f, p);
    game_exec("OpenScreen CharacterCustomization 0");
    int32_t sidx = 0, midx = 0;
    UObject *scr = live_at("CharacterCustomizationScreen", &sidx);
    UObject *m2 = live_at("CharacterCustomizationManager", &midx);
    our_screen = scr; our_screen_idx = sidx; our_mgr = m2; our_mgr_idx = midx; our_ps = ps;   // tick_screen leaves it
    UFunction *g = fn_of(scr, "SetCharacter");
    int32_t go = parm_off(g, "CharacterRowHandle");
    if (!g || go < 0) { LOG("models: customization screen: no screen (%p)", (void *)scr); return "The customization screen could not be opened (see the log)."; }
    memset(p, 0, sizeof p);
    RowHandle *h = (RowHandle *)(p + go), *src = (RowHandle *)((char *)slot + ho);
    h->table = src->table; h->row = src->row;
    ue_process_event(scr, g, p);
    int hi = slot_hero(slot);
    char rn[64];
    snprintf(rn, sizeof rn, OUTFIT_PREFIX "%s", outfit ? outfit : "");
    UFunction *e = outfit ? fn_of(scr, "EquipCustomization") : NULL;
    int32_t eo = parm_off(e, "RowName"), so = parm_off(e, "InSlot");
    int shown = 0;
    if (e && eo >= 0 && so >= 0 && dt_row(heroes[hi].custtable, make_name(rn))) {   // shown on the screen's mannequin
        memset(p, 0, sizeof p);
        p[so] = SLOT_OUTFIT;
        *(FName *)(p + eo) = make_name(rn);
        ue_process_event(scr, e, p);
        shown = 1;
    }
    LOG("models: customization screen opened for %s (~ window)%s%s", heroes[hi].slug, shown ? ", showing " : "", shown ? rn : "");
    overlay_close();
    return NULL;
}

// Host rules (looks.c, the bottom of the Add-ons tab): model swaps allowed (/models on|off), one line.
void models_host_panel(void) {
    int client = is_client();
    ov_begin_perm(CMD_HOST);
    int on = client ? host_off != 1 : !locked;   // a client shows what the host's notices said
    if (ov_checkbox("Model swaps allowed##models", &on)) ov_run(on ? "models on" : "models off");
    ov_tooltip(client ? "The host's setting, as it last announced it. Off (/models off): nobody can wear another "
                        "survivor's outfit, an NPC body, an add-on outfit or a weapon look."
                      : "Off (/models off): every swapped look goes back to normal; nobody can wear another survivor's "
                        "outfit, an NPC body, an add-on outfit or a weapon look. A player's own survivor's outfits stay "
                        "allowed. /models shows the state.");
    ov_end_perm();
}

#ifndef B4B_RELEASE
// ---- dev: exploration (docs/investigations/model-swap.md) ----
static UObject *nth_hero(int idx) {
    static UClass *hc;
    if (!hc) hc = ue_find_class("HeroCharacter");
    UObject *w = ue_world();
    int32_t n = ue_num_objects(); int k = 0;
    for (int32_t i = 0; hc && w && i < n; i++) {
        UObject *o = ue_object_at(i);
        if (!is_live(o) || !ue_is_a(o, hc)) continue;
        UObject *lvl = U_OUTER(o);
        if (!lvl || U_OUTER(lvl) != w) continue;
        if (k++ == idx) return o;
    }
    return NULL;
}

static void dump_comps(UObject *actor, Out *o) {
    static UClass *smc, *mc;
    if (!smc) smc = ue_find_class("SkinnedMeshComponent");
    if (!mc) mc = ue_find_class("MeshComponent");
    int32_t n = ue_num_objects();
    char a[128], b[300], c[200], d[200], e[200];
    for (int32_t i = 0; mc && i < n; i++) {
        UObject *x = ue_object_at(i);
        if (!is_live(x) || U_OUTER(x) != actor || !ue_is_a(x, mc)) continue;
        out_printf(o, "    %s [%s]", ue_obj_name(x, a, sizeof a), ue_obj_name(U_CLASS(x), c, sizeof c));
        if (smc && ue_is_a(x, smc)) {
            UObject *m = ue_get_ptr(x, "SkeletalMesh");
            UObject *sk = m ? ue_get_ptr(m, "Skeleton") : NULL, *pa = m ? ue_get_ptr(m, "PhysicsAsset") : NULL;
            UObject *ac = ue_get_ptr(x, "AnimClass");
            int32_t mo = ue_prop_offset(x, "MasterPoseComponent");
            UObject *mp = mo >= 0 ? ue_weak_get((char *)x + mo) : NULL;
            out_printf(o, " mesh=%s skel=%s phys=%s anim=%s master=%s", m ? ue_full_path(m, b, sizeof b) : "-",
                       sk ? ue_obj_name(sk, d, sizeof d) : "-", pa ? ue_obj_name(pa, e, sizeof e) : "-",
                       ac ? ue_obj_name(ac, c, sizeof c) : "-", mp ? ue_obj_name(mp, a, sizeof a) : "-");
        }
        int32_t om = ue_prop_offset(x, "OverrideMaterials");
        if (om >= 0) out_printf(o, " overrides=%d", ((TArray *)((char *)x + om))->num);
        out_printf(o, "\n");
    }
}

static void cmd_dump(Out *o) {
    need_catalogue();
    char a[128], b[300];
    for (int i = 0; ; i++) {
        UObject *h = nth_hero(i);
        if (!h) break;
        int32_t ro = ue_prop_offset(h, "Role"), hd = ue_prop_offset(h, "HeroDefinitionRowHandle");
        UObject *slot = ue_get_ptr(h, "OccupiedPlayerSlot");
        out_printf(o, "#%d %s role=%d hero=", i, ue_obj_name(h, a, sizeof a), ro >= 0 ? *((uint8_t *)h + ro) : -1);
        if (hd >= 0) { RowHandle *r = (RowHandle *)((char *)h + hd); out_printf(o, "%s", r->table ? ue_name(r->row, b, sizeof b) : "-"); }
        out_printf(o, " slot=%s herocust=%d\n", slot ? ue_obj_name(slot, a, sizeof a) : "-", slot_hero(slot));
        CustSet *s = slot_set(slot);
        if (s) {
            for (int k = 0; k < 4; k++) { handle_str(&s->slot[k], b, sizeof b); out_printf(o, "    set.%s %s\n", SLOTN[k], b); }
            out_printf(o, "    set.last %d\n", s->last);
        }
        dump_comps(h, o);
    }
}

static void cmd_rows(const char *filter, Out *o) {
    build_catalogue();
    char b[64], c[160];
    for (int i = 0; i < n_heroes; i++) {
        handle_str(&heroes[i].defskin, c, sizeof c);
        out_printf(o, "hero %s custtable=%s rows=%d defskin=%s\n", heroes[i].slug,
                   heroes[i].custtable ? ue_obj_name(heroes[i].custtable, b, sizeof b) : "-", heroes[i].count, c);
    }
    for (int i = 0; i < n_cat; i++) {
        const Entry *e = &cat[i];
        if (filter && *filter && !strstr(e->name, filter) && strcmp(heroes[e->hero].slug, filter)) continue;
        uint8_t *r = dt_row(e->table, e->row);
        char fp[160];
        ue_name(HMD_MESHPATH(CC_FP(r)), fp, sizeof fp);
        out_printf(o, "%-28s %-8s %-6s row=%s mats=%d/%d 3p=%s fp=%s\n", e->name, heroes[e->hero].slug, SLOTN[e->slot],
                   ue_name(e->row, b, sizeof b), HMD_MATS(CC_3P(r))->num, HMD_MATS(CC_FP(r))->num, e->mesh3p, fp);
    }
}

// #33: rows, picks, substitutions; #37: actors that wear a set besides heroes (cutscene stand-ins, mannequins)
static void cmd_screen(Out *o) {
    if (n_picks < 0) picks_load();
    out_printf(o, "screen rows: %s, added %d, failed %d; profile looks answered with an add-on outfit: %d%s; slot applies: %d\n",
               screen_off ? "off" : "on", n_rows_added, n_rows_failed, n_subst, no_subst ? " (host refused: off)" : "", n_slot_applied);
    for (int i = 0; i < n_picks; i++) out_printf(o, "pick: %s %s%s\n", picks[i].hero, picks[i].outfit, outfit_by_name(picks[i].outfit) ? "" : " (add-on not here)");
    char b[64], rn[64];
    for (int h = 0; h < n_heroes; h++) {
        int k = 0;
        for (int i = 0; i < n_outfs; i++) { snprintf(rn, sizeof rn, OUTFIT_PREFIX "%s", outfs[i].name); k += dt_row(heroes[h].custtable, make_name(rn)) != NULL; }
        out_printf(o, "  %s: %s %d rows (%d ours)\n", heroes[h].slug, heroes[h].custtable ? ue_obj_name(heroes[h].custtable, b, sizeof b) : "-",
                   heroes[h].custtable ? ((TArray *)DT_ROWMAP(heroes[h].custtable))->num : 0, k);
    }
}
static void cmd_standins(const char *cls, Out *o) {
    const char *names[] = {"PlayerStandIn", "CustomizationMannequin"};
    const char *props[] = {"CustomizationSet", "DesiredCustomizationSet"};
    char a[128], b[64];
    need_catalogue();
    for (int c = 0; c < 2; c++) {
        if (cls && *cls && _stricmp(cls, names[c])) continue;
        UClass *k = ue_find_class(names[c]);
        for (int32_t i = 0, n = ue_num_objects(); k && i < n; i++) {
            UObject *x = ue_object_at(i);
            if (!is_live(x) || !ue_is_a(x, k)) continue;
            int32_t so = ue_prop_offset(x, props[c]), hd = ue_prop_offset(x, "bHidden");
            out_printf(o, "%s [%s] hidden=%d\n", ue_full_path(x, a, sizeof a), ue_obj_name(U_CLASS(x), b, sizeof b), hd >= 0 ? *((uint8_t *)x + hd) & 1 : -1);
            if (so >= 0) {
                CustSet *st = (CustSet *)((char *)x + so);
                for (int s2 = 0; s2 < 4; s2++) { char hb[96]; handle_str(&st->slot[s2], hb, sizeof hb); out_printf(o, "    set.%s %s\n", SLOTN[s2], hb); }
                out_printf(o, "    set.last %d\n", st->last);
            }
            dump_comps(x, o);
        }
    }
}

// mdl cs open | list | equip <row> | close: the customization screen without walking to it (dev; #33 tests)
static void cmd_cs(const char *a, const char *arg, Out *o) {
    UObject *ps = my_ps(), *slot = ps_slot(ps), *mgr = live_of("CharacterCustomizationManager");
    UObject *scr = live_of("CharacterCustomizationScreen");
    if (!strcmp(a, "open")) {   // as the ~ window's Wear (tick_screen leaves the view when it closes); arg: an outfit
        const char *e = models_open_screen(arg);
        if (e) out_printf(o, "%s\n", e);
        else out_printf(o, "screen open for %s\n", slot_hero(slot) >= 0 ? heroes[slot_hero(slot)].slug : "?");
        return;
    }
    if (!scr) { out_printf(o, "no screen\n"); return; }
    if (!strcmp(a, "list")) {
        UFunction *f = fn_of(scr, "GetCustomizations");
        uint8_t p[64] = {0};
        if (!f) return;
        p[parm_off(f, "InSlot")] = SLOT_OUTFIT;
        ue_process_event(scr, f, p);
        TArray *arr = (TArray *)(p + parm_off(f, "ReturnValue"));   // UICustomizationData (0x340), left as is (dev)
        UClass *k = ue_find_class("KismetTextLibrary");
        UFunction *t = k ? fn_of(UC_CDO(k), "Conv_TextToString") : NULL;
        for (int i = 0; i < arr->num; i++) {
            uint8_t *e = (uint8_t *)arr->data + (size_t)i * 0x340;
            char rn[80], nm[96] = "?";
            if (t) {
                uint8_t q[64] = {0};
                memcpy(q + parm_off(t, "InText"), e + 8 + 8, 0x18);
                ue_process_event(UC_CDO(k), t, q);
                FString *fs = (FString *)(q + parm_off(t, "ReturnValue"));
                int j = 0;
                for (; fs->data && j < fs->num && fs->data[j] && j < 95; j++) nm[j] = fs->data[j] < 128 ? (char)fs->data[j] : '?';
                nm[j] = 0;
            }
            out_printf(o, "%2d %-40s \"%s\" locked=%d dlc=%d\n", i, ue_name(*(FName *)e, rn, sizeof rn), nm, e[0x328], e[0x329]);
        }
        return;
    }
    if (!strcmp(a, "equip") && arg) {
        UFunction *f = fn_of(scr, "EquipCustomization");
        uint8_t p[64] = {0};
        if (!f) return;
        p[parm_off(f, "InSlot")] = SLOT_OUTFIT;
        *(FName *)(p + parm_off(f, "RowName")) = make_name(arg);
        ue_process_event(scr, f, p);
        out_printf(o, "equipped %s on the screen\n", arg);
        return;
    }
    if (!strcmp(a, "close")) {
        game_exec("CloseScreen CharacterCustomization 0");
        if (our_screen) { out_printf(o, "closed (tick_screen leaves the customization view)\n"); return; }
        UFunction *f = fn_of(mgr, "ExitCharacterCustomization");
        uint8_t p[64] = {0};
        if (f) { *(UObject **)(p + parm_off(f, "GobiPlayerState")) = ps; ue_process_event(mgr, f, p); }
        out_printf(o, "closed\n");
    }
}

// mdl dump | rows [filter] | setslot <hero#> <entry> | rpc <entry> | reinit | mesh <hero#> <comp> <path> | load <path>
//     | skm [filter] | reg [filter] [class]
int models_cmd(const char *verb, char *rest, Out *o) {
    if (!strcmp(verb, "model") || !strcmp(verb, "models")) { models_slash(verb, rest, o); return 1; }
    if (!strcmp(verb, "wlook")) return wlooks_cmd(verb, rest, o);
    if (strcmp(verb, "mdl")) return 0;
    char *sub = rest ? strtok(rest, " ") : NULL, *a1 = sub ? strtok(NULL, " ") : NULL, *a2 = a1 ? strtok(NULL, " ") : NULL;
    char *a3 = a2 ? strtok(NULL, "") : NULL;
    if (!sub || !strcmp(sub, "dump")) { cmd_dump(o); return 1; }
    if (!strcmp(sub, "rows")) { cmd_rows(a1, o); return 1; }
    if (!strcmp(sub, "screen")) {
        if (a1 && !strcmp(a1, "retry")) { memset(done, 0, sizeof done); rows_acc = 0; for (int i = 0; i < n_outfs; i++) outfs[i].rowfail = 0; }
        cmd_screen(o);
        return 1;
    }
    if (!strcmp(sub, "standins")) { cmd_standins(a1, o); return 1; }
    if (!strcmp(sub, "cs") && a1) { cmd_cs(a1, a2, o); return 1; }
    if (!strcmp(sub, "setslot") && a2) {
        UObject *h = nth_hero(atoi(a1)), *slot = h ? ue_get_ptr(h, "OccupiedPlayerSlot") : NULL;
        Want w = {0}; char label[48];
        CustSet *cur = slot_set(slot);
        if (!cur || resolve(a2, &w, label, sizeof label)) { out_printf(o, "no hero/slot/entry\n"); return 1; }
        CustSet s; compose(cur, &w, slot_hero(slot), &s);
        out_printf(o, "host_write %s: %d\n", label, host_write(slot, &s));
        return 1;
    }
    if (!strcmp(sub, "rpc") && a1) {
        UObject *ps = my_ps(), *slot = ps_slot(ps);
        Want w = {0}; char label[48];
        CustSet *cur = slot_set(slot);
        if (!cur || resolve(a1, &w, label, sizeof label)) { out_printf(o, "no slot/entry\n"); return 1; }
        CustSet s; compose(cur, &w, slot_hero(slot), &s);
        out_printf(o, "ServerSelectCustomizationSet %s: %d\n", label, send_select(ps, &s));
        return 1;
    }
    if ((!strcmp(sub, "look") || !strcmp(sub, "bring")) && a1) {   // test views: face hero #, or (host) put it in front of us
        UObject *h = nth_hero(atoi(a1)), *pc = ue_local_pc(), *me_pawn = pc ? ue_get_ptr(pc, "Pawn") : NULL;
        UFunction *gl = fn_of(h, "K2_GetActorLocation"), *gr = fn_of(pc, "GetControlRotation");
        if (!h || !me_pawn || !gl || !gr) { out_printf(o, "no hero/pawn\n"); return 1; }
        uint8_t p[256] = {0};
        float hl[3], ml[3], rot[3];
        ue_process_event(h, gl, p); memcpy(hl, p + parm_off(gl, "ReturnValue"), 12);
        memset(p, 0, sizeof p); ue_process_event(me_pawn, gl, p); memcpy(ml, p + parm_off(gl, "ReturnValue"), 12);
        memset(p, 0, sizeof p); ue_process_event(pc, gr, p); memcpy(rot, p + parm_off(gr, "ReturnValue"), 12);
        float dist = a2 ? (float)atof(a2) : 300.f;
        if (!strcmp(sub, "bring")) {
            float yaw = rot[1] * 3.14159265f / 180.f;
            float to[3] = {ml[0] + dist * cosf(yaw), ml[1] + dist * sinf(yaw), ml[2]};
            float face[3] = {0, rot[1] + 180.f, 0};
            UFunction *f = fn_of(h, "K2_SetActorLocationAndRotation");
            memset(p, 0, sizeof p);
            memcpy(p + parm_off(f, "NewLocation"), to, 12);
            memcpy(p + parm_off(f, "NewRotation"), face, 12);
            if (parm_off(f, "bTeleport") >= 0) p[parm_off(f, "bTeleport")] = 1;
            ue_process_event(h, f, p);
            out_printf(o, "hero %s at %.0f %.0f %.0f\n", a1, to[0], to[1], to[2]);
        } else {
            float r[3] = {-5.f, atan2f(hl[1] - ml[1], hl[0] - ml[0]) * 180.f / 3.14159265f, 0};
            UFunction *f = fn_of(pc, "SetControlRotation");
            memset(p, 0, sizeof p);
            memcpy(p + parm_off(f, "NewRotation"), r, 12);
            ue_process_event(pc, f, p);
            out_printf(o, "looking at hero %s (yaw %.0f)\n", a1, r[1]);
        }
        return 1;
    }
    if (!strcmp(sub, "reinit")) {
        UObject *ps = my_ps();
        out_printf(o, "reinit: %d\n", reinit_from_profile(ps, ps_slot(ps)));
        return 1;
    }
    if (!strcmp(sub, "mesh") && a3) {
        UObject *h = nth_hero(atoi(a1)), *c = h ? comp_named(h, a2) : NULL;
        if (!c) { out_printf(o, "no hero/component\n"); return 1; }
        UObject *m = load_asset(a3);
        if (!m) { out_printf(o, "load failed: %s\n", a3); return 1; }
        set_mesh(c, m);   // also empties OverrideMaterials: the mesh's own slot materials show
        out_printf(o, "SetSkeletalMesh done (material overrides cleared)\n");
        return 1;
    }
    if (!strcmp(sub, "load") && a1) {
        char b[300];
        UObject *m = load_asset(a1), *sk = m ? ue_get_ptr(m, "Skeleton") : NULL;
        out_printf(o, "%s skel=%s\n", m ? ue_full_path(m, b, sizeof b) : "load failed", sk ? ue_obj_name(sk, b, sizeof b) : "-");
        return 1;
    }
    if (!strcmp(sub, "skm")) {   // loaded skeletal meshes (filter a1) with skeleton
        UClass *c = ue_find_class("SkeletalMesh");
        int32_t n = ue_num_objects(); char b[300], d[128];
        for (int32_t i = 0; c && i < n; i++) {
            UObject *x = ue_object_at(i);
            if (!is_live(x) || !ue_is_a(x, c)) continue;
            ue_full_path(x, b, sizeof b);
            if (a1 && !strstr(b, a1)) continue;
            UObject *sk = ue_get_ptr(x, "Skeleton");
            out_printf(o, "%s skel=%s\n", b, sk ? ue_full_path(sk, d, sizeof d) : "-");
        }
        return 1;
    }
    if (!strcmp(sub, "reg")) {   // asset registry: every asset of a class (default SkeletalMesh), not loaded
        UObject *ar = ue_find_first_of("AssetRegistryImpl");
        if (!ar) { UClass *c = ue_find_class("AssetRegistryImpl"); ar = c ? UC_CDO(c) : NULL; }
        UClass *ic = ue_find_class("AssetRegistry");
        UFunction *f = ic ? ue_find_function(ic, "GetAssetsByClass") : NULL;
        int32_t pc = parm_off(f, "ClassName"), pa = parm_off(f, "OutAssetData"), ps = parm_off(f, "bSearchSubClasses");
        if (!ar || !f || pc < 0 || pa < 0 || UFN_PARMSSIZE(f) > 64) { out_printf(o, "no asset registry (%p %p)\n", (void *)ar, (void *)f); return 1; }
        uint8_t p[64] = {0};
        *(FName *)(p + pc) = make_name(a2 ? a2 : "SkeletalMesh");
        if (ps >= 0) p[ps] = 1;
        ue_process_event(ar, f, p);
        TArray *arr = (TArray *)(p + pa);
        char b[300]; int shown = 0;
        for (int i = 0; i < arr->num; i++) {
            ue_name(*(FName *)((char *)arr->data + i * 0x50), b, sizeof b);
            if (a1 && strcmp(a1, "*") && !strstr(b, a1)) continue;
            if (shown++ < 4000) out_printf(o, "%s\n", b);
        }
        out_printf(o, "%d assets, %d shown\n", arr->num, shown);
        return 1;
    }
    out_printf(o, "usage: mdl dump|rows [filter]|setslot <hero#> <entry>|rpc <entry>|reinit|mesh <hero#> <comp> <path>|load <path>|skm [filter]|reg [filter] [class]\n");
    return 1;
}
#endif  // !B4B_RELEASE

static void hook(uintptr_t at, const uint8_t *sig, size_t n, void *detour, void **orig, const char *what) {
    if (memcmp((void *)at, sig, n)) { LOG("models: %s signature mismatch", what); return; }
    if (MH_CreateHook((void *)at, detour, orig) != MH_OK || MH_EnableHook((void *)at) != MH_OK) { LOG("models: %s hook failed", what); *orig = NULL; }
}

int models_init(void) {
    wlooks_init();
    hook(ADDR_RUNREFRESH, SIG_RUNREFRESH, sizeof SIG_RUNREFRESH, (void *)runrefresh_detour, (void **)&orig_runrefresh, "campaign run save");
    hook(ADDR_APPLYSLOT, SIG_APPLYSLOT, sizeof SIG_APPLYSLOT, (void *)applyslot_detour, (void **)&orig_applyslot, "customization apply");
    // the profile guard first: rows only when it is in (an equipped row must never reach the profile)
    hook(ADDR_EQUIP, SIG_EQUIP, sizeof SIG_EQUIP, (void *)equip_detour, (void **)&orig_equip, "profile equip");
    if (orig_equip) hook(ADDR_GETPROF, SIG_GETPROF, sizeof SIG_GETPROF, (void *)getprof_detour, (void **)&orig_getprof, "profile look");
    if (!orig_equip || !orig_getprof) screen_off = 1;
    if (screen_off) LOG("models: add-on outfits on the customization screen: off (hooks)");
    if (memcmp((void *)ADDR_SELECTSET, SIG_SELECTSET, sizeof SIG_SELECTSET)) { LOG("models: SelectCustomizationSet signature mismatch, no host lock"); return -1; }
    if (MH_CreateHook((void *)ADDR_SELECTSET, (void *)selectset_detour, (void **)&orig_selectset) != MH_OK ||
        MH_EnableHook((void *)ADDR_SELECTSET) != MH_OK) { LOG("models: hook failed, no host lock"); orig_selectset = NULL; return -1; }
    LOG("models: on");
    return 0;
}
