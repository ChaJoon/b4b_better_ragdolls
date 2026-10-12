// shop: Get more in the `~` window's "Looks & add-ons" tab (looks.c; was the Browse tab, #36,
// docs/investigations/shop.md), a "Workshop" for publicly licensed add-ons published in a public GitHub repo; also the
// Update / Remove / Undo of Installed rows (Remove: any add-on in the folder). Nothing is fetched, downloaded or
// changed without the player's click, and nothing about what a player has leaves the machine (GitHub only sees the
// downloads themselves).
//   Get the list  catalog.json + catalog.json.sig (ed25519, the shop key from signkeys.h; updcore.c parses and checks it),
//                 then the thumbnails it names (each checked against the SHA-256 in the signed list before
//                 imgdecode.c looks at it).
//   Add           the pak is streamed to <add-ons>\.shop\<id>.pak.part, its size and SHA-256 must equal the signed
//                 list, then it becomes <add-ons>\<id>.pak and addons.c takes it (addons_add_runtime): appended to
//                 addonlist.txt and mounted right away when that is safe (addons.c addons_load_now: cosmetic, new
//                 files or replaced assets not in memory, as a client within the host's policy), else at the next start.
//   Remove        (any add-on, Installed) switched off in addonlist.txt and noted in .shop\pending.txt; the next start
//                 deletes the file before anything is mounted (shop_early, from addons_scan in DllMain). A mounted pak
//                 is never unmounted.
//   Update        (a newer file in the list) downloaded to .shop\<id>.pak.new and swapped in by the next start; loaded
//                 at once from there (above the old one) when nothing of the old version is in memory
//                 (addons_update_runtime).
// netguard lets GitHub's hosts through only while a request runs (the updater's scope). ini shop=0 hides Get more and
// Installed's Update (Remove stays).
// Dev builds only: shop_catalog=<url> (e.g. http://127.0.0.1:<port>/catalog.json), shop_pubkey=<64 hex> (a test key),
// dev command `shop status|fetch|add <id>|remove <id or file>|undo <id or file>|list`.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include "log.h"
#include "cmds.h"
#include "netguard.h"
#include "overlay.h"
#include "updcore.h"
#include "imgdecode.h"
#include "signkeys.h"
#include "b4bcoop_version.h"

// The catalog lives in the shop repo's default branch (a workflow rebuilds and signs it when an add-on release is
// published; shop.md §2). raw.githubusercontent.com is inside netguard's updater scope. It is verified with the shop
// key only (signkeys.h), never the release key.
#define DEFAULT_CATALOG "https://raw.githubusercontent.com/actuallydan/back4blood-shop/main/catalog.json"
#define THUMB_SIDE 96        // thumbnails are scaled down to fit 96x96 (36 KB each on the GPU)
#define MAX_THUMBS 128       // per session (ov_texture has OV_MAX_TEX)
#define SHOP_DIR L".shop"
static const uint8_t SHOP_PUBKEY[32] = {B4B_SHOP_PUBKEY_BYTES};

int updater_http_get(const char *url, size_t max, uint8_t **out, size_t *n, char *err, size_t en);
int updater_http_to_file(const char *url, const wchar_t *path, uint64_t max, uint8_t sha256[32], uint64_t *got,
                         volatile LONG64 *progress, char *err, size_t en);

static int enabled = 1;
static char cat_url[400] = DEFAULT_CATALOG;
static uint8_t pubkey[32];
static int test_key;

// ---- next start: pending removals/updates (DllMain, kernel32 only) ----
#define MAX_PEND 128
#define FILE_MAX 64
static char forget[MAX_PEND][FILE_MAX];   // removed at this start: addons_scan drops their addonlist.txt lines
static int nforget;
// A plain .pak file name in the add-ons folder (Remove works for any add-on, not only the shop's)
static int file_ok(const char *f) {
    size_t l = strlen(f);
    if (l < 5 || l >= FILE_MAX || _stricmp(f + l - 4, ".pak") || f[0] == '.' || f[0] == ' ') return 0;
    for (const char *c = f; *c; c++) if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7e || strchr("\\/:*?\"<>|", *c)) return 0;
    return 1;
}
static int shop_file_ok(const char *f) {   // "<shop id>.pak" (updates)
    size_t l = strlen(f);
    char id[40];
    if (l < 5 || l > 36 || _stricmp(f + l - 4, ".pak")) return 0;
    snprintf(id, sizeof id, "%.*s", (int)(l - 4), f);
    return upd_shop_id_ok(id);
}
void shop_early(const wchar_t *dir) {
    wchar_t p[MAX_PATH * 2];
    _snwprintf(p, MAX_PATH * 2, L"%ls\\" SHOP_DIR L"\\pending.txt", dir);
    p[MAX_PATH * 2 - 1] = 0;
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[16384]; DWORD n = 0;
    ReadFile(h, buf, sizeof buf - 1, &n, NULL);
    CloseHandle(h);
    buf[n] = 0;
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char op[16], *f = line + strcspn(line, " ");   // "<op> <file name>" (the name may have spaces)
        snprintf(op, sizeof op, "%.*s", (int)(f - line), line);
        while (*f == ' ') f++;
        size_t fl = strlen(f);
        while (fl && f[fl - 1] == ' ') f[--fl] = 0;
        if (!file_ok(f) || (!strcmp(op, "update") && !shop_file_ok(f))) continue;
        wchar_t wf[FILE_MAX], live[MAX_PATH * 2], nw[MAX_PATH * 2];
        MultiByteToWideChar(CP_UTF8, 0, f, -1, wf, FILE_MAX);
        _snwprintf(live, MAX_PATH * 2, L"%ls\\%ls", dir, wf);
        _snwprintf(nw, MAX_PATH * 2, L"%ls\\" SHOP_DIR L"\\%ls.new", dir, wf);
        if (!strcmp(op, "remove")) {
            BOOL ok = DeleteFileW(live) || GetLastError() == ERROR_FILE_NOT_FOUND;
            DeleteFileW(nw);
            LOG("shop: removed %s (~ window, last session)%s", f, ok ? "" : ": delete FAILED, it stays switched off");
            if (ok && nforget < MAX_PEND) snprintf(forget[nforget++], sizeof forget[0], "%s", f);
        } else if (!strcmp(op, "update")) {
            BOOL ok = MoveFileExW(nw, live, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            LOG("shop: updated %s (~ window, last session)%s", f, ok ? "" : ": swap FAILED, the old file stays");
        }
    }
    DeleteFileW(p);
}
int shop_forget(const char *file) {
    for (int i = 0; i < nforget; i++) if (!_stricmp(forget[i], file)) return 1;
    return 0;
}

// ---- state (worker threads + game thread) ----
enum { C_IDLE, C_FETCHING, C_READY, C_ERROR };
enum { D_NONE, D_DOWNLOADING, D_VERIFIED, D_DONE, D_ERROR };
typedef struct {
    int dl;                         // D_*
    char msg[200];
    char local_sha[65];             // SHA-256 of the file in the folder (worker, after the list arrived), "" unknown
    int staged;                     // D_VERIFIED: 1 = <id>.pak in place (Add), 2 = .shop\<id>.pak.new (update)
    int like_checked;               // like/like_why computed (game thread; reset when the folder's add-ons change)
    char like[64], like_why[48];    // the same add-on already in the folder under another file name (e.g. batman.pak)
} ItemRt;
// This session's Remove (any add-on) and Update (shop add-ons) clicks, by file name (game thread), written to
// .shop\pending.txt for the next start. live: the update is loaded already (addons_update_runtime): no Undo of it,
// no second update before a restart.
typedef struct { char file[FILE_MAX]; int op, live; } Pend;   // op 0 none, 1 remove, 2 update
static Pend pend[MAX_PEND];
static int npend;
static Pend *pend_of(const char *file, int create) {
    for (int i = 0; i < npend; i++) if (!_stricmp(pend[i].file, file)) return &pend[i];
    if (!create || npend == MAX_PEND) return NULL;
    Pend *p = &pend[npend++];
    memset(p, 0, sizeof *p);
    snprintf(p->file, sizeof p->file, "%s", file);
    return p;
}
static int pend_op(const char *file) { Pend *p = pend_of(file, 0); return p ? p->op : 0; }
static int pend_live(const char *file) { Pend *p = pend_of(file, 0); return p && p->live; }
static CRITICAL_SECTION cs;
static int cs_ready;
static ShopCatalog cat;             // game thread only; a fetch hands its result over in next_cat (shop_tick swaps)
static ItemRt *rt;
static ShopCatalog next_cat;
static ItemRt *next_rt;
static int have_next;
static int phase = C_IDLE;
static char msg[300];
static HANDLE worker;
static volatile LONG64 dl_got;
static int dl_item = -1;            // the running download
static char retry_id[40];           // a verified download whose loading waits for the game's loader (addons_* 3)
static ULONGLONG retry_until;
typedef struct { uint8_t sha[32]; int tex; uint8_t *rgba; int w, h; } Thumb;   // by thumbnail SHA-256, whole session
static Thumb thumbs[MAX_THUMBS];
static int nthumbs;

static void setmsg(int ph, const char *fmt, ...) {
    char m[300];
    va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    EnterCriticalSection(&cs);
    if (ph >= 0) phase = ph;
    snprintf(msg, sizeof msg, "%s", m);
    LeaveCriticalSection(&cs);
    LOG("shop: %s", m);
}
static int busy(void) { return worker && WaitForSingleObject(worker, 0) == WAIT_TIMEOUT; }
static void start(LPTHREAD_START_ROUTINE fn, void *arg) {
    if (worker) { CloseHandle(worker); worker = NULL; }
    worker = CreateThread(NULL, 0, fn, arg, 0, NULL);
}
static void scope_host(const char *url, char *h, size_t n) {   // dev: the catalog's host, for netguard's scope
    h[0] = 0;
    const char *p = strstr(url, "://");
    if (!p) return;
    p += 3;
    size_t k = strcspn(p, ":/");
    snprintf(h, n, "%.*s", (int)(k < n ? k : n - 1), p);
}
static void shop_path(wchar_t *out, const char *name) {   // <add-ons>\.shop\<name> ("" = the folder)
    wchar_t w[80];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 80);
    _snwprintf(out, MAX_PATH * 2, L"%ls\\" SHOP_DIR L"%ls%ls", addons_dir_w(), name[0] ? L"\\" : L"", w);
    out[MAX_PATH * 2 - 1] = 0;
}
static void live_path(wchar_t *out, const char *id) {
    _snwprintf(out, MAX_PATH * 2, L"%ls\\%hs.pak", addons_dir_w(), id);
    out[MAX_PATH * 2 - 1] = 0;
}
static int ensure_dirs(void) {
    wchar_t p[MAX_PATH * 2];
    CreateDirectoryW(addons_dir_w(), NULL);
    shop_path(p, "");
    CreateDirectoryW(p, NULL);
    SetFileAttributesW(p, FILE_ATTRIBUTE_HIDDEN);
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static int file_sha(const wchar_t *path, char hex[65]) {   // SHA-256 of a file (worker threads)
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint8_t *b = malloc(1 << 20), d[32];
    UpdSha s; upd_sha256_init(&s);
    DWORD got;
    int ok = b != NULL;
    while (ok && ReadFile(h, b, 1 << 20, &got, NULL) && got) upd_sha256_update(&s, b, got);
    CloseHandle(h);
    free(b);
    if (!ok) return 0;
    upd_sha256_final(&s, d);
    upd_hex(d, 32, hex);
    return 1;
}

// .shop\pending.txt: "remove <file>" / "update <file>", applied by shop_early at the next start
static int pending_write(void) {
    char buf[16384]; int n = snprintf(buf, sizeof buf, "; b4bcoop ~ window (Looks & add-ons): applied at the next game start. Safe to delete (cancels them).\r\n");
    for (int i = 0; i < npend && n < (int)sizeof buf - 96; i++)
        if (pend[i].op) n += snprintf(buf + n, sizeof buf - n, "%s %s\r\n", pend[i].op == 1 ? "remove" : "update", pend[i].file);
    wchar_t p[MAX_PATH * 2], t[MAX_PATH * 2];
    shop_path(p, "pending.txt"); shop_path(t, "pending.txt.tmp");
    if (!ensure_dirs()) return 0;
    HANDLE h = CreateFileW(t, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0;
    BOOL ok = WriteFile(h, buf, (DWORD)n, &w, NULL) && w == (DWORD)n;
    CloseHandle(h);
    return ok && MoveFileExW(t, p, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
static const char *file_of(int i) {   // "<id>.pak" of catalog entry i (a few calls may share a line)
    static char b[4][48]; static int k;
    char *s = b[k++ & 3];
    snprintf(s, 48, "%s.pak", cat.items[i].id);
    return s;
}
static int cat_index(const char *file);

// ---- Get the list (worker) ----
static Thumb *thumb_of(const uint8_t sha[32]) {
    for (int i = 0; i < nthumbs; i++) if (!memcmp(thumbs[i].sha, sha, 32)) return &thumbs[i];
    return NULL;
}
static DWORD WINAPI fetch_worker(void *arg) {
    (void)arg;
    char url[420], sigurl[420], err[300], extra[112];
    EnterCriticalSection(&cs); snprintf(url, sizeof url, "%s", cat_url); LeaveCriticalSection(&cs);
    snprintf(sigurl, sizeof sigurl, "%s.sig", url);
    scope_host(url, extra, sizeof extra);
    netguard_updater_scope(1, extra);
    uint8_t *json = NULL, *sig = NULL; size_t jn = 0, sn = 0;
    ShopCatalog c; memset(&c, 0, sizeof c);
    ItemRt *r = NULL;
    LOG("shop: getting the add-on list %s", url);
    if (!updater_http_get(url, SHOP_MAX_CATALOG, &json, &jn, err, sizeof err) || !updater_http_get(sigurl, 1024, &sig, &sn, err, sizeof err)) {
        setmsg(C_ERROR, "Could not get the add-on list: %s. Details: the shop: and netguard: lines of the b4bcoop log.", err); goto out;
    }
    if (!upd_catalog_verify((char *)json, jn, sig, sn, pubkey, &c, err, sizeof err)) { setmsg(C_ERROR, "Not shown: %s.", err); goto out; }
    r = calloc(c.n ? c.n : 1, sizeof *r);
    if (!r) { upd_catalog_free(&c); setmsg(C_ERROR, "Out of memory."); goto out; }
    EnterCriticalSection(&cs);
    next_cat = c; next_rt = r; have_next = 1;   // the game thread takes it over (shop_tick); r stays valid after that
    LeaveCriticalSection(&cs);
    if (!c.n) setmsg(C_READY, "Reached the shop, but its list is empty (updated %s): no add-ons are published there yet%s.",
                     c.updated[0] ? c.updated : "?", c.n_bad ? " (or none usable by this b4bcoop)" : "");
    else setmsg(C_READY, "%d add-on(s) in the list (updated %s)%s.", c.n, c.updated[0] ? c.updated : "?", c.n_bad ? "; some entries were not usable" : "");
    if (c.n_bad) LOG("shop: %d catalog entr%s skipped (bad fields)", c.n_bad, c.n_bad == 1 ? "y" : "ies");
    // thumbnails: only bytes whose SHA-256 the signed list names are decoded
    for (int i = 0; i < c.n; i++) {
        ShopItem *it = &c.items[i];
        if (!it->thumb[0]) continue;
        EnterCriticalSection(&cs);
        Thumb *t = thumb_of(it->thumb_sha256);
        int full = nthumbs >= MAX_THUMBS;
        LeaveCriticalSection(&cs);
        if (t || full) continue;
        uint8_t *img = NULL, d[32]; size_t in = 0;
        if (!updater_http_get(it->thumb, SHOP_MAX_THUMB, &img, &in, err, sizeof err)) { LOG("shop: thumbnail of %s: %s", it->id, err); continue; }
        upd_sha256(img, in, d);
        int w = 0, h = 0;
        uint8_t *rgba = memcmp(d, it->thumb_sha256, 32) ? NULL : img_thumb(img, in, THUMB_SIDE, &w, &h);
        if (memcmp(d, it->thumb_sha256, 32)) LOG("shop: thumbnail of %s does not match the list's SHA-256: not shown", it->id);
        else if (!rgba) LOG("shop: thumbnail of %s is not a PNG/JPEG picture", it->id);
        free(img);
        if (!rgba) continue;
        EnterCriticalSection(&cs);
        if (nthumbs < MAX_THUMBS && !thumb_of(it->thumb_sha256)) {
            Thumb *n = &thumbs[nthumbs];
            memcpy(n->sha, it->thumb_sha256, 32); n->rgba = rgba; n->w = w; n->h = h; n->tex = 0;
            nthumbs++;
            rgba = NULL;
        }
        LeaveCriticalSection(&cs);
        free(rgba);
    }
    netguard_updater_scope(0, NULL);
    // files already in the folder: their SHA-256 tells "installed" from "newer version in the list"
    for (int i = 0; i < c.n; i++) {
        wchar_t p[MAX_PATH * 2]; char hex[65] = "";
        live_path(p, c.items[i].id);
        if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES || !file_sha(p, hex)) continue;
        EnterCriticalSection(&cs);
        snprintf(r[i].local_sha, sizeof r[i].local_sha, "%s", hex);
        LeaveCriticalSection(&cs);
    }
    free(json); free(sig);
    return 0;
out:
    netguard_updater_scope(0, NULL);
    free(json); free(sig);
    return 0;
}

// ---- Add / Update (worker): download, verify, put in place; the game thread finishes (shop_tick) ----
static void item_msg(int i, int dl, const char *fmt, ...) {
    char m[200];
    va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    EnterCriticalSection(&cs);
    rt[i].dl = dl;
    snprintf(rt[i].msg, sizeof rt[i].msg, "%s", m);
    LeaveCriticalSection(&cs);
    LOG("shop: %s: %s", cat.items[i].id, m);
}
static DWORD WINAPI download_worker(void *arg) {
    int i = (int)(intptr_t)arg;
    ShopItem it = cat.items[i];
    char err[300], extra[112], part[48], hex[65];
    wchar_t pp[MAX_PATH * 2], live[MAX_PATH * 2], nw[MAX_PATH * 2];
    snprintf(part, sizeof part, "%s.pak.part", it.id);
    shop_path(pp, part);
    live_path(live, it.id);
    char nn[48]; snprintf(nn, sizeof nn, "%s.pak.new", it.id);
    shop_path(nw, nn);
    if (!ensure_dirs()) { item_msg(i, D_ERROR, "Can't create the add-ons folder %s.", addons_dir8()); return 0; }
    int update = GetFileAttributesW(live) != INVALID_FILE_ATTRIBUTES;
    scope_host(it.url, extra, sizeof extra);
    netguard_updater_scope(1, extra);
    InterlockedExchange64(&dl_got, 0);
    uint8_t sha[32]; uint64_t got = 0;
    LOG("shop: downloading %s (%llu bytes) from %s", it.id, (unsigned long long)it.size, it.url);
    int ok = updater_http_to_file(it.url, pp, it.size, sha, &got, &dl_got, err, sizeof err);
    netguard_updater_scope(0, NULL);
    if (!ok) { DeleteFileW(pp); item_msg(i, D_ERROR, "Download failed: %s. Nothing was changed.", err); return 0; }
    if (got != it.size) { DeleteFileW(pp); item_msg(i, D_ERROR, "Download incomplete (%llu of %llu bytes). Nothing was changed.", (unsigned long long)got, (unsigned long long)it.size); return 0; }
    if (memcmp(sha, it.sha256, 32)) { DeleteFileW(pp); item_msg(i, D_ERROR, "Download damaged (its SHA-256 does not match the signed list). Nothing was changed."); return 0; }
    upd_hex(sha, 32, hex);
    LOG("shop: %s verified (size, SHA-256 %s)", it.id, hex);
    int staged;
    if (update) {   // the old file may be mounted: the swap waits for the next start
        if (!MoveFileExW(pp, nw, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(pp); item_msg(i, D_ERROR, "Can't store the update (Windows error %lu).", GetLastError()); return 0; }
        staged = 2;
    } else {
        if (!MoveFileExW(pp, live, MOVEFILE_WRITE_THROUGH)) { DeleteFileW(pp); item_msg(i, D_ERROR, "Can't put it into the add-ons folder (Windows error %lu).", GetLastError()); return 0; }
        staged = 1;
    }
    EnterCriticalSection(&cs);
    rt[i].staged = staged;
    if (staged == 1) snprintf(rt[i].local_sha, sizeof rt[i].local_sha, "%s", hex);
    LeaveCriticalSection(&cs);
    item_msg(i, D_VERIFIED, "Verified.");
    return 0;
}

// game thread: a verified download becomes an add-on (or the newer file of one): loaded now when addons.c finds that
// safe, else at the next start. A busy loader (only matters for files that replace existing ones) is asked again
// every frame for up to 15 s.
static int find(const char *id);
static void finish(int i, int last) {
    char file[48], m[320];
    snprintf(file, sizeof file, "%s.pak", cat.items[i].id);
    int again = !strcmp(retry_id, cat.items[i].id);
    if (rt[i].staged == 2) {
        if (!again) {
            Pend *p = pend_of(file, 1);
            if (p) p->op = 2;
            if (!p || !pending_write()) { item_msg(i, D_ERROR, "Downloaded, but .shop\\pending.txt could not be written."); return; }
        }
        wchar_t nw[MAX_PATH * 2]; char nn[48];
        snprintf(nn, sizeof nn, "%s.pak.new", cat.items[i].id);
        shop_path(nw, nn);
        int r = addons_update_runtime(file, nw, last, m, sizeof m);
        if (r == 3) { snprintf(retry_id, sizeof retry_id, "%s", cat.items[i].id); return; }
        retry_id[0] = 0;
        if (r == 2) {
            Pend *p = pend_of(file, 1);
            if (p) p->live = 1;
            item_msg(i, D_DONE, "Updated and ready now.");
            overlay_note("[add-ons] update ready");
        } else {
            item_msg(i, D_DONE, "Updated file downloaded: it replaces the old one when you restart the game (not now: %s).", m);
            overlay_note("[add-ons] update downloaded; restart the game to use it");
        }
        return;
    }
    int r = again ? addons_load_now(file, last, m, sizeof m) : addons_add_runtime(file, m, sizeof m);
    if (r == 3) { snprintf(retry_id, sizeof retry_id, "%s", cat.items[i].id); return; }
    retry_id[0] = 0;
    for (int k = 0; k < cat.n; k++) rt[k].like_checked = 0;   // a new add-on in the folder: "installed as" may change
    if (r == 2) {
        item_msg(i, D_DONE, "Added and ready now.");
        overlay_note("[add-ons] add-on added and ready");
    } else if (r == 1) item_msg(i, D_DONE, "Added: %s.", m);
    else item_msg(i, D_ERROR, "Downloaded, but not added: %s.", m);
}
void shop_tick(float dt) {
    (void)dt;
    if (!cs_ready) return;
    if (have_next) {   // a fetched list replaces the shown one (only here: the panel reads cat without a lock)
        EnterCriticalSection(&cs);
        ShopCatalog old = cat; ItemRt *oldrt = rt;
        cat = next_cat; rt = next_rt; have_next = 0;
        memset(&next_cat, 0, sizeof next_cat); next_rt = NULL;
        LeaveCriticalSection(&cs);
        upd_catalog_free(&old); free(oldrt);   // this session's Remove/Update clicks stay in pend[] (by file)
    }
    if (retry_id[0]) {
        int i = find(retry_id);
        if (i < 0) retry_id[0] = 0;
        else { finish(i, GetTickCount64() > retry_until); return; }
    }
    if (dl_item < 0 || busy()) return;
    int i = dl_item;
    EnterCriticalSection(&cs);
    int st = rt && i < cat.n ? rt[i].dl : D_NONE;
    LeaveCriticalSection(&cs);
    if (st == D_DOWNLOADING) return;
    dl_item = -1;
    if (st != D_VERIFIED) return;
    retry_until = GetTickCount64() + 15000;
    finish(i, 0);
}

// ---- actions (game thread) ----
static int find(const char *id) {
    for (int i = 0; i < cat.n; i++) if (!_stricmp(cat.items[i].id, id)) return i;
    return -1;
}
static int cat_index(const char *file) {   // the catalog entry whose file is `file` ("<id>.pak"), -1 none
    size_t l = strlen(file);
    char id[48];
    if (l < 5 || l >= sizeof id || _stricmp(file + l - 4, ".pak")) return -1;
    snprintf(id, sizeof id, "%.*s", (int)(l - 4), file);
    return find(id);
}
static int installed(int i) {   // the file is in the add-ons folder
    wchar_t p[MAX_PATH * 2];
    live_path(p, cat.items[i].id);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}
// The same add-on already in the folder under another file name (a player's own batman.pak for the shop's
// batman-any): same content id, or it adds an outfit / weapon look of the same name. NULL = none.
static const char *like_of(int i, const char **why) {
    if (!rt[i].like_checked) {
        char w[48];
        const char *f = addons_installed_like(file_of(i), cat.items[i].content_id, cat.items[i].adds, w, sizeof w);
        snprintf(rt[i].like, sizeof rt[i].like, "%s", f ? f : "");
        snprintf(rt[i].like_why, sizeof rt[i].like_why, "%s", f ? w : "");
        rt[i].like_checked = 1;
    }
    if (why) *why = rt[i].like_why;
    return rt[i].like[0] ? rt[i].like : NULL;
}
static const char *too_old(const ShopItem *it) {
    static char b[80];
    if (!it->min_version[0] || upd_version_cmp(B4B_VERSION, it->min_version) >= 0) return NULL;
    snprintf(b, sizeof b, "needs b4bcoop %s or newer (Updates tab)", it->min_version);
    return b;
}
static const char *fetch(void) {
    if (!enabled) return "the add-on shop is off (shop=0)";
    if (!addons_enabled()) return "add-ons are off (addons=0)";
    if (busy()) return "busy";
    setmsg(C_FETCHING, "Getting the add-on list...");
    start(fetch_worker, NULL);
    return NULL;
}
static const char *add(int i) {
    if (i < 0 || i >= cat.n) return "no such add-on in the list";
    if (!addons_enabled()) return "add-ons are off (addons=0)";
    if (busy()) return "busy";
    const char *old = too_old(&cat.items[i]);
    if (old) return old;
    if (pend_live(file_of(i))) return "its update is loaded; restart the game before the next update";
    if (pend_op(file_of(i))) return "it has a change waiting for the next start";
    int upd = installed(i);
    static char as[120];
    const char *like = upd ? NULL : like_of(i, NULL);
    if (like) { snprintf(as, sizeof as, "already installed as %s", like); return as; }
    if (upd && !rt[i].local_sha[0]) return "already in your add-ons folder";
    if (upd) {
        char hex[65]; upd_hex(cat.items[i].sha256, 32, hex);
        if (!strcmp(hex, rt[i].local_sha)) return "you have this version";
    }
    EnterCriticalSection(&cs);
    rt[i].dl = D_DOWNLOADING; rt[i].staged = 0;
    snprintf(rt[i].msg, sizeof rt[i].msg, "Downloading...");
    LeaveCriticalSection(&cs);
    dl_item = i;
    start(download_worker, (void *)(intptr_t)i);
    return NULL;
}
// Remove (any add-on in the folder, Installed): switched off now, its file deleted at the next start (shop_early).
static const char *remove_file(const char *file) {
    AddonState st;
    if (!addons_state(file, &st)) return "not in your add-ons folder";
    if (!file_ok(file)) return "its file name has characters this can't handle: delete the file from the add-ons folder";
    int i = cat_index(file);
    if (i >= 0 && busy() && dl_item == i) return "busy";
    Pend *p = pend_of(file, 1);
    if (!p) return "too many changes wait for the next start already";
    int was = p->op;
    wchar_t nw[MAX_PATH * 2]; char nn[FILE_MAX + 8];
    snprintf(nn, sizeof nn, "%s.new", file);
    shop_path(nw, nn);
    if (was == 2 && !p->live) DeleteFileW(nw);   // a downloaded update goes with it (a loaded one: the next start deletes both)
    p->op = 1;
    if (!pending_write()) { p->op = was; return "could not write .shop\\pending.txt"; }
    addons_set_on(file, 0);
    LOG("shop: remove %s at the next start", file);
    if (i >= 0) item_msg(i, D_DONE, "Removed when you restart the game (switched off until then).");
    return NULL;
}
static const char *undo_file(const char *file) {
    Pend *p = pend_of(file, 0);
    if (!p || !p->op) return "nothing to undo";
    int was = p->op;
    if (p->live && was == 2) return "the update is already loaded";
    p->op = p->live ? 2 : 0;   // undoing a removal after a loaded update: the update stays
    if (!pending_write()) { p->op = was; return "could not write .shop\\pending.txt"; }
    if (was == 1) addons_set_on(file, 1);
    else { wchar_t nw[MAX_PATH * 2]; char nn[FILE_MAX + 8]; snprintf(nn, sizeof nn, "%s.new", file); shop_path(nw, nn); DeleteFileW(nw); }
    int i = cat_index(file);
    if (i >= 0) item_msg(i, D_NONE, "");
    LOG("shop: undo %s of %s", was == 1 ? "remove" : "update", file);
    return NULL;
}
static void note_err(const char *e) { if (e) overlay_note(e); }

// ---- ~ window, "Looks & add-ons" tab (looks.c): Get more, and the shop's part of Installed rows ----
static int has(const char *hay, const char *needle) {
    if (!*needle) return 1;
    char h[1600], n[80];
    snprintf(h, sizeof h, "%s", hay); snprintf(n, sizeof n, "%s", needle);
    for (char *c = h; *c; c++) *c = (char)tolower((unsigned char)*c);
    for (char *c = n; *c; c++) *c = (char)tolower((unsigned char)*c);
    return strstr(h, n) != NULL;
}
static void size_str(uint64_t b, char *out, size_t n) {
    if (b >= 1048576) snprintf(out, n, "%.1f MB", b / 1048576.0);
    else snprintf(out, n, "%llu KB", (unsigned long long)(b + 1023) / 1024);
}
// what the row says and offers: 0 Add, 1 installed, 2 removal pending (Undo), 3 update pending (Undo),
// 4 newer version (Update), 5 downloading/busy, 6 installed under another file name (*why = that file)
static int row_state(int i, const char **why) {
    *why = NULL;
    if (rt[i].dl == D_DOWNLOADING) return 5;
    int op = pend_op(file_of(i));
    if (op == 1) return 2;
    if (op == 2) return 3;
    if (!installed(i)) return (*why = like_of(i, NULL)) ? 6 : 0;
    char hex[65]; upd_hex(cat.items[i].sha256, 32, hex);
    if (rt[i].local_sha[0] && strcmp(hex, rt[i].local_sha)) return 4;
    return 1;
}
int shop_enabled(void) { return enabled; }
// pictures decoded by the worker go to the GPU here (game thread, overlay lock inside ov_texture)
static void upload_thumbs(void) {
    EnterCriticalSection(&cs);
    for (int t = 0; t < nthumbs; t++)
        if (thumbs[t].rgba && !thumbs[t].tex) {
            thumbs[t].tex = ov_texture(thumbs[t].rgba, thumbs[t].w, thumbs[t].h);
            free(thumbs[t].rgba); thumbs[t].rgba = NULL;
            if (!thumbs[t].tex) thumbs[t].tex = -1;
        }
    LeaveCriticalSection(&cs);
}
static void update_button(int i, int running) {
    const char *old = too_old(&cat.items[i]);
    char lab[64];
    ov_begin_disabled(running || old, running ? "busy" : old);
    snprintf(lab, sizeof lab, "Update##%s", cat.items[i].id);
    if (ov_button(lab)) note_err(add(i));
    ov_tooltip("A newer file is in the shop's list: downloaded now, ready at once when nothing of the old version is "
               "in use, else after a restart.");
    ov_end_disabled();
}
// A Remove / Update waiting for the next start: what (the row's text column) and Undo (its button column)
static void pending_text(const char *file) {
    int op = pend_op(file);
    if (!op) return;
    if (op == 2 && pend_live(file)) {
        ov_text_dim("updated (the new version is loaded)");
        ov_tooltip("Its file replaces the old one in the add-ons folder when the game next starts.");
    } else ov_text_warn(op == 1 ? "removed when the game restarts" : "updated when the game restarts");
}
static void undo_button(const char *file, const char *undo_id) {
    char lab[96];
    if (!pend_op(file) || (pend_op(file) == 2 && pend_live(file))) return;
    snprintf(lab, sizeof lab, "Undo##%s", undo_id);
    if (ov_button(lab)) note_err(undo_file(file));
    ov_tooltip(pend_op(file) == 1 ? "Keep it (switched back on)." : "Keep the current version (the downloaded one is deleted).");
}
static void item_note(int i) {   // the entry's last message (download, verify, loaded now or why not)
    char im[200]; int idl;
    EnterCriticalSection(&cs); snprintf(im, sizeof im, "%s", rt[i].msg); idl = rt[i].dl; LeaveCriticalSection(&cs);
    if (!im[0] || idl == D_DOWNLOADING) return;
    if (idl == D_ERROR) ov_text_warn("%s", im); else ov_text_dim("%s", im);
}
static void details(int i) {
    ShopItem *it = &cat.items[i];
    char hex[65], sz[32];
    ov_text("%s%s%s", it->name, it->version[0] ? "  v" : "", it->version);
    if (it->author[0]) ov_text_dim("by %s", it->author);
    if (it->desc[0]) ov_text_dim("%s", it->desc);
    ov_text("License: %s", it->license);
    if (it->license_url[0]) { ov_same_line(); if (ov_button("Copy the license link")) ov_copy(it->license_url); }
    ov_text("Content: %s%s%s%s", it->cls, it->kinds[0] ? " (" : "", it->kinds, it->kinds[0] ? ")" : "");
    if (it->adds[0]) ov_text_dim("Adds: %s. New looks are ready right after Add: Wear / Use on the row, or in Wear (above).", it->adds);
    if (it->replaces[0]) ov_text_dim("Replaces: %s. A replacement works right after Add unless the game is using what it replaces (then after a restart).", it->replaces);
    if (!strcmp(it->cls, "gameplay")) ov_text_warn("Gameplay-affecting: hosts with addons_policy=cosmetic, none or match (without the same add-on) refuse players who run it.");
    size_str(it->size, sz, sizeof sz);
    upd_hex(it->sha256, 32, hex);
    ov_text_dim("File %s.pak, %s, SHA-256 %.16s...", it->id, sz, hex);
    if (it->min_version[0]) ov_text_dim("Needs b4bcoop %s or newer.", it->min_version);
    if (ov_button("Copy the download link")) ov_copy(it->url);
}

// Get more: the shop's list. filter: 0 all, 1 new looks, 2 replacements, 3 not installed yet.
void shop_get_panel(const char *search, int filter) {
    static int sel = -1;
    EnterCriticalSection(&cs);
    int ph = phase; char m[300]; snprintf(m, sizeof m, "%s", msg);
    LeaveCriticalSection(&cs);
    int running = busy();
    static const char *INTRO = "Free add-ons with a public license (github.com/actuallydan/back4blood-shop). Nothing is "
                               "downloaded until you click, nobody else learns what you add, and every file is checked "
                               "against the signed list.";
    if (!cat.n) ov_text_dim("%s", INTRO);
    if (test_key) ov_text_warn("Dev build: shop_pubkey= test key, list %s", cat_url);
    if (!addons_enabled()) { ov_text_warn("Add-ons are off (addons=0 in b4bcoop.ini): Installed (above) turns them on."); return; }
    ov_begin_disabled(running, "busy");
    if (ov_button(cat.n || ph == C_READY ? "Refresh the list" : "Get the add-on list")) note_err(fetch());
    ov_end_disabled();
    if (cat.n) ov_tooltip(INTRO);
    if (ph == C_FETCHING || ph == C_ERROR || ph == C_READY) { ov_same_line(); if (ph == C_ERROR) ov_text_warn("%s", m); else ov_text_dim("%s", m); }
    if (!cat.n) return;
    upload_thumbs();
    if (sel >= cat.n) sel = -1;
    int open = ov_table_begin("shop", 3), shown = 0;
    if (open) { static const char *H[] = {"", "Add-on", ""}; ov_table_header(H, 3); }
    for (int i = 0; i < cat.n && open; i++) {
        ShopItem *it = &cat.items[i];
        char hay[1600], lab[160];
        snprintf(hay, sizeof hay, "%s|%s|%s|%s|%s|%s|%s|%s", it->name, it->author, it->license, it->desc, it->adds, it->replaces, it->kinds, it->id);
        if (!has(hay, search)) continue;
        const char *why;
        int rs = row_state(i, &why);
        if (filter == 1 && (!it->adds[0] || it->replaces[0])) continue;
        if (filter == 2 && !it->replaces[0]) continue;
        if (filter == 3 && rs != 0 && rs != 5) continue;
        shown++;
        ov_push_id(i);
        ov_table_next();
        EnterCriticalSection(&cs);
        Thumb *t = it->thumb[0] ? thumb_of(it->thumb_sha256) : NULL;
        int tex = t && t->tex > 0 ? t->tex : 0, pw = t ? t->w : 0, phh = t ? t->h : 0;
        LeaveCriticalSection(&cs);
        float tw = 3.5f, th = 3.5f;
        if (tex && pw && phh) { if (pw >= phh) th = 3.5f * phh / pw; else tw = 3.5f * pw / phh; }
        ov_image(tex, tw, th);
        ov_table_next();
        snprintf(lab, sizeof lab, "%s%s%s##sel:%s", it->name, it->version[0] ? "  v" : "", it->version, it->id);
        if (ov_selectable(lab, sel == i)) sel = sel == i ? -1 : i;
        ov_tooltip("Click for the details (license, description, file).");
        char sz[32]; size_str(it->size, sz, sizeof sz);
        ov_text_dim("%s%s%slicense %s, %s, %s", it->author[0] ? "by " : "", it->author, it->author[0] ? ", " : "", it->license, it->cls, sz);
        if (it->adds[0]) ov_text_dim("Adds: %s", it->adds);
        if (it->replaces[0]) ov_text_dim("Replaces: %s", it->replaces);
        if (rs == 6) {   // already in the folder under another file name: said here (the last column is narrow)
            const char *lw; like_of(i, &lw);
            ov_text("Installed (as %s)", why);
            ov_tooltip("Already in your add-ons folder under another file name, so it isn't added twice. Installed "
                       "(above) switches it on/off.");
            if (!strcmp(lw, "same content")) ov_text_dim("the same add-on");
            else ov_text_dim("%s adds the same %s", why, lw);
        }
        if (rs == 2 || rs == 3) pending_text(file_of(i));
        if (rs != 5) item_note(i);
        ov_table_next();
        const char *old = too_old(it);
        if (rs == 5) {
            LONG64 g = dl_got;
            ov_text_dim("%lld / %llu KB", (long long)g / 1024, (unsigned long long)it->size / 1024);
        } else if (rs == 0) {
            ov_begin_disabled(running || old, running ? "busy" : old);
            snprintf(lab, sizeof lab, "Add##%s", it->id);
            if (ov_button(lab)) note_err(add(i));
            ov_end_disabled();
        } else if (rs == 6) {
            AddonState as;   // that add-on's state, short like an installed row's ("on", "off", ...)
            ov_text("%s", addons_state(why, &as) ? as.state : "in the folder");
            addons_look_buttons(why);
        } else if (rs == 2 || rs == 3) {
            undo_button(file_of(i), it->id);
            if (rs == 3) addons_look_buttons(file_of(i));
        } else {
            AddonState as;
            if (addons_state(file_of(i), &as)) ov_text("%s", as.state); else ov_text("in the folder");
            addons_look_buttons(file_of(i));   // ready now: wear / use it right here
            if (rs == 4) update_button(i, running);
        }
        ov_pop_id();
        if (sel == i) {   // the details open under the row: this table ends, a new one (same columns) follows
            ov_table_end();
            details(i);
            ov_separator();
            open = ov_table_begin("shop", 3);
        }
    }
    if (open) ov_table_end();
    if (!shown) ov_text_dim("Nothing in the list matches%s.", filter ? " (with this filter)" : "");
}

// Installed: this add-on's shop actions in its row (any add-on: the pending Remove + Undo; shop add-ons: Update when
// the list has a newer file, the download's progress).
void shop_installed_actions(const char *file) {
    int i = enabled ? cat_index(file) : -1;
    if (i >= 0 && rt[i].dl == D_DOWNLOADING) {
        LONG64 g = dl_got;
        ov_text_dim("%lld / %llu KB", (long long)g / 1024, (unsigned long long)cat.items[i].size / 1024);
        return;
    }
    if (pend_op(file)) undo_button(file, file);
    else if (i >= 0) {
        const char *why;
        if (row_state(i, &why) == 4) update_button(i, busy());
    }
}
void shop_item_note(const char *file) {   // Installed: a waiting Remove/Update and the shop's last message (under the title)
    pending_text(file);
    int i = enabled ? cat_index(file) : -1;
    if (i >= 0 && rt[i].dl != D_DOWNLOADING) item_note(i);
}
int shop_pending(const char *file) { return pend_op(file); }
const char *shop_remove(const char *file) { return remove_file(file); }
// Installed, above the list: get the shop's list (as Get more does) so that shop add-ons show Update.
void shop_check_button(void) {
    if (!enabled || !addons_enabled()) return;
    EnterCriticalSection(&cs);
    int ph = phase; char m[300]; snprintf(m, sizeof m, "%s", msg);
    LeaveCriticalSection(&cs);
    ov_begin_disabled(busy(), "busy");
    if (ov_button("Check for updates##shop")) note_err(fetch());
    ov_tooltip("Gets the add-on shop's list (as Get more does): your add-ons from the shop show Update when it has a newer file.");
    ov_end_disabled();
    if (ph == C_FETCHING || ph == C_ERROR) { ov_same_line(); if (ph == C_ERROR) ov_text_warn("%s", m); else ov_text_dim("%s", m); return; }
    if (ph != C_READY) return;
    int n = 0;
    const char *why;
    for (int i = 0; i < cat.n; i++) n += row_state(i, &why) == 4;
    ov_same_line();
    ov_text_dim(n ? "%d update(s) in the shop's list." : "No updates in the shop's list.", n);
}
// ---- init / ini ----
static void ini_pair(const char *k, const char *v, void *ctx) {
    (void)ctx;
    if (!strcmp(k, "shop")) enabled = atoi(v) != 0;
#ifndef B4B_RELEASE
    else if (!strcmp(k, "shop_catalog") && *v) snprintf(cat_url, sizeof cat_url, "%s", v);
    else if (!strcmp(k, "shop_pubkey")) { if (upd_hex_decode(v, pubkey, 32)) test_key = 1; else LOG("shop: bad shop_pubkey (64 hex digits)"); }
#endif
}
int shop_live(const char *k, const char *v) {
    if (!strcmp(k, "shop")) {
        enabled = !v || atoi(v) != 0;   // the ~ window's Get more (looks.c) reads it every frame
        return 1;
    }
#ifndef B4B_RELEASE
    if (!strcmp(k, "shop_catalog") || !strcmp(k, "shop_pubkey")) {
        EnterCriticalSection(&cs);
        if (v) ini_pair(k, v, NULL);
        else if (!strcmp(k, "shop_catalog")) snprintf(cat_url, sizeof cat_url, "%s", DEFAULT_CATALOG);
        else { memcpy(pubkey, SHOP_PUBKEY, 32); test_key = 0; }
        LeaveCriticalSection(&cs);
        LOG("shop: list %s, key %s", cat_url, test_key ? "test" : "shop");
        return 1;
    }
#endif
    return 0;
}
void shop_init(void) {
    InitializeCriticalSection(&cs); cs_ready = 1;
    memcpy(pubkey, SHOP_PUBKEY, 32);
    cmds_ini_each(ini_pair, NULL);
    LOG("shop: %s; list %s%s", enabled ? "on (~ window, Get more)" : "off (shop=0)", cat_url, test_key ? " (test key)" : "");
}

#ifndef B4B_RELEASE
int shop_cmd(const char *verb, char *rest, Out *o) {
    if (strcmp(verb, "shop")) return 0;
    char *a = rest ? strtok(rest, " ") : NULL, *b = a ? strtok(NULL, " ") : NULL;
    const char *e = NULL;
    if (a && !strcmp(a, "fetch")) e = fetch();
    else if (a && !strcmp(a, "add") && b) e = add(find(b));
    else if (a && (!strcmp(a, "remove") || !strcmp(a, "undo")) && b) {   // a catalog id or any add-on's file name
        char f[FILE_MAX + 8];
        int i = find(b);
        snprintf(f, sizeof f, "%s", i >= 0 ? file_of(i) : b);
        e = !strcmp(a, "remove") ? remove_file(f) : undo_file(f);
    }
    else if (a && strcmp(a, "status") && strcmp(a, "list")) { out_printf(o, "usage: shop [status|list|fetch|add <id>|remove <id>|undo <id>]\n"); return 1; }
    if (e) out_printf(o, "error: %s\n", e);
    static const char *P[] = {"idle", "fetching", "ready", "error"}, *D[] = {"-", "downloading", "verified", "done", "error"};
    EnterCriticalSection(&cs);
    out_printf(o, "shop: phase=%s busy=%d items=%d thumbs=%d msg=%s\nlist=%s key=%s\n", P[phase], busy(), cat.n, nthumbs, msg, cat_url, test_key ? "test" : "shop");
    for (int i = 0; i < cat.n; i++) {
        const char *why;
        int rs = row_state(i, &why);
        Thumb *t = cat.items[i].thumb[0] ? thumb_of(cat.items[i].thumb_sha256) : NULL;
        static const char *R[] = {"add", "installed", "remove-pending", "update-pending", "update-available", "downloading", "installed-as"};
        const char *lw = "";
        if (rs == 6) like_of(i, &lw);
        out_printf(o, "  %s [%s] dl=%s pending=%d thumb=%s local=%.8s msg=%s%s%s%s%s\n", cat.items[i].id, R[rs], D[rt[i].dl], pend_op(file_of(i)),
                   !t ? "-" : t->tex > 0 ? "uploaded" : t->rgba ? "decoded" : "failed", rt[i].local_sha, rt[i].msg,
                   rs == 6 ? "installed as " : "", rs == 6 ? why : "", rs == 6 ? ": " : "", lw);
    }
    for (int i = 0; i < npend; i++) if (pend[i].op) out_printf(o, "pending: %s %s%s\n", pend[i].op == 1 ? "remove" : "update", pend[i].file, pend[i].live ? " (loaded)" : "");
    LeaveCriticalSection(&cs);
    return 1;
}
#endif
