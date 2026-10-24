// /map: load a map by its path (fork module, for the custom-map tests). Host only. Nothing is sent to other players
// beyond the game's own server travel (clients follow it like a mission start and need the same add-on to load).
//
//   /map                                 the current map: package path, game mode class, the options it was opened with
//   /map <path> [difficulty] [?opts]     a mission travel, the same URL as the war table's (minus the campaign run):
//                                        servertravel <path>?Difficulty=..?PoolConfig=Coop?game=HeroGameMode_BP?RunOwner=1
//                                        ?listen (upstream's developer command `mission raw`). Difficulty: easy|normal|
//                                        hard|veryhard or recruit|veteran|nightmare|nohope (default easy)
//   /map <path> plain [?opts]            servertravel <path>?listen: the map's own game mode (its world settings', else
//                                        the game's default)
//   /map back                            to the map /map last left, with that map's game mode and options
//
// <path> is a package path (/Game/Folder/MAP_Name, or /Game/Folder/MAP_Name.MAP_Name). Extra tokens starting with '?'
// are appended to the URL as they are (e.g. ?game=/Script/Engine.GameModeBase). The package must exist in the
// engine's file system (the game's paks, an add-on, loose files) before anything happens: a server travel to a missing
// map fails and drops the game to the main menu.
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "ue.h"
#include "log.h"
#include "cmds.h"

static const char *HERO_MODE = "/Game/Modes/HeroGameMode_BP.HeroGameMode_BP_C";   // testing.c `mission raw`
static const struct { const char *name, *alias, *url; } DIFF[] = {
    {"easy", "recruit", "Easy"}, {"normal", "veteran", "Normal"}, {"hard", "nightmare", "Hard"}, {"veryhard", "nohope", "VeryHard"}};

static char back_url[2048];   // the map /map last left: package + its options (with ?game=)

static void fs_ascii(const FString *s, char *buf, size_t len) {
    size_t i = 0;
    if (s && s->data && s->num > 0)
        for (; i < len - 1 && (int32_t)i < s->num && s->data[i]; i++) buf[i] = s->data[i] < 128 ? (char)s->data[i] : '?';
    buf[i] = 0;
}

// the current map's game mode class (as a ?game= value) and the options string it was opened with
static void current(UObject *w, char *pkg, size_t pl, char *mode, size_t ml, char *opts, size_t ol) {
    pkg[0] = mode[0] = opts[0] = 0;
    ue_world_package(w, pkg, pl);
    UObject *gm = ue_get_ptr(w, "AuthorityGameMode");
    if (!gm) return;
    ue_full_path((UObject *)U_CLASS(gm), mode, ml);
    int32_t off = ue_prop_offset(gm, "OptionsString");
    if (off >= 0) fs_ascii((FString *)((char *)gm + off), opts, ol);
}

// ?key= present in an options string (case-insensitive)
static int has_opt(const char *opts, const char *key) {
    size_t kl = strlen(key);
    for (const char *p = opts; (p = strchr(p, '?')); p++)
        if (!_strnicmp(p + 1, key, kl) && p[1 + kl] == '=') return 1;
    return 0;
}

// "/Game/X/MAP_Y.MAP_Y" -> "/Game/X/MAP_Y" (the package)
static void strip_object(char *path) {
    char *slash = strrchr(path, '/'), *dot = slash ? strchr(slash, '.') : NULL;
    if (dot) *dot = 0;
}

static int path_ok(const char *p) {
    if (_strnicmp(p, "/Game/", 6) || strlen(p) > 400) return 0;
    for (const char *c = p; *c; c++) if (!isalnum((unsigned char)*c) && !strchr("/_-.", *c)) return 0;
    return 1;
}

// "/Game/A/MAP_X" -> "gobi/content/a/map_x" + ext (paks_file_exists' key)
static int pkg_exists(const char *pkg, const char *ext) {
    char k[600];
    snprintf(k, sizeof k, "gobi/content/%s%s", pkg + 6, ext);
    for (char *c = k; *c; c++) *c = (char)tolower((unsigned char)*c);
    return paks_file_exists(k);
}

static void travel(const char *url, Out *o) {
    char cmd[2100];
    snprintf(cmd, sizeof cmd, "servertravel %s", url);
    LOG("maptest: %s", cmd);
    game_exec(cmd);
    out_printf(o, "/map: travelling: %s\n", url);
}

// ---- the map row (24 Oct) -------------------------------------------------------------------------------------------
// The game starts a map's pre-round (character select, deck, burn and corruption cards) and post-round (success splash,
// stats) only when a map table has a row for it: the copy at a new path logged "TryStartPreRound / TryStartPostRound
// Failed to find map row for map '<path>'". Which table, and how its row names the map, isn't known yet:
//   /map rows [<path>]      every loaded data table row that names the map (default: the current one) in chat; the whole
//                           rows and their table's layout in the log ("maptest: row" lines)
//   /map row <from> <to>    test: in those rows, every soft path, name or string equal to map <from> (its path, or its
//                           short name for a name/string) now names <to> (in place; <from> has no row meanwhile; only a
//                           retargeted string allocates its new text)
//   /map row undo           puts them back;  /map row  lists them
// Layouts, as models.c / rewardguard.c / ragdoll.c read them in this build: UDataTable RowStruct +0x30, RowMap +0x38
// (TMap<FName, uint8*>: sparse array of 0x18-byte entries {key, row, ...}); FProperty subtype (a struct property's
// struct, an array property's inner property) +0x78; bool field size +0x78, byte offset +0x79, field mask +0x7B;
// TSoftObjectPtr {FWeakObjectPtr +0, TagAtLastTest +8, FSoftObjectPath {AssetPathName FName +0x10, SubPathString +0x18}}.
#define DT_ROWSTRUCT(t) (*(UObject **)((char *)(t) + 0x30))
#define DT_ROWMAP(t)    ((TArray *)((char *)(t) + 0x38))
#define FP_SUB(f)       (*(void **)((char *)(f) + 0x78))
#define FBP_SIZE(f)     (*((uint8_t *)(f) + 0x78))
#define FBP_BYTEOFF(f)  (*((uint8_t *)(f) + 0x79))
#define FBP_MASK(f)     (*((uint8_t *)(f) + 0x7B))
typedef FName *(*FNameCtorFn)(FName *self, const wchar_t *name, int find_type);
#define ADDR_FNAME_CTOR VA(0x1424BC8E0ull)
#define ADDR_GMALLOC    VA(0x1469E59F0ull)   // FMalloc* GMalloc; vtable +0x20 Realloc(this, ptr, size, align)
typedef void *(*ReallocFn)(void *self, void *p, size_t n, uint32_t align);

static void *gm_realloc(void *p, size_t n) {
    void *gm = *(void **)ADDR_GMALLOC;
    return gm ? ((ReallocFn)(*(void ***)gm)[4])(gm, p, n, 16) : NULL;
}
static FName make_name(const char *s) {
    wchar_t wb[512]; int k = 0;
    for (; s[k] && k < 511; k++) wb[k] = (wchar_t)(unsigned char)s[k];
    wb[k] = 0;
    FName n = {0};
    ((FNameCtorFn)ADDR_FNAME_CTOR)(&n, wb, 1);
    return n;
}
static void *sparse_at(const void *set, int i, int stride) {
    const TArray *a = set;
    if (!a->data || i < 0 || i >= a->num || i >= *(const int32_t *)((const char *)set + 0x28)) return NULL;
    const uint32_t *bits = *(uint32_t *const *)((const char *)set + 0x20);
    if (!bits) bits = (const uint32_t *)((const char *)set + 0x10);
    return (bits[i >> 5] >> (i & 31)) & 1 ? (char *)a->data + (size_t)i * stride : NULL;
}
static const char *ptype(FField *f, char *b, size_t n) { return ue_name(*(FName *)FF_CLASS(f), b, n); }
static const char *short_of(const char *pkg) { const char *s = strrchr(pkg, '/'); return s ? s + 1 : pkg; }

// a value that can name a map: kind and where its name/string lives
enum { K_SOFTPTR = 1, K_SOFTPATH, K_NAME, K_STR };
static FName *kind_name(int kind, uint8_t *a) { return kind == K_SOFTPTR ? (FName *)(a + 0x10) : kind == K_STR ? NULL : (FName *)a; }
static void kind_text(int kind, uint8_t *a, char *out, size_t n) {
    if (kind == K_STR) fs_ascii((FString *)a, out, n);
    else ue_name(*kind_name(kind, a), out, n);
}
// "/Game/X/MAP_Y" or "/Game/X/MAP_Y.MAP_Y" (any subobject after ':' too) is map pkg; exact, case-insensitive
static int names_map(const char *v, const char *pkg) {
    size_t pl = strlen(pkg);
    if (_strnicmp(v, pkg, pl)) return 0;
    return v[pl] == 0 || v[pl] == '.' || v[pl] == ':';
}
// row names and other strings that merely mention the map ("Evansburgh_B" in "C1_Evansburgh_B"): leads, not matches
static int mentions(const char *v, const char *core) {
    size_t cl = strlen(core);
    if (cl < 4) return 0;
    for (const char *p = v; *p; p++) if (!_strnicmp(p, core, cl)) return 1;
    return 0;
}

typedef struct Scan Scan;
typedef void (*ScanFn)(Scan *s, const char *field, int kind, uint8_t *a, const char *text);
struct Scan { ScanFn fn; const char *pkg, *core; UObject *table; const char *row; int hits, leads; int32_t ti; uint8_t *rowp; };

static void scan_struct(Scan *s, UObject *st, uint8_t *base, const char *prefix, int depth);
static void scan_value(Scan *s, FField *f, uint8_t *a, const char *field, int depth) {
    char t[64], v[600];
    ptype(f, t, sizeof t);
    int kind = !strcmp(t, "SoftObjectProperty") || !strcmp(t, "SoftClassProperty") ? K_SOFTPTR
             : !strcmp(t, "NameProperty") ? K_NAME : !strcmp(t, "StrProperty") ? K_STR : 0;
    if (!strcmp(t, "StructProperty")) {
        char sn[64];
        ue_obj_name((UObject *)FP_SUB(f), sn, sizeof sn);
        if (!strcmp(sn, "SoftObjectPath") || !strcmp(sn, "SoftClassPath")) kind = K_SOFTPATH;
        else if (depth > 0) { char pre[200]; snprintf(pre, sizeof pre, "%s.", field); scan_struct(s, (UObject *)FP_SUB(f), a, pre, depth - 1); return; }
    }
    if (!strcmp(t, "ArrayProperty")) {
        FField *in = (FField *)FP_SUB(f);
        TArray *arr = (TArray *)a;
        int es = in ? FP_ELSIZE(in) : 0;
        if (!in || es <= 0 || !arr->data) return;
        for (int i = 0; i < arr->num && i < 256; i++) {
            char ef[200];
            snprintf(ef, sizeof ef, "%s[%d]", field, i);
            scan_value(s, in, (uint8_t *)arr->data + (size_t)i * es, ef, depth);
        }
        return;
    }
    if (!kind) return;
    kind_text(kind, a, v, sizeof v);
    if (v[0]) s->fn(s, field, kind, a, v);
}
static void scan_struct(Scan *s, UObject *st, uint8_t *base, const char *prefix, int depth) {
    for (UObject *u = st; u; u = US_SUPER(u))
        for (FField *f = US_CHILDPROPS(u); f; f = FF_NEXT(f)) {
            char fn[64], field[200];
            snprintf(field, sizeof field, "%s%s", prefix, ue_name(FF_NAME(f), fn, sizeof fn));
            scan_value(s, f, base + FP_OFFSET(f), field, depth);
        }
}
// every loaded data table's rows (the class default object and archetypes left out)
static void scan_tables(Scan *s, void (*row_done)(Scan *s, UObject *t, const char *row, uint8_t *data, int hits, int leads)) {
    UClass *dtc = ue_find_class("DataTable");
    if (!dtc) return;
    int n = ue_num_objects();
    for (int i = 0; i < n; i++) {
        UObject *t = ue_object_at(i);
        // not the class default object or archetypes (0x30), nor a table still loading (RF_NeedLoad 0x400,
        // RF_NeedPostLoad 0x1000) or on its way out (ue_object_usable)
        if (!t || (U_FLAGS(t) & 0x1430) || !ue_object_usable(t, i) || !ue_is_a(t, dtc) || !DT_ROWSTRUCT(t)) continue;
        TArray *m = DT_ROWMAP(t);
        for (int k = 0; k < m->num; k++) {
            char *e = sparse_at(m, k, 0x18);
            uint8_t *row = e ? *(uint8_t **)(e + 8) : NULL;
            if (!row) continue;
            char rn[128];
            ue_name(*(FName *)e, rn, sizeof rn);
            s->table = t; s->row = rn; s->ti = i; s->rowp = row;
            int h0 = s->hits, l0 = s->leads;
            if (s->core && mentions(rn, s->core)) s->leads++;
            scan_struct(s, DT_ROWSTRUCT(t), row, "", 2);
            if (row_done) row_done(s, t, rn, row, s->hits - h0, s->leads - l0);
        }
    }
}

// a value in a row, for the log (the types a map row is likely to hold; others by type name)
static void fmt_val(FField *f, uint8_t *a, char *out, size_t n) {
    char t[64], b[256];
    ptype(f, t, sizeof t);
    if (!strcmp(t, "BoolProperty")) {
        if (FBP_SIZE(f) == 1 && FBP_MASK(f)) snprintf(out, n, "%d", (a[FBP_BYTEOFF(f)] & FBP_MASK(f)) != 0);
        else snprintf(out, n, "byte %u", *a);
    } else if (!strcmp(t, "IntProperty")) snprintf(out, n, "%d", *(int32_t *)a);
    else if (!strcmp(t, "FloatProperty")) snprintf(out, n, "%g", *(float *)a);
    else if (!strcmp(t, "ByteProperty") || !strcmp(t, "EnumProperty")) snprintf(out, n, "%u", *a);
    else if (!strcmp(t, "NameProperty")) snprintf(out, n, "%s", ue_name(*(FName *)a, b, sizeof b));
    else if (!strcmp(t, "StrProperty")) { fs_ascii((FString *)a, b, sizeof b); snprintf(out, n, "\"%s\"", b); }
    else if (!strcmp(t, "SoftObjectProperty") || !strcmp(t, "SoftClassProperty")) snprintf(out, n, "%s", ue_name(*(FName *)(a + 0x10), b, sizeof b));
    else if (!strcmp(t, "ObjectProperty") || !strcmp(t, "ClassProperty")) {
        UObject *o2 = *(UObject **)a;
        snprintf(out, n, "%s", o2 ? ue_full_path(o2, b, sizeof b) : "None");
    } else if (!strcmp(t, "ArrayProperty")) snprintf(out, n, "%d entries", ((TArray *)a)->num);
    else if (!strcmp(t, "StructProperty")) {
        char sn[64];
        ue_obj_name((UObject *)FP_SUB(f), sn, sizeof sn);
        if (!strcmp(sn, "SoftObjectPath") || !strcmp(sn, "SoftClassPath")) snprintf(out, n, "%s", ue_name(*(FName *)a, b, sizeof b));
        else if (!strcmp(sn, "DataTableRowHandle")) {   // {UDataTable*, FName RowName, ...} (rewardguard.c)
            UObject *dt = *(UObject **)a;
            char tn[96];
            snprintf(out, n, "%s/%s", dt ? ue_obj_name(dt, tn, sizeof tn) : "None", ue_name(*(FName *)(a + 8), b, sizeof b));
        } else snprintf(out, n, "<%s>", sn);
    } else snprintf(out, n, "<%s>", t);
}

static UObject *logged_layout[16];
static int n_logged_layout;
static void log_row(UObject *t, const char *row, uint8_t *data) {
    char tn[128], sn[96], fn[64], ty[64], v[400];
    UObject *rs = DT_ROWSTRUCT(t);
    int seen = 0;
    for (int i = 0; i < n_logged_layout; i++) if (logged_layout[i] == t) seen = 1;
    if (!seen && n_logged_layout < 16) {   // the table's layout once: every field with type, offset and size
        logged_layout[n_logged_layout++] = t;
        LOG("maptest: row table %s: row struct %s, %d bytes, %d rows", ue_full_path(t, tn, sizeof tn), ue_obj_name(rs, sn, sizeof sn),
            US_PROPSSIZE(rs), DT_ROWMAP(t)->num);
        for (UObject *u = rs; u; u = US_SUPER(u))
            for (FField *f = US_CHILDPROPS(u); f; f = FF_NEXT(f)) {
                char sub[64] = "";
                ptype(f, ty, sizeof ty);
                if ((!strcmp(ty, "StructProperty") || !strcmp(ty, "ObjectProperty")) && FP_SUB(f)) ue_obj_name((UObject *)FP_SUB(f), sub, sizeof sub);
                if (!strcmp(ty, "ArrayProperty") && FP_SUB(f)) ptype((FField *)FP_SUB(f), sub, sizeof sub);
                LOG("maptest: row   field %s: %s%s%s%s @0x%x (%d bytes)", ue_name(FF_NAME(f), fn, sizeof fn), ty, sub[0] ? " (" : "", sub,
                    sub[0] ? ")" : "", FP_OFFSET(f), FP_ELSIZE(f));
            }
    }
    LOG("maptest: row %s / %s:", ue_obj_name(t, tn, sizeof tn), row);
    for (UObject *u = rs; u; u = US_SUPER(u))
        for (FField *f = US_CHILDPROPS(u); f; f = FF_NEXT(f)) {
            fmt_val(f, data + FP_OFFSET(f), v, sizeof v);
            LOG("maptest: row   %s = %s", ue_name(FF_NAME(f), fn, sizeof fn), v);
        }
}

// /map rows
static struct { UObject *t; char row[96], field[200], text[300]; int lead; } found[24];
static int n_found;
// a name or string that is exactly the map's short name ("MAP_PERS_Evansburgh_B") names it too
static int names_short(int kind, const char *text, const char *pkg) {
    return (kind == K_NAME || kind == K_STR) && !_stricmp(text, short_of(pkg));
}
static void rows_hit(Scan *s, const char *field, int kind, uint8_t *a, const char *text) {
    (void)a;
    int exact = names_map(text, s->pkg) || names_short(kind, text, s->pkg), lead = !exact && mentions(text, s->core);
    if (!exact && !lead) return;
    if (exact) s->hits++; else s->leads++;
    if (n_found < 24) {
        found[n_found].t = s->table; found[n_found].lead = lead;
        snprintf(found[n_found].row, sizeof found[0].row, "%s", s->row);
        snprintf(found[n_found].field, sizeof found[0].field, "%s", field);
        snprintf(found[n_found].text, sizeof found[0].text, "%s", text);
        n_found++;
    }
}
static int rows_logged;
static void rows_row_done(Scan *s, UObject *t, const char *row, uint8_t *data, int hits, int leads) {
    (void)s;
    if ((hits || leads) && rows_logged < 12) { rows_logged++; log_row(t, row, data); }
}
static void cmd_rows(const char *path, Out *o) {
    char core[128];
    snprintf(core, sizeof core, "%s", short_of(path));
    const char *c = core;   // "MAP_PERS_Evansburgh_B" -> "Evansburgh_B" (how other names would mention it)
    if (!_strnicmp(c, "MAP_PERS_", 9)) c += 9; else if (!_strnicmp(c, "MAP_", 4)) c += 4;
    Scan s = {rows_hit, path, c, NULL, NULL, 0, 0, 0, NULL};
    n_found = 0; rows_logged = 0; n_logged_layout = 0;
    LOG("maptest: rows naming %s (or mentioning %s)", path, c);
    scan_tables(&s, rows_row_done);
    out_printf(o, "/map rows: %d value(s) name %s, %d more mention \"%s\"%s\n", s.hits, path, s.leads, c,
               s.hits || s.leads ? " (whole rows in the log: \"maptest: row\" lines)" : "");
    char tn[96];
    for (int i = 0; i < n_found && i < 12; i++)
        out_printf(o, "  %s%s / %s: %s = %s\n", found[i].lead ? "(mentions) " : "", ue_obj_name(found[i].t, tn, sizeof tn), found[i].row,
                   found[i].field, found[i].text);
    for (int i = 0; i < n_found; i++)
        LOG("maptest: rows %s %s / %s: %s = %s", found[i].lead ? "mention" : "NAMES", ue_obj_name(found[i].t, tn, sizeof tn), found[i].row,
            found[i].field, found[i].text);
    if (!s.hits && !s.leads) out_printf(o, "  no loaded table names it here; try again inside that map, or in Fort Hope\n");
}

// /map row <from> <to>: retarget (test), with undo
typedef struct { uint8_t *a; int kind; uint8_t old[0x28]; void *newstr; UObject *t; int32_t ti; uint8_t *rowp; char table[64], row[64], field[96]; } Retarget;
static Retarget rt[64];
static int n_rt;
static const char *rt_to;
static void row_hit(Scan *s, const char *field, int kind, uint8_t *a, const char *text) {
    int by_short = !names_map(text, s->pkg) && names_short(kind, text, s->pkg);
    if ((!by_short && !names_map(text, s->pkg)) || n_rt >= 64) return;
    for (int i = 0; i < n_rt; i++) if (rt[i].a == a) return;
    Retarget *r = &rt[n_rt];
    char nv[600];
    // the same form as the old value: short name, package, package.object, or package.object:subobject
    const char *rest = by_short ? "" : text + strlen(s->pkg);
    if (by_short) snprintf(nv, sizeof nv, "%s", short_of(rt_to));
    else if (*rest == '.') {
        const char *colon = strchr(rest, ':');
        snprintf(nv, sizeof nv, "%s.%s%s", rt_to, short_of(rt_to), colon ? colon : "");
    } else snprintf(nv, sizeof nv, "%s%s", rt_to, rest);
    r->a = a; r->kind = kind; r->newstr = NULL; r->t = s->table; r->ti = s->ti; r->rowp = s->rowp;
    memcpy(r->old, a, kind == K_SOFTPTR ? 0x28 : kind == K_STR ? sizeof(FString) : sizeof(FName));
    if (kind == K_STR) {
        FString *fs = (FString *)a;
        int len = (int)strlen(nv) + 1;
        wchar_t *wd = gm_realloc(NULL, (size_t)len * sizeof(wchar_t));
        if (!wd) return;
        for (int k = 0; k < len; k++) wd[k] = (wchar_t)(unsigned char)nv[k];
        fs->data = wd; fs->num = fs->max = len;   // the old text stays allocated for undo
        r->newstr = wd;
    } else {
        *kind_name(kind, a) = make_name(nv);
        if (kind == K_SOFTPTR) {   // no cached object: the soft pointer resolves the new path when asked
            *(int32_t *)a = -1; *(int32_t *)(a + 4) = 0; *(int32_t *)(a + 8) = 0;
        }
    }
    ue_obj_name(s->table, r->table, sizeof r->table);
    snprintf(r->row, sizeof r->row, "%s", s->row);
    snprintf(r->field, sizeof r->field, "%s", field);
    LOG("maptest: row retarget %s / %s: %s = %s -> %s", r->table, r->row, field, text, nv);
    n_rt++; s->hits++;
}
// the table is still the same object and still holds the row (a map change may unload and reload it: then the
// change is gone with it, and writing the old value back would hit freed memory)
static int rt_alive(const Retarget *r) {
    if (!r->t || ue_object_at(r->ti) != r->t || !ue_object_usable(r->t, r->ti) || (U_FLAGS(r->t) & 0x1430)) return 0;
    TArray *m = DT_ROWMAP(r->t);
    for (int k = 0; k < m->num; k++) {
        char *e = sparse_at(m, k, 0x18);
        if (e && *(uint8_t **)(e + 8) == r->rowp) return 1;
    }
    return 0;
}
static void rt_undo(Out *o) {
    int back = 0, gone = 0;
    for (int i = n_rt - 1; i >= 0; i--) {
        Retarget *r = &rt[i];
        if (!rt_alive(r)) { gone++; continue; }   // its table was unloaded: nothing to put back (its new text went with it)
        memcpy(r->a, r->old, r->kind == K_SOFTPTR ? 0x28 : r->kind == K_STR ? sizeof(FString) : sizeof(FName));
        if (r->newstr) gm_realloc(r->newstr, 0);   // Realloc to 0 = free
        back++;
    }
    out_printf(o, "/map row: %d value(s) put back%s\n", back, gone ? "; some tables were reloaded since (their rows are the game's own again)" : "");
    LOG("maptest: row undo: %d value(s) put back, %d in tables reloaded since", back, gone);
    n_rt = 0;
}
static void cmd_row(char **tok, int n, Out *o) {
    if (n == 1) {
        if (!n_rt) { out_printf(o, "/map row: nothing retargeted. /map row <from map> <to map> (a test; /map row undo)\n"); return; }
        out_printf(o, "/map row: %d value(s) retargeted (/map row undo puts them back):\n", n_rt);
        for (int i = 0; i < n_rt && i < 10; i++)
            out_printf(o, "  %s / %s: %s%s\n", rt[i].table, rt[i].row, rt[i].field, rt_alive(&rt[i]) ? "" : " (table reloaded since: the game's own again)");
        return;
    }
    if (!_stricmp(tok[1], "undo")) { rt_undo(o); return; }
    if (n != 3) { out_printf(o, "usage: /map row <from map path> <to map path>  |  /map row undo\n"); return; }
    static char from[512], to[512];
    snprintf(from, sizeof from, "%s", tok[1]);
    snprintf(to, sizeof to, "%s", tok[2]);
    strip_object(from);
    strip_object(to);
    if (!path_ok(from) || !path_ok(to)) { out_printf(o, "/map row: expected two package paths like /Game/Folder/MAP_Name\n"); return; }
    if (n_rt) { out_printf(o, "/map row: %d value(s) already retargeted; /map row undo first\n", n_rt); return; }
    rt_to = to;
    Scan s = {row_hit, from, NULL, NULL, NULL, 0, 0, 0, NULL};
    scan_tables(&s, NULL);
    if (!n_rt) { out_printf(o, "/map row: no loaded table row names %s (/map rows %s shows what does)\n", from, from); return; }
    out_printf(o, "/map row: %d value(s) that named %s now name %s (a test: %s has no row until /map row undo):\n", n_rt, from, to,
               short_of(from));
    for (int i = 0; i < n_rt && i < 10; i++) out_printf(o, "  %s / %s: %s\n", rt[i].table, rt[i].row, rt[i].field);
}

void maptest_slash(char *rest, Out *o) {
    UObject *w = ue_world();
    if (!w || !ue_get_ptr(w, "AuthorityGameMode")) { out_printf(o, "/map: not now (no game mode: loading, or not the host)\n"); return; }
    char pkg[512], mode[512], opts[700];
    current(w, pkg, sizeof pkg, mode, sizeof mode, opts, sizeof opts);
    char *tok[8] = {0};
    int n = 0;
    for (char *t = rest ? strtok(rest, " ") : NULL; t && n < 8; t = strtok(NULL, " ")) tok[n++] = t;
    if (!n) {
        out_printf(o, "/map: %s\n  game mode %s\n  options %s\n", pkg, mode[0] ? mode : "(none)", opts[0] ? opts : "(none)");
        if (back_url[0]) out_printf(o, "  /map back: %s\n", back_url);
        if (n_rt) out_printf(o, "  %d map table value(s) retargeted (/map row lists them, /map row undo)\n", n_rt);
        LOG("maptest: current %s, game mode %s, options %s", pkg, mode, opts);
        return;
    }
    if (!_stricmp(tok[0], "back")) {
        if (!back_url[0]) { out_printf(o, "/map back: nowhere to go back to (no /map travel since the game started)\n"); return; }
        static char url[2048];
        snprintf(url, sizeof url, "%s", back_url);
        travel(url, o);
        return;
    }
    // the path: strip an object name ("/Game/X/MAP_Y.MAP_Y" -> "/Game/X/MAP_Y")
    if (!_stricmp(tok[0], "rows")) {
        static char rp[512];
        snprintf(rp, sizeof rp, "%s", n > 1 ? tok[1] : pkg);
        strip_object(rp);
        if (!path_ok(rp)) { out_printf(o, "usage: /map rows [<map path>]  (default: this map)\n"); return; }
        cmd_rows(rp, o);
        return;
    }
    if (!_stricmp(tok[0], "row")) { cmd_row(tok, n, o); return; }
    char path[512];
    snprintf(path, sizeof path, "%s", tok[0]);
    strip_object(path);
    if (!path_ok(path)) { out_printf(o, "/map: expected a package path like /Game/Folder/MAP_Name, got %s\n", tok[0]); return; }
    int plain = 0, diff = 0;
    char extra[500] = "";
    for (int i = 1; i < n; i++) {
        int found = 0;
        if (!_stricmp(tok[i], "plain")) { plain = 1; continue; }
        if (!_stricmp(tok[i], "mission")) continue;
        for (int d = 0; d < 4; d++)
            if (!_stricmp(tok[i], DIFF[d].name) || !_stricmp(tok[i], DIFF[d].alias)) { diff = d; found = 1; }
        if (found) continue;
        if (tok[i][0] == '?' && !strchr(tok[i], ' ') && strlen(extra) + strlen(tok[i]) < sizeof extra) { strcat(extra, tok[i]); continue; }
        out_printf(o, "usage: /map [<path> [easy|normal|hard|veryhard|plain] [?option=value...]|back]\n");
        return;
    }
    int as_map = pkg_exists(path, ".umap"), as_asset = !as_map && pkg_exists(path, ".uasset");
    if (!as_map && !as_asset) {
        out_printf(o, "/map: %s isn't in the game's files or any add-on (is the add-on installed and on? /addons)\n", path);
        LOG("maptest: %s: no .umap or .uasset", path);
        return;
    }
    if (as_asset) out_printf(o, "/map: note: %s is a .uasset, not a .umap (maps are .umap); trying anyway\n", path);
    static char url[2048];
    if (plain) snprintf(url, sizeof url, "%s%s?listen", path, extra);
    else snprintf(url, sizeof url, "%s?Difficulty=%s?PoolConfig=Coop%s%s?RunOwner=1%s?listen", path, DIFF[diff].url,
                  has_opt(extra, "game") ? "" : "?game=", has_opt(extra, "game") ? "" : HERO_MODE, extra);
    // where /map back goes: this map with the options it was opened with (minus listen, added again), and its game
    // mode as ?game= unless the options already name one
    char o2[700] = "", copy[700];
    snprintf(copy, sizeof copy, "%s", opts);
    for (char *t = strtok(copy, "?"); t; t = strtok(NULL, "?"))
        if (_stricmp(t, "listen") && strlen(o2) + strlen(t) + 2 < sizeof o2) { strcat(o2, "?"); strcat(o2, t); }
    int add_mode = mode[0] && !has_opt(o2, "game");
    snprintf(back_url, sizeof back_url, "%s%s%s%s?listen", pkg, o2, add_mode ? "?game=" : "", add_mode ? mode : "");
    int nc = ue_num_clients(w);
    if (nc > 0) out_printf(o, "/map: %d player(s) will follow; they need the same add-on or their load fails\n", nc);
    LOG("maptest: from %s (game mode %s, options %s) to %s (%s)", pkg, mode, opts, path, as_map ? ".umap" : ".uasset");
    travel(url, o);
}
