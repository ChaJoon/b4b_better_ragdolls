// updinst: the updater's Windows side, shared by the agent (updater.c, the ~ window's Updates tab) and the launcher
// (native/launcher/redirect.c: the automatic update before the game starts). docs/investigations/updater.md.
//   - HTTPS GET through WinHTTP (loaded on the first request), with timeouts and a cancel flag
//   - a release check: GitHub's release JSON, then its signed manifest b4bcoop-update.txt + .sig
//   - the download: zip + .sig, size / SHA-256 / ed25519 checked (updcore.c) before anything is written
//   - the install: the zip's installable files (upd_install_path) staged in b4bcoop-update\staged\<ver>\, the running
//     files moved to backup\<running ver>\ and the new ones into place, by renames (Windows renames a mapped DLL, it
//     doesn't overwrite one); any failure undoes every move
//   - b4bcoop-update\state.txt and the revert (the agent's start check, updater_early, and "Go back")
// No game code and no netguard here: the agent opens netguard's updater scope around each request itself; the
// launcher runs in Steam's launcher process, where netguard isn't (its only requests are these).
#pragma once
#include <windows.h>
#include <stdint.h>
#include "updcore.h"

void updi_set_log(void (*fn)(const char *line));   // file-step lines ("can't move ..."); default: none
void updi_set_user_agent(const char *product);     // User-Agent "<product>/<B4B_VERSION>" (default b4bcoop-updater)

// ---- paths ----
void updi_set_root(const wchar_t *root);            // the game root, trailing backslash
extern wchar_t updi_root[MAX_PATH];                // "" until set
extern wchar_t updi_dir[MAX_PATH];                 // <root>b4bcoop-update\  (trailing backslash)
void updi_join(wchar_t *out, const wchar_t *dir, const char *rel);   // dir + "a/b" as "a\b"; out MAX_PATH * 2
int updi_exists(const wchar_t *p);
void updi_mkdirs_for(const wchar_t *file);
void updi_rmtree(const wchar_t *dir);              // refuses anything outside updi_dir
int updi_write_file(const wchar_t *p, const void *data, size_t n);   // parents created, flushed
int updi_move(const wchar_t *from, const wchar_t *to);               // parents created, replaces
int updi_file_has(const wchar_t *p, const char *needle);             // the file contains these bytes (max 4 MB)

// ---- b4bcoop-update\state.txt ----
// skip: a version the launcher must not install by itself again (it was put back: crashed twice, or Go back)
typedef struct { char installed[40], previous[40], files[600], result[200], skip[40]; int pending, starts; } UpdState;
void updi_state_read(UpdState *s);
int updi_state_write(const UpdState *s);
// Put backup\<previous>\ back and move what the update installed to failed\<installed>\ (works while the new DLL is
// loaded). why goes to result=; skip= becomes the version that was put back. 1 = done.
int updi_revert(UpdState *s, const char *why, char *err, size_t en);

// ---- HTTP ----
typedef struct { int resolve, connect, send, receive; } UpdiTimeouts;   // milliseconds (WinHttpSetTimeouts)
extern const UpdiTimeouts UPDI_TIMEOUTS_NORMAL;                          // 15/15/20/30 s
typedef int (*UpdiSink)(const uint8_t *p, size_t n, void *ctx, char *err, size_t en);   // 1 = go on
// GET url; the body to sink in pieces (more than max bytes = error). 1 = HTTP 200 and the whole body taken.
// progress (may be NULL): bytes so far. cancel (may be NULL): set to stop at the next piece ("cancelled").
// Plain http:// only to 127.0.0.1 in dev builds (a test server); player builds refuse it.
int updi_http_fetch(const char *url, const char *accept, uint64_t max, UpdiSink sink, void *ctx, volatile LONG64 *progress,
                    const UpdiTimeouts *t, volatile LONG *cancel, char *err, size_t en);
// the same into a malloc'd buffer, NUL-terminated (text replies)
int updi_http_get(const char *url, const char *accept, size_t max, uint8_t **out, size_t *outn, volatile LONG64 *progress,
                  const UpdiTimeouts *t, volatile LONG *cancel, char *err, size_t en);

// ---- check, download, install ----
#define UPDI_DEFAULT_API "https://api.github.com"
#define UPDI_DEFAULT_REPO "actuallydan/b4b-coop"
#define UPDI_MANIFEST_NAME "b4bcoop-update.txt"
// <api>/repos/<repo>/releases/latest, or /releases/tags/v<ver> when ver is a version
void updi_release_url(char *url, size_t n, const char *api, const char *repo, const char *ver);
// GET the release JSON and parse it. 1 = ok; 0 = error (err; "not found on GitHub (404)" for a missing tag)
int updi_fetch_release(const char *url, UpdRelease *rel, const UpdiTimeouts *t, volatile LONG *cancel, char *err, size_t en);
typedef struct { UpdManifest m; char zip_url[600], sig_url[600]; } UpdiTarget;
// The release's manifest + its signature, verified against pub; the tag must name the manifest's version and the
// release must carry the zip and its .sig. 1 = ok (out filled); 2 = the release has no update files (older than the
// updater, 0.6.1 and before); 0 = couldn't download (err); -1 = not safe to install (err).
int updi_fetch_manifest(const UpdRelease *rel, const uint8_t pub[32], UpdiTarget *out, const UpdiTimeouts *t,
                        volatile LONG *cancel, char *err, size_t en);
// The zip + its .sig into memory, then upd_zip_verify. 1 = verified (*zip malloc'd, free it); 0 = download failed;
// -1 = not safe (wrong size, hash or signature).
int updi_download(const UpdiTarget *tg, const uint8_t pub[32], volatile LONG64 *progress, volatile LONG *cancel,
                  uint8_t **zip, size_t *zn, char *err, size_t en);
// Stage the verified zip's installable files and swap them in; the running version cur goes to backup\<cur>\ (the
// older backups and failed\ are deleted) and state.txt says installed=<new> previous=<cur> pending=1 starts=0.
// 1 = installed (*out = the new state); 0 = nothing changed (err).
int updi_install(const uint8_t *zip, size_t zn, const UpdManifest *m, const char *cur, UpdState *out, char *err, size_t en);
