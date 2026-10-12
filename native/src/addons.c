// Add-ons (#20, epic #23), L4D style: drop-in paks, no in-game browser. docs/investigations/addons.md.
//   Folder: <game>\b4bcoop-addons\ (next to Back4Blood.exe; ini addons_dir=<windows path> moves it, addons=0 turns
//   add-ons off). Each add-on is one .pak written by modkit/addon.py: cooked files plus an optional
//   b4bcoop-addoninfo.txt entry (title, author, version, category, description; `key=value` or L4D's
//   `addontitle "..."`). Any pak in our format loads; without addoninfo its file name is its title.
//   addonlist.txt in the folder: `<file>.pak=1|0`, load order top to bottom; later add-ons win conflicts. New add-ons
//   are appended switched on (sorted by name) and the file is rewritten. /addons on|off edits it: off applies at the
//   next start, on loads the add-on at once when that is safe (addons_load_now), else at the next start.
//   Everything is read in DllMain (addons_scan, before the engine starts) so that paks.c installs no pak hook when
//   there is nothing to mount; the paks are mounted from the FPakPlatformFile::Initialize hook (addons_mount).
//   Our pak format only (modkit/b4bpak.py; pakfmt.c): v9 or v8, magic 0x18772, plain index; the index SHA1 is verified (a damaged
//   index would be a Fatal in the engine) and is the add-on's content id (#22: compare it across a session).
//   Conflicts: two enabled add-ons with the same file path; logged, listed by /addons, and one chat notice. "Mixed":
//   one package's files (.uasset/.uexp/.ubulk) end up from different add-ons, which can crash the game.
//   Each add-on is classified from its files (addonclass.c, #22): cosmetic or gameplay-affecting; the addoninfo's
//   own `content=` line is only compared. Add-ons stay client-side (each player sees their own); what a joiner runs is
//   checked by the joiner itself against the host's announced addons_policy, never sent (addons_mp.c).
//   `~` window: Installed in the "Looks & add-ons" tab (addons_installed_panel, below; looks.c draws the tab): list,
//   on/off (= /addons on|off), load order (Up/Down rewrite addonlist.txt: A[] keeps the mounted order, ord[] is the
//   order in the file), details, Wear / Use for the looks an add-on adds; Update / Remove from shop.c.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "log.h"
#include "cmds.h"
#include "addonclass.h"
#include "overlay.h"
#include "pakfmt.h"

#define ADDON_ORDER 1000          // read order of the first add-on; retail paks are 4 (+100 per patch level)
#define ENTRY_HDR 53              // in-data FPakEntry of an uncompressed entry (B4B layout, modkit/b4bpak.py)
#define INFO_NAME "b4bcoop-addoninfo.txt"
#define LIST_NAME "addonlist.txt"
#define MAX_CONFLICTS 64

typedef struct {
    wchar_t file[MAX_PATH];       // file name in the add-ons folder
    char name[MAX_PATH];          // same, UTF-8 (addonlist.txt key, chat)
    char title[96], author[64], version[32], category[48], desc[400], hash[41], claim[48];   // claim: addoninfo content=
    int nfiles, present, on, on_at_start, valid, mounted;   // mounted: 1 ok, -1 Mount failed, 0 not mounted
    uint32_t pakver;              // pak version (9, or 8 for older community paks; pakfmt.c)
    int pos0;                     // 1-based place among the present add-ons at game start (0 = not present)
    char why[120];                // why it can't load
    uint32_t order;
    char **keys;                  // normalized file paths (conflict check, freed after the scan)
    AddonClass cls;               // cosmetic or gameplay, from the files (addonclass.c)
} Addon;
static Addon A[MAX_ADDONS];
static int nA, n_mount;
static int enabled_cfg = 1, shop_cfg = 1, list_dirty;
static char unavailable[80];
static wchar_t dirw[MAX_PATH];
static char dir8[MAX_PATH * 3];
// same: identical content (same pak index SHA1, e.g. one add-on under two file names); names: outfits/weapon looks
// both declare ("outfit batman, weapon look ak47"), only the later one's is used
typedef struct { int loser, winner, nfiles, mixed, same; char example[160], mixed_example[160], names[120]; } Conflict;
static Conflict C[MAX_CONFLICTS];
static int nC;
static CRITICAL_SECTION cs;       // addonlist.txt writes (chat thread) vs. nothing else after startup; cheap anyway
// addonlist.txt order: ord[k] = index into A. A[] itself never moves after the scan (conflicts, outfits and the
// mounted state point into it); a reorder only changes ord[] and the file, applied at the next start.
static int ord[MAX_ADDONS];
static FILETIME list_mtime;       // addonlist.txt as last read or written by us (the panel re-reads it when it changes)

// ---- small helpers ----
static void w2u(const wchar_t *w, char *out, int n) { if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL)) out[0] = 0; }
static void u2w(const char *u, wchar_t *out, int n) { if (!MultiByteToWideChar(CP_UTF8, 0, u, -1, out, n)) out[0] = 0; }
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}
static char *unquote(char *s) {
    s = trim(s);
    size_t l = strlen(s);
    if (l >= 2 && s[0] == '"' && s[l - 1] == '"') { s[l - 1] = 0; s++; }
    return s;
}

static int read_at(HANDLE h, uint64_t off, void *buf, DWORD n) {
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    DWORD got;
    return SetFilePointerEx(h, li, NULL, FILE_BEGIN) && ReadFile(h, buf, n, &got, NULL) && got == n;
}

// ---- added outfits: addoninfo `outfit=<name>|<survivor>|<3P mesh>|<FP mesh>|<title>` (modkit: b4bmod survivor --as;
// docs/investigations/new-assets.md). models.c wears them (/model <name>). ----
#define MAX_OUTFITS 512
typedef struct { Addon *addon; char name[33], hero[24], mesh3p[200], meshfp[200], title[64]; } Outfit;
static Outfit OF[MAX_OUTFITS];
static int nOF;
static int outfit_path(const char *in, char *out, size_t n) {   // "/Game/X/Y[.Y]" -> "/Game/X/Y.Y"; "" = none
    out[0] = 0;
    if (!*in) return 1;
    if (strncmp(in, "/Game/", 6) || strlen(in) >= n / 2 - 2) return 0;
    for (const char *c = in; *c; c++) if ((unsigned char)*c < 33 || (unsigned char)*c > 126 || *c == '"' || *c == '\\') return 0;
    const char *base = strrchr(in, '/') + 1;
    if (strchr(base, '.')) snprintf(out, n, "%s", in);
    else snprintf(out, n, "%s.%s", in, base);
    return 1;
}
static void parse_outfit(Addon *a, char *v) {
    char *f[5] = {0}; int k = 0;
    for (char *p = v; k < 5; k++) { f[k] = p; p = strchr(p, '|'); if (!p) { k++; break; } *p++ = 0; }
    Outfit o = {0};
    o.addon = a;
    int ok = k >= 3 && strlen(f[0]) > 0 && strlen(f[0]) < sizeof o.name && isalpha((unsigned char)f[0][0]);
    for (const char *c = f[0]; ok && *c; c++) ok = islower((unsigned char)*c) || isdigit((unsigned char)*c) || *c == '_';
    if (ok) ok = outfit_path(trim(f[2]), o.mesh3p, sizeof o.mesh3p) && o.mesh3p[0] && (k < 4 || outfit_path(trim(f[3]), o.meshfp, sizeof o.meshfp));
    if (!ok || nOF >= MAX_OUTFITS) { LOG("addons: %s: outfit line ignored (%s): %s", a->name, ok ? "too many outfits" : "expected <name>|<survivor>|<3P mesh>|<FP mesh>|<title>", f[0] ? f[0] : ""); return; }
    snprintf(o.name, sizeof o.name, "%s", f[0]);
    snprintf(o.hero, sizeof o.hero, "%s", f[1] ? trim(f[1]) : "-");
    snprintf(o.title, sizeof o.title, "%s", k >= 5 && *f[4] ? trim(f[4]) : o.name);
    OF[nOF++] = o;
}

// Outfits of the mounted add-ons; a name in two add-ons: the later one (load order) wins, like files.
int addons_outfits(AddonOutfit *out, int max) {
    int k = 0;
    uint32_t won[MAX_OUTFITS];   // read order of the add-on whose entry out[j] is (live mounts append out of order)
    for (int i = 0; i < nOF; i++) {
        const Outfit *o = &OF[i];
        if (o->addon->mounted <= 0) continue;
        int j = 0;
        while (j < k && strcmp(out[j].name, o->name)) j++;
        if (j == k) { if (k >= max || k >= MAX_OUTFITS) continue; k++; }
        else if (won[j] > o->addon->order) continue;
        won[j] = o->addon->order;
        out[j].name = o->name; out[j].hero = o->hero; out[j].mesh3p = o->mesh3p; out[j].meshfp = o->meshfp;
        out[j].title = o->title; out[j].addon = o->addon->title;
    }
    return k;
}

// ---- added weapon looks: addoninfo `weapon=<name>|<code>|<FP mesh>|<3P static mesh>|<3P skeletal mesh>|<title>`
// (modkit: b4bmod weapon --as; docs/investigations/new-assets.md §9). weaponlooks.c puts them on the chooser's weapon
// of that code (/model <name>). Mesh fields may be empty (at least one set). ----
#define MAX_WEAPONS 512
typedef struct { Addon *addon; char name[33], code[16], fp[200], sm3p[200], skm3p[200], title[64]; } Weapon;
static Weapon WP[MAX_WEAPONS];
static int nWP;
static void parse_weapon(Addon *a, char *v) {
    char *f[6] = {0}; int k = 0;
    for (char *p = v; k < 6; k++) { f[k] = p; p = strchr(p, '|'); if (!p) { k++; break; } *p++ = 0; }
    Weapon w = {0};
    w.addon = a;
    int ok = k >= 3 && strlen(f[0]) > 0 && strlen(f[0]) < sizeof w.name && isalpha((unsigned char)f[0][0]);
    for (const char *c = f[0]; ok && *c; c++) ok = islower((unsigned char)*c) || isdigit((unsigned char)*c) || *c == '_';
    const char *code = ok ? trim(f[1]) : "";
    ok = ok && *code && strlen(code) < sizeof w.code;
    for (const char *c = code; ok && *c; c++) ok = isalnum((unsigned char)*c);
    if (ok) ok = outfit_path(trim(f[2]), w.fp, sizeof w.fp) && (k < 4 || outfit_path(trim(f[3]), w.sm3p, sizeof w.sm3p)) &&
                 (k < 5 || outfit_path(trim(f[4]), w.skm3p, sizeof w.skm3p)) && (w.fp[0] || w.sm3p[0] || w.skm3p[0]);
    if (!ok || nWP >= MAX_WEAPONS) { LOG("addons: %s: weapon line ignored (%s): %s", a->name, ok ? "too many weapons" : "expected <name>|<code>|<FP mesh>|<3P static mesh>|<3P skeletal mesh>|<title>", f[0] ? f[0] : ""); return; }
    snprintf(w.name, sizeof w.name, "%s", f[0]);
    snprintf(w.code, sizeof w.code, "%s", code);
    snprintf(w.title, sizeof w.title, "%s", k >= 6 && *f[5] ? trim(f[5]) : w.name);
    WP[nWP++] = w;
}

// Weapon looks of the mounted add-ons; a name in two add-ons: the later one wins.
int addons_weapons(AddonWeapon *out, int max) {
    int k = 0;
    uint32_t won[MAX_WEAPONS];
    for (int i = 0; i < nWP; i++) {
        const Weapon *w = &WP[i];
        if (w->addon->mounted <= 0) continue;
        int j = 0;
        while (j < k && strcmp(out[j].name, w->name)) j++;
        if (j == k) { if (k >= max || k >= MAX_WEAPONS) continue; k++; }
        else if (won[j] > w->addon->order) continue;
        won[j] = w->addon->order;
        out[j].name = w->name; out[j].code = w->code; out[j].fp = w->fp; out[j].sm3p = w->sm3p; out[j].skm3p = w->skm3p;
        out[j].title = w->title; out[j].addon = w->addon->title;
    }
    return k;
}

// ---- addoninfo ----
static void parse_info(Addon *a, char *text) {
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *l = trim(line), *v;
        if (!*l || *l == '#' || *l == ';' || *l == '{' || *l == '}' || !strncmp(l, "//", 2)) continue;
        if ((v = strchr(l, '='))) *v++ = 0;                 // key=value
        else {                                               // L4D KeyValues: addontitle "value"
            if (*l == '"') { char *q = strchr(l + 1, '"'); if (!q) continue; v = q + 1; *q = 0; l++; }
            else { v = l + strcspn(l, " \t"); if (*v) *v++ = 0; }
        }
        char *k = unquote(l);
        v = unquote(v);
        if (!_strnicmp(k, "addon", 5)) k += 5;
        char *dst = NULL; size_t n = 0;
        if (!_stricmp(k, "title")) dst = a->title, n = sizeof a->title;
        else if (!_stricmp(k, "author")) dst = a->author, n = sizeof a->author;
        else if (!_stricmp(k, "version")) dst = a->version, n = sizeof a->version;
        else if (!_stricmp(k, "category")) dst = a->category, n = sizeof a->category;
        else if (!_stricmp(k, "description")) dst = a->desc, n = sizeof a->desc;
        else if (!_stricmp(k, "content")) dst = a->claim, n = sizeof a->claim;
        else if (!_stricmp(k, "outfit")) parse_outfit(a, v);
        else if (!_stricmp(k, "weapon")) parse_weapon(a, v);
        if (dst) snprintf(dst, n, "%s", v);
    }
}

// ---- pak index (our format only) ----
typedef struct { const uint8_t *p; size_t n, o; int bad; } Rd;
static const uint8_t *rd(Rd *r, size_t k) { if (r->bad || r->n - r->o < k) { r->bad = 1; return NULL; } const uint8_t *q = r->p + r->o; r->o += k; return q; }
static uint32_t rd32(Rd *r) { const uint8_t *q = rd(r, 4); uint32_t v = 0; if (q) memcpy(&v, q, 4); return v; }
static uint64_t rd64(Rd *r) { const uint8_t *q = rd(r, 8); uint64_t v = 0; if (q) memcpy(&v, q, 8); return v; }
static void rdstr(Rd *r, char *out, size_t n) {   // FString -> UTF-8
    int32_t len = (int32_t)rd32(r);
    out[0] = 0;
    if (len > 0 && len < 4096) {
        const uint8_t *q = rd(r, (size_t)len);
        if (q) snprintf(out, n, "%.*s", len, (const char *)q);
    } else if (len < 0 && len > -4096) {
        const uint8_t *q = rd(r, (size_t)-len * 2);
        if (q) {
            wchar_t w[4096];
            memcpy(w, q, (size_t)-len * 2);
            w[-len - 1] = 0;
            w2u(w, out, (int)n);
        }
    } else if (len) r->bad = 1;
}

static void norm_key(char *s) {   // mount point + name -> "gobi/content/..." (lower case, '/')
    for (char *c = s; *c; c++) *c = (char)tolower((unsigned char)(*c == '\\' ? '/' : *c));
    char *p = s;
    while (!strncmp(p, "../", 3)) p += 3;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

// ---- content class (addonclass.c): each .uasset's header is read from the pak ----
typedef struct { uint64_t off, size; uint32_t method; char path[200]; } Ent;
typedef struct { const char *key; size_t kl; int owner; } Slot;   // key[0..kl): a file path or a package stem
static uint64_t fnv(const char *s, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 1099511628211ull; return h; }
static Slot *slot_of(Slot *t, size_t cap, const char *k, size_t kl) {
    for (size_t i = fnv(k, kl) & (cap - 1);; i = (i + 1) & (cap - 1))
        if (!t[i].key || (t[i].kl == kl && !memcmp(t[i].key, k, kl))) return &t[i];
}
static size_t stem_len(const char *k);

static void classify(Addon *a, HANDLE h, const Ent *ents) {
    size_t cap = 16;
    while (cap < (size_t)a->nfiles * 2 + 16) cap <<= 1;
    Slot *assets = calloc(cap, sizeof *assets);   // stems that have a .uasset in this add-on
    addonclass_begin(&a->cls);
    if (!assets) { a->cls.gameplay = 1; snprintf(a->cls.reason, sizeof a->cls.reason, "out of memory"); return; }
    for (int i = 0; i < a->nfiles; i++) {
        size_t l = strlen(a->keys[i]);
        if (l > 7 && !strcmp(a->keys[i] + l - 7, ".uasset")) { Slot *s = slot_of(assets, cap, a->keys[i], l - 7); s->key = a->keys[i]; s->kl = l - 7; }
    }
    for (int i = 0; i < a->nfiles; i++) {
        const char *k = a->keys[i];
        size_t sl = stem_len(k), l = strlen(k);
        int has_uasset = slot_of(assets, cap, k, sl)->key != NULL;
        uint8_t *data = NULL;
        if (l > 7 && !strcmp(k + l - 7, ".uasset") && !ents[i].method && ents[i].size <= (32u << 20)) {
            data = malloc(ents[i].size ? ents[i].size : 1);
            if (data && !read_at(h, ents[i].off + ENTRY_HDR, data, (DWORD)ents[i].size)) { free(data); data = NULL; }
        }
        addonclass_file(&a->cls, k, ents[i].path, has_uasset, data, data ? ents[i].size : 0);
        free(data);
    }
    free(assets);
}

static int load_pak(Addon *a, const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { snprintf(a->why, sizeof a->why, "can't open (error %lu)", GetLastError()); return 0; }
    LARGE_INTEGER sz; uint8_t ft[PAK_FOOTER_MAX]; uint8_t *idx = NULL; int ok = 0; PakFooter pf;
    DWORD tl = 0;
    if (GetFileSizeEx(h, &sz)) tl = sz.QuadPart < PAK_FOOTER_MAX ? (DWORD)sz.QuadPart : PAK_FOOTER_MAX;
    if (!tl || !read_at(h, sz.QuadPart - tl, ft, tl)) { snprintf(a->why, sizeof a->why, "not a pak file"); goto out; }
    if (!pak_footer_parse(ft, tl, (uint64_t)sz.QuadPart, &pf, a->why, sizeof a->why)) goto out;
    uint64_t isz = pf.index_size, ioff = pf.index_offset;
    idx = malloc(isz ? isz : 1);
    if (!idx || !read_at(h, ioff, idx, (DWORD)isz)) { snprintf(a->why, sizeof a->why, "damaged pak (index unreadable)"); goto out; }
    if (!pak_index_ok(idx, isz, &pf, a->why, sizeof a->why)) goto out;
    a->pakver = pf.version;
    const uint8_t *hash = pf.index_sha1;
    for (int i = 0; i < 20; i++) snprintf(a->hash + 2 * i, 3, "%02x", hash[i]);
    Rd r = {idx, isz, 0, 0};
    char mount[520], name[1040], key[1600];
    rdstr(&r, mount, sizeof mount);
    int32_t n = (int32_t)rd32(&r);
    if (r.bad || n < 0 || n > 1000000) { snprintf(a->why, sizeof a->why, "damaged pak (bad index)"); goto out; }
    a->keys = calloc((size_t)n + 1, sizeof *a->keys);
    Ent *ents = calloc((size_t)n + 1, sizeof *ents);
    if (!a->keys || !ents) { snprintf(a->why, sizeof a->why, "out of memory"); free(ents); goto out; }
    for (int32_t i = 0; i < n && !r.bad; i++) {
        rdstr(&r, name, sizeof name);
        uint64_t off = rd64(&r), size = rd64(&r); rd64(&r);
        uint32_t method = rd32(&r);
        rd(&r, 20);
        if (method) { uint32_t nb = rd32(&r); rd(&r, (size_t)nb * 16); }
        rd32(&r); rd(&r, 1);
        if (r.bad) break;
        if (off > ioff || size > ioff - off) { snprintf(a->why, sizeof a->why, "damaged pak (file %s out of range)", name); free(ents); goto out; }
        snprintf(key, sizeof key, "%s%s", mount, name);
        norm_key(key);
        if (!strcmp(key, INFO_NAME)) {
            if (!method && size < 65536) {
                char *t = calloc(1, size + 1);
                if (t && read_at(h, off + ENTRY_HDR, t, (DWORD)size)) parse_info(a, t);
                free(t);
            }
            continue;
        }
        ents[a->nfiles].off = off; ents[a->nfiles].size = size; ents[a->nfiles].method = method;
        snprintf(ents[a->nfiles].path, sizeof ents[0].path, "%s", name);
        a->keys[a->nfiles++] = _strdup(key);
    }
    if (r.bad) { snprintf(a->why, sizeof a->why, "damaged pak (bad index)"); free(ents); goto out; }
    classify(a, h, ents);
    free(ents);
    ok = 1;
out:
    free(idx);
    CloseHandle(h);
    return ok;
}

static void free_keys(Addon *a) {
    if (!a->keys) return;
    for (int i = 0; i < a->nfiles; i++) free(a->keys[i]);
    free(a->keys);
    a->keys = NULL;
}

// "cosmetic (textures, materials)" / "gameplay (3 files)"
static const char *addon_class_str(const Addon *a, char *buf, size_t n) {
    char k[64];
    if (a->cls.gameplay) snprintf(buf, n, "gameplay (%d file%s)", a->cls.n_gameplay, a->cls.n_gameplay == 1 ? "" : "s");
    else snprintf(buf, n, "cosmetic%s%s%s", a->cls.kinds ? " (" : "", addonclass_kinds(a->cls.kinds, k, sizeof k), a->cls.kinds ? ")" : "");
    return buf;
}

// ---- conflicts: file path -> add-on that wins it (last in load order) ----
static Conflict *conflict(int loser, int winner) {
    for (int i = 0; i < nC; i++) if (C[i].loser == loser && C[i].winner == winner) return &C[i];
    if (nC == MAX_CONFLICTS) return NULL;
    Conflict *c = &C[nC++];
    memset(c, 0, sizeof *c);
    c->loser = loser; c->winner = winner;
    return c;
}
static size_t stem_len(const char *k) {   // "x/y.uexp" -> length of "x/y" for package files, else the whole path
    const char *dot = strrchr(k, '.'), *sl = strrchr(k, '/');
    if (!dot || (sl && dot < sl)) return strlen(k);
    if (!strcmp(dot, ".uasset") || !strcmp(dot, ".uexp") || !strcmp(dot, ".ubulk") || !strcmp(dot, ".uptnl") || !strcmp(dot, ".umap"))
        return (size_t)(dot - k);
    return strlen(k);
}

// Two add-ons on at start that are the same add-on (same content id) or declare the same outfit / weapon look name.
static void name_dup(int a, int b, const char *what, const char *name) {
    if (a == b) return;
    Conflict *c = conflict(a < b ? a : b, a < b ? b : a);
    if (!c) return;
    char add[64]; snprintf(add, sizeof add, "%s %s", what, name);
    size_t l = strlen(c->names);
    if (strstr(c->names, add)) return;
    if (l + strlen(add) + 3 < sizeof c->names) snprintf(c->names + l, sizeof c->names - l, "%s%s", l ? ", " : "", add);
}
static int dup_ok(const Addon *a) { return a->present && a->valid && a->on_at_start; }
static void find_duplicates(void) {
    for (int i = 0; i < nA; i++)
        for (int j = i + 1; j < nA; j++)
            if (dup_ok(&A[i]) && dup_ok(&A[j]) && A[i].hash[0] && !strcmp(A[i].hash, A[j].hash)) { Conflict *c = conflict(i, j); if (c) c->same = 1; }
    for (int i = 0; i < nOF; i++)
        for (int j = i + 1; j < nOF; j++)
            if (OF[i].addon != OF[j].addon && dup_ok(OF[i].addon) && dup_ok(OF[j].addon) && !strcmp(OF[i].name, OF[j].name))
                name_dup((int)(OF[i].addon - A), (int)(OF[j].addon - A), "outfit", OF[i].name);
    for (int i = 0; i < nWP; i++)
        for (int j = i + 1; j < nWP; j++)
            if (WP[i].addon != WP[j].addon && dup_ok(WP[i].addon) && dup_ok(WP[j].addon) && !strcmp(WP[i].name, WP[j].name))
                name_dup((int)(WP[i].addon - A), (int)(WP[j].addon - A), "weapon look", WP[i].name);
}

static void find_conflicts(void) {
    size_t total = 0, cap = 16;
    for (int i = 0; i < nA; i++) if (A[i].valid && A[i].on_at_start) total += A[i].nfiles;
    while (cap < total * 2 + 16) cap <<= 1;
    Slot *files = calloc(cap, sizeof *files), *stems = calloc(cap, sizeof *stems);
    if (!files || !stems) { free(files); free(stems); return; }
    for (int i = 0; i < nA; i++) {
        if (!A[i].valid || !A[i].on_at_start) continue;
        for (int j = 0; j < A[i].nfiles; j++) {
            const char *k = A[i].keys[j];
            Slot *s = slot_of(files, cap, k, strlen(k));
            if (s->key && s->owner != i) {
                Conflict *c = conflict(s->owner, i);
                if (c && !c->nfiles++) snprintf(c->example, sizeof c->example, "%s", k);
            }
            s->key = k; s->kl = strlen(k); s->owner = i;
        }
    }
    for (size_t i = 0; i < cap; i++) {   // one package from two add-ons (e.g. .uasset from A, .uexp from B)
        if (!files[i].key) continue;
        size_t sl = stem_len(files[i].key);
        Slot *s = slot_of(stems, cap, files[i].key, sl);
        if (!s->key) { s->key = files[i].key; s->kl = sl; s->owner = files[i].owner; continue; }
        if (s->owner == files[i].owner) continue;
        int lo = s->owner < files[i].owner ? s->owner : files[i].owner, hi = s->owner ^ files[i].owner ^ lo;
        Conflict *c = conflict(lo, hi);
        if (c && !c->mixed++) snprintf(c->mixed_example, sizeof c->mixed_example, "%.*s", (int)sl, files[i].key);
    }
    // stems[].key point into files[].key strings; both tables go, the keys themselves are freed with the add-ons
    free(files); free(stems);
}

// ---- which add-on serves each file now: the paths of the mounted add-ons by hash (a live mount checks its files
// against it: overriding another add-on is fine for a new add-on, which is last in the load order, but not for an
// update, which is mounted above add-ons that come after it in addonlist.txt) ----
typedef struct { uint64_t h; int owner; } Own;
static Own *own_t;
static size_t own_cap, own_n;
static uint64_t key_hash(const char *k) { uint64_t h = fnv(k, strlen(k)); return h ? h : 1; }
static Own *own_slot(Own *t, size_t cap, uint64_t h) {
    for (size_t i = h & (cap - 1);; i = (i + 1) & (cap - 1)) if (!t[i].h || t[i].h == h) return &t[i];
}
static void own_put(uint64_t h, int owner) {
    if ((own_n + 1) * 2 > own_cap) {
        size_t nc = own_cap ? own_cap * 2 : 4096;
        Own *nt = calloc(nc, sizeof *nt);
        if (!nt) return;
        for (size_t i = 0; i < own_cap; i++) if (own_t[i].h) *own_slot(nt, nc, own_t[i].h) = own_t[i];
        free(own_t); own_t = nt; own_cap = nc;
    }
    Own *s = own_slot(own_t, own_cap, h);
    if (!s->h) { s->h = h; s->owner = owner; own_n++; return; }
    if (A[s->owner].order <= A[owner].order) s->owner = owner;   // the higher read order serves the file
}
static void own_add(int ai) { for (int j = 0; j < A[ai].nfiles; j++) own_put(key_hash(A[ai].keys[j]), ai); }
static int own_get(const char *k) {   // index of the add-on serving this path now, -1 = none (the game's, or nothing)
    if (!own_cap) return -1;
    Own *s = own_slot(own_t, own_cap, key_hash(k));
    return s->h ? s->owner : -1;
}

// ---- addonlist.txt ----
static void list_path(wchar_t *out, size_t n) { swprintf(out, n, L"%ls\\" LIST_NAME, dirw); }
static int list_stat(FILETIME *mt) {
    wchar_t p[MAX_PATH + 32];
    WIN32_FILE_ATTRIBUTE_DATA a;
    list_path(p, MAX_PATH + 32);
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &a)) { memset(mt, 0, sizeof *mt); return 0; }
    *mt = a.ftLastWriteTime;
    return 1;
}

static int write_list(void) {
    wchar_t p[MAX_PATH + 32], tmp[MAX_PATH + 40];
    list_path(p, MAX_PATH + 32);
    swprintf(tmp, MAX_PATH + 40, L"%ls.tmp", p);
    FILE *f = _wfopen(tmp, L"wb");
    if (!f) { LOG("addons: can't write %ls", p); return 0; }
    fputs("# b4bcoop add-ons: one line per add-on (.pak file in this folder), =1 on, =0 off.\r\n"
          "# Load order is top to bottom: when two add-ons change the same file, the one further down wins.\r\n"
          "# New add-ons are added at the bottom, switched on. Changes apply the next time the game starts.\r\n", f);
    for (int k = 0; k < nA; k++) fprintf(f, "%s=%d\r\n", A[ord[k]].name, A[ord[k]].on);
    int ok = !fclose(f) && MoveFileExW(tmp, p, MOVEFILE_REPLACE_EXISTING);
    if (!ok) LOG("addons: can't replace %ls (error %lu)", p, GetLastError());
    list_stat(&list_mtime);
    return ok;
}

static Addon *add(const char *name8) {
    if (nA == MAX_ADDONS) return NULL;
    ord[nA] = nA;
    Addon *a = &A[nA++];
    memset(a, 0, sizeof *a);
    snprintf(a->name, sizeof a->name, "%s", name8);
    u2w(name8, a->file, MAX_PATH);
    a->on = 1;
    return a;
}
static Addon *by_name(const char *name8) {
    for (int i = 0; i < nA; i++) if (!_stricmp(A[i].name, name8)) return &A[i];
    return NULL;
}
static int cmp_name(const void *x, const void *y) { return _stricmp(*(const char **)x, *(const char **)y); }

static void read_list(void) {
    wchar_t p[MAX_PATH + 32];
    list_path(p, MAX_PATH + 32);
    FILE *f = _wfopen(p, L"rb");
    char line[700];
    while (f && fgets(line, sizeof line, f)) {
        char *l = trim(line), *v;
        if (!*l || *l == '#' || *l == ';' || !(v = strchr(l, '='))) continue;
        *v++ = 0;
        l = unquote(l); v = unquote(v);
        if (!*l || by_name(l)) continue;
        if (shop_forget(l)) { list_dirty = 1; continue; }   // removed in the ~ window, deleted at this start
        Addon *a = add(l);
        if (a) a->on = !(!strcmp(v, "0") || !_stricmp(v, "off") || !_stricmp(v, "false"));
    }
    if (f) fclose(f);
    else list_dirty = 1;
    list_stat(&list_mtime);
}

// addonlist.txt was edited while the game runs (not by us): take its on/off and order for the add-ons we know, so
// the window shows the file and our next write doesn't undo the edit. Lines for unknown files count at the next start.
static void list_sync(void) {
    FILETIME mt;
    if (!nA || !list_stat(&mt) || !CompareFileTime(&mt, &list_mtime)) return;
    wchar_t p[MAX_PATH + 32];
    list_path(p, MAX_PATH + 32);
    FILE *f = _wfopen(p, L"rb");
    if (!f) return;
    EnterCriticalSection(&cs);
    int seen[MAX_ADDONS] = {0}, no[MAX_ADDONS], n = 0;
    char line[700];
    while (fgets(line, sizeof line, f)) {
        char *l = trim(line), *v;
        if (!*l || *l == '#' || *l == ';' || !(v = strchr(l, '='))) continue;
        *v++ = 0;
        l = unquote(l); v = unquote(v);
        Addon *a = by_name(l);
        if (!a || seen[a - A]) continue;
        seen[a - A] = 1;
        a->on = !(!strcmp(v, "0") || !_stricmp(v, "off") || !_stricmp(v, "false"));
        no[n++] = (int)(a - A);
    }
    fclose(f);
    for (int k = 0; k < nA; k++) if (!seen[ord[k]]) no[n++] = ord[k];   // not in the file: keep their relative order
    memcpy(ord, no, sizeof(int) * (size_t)nA);
    list_mtime = mt;
    LeaveCriticalSection(&cs);
    LOG("addons: %s changed outside the game, re-read", LIST_NAME);
}

// Move an add-on up (-1) or down (+1) in the load order past the next present one; rewrites addonlist.txt.
// 1 = moved, 0 = already first/last, -1 = write failed (nothing changed).
static int addons_move(Addon *a, int dir) {
    EnterCriticalSection(&cs);
    int k = 0, j;
    while (k < nA && &A[ord[k]] != a) k++;
    for (j = k + dir; j >= 0 && j < nA && !A[ord[j]].present; j += dir) {}
    int r = 0;
    if (k < nA && j >= 0 && j < nA) {
        int t = ord[k]; ord[k] = ord[j]; ord[j] = t;
        r = write_list() ? 1 : -1;
        if (r < 0) { t = ord[k]; ord[k] = ord[j]; ord[j] = t; }
    }
    LeaveCriticalSection(&cs);
    if (r) LOG("addons: move %s %s -> %s", a->name, dir < 0 ? "up" : "down", r > 0 ? "saved" : "write FAILED");
    return r;
}

static int pos_now(const Addon *a) {   // 1-based place among the present add-ons in addonlist.txt order
    int p = 0;
    for (int k = 0; k < nA; k++) { if (A[ord[k]].present) p++; if (&A[ord[k]] == a) return p; }
    return 0;
}

static void config(void) {
    FILE *f = fopen(cmds_config_path(), "r");
    char line[700], custom[MAX_PATH * 3] = "";
    while (f && fgets(line, sizeof line, f)) {
        char *l = trim(line), *v;
        if (*l == '#' || *l == ';' || !(v = strchr(l, '='))) continue;
        *v++ = 0;
        v = trim(v);
        if (!strcmp(l, "addons")) enabled_cfg = atoi(v) != 0;
        else if (!strcmp(l, "shop")) shop_cfg = atoi(v) != 0;
        else if (!strcmp(l, "addons_dir") && *v) snprintf(custom, sizeof custom, "%s", v);
        else if (!strcmp(l, "addons_policy") && addons_policy_set(v)) LOG("addons: bad addons_policy=%s (any|cosmetic|none|match), keeping %s", v, addons_policy_name());
    }
    if (f) fclose(f);
    wchar_t want[MAX_PATH * 2];
    if (custom[0]) u2w(custom, want, MAX_PATH * 2);
    else {   // <game>\b4bcoop-addons: the DLL is in <game>\Gobi\Binaries\Win64
        extern char g_module_dir[];
        swprintf(want, MAX_PATH * 2, L"%hs..\\..\\..\\b4bcoop-addons", g_module_dir);
    }
    if (!GetFullPathNameW(want, MAX_PATH, dirw, NULL)) swprintf(dirw, MAX_PATH, L"%ls", want);
    size_t l = wcslen(dirw);
    while (l > 3 && (dirw[l - 1] == L'\\' || dirw[l - 1] == L'/')) dirw[--l] = 0;
    w2u(dirw, dir8, sizeof dir8);
}

static int is_ascii(const char *s) { for (; *s; s++) if ((unsigned char)*s >= 0x80) return 0; return 1; }

// DllMain (paks_early_init): read everything, decide what to mount. Returns the number of add-ons to mount.
int addons_scan(void) {
    InitializeCriticalSection(&cs);
    config();
    LOG("addons: policy for joiners: %s", addons_policy_name());
    if (!enabled_cfg) { LOG("addons: off (addons=0)"); return 0; }
    DWORD at = GetFileAttributesW(dirw);
    if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) { LOG("addons: no add-ons folder (%s)", dir8); return 0; }
    shop_early(dirw);   // the ~ window's pending removals/updates (files replaced or deleted before anything is read)
    read_list();
    // *.pak in the folder: listed ones keep their place, new ones are appended sorted by name, switched on
    char *fresh[MAX_ADDONS]; int nfresh = 0;
    wchar_t pat[MAX_PATH + 8];
    swprintf(pat, MAX_PATH + 8, L"%ls\\*.pak", dirw);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            char n8[MAX_PATH * 3];
            w2u(fd.cFileName, n8, sizeof n8);
            Addon *a = by_name(n8);
            if (a) { a->present = 1; continue; }
            if (nfresh < MAX_ADDONS) fresh[nfresh++] = _strdup(n8);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    qsort(fresh, nfresh, sizeof *fresh, cmp_name);
    for (int i = 0; i < nfresh; i++) {
        Addon *a = add(fresh[i]);
        if (a) { a->present = 1; list_dirty = 1; LOG("addons: new add-on %s, switched on", fresh[i]); }
        else LOG("addons: more than %d add-ons, %s ignored", MAX_ADDONS, fresh[i]);
        free(fresh[i]);
    }
    if (list_dirty) write_list();
    // read each add-on present; order = position in the list
    int pos = 0;
    for (int i = 0; i < nA; i++) {
        Addon *a = &A[i];
        a->on_at_start = a->on;
        if (!a->present) continue;
        a->order = ADDON_ORDER + pos++;
        a->pos0 = pos;
        wchar_t full[MAX_PATH * 2];
        swprintf(full, MAX_PATH * 2, L"%ls\\%ls", dirw, a->file);
        if (!is_ascii(a->name) || !is_ascii(dir8)) snprintf(a->why, sizeof a->why, "rename it (and its folder path) to plain letters, digits, - and _");
        else a->valid = load_pak(a, full);
        if (!a->title[0]) snprintf(a->title, sizeof a->title, "%.*s", (int)(strlen(a->name) - 4), a->name);
        if (a->valid && a->on) n_mount++;
        LOG("addons: %d. %s \"%s\" v%s by %s [%s]: %s, %d file(s), id %s%s%s%s", pos, a->name, a->title, a->version[0] ? a->version : "-",
            a->author[0] ? a->author : "-", a->category, a->on ? "on" : "off", a->nfiles, a->hash[0] ? a->hash : "-",
            a->pakver == 8 ? ", pak v8" : "", a->valid ? "" : ", NOT LOADED: ", a->why);
        if (a->valid) {
            char kinds[80];
            LOG("addons:    content: %s%s%s", addon_class_str(a, kinds, sizeof kinds), a->cls.gameplay ? ": " : "", a->cls.gameplay ? a->cls.reason : "");
            int claim_g = !_strnicmp(a->claim, "gameplay", 8), claim_c = !_strnicmp(a->claim, "cosmetic", 8);
            if ((claim_g || claim_c) && claim_g != a->cls.gameplay)
                LOG("addons:    its addoninfo says content=%s; the files say %s (the files decide)", a->claim, a->cls.gameplay ? "gameplay" : "cosmetic");
        }
    }
    find_conflicts();
    find_duplicates();
    for (int i = 0; i < nC; i++) {
        Conflict *c = &C[i];
        if (c->same)
            LOG("addons: duplicate: %s and %s are the same add-on (content id %.8s): switch one off (%s is used)",
                A[c->loser].name, A[c->winner].name, A[c->winner].hash, A[c->winner].name);
        else if (c->names[0])
            LOG("addons: duplicate: %s and %s both add %s: %s's is used (later in " LIST_NAME "); switch one off",
                A[c->loser].name, A[c->winner].name, c->names, A[c->winner].name);
        if (c->nfiles && !c->same)
            LOG("addons: conflict: %s and %s both change %d file(s) (e.g. %s): %s wins (later in " LIST_NAME ")",
                A[c->loser].name, A[c->winner].name, c->nfiles, c->example, A[c->winner].name);
        if (c->mixed)
            LOG("addons: conflict: %s and %s each replace part of the same asset (%d file(s), e.g. %s): the game may crash; switch one off",
                A[c->loser].name, A[c->winner].name, c->mixed, c->mixed_example);
    }
    for (int i = 0; i < nA; i++) if (A[i].valid && A[i].on_at_start) own_add(i);
    for (int i = 0; i < nA; i++) free_keys(&A[i]);
    LOG("addons: %s: %d add-on(s), %d to mount, %d conflict(s)", dir8, pos, n_mount, nC);
    return n_mount;
}

// Add-ons mounted in this game, in load order (addons_mp.c: the joiner's own policy check, match ids)
int addons_active(AddonRef *out, int max) {
    int k = 0;
    for (int i = 0; i < nA && k < max; i++) {
        const Addon *a = &A[i];
        if (a->mounted <= 0) continue;
        out[k].title = a->title; out[k].file = a->name; out[k].hash = a->hash;
        out[k].gameplay = a->cls.gameplay; out[k].kinds = a->cls.kinds; out[k].reason = a->cls.reason;
        k++;
    }
    return k;
}

const char *addons_title_of(const char *id8) {
    for (int i = 0; i < nA; i++) if (A[i].present && A[i].hash[0] && !_strnicmp(A[i].hash, id8, 8)) return A[i].title;
    return NULL;
}

// Get more (shop.c): an add-on in the folder other than `skip` (the shop's own <id>.pak) that is the same add-on
// (content id = pak index SHA1) or adds one of the outfits / weapon looks a catalog entry lists in `adds`
// ("outfit batman (mom); weapon look ak47 (AR02)"). Returns its file name, why = "same content" / "outfit batman".
const char *addons_installed_like(const char *skip, const char *content_id, const char *adds, char *why, size_t wn) {
    why[0] = 0;
    for (int i = 0; i < nA; i++) {
        const Addon *a = &A[i];
        if (a->present && a->hash[0] && content_id && content_id[0] && _stricmp(a->name, skip) && !_stricmp(a->hash, content_id)) {
            snprintf(why, wn, "same content");
            return a->name;
        }
    }
    for (const char *p = adds; p && *p; ) {
        while (*p == ' ' || *p == ';') p++;
        int weapon = !strncmp(p, "weapon look ", 12), outfit = !strncmp(p, "outfit ", 7);
        const char *n = p + (weapon ? 12 : outfit ? 7 : 0);
        size_t nl = strcspn(n, " ;(");
        p = n + strcspn(n, ";");
        if ((!weapon && !outfit) || !nl || nl > 32) continue;
        int cnt = weapon ? nWP : nOF;
        for (int k = 0; k < cnt; k++) {
            const Addon *a = weapon ? WP[k].addon : OF[k].addon;
            const char *nm = weapon ? WP[k].name : OF[k].name;
            if (!a->present || !_stricmp(a->name, skip) || strlen(nm) != nl || strncmp(nm, n, nl)) continue;
            snprintf(why, wn, "%s %.*s", weapon ? "weapon look" : "outfit", (int)nl, n);
            return a->name;
        }
    }
    return NULL;
}

void addons_unavailable(const char *why) { snprintf(unavailable, sizeof unavailable, "%s", why); }

// ---- runtime (add-on shop, shop.c; game thread) ----
static const char *state_of(const Addon *a);
int addons_runtime_wanted(void) { return enabled_cfg && shop_cfg; }
int addons_enabled(void) { return enabled_cfg; }
const wchar_t *addons_dir_w(void) { return dirw; }
const char *addons_dir8(void) { return dir8; }
int addons_state(const char *name8, AddonState *st) {
    memset(st, 0, sizeof *st);
    Addon *a = by_name(name8);
    if (!a || !a->present) return 0;
    st->on = a->on; st->on_at_start = a->on_at_start; st->mounted = a->mounted; st->valid = a->valid;
    st->gameplay = a->cls.gameplay;
    snprintf(st->hash, sizeof st->hash, "%s", a->hash);
    snprintf(st->state, sizeof st->state, "%s", state_of(a));
    return 1;
}
// on/off in addonlist.txt (applies at the next start), like /addons on|off. 1 = saved
int addons_set_on(const char *name8, int on) {
    Addon *a = by_name(name8);
    if (!a) return 0;
    EnterCriticalSection(&cs);
    int was = a->on;
    a->on = on;
    int ok = was == on || write_list();
    if (!ok) a->on = was;
    LeaveCriticalSection(&cs);
    LOG("addons: %s %s (~ window) -> %s", on ? "on" : "off", a->name, ok ? "saved" : "write FAILED");
    return ok;
}
// ---- loading an add-on now (the ~ window: Get more Add/Update, Installed "on"; /addons on; game thread) ----
// What can be mounted while the game runs, without a restart (docs/investigations/shop.md §1):
//  - files at new paths: nothing loaded can refer to a path that didn't exist (added outfits, weapon looks);
//  - files that replace existing ones (the game's, another add-on's, or the add-on's own older version) only when
//    every replaced file belongs to a package (.uasset/.uexp/.ubulk/.uptnl), the add-on brings every file of that
//    package that exists now (no package half from the old file, half from the new), none of those packages is in
//    memory (a loaded package keeps reading its bulk data by path: textures stream mips long after the .uasset was
//    read), and the engine's loader is idle (no package can start loading between the check and the mount: new
//    loads are only queued from the game thread, which is here). Other files (shader libraries, .locres, sounds
//    banks, .ini) are read at start: restart.
//  - only cosmetic add-ons, and as a client only what the host's add-on policy allows (addons_mp.c).
// An update is mounted from .shop\<id>.pak.new above every add-on, so it must not touch files of any other add-on.
#define MAX_REPL 512
static uint32_t next_order(void) {   // above every add-on mounted or placed so far
    uint32_t m = ADDON_ORDER - 1;
    for (int i = 0; i < nA; i++) if (A[i].order > m) m = A[i].order;
    return m + 1;
}
static int has_key(const Addon *a, const char *k) {
    for (int i = 0; i < a->nfiles; i++) if (!strcmp(a->keys[i], k)) return 1;
    return 0;
}
// "gobi/content/x/y" -> "/game/x/y"; "engine/content/x" -> "/engine/x"; "<root>/plugins/.../<p>/content/x" -> "/<p>/x"
static int package_of(const char *stem, char *out, size_t n) {
    const char *c = strstr(stem, "/content/");
    if (!c) return 0;
    size_t rl = (size_t)(c - stem);
    const char *rest = c + 9;
    if (rl == 4 && !strncmp(stem, "gobi", 4)) snprintf(out, n, "/game/%s", rest);
    else if (rl == 6 && !strncmp(stem, "engine", 6)) snprintf(out, n, "/engine/%s", rest);
    else if (strstr(stem, "/plugins/") && strstr(stem, "/plugins/") < c) {
        const char *p = c;
        while (p > stem && p[-1] != '/') p--;
        snprintf(out, n, "/%.*s/%s", (int)(c - p), p, rest);
    } else return 0;
    return 1;
}
// NULL = it can be mounted now; else why not (ex: an example file or package). *retry: only the engine's loader is
// busy right now (ask again in a moment). upd = the add-on whose older version `n` updates (-1: none).
static const char *live_check(Addon *n, int upd, char *ex, size_t en, int *retry) {
    static char pol[160], why[200];
    *retry = 0; ex[0] = 0;
    if (n->cls.gameplay) return "it changes gameplay";
    if (unavailable[0]) return unavailable;
    if (!paks_runtime_ready()) return "the game's pak layer was not found";
    if (!addons_session_allows(n->cls.gameplay, pol, sizeof pol)) return pol;
    if (!n->keys) return "its file list is not available";
    static char pk[MAX_REPL][260];
    const char *pv[MAX_REPL];
    int np = 0, nrep = 0;
    for (int i = 0; i < n->nfiles; i++) {
        const char *k = n->keys[i];
        if (!paks_file_exists(k)) continue;   // a new path
        nrep++;
        // another add-on's file: an update (mounted above all add-ons) must not touch any; a new add-on may override
        // those below it in the load order (as after a restart), not one above it (that one keeps parts of the asset)
        int ow = own_get(k);
        if (ow >= 0 && ow != upd && (upd >= 0 || A[ow].order > n->order)) {
            snprintf(ex, en, "%s", k);
            snprintf(why, sizeof why, "the add-on \"%.80s\" changes the same files", A[ow].title);
            return why;
        }
        size_t sl = stem_len(k);
        if (sl == strlen(k)) { snprintf(ex, en, "%s", k); return "it replaces a file the game reads only when it starts"; }
        char stem[260], pkg[260];
        snprintf(stem, sizeof stem, "%.*s", (int)sl, k);
        if (!package_of(stem, pkg, sizeof pkg)) { snprintf(ex, en, "%s", k); return "it replaces a file the game reads only when it starts"; }
        int seen = 0;
        for (int j = 0; j < np && !seen; j++) seen = !strcmp(pk[j], pkg);
        if (seen) continue;
        static const char *EXT[] = {".uasset", ".uexp", ".ubulk", ".uptnl", ".umap"};
        for (int e = 0; e < 5; e++) {   // the whole package must come from this add-on
            char f[280];
            snprintf(f, sizeof f, "%s%s", stem, EXT[e]);
            if (!has_key(n, f) && paks_file_exists(f)) { snprintf(ex, en, "%s", f); return "it replaces only part of an asset (the rest would stay the old one)"; }
        }
        if (np == MAX_REPL) return "it replaces too many existing assets to check them now";
        snprintf(pk[np], sizeof pk[0], "%s", pkg);
        pv[np] = pk[np];
        np++;
    }
    if (!nrep) return NULL;
    int busy = paks_async_loading();
    if (busy < 0) return "it replaces existing assets and the game's loader can't be checked";
    if (busy) { *retry = 1; return "the game is loading something right now"; }
    int k = paks_package_loaded(pv, np);
    if (k >= 0) { snprintf(ex, en, "%s", pv[k]); return "it replaces an asset the game is using now"; }
    LOG("addons: %s replaces %d existing file(s) (%d asset(s)), none of them loaded now", n->name, nrep, np);
    return NULL;
}
// outfit=/weapon= entries of an add-on: dropped (before its file is read again) or handed to another Addon (update)
static void names_drop(const Addon *a) {
    int k = 0;
    for (int i = 0; i < nOF; i++) if (OF[i].addon != a) OF[k++] = OF[i];
    nOF = k; k = 0;
    for (int i = 0; i < nWP; i++) if (WP[i].addon != a) WP[k++] = WP[i];
    nWP = k;
}
static void names_move(const Addon *from, Addon *to) {
    for (int i = 0; i < nOF; i++) if (OF[i].addon == from) OF[i].addon = to;
    for (int i = 0; i < nWP; i++) if (WP[i].addon == from) WP[i].addon = to;
}
static void reread(Addon *a) {   // its file list again (freed after the start's checks)
    wchar_t full[MAX_PATH * 2];
    swprintf(full, MAX_PATH * 2, L"%ls\\%ls", dirw, a->file);
    free_keys(a);
    names_drop(a);
    a->nfiles = 0;
    a->why[0] = 0;
    if (!is_ascii(a->name) || !is_ascii(dir8)) { snprintf(a->why, sizeof a->why, "rename it (and its folder path) to plain letters, digits, - and _"); a->valid = 0; }
    else a->valid = load_pak(a, full);
}
static void after_live_mount(int ai) {
    own_add(ai);
    free_keys(&A[ai]);
    models_outfits_refresh();
    wlooks_refresh();
}
static void later_msg(char *msg, size_t mn, const char *later, const char *ex) {
    snprintf(msg, mn, "applies after a restart (%s%s%s)", later, ex[0] ? ": " : "", ex);
}

// An add-on in the folder, switched on, not loaded: load it now if that is safe. last: no "ask again" (3) answer.
// 2 = loaded now, 1 = at the next start (msg says why), 3 = the game's loader is busy: ask again in a moment.
int addons_load_now(const char *name8, int last, char *msg, size_t mn) {
    Addon *a = by_name(name8);
    if (!a || !a->present) { snprintf(msg, mn, "not in the add-ons folder"); return 1; }
    if (a->mounted > 0) { snprintf(msg, mn, "already loaded"); return 2; }
    if (!a->on) { snprintf(msg, mn, "it is switched off"); return 1; }
    if (!a->keys) reread(a);
    if (!a->valid) { free_keys(a); snprintf(msg, mn, "it can't load: %s", a->why); return 1; }
    if (!a->order) a->order = next_order();
    char ex[280];
    int retry;
    const char *later = live_check(a, -1, ex, sizeof ex, &retry);
    if (later && retry && !last) return 3;
    if (later) {
        free_keys(a);
        LOG("addons: %s: next start (%s%s%s)", a->name, later, ex[0] ? ", e.g. " : "", ex);
        later_msg(msg, mn, later, ex);
        return 1;
    }
    wchar_t full[MAX_PATH * 2];
    swprintf(full, MAX_PATH * 2, L"%ls\\%ls", dirw, a->file);
    char err[120];
    a->mounted = paks_mount_runtime(full, a->order, err, sizeof err) ? 1 : -1;
    LOG("addons: %s -> %s (read order %u, at runtime)%s%s", a->name, a->mounted > 0 ? "mounted" : "MOUNT FAILED", a->order,
        a->mounted > 0 ? "" : ": ", a->mounted > 0 ? "" : err);
    if (a->mounted < 0) { free_keys(a); a->mounted = 0; snprintf(msg, mn, "could not be loaded now (%s): applies after a restart", err); return 1; }
    a->on_at_start = 1;   // loaded now: nothing pending
    after_live_mount((int)(a - A));
    snprintf(msg, mn, "ready now");
    return 2;
}

// A pak just put into the folder (Get more's Add): read it like at start, append it to addonlist.txt (switched
// on, last = wins) and load it now when that is safe (addons_load_now). Return values as addons_load_now, 0 = not added.
int addons_add_runtime(const char *name8, char *msg, size_t mn) {
    if (!enabled_cfg) { snprintf(msg, mn, "add-ons are off (addons=0)"); return 0; }
    Addon *a = by_name(name8);
    int fresh = !a;
    if (!a && !(a = add(name8))) { snprintf(msg, mn, "more than %d add-ons", MAX_ADDONS); return 0; }
    if (fresh || !a->present) {
        a->present = 1;
        int pos = 0;
        for (int i = 0; i < nA; i++) pos += A[i].present;
        a->pos0 = pos;
        a->order = next_order();
    } else if (a->mounted > 0) { snprintf(msg, mn, "already loaded"); return 2; }
    a->on = 1;
    a->on_at_start = 0;
    if (!a->valid && !a->nfiles) {   // not read at start (new file): read it now
        memset(&a->cls, 0, sizeof a->cls);
        reread(a);
        if (!a->title[0]) snprintf(a->title, sizeof a->title, "%.*s", (int)(strlen(a->name) - 4), a->name);
    }
    EnterCriticalSection(&cs);
    int wrote = write_list();
    LeaveCriticalSection(&cs);
    LOG("addons: added %s \"%s\" (~ window, Get more), %d file(s), id %s%s%s%s", a->name, a->title, a->nfiles, a->hash[0] ? a->hash : "-",
        a->valid ? "" : ", NOT LOADABLE: ", a->why, wrote ? "" : "; addonlist.txt write FAILED");
    return addons_load_now(name8, 0, msg, mn);
}

// Update (shop.c): the newer file `newpath` (.shop\<id>.pak.new, swapped in by the next start) of a loaded
// add-on. Mounted now, above the old one, when it is cosmetic, touches no other add-on's files and none of the
// assets it replaces (its older version's included) is in memory; the add-on's outfits / weapon looks then come
// from the new file. Return values as addons_load_now.
static Addon UPD;   // the new version while it is checked
int addons_update_runtime(const char *name8, const wchar_t *newpath, int last, char *msg, size_t mn) {
    Addon *a = by_name(name8);
    if (!enabled_cfg || !a || !a->present || a->mounted <= 0 || !a->on) {
        snprintf(msg, mn, "%s", !a || a->mounted <= 0 ? "its current version isn't loaded either" : "it is switched off");
        return 1;
    }
    memset(&UPD, 0, sizeof UPD);
    snprintf(UPD.name, sizeof UPD.name, "%s", a->name);
    wcsncpy(UPD.file, a->file, MAX_PATH - 1);
    UPD.present = 1;
    UPD.valid = load_pak(&UPD, newpath);
    char ex[280] = "";
    int retry = 0;
    const char *later = UPD.valid ? live_check(&UPD, (int)(a - A), ex, sizeof ex, &retry) : UPD.why;
    if (later) {
        names_drop(&UPD);
        free_keys(&UPD);
        if (retry && !last) return 3;
        LOG("addons: update of %s: next start (%s%s%s)", a->name, later, ex[0] ? ", e.g. " : "", ex);
        snprintf(msg, mn, "%s%s%s", later, ex[0] ? ": " : "", ex);
        return 1;
    }
    uint32_t order = next_order();
    char err[120];
    int ok = paks_mount_runtime(newpath, order, err, sizeof err);
    LOG("addons: update of %s (%ls) -> %s (read order %u, at runtime; the file is swapped in at the next start)%s%s", a->name,
        newpath, ok ? "mounted" : "MOUNT FAILED", order, ok ? "" : ": ", ok ? "" : err);
    if (!ok) {
        names_drop(&UPD);
        free_keys(&UPD);
        snprintf(msg, mn, "the game refused to load it now (%s)", err);
        return 1;
    }
    names_drop(a);   // the old version's outfit=/weapon= lines give way to the new ones
    names_move(&UPD, a);
    snprintf(a->title, sizeof a->title, "%s", UPD.title[0] ? UPD.title : a->title);
    snprintf(a->author, sizeof a->author, "%s", UPD.author);
    snprintf(a->version, sizeof a->version, "%s", UPD.version);
    snprintf(a->category, sizeof a->category, "%s", UPD.category);
    snprintf(a->desc, sizeof a->desc, "%s", UPD.desc);
    snprintf(a->claim, sizeof a->claim, "%s", UPD.claim);
    snprintf(a->hash, sizeof a->hash, "%s", UPD.hash);
    a->cls = UPD.cls;
    a->pakver = UPD.pakver;
    a->order = order;
    free_keys(a);
    a->keys = UPD.keys; a->nfiles = UPD.nfiles;
    UPD.keys = NULL;
    after_live_mount((int)(a - A));
    snprintf(msg, mn, "ready now");
    return 2;
}

// FPakPlatformFile::Initialize hook, right after the retail paks: mount in load order (later = higher read order).
void addons_mount(void) {
    for (int i = 0; i < nA; i++) {
        Addon *a = &A[i];
        if (!a->present || !a->valid || !a->on_at_start) continue;
        wchar_t full[MAX_PATH * 2];
        swprintf(full, MAX_PATH * 2, L"%ls\\%ls", dirw, a->file);
        a->mounted = paks_mount_unsigned(full, a->order) ? 1 : -1;
        LOG("addons: %s -> %s (read order %u)", a->name, a->mounted > 0 ? "mounted" : "MOUNT FAILED", a->order);
    }
}

// init_thread: one chat notice for the player (shown once they are in a map).
void addons_init(void) {
    int bad = 0, failed = 0;
    for (int i = 0; i < nA; i++) {
        if (A[i].present && A[i].on_at_start && !A[i].valid) bad++;
        if (A[i].mounted < 0) failed++;
    }
    char msg[300] = "";
    const Conflict *mix = NULL, *dup = NULL;
    for (int i = 0; i < nC && !mix; i++) if (C[i].mixed) mix = &C[i];
    for (int i = 0; i < nC && !dup; i++) if (C[i].same || C[i].names[0]) dup = &C[i];
    char more[40] = "";
    if (nC > 1) snprintf(more, sizeof more, " (%d more conflict%s)", nC - 1, nC == 2 ? "" : "s");
    if (unavailable[0] && n_mount) snprintf(msg, sizeof msg, "Add-ons are off: %s. /addons", unavailable);
    else if (mix)   // the one that can crash the game first
        snprintf(msg, sizeof msg, "Add-ons \"%s\" and \"%s\" mix parts of one asset (may crash): switch one off.%s /addons",
                 A[mix->loser].title, A[mix->winner].title, more);
    else if (dup && dup->same)
        snprintf(msg, sizeof msg, "Add-on \"%s\" is installed twice (%s and %s): switch one off.%s /addons",
                 A[dup->winner].title, A[dup->loser].name, A[dup->winner].name, more);
    else if (dup)
        snprintf(msg, sizeof msg, "Add-ons %s and %s both add %s: %s's is used. Switch one off.%s /addons",
                 A[dup->loser].name, A[dup->winner].name, dup->names, A[dup->winner].name, more);
    else if (nC == 1)
        snprintf(msg, sizeof msg, "Add-on conflict: \"%s\" overrides \"%s\". /addons", A[C[0].winner].title, A[C[0].loser].title);
    else if (nC) snprintf(msg, sizeof msg, "%d add-on conflicts. /addons for details", nC);
    else if (bad || failed) snprintf(msg, sizeof msg, "%d add-on(s) could not be loaded. /addons for details", bad + failed);
    if (msg[0]) { LOG("addons: notice: %s", msg); chat_local_later(msg); }
}

// ---- chat /addons ----
static int order_changed(void) {   // the present add-ons' order in addonlist.txt differs from the mounted one
    for (int i = 0; i < nA; i++) if (A[i].present && pos_now(&A[i]) != A[i].pos0) return 1;
    return 0;
}
static const char *state_of(const Addon *a) {
    if (!a->present) return "missing";
    if (a->on != a->on_at_start) return a->on ? "on after restart" : "off after restart";
    if (!a->on) return "off";
    if (!a->valid) return "on, NOT LOADED";
    if (a->mounted > 0) return "on";
    if (a->mounted < 0) return "on, MOUNT FAILED";
    return unavailable[0] ? "on, not loaded" : "on";
}

static Addon *pick(const char *arg, int *num, Out *o) {   // "#", "3" or a file name (with or without .pak)
    while (arg && (*arg == ' ' || *arg == '#')) arg++;
    if (!arg || !*arg) { out_printf(o, "which add-on? /addons lists them with numbers\n"); return NULL; }
    // a number, the file name (with or without .pak) or the title; else a part of one of them matching one add-on
    int k = 0, nsub = 0, ksub = 0;
    Addon *sub = NULL;
    char want[MAX_PATH];
    snprintf(want, sizeof want, "%s", arg);
    for (char *c = want; *c; c++) *c = (char)tolower((unsigned char)*c);
    const char *end = arg + strspn(arg, "0123456789");
    while (*end == ' ') end++;
    int number = *end ? 0 : atoi(arg);   // "2", not "2_skin.pak"
    for (int q = 0; q < nA; q++) {
        Addon *a = &A[ord[q]];
        if (!a->present) continue;
        k++;
        char base[MAX_PATH], hay[MAX_PATH + 100];
        snprintf(base, sizeof base, "%.*s", (int)(strlen(a->name) - 4), a->name);
        if (number == k || !_stricmp(arg, a->name) || !_stricmp(arg, base) || !_stricmp(arg, a->title)) { *num = k; return a; }
        snprintf(hay, sizeof hay, "%s|%s", a->name, a->title);
        for (char *c = hay; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (strstr(hay, want)) { nsub++; sub = a; ksub = k; }
    }
    if (nsub == 1) { *num = ksub; return sub; }
    out_printf(o, nsub ? "\"%s\" matches %d add-ons: use the number (/addons)\n" : "no add-on \"%s\" (/addons)\n", arg, nsub);
    return NULL;
}

void addons_slash(const char *verb, char *rest, Out *o) {
    (void)verb;
    char *sub = rest ? strtok(rest, " ") : NULL, *arg = sub ? strtok(NULL, "") : NULL;
    if (sub) for (char *c = sub; *c; c++) *c = (char)tolower((unsigned char)*c);
    int present = 0;
    for (int i = 0; i < nA; i++) present += A[i].present;
    if (!sub || !strcmp(sub, "list")) {
        if (!enabled_cfg) { out_printf(o, "add-ons are off (addons=0 in b4bcoop.ini)\n"); return; }
        if (!present) { out_printf(o, "no add-ons. Put add-on .pak files in %s and restart\n", dir8); return; }
        if (unavailable[0]) out_printf(o, "add-ons are off: %s\n", unavailable);
        out_printf(o, "%d add-on(s), load order (a later one wins):\n", present);
        int k = 0;
        for (int q = 0; q < nA; q++) {
            Addon *a = &A[ord[q]];
            if (!a->present) continue;
            out_printf(o, "%d. %s%s%s [%s%s] %s\n", ++k, a->title, a->version[0] ? " " : "", a->version, state_of(a),
                       !a->valid ? "" : a->cls.gameplay ? ", gameplay" : ", cosmetic", a->name);
        }
        if (order_changed()) out_printf(o, "load order changed in " LIST_NAME ": applies after restart\n");
        for (int i = 0; i < nC; i++) {
            if (C[i].same) out_printf(o, "duplicate: %s and %s are the same add-on: switch one off\n", A[C[i].loser].name, A[C[i].winner].name);
            else if (C[i].names[0]) out_printf(o, "duplicate: %s and %s both add %s (%s's is used): switch one off\n", A[C[i].loser].name, A[C[i].winner].name, C[i].names, A[C[i].winner].name);
            if (C[i].nfiles && !C[i].same) out_printf(o, "conflict: %s overrides %s (%d file(s))\n", A[C[i].winner].name, A[C[i].loser].name, C[i].nfiles);
            if (C[i].mixed) out_printf(o, "conflict: %s and %s mix parts of one asset: may crash, switch one off\n", A[C[i].loser].name, A[C[i].winner].name);
        }
        out_printf(o, "/addons on|off <#>  /addons info <#>  /addons policy\n");
        return;
    }
    int num = 0;
    if (!strcmp(sub, "info")) {
        Addon *a = pick(arg, &num, o);
        if (!a) return;
        out_printf(o, "%d. %s%s%s (%s)\n", num, a->title, a->version[0] ? " " : "", a->version, a->name);
        if (a->author[0] || a->category[0])
            out_printf(o, "%s%s%s%s\n", a->author[0] ? "by " : "", a->author, a->author[0] && a->category[0] ? ", " : "", a->category);
        if (a->desc[0]) out_printf(o, "%s\n", a->desc);
        out_printf(o, "[%s] %d file(s)%s%s\n", state_of(a), a->nfiles, a->valid ? "" : ": ", a->valid ? "" : a->why);
        if (a->valid) {
            char k[80];
            out_printf(o, "content: %s%s%s\nid %.8s\n", addon_class_str(a, k, sizeof k), a->cls.gameplay ? ", e.g. " : "",
                       a->cls.gameplay ? a->cls.reason : "", a->hash);
        }
        return;
    }
    if (!strcmp(sub, "on") || !strcmp(sub, "off") || !strcmp(sub, "enable") || !strcmp(sub, "disable")) {
        int on = !strcmp(sub, "on") || !strcmp(sub, "enable");
        Addon *a = pick(arg, &num, o);
        if (!a) return;
        EnterCriticalSection(&cs);
        int was = a->on;
        a->on = on;
        int ok = was == on || write_list();
        if (!ok) a->on = was;
        LeaveCriticalSection(&cs);
        LOG("addons: /addons %s %s -> %s", sub, a->name, ok ? "saved" : "write FAILED");
        if (ok && on && a->present && a->valid && a->mounted <= 0) {   // not loaded yet: now, when that is safe
            char m[320];
            int r = addons_load_now(a->name, 0, m, sizeof m);
            if (r == 2) out_printf(o, "%s: on, ready now\n", a->title);
            else if (r == 3) out_printf(o, "%s: on; the game is loading something right now: /addons on %d again in a moment loads it now, else the next start does\n", a->title, num);
            else out_printf(o, "%s: on, %s\n", a->title, m);
            return;
        }
        if (!ok) out_printf(o, "could not write %s\\" LIST_NAME "\n", dir8);
        else if (on == a->on_at_start) out_printf(o, "%s: %s (as now)\n", a->title, on ? "on" : "off");
        else out_printf(o, "%s: %s, applies after restart\n", a->title, on ? "on" : "off");
        if (ok && on && !a->valid) out_printf(o, "note: it can't load: %s\n", a->why);
        return;
    }
    if (addons_mp_slash(sub, arg, o)) return;
    out_printf(o, "usage: /addons [list]  /addons on|off <#>  /addons info <#>  /addons policy [any|cosmetic|none|match]\n");
}

// ---- ~ window, "Looks & add-ons" tab (looks.c; overlay.h; game thread): Installed, and the Wear / Use buttons of any
// add-on row (Get more too). On/off runs /addons on|off (ov_run), the order buttons addons_move(); both write
// addonlist.txt. Update / Remove / Undo: shop.c. Host rules (policy): addons_mp_panel(), drawn by looks.c. ----
static void looks_count(const Addon *a, int *no, int *nw) {
    *no = *nw = 0;
    for (int i = 0; i < nOF; i++) *no += OF[i].addon == a;
    for (int i = 0; i < nWP; i++) *nw += WP[i].addon == a;
}
// Wear (an outfit) / Use (a weapon look) for yourself, while the add-on is loaded; several looks: "Wear (n)" shows
// them in Wear. 1 = something drawn.
int addons_look_buttons(const char *file) {
    Addon *a = by_name(file);
    if (!a || !a->present || a->mounted <= 0) return 0;
    int no, nw;
    looks_count(a, &no, &nw);
    if (!no && !nw) return 0;
    char lab[96];
    ov_begin_disabled(models_off(), "Model swaps are off (Host rules, below).");
    if (no + nw == 1) {
        const char *name = NULL;
        for (int i = 0; i < nOF && !name; i++) if (OF[i].addon == a) name = OF[i].name;
        for (int i = 0; i < nWP && !name; i++) if (WP[i].addon == a) name = WP[i].name;
        snprintf(lab, sizeof lab, "%s##%s", no ? "Wear" : "Use", name);
        if (ov_button(lab)) ov_run("model %s", name);
        ov_tooltip(no ? "You wear it now (/model <name>): players with the same add-on see it on you, the others your survivor."
                      : "Your weapon of that type shows it now (/model <name>); players without the add-on see the normal weapon.");
    } else {
        snprintf(lab, sizeof lab, "Wear (%d)##looks:%s", no + nw, a->name);
        if (ov_button(lab)) looks_show(LOOKS_WEAR, a->title, -1);
        ov_tooltip("Its outfits and weapon looks, in Wear.");
    }
    ov_end_disabled();
    return 1;
}
static const char *kind_str(const Addon *a, char *b, size_t n) {
    int no, nw;
    looks_count(a, &no, &nw);
    if (!a->valid) snprintf(b, n, "-");
    else if (a->cls.gameplay) snprintf(b, n, "gameplay");
    else if (no && nw) snprintf(b, n, "%d outfit%s, %d weapon look%s", no, no == 1 ? "" : "s", nw, nw == 1 ? "" : "s");
    else if (no) { if (no == 1) snprintf(b, n, "outfit"); else snprintf(b, n, "%d outfits", no); }
    else if (nw) {
        const char *code = "";
        for (int i = 0; i < nWP; i++) if (WP[i].addon == a) code = WP[i].code;
        if (nw == 1) snprintf(b, n, "weapon look (%s)", code); else snprintf(b, n, "%d weapon looks", nw);
    } else snprintf(b, n, "replacement");
    return b;
}
static int row_match(const Addon *a, const char *s) {
    if (!*s) return 1;
    size_t n = strlen(s);
    const char *f[6] = {a->title, a->name, a->author, a->category, a->desc, a->version};
    for (int k = 0; k < 6; k++) for (const char *h = f[k]; *h; h++) if (!_strnicmp(h, s, n)) return 1;
    for (int i = 0; i < nOF; i++) if (OF[i].addon == a) for (const char *h = OF[i].name; *h; h++) if (!_strnicmp(h, s, n)) return 1;
    for (int i = 0; i < nWP; i++) if (WP[i].addon == a) for (const char *h = WP[i].name; *h; h++) if (!_strnicmp(h, s, n)) return 1;
    return 0;
}
static void panel_details(Addon *a) {
    char b[160], lab[MAX_PATH + 24];
    if (a->author[0] || a->category[0])
        ov_text_dim("%s%s%s%s", a->author[0] ? "by " : "", a->author, a->author[0] && a->category[0] ? ", " : "", a->category);
    if (a->desc[0]) ov_text_dim("%s", a->desc);
    ov_text("File: %s, %d file(s) inside", a->name, a->nfiles);
    ov_text("State: %s; load order #%d%s", state_of(a), pos_now(a), pos_now(a) != a->pos0 ? " after restart" : "");
    if (!a->valid) ov_text_warn("Not loaded: %s", a->why);
    else {
        ov_text("Content: %s", addon_class_str(a, b, sizeof b));
        if (a->cls.gameplay) ov_text_dim("First gameplay file: %s. With it on, you can't join hosts with "
                                         "addons_policy=cosmetic or none.", a->cls.reason);
        int claim_g = !_strnicmp(a->claim, "gameplay", 8), claim_c = !_strnicmp(a->claim, "cosmetic", 8);
        if ((claim_g || claim_c) && claim_g != a->cls.gameplay)
            ov_text_dim("Its author labels it %s; the files decide.", a->claim);
        ov_text("Id: %.8s", a->hash);
        ov_same_line();
        if (ov_button("Copy##id")) ov_copy(a->hash);
        ov_tooltip("The content id (pak index SHA1). Only a host with addons_policy=match shows joiners its own (first 8 digits).");
    }
    int no, nw;
    looks_count(a, &no, &nw);
    if (no + nw) {
        ov_text(a->mounted > 0 ? "Looks it adds (for yourself; /model <name>):" : "Looks it adds (once it is loaded):");
        ov_begin_disabled(a->mounted <= 0 || models_off(), a->mounted <= 0 ? "Not loaded yet (see State)." : "Model swaps are off (Host rules, below).");
        for (int i = 0; i < nOF; i++) {
            const Outfit *o = &OF[i];
            if (o->addon != a) continue;
            snprintf(lab, sizeof lab, "Wear##d:%s", o->name);
            if (ov_button(lab)) ov_run("model %s", o->name);
            ov_same_line();
            ov_text_dim("outfit %s: %s (made on %s)", o->name, o->title, o->hero);
        }
        for (int i = 0; i < nWP; i++) {
            const Weapon *w = &WP[i];
            if (w->addon != a) continue;
            snprintf(lab, sizeof lab, "Use##d:%s", w->name);
            if (ov_button(lab)) ov_run("model %s", w->name);
            ov_same_line();
            ov_text_dim("weapon look %s: %s (%s)", w->name, w->title, w->code);
        }
        ov_end_disabled();
    }
    for (int i = 0; i < nC; i++) {
        const Conflict *c = &C[i];
        const Addon *other = &A[c->loser] == a ? &A[c->winner] : &A[c->winner] == a ? &A[c->loser] : NULL;
        if (!other) continue;
        if (c->same) ov_text_warn("Installed twice: %s is the same add-on. Switch one off.", other->name);
        else if (c->names[0]) ov_text_warn("%s also adds %s (%s's is used). Switch one off.", other->name, c->names, A[c->winner].name);
        if (c->nfiles && !c->same) ov_text_dim("%s \"%s\" (%d file(s), e.g. %s)", &A[c->winner] == a ? "Overrides" : "Overridden by",
                                   other->title, c->nfiles, c->example);
        if (c->mixed) ov_text_warn("Mixes parts of one asset with \"%s\" (e.g. %s): may crash, switch one off.", other->title, c->mixed_example);
    }
    if (ov_button("Show in the log (/addons info)")) ov_run("addons info %s", a->name);
    if (!shop_pending(a->name)) {
        ov_same_line();
        snprintf(lab, sizeof lab, "Remove##%s", a->name);
        if (ov_button_confirm(lab, "Click again to remove")) { const char *e = shop_remove(a->name); if (e) overlay_note(e); }
        ov_tooltip("Switched off now; its file is deleted when the game next starts (Undo until then).");
    }
}

static void load_checkbox(void) {
    const char *v = cmds_ini_value("addons");
    int load = !(v && atoi(v) == 0);
    if (ov_checkbox("Load add-ons##addons", &load)) ov_setting("addons", load ? NULL : "0", 1);
    ov_tooltip("Off = addons=0 in b4bcoop.ini: no add-on is loaded (troubleshooting). Applies after a restart.");
}
static void folder_section(void) {
    if (!ov_header("Folder and troubleshooting###addonsfolder", 0)) return;
    ov_text("%s", dir8);
    ov_same_line();
    if (ov_button("Copy##dir")) ov_copy(dir8);
    ov_text_dim("By hand: put an add-on's .pak there (an add-on zip: extract it into the game folder), then restart the "
                "game; delete the .pak to remove it. addonlist.txt there holds on/off and the load order.");
    if (enabled_cfg) load_checkbox();
    if (ov_button("List in the log (/addons)")) ov_run("addons");
}

// Installed: the add-ons in load order with on/off, kind and state, Wear / Use, Update, Up / Down; click one for its
// details (under its row) with Remove.
void addons_installed_panel(const char *search) {
    static ULONGLONG t_sync;
    static int sel = -1;   // index into A
    if (GetTickCount64() - t_sync > 1000) { t_sync = GetTickCount64(); list_sync(); }
    const char *v = cmds_ini_value("addons");
    int load = !(v && atoi(v) == 0);
    if (!enabled_cfg) { ov_text_warn("Add-ons are off (addons=0 in b4bcoop.ini): none is loaded."); load_checkbox(); }
    if (load != enabled_cfg) ov_text_warn("Add-ons will be %s after you restart the game.", load ? "loaded" : "off");
    if (unavailable[0] && enabled_cfg) ov_text_warn("Add-ons are off: %s.", unavailable);
    if (!enabled_cfg) { folder_section(); return; }
    int present = 0, nsw = 0;
    for (int i = 0; i < nA; i++) if (A[i].present) { present++; nsw += A[i].on != A[i].on_at_start; }
    if (nsw || order_changed())
        ov_text_warn("%s differs from what is loaded now (%d switched%s): restart the game to apply.", LIST_NAME, nsw,
                     order_changed() ? ", load order changed" : "");
    if (!present) ov_text_dim("No add-ons yet. Get more (above) lists free ones; or put add-on .pak files in the folder (below) and restart the game.");
    else {
        shop_check_button();
        ov_text_dim("%d add-on(s) in load order: further down wins when two change the same file.", present);
        if (sel >= 0 && (sel >= nA || !A[sel].present)) sel = -1;
        int open = ov_table_begin("addons", 4), k = 0, shown = 0;
        for (int q = 0; q < nA && open; q++) {
            int i = ord[q];
            Addon *a = &A[i];
            if (!a->present) continue;
            k++;
            if (!row_match(a, search)) continue;
            shown++;
            char lab[MAX_PATH + 24], kind[64];
            ov_push_id(i);
            ov_table_next();
            int on = a->on;
            snprintf(lab, sizeof lab, "##on:%s", a->name);
            if (ov_checkbox(lab, &on)) ov_run("addons %s %s", on ? "on" : "off", a->name);
            ov_tooltip("On / off (/addons on|off, writes addonlist.txt). On: loaded right away when that is safe, else at the "
                       "next start. Off: at the next start.");
            ov_table_next();
            snprintf(lab, sizeof lab, "%d. %s%s%s##sel:%s", k, a->title, a->version[0] ? " " : "", a->version, a->name);
            if (ov_selectable(lab, sel == i)) sel = sel == i ? -1 : i;
            ov_tooltip("Click for the details (author, content, looks it adds, conflicts, Remove).");
            kind_str(a, kind, sizeof kind);
            if (!a->valid) ov_text_warn("not loaded: %s", a->why);
            else if (a->cls.gameplay) ov_text_warn("gameplay, %s%s", state_of(a), pos_now(a) != a->pos0 ? ", moves after restart" : "");
            else ov_text_dim("%s, %s%s", kind, state_of(a), pos_now(a) != a->pos0 ? ", moves after restart" : "");
            shop_item_note(a->name);
            ov_table_next();
            addons_look_buttons(a->name);
            shop_installed_actions(a->name);
            ov_table_next();
            ov_begin_disabled(k == 1, "Already first.");
            snprintf(lab, sizeof lab, "Up##%s", a->name);
            if (ov_button(lab) && addons_move(a, -1) < 0) overlay_note("could not write addonlist.txt");
            ov_tooltip("Earlier in the load order (applies at the next start).");
            ov_end_disabled();
            ov_same_line();
            ov_begin_disabled(k == present, "Already last.");
            snprintf(lab, sizeof lab, "Down##%s", a->name);
            if (ov_button(lab) && addons_move(a, 1) < 0) overlay_note("could not write addonlist.txt");
            ov_tooltip("Later in the load order: wins conflicts (applies at the next start).");
            ov_end_disabled();
            ov_pop_id();
            if (sel == i) {   // the details open under the row: this table ends, a new one (same columns) follows
                ov_table_end();
                panel_details(a);
                ov_separator();
                open = ov_table_begin("addons", 4);
            }
        }
        if (open) ov_table_end();
        if (!shown) ov_text_dim("No add-on matches the search.");
        int conflicts = 0;
        for (int i = 0; i < nC; i++) {
            const Conflict *c = &C[i];
            if (!conflicts++) ov_heading("Conflicts (as loaded now)");
            if (c->same) ov_text_warn("\"%s\" is installed twice (%s and %s): switch one off.", A[c->winner].title, A[c->loser].name, A[c->winner].name);
            else if (c->names[0]) ov_text_warn("%s and %s both add %s (%s's is used): switch one off.", A[c->loser].name, A[c->winner].name, c->names, A[c->winner].name);
            if (c->nfiles && !c->same) ov_text("\"%s\" overrides \"%s\" (%d file(s))", A[c->winner].title, A[c->loser].title, c->nfiles);
            if (c->mixed) ov_text_warn("\"%s\" and \"%s\" mix parts of one asset: may crash, switch one off.", A[c->loser].title, A[c->winner].title);
        }
    }
    folder_section();
}
