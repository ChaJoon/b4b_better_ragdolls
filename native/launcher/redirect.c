// b4bcoop launcher: xinput1_3.dll in the game's ROOT folder, next to the root Back4Blood.exe.
//
// Steam's only public launch entry for app 924970 runs that root exe, a UE bootstrap stub. It builds
//   "<root>\start_protected_game.exe" Gobi -SaveToUserDir <Steam launch options>
// (the Easy Anti-Cheat bootstrapper), then checks prerequisites with a plain LoadLibraryW("XINPUT1_3.DLL") (app dir
// first: this file) and calls CreateProcessW. We patch the stub's import of CreateProcessW. When the stub starts EAC,
// the player chooses (#47, docs/investigations/online-mode.md):
//   b4bcoop co-op  the b4bcoop files switched off earlier (b4bcoop-online\) move back, and
//                  "<root>\Gobi\Binaries\Win64\Back4Blood.exe" starts with the same arguments instead of EAC, so the
//                  agent (X3DAudio1_7.dll) loads.
//   Online         the agent DLLs in Gobi\Binaries\Win64 (X3DAudio1_7.dll, legacy dwmapi.dll) move to
//                  <root>\b4bcoop-online\ first (renames; the game can't load what isn't there), then the stub's own
//                  call starts EAC unchanged: the retail game with none of our code in it. This file stays: only the
//                  stub loads it (the game imports XINPUT1_4, EAC's bootstrapper loads XInput1_4 first).
// The choice: a Steam join (+b4bcoop_join) = co-op; launch option -b4bcoop=coop|off|online|ask; else b4bcoop.ini
// launch=coop|online (a remembered choice; Shift held at the start or -b4bcoop=ask shows the prompt anyway); else a
// prompt (task dialog, mouse/keyboard/controller; with no input for 15 s it picks co-op). Closing it starts nothing.
// Windows loads this file by itself. Wine/Proton prefers its builtin xinput1_3 unless the prefix has a DllOverride:
// the agent sets one for Back4Blood.exe on its first start (online.c), so Proton gets the same prompt from then on.
// Pass-through (EAC, no prompt) when the agent isn't installed (neither in place nor switched off).
// Automatic update (co-op starts only, never Online; docs/investigations/updater.md §7): before the game starts,
// GitHub's releases/latest (or the Steam join's host version: releases/tags/v<ver>) is asked, with 2 s for the whole
// check; no answer, an error or nothing newer = the game starts as before. A newer release is downloaded, verified
// and installed exactly like the ~ window's Updates tab does it (updinst.c + updcore.c: signed manifest, zip size /
// SHA-256 / ed25519, renames with undo, state.txt for the new agent's start check) in a small progress window with
// Cancel (= start the current version). ini auto_update=0 turns it off (player builds; dev builds: off unless
// auto_update=1).
// Log: Gobi\Binaries\Win64\b4bcoop-launcher.log (rewritten every start; each line starts with the ms since the stub
// loaded this file). docs/investigations/launch.md.
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "updcore.h"
#include "updinst.h"
#include "signkeys.h"
#include "b4bcoop_version.h"

typedef BOOL (WINAPI *CreateProcessWFn)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                         LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
static CreateProcessWFn real_cp;
static HMODULE self;
static wchar_t root[MAX_PATH];     // install dir, trailing backslash
static wchar_t bin[MAX_PATH];      // <root>Gobi\Binaries\Win64\  (trailing backslash)
static wchar_t park_dir[MAX_PATH]; // <root>b4bcoop-online\  (trailing backslash): the switched-off agent files
static HANDLE logf = INVALID_HANDLE_VALUE;

// Marker the agent looks for before it offers the launch choice (online.c): this launcher knows "launch=".
// auto-update-1: it updates b4bcoop before the game starts (updater.c's Updates tab shows its switch).
__attribute__((used)) static const char launcher_features[] = "b4bcoop-launcher-features: launch-choice-1 auto-update-1";

// b4bcoop code the game would load from its folder (relative to root). The game never ships either name in Win64.
static const wchar_t *const AGENT[] = { L"Gobi\\Binaries\\Win64\\X3DAudio1_7.dll", L"Gobi\\Binaries\\Win64\\dwmapi.dll" };
#define NAGENT (int)(sizeof AGENT / sizeof AGENT[0])

enum { CH_NONE, CH_COOP, CH_ONLINE, CH_ASK, CH_CANCEL };
static const wchar_t *const CH_NAME[] = { L"none", L"b4bcoop co-op", L"online", L"ask", L"cancel" };

static CRITICAL_SECTION log_cs;    // the update's worker threads log too
static DWORD log_t0;                // GetTickCount at load: every line starts with the ms since then
static void logw(const wchar_t *fmt, ...) {
    if (logf == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&log_cs);
    static wchar_t w[4200]; static char u[12600];
    int n = wsprintfW(w, L"[%6lu ms] ", GetTickCount() - log_t0);
    va_list ap; va_start(ap, fmt);
    n += wvsprintfW(w + n, fmt, ap);   // truncates at 1024 chars; command lines are shorter
    va_end(ap);
    w[n++] = L'\r'; w[n++] = L'\n';
    int m = WideCharToMultiByte(CP_UTF8, 0, w, n, u, sizeof u, NULL, NULL);
    DWORD wr; WriteFile(logf, u, m, &wr, NULL);
    LeaveCriticalSection(&log_cs);
}

static void cat3(wchar_t *out, const wchar_t *a, const wchar_t *b) {   // out = a + b (MAX_PATH * 2)
    lstrcpynW(out, a, MAX_PATH * 2);
    int n = lstrlenW(out);
    lstrcpynW(out + n, b, MAX_PATH * 2 - n);
}
static int fexists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static int exists(const wchar_t *dir, const wchar_t *name) { wchar_t p[MAX_PATH * 2]; cat3(p, dir, name); return fexists(p); }
static void mkparents(const wchar_t *file) {
    wchar_t p[MAX_PATH * 2];
    lstrcpynW(p, file, MAX_PATH * 2);
    for (wchar_t *c = p + 3; *c; c++)
        if (*c == L'\\') { *c = 0; CreateDirectoryW(p, NULL); *c = L'\\'; }
}

static const wchar_t *find_ci(const wchar_t *s, const wchar_t *sub) {
    int n = lstrlenW(sub);
    for (; *s; s++) if (CompareStringOrdinal(s, n, sub, n, TRUE) == CSTR_EQUAL) return s;
    return NULL;
}

// First token of a command line (quoted or not) -> pointer just past it.
static const wchar_t *skip_token(const wchar_t *s, const wchar_t **tok, int *len) {
    while (*s == L' ' || *s == L'\t') s++;
    if (*s == L'"') { *tok = ++s; while (*s && *s != L'"') s++; *len = (int)(s - *tok); if (*s) s++; }
    else { *tok = s; while (*s && *s != L' ' && *s != L'\t') s++; *len = (int)(s - *tok); }
    return s;
}

static int word_is(const wchar_t *v, int n, const wchar_t *w) {
    return n == lstrlenW(w) && CompareStringOrdinal(v, n, w, n, TRUE) == CSTR_EQUAL;
}
static int choice_of(const wchar_t *v, int n) {   // "coop" / "online" / "ask" (and -b4bcoop=off = online)
    if (word_is(v, n, L"coop") || word_is(v, n, L"co-op") || word_is(v, n, L"on")) return CH_COOP;
    if (word_is(v, n, L"online") || word_is(v, n, L"off")) return CH_ONLINE;
    if (word_is(v, n, L"ask")) return CH_ASK;
    return CH_NONE;
}
// -b4bcoop=<value> in the launch options
static int option_choice(const wchar_t *rest) {
    const wchar_t *p = find_ci(rest, L"-b4bcoop=");
    if (!p) return CH_NONE;
    p += 9;
    int n = 0;
    while (p[n] && p[n] != L' ' && p[n] != L'\t' && p[n] != L'"') n++;
    return choice_of(p, n);
}

// ---- moving the agent out of the game's way and back ----
static int n_live(void) { int n = 0; for (int i = 0; i < NAGENT; i++) n += exists(root, AGENT[i]); return n; }
static int n_parked(void) { int n = 0; for (int i = 0; i < NAGENT; i++) n += exists(park_dir, AGENT[i]); return n; }

static void park_readme(void) {
    static const char txt[] =
        "b4bcoop is switched off: you chose Online when the game started.\r\n"
        "Its game files are kept in this folder, so the online game (with Easy Anti-Cheat) runs without them.\r\n"
        "They move back by themselves the next time you choose \"b4bcoop co-op\" when the game starts.\r\n"
        "\r\n"
        "By hand: move the Gobi folder from here into the game folder (merge it with the game's Gobi folder),\r\n"
        "or extract the b4bcoop zip again.\r\n";
    wchar_t p[MAX_PATH * 2]; cat3(p, park_dir, L"README.txt");
    mkparents(p);
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(h, txt, sizeof txt - 1, &w, NULL);
    CloseHandle(h);
}
// 1 when no agent file is left in the game's folders
static int park(void) {
    for (int i = 0; i < NAGENT; i++) {
        wchar_t live[MAX_PATH * 2], dst[MAX_PATH * 2];
        cat3(live, root, AGENT[i]); cat3(dst, park_dir, AGENT[i]);
        if (!fexists(live)) continue;
        mkparents(dst);
        if (MoveFileExW(live, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) logw(L"switched off: %s -> b4bcoop-online\\%s", AGENT[i], AGENT[i]);
        else logw(L"can't move %s: error %lu", AGENT[i], GetLastError());
    }
    park_readme();
    int left = n_live();
    if (left) logw(L"%d agent file(s) still in place", left);
    return !left;
}
static void rm_park_dirs(void) {
    static const wchar_t *const dirs[] = { L"Gobi\\Binaries\\Win64", L"Gobi\\Binaries", L"Gobi", L"" };
    wchar_t p[MAX_PATH * 2];
    cat3(p, park_dir, L"README.txt");
    DeleteFileW(p);
    for (int i = 0; i < 4; i++) { cat3(p, park_dir, dirs[i]); RemoveDirectoryW(p); }   // only empty ones go
}
// 1 when nothing is left switched off
static int unpark(void) {
    for (int i = 0; i < NAGENT; i++) {
        wchar_t live[MAX_PATH * 2], dst[MAX_PATH * 2];
        cat3(live, root, AGENT[i]); cat3(dst, park_dir, AGENT[i]);
        if (!fexists(dst)) continue;
        if (fexists(live)) {   // installed again in the meantime (a zip extracted over it): that copy wins
            if (DeleteFileW(dst)) logw(L"kept the installed %s, dropped the switched-off copy", AGENT[i]);
            else logw(L"can't delete b4bcoop-online\\%s: error %lu", AGENT[i], GetLastError());
        } else if (MoveFileExW(dst, live, MOVEFILE_WRITE_THROUGH)) logw(L"switched on: b4bcoop-online\\%s -> %s", AGENT[i], AGENT[i]);
        else logw(L"can't move b4bcoop-online\\%s back: error %lu", AGENT[i], GetLastError());
    }
    int left = n_parked();
    if (!left) rm_park_dirs();
    return !left;
}

// ---- the offline save, before an online start ----
// An online sign-in checks the local PlayerProfileSettings against the online account and may reset it
// (docs/investigations/test-profiles.md). Before each online start a copy of the save (.sav = the save, .json = the
// game's export) goes to PlayerProfileSettings-b4bcoop-before-online-<yyyymmdd-hhmmss>.sav/.json next to it; the 5
// newest are kept. The game runs with -SaveToUserDir: %LOCALAPPDATA%\Back4Blood\Steam\Saved\SaveGames.
#define KEEP_SAVE_BACKUPS 5
static void backup_save(void) {
    wchar_t dir[MAX_PATH * 2];
    DWORD k = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (!k || k >= MAX_PATH) { logw(L"save backup: no LOCALAPPDATA"); return; }
    lstrcatW(dir, L"\\Back4Blood\\Steam\\Saved\\SaveGames\\");
    if (!exists(dir, L"PlayerProfileSettings.sav")) { logw(L"save backup: no PlayerProfileSettings.sav in %s", dir); return; }
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t stamp[32];
    wsprintfW(stamp, L"%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    static const wchar_t *const ext[] = { L".sav", L".json" };
    for (int i = 0; i < 2; i++) {
        wchar_t from[MAX_PATH * 2], to[MAX_PATH * 2], name[96];
        wsprintfW(name, L"PlayerProfileSettings%s", ext[i]); cat3(from, dir, name);
        wsprintfW(name, L"PlayerProfileSettings-b4bcoop-before-online-%s%s", stamp, ext[i]); cat3(to, dir, name);
        if (fexists(from)) {
            if (CopyFileW(from, to, TRUE)) logw(L"save backup: %s", name);
            else logw(L"save backup: can't copy to %s: error %lu", name, GetLastError());
        }
    }
    // keep the newest KEEP_SAVE_BACKUPS (names sort by time)
    static wchar_t names[64][64];
    int n = 0;
    wchar_t pat[MAX_PATH * 2]; cat3(pat, dir, L"PlayerProfileSettings-b4bcoop-before-online-*.sav");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do if (n < 64 && lstrlenW(fd.cFileName) < 60) lstrcpyW(names[n++], fd.cFileName); while (FindNextFileW(h, &fd));
    FindClose(h);
    for (; n > KEEP_SAVE_BACKUPS; n--) {   // drop the oldest (smallest name) each round
        int o = 0;
        for (int i = 1; i < n; i++) if (lstrcmpW(names[i], names[o]) < 0) o = i;
        wchar_t p[MAX_PATH * 2], base[64];
        lstrcpyW(base, names[o]); base[lstrlenW(base) - 4] = 0;   // without ".sav"
        wsprintfW(pat, L"%s.sav", base); cat3(p, dir, pat); DeleteFileW(p);
        wsprintfW(pat, L"%s.json", base); cat3(p, dir, pat); DeleteFileW(p);
        lstrcpyW(names[o], names[n - 1]);
    }
}

// ---- b4bcoop.ini: launch= ----
static void ini_path(wchar_t *p) {   // the agent's rule (cmds_config_path): B4B_COOP_CONFIG, else next to the agent
    DWORD n = GetEnvironmentVariableW(L"B4B_COOP_CONFIG", p, MAX_PATH);
    if (!n || n >= MAX_PATH) cat3(p, bin, L"b4bcoop.ini");
}
static char *read_all(const wchar_t *p, DWORD *len) {
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD n = GetFileSize(h, NULL), got = 0;
    char *b = n < (1 << 20) ? HeapAlloc(GetProcessHeap(), 0, n + 1) : NULL;
    if (b && (!ReadFile(h, b, n, &got, NULL) || got != n)) { HeapFree(GetProcessHeap(), 0, b); b = NULL; }
    CloseHandle(h);
    if (b) { b[n] = 0; *len = n; }
    return b;
}
// an active "launch=" line (key at the start of the line, like the agent's reader)
static int ini_launch(void) {
    wchar_t p[MAX_PATH * 2]; ini_path(p);
    DWORD len; char *b = read_all(p, &len);
    if (!b) return CH_NONE;
    int r = CH_NONE;
    for (DWORD i = 0; i < len; ) {
        DWORD e = i;
        while (e < len && b[e] != '\n') e++;
        if (e - i > 7 && !memcmp(b + i, "launch=", 7)) {
            DWORD v = i + 7, ve = e;
            while (v < ve && b[v] == ' ') v++;
            while (ve > v && (b[ve - 1] == '\r' || b[ve - 1] == ' ' || b[ve - 1] == '\t')) ve--;
            wchar_t w[16]; int n = 0;
            for (DWORD k = v; k < ve && n < 15; k++) w[n++] = (wchar_t)(unsigned char)b[k];
            r = choice_of(w, n);
            break;
        }
        i = e + 1;
    }
    HeapFree(GetProcessHeap(), 0, b);
    return r;
}
// "Remember my choice": launch=<v> in b4bcoop.ini, like the agent's writer (cmds_ini_set): the first active line is
// replaced, else a commented ";launch=" line, else it is appended; everything else stays as it is.
static void ini_set_launch(const char *v) {
    wchar_t p[MAX_PATH * 2], tmp[MAX_PATH * 2];
    ini_path(p);
    cat3(tmp, p, L".tmp");
    DWORD len = 0; char *b = read_all(p, &len);
    const char *eol = b && strstr(b, "\r\n") ? "\r\n" : b ? "\n" : "\r\n";
    long act = -1, com = -1;
    for (DWORD i = 0; b && i < len; ) {
        DWORD e = i;
        while (e < len && b[e] != '\n') e++;
        if (act < 0 && e - i >= 7 && !memcmp(b + i, "launch=", 7)) act = i;
        else if (com < 0 && e - i >= 8 && (b[i] == ';' || b[i] == '#') && !memcmp(b + i + 1, "launch=", 7)) com = i;
        i = e + 1;
    }
    long at = act >= 0 ? act : com;
    char line[64];
    wsprintfA(line, "launch=%s", v);
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { logw(L"can't write %s: error %lu", tmp, GetLastError()); if (b) HeapFree(GetProcessHeap(), 0, b); return; }
    DWORD w;
    if (at >= 0) {
        DWORD e = at;
        while (e < len && b[e] != '\n' && b[e] != '\r') e++;
        WriteFile(h, b, at, &w, NULL);
        WriteFile(h, line, lstrlenA(line), &w, NULL);
        WriteFile(h, b + e, len - e, &w, NULL);
    } else {
        if (b) WriteFile(h, b, len, &w, NULL);
        if (b && len && b[len - 1] != '\n') WriteFile(h, eol, lstrlenA(eol), &w, NULL);
        WriteFile(h, line, lstrlenA(line), &w, NULL);
        WriteFile(h, eol, lstrlenA(eol), &w, NULL);
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    if (b) HeapFree(GetProcessHeap(), 0, b);
    if (MoveFileExW(tmp, p, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) logw(L"remembered: launch=%S in %s", v, p);
    else { logw(L"can't replace %s: error %lu", p, GetLastError()); DeleteFileW(tmp); }
}
// Any other key, read like the agent reads it (cmds_ini_each): "key=value" with the key at the start of the line,
// spaces after '=' skipped; the last such line counts. 1 = found.
static int ini_get(const char *key, char *out, int n) {
    wchar_t p[MAX_PATH * 2]; ini_path(p);
    DWORD len; char *b = read_all(p, &len);
    if (!b) return 0;
    int kl = lstrlenA(key), found = 0;
    for (DWORD i = 0; i < len; ) {
        DWORD e = i;
        while (e < len && b[e] != '\n') e++;
        if (e - i > (DWORD)kl && !memcmp(b + i, key, kl) && b[i + kl] == '=') {
            DWORD v = i + kl + 1, ve = e;
            while (v < ve && b[v] == ' ') v++;
            while (ve > v && (b[ve - 1] == '\r' || b[ve - 1] == ' ' || b[ve - 1] == '\t')) ve--;
            int m = (int)(ve - v) < n - 1 ? (int)(ve - v) : n - 1;
            memcpy(out, b + v, m); out[m] = 0;
            found = 1;
        }
        i = e + 1;
    }
    HeapFree(GetProcessHeap(), 0, b);
    return found;
}

#ifndef B4B_RELEASE
// Dev: B4B_LAUNCHER_SHOT=<Windows path prefix> saves the launcher's windows as they are on screen (BMP, 32-bit) as
// <prefix><n>-<tag>.bmp: the prompt once, the update window at each new text (tests check the wording this way).
static char shot_prefix[300];
static int shot_n;
static void dev_shot(HWND h, const char *tag) {
    if (!shot_prefix[0]) return;
    RECT r;
    if (!GetWindowRect(h, &r)) return;
    int w = r.right - r.left, ht = r.bottom - r.top;
    if (w <= 0 || ht <= 0 || w > 4000 || ht > 4000) return;
    HDC wdc = GetWindowDC(h), mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -ht;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bm = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bm && bits) {
        HGDIOBJ old = SelectObject(mdc, bm);
        BitBlt(mdc, 0, 0, w, ht, wdc, 0, 0, SRCCOPY);
        SelectObject(mdc, old);
        char path[400];
        snprintf(path, sizeof path, "%s%02d-%s.bmp", shot_prefix, ++shot_n, tag);
        HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            BITMAPFILEHEADER fh; memset(&fh, 0, sizeof fh);
            DWORD img = (DWORD)w * ht * 4, wr;
            fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader; fh.bfSize = fh.bfOffBits + img;
            WriteFile(f, &fh, sizeof fh, &wr, NULL);
            WriteFile(f, &bi.bmiHeader, sizeof bi.bmiHeader, &wr, NULL);
            WriteFile(f, bits, img, &wr, NULL);
            CloseHandle(f);
            logw(L"dev: screenshot %S (%dx%d)", path, w, ht);
        }
    }
    if (bm) DeleteObject(bm);
    DeleteDC(mdc);
    ReleaseDC(h, wdc);
}
#endif

// ---- the prompt ----
#define ID_COOP 1001
#define ID_ONLINE 1002
typedef DWORD (WINAPI *XInputGetStateFn)(DWORD, void *);
static XInputGetStateFn xget;
static WORD pad_last = 0xFFFF;     // buttons held when the prompt opened count only after they were released
static WORD pad_down(void);
#ifndef B4B_RELEASE
static char auto_answer[32];       // dev: B4B_LAUNCHER_ANSWER=coop|online|coop+remember|online+remember|cancel|wait
#endif                             // (wait: no answer, the countdown decides)

// With no input at all (no key, mouse, controller) for COUNTDOWN_S seconds the prompt picks co-op, the safe default
// (b4bcoop never goes online). Any input stops the countdown; then it waits for the player.
#define COUNTDOWN_S 15
static int countdown = 1, shown_s = -1;
static POINT cursor_at_open;
// Real input only: the mouse moved or a key/mouse button is (or was since the last look) down. Not
// GetLastInputInfo: Wine counts window focus changes and screen grabs as input too.
static int input_seen(void) {
    POINT p;
    if (GetCursorPos(&p) && (p.x - cursor_at_open.x > 4 || cursor_at_open.x - p.x > 4 ||
                             p.y - cursor_at_open.y > 4 || cursor_at_open.y - p.y > 4)) return 1;
    for (int vk = 1; vk < 255; vk++) if (GetAsyncKeyState(vk)) return 1;
    return 0;
}
static void countdown_text(HWND h, int s) {
    static wchar_t t[160];
    if (s >= 0) wsprintfW(t, L"b4bcoop is installed. Starting b4bcoop co-op in %d s...", s);
    else lstrcpyW(t, L"b4bcoop is installed.");
    SendMessageW(h, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)t);
}
static void countdown_stop(HWND h, const wchar_t *why) {
    if (!countdown) return;
    countdown = 0;
    logw(L"prompt: countdown stopped (%s)", why);
    countdown_text(h, -1);
}

static HRESULT CALLBACK td_cb(HWND h, UINT msg, WPARAM wp, LPARAM lp, LONG_PTR ref) {
    (void)lp; (void)ref;
    if (msg == TDN_CREATED) { SetForegroundWindow(h); GetCursorPos(&cursor_at_open); countdown_text(h, COUNTDOWN_S); return S_OK; }
    if (msg == TDN_VERIFICATION_CLICKED) { logw(L"prompt: remember %s", wp ? L"on" : L"off"); countdown_stop(h, L"input"); return S_OK; }
    if (msg != TDN_TIMER) return S_OK;
    if (countdown) {
        if (wp < 1000) {   // keys still held from the start (Shift, the Play click) don't count
            GetCursorPos(&cursor_at_open);
            for (int vk = 1; vk < 255; vk++) GetAsyncKeyState(vk);
        } else if (input_seen()) countdown_stop(h, L"keyboard or mouse");
        else if (wp >= COUNTDOWN_S * 1000) {
            countdown = 0;
            logw(L"prompt: no input for %d s: b4bcoop co-op", COUNTDOWN_S);
            SendMessageW(h, TDM_CLICK_BUTTON, ID_COOP, 0);
            return S_OK;
        } else {
            int s = COUNTDOWN_S - (int)(wp / 1000);
            if (s != shown_s) { shown_s = s; countdown_text(h, s); }
        }
    }
#ifndef B4B_RELEASE
    static int prompt_shot;
    if (!prompt_shot && wp >= 1000) { prompt_shot = 1; dev_shot(h, "prompt"); }
    if (auto_answer[0] && strcmp(auto_answer, "wait") && wp >= 1500) {   // ms since the dialog opened: long enough to see it (screenshots)
        const char *a = auto_answer;
        logw(L"prompt: dev auto-answer %S", a);
        if (strstr(a, "remember")) SendMessageW(h, TDM_CLICK_VERIFICATION, TRUE, FALSE);
        SendMessageW(h, TDM_CLICK_BUTTON, !strncmp(a, "coop", 4) ? ID_COOP : !strncmp(a, "online", 6) ? ID_ONLINE : IDCANCEL, 0);
        auto_answer[0] = 0;
        return S_OK;
    }
#endif
    // controller (Steam Deck, couch): A = co-op, Y = online, X = remember, B = close
    WORD down = pad_down();
    if (down) countdown_stop(h, L"controller");
    if (down & 0x1000) SendMessageW(h, TDM_CLICK_BUTTON, ID_COOP, 0);
    else if (down & 0x8000) SendMessageW(h, TDM_CLICK_BUTTON, ID_ONLINE, 0);
    else if (down & 0x2000) SendMessageW(h, TDM_CLICK_BUTTON, IDCANCEL, 0);
    else if (down & 0x4000) {
        static BOOL on;
        on = !on;
        SendMessageW(h, TDM_CLICK_VERIFICATION, on, FALSE);
    }
    return S_OK;
}

// The windows (the prompt, the update's progress): comctl32 v6 (task dialogs, themed controls) through our own
// manifest, resource 3 (launcher.rc), active only between ui_begin and ui_end; the controller through XInput 1.4.
typedef HRESULT (WINAPI *TDI)(const TASKDIALOGCONFIG *, int *, int *, BOOL *);
static HANDLE ui_ctx = INVALID_HANDLE_VALUE;
static ULONG_PTR ui_cookie;
static TDI ui_begin(void) {   // TaskDialogIndirect, or NULL (very old comctl32)
    wchar_t me[MAX_PATH];
    GetModuleFileNameW(self, me, MAX_PATH);
    ACTCTXW ac = { sizeof ac };
    ac.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    ac.lpSource = me; ac.hModule = self; ac.lpResourceName = MAKEINTRESOURCEW(3);
    ui_ctx = CreateActCtxW(&ac);
    ui_cookie = 0;
    if (ui_ctx != INVALID_HANDLE_VALUE && !ActivateActCtx(ui_ctx, &ui_cookie)) ui_cookie = 0;
    HMODULE cc = LoadLibraryW(L"comctl32.dll");
    TDI tdi = cc ? (TDI)(void *)GetProcAddress(cc, "TaskDialogIndirect") : NULL;
    wchar_t xp[MAX_PATH];
    UINT sl = GetSystemDirectoryW(xp, MAX_PATH);
    if (!xget && sl && sl < MAX_PATH - 20) {
        lstrcatW(xp, L"\\XInput1_4.dll");
        HMODULE x = LoadLibraryW(xp);
        if (x) xget = (XInputGetStateFn)(void *)GetProcAddress(x, "XInputGetState");
    }
    pad_last = 0xFFFF;
    return tdi;
}
static void ui_end(void) {
    if (ui_cookie) DeactivateActCtx(0, ui_cookie);
    if (ui_ctx != INVALID_HANDLE_VALUE) ReleaseActCtx(ui_ctx);
    ui_cookie = 0; ui_ctx = INVALID_HANDLE_VALUE;
}
static WORD pad_down(void) {   // controller buttons pressed since the last look (any of 4 pads)
    if (!xget) return 0;
    struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } st;
    WORD now = 0;
    for (DWORD i = 0; i < 4; i++) if (xget(i, &st) == ERROR_SUCCESS) now |= st.buttons;
    WORD down = now & ~pad_last;
    pad_last = now;
    return down;
}

static int prompt(int *remember) {
    *remember = 0;
    TDI tdi = ui_begin();
    int r = CH_CANCEL;
    if (tdi) {
        static const TASKDIALOG_BUTTON buttons[] = {
            { ID_COOP, L"b4bcoop co-op\nOffline mode, with your Steam friends." },
            { ID_ONLINE, L"Online\nThe official online game with Easy Anti-Cheat. b4bcoop switches itself off until you "
                         L"pick co-op again." },
        };
        TASKDIALOGCONFIG c = { sizeof c };
        c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_CALLBACK_TIMER;
        c.pszWindowTitle = L"Back 4 Blood";
        c.pszMainInstruction = L"How do you want to play?";
        c.pszContent = L"b4bcoop is installed.";
        c.cButtons = 2; c.pButtons = buttons; c.nDefaultButton = ID_COOP;
        c.pszVerificationText = L"Remember my choice";
        c.pszFooter = L"Ask again later: hold Shift while the game starts, or ~ window > Settings in co-op. "
                      L"Controller: A co-op, Y online, X remember.";
        c.pfCallback = td_cb;
        int btn = 0; BOOL ver = FALSE;
        HRESULT hr = tdi(&c, &btn, NULL, &ver);
        if (FAILED(hr)) { logw(L"prompt: TaskDialogIndirect failed 0x%08lx", (unsigned long)hr); tdi = NULL; }
        else { r = btn == ID_COOP ? CH_COOP : btn == ID_ONLINE ? CH_ONLINE : CH_CANCEL; *remember = ver && r != CH_CANCEL; }
    }
    if (!tdi) {   // no task dialogs (very old comctl32): a plain message box
        logw(L"prompt: message box (no task dialog)");
#ifndef B4B_RELEASE
        if (auto_answer[0]) r = !strncmp(auto_answer, "coop", 4) ? CH_COOP : !strncmp(auto_answer, "online", 6) ? CH_ONLINE : CH_CANCEL;
        else
#endif
        {
            int m = MessageBoxW(NULL, L"b4bcoop is installed. How do you want to play?\n\n"
                                      L"Yes: b4bcoop co-op (offline mode with your Steam friends)\n"
                                      L"No: Online (Easy Anti-Cheat; b4bcoop switches itself off until you pick co-op again)\n"
                                      L"Cancel: don't start",
                                L"Back 4 Blood", MB_YESNOCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST);
            r = m == IDYES ? CH_COOP : m == IDNO ? CH_ONLINE : CH_CANCEL;
        }
    }
    ui_end();
    return r;
}

static void fail_box(const wchar_t *text) {
    logw(L"error: %s", text);
#ifndef B4B_RELEASE
    if (auto_answer[0] || GetEnvironmentVariableW(L"B4B_LAUNCHER_ANSWER", NULL, 0)) return;   // unattended tests
#endif
    MessageBoxW(NULL, text, L"b4bcoop", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
}

// ---- automatic update before a co-op start (docs/investigations/updater.md §7) ----
// 1. On: ini auto_update (player builds default 1; dev builds default 0, so a release never replaces a developer's
//    build by itself; dev env B4B_LAUNCHER_UPDATE=off|on|cancel overrides it, cancel = press Cancel once the download
//    runs), X3DAudio1_7.dll in Gobi\Binaries\Win64 (not a legacy dwmapi.dll dev install), no update of this version
//    still waiting for its first 15 s of play (state.txt pending=1).
// 2. What to ask for (updcore.c upd_auto_plan): a Steam join's ver: = the host's version (none if it is ours, or
//    older with our protocol), else releases/latest. One request, on a worker thread; 2 s for all of it (WinHTTP
//    loading, DNS, TLS, the reply). No answer in time, an error, a version not newer (or not the host's) = the game
//    starts as today, nothing shown. A version put back before (state.txt skip=, failed\<ver>\) isn't installed
//    by itself.
// 3. A task dialog "Updating b4bcoop to X" with a progress bar and "Cancel: start b4bcoop <ours>" (also Esc, the
//    window's X, controller B); its worker fetches the signed manifest, the zip + .sig, verifies them and installs
//    (updinst.c, the Updates tab's code). Cancel works until the files start moving (a second or less); a failure
//    anywhere leaves the current version (updinst undoes every move) and the dialog says so for 4 s. The result goes
//    to the log and to b4bcoop-update\auto.txt (the Updates tab shows it). Then the game starts, with the new agent.
// The launcher itself (this file) is renamed into backup\<ours>\ while it runs: Windows renames a mapped DLL.
#define AU_CHECK_MS 2000
static const UpdiTimeouts AU_QUICK = {AU_CHECK_MS, AU_CHECK_MS, AU_CHECK_MS, AU_CHECK_MS};
#if defined(B4B_RELEASE) && !defined(B4B_FORK)
#define AU_DEFAULT 1
#else
#define AU_DEFAULT 0      // dev builds, and the ragdoll fork (B4B_FORK, build.sh): a release would replace this build
#endif
#ifdef B4B_FORK
#define AU_OFF_WHY "off by default in this fork build (ragdoll; an update installs the official b4bcoop; auto_update=1 turns it on)"
#else
#define AU_OFF_WHY "off by default in developer builds (auto_update=1 turns it on)"
#endif
static char au_api[200] = UPDI_DEFAULT_API, au_repo[100] = UPDI_DEFAULT_REPO;
static uint8_t au_pub[32] = {B4B_RELEASE_PUBKEY_BYTES};
#ifndef B4B_RELEASE
static int au_dev_cancel;   // dev: B4B_LAUNCHER_UPDATE=cancel
#endif

static void au_log(const char *line) { logw(L"update: %S", line); }

// The quick check: the worker owns a reference, the waiting thread the other; whoever lets go last frees it, so a
// check that missed its 2 s just ends by itself (its result unused, nothing written).
typedef struct { volatile LONG refs, cancel; char url[400]; int ok; char err[300]; UpdRelease rel; } AuCheck;
static void au_check_unref(AuCheck *c) { if (!InterlockedDecrement(&c->refs)) free(c); }
static DWORD WINAPI au_check_thread(void *p) {
    AuCheck *c = p;
    c->ok = updi_fetch_release(c->url, &c->rel, &AU_QUICK, &c->cancel, c->err, sizeof c->err);
    au_check_unref(c);
    return 0;
}

// The download + install job behind the progress window. phase moves FETCH -> DL -> INSTALL -> DONE|FAILED by the
// worker; CANCELLED only from FETCH or DL, by the window (compare-and-swap), so a cancelled job never writes a file
// and a job that has started moving files always finishes (or undoes) first.
enum { AU_FETCH, AU_DL, AU_INSTALL, AU_DONE, AU_FAILED, AU_CANCELLED };
typedef struct {
    volatile LONG refs, cancel, phase;
    volatile LONG64 got;
    UpdRelease rel;
    UpdiTarget tg;
    char ver[40], err[300];
    UpdState st;
} AuJob;
static void au_job_unref(AuJob *j) { if (!InterlockedDecrement(&j->refs)) free(j); }
static void au_fail(AuJob *j, LONG from, const char *what, const char *err) {   // j->err: the window's text
    snprintf(j->err, sizeof j->err, "%s", err);
    if (InterlockedCompareExchange(&j->phase, AU_FAILED, from) == from) logw(L"update: failed: %S%S", what, err);
}
static DWORD WINAPI au_job_thread(void *p) {
    AuJob *j = p;
    char err[300];
    uint8_t *zip = NULL; size_t zn = 0;
    int r = updi_fetch_manifest(&j->rel, au_pub, &j->tg, NULL, &j->cancel, err, sizeof err);
    if (r != 1) { au_fail(j, AU_FETCH, r < 0 ? "not safe to install: " : "", err); goto out; }
    if (InterlockedCompareExchange(&j->phase, AU_DL, AU_FETCH) != AU_FETCH) goto out;   // cancelled
    logw(L"update: downloading %S (%lu bytes)", j->tg.zip_url, (unsigned long)j->tg.m.size);
    r = updi_download(&j->tg, au_pub, &j->got, &j->cancel, &zip, &zn, err, sizeof err);
    if (r != 1) { au_fail(j, AU_DL, r < 0 ? "not safe to install: " : "download failed: ", err); goto out; }
    if (InterlockedCompareExchange(&j->phase, AU_INSTALL, AU_DL) != AU_DL) goto out;     // cancelled
    logw(L"update: %S verified (size, SHA-256, signature); installing", j->tg.m.zip);
    if (updi_install(zip, zn, &j->tg.m, B4B_VERSION, &j->st, err, sizeof err)) {
        logw(L"update: installed %S over %S (previous files in b4bcoop-update\\backup\\%S)", j->tg.m.version, B4B_VERSION, B4B_VERSION);
        InterlockedExchange(&j->phase, AU_DONE);
    } else {
        snprintf(j->err, sizeof j->err, "%s", err);
        logw(L"update: not installed: %S; the current version is unchanged", j->err);
        InterlockedExchange(&j->phase, AU_FAILED);
    }
out:
    free(zip);
    au_job_unref(j);
    return 0;
}
static int au_try_cancel(AuJob *j) {
    for (;;) {
        LONG ph = j->phase;
        if (ph != AU_FETCH && ph != AU_DL) return 0;
        if (InterlockedCompareExchange(&j->phase, AU_CANCELLED, ph) == ph) { InterlockedExchange(&j->cancel, 1); return 1; }
    }
}

#define ID_AU_CANCEL 1101
static AuJob *au_job;
static int au_closing, au_shown = -1;
static UINT au_close_at;
static wchar_t au_text[600];
static void au_set_text(HWND h, const wchar_t *t) {
    if (!lstrcmpW(t, au_text)) return;
    lstrcpynW(au_text, t, 600);
    SendMessageW(h, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)au_text);
    RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);   // Wine left stale bands
}
static HRESULT CALLBACK au_cb(HWND h, UINT msg, WPARAM wp, LPARAM lp, LONG_PTR ref) {
    (void)lp; (void)ref;
    AuJob *j = au_job;
    if (msg == TDN_CREATED) {
        SetForegroundWindow(h);
        InterlockedIncrement(&j->refs);   // the worker's
        HANDLE th = CreateThread(NULL, 0, au_job_thread, j, 0, NULL);
        if (th) CloseHandle(th);
        else { au_fail(j, AU_FETCH, "can't start the download: ", "no thread"); au_job_unref(j); }
        return S_OK;
    }
    if (msg == TDN_BUTTON_CLICKED) {
        if (au_closing) return S_OK;   // finished (done or failed): closing starts the game, now or after the pause
        if (au_try_cancel(j)) { logw(L"update: skipped by the player: starting b4bcoop %S", B4B_VERSION); return S_OK; }
        if (j->phase == AU_INSTALL) { au_set_text(h, L"Installing, one moment..."); return S_FALSE; }   // never half-installed
        return S_OK;
    }
    if (msg != TDN_TIMER) return S_OK;
    LONG ph = j->phase;
#ifndef B4B_RELEASE
    static LONG shot_ph = -1;   // dev: one screenshot per phase, a tick after its text was set (painted)
    static int shot_ticks;
    if (ph != shot_ph && ++shot_ticks >= 2) {
        static const char *const names[] = {"getting", "downloading", "installing", "done", "failed", "cancelled"};
        shot_ph = ph; shot_ticks = 0;
        dev_shot(h, names[ph]);
    }
#endif
    wchar_t t[600];
    int pos = 0;
    if (ph == AU_FETCH) { wsprintfW(t, L"Getting b4bcoop %S from GitHub...", j->ver); pos = 2; }
    else if (ph == AU_DL) {
        unsigned long got = (unsigned long)(j->got / 1024), all = (unsigned long)(j->tg.m.size / 1024);
        wsprintfW(t, L"Downloading: %lu of %lu KB", got, all);
        pos = all ? 5 + (int)(85.0 * got / all) : 5;
    } else if (ph == AU_INSTALL) { lstrcpyW(t, L"Installing..."); pos = 95; }
    else if (ph == AU_DONE) { wsprintfW(t, L"b4bcoop %S is installed. Starting the game...", j->ver); pos = 100; }
    else if (ph == AU_FAILED) { wsprintfW(t, L"Couldn't update: %S. Starting b4bcoop %S...", j->err, B4B_VERSION); pos = 0; }
    else return S_OK;
    au_set_text(h, t);
    if (pos != au_shown) { au_shown = pos; SendMessageW(h, TDM_SET_PROGRESS_BAR_POS, pos, 0); }
    // nothing to cancel once the files move; on a failure the button still starts the current version at once
    static int btn_off;
    if ((ph == AU_INSTALL || ph == AU_DONE) != btn_off) { btn_off = !btn_off; SendMessageW(h, TDM_ENABLE_BUTTON, ID_AU_CANCEL, !btn_off); }
    // done: long enough to read (a fast download is over in a second), a failure longer
    if ((ph == AU_DONE || ph == AU_FAILED) && !au_closing) { au_closing = 1; au_close_at = (UINT)wp + (ph == AU_DONE ? 1500 : 4000); }
    if (au_closing && (UINT)wp >= au_close_at) {   // a disabled button would not take the click
        SendMessageW(h, TDM_ENABLE_BUTTON, ID_AU_CANCEL, TRUE);
        SendMessageW(h, TDM_CLICK_BUTTON, ID_AU_CANCEL, 0);
        return S_OK;
    }
    if (au_closing) return S_OK;
    if (pad_down() & 0x2000) SendMessageW(h, TDM_CLICK_BUTTON, ID_AU_CANCEL, 0);   // controller B
#ifndef B4B_RELEASE
    else if (au_dev_cancel && ph == AU_DL && wp >= 300) {
        au_dev_cancel = 0;
        logw(L"update: dev B4B_LAUNCHER_UPDATE=cancel: pressing Skip update");
        SendMessageW(h, TDM_CLICK_BUTTON, ID_AU_CANCEL, 0);
    }
#endif
    return S_OK;
}

static void au_note(const char *fmt, ...) {   // b4bcoop-update\auto.txt: the last automatic update (Updates tab)
    char r[400], b[600];
    va_list ap; va_start(ap, fmt); vsnprintf(r, sizeof r, fmt, ap); va_end(ap);
    SYSTEMTIME t; GetLocalTime(&t);
    int n = snprintf(b, sizeof b, "; b4bcoop launcher: the last automatic update at a game start. Safe to delete.\r\n"
                     "when=%04d-%02d-%02d %02d:%02d\r\nresult=%s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, r);
    wchar_t p[MAX_PATH * 2]; cat3(p, updi_dir, L"auto.txt");
    if (!updi_write_file(p, b, n)) logw(L"update: can't write %s (%lu)", p, GetLastError());
}

static int au_enabled(const char **why) {
    char v[16];
    int on = AU_DEFAULT;
    if (ini_get("auto_update", v, sizeof v) && v[0]) { on = atoi(v) != 0; *why = on ? "auto_update=1" : "auto_update=0"; }
    else *why = AU_DEFAULT ? "on by default" : AU_OFF_WHY;
#ifndef B4B_RELEASE
    if (GetEnvironmentVariableA("B4B_LAUNCHER_UPDATE", v, sizeof v)) {
        on = strcmp(v, "off") != 0;
        au_dev_cancel = !strcmp(v, "cancel");
        *why = "dev B4B_LAUNCHER_UPDATE";
    }
    // dev overrides, like the agent's (updater.c): a local test server and its key
    if (ini_get("update_api", au_api, sizeof au_api) && au_api[0]) { size_t l = strlen(au_api); while (l && au_api[l - 1] == '/') au_api[--l] = 0; }
    else snprintf(au_api, sizeof au_api, "%s", UPDI_DEFAULT_API);
    if (!ini_get("update_repo", au_repo, sizeof au_repo) || !au_repo[0]) snprintf(au_repo, sizeof au_repo, "%s", UPDI_DEFAULT_REPO);
    char hex[80];
    if (ini_get("update_pubkey", hex, sizeof hex)) {
        if (upd_hex_decode(hex, au_pub, 32)) logw(L"update: dev update_pubkey= test key, api %S repo %S", au_api, au_repo);
        else logw(L"update: bad update_pubkey (64 hex digits): the release key stays");
    }
#endif
    return on;
}

static void auto_update(const wchar_t *rest) {
    const char *why;
    if (!au_enabled(&why)) { logw(L"update: automatic update off (%S)", why); return; }
    if (!exists(bin, L"X3DAudio1_7.dll")) { logw(L"update: no X3DAudio1_7.dll in Gobi\\Binaries\\Win64 (a developer install): no automatic update"); return; }
    updi_set_root(root);
    updi_set_log(au_log);
    updi_set_user_agent("b4bcoop-launcher");
    UpdState s; updi_state_read(&s);
    if (s.pending && !strcmp(s.installed, B4B_VERSION)) {
        logw(L"update: %S was just installed and hasn't had its first 15 s of play yet: no check this start", B4B_VERSION);
        return;
    }
    char line[4096]; int k = 0;
    for (; rest[k] && k < (int)sizeof line - 1; k++) line[k] = rest[k] < 128 ? (char)rest[k] : '?';
    line[k] = 0;
    char hv[40], hp[16], target[40];
    int join = upd_join_parse(line, hv, hp);
    int plan = upd_auto_plan(B4B_VERSION, B4B_PROTOCOL, join ? hv : NULL, hp, target);
    if (plan == UPD_AUTO_NONE) {
        logw(L"update: Steam join, the host runs b4bcoop %S (protocol %S); that works with %S: no check", hv, hp[0] ? hp : "?", B4B_VERSION);
        return;
    }
    AuCheck *c = calloc(1, sizeof *c);
    if (!c) return;
    c->refs = 2;
    updi_release_url(c->url, sizeof c->url, au_api, au_repo, plan == UPD_AUTO_TAG ? target : NULL);
    logw(L"update: %S (%S): checking %S", B4B_VERSION, why, c->url);
    if (plan == UPD_AUTO_TAG) logw(L"update: Steam join, the host runs b4bcoop %S (protocol %S): looking for that version", target, hp[0] ? hp : "?");
    DWORD t0 = GetTickCount();
    HANDLE th = CreateThread(NULL, 0, au_check_thread, c, 0, NULL);
    if (!th) { free(c); return; }
    DWORD w = WaitForSingleObject(th, AU_CHECK_MS);
    CloseHandle(th);
    if (w != WAIT_OBJECT_0) {
        InterlockedExchange(&c->cancel, 1);
        logw(L"update: no answer within %d ms (offline or slow): starting b4bcoop %S", AU_CHECK_MS, B4B_VERSION);
        au_check_unref(c);
        return;
    }
    logw(L"update: GitHub answered in %lu ms", GetTickCount() - t0);
    if (!c->ok) {
        if (plan == UPD_AUTO_TAG && strstr(c->err, "404")) logw(L"update: there is no b4bcoop %S release on GitHub: starting b4bcoop %S", target, B4B_VERSION);
        else logw(L"update: check failed (%S): starting b4bcoop %S", c->err, B4B_VERSION);
        au_check_unref(c); return;
    }
    const char *v = upd_tag_version(c->rel.tag);
    if (!upd_auto_wanted(plan, B4B_VERSION, target, c->rel.tag)) {
        logw(L"update: GitHub's release is %S, this is %S: nothing to install", c->rel.tag, B4B_VERSION);
        au_check_unref(c); return;
    }
    if (!upd_release_asset(&c->rel, UPDI_MANIFEST_NAME) || !upd_release_asset(&c->rel, UPDI_MANIFEST_NAME ".sig")) {
        logw(L"update: release %S has no update files (older than the updater): starting b4bcoop %S", c->rel.tag, B4B_VERSION);
        au_check_unref(c); return;
    }
    wchar_t fd[MAX_PATH * 2], wv[40];
    MultiByteToWideChar(CP_UTF8, 0, v, -1, wv, 40);
    wsprintfW(fd, L"%sfailed\\%s", updi_dir, wv);
    if (upd_auto_blocked(v, s.skip, s.result) || updi_exists(fd)) {
        logw(L"update: %S was put back before (it didn't start properly, or you went back from it): not installing it by "
             L"itself; the ~ window's Updates tab can", v);
        au_check_unref(c); return;
    }
    AuJob *j = calloc(1, sizeof *j);
    if (!j) { au_check_unref(c); return; }
    j->refs = 1;
    j->rel = c->rel;
    snprintf(j->ver, sizeof j->ver, "%s", v);
    au_check_unref(c);

    TDI tdi = ui_begin();
    if (!tdi) {   // never update without showing it
        ui_end();
        logw(L"update: b4bcoop %S is available, but no window can show the update here (no task dialogs): starting %S", j->ver, B4B_VERSION);
        au_job_unref(j); return;
    }
    static wchar_t title[120], first[200], footer[300];
    wsprintfW(title, L"Updating b4bcoop to %S", j->ver);
    wsprintfW(first, L"Getting b4bcoop %S from GitHub...", j->ver);
    wsprintfW(footer, L"Skip update plays b4bcoop %S now. Automatic updates can be turned off in the ~ window, tab "
                      L"Updates.", B4B_VERSION);
    TASKDIALOG_BUTTON buttons[] = { { ID_AU_CANCEL, L"Skip update" } };   // short: Wine sizes buttons to fit badly
    TASKDIALOGCONFIG cfg = { sizeof cfg };
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_CALLBACK_TIMER | TDF_SHOW_PROGRESS_BAR;
    cfg.pszWindowTitle = L"Back 4 Blood";
    cfg.pszMainInstruction = title;
    cfg.pszContent = first;
    cfg.cButtons = 1; cfg.pButtons = buttons; cfg.nDefaultButton = ID_AU_CANCEL;
    cfg.pszFooter = footer;
    cfg.cxWidth = 260;   // dialog units: room for the status line on one line
    cfg.pfCallback = au_cb;
    au_job = j; au_closing = 0; au_shown = -1; lstrcpyW(au_text, first);
    logw(L"update: b4bcoop %S is available: downloading, with a progress window", j->ver);
    HRESULT hr = tdi(&cfg, NULL, NULL, NULL);
    ui_end();
    if (FAILED(hr)) logw(L"update: progress window failed 0x%08lx", (unsigned long)hr);
    au_try_cancel(j);   // the window is gone: a download still running stops (nothing written)
    for (int i = 0; i < 600 && j->phase == AU_INSTALL; i++) Sleep(50);   // never start the game halfway through the renames
    LONG ph = j->phase;
    if (ph == AU_DONE) au_note("installed %s over %s at the game start", j->ver, B4B_VERSION);
    else if (ph == AU_FAILED) au_note("couldn't install %s at the game start (%s); kept %s", j->ver, j->err, B4B_VERSION);
    else if (FAILED(hr)) au_note("%s was available; no progress window could be shown, so it was not installed", j->ver);
    else { au_note("%s was available; you skipped it at the game start and kept %s", j->ver, B4B_VERSION); }
    logw(L"update: %S", ph == AU_DONE ? "done: the game starts with the new version" : "the game starts with the current version");
    au_job = NULL;
    au_job_unref(j);
}

static BOOL WINAPI cp_hook(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inh,
                           DWORD flags, LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    const wchar_t *tok; int len;
    const wchar_t *rest = cmd ? skip_token(cmd, &tok, &len) : NULL;
    static const wchar_t eac[] = L"start_protected_game.exe";
    int is_eac = !app && rest && len >= 24 && CompareStringOrdinal(tok + len - 24, 24, eac, 24, TRUE) == CSTR_EQUAL;
    logw(L"stub: CreateProcessW(%s, %s)", app ? app : L"NULL", cmd ? cmd : L"NULL");
    if (!is_eac) return real_cp(app, cmd, pa, ta, inh, flags, env, cwd, si, pi);
    int live = n_live(), parked = n_parked();
    logw(L"agent: %d file(s) in place, %d switched off (b4bcoop-online)", live, parked);
    const wchar_t *why = NULL;
    if (!exists(bin, L"Back4Blood.exe")) why = L"no Gobi\\Binaries\\Win64\\Back4Blood.exe";
    else if (!live && !parked) why = L"agent DLL not installed";
    if (why) { logw(L"pass-through to Easy Anti-Cheat: %s", why); return real_cp(app, cmd, pa, ta, inh, flags, env, cwd, si, pi); }

    int choice, opt = option_choice(rest);
    const wchar_t *how;
    if (find_ci(rest, L"+b4bcoop_join")) { choice = CH_COOP; how = L"a Steam join"; }
    else if (opt == CH_COOP || opt == CH_ONLINE) { choice = opt; how = L"launch option -b4bcoop="; }
    else {
        int shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0, ini = ini_launch();
        if (ini == CH_COOP || ini == CH_ONLINE) {
            if (opt == CH_ASK || shift) logw(L"b4bcoop.ini launch=%s, but %s: asking", ini == CH_COOP ? L"coop" : L"online",
                                             shift ? L"Shift is held" : L"-b4bcoop=ask");
            else { choice = ini; how = L"b4bcoop.ini launch="; goto chosen; }
        }
        int remember;
        choice = prompt(&remember);
        how = L"the prompt";
        if (remember) ini_set_launch(choice == CH_COOP ? "coop" : "online");
    }
chosen:
    logw(L"choice: %s (%s)", CH_NAME[choice], how);
    if (choice == CH_CANCEL) { logw(L"nothing started"); CloseHandle(logf); ExitProcess(0); }

    if (choice == CH_ONLINE) {
        if (!park()) {
            unpark();
            fail_box(L"b4bcoop couldn't switch itself off (a file in Gobi\\Binaries\\Win64 couldn't be moved), so the "
                     L"online game was not started. Close anything that uses the game folder and try again, or remove "
                     L"X3DAudio1_7.dll from Gobi\\Binaries\\Win64 by hand.");
            CloseHandle(logf); ExitProcess(1);
        }
        backup_save();
#ifndef B4B_RELEASE
        // tests (-b4bcoop_test_suspend, or any unattended run: B4B_LAUNCHER_ANSWER set): EAC's launcher is created
        // but never runs, so a test can't reach the online game
        if (find_ci(rest, L"-b4bcoop_test_suspend") || GetEnvironmentVariableW(L"B4B_LAUNCHER_ANSWER", NULL, 0)) {
            flags |= CREATE_SUSPENDED;
            logw(L"test: starting Easy Anti-Cheat's launcher suspended");
        }
#endif
        logw(L"online: b4bcoop is switched off; starting Easy Anti-Cheat unchanged");
        BOOL ok = real_cp(app, cmd, pa, ta, inh, flags, env, cwd, si, pi);
        logw(ok ? L"started pid %lu" : L"CreateProcessW failed: %lu", ok ? pi->dwProcessId : GetLastError());
        return ok;
    }

    if (!unpark() || !n_live()) {
        fail_box(L"b4bcoop couldn't switch itself back on: its files in the b4bcoop-online folder (next to "
                 L"Back4Blood.exe) couldn't be moved back. Move the Gobi folder from there into the game folder by hand, "
                 L"or extract the b4bcoop zip again.");
        CloseHandle(logf); ExitProcess(1);
    }
    auto_update(rest);   // a newer b4bcoop, before the game loads it (never on the Online path above)
    static wchar_t newcmd[32768];
    wsprintfW(newcmd, L"\"%sBack4Blood.exe\"", bin);
    if (lstrlenW(newcmd) + lstrlenW(rest) >= 32767) return real_cp(app, cmd, pa, ta, inh, flags, env, cwd, si, pi);
    lstrcatW(newcmd, rest);
    // Steam sets these for the stub; make sure the game's steam_api sees them (no relaunch through Steam).
    wchar_t v[16];
    if (!GetEnvironmentVariableW(L"SteamAppId", v, 16)) SetEnvironmentVariableW(L"SteamAppId", L"924970");
    if (!GetEnvironmentVariableW(L"SteamGameId", v, 16)) SetEnvironmentVariableW(L"SteamGameId", L"924970");
    logw(L"redirect (no EAC): %s   cwd %s", newcmd, bin);
    BOOL ok = real_cp(NULL, newcmd, pa, ta, inh, flags, env, bin, si, pi);
    logw(ok ? L"started pid %lu" : L"CreateProcessW failed: %lu", ok ? pi->dwProcessId : GetLastError());
    return ok;
}

// Point the main module's CreateProcessW import at cp_hook.
static int patch_iat(HMODULE exe) {
    BYTE *b = (BYTE *)exe;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + dd.VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((char *)(b + d->Name), "kernel32.dll") || !d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(b + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(b + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(b + names->u1.AddressOfData);
            if (lstrcmpA((char *)ibn->Name, "CreateProcessW")) continue;
            DWORD old;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) return 0;
            real_cp = (CreateProcessWFn)iat->u1.Function;
            iat->u1.Function = (ULONG_PTR)cp_hook;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
            return 1;
        }
    }
    return 0;
}

#ifndef B4B_RELEASE
// Dev: the prompt alone (rundll32 xinput1_3.dll,b4bcoop_prompt_test <result file>), for screenshots and the dialog's
// behaviour under Wine without a game folder.
__declspec(dllexport) void CALLBACK b4bcoop_prompt_test(HWND h, HINSTANCE i, LPSTR arg, int show) {
    (void)h; (void)i; (void)show;
    GetEnvironmentVariableA("B4B_LAUNCHER_ANSWER", auto_answer, sizeof auto_answer);
    char lp[300]; wsprintfA(lp, "%s.log", arg && *arg ? arg : "prompt-test");
    logf = CreateFileA(lp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    int rem, c = prompt(&rem);
    logw(L"prompt test: choice %d remember %d", c, rem);
    CloseHandle(logf); logf = INVALID_HANDLE_VALUE;
    char line[80]; wsprintfA(line, "choice=%d remember=%d\r\n", c, rem);
    HANDLE f = CreateFileA(arg && *arg ? arg : "prompt-test.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, line, lstrlenA(line), &w, NULL); CloseHandle(f); }
}
// Dev: the automatic update alone, as a co-op start would run it, without the stub or the game:
//   rundll32 xinput1_3.dll,b4bcoop_update_test <game root>\[|<launch options, e.g. +b4bcoop_join steam:1 proto:3 ver:1.2.3>]
// (B4B_COOP_CONFIG / <root>Gobi\Binaries\Win64\b4bcoop.ini as usual: update_api=, update_pubkey=, auto_update=1;
// or B4B_LAUNCHER_UPDATE=on|cancel). Logs to <root>Gobi\Binaries\Win64\b4bcoop-launcher.log.
__declspec(dllexport) void CALLBACK b4bcoop_update_test(HWND h, HINSTANCE i, LPSTR arg, int show) {
    (void)h; (void)i; (void)show;
    static wchar_t w[4096], rest[4096];
    if (!arg || !MultiByteToWideChar(CP_ACP, 0, arg, -1, w, 4096)) return;
    wchar_t *bar = wcschr(w, L'|');
    rest[0] = L' '; rest[1] = 0;
    if (bar) { *bar = 0; lstrcpynW(rest + 1, bar + 1, 4094); }
    lstrcpynW(root, w, MAX_PATH - 1);
    int n = lstrlenW(root);
    if (n && root[n - 1] != L'\\') lstrcatW(root, L"\\");
    lstrcpyW(bin, root); lstrcatW(bin, L"Gobi\\Binaries\\Win64\\");
    lstrcpyW(park_dir, root); lstrcatW(park_dir, L"b4bcoop-online\\");
    wchar_t lp[MAX_PATH * 2]; cat3(lp, bin, L"b4bcoop-launcher.log");
    logf = CreateFileW(lp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    GetEnvironmentVariableA("B4B_LAUNCHER_SHOT", shot_prefix, sizeof shot_prefix);
    GetEnvironmentVariableA("B4B_LAUNCHER_ANSWER", auto_answer, sizeof auto_answer);
    logw(L"update test: game root %s, launch options%s", root, rest);
    auto_update(rest);
    logw(L"update test: done");
    CloseHandle(logf); logf = INVALID_HANDLE_VALUE;
}
#endif

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID _) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    self = inst;
    InitializeCriticalSection(&log_cs);
    log_t0 = GetTickCount();
    // Only in the root stub: <our dir>\Back4Blood.exe with a Gobi\ tree next to it.
    wchar_t exe[MAX_PATH], me[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH) || !GetModuleFileNameW(inst, me, MAX_PATH)) return TRUE;
    wchar_t *se = wcsrchr(exe, L'\\'), *sm = wcsrchr(me, L'\\');
    if (!se || !sm || se - exe != sm - me || CompareStringOrdinal(exe, (int)(se - exe), me, (int)(sm - me), TRUE)
        != CSTR_EQUAL || lstrcmpiW(se + 1, L"Back4Blood.exe")) return TRUE;
    se[1] = 0;
    lstrcpyW(root, exe);
    lstrcpyW(bin, root); lstrcatW(bin, L"Gobi\\Binaries\\Win64\\");
    lstrcpyW(park_dir, root); lstrcatW(park_dir, L"b4bcoop-online\\");
    if (!exists(bin, L"")) return TRUE;
    // stay loaded whatever the stub does with its module handle
    HMODULE pin; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                    (LPCWSTR)DllMain, &pin);
#ifndef B4B_RELEASE
    GetEnvironmentVariableA("B4B_LAUNCHER_ANSWER", auto_answer, sizeof auto_answer);
    GetEnvironmentVariableA("B4B_LAUNCHER_SHOT", shot_prefix, sizeof shot_prefix);
#endif
    wchar_t lp[MAX_PATH * 2];
    lstrcpyW(lp, bin); lstrcatW(lp, L"b4bcoop-launcher.log");
    logf = CreateFileW(lp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    SYSTEMTIME t; GetLocalTime(&t);
    logw(L"b4bcoop launcher (xinput1_3.dll) %04d-%02d-%02d %02d:%02d:%02d, game root %s", t.wYear, t.wMonth, t.wDay,
         t.wHour, t.wMinute, t.wSecond, exe);
    logw(patch_iat(GetModuleHandleW(NULL)) ? L"hooked CreateProcessW" : L"CreateProcessW import not found");
    return TRUE;
}
