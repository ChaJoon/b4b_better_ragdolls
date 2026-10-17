// updcore: the in-game updater's checks and file formats (#34, docs/investigations/updater.md). Portable C with no
// Windows calls, so native/test/updcore_test.c runs the same code on Linux: SHA-256, the ed25519 release signature
// (Monocypher), the signed update manifest, the zip reader (zlib's puff for deflate), the GitHub release JSON and
// version comparison. updater.c does the downloads, files and the ~ window.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define UPD_MAX_ZIP (64u << 20)       // largest release zip the updater accepts (0.6.1: 0.7 MB)
#define UPD_MAX_FILE (32u << 20)      // largest single file inside it

void upd_sha256(const uint8_t *p, size_t n, uint8_t out[32]);
typedef struct { uint32_t h[8]; uint64_t n; uint8_t buf[64]; size_t k; } UpdSha;   // streaming SHA-256 (downloads to disk)
void upd_sha256_init(UpdSha *s);
void upd_sha256_update(UpdSha *s, const void *p, size_t n);
void upd_sha256_final(UpdSha *s, uint8_t out[32]);
int upd_hex_decode(const char *hex, uint8_t *out, size_t n);   // exactly 2n hex digits -> 1
void upd_hex(const uint8_t *p, size_t n, char *out);           // out: 2n+1 chars
// 1 = sig is a valid ed25519 signature of msg by pub
int upd_sig_ok(const uint8_t *msg, size_t n, const uint8_t *sig, size_t sig_len, const uint8_t pub[32]);

// b4bcoop-update.txt, the release's update manifest (launch/package.sh writes it, release.yml signs it):
//   b4bcoop-update 1
//   version=0.6.2
//   protocol=2
//   zip=b4bcoop-0.6.2.zip
//   size=685901
//   sha256=<64 hex digits>
typedef struct { char version[40]; int protocol; char zip[96]; uint64_t size; uint8_t sha256[32]; } UpdManifest;
// 1 = sig verifies over txt and every field is present and well formed
int upd_manifest_verify(const char *txt, size_t n, const uint8_t *sig, size_t sig_len, const uint8_t pub[32],
                        UpdManifest *m, char *err, size_t en);
int upd_manifest_parse(const char *txt, size_t n, UpdManifest *m, char *err, size_t en);   // no signature check
// The downloaded zip against a verified manifest: size, SHA-256, and its own signature (b4bcoop-<v>.zip.sig). 1 = ok
int upd_zip_verify(const uint8_t *zip, size_t n, const uint8_t *sig, size_t sig_len, const UpdManifest *m,
                   const uint8_t pub[32], char *err, size_t en);

// Zip entries (stored or deflate; no zip64, no encryption), CRC-checked. fn returns 0 to go on, else stops with it.
typedef int (*UpdZipFn)(const char *name, const uint8_t *data, size_t n, void *ctx);
int upd_zip_each(const uint8_t *zip, size_t n, UpdZipFn fn, void *ctx, char *err, size_t en);   // 0 ok, <0 error
// The zip paths the updater installs: xinput1_3.dll and b4bcoop-*.txt in the game root, the agent
// Gobi/Binaries/Win64/X3DAudio1_7.dll. Never b4bcoop.ini (the player's settings), bans, add-ons or logs.
int upd_install_path(const char *name);

// GitHub release JSON (api.github.com/repos/<o>/<r>/releases/latest or /releases/tags/<tag>)
typedef struct { char name[96]; char url[600]; uint64_t size; } UpdAsset;
typedef struct {
    char tag[48], title[120], page[300];
    char body[4096];   // release notes (truncated)
    UpdAsset assets[16]; int n_assets;
} UpdRelease;
int upd_release_parse(const char *json, size_t n, UpdRelease *r, char *err, size_t en);   // 1 = ok
const UpdAsset *upd_release_asset(const UpdRelease *r, const char *name);

// Semantic versions "1.2.3" / "1.2.3-test.1": <0, 0, >0. Unparsable sorts lowest.
int upd_version_cmp(const char *a, const char *b);
int upd_version_valid(const char *v);
const char *upd_tag_version(const char *tag);   // GitHub tag "v1.2.3" -> "1.2.3" (a tag without the v as it is)

// ---- automatic update before the game starts (the launcher, native/launcher/redirect.c) ----
// A Steam join on the command line: "... +b4bcoop_join steam:<id64> proto:<n> ver:<x.y.z> addons:<p> ..." (presence.c
// writes it). 1 = a b4bcoop join is there; ver / proto = the host's ("" when missing or malformed: an older host).
int upd_join_parse(const char *cmdline, char ver[40], char proto[16]);
// What the launcher asks GitHub for:
//   UPD_AUTO_LATEST  releases/latest (no join, or a join without the host's version)
//   UPD_AUTO_TAG     releases/tags/v<target>: the host's version, when it is newer than ours, or older with another
//                    protocol (the join would be refused otherwise)
//   UPD_AUTO_NONE    nothing: the host runs our version, or an older one with our protocol (the join works as it is)
enum { UPD_AUTO_NONE, UPD_AUTO_LATEST, UPD_AUTO_TAG };
int upd_auto_plan(const char *ours, int our_proto, const char *host_ver, const char *host_proto, char target[40]);
// The release GitHub answered with (its tag): 1 = install it. LATEST: newer than ours. TAG: exactly target, not ours.
int upd_auto_wanted(int plan, const char *ours, const char *target, const char *tag);
// 1 = never install ver by itself: it was put back before (state.txt skip=<ver>: it didn't start properly twice, or
// the player went back from it), also "you went back from <ver> to ..." in result= (written by agents before 0.9.11).
// The Updates tab still installs it on a click.
int upd_auto_blocked(const char *ver, const char *state_skip, const char *state_result);

// ---- add-on shop catalog (#36, docs/investigations/shop.md): catalog.json + catalog.json.sig (ed25519, shop key) ----
//   {"b4bcoop-shop": 1, "updated": "2026-09-27", "addons": [{"id": "casual_joe", "name": "...", "author": "...",
//    "license": "CC0-1.0", "license_url": "...", "version": "1.0", "class": "cosmetic", "kinds": "textures, meshes",
//    "adds": ["outfit casual_joe"], "replaces": [], "description": "...", "size": 123, "sha256": "<64 hex>",
//    "content_id": "<40 hex: pak index SHA1>", "url": "https://...", "thumb": "https://...", "thumb_sha256": "<64 hex>",
//    "min_b4bcoop": "0.8.0"}, ...]}
// Entries that break a rule are skipped (counted in n_bad); unknown keys are ignored (newer catalogs stay readable).
#define SHOP_FORMAT 1
#define SHOP_MAX_ITEMS 256
#define SHOP_MAX_PAK (1024ull << 20)   // largest add-on the shop downloads
#define SHOP_MAX_THUMB (1u << 20)      // largest thumbnail file
#define SHOP_MAX_CATALOG (4u << 20)
typedef struct {
    char id[33];                  // [a-z0-9][a-z0-9_-]*: the add-on's file is <id>.pak in b4bcoop-addons
    char name[96], author[80], license[48], license_url[200], version[32], cls[16], kinds[80], desc[600];
    char adds[300], replaces[300];   // the lists joined with "; " (shown to the player; the files decide what mounts live)
    char url[400], thumb[400], min_version[40], content_id[41];
    uint64_t size;
    uint8_t sha256[32], thumb_sha256[32];
} ShopItem;
typedef struct { int format; char updated[40]; ShopItem *items; int n, n_bad; } ShopCatalog;
int upd_catalog_parse(const char *json, size_t n, ShopCatalog *c, char *err, size_t en);   // 1 = ok; free with upd_catalog_free
int upd_catalog_verify(const char *json, size_t n, const uint8_t *sig, size_t sig_len, const uint8_t pub[32],
                       ShopCatalog *c, char *err, size_t en);
void upd_catalog_free(ShopCatalog *c);
int upd_shop_id_ok(const char *id);
