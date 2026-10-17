// updater: in-game updates from the ~ window's Updates tab (#34, docs/investigations/updater.md). In the game nothing
// is fetched or changed without the player's click, and nothing is hot-reloaded. (The launcher, the root
// xinput1_3.dll, installs a newer release before the game starts, with this same code in updinst.c: ini auto_update,
// its switch is in this tab; native/launcher/redirect.c.)
//   Check     GET <api>/repos/<repo>/releases/latest (or /releases/tags/v<ver>: the version of a host that refused
//             us), then that release's b4bcoop-update.txt + .sig: the manifest (version, protocol, zip name, size,
//             SHA-256) signed with the b4bcoop release key (ed25519, public key in signkeys.h, updcore.c checks it).
//   Download  "Download and install on next start": the zip + its .sig into memory; size, SHA-256 and signature are
//             checked before anything is written. Its files (upd_install_path: the two DLLs and b4bcoop-*.txt; never
//             b4bcoop.ini, bans, add-ons or logs) are staged in <game>\b4bcoop-update\staged\<ver>\ and swapped in
//             by renames: each running file moves to backup\<running ver>\ (Windows can rename a loaded DLL, not
//             overwrite it), the new one takes its place. This game keeps running the old code; the new version
//             loads at the next start.
//   Start     b4bcoop-update\state.txt: the new version counts its own starts from DllMain (updater_early); 15 s of
//             game ticks mark it healthy. A third start without that (it crashed twice) moves the backup back
//             (revert) and runs nothing this time; the next start is the previous version again. "Go back to <prev>"
//             in the tab does the same revert by hand.
// netguard lets GitHub's release hosts through only while a check or download runs (netguard_updater_scope).
// ini: updates=0 hides the tab; auto_update (read by the launcher; shown and saved here). Dev builds only:
// update_api=<base URL> (e.g. a local test server), update_repo=, update_pubkey=<64 hex> (a test key; the launcher
// reads all three too), dev command `update status|check [ver]|install|goback`.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "log.h"
#include "cmds.h"
#include "netguard.h"
#include "overlay.h"
#include "updcore.h"
#include "updinst.h"
#include "signkeys.h"
#include "b4bcoop_version.h"

// The b4bcoop release key (signkeys.h; verifies releases only, the shop has its own key).
static const uint8_t RELEASE_PUBKEY[32] = {B4B_RELEASE_PUBKEY_BYTES};
#define DEFAULT_API UPDI_DEFAULT_API
#define DEFAULT_REPO UPDI_DEFAULT_REPO
#define HEALTHY_SECONDS 15.f
#define MAX_STARTS 2          // starts of a fresh update that may end without reaching HEALTHY_SECONDS

static int enabled = 1;       // ini updates=0: no Updates tab
#ifdef B4B_RELEASE
#define AUTO_DEFAULT 1        // ini auto_update: the launcher's automatic update (redirect.c has the same default)
#else
#define AUTO_DEFAULT 0        // dev builds: off unless auto_update=1, so a release never replaces a developer's build
#endif
static int auto_on = AUTO_DEFAULT;
static int launcher_auto;     // the root xinput1_3.dll is a launcher that updates (feature "auto-update-1")
static char auto_note[400];   // b4bcoop-update\auto.txt: the launcher's last automatic update
static int never_healthy;     // dev: update_never_healthy=1 (tests the automatic revert)
static char api[200] = DEFAULT_API, repo[100] = DEFAULT_REPO;
static uint8_t pubkey[32];
static int test_key;          // dev: update_pubkey= in use

// ---- paths (updinst.c: updi_root, updi_dir) ----
static wchar_t module_path[MAX_PATH];
static const char *unavailable;      // why this install can't be updated in game (NULL = it can)

static void ulog(const char *line) { LOG("update: %s", line); }
static void paths_init(HMODULE self) {
    static int done;
    if (done) return;
    done = 1;
    updi_set_log(ulog);
    if (!GetModuleFileNameW(self, module_path, MAX_PATH)) { unavailable = "can't find the mod's own file"; return; }
    wchar_t *base = wcsrchr(module_path, L'\\');
    if (!base || _wcsicmp(base + 1, L"X3DAudio1_7.dll")) { unavailable = "this is a developer install (not X3DAudio1_7.dll)"; return; }
    static const wchar_t tail[] = L"\\Gobi\\Binaries\\Win64";
    size_t dl = base - module_path, tl = wcslen(tail);
    if (dl <= tl || _wcsnicmp(module_path + dl - tl, tail, tl)) { unavailable = "the mod is not in Gobi\\Binaries\\Win64 of the game folder"; return; }
    wchar_t root[MAX_PATH];
    wcsncpy(root, module_path, dl - tl + 1);
    root[dl - tl + 1] = 0;
    updi_set_root(root);
}

// ---- state.txt (updinst.c) ----
typedef UpdState State;
static State st;
static CRITICAL_SECTION st_cs;
static int st_cs_ready;
#define state_read updi_state_read
#define state_write updi_state_write
#define revert updi_revert

// DllMain, right after the log opened (loader lock: kernel32 file calls only). 1 = the fresh update failed to
// start twice and was reverted: run nothing this time.
int updater_early(void *module) {
    paths_init((HMODULE)module);
    if (unavailable) return 0;
    State s; state_read(&s);
    if (!s.pending) return 0;
    if (strcmp(s.installed, B4B_VERSION)) {   // files replaced by hand since (a zip extracted over it)
        LOG("update: state says %s was installed, this is %s: forgetting the pending update", s.installed, B4B_VERSION);
        s.pending = 0; s.starts = 0;
        state_write(&s);
        return 0;
    }
    s.starts++;
    if (s.starts > MAX_STARTS) {
        char why[200], err[200];
        snprintf(why, sizeof why, "%s did not start properly %d times, so %s was put back", B4B_VERSION, MAX_STARTS, s.previous);
        if (revert(&s, why, err, sizeof err)) {
            LOG("update: %s; running nothing this time, restart the game", why);
            return 1;
        }
        LOG("update: revert failed: %s", err);
        return 0;
    }
    LOG("update: start %d of the freshly installed %s (healthy after %.0f s of play)", s.starts, B4B_VERSION, HEALTHY_SECONDS);
    state_write(&s);
    return 0;
}

// ---- HTTP (updinst.c) ----
static volatile LONG64 dl_got;   // bytes of the running download (progress)

// For the add-on shop (shop.c): the same HTTPS path (the caller opens netguard's updater scope around it).
int updater_http_get(const char *url, size_t max, uint8_t **out, size_t *n, char *err, size_t en) {
    return updi_http_get(url, "application/octet-stream", max, out, n, &dl_got, NULL, NULL, err, en);
}
typedef struct { HANDLE h; UpdSha sha; } FileSink;
static int file_sink(const uint8_t *p, size_t n, void *ctx, char *err, size_t en) {
    FileSink *f = ctx;
    DWORD w = 0;
    upd_sha256_update(&f->sha, p, n);
    if (!WriteFile(f->h, p, (DWORD)n, &w, NULL) || w != n) { snprintf(err, en, "can't write the file (Windows error %lu; disk full?)", GetLastError()); return 0; }
    return 1;
}
// GET url into a new file (created or truncated), SHA-256 on the way. 1 = the whole body written; *got = its size.
int updater_http_to_file(const char *url, const wchar_t *path, uint64_t max, uint8_t sha256[32], uint64_t *got,
                         volatile LONG64 *progress, char *err, size_t en) {
    FileSink f;
    *got = 0;
    upd_sha256_init(&f.sha);
    f.h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f.h == INVALID_HANDLE_VALUE) { snprintf(err, en, "can't create the file (Windows error %lu)", GetLastError()); return 0; }
    int ok = updi_http_fetch(url, "application/octet-stream", max, file_sink, &f, progress, NULL, NULL, err, en);
    ok = FlushFileBuffers(f.h) && ok;
    CloseHandle(f.h);
    upd_sha256_final(&f.sha, sha256);
    *got = f.sha.n;
    return ok;
}

// ---- the update state machine (worker thread + game-thread UI) ----
enum { P_IDLE, P_CHECKING, P_CHECKED, P_DOWNLOADING, P_INSTALLED, P_ERROR };
static struct {
    int phase;
    char msg[400];              // error or status text
    char target[40];            // the version being checked ("" = latest)
    UpdRelease rel;             // last check
    int have_manifest;          // rel has a verified manifest (m)
    UpdiTarget t;               // its verified manifest (t.m) and the zip's links
    char host_ver[40], host_proto[16];   // a host with another version refused us
} U;
static CRITICAL_SECTION u_cs;
static HANDLE worker;
static int installed_this_session;   // one install/go-back per game session (the running files moved)

static int u_ready;
static void set_phase(int phase, const char *fmt, ...) {
    char m[400];
    va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    EnterCriticalSection(&u_cs);
    U.phase = phase;
    snprintf(U.msg, sizeof U.msg, "%s", m);
    LeaveCriticalSection(&u_cs);
    LOG("update: %s", m);
}

static void scope_host(char *h, size_t n) {   // dev: the update_api host, for netguard's scope
    h[0] = 0;
    const char *p = strstr(api, "://");
    if (!p) return;
    p += 3;
    size_t k = strcspn(p, ":/");
    snprintf(h, n, "%.*s", (int)(k < n ? k : n - 1), p);
}

static DWORD WINAPI check_worker(void *arg) {
    (void)arg;
    char target[40], url[400], err[300], extra[112];
    EnterCriticalSection(&u_cs); snprintf(target, sizeof target, "%s", U.target); LeaveCriticalSection(&u_cs);
    updi_release_url(url, sizeof url, api, repo, target[0] ? target : NULL);
    scope_host(extra, sizeof extra);
    netguard_updater_scope(1, extra);
    UpdRelease *rel = calloc(1, sizeof *rel);
    UpdiTarget tg;
    LOG("update: checking %s", url);
    if (!rel) { set_phase(P_ERROR, "Out of memory."); goto out; }
    if (!updi_fetch_release(url, rel, NULL, NULL, err, sizeof err)) {
        if (target[0] && strstr(err, "404")) set_phase(P_ERROR, "There is no b4bcoop %s release on GitHub.", target);
        else set_phase(P_ERROR, "Check failed: %s.", err);
        goto out;
    }
    int r = updi_fetch_manifest(rel, pubkey, &tg, NULL, NULL, err, sizeof err);
    if (r == 2) {   // a release from before the updater (<= 0.6.1)
        EnterCriticalSection(&u_cs); U.rel = *rel; U.have_manifest = 0; LeaveCriticalSection(&u_cs);
        set_phase(P_CHECKED, "Release %s has no in-game update files (it is older than the updater). Get it from GitHub: %s", rel->tag, rel->page);
        goto out;
    }
    if (r == 0) { set_phase(P_ERROR, "Check failed: %s.", err); goto out; }
    if (r < 0) { set_phase(P_ERROR, "Not safe to install: %s.", err); goto out; }
    EnterCriticalSection(&u_cs);
    U.rel = *rel; U.t = tg; U.have_manifest = 1;
    LeaveCriticalSection(&u_cs);
    int c = upd_version_cmp(tg.m.version, B4B_VERSION);
    if (c == 0) set_phase(P_CHECKED, "You have b4bcoop %s already.", tg.m.version);
    else set_phase(P_CHECKED, c > 0 ? "b4bcoop %s is available (protocol %d)." : "b4bcoop %s (protocol %d) is older than yours.", tg.m.version, tg.m.protocol);
out:
    netguard_updater_scope(0, NULL);
    free(rel);
    return 0;
}

static DWORD WINAPI download_worker(void *arg) {
    (void)arg;
    UpdiTarget tg; char err[300], extra[112];
    EnterCriticalSection(&u_cs); tg = U.t; LeaveCriticalSection(&u_cs);
    scope_host(extra, sizeof extra);
    netguard_updater_scope(1, extra);
    uint8_t *zip = NULL; size_t zn = 0;
    LOG("update: downloading %s (%llu bytes)", tg.zip_url, (unsigned long long)tg.m.size);
    int r = updi_download(&tg, pubkey, &dl_got, NULL, &zip, &zn, err, sizeof err);
    netguard_updater_scope(0, NULL);
    if (r == 0) { set_phase(P_ERROR, "Download failed: %s. Nothing was changed.", err); goto out; }
    if (r < 0) { set_phase(P_ERROR, "Not installed: %s. Nothing was changed.", err); goto out; }
    LOG("update: %s verified (size, SHA-256, signature)", tg.m.zip);
    State s;
    if (!updi_install(zip, zn, &tg.m, B4B_VERSION, &s, err, sizeof err)) {
        set_phase(P_ERROR, "Not installed: %s. Your current version is unchanged.", err);
        goto out;
    }
    EnterCriticalSection(&st_cs); st = s; LeaveCriticalSection(&st_cs);
    installed_this_session = 1;
    set_phase(P_INSTALLED, "b4bcoop %s is installed. Restart the game to finish (this game keeps running %s).", tg.m.version, B4B_VERSION);
    overlay_note("[update] installed; restart the game to finish");
out:
    free(zip);
    return 0;
}

static int busy(void) { return worker && WaitForSingleObject(worker, 0) == WAIT_TIMEOUT; }
static void start(LPTHREAD_START_ROUTINE fn) {
    if (worker) { CloseHandle(worker); worker = NULL; }
    worker = CreateThread(NULL, 0, fn, NULL, 0, NULL);
}
static const char *check(const char *ver) {   // NULL = started
    if (unavailable) return unavailable;
    if (busy()) return "busy";
    EnterCriticalSection(&u_cs);
    snprintf(U.target, sizeof U.target, "%s", ver && upd_version_valid(ver) ? ver : "");
    U.have_manifest = 0;
    LeaveCriticalSection(&u_cs);
    set_phase(P_CHECKING, ver ? "Checking GitHub for b4bcoop %s..." : "Checking GitHub for the latest b4bcoop...", ver);
    start(check_worker);
    return NULL;
}
static const char *install(void) {
    if (unavailable) return unavailable;
    if (busy()) return "busy";
    if (installed_this_session) return "already changed this session: restart the game first";
    EnterCriticalSection(&u_cs);
    int ok = U.phase == P_CHECKED && U.have_manifest && strcmp(U.t.m.version, B4B_VERSION);
    LeaveCriticalSection(&u_cs);
    if (!ok) return "check first";
    set_phase(P_DOWNLOADING, "Downloading b4bcoop %s...", U.t.m.version);
    start(download_worker);
    return NULL;
}
static const char *go_back(void) {
    if (unavailable) return unavailable;
    if (busy()) return "busy";
    if (installed_this_session) return "already changed this session: restart the game first";
    State s; state_read(&s);
    if (!s.previous[0] || strcmp(s.installed, B4B_VERSION)) return "no previous version kept";
    char why[200], err[200];
    snprintf(why, sizeof why, "you went back from %s to %s", B4B_VERSION, s.previous);
    if (!revert(&s, why, err, sizeof err)) { set_phase(P_ERROR, "Going back failed: %s.", err); return NULL; }
    EnterCriticalSection(&st_cs); st = s; LeaveCriticalSection(&st_cs);
    installed_this_session = 1;
    set_phase(P_INSTALLED, "b4bcoop %s is back in place. Restart the game to finish.", s.installed);
    return NULL;
}

// ---- host version hints (a host with another protocol refused us) ----
void updater_note_host(const char *ver, const char *proto) {
    if (!ver || !upd_version_valid(ver) || !u_ready) return;
    EnterCriticalSection(&u_cs);
    snprintf(U.host_ver, sizeof U.host_ver, "%s", ver);
    snprintf(U.host_proto, sizeof U.host_proto, "%s", proto ? proto : "");
    LeaveCriticalSection(&u_cs);
    LOG("update: noted the host's version %s (protocol %s)", ver, proto ? proto : "?");
}
const char *updater_hint(void) {
    return enabled && !unavailable ? " Press ~, tab Updates, to get the same version." : "";
}

// ---- ~ window ----
static void notes_short(const char *body, char *out, size_t n) {   // up to the provenance part, ~12 lines
    size_t k = 0; int lines = 0;
    for (const char *p = body; *p && k + 1 < n; p++) {
        if (!strncmp(p, "\n---", 4)) break;
        if (*p == '\r') continue;
        if (*p == '\n' && ++lines >= 12) { snprintf(out + k, n - k, "\n..."); return; }
        out[k++] = *p;
    }
    while (k && (out[k - 1] == '\n' || out[k - 1] == ' ')) k--;
    out[k] = 0;
}
static void panel(void) {
    EnterCriticalSection(&u_cs);
    int phase = U.phase, have = U.have_manifest;
    char msg[400], host_ver[40], host_proto[16], page[300], notes[1200], title[120];
    UpdManifest m = U.t.m;
    snprintf(msg, sizeof msg, "%s", U.msg);
    snprintf(host_ver, sizeof host_ver, "%s", U.host_ver); snprintf(host_proto, sizeof host_proto, "%s", U.host_proto);
    snprintf(page, sizeof page, "%s", U.rel.page); snprintf(title, sizeof title, "%s", U.rel.title[0] ? U.rel.title : U.rel.tag);
    notes_short(U.rel.body, notes, sizeof notes);
    LeaveCriticalSection(&u_cs);
    EnterCriticalSection(&st_cs); State s = st; LeaveCriticalSection(&st_cs);
    int running = busy();

    ov_text("Installed: b4bcoop %s (protocol %d)", B4B_VERSION, B4B_PROTOCOL);
    if (test_key) ov_text_warn("Dev build: update_pubkey= test key and %s", api);
    if (unavailable) { ov_text_warn("In-game updates are not available here: %s.", unavailable); return; }
    ov_text_dim("Releases come from GitHub and must carry the b4bcoop release signature. b4bcoop.ini, add-ons, bans and "
                "logs are never touched. Here nothing is downloaded or changed until you click; a new version starts with "
                "your next game start.");
    if (auto_note[0]) ov_text_dim("Last automatic update: %s.", auto_note);
    if (s.result[0] && strcmp(s.installed, B4B_VERSION) == 0 && strstr(s.result, "put back"))
        ov_text_warn("Last update: %s.", s.result);
    else if (s.result[0] && strstr(s.result, "went back"))
        ov_text_dim("Last change: %s.", s.result);

    if (host_ver[0] && strcmp(host_ver, B4B_VERSION)) {
        ov_heading("The host's version");
        ov_text("A host you tried to join runs b4bcoop %s%s%s%s.", host_ver, host_proto[0] ? " (protocol " : "", host_proto, host_proto[0] ? ")" : "");
        ov_begin_disabled(running || installed_this_session, running ? "busy" : "restart the game first");
        char lab[80]; snprintf(lab, sizeof lab, "Check %s##hostver", host_ver);
        if (ov_button(lab)) check(host_ver);
        ov_end_disabled();
    }

    ov_heading("Updates");
    ov_begin_disabled(running || installed_this_session, running ? "busy" : "restart the game first");
    if (ov_button("Check for updates")) check(NULL);
    ov_end_disabled();
    if (phase == P_CHECKING) ov_text_dim("%s", msg);
    else if (phase == P_DOWNLOADING) {
        LONG64 got = dl_got;
        ov_text_dim("Downloading b4bcoop %s: %lld of %llu KB...", m.version, (long long)got / 1024, (unsigned long long)m.size / 1024);
    } else if (phase == P_ERROR) ov_text_warn("%s", msg);
    else if (phase == P_INSTALLED) ov_text_warn("%s", msg);
    else if (phase == P_CHECKED) {
        if (!have) ov_text_warn("%s", msg);
        else {
            int c = upd_version_cmp(m.version, B4B_VERSION);
            if (c == 0) ov_text("You have b4bcoop %s. Nothing to update.", m.version);
            else {
                ov_text("%s b4bcoop %s (protocol %d)", c > 0 ? "New:" : "Older:", m.version, m.protocol);
                if (m.protocol == B4B_PROTOCOL) ov_text_dim("Same protocol as yours: you can still play with friends who haven't changed version.");
                else ov_text_warn("Protocol %d (yours: %d): afterwards you can only play with friends on protocol %d, and they only with you once they have it too.",
                                  m.protocol, B4B_PROTOCOL, m.protocol);
                if (notes[0]) { ov_text_dim("%s", title); ov_text_dim("%s", notes); }
                if (page[0]) { if (ov_button("Copy the release page link")) ov_copy(page); }
                ov_begin_disabled(running || installed_this_session, running ? "busy" : "restart the game first");
                if (ov_button(c > 0 ? "Download and install on next start" : "Download and install this older version on next start##older")) install();
                ov_end_disabled();
                ov_text_dim("%.1f MB. The files you have now are kept in b4bcoop-update\\backup (\"Go back\" below).", m.size / 1048576.0);
            }
        }
    }
    ov_heading("At the game start");
    int on = auto_on;
    if (ov_checkbox("Update b4bcoop automatically when the game starts##autoupdate", &on))
        ov_setting("auto_update", on == AUTO_DEFAULT ? NULL : on ? "1" : "0", 1);
    ov_text_dim("Before the game opens (co-op starts only), b4bcoop asks GitHub for a newer release, for 2 seconds at most, "
                "and installs it with a progress window (Cancel starts the version you have). Joining a friend through "
                "Steam gets the friend's version instead. Offline or no answer: the game starts as usual.");
    if (!launcher_auto) ov_text_warn("Needs this version's b4bcoop launcher (xinput1_3.dll next to Back4Blood.exe, in the "
                                     "zip and in every update); it isn't there or is older.");
#ifndef B4B_RELEASE
    ov_text_dim("Developer build: off unless auto_update=1 (env B4B_LAUNCHER_UPDATE=off|on|cancel overrides it).");
#endif

    if (s.previous[0] && !strcmp(s.installed, B4B_VERSION)) {
        ov_heading("Previous version");
        ov_text_dim("b4bcoop %s is kept in b4bcoop-update\\backup.%s", s.previous, s.pending ? " (If this version doesn't start "
                    "properly twice, it is put back automatically.)" : "");
        char lab[80]; snprintf(lab, sizeof lab, "Go back to %s on next start##goback", s.previous);
        ov_begin_disabled(running || installed_this_session, running ? "busy" : "restart the game first");
        if (ov_button_confirm(lab, "Click again to go back")) { const char *e = go_back(); if (e) overlay_note(e); }
        ov_end_disabled();
    }
}

// ---- init / tick / ini ----
static void auto_note_read(void) {   // the launcher's b4bcoop-update\auto.txt: "<when>: <result>"
    wchar_t p[MAX_PATH * 2];
    _snwprintf(p, MAX_PATH * 2, L"%lsauto.txt", updi_dir);
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[1024], when[40] = "", res[300] = ""; DWORD n = 0;
    ReadFile(h, buf, sizeof buf - 1, &n, NULL);
    CloseHandle(h);
    buf[n] = 0;
    char *save;
    for (char *line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (!strncmp(line, "when=", 5)) snprintf(when, sizeof when, "%s", line + 5);
        else if (!strncmp(line, "result=", 7)) snprintf(res, sizeof res, "%s", line + 7);
    }
    if (res[0]) snprintf(auto_note, sizeof auto_note, "%s%s%s", when, when[0] ? ": " : "", res);
}
static void ini_pair(const char *k, const char *v, void *ctx);
int updater_live(const char *k, const char *v) {
    if (!strcmp(k, "auto_update")) {   // the launcher reads it at the next start; here only the tab's checkbox
        auto_on = v && *v ? atoi(v) != 0 : AUTO_DEFAULT;
        return 1;
    }
    if (!strcmp(k, "updates")) {
        enabled = !v || atoi(v) != 0;
        overlay_add_panel("Updates", 95, enabled ? panel : NULL);
        return 1;
    }
#ifndef B4B_RELEASE
    if (!strcmp(k, "update_api") || !strcmp(k, "update_repo") || !strcmp(k, "update_pubkey")) {
        if (busy()) { LOG("update: %s changes after the running request", k); }
        if (v) ini_pair(k, v, NULL);
        else if (!strcmp(k, "update_api")) snprintf(api, sizeof api, "%s", DEFAULT_API);
        else if (!strcmp(k, "update_repo")) snprintf(repo, sizeof repo, "%s", DEFAULT_REPO);
        else { memcpy(pubkey, RELEASE_PUBKEY, 32); test_key = 0; }
        LOG("update: api %s repo %s key %s", api, repo, test_key ? "test" : "release");
        return 1;
    }
#endif
    return 0;
}
static void ini_pair(const char *k, const char *v, void *ctx) {
    (void)ctx;
    if (!strcmp(k, "updates")) enabled = atoi(v) != 0;
    else if (!strcmp(k, "auto_update")) auto_on = *v ? atoi(v) != 0 : AUTO_DEFAULT;
#ifndef B4B_RELEASE
    else if (!strcmp(k, "update_api") && *v) { snprintf(api, sizeof api, "%s", v); size_t l = strlen(api); while (l && api[l - 1] == '/') api[--l] = 0; }
    else if (!strcmp(k, "update_repo") && *v) snprintf(repo, sizeof repo, "%s", v);
    else if (!strcmp(k, "update_never_healthy")) never_healthy = atoi(v) != 0;
    else if (!strcmp(k, "update_pubkey")) { if (upd_hex_decode(v, pubkey, 32)) test_key = 1; else LOG("update: bad update_pubkey (64 hex digits)"); }
#endif
}
void updater_init(void) {
    InitializeCriticalSection(&u_cs); u_ready = 1;
    InitializeCriticalSection(&st_cs); st_cs_ready = 1;
    memcpy(pubkey, RELEASE_PUBKEY, 32);
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)updater_init, &self);
    paths_init(self);
    cmds_ini_each(ini_pair, NULL);
    if (!unavailable) {
        state_read(&st);
        wchar_t p[MAX_PATH * 2];
        _snwprintf(p, MAX_PATH * 2, L"%lsxinput1_3.dll", updi_root);
        launcher_auto = updi_file_has(p, "b4bcoop-launcher-features: launch-choice-1 auto-update-1");
        auto_note_read();
    }
    if (enabled) overlay_add_panel("Updates", 95, panel);
    LOG("update: %s%s%s; api %s%s; state: installed=%s previous=%s pending=%d starts=%d skip=%s; at the game start: %s%s%s",
        enabled ? "Updates tab on" : "off (updates=0)",
        unavailable ? ", unavailable: " : "", unavailable ? unavailable : "", api, test_key ? " (test key)" : "",
        st.installed[0] ? st.installed : "-", st.previous[0] ? st.previous : "-", st.pending, st.starts,
        st.skip[0] ? st.skip : "-", !launcher_auto ? "no updating launcher" : auto_on ? "automatic update on" : "automatic update off",
        auto_note[0] ? "; last: " : "", auto_note);
}
void updater_tick(float dt) {
    static float t;
    static int done;
    if (done || !st_cs_ready) return;
    if (!st.pending || strcmp(st.installed, B4B_VERSION)) { done = 1; return; }
    if (never_healthy) { done = 1; LOG("update: update_never_healthy=1: not marking %s healthy (test)", B4B_VERSION); return; }
    t += dt;
    if (t < HEALTHY_SECONDS) return;
    done = 1;
    EnterCriticalSection(&st_cs);
    st.pending = 0; st.starts = 0;
    snprintf(st.result, sizeof st.result, "%s started fine", B4B_VERSION);
    State s = st;
    LeaveCriticalSection(&st_cs);
    state_write(&s);
    LOG("update: %s started fine (%.0f s of play); previous version %s stays in the backup", B4B_VERSION, t, s.previous);
}

#ifndef B4B_RELEASE
int updater_cmd(const char *verb, char *rest, Out *o) {
    if (strcmp(verb, "update")) return 0;
    char *a = rest ? strtok(rest, " ") : NULL, *b = a ? strtok(NULL, " ") : NULL;
    const char *e = NULL;
    if (a && !strcmp(a, "check")) e = check(b);
    else if (a && !strcmp(a, "install")) e = install();
    else if (a && !strcmp(a, "goback")) e = go_back();
    else if (a && !strcmp(a, "host") && b) { updater_note_host(b, strtok(NULL, " ")); }
    else if (a && !strcmp(a, "fetch") && b) {   // spike/test: one GET through the same path (noscope: netguard must block it)
        char *mode = strtok(NULL, " "), err[300], hex[65];
        int scoped = !mode || strcmp(mode, "noscope");
        uint8_t *buf, h[32]; size_t n;
        if (scoped) netguard_updater_scope(1, NULL);
        int ok = updi_http_get(b, "application/octet-stream", UPD_MAX_ZIP, &buf, &n, &dl_got, NULL, NULL, err, sizeof err);
        if (scoped) netguard_updater_scope(0, NULL);
        if (!ok) { out_printf(o, "fetch failed: %s\n", err); return 1; }
        upd_sha256(buf, n, h); upd_hex(h, 32, hex);
        out_printf(o, "fetch ok: %zu bytes sha256 %s\n", n, hex);
        free(buf);
        return 1;
    }
    else if (a && strcmp(a, "status")) { out_printf(o, "usage: update [status|check [ver]|install|goback|host <ver> [proto]|fetch <url> [noscope]]\n"); return 1; }
    if (e) out_printf(o, "error: %s\n", e);
    EnterCriticalSection(&u_cs);
    static const char *names[] = {"idle", "checking", "checked", "downloading", "installed", "error"};
    out_printf(o, "update: phase=%s busy=%d msg=%s\n", names[U.phase], busy(), U.msg);
    if (U.have_manifest) out_printf(o, "manifest: version=%s protocol=%d zip=%s size=%llu\n", U.t.m.version, U.t.m.protocol, U.t.m.zip, (unsigned long long)U.t.m.size);
    LeaveCriticalSection(&u_cs);
    State s; state_read(&s);
    out_printf(o, "state: installed=%s previous=%s pending=%d starts=%d files=%s result=%s\n", s.installed, s.previous, s.pending,
               s.starts, s.files, s.result);
    out_printf(o, "api=%s repo=%s key=%s unavailable=%s session_changed=%d\n", api, repo, test_key ? "test" : "release",
               unavailable ? unavailable : "-", installed_this_session);
    return 1;
}
#endif
