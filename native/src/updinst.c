// updinst: see updinst.h. Windows only; the agent (updater.c) and the launcher (native/launcher/redirect.c) link it.
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "updinst.h"
#include "b4bcoop_version.h"

static void (*logfn)(const char *line);
static char ua_product[48] = "b4bcoop-updater";
void updi_set_log(void (*fn)(const char *line)) { logfn = fn; }
void updi_set_user_agent(const char *product) { snprintf(ua_product, sizeof ua_product, "%s", product); }
static void ilog(const char *fmt, ...) {
    if (!logfn) return;
    char b[600];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    logfn(b);
}

// ---- paths ----
wchar_t updi_root[MAX_PATH], updi_dir[MAX_PATH];
void updi_set_root(const wchar_t *root) {
    _snwprintf(updi_root, MAX_PATH, L"%ls", root); updi_root[MAX_PATH - 1] = 0;
    _snwprintf(updi_dir, MAX_PATH, L"%lsb4bcoop-update\\", updi_root); updi_dir[MAX_PATH - 1] = 0;
}
void updi_join(wchar_t *out, const wchar_t *dir, const char *rel) {
    wchar_t w[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, rel, -1, w, MAX_PATH)) w[0] = 0;
    for (wchar_t *c = w; *c; c++) if (*c == L'/') *c = L'\\';
    _snwprintf(out, MAX_PATH * 2, L"%ls%ls", dir, w);
    out[MAX_PATH * 2 - 1] = 0;
}
void updi_mkdirs_for(const wchar_t *file) {
    wchar_t p[MAX_PATH * 2];
    wcsncpy(p, file, MAX_PATH * 2 - 1); p[MAX_PATH * 2 - 1] = 0;
    for (wchar_t *c = p + 3; *c; c++)
        if (*c == L'\\') { *c = 0; CreateDirectoryW(p, NULL); *c = L'\\'; }
}
int updi_exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
void updi_rmtree(const wchar_t *dir) {
    if (!updi_dir[0] || _wcsnicmp(dir, updi_dir, wcslen(updi_dir)) || wcsstr(dir, L"..")) return;
    wchar_t pat[MAX_PATH * 2]; WIN32_FIND_DATAW fd;
    _snwprintf(pat, MAX_PATH * 2, L"%ls\\*", dir);
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t p[MAX_PATH * 2];
            _snwprintf(p, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) updi_rmtree(p);
            else { SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL); DeleteFileW(p); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}
int updi_write_file(const wchar_t *p, const void *data, size_t n) {
    updi_mkdirs_for(p);
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0;
    BOOL ok = WriteFile(h, data, (DWORD)n, &w, NULL) && w == n;
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    return ok;
}
int updi_move(const wchar_t *from, const wchar_t *to) {
    updi_mkdirs_for(to);
    return MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
int updi_file_has(const wchar_t *p, const char *needle) {
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD n = GetFileSize(h, NULL), got = 0;
    char *b = n && n < (4u << 20) ? malloc(n) : NULL;
    int r = 0;
    if (b && ReadFile(h, b, n, &got, NULL) && got == n) {
        size_t k = strlen(needle);
        for (DWORD i = 0; i + k <= n && !r; i++) r = b[i] == needle[0] && !memcmp(b + i, needle, k);
    }
    free(b);
    CloseHandle(h);
    return r;
}

// ---- state.txt ----
void updi_state_read(UpdState *s) {
    memset(s, 0, sizeof *s);
    if (!updi_dir[0]) return;
    wchar_t p[MAX_PATH * 2]; _snwprintf(p, MAX_PATH * 2, L"%lsstate.txt", updi_dir);
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[2048]; DWORD n = 0;
    ReadFile(h, buf, sizeof buf - 1, &n, NULL);
    CloseHandle(h);
    buf[n] = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char *v = strchr(line, '=');
        if (!v) continue;
        *v++ = 0;
        if (!strcmp(line, "installed")) snprintf(s->installed, sizeof s->installed, "%s", v);
        else if (!strcmp(line, "previous")) snprintf(s->previous, sizeof s->previous, "%s", v);
        else if (!strcmp(line, "files")) snprintf(s->files, sizeof s->files, "%s", v);
        else if (!strcmp(line, "result")) snprintf(s->result, sizeof s->result, "%s", v);
        else if (!strcmp(line, "skip")) snprintf(s->skip, sizeof s->skip, "%s", v);
        else if (!strcmp(line, "pending")) s->pending = atoi(v);
        else if (!strcmp(line, "starts")) s->starts = atoi(v);
    }
    if (!upd_version_valid(s->installed)) s->installed[0] = 0;
    if (!upd_version_valid(s->previous)) s->previous[0] = 0;
    if (!upd_version_valid(s->skip)) s->skip[0] = 0;
}
int updi_state_write(const UpdState *s) {
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "; b4bcoop in-game updater (docs: b4bcoop-COMMANDS.txt, \"Updates\"). Safe to delete.\r\n"
                     "installed=%s\r\nprevious=%s\r\npending=%d\r\nstarts=%d\r\nfiles=%s\r\nresult=%s\r\nskip=%s\r\n",
                     s->installed, s->previous, s->pending, s->starts, s->files, s->result, s->skip);
    wchar_t p[MAX_PATH * 2], t[MAX_PATH * 2];
    _snwprintf(p, MAX_PATH * 2, L"%lsstate.txt", updi_dir);
    _snwprintf(t, MAX_PATH * 2, L"%lsstate.txt.tmp", updi_dir);
    return updi_write_file(t, buf, n) && MoveFileExW(t, p, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

int updi_revert(UpdState *s, const char *why, char *err, size_t en) {
    if (!s->previous[0]) { snprintf(err, en, "no previous version kept"); return 0; }
    wchar_t bdir[MAX_PATH], fdir[MAX_PATH];
    wchar_t wp[40], wi[40];
    MultiByteToWideChar(CP_UTF8, 0, s->previous, -1, wp, 40);
    MultiByteToWideChar(CP_UTF8, 0, s->installed[0] ? s->installed : "unknown", -1, wi, 40);
    _snwprintf(bdir, MAX_PATH, L"%lsbackup\\%ls\\", updi_dir, wp);
    _snwprintf(fdir, MAX_PATH, L"%lsfailed\\%ls\\", updi_dir, wi);
    if (!updi_exists(bdir)) { snprintf(err, en, "the backup of %s is missing (b4bcoop-update\\backup)", s->previous); return 0; }
    int bad = 0;
    char files[600], *save;
    snprintf(files, sizeof files, "%s", s->files);
    for (char *rel = strtok_r(files, "|", &save); rel; rel = strtok_r(NULL, "|", &save)) {
        if (!upd_install_path(rel)) continue;
        wchar_t live[MAX_PATH * 2], to[MAX_PATH * 2];
        updi_join(live, updi_root, rel); updi_join(to, fdir, rel);
        if (updi_exists(live) && !updi_move(live, to)) { ilog("revert: can't move %s aside (%lu)", rel, GetLastError()); bad++; }
    }
    // what the install moved to the backup goes back (the same list: only files that existed then are there)
    snprintf(files, sizeof files, "%s", s->files);
    for (char *rel = strtok_r(files, "|", &save); rel; rel = strtok_r(NULL, "|", &save)) {
        wchar_t from[MAX_PATH * 2], live[MAX_PATH * 2];
        updi_join(from, bdir, rel); updi_join(live, updi_root, rel);
        if (upd_install_path(rel) && updi_exists(from) && !updi_move(from, live)) { ilog("revert: can't restore %s (%lu)", rel, GetLastError()); bad++; }
    }
    ilog("reverted %s -> %s (%s)%s", s->installed, s->previous, why, bad ? ", with errors" : "");
    UpdState n; memset(&n, 0, sizeof n);
    snprintf(n.installed, sizeof n.installed, "%s", s->previous);
    snprintf(n.result, sizeof n.result, "%s", why);
    snprintf(n.skip, sizeof n.skip, "%s", s->installed);   // the launcher doesn't install it again by itself
    *s = n;
    updi_state_write(s);
    if (bad) { snprintf(err, en, "%d file(s) could not be moved back; see the log", bad); return 0; }
    return 1;
}

// ---- HTTP (WinHTTP, loaded on the first request) ----
#define WH(f) static __typeof__(f) *p##f
WH(WinHttpOpen); WH(WinHttpConnect); WH(WinHttpOpenRequest); WH(WinHttpSendRequest); WH(WinHttpReceiveResponse);
WH(WinHttpQueryHeaders); WH(WinHttpReadData); WH(WinHttpCloseHandle); WH(WinHttpSetTimeouts); WH(WinHttpCrackUrl);
static int winhttp_load(char *err, size_t en) {
    static volatile LONG ok;
    if (ok) return 1;
    HMODULE m = LoadLibraryW(L"winhttp.dll");   // in the game: netguard hooks WinHttpConnect as it maps
    if (!m) { snprintf(err, en, "winhttp.dll not available (%lu)", GetLastError()); return 0; }
#define LD(f) if (!(p##f = (__typeof__(f) *)(void *)GetProcAddress(m, #f))) { snprintf(err, en, "winhttp.dll has no " #f); return 0; }
    LD(WinHttpOpen); LD(WinHttpConnect); LD(WinHttpOpenRequest); LD(WinHttpSendRequest); LD(WinHttpReceiveResponse);
    LD(WinHttpQueryHeaders); LD(WinHttpReadData); LD(WinHttpCloseHandle); LD(WinHttpSetTimeouts); LD(WinHttpCrackUrl);
    InterlockedExchange(&ok, 1);
    return 1;
}

const UpdiTimeouts UPDI_TIMEOUTS_NORMAL = {15000, 15000, 20000, 30000};

int updi_http_fetch(const char *url, const char *accept, uint64_t max, UpdiSink sink, void *ctx, volatile LONG64 *progress,
                    const UpdiTimeouts *t, volatile LONG *cancel, char *err, size_t en) {
    if (!winhttp_load(err, en)) return 0;
    if (!t) t = &UPDI_TIMEOUTS_NORMAL;
    wchar_t wurl[1024], host[256], path[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024)) { snprintf(err, en, "bad URL"); return 0; }
    URL_COMPONENTS u; memset(&u, 0, sizeof u);
    u.dwStructSize = sizeof u;
    u.lpszHostName = host; u.dwHostNameLength = 256; u.lpszUrlPath = path; u.dwUrlPathLength = 1024;
    wchar_t extra[512]; u.lpszExtraInfo = extra; u.dwExtraInfoLength = 512;
    if (!pWinHttpCrackUrl(wurl, 0, 0, &u)) { snprintf(err, en, "bad URL %s", url); return 0; }
    int https = u.nScheme == INTERNET_SCHEME_HTTPS;
    if (!https) {
#ifdef B4B_RELEASE
        snprintf(err, en, "refusing a non-HTTPS URL"); return 0;
#else
        if (_wcsicmp(host, L"127.0.0.1") && _wcsicmp(host, L"localhost")) { snprintf(err, en, "plain HTTP only to 127.0.0.1 (dev test server)"); return 0; }
#endif
    }
    wcsncat(path, extra, 1023 - wcslen(path));
    wchar_t ua[80]; _snwprintf(ua, 80, L"%hs/%hs", ua_product, B4B_VERSION); ua[79] = 0;
    HINTERNET s = pWinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = NULL, r = NULL;
    int ok = 0;
    uint8_t *buf = NULL;
    uint64_t n = 0;
    if (!s) { snprintf(err, en, "WinHttpOpen failed (%lu)", GetLastError()); goto done; }
    pWinHttpSetTimeouts(s, t->resolve, t->connect, t->send, t->receive);
    c = pWinHttpConnect(s, host, u.nPort, 0);
    if (!c) { DWORD e = GetLastError(); snprintf(err, en, e == 12007 ? "can't reach %ls (no internet, or blocked)" : "can't connect to %ls (%lu)", host, e); goto done; }
    r = pWinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    wchar_t hdr[256]; _snwprintf(hdr, 256, L"Accept: %hs\r\nX-GitHub-Api-Version: 2022-11-28\r\n", accept);
    if (!r || !pWinHttpSendRequest(r, hdr, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !pWinHttpReceiveResponse(r, NULL)) {
        DWORD e = GetLastError();
        const char *what = e == 12002 ? "timed out" : e == 12007 ? "name not resolved (no internet, or blocked)" :
                           e == 12029 || e == 12030 ? "connection failed" : (e >= 12037 && e <= 12045) || e == 12175 ? "secure connection (TLS/certificate) failed" : "failed";
        snprintf(err, en, "request to %ls %s (%lu)", host, what, e);
        goto done;
    }
    DWORD status = 0, sl = sizeof status;
    pWinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        wchar_t rem[32] = L"", reset[32] = L""; DWORD l1 = sizeof rem, l2 = sizeof reset;
        pWinHttpQueryHeaders(r, WINHTTP_QUERY_CUSTOM, L"x-ratelimit-remaining", rem, &l1, WINHTTP_NO_HEADER_INDEX);
        pWinHttpQueryHeaders(r, WINHTTP_QUERY_CUSTOM, L"x-ratelimit-reset", reset, &l2, WINHTTP_NO_HEADER_INDEX);
        if ((status == 403 || status == 429) && !wcscmp(rem, L"0")) {
            time_t tt = (time_t)_wtoi64(reset);
            struct tm *lt = localtime(&tt);
            if (lt) snprintf(err, en, "GitHub's limit for update checks from your internet address is used up (60 per hour); try again after %02d:%02d", lt->tm_hour, lt->tm_min);
            else snprintf(err, en, "GitHub's limit for update checks from your internet address is used up (60 per hour); try again later");
        } else if (status == 404) snprintf(err, en, "not found on GitHub (404)");
        else snprintf(err, en, "GitHub answered HTTP %lu", status);
        goto done;
    }
    buf = malloc(65536);
    if (!buf) { snprintf(err, en, "out of memory"); goto done; }
    for (;;) {
        if (cancel && *cancel) { snprintf(err, en, "cancelled"); goto done; }
        DWORD got = 0;
        if (!pWinHttpReadData(r, buf, 65536, &got)) { snprintf(err, en, "download interrupted (%lu)", GetLastError()); goto done; }
        if (!got) break;
        n += got;
        if (progress) InterlockedExchange64(progress, (LONG64)n);
        if (n > max) { snprintf(err, en, "download larger than expected (over %llu bytes)", (unsigned long long)max); goto done; }
        if (!sink(buf, got, ctx, err, en)) goto done;
    }
    if (cancel && *cancel) { snprintf(err, en, "cancelled"); goto done; }
    ok = 1;
done:
    free(buf);
    if (r) pWinHttpCloseHandle(r);
    if (c) pWinHttpCloseHandle(c);
    if (s) pWinHttpCloseHandle(s);
    return ok;
}

typedef struct { uint8_t *p; size_t n, cap; } MemSink;
static int mem_sink(const uint8_t *p, size_t n, void *ctx, char *err, size_t en) {
    MemSink *m = ctx;
    if (m->n + n + 1 > m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 131072;
        while (nc < m->n + n + 1) nc *= 2;
        uint8_t *nb = realloc(m->p, nc);
        if (!nb) { snprintf(err, en, "out of memory"); return 0; }
        m->p = nb; m->cap = nc;
    }
    memcpy(m->p + m->n, p, n);
    m->n += n;
    return 1;
}
int updi_http_get(const char *url, const char *accept, size_t max, uint8_t **out, size_t *outn, volatile LONG64 *progress,
                  const UpdiTimeouts *t, volatile LONG *cancel, char *err, size_t en) {
    MemSink m = {0};
    *out = NULL; *outn = 0;
    if (!updi_http_fetch(url, accept, max, mem_sink, &m, progress, t, cancel, err, en) || !mem_sink((const uint8_t *)"", 0, &m, err, en)) {
        free(m.p); return 0;
    }
    m.p[m.n] = 0;
    *out = m.p; *outn = m.n;
    return 1;
}

// ---- check, download, install ----
void updi_release_url(char *url, size_t n, const char *api, const char *repo, const char *ver) {
    if (ver && upd_version_valid(ver)) snprintf(url, n, "%s/repos/%s/releases/tags/v%s", api, repo, ver);
    else snprintf(url, n, "%s/repos/%s/releases/latest", api, repo);
}
int updi_fetch_release(const char *url, UpdRelease *rel, const UpdiTimeouts *t, volatile LONG *cancel, char *err, size_t en) {
    uint8_t *json = NULL; size_t jn;
    if (!updi_http_get(url, "application/vnd.github+json", 2u << 20, &json, &jn, NULL, t, cancel, err, en)) return 0;
    int ok = upd_release_parse((char *)json, jn, rel, err, en);
    free(json);
    return ok;
}
int updi_fetch_manifest(const UpdRelease *rel, const uint8_t pub[32], UpdiTarget *out, const UpdiTimeouts *t,
                        volatile LONG *cancel, char *err, size_t en) {
    memset(out, 0, sizeof *out);
    const UpdAsset *am = upd_release_asset(rel, UPDI_MANIFEST_NAME), *as = upd_release_asset(rel, UPDI_MANIFEST_NAME ".sig");
    if (!am || !as) { snprintf(err, en, "release %s has no in-game update files (it is older than the updater)", rel->tag); return 2; }
    uint8_t *man = NULL, *sig = NULL; size_t mn, sn;
    int r = 0;
    if (!updi_http_get(am->url, "application/octet-stream", 16384, &man, &mn, NULL, t, cancel, err, en) ||
        !updi_http_get(as->url, "application/octet-stream", 1024, &sig, &sn, NULL, t, cancel, err, en)) goto out;
    r = -1;
    if (!upd_manifest_verify((char *)man, mn, sig, sn, pub, &out->m, err, en)) goto out;
    if (strcmp(upd_tag_version(rel->tag), out->m.version)) {
        snprintf(err, en, "release %s carries update files for %s", rel->tag, out->m.version); goto out;
    }
    const UpdAsset *az = upd_release_asset(rel, out->m.zip);
    char zsig[120]; snprintf(zsig, sizeof zsig, "%s.sig", out->m.zip);
    const UpdAsset *azs = upd_release_asset(rel, zsig);
    if (!az || !azs) { snprintf(err, en, "release %s is missing %s or its signature", rel->tag, out->m.zip); goto out; }
    snprintf(out->zip_url, sizeof out->zip_url, "%s", az->url);
    snprintf(out->sig_url, sizeof out->sig_url, "%s", azs->url);
    r = 1;
out:
    free(man); free(sig);
    return r;
}
int updi_download(const UpdiTarget *tg, const uint8_t pub[32], volatile LONG64 *progress, volatile LONG *cancel,
                  uint8_t **zip, size_t *zn, char *err, size_t en) {
    uint8_t *sig = NULL; size_t sn = 0;
    *zip = NULL; *zn = 0;
    if (progress) InterlockedExchange64(progress, 0);
    if (!updi_http_get(tg->zip_url, "application/octet-stream", (size_t)tg->m.size, zip, zn, progress, NULL, cancel, err, en) ||
        !updi_http_get(tg->sig_url, "application/octet-stream", 1024, &sig, &sn, NULL, NULL, cancel, err, en)) {
        free(*zip); *zip = NULL; return 0;
    }
    int ok = upd_zip_verify(*zip, *zn, sig, sn, &tg->m, pub, err, en);
    free(sig);
    if (!ok) { free(*zip); *zip = NULL; return -1; }
    return 1;
}

// Staging: each installable zip entry -> staged\<ver>\<rel>; the list of rels in files
typedef struct { wchar_t dir[MAX_PATH]; char files[600]; int n, agent; char err[200]; } Stage;
static int stage_cb(const char *name, const uint8_t *data, size_t n, void *ctx) {
    Stage *s = ctx;
    if (!upd_install_path(name)) { ilog("zip: skipping %s (never replaced by the updater)", name); return 0; }
    wchar_t p[MAX_PATH * 2];
    updi_join(p, s->dir, name);
    if (!updi_write_file(p, data, n)) { snprintf(s->err, sizeof s->err, "can't write %s (%lu)", name, GetLastError()); return 1; }
    if (strlen(s->files) + strlen(name) + 2 >= sizeof s->files) { snprintf(s->err, sizeof s->err, "too many files"); return 1; }
    if (s->files[0]) strcat(s->files, "|");
    strcat(s->files, name);
    s->n++;
    if (!strcmp(name, "Gobi/Binaries/Win64/X3DAudio1_7.dll")) s->agent = 1;
    return 0;
}

// staged files -> live, running files -> backup\<cur>\ (renames). 1 = done.
static int swap_in(const Stage *sg, const char *newver, const char *cur, UpdState *out, char *err, size_t en) {
    wchar_t bdir[MAX_PATH], wv[40];
    MultiByteToWideChar(CP_UTF8, 0, cur, -1, wv, 40);
    wchar_t b[MAX_PATH];
    _snwprintf(b, MAX_PATH, L"%lsbackup", updi_dir); updi_rmtree(b);    // only the newest backup is kept
    _snwprintf(b, MAX_PATH, L"%lsfailed", updi_dir); updi_rmtree(b);
    _snwprintf(bdir, MAX_PATH, L"%lsbackup\\%ls\\", updi_dir, wv);
    char list[600], *save; snprintf(list, sizeof list, "%s", sg->files);
    char *rels[16]; int n = 0;
    for (char *r = strtok_r(list, "|", &save); r && n < 16; r = strtok_r(NULL, "|", &save)) rels[n++] = r;
    int old_moved[16] = {0}, new_moved[16] = {0}, i;
    for (i = 0; i < n; i++) {
        wchar_t live[MAX_PATH * 2], bk[MAX_PATH * 2];
        updi_join(live, updi_root, rels[i]); updi_join(bk, bdir, rels[i]);
        if (!updi_exists(live)) continue;
        if (!updi_move(live, bk)) { snprintf(err, en, "can't move %s to the backup (Windows error %lu)", rels[i], GetLastError()); goto undo; }
        old_moved[i] = 1;
    }
    for (i = 0; i < n; i++) {
        wchar_t live[MAX_PATH * 2], from[MAX_PATH * 2];
        updi_join(live, updi_root, rels[i]); updi_join(from, sg->dir, rels[i]);
        if (!updi_move(from, live)) { snprintf(err, en, "can't put the new %s in place (Windows error %lu)", rels[i], GetLastError()); goto undo; }
        new_moved[i] = 1;
    }
    UpdState s; memset(&s, 0, sizeof s);
    snprintf(s.installed, sizeof s.installed, "%s", newver);
    snprintf(s.previous, sizeof s.previous, "%s", cur);
    snprintf(s.files, sizeof s.files, "%s", sg->files);
    snprintf(s.result, sizeof s.result, "installed %s over %s", newver, cur);
    s.pending = 1;
    if (!updi_state_write(&s)) ilog("warning: can't write state.txt (%lu); the start check won't run", GetLastError());
    *out = s;
    return 1;
undo:
    ilog("%s; undoing", err);
    for (i = n - 1; i >= 0; i--) {
        wchar_t live[MAX_PATH * 2], p[MAX_PATH * 2];
        updi_join(live, updi_root, rels[i]);
        if (new_moved[i]) { updi_join(p, sg->dir, rels[i]); updi_move(live, p); }
        if (old_moved[i]) { updi_join(p, bdir, rels[i]); if (!updi_move(p, live)) ilog("undo: can't restore %s (%lu)", rels[i], GetLastError()); }
    }
    return 0;
}

int updi_install(const uint8_t *zip, size_t zn, const UpdManifest *m, const char *cur, UpdState *out, char *err, size_t en) {
    if (!updi_dir[0]) { snprintf(err, en, "no game folder"); return 0; }
    Stage sg; memset(&sg, 0, sizeof sg);
    wchar_t wv[40]; MultiByteToWideChar(CP_UTF8, 0, m->version, -1, wv, 40);
    _snwprintf(sg.dir, MAX_PATH, L"%lsstaged\\%ls\\", updi_dir, wv);
    wchar_t sd[MAX_PATH]; _snwprintf(sd, MAX_PATH, L"%lsstaged", updi_dir); updi_rmtree(sd);
    CreateDirectoryW(updi_dir, NULL);
    char zerr[300] = "";
    int r = upd_zip_each(zip, zn, stage_cb, &sg, zerr, sizeof zerr);
    if (r < 0 || r > 0 || !sg.agent) {
        updi_rmtree(sd);
        snprintf(err, en, "%s", r < 0 ? zerr : r > 0 ? sg.err : "the zip has no X3DAudio1_7.dll");
        return 0;
    }
    ilog("staged %d files: %s", sg.n, sg.files);
    int ok = swap_in(&sg, m->version, cur, out, err, en);
    updi_rmtree(sd);
    return ok;
}
