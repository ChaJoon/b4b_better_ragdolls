// Unit tests for the updater's checks (native/src/updcore.c), run on Linux by native/test/run.sh (also in CI):
// SHA-256 vectors, ed25519 signatures (Monocypher-made and, with run.sh's files, OpenSSL-made like release.yml's),
// the manifest, the download check (bad signature, bad hash, truncated), the zip reader, the install filter, the
// GitHub release JSON and version order, and the launcher's automatic-update decisions.
//   updcore_test [<dir with run.sh fixtures>]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "updcore.h"
#include "signkeys.h"
#include "monocypher-ed25519.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint8_t *slurp(const char *dir, const char *name, size_t *n) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) { printf("FAIL: missing fixture %s\n", p); fails++; *n = 0; return NULL; }
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(l + 1);
    *n = fread(b, 1, l, f); b[*n] = 0;
    fclose(f);
    return b;
}

static void sha_hex(const char *s, size_t n, char *out) { uint8_t h[32]; upd_sha256((const uint8_t *)s, n, h); upd_hex(h, 32, out); }

static void test_sha256(void) {
    char h[65];
    sha_hex("", 0, h); CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    sha_hex("abc", 3, h); CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";   // 56 bytes: padding spills a block
    sha_hex(m, strlen(m), h); CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    char *a = malloc(1000000); memset(a, 'a', 1000000);
    sha_hex(a, 1000000, h); CHECK(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    free(a);
}

static void test_versions(void) {
    CHECK(upd_version_cmp("0.6.2", "0.6.1") > 0);
    CHECK(upd_version_cmp("0.6.1", "0.6.1") == 0);
    CHECK(upd_version_cmp("0.10.0", "0.9.9") > 0);
    CHECK(upd_version_cmp("1.0.0", "0.99.99") > 0);
    CHECK(upd_version_cmp("0.6.2-test", "0.6.2") < 0);
    CHECK(upd_version_cmp("0.6.2-test", "0.6.1") > 0);
    CHECK(upd_version_cmp("1.0.0-alpha", "1.0.0-alpha.1") < 0);
    CHECK(upd_version_cmp("1.0.0-alpha.1", "1.0.0-alpha.beta") < 0);
    CHECK(upd_version_cmp("1.0.0-beta.2", "1.0.0-beta.11") < 0);
    CHECK(upd_version_cmp("1.0.0-rc.1", "1.0.0") < 0);
    CHECK(upd_version_valid("0.6.2") && upd_version_valid("0.6.2-test.1"));
    CHECK(!upd_version_valid("v0.6.2") && !upd_version_valid("0.6") && !upd_version_valid("0.06.1") && !upd_version_valid("0.6.1-")
          && !upd_version_valid("0.6.1 ") && !upd_version_valid(""));
    CHECK(upd_version_cmp("garbage", "0.0.1") < 0);
}

static void test_install_filter(void) {
    CHECK(upd_install_path("xinput1_3.dll"));
    CHECK(upd_install_path("Gobi/Binaries/Win64/X3DAudio1_7.dll"));
    CHECK(upd_install_path("b4bcoop-README.txt") && upd_install_path("b4bcoop-COMMANDS.txt") && upd_install_path("b4bcoop-LICENSE.txt"));
    CHECK(!upd_install_path("Gobi/Binaries/Win64/b4bcoop.ini"));     // the player's settings: never
    CHECK(!upd_install_path("Gobi/Binaries/Win64/b4bcoop-bans.txt"));
    CHECK(!upd_install_path("b4bcoop-addons/x.pak") && !upd_install_path("b4bcoop-addons/addonlist.txt"));
    CHECK(!upd_install_path("Gobi/Binaries/Win64/dwmapi.dll") && !upd_install_path("Back4Blood.exe"));
    CHECK(!upd_install_path("b4bcoop-../x.txt") && !upd_install_path("b4bcoop-a/b.txt") && !upd_install_path("../b4bcoop-x.txt"));
}

static void keypair(uint8_t sk[64], uint8_t pk[32], uint8_t seed_byte) {
    uint8_t seed[32]; memset(seed, seed_byte, 32);
    crypto_ed25519_key_pair(sk, pk, seed);
}

static const char *MANIFEST_FMT = "b4bcoop-update 1\nversion=0.6.2\nprotocol=2\nzip=b4bcoop-0.6.2.zip\nsize=%zu\nsha256=%s\n";

static void test_manifest_and_download(void) {
    uint8_t sk[64], pk[32], sk2[64], pk2[32];
    keypair(sk, pk, 7); keypair(sk2, pk2, 9);
    uint8_t zip[3000];
    for (size_t i = 0; i < sizeof zip; i++) zip[i] = (uint8_t)(i * 31 + 7);
    uint8_t h[32]; char hex[65];
    upd_sha256(zip, sizeof zip, h); upd_hex(h, 32, hex);
    char man[400]; int mn = snprintf(man, sizeof man, MANIFEST_FMT, sizeof zip, hex);
    uint8_t msig[64], zsig[64], bad[64];
    crypto_ed25519_sign(msig, sk, (uint8_t *)man, mn);
    crypto_ed25519_sign(zsig, sk, zip, sizeof zip);
    UpdManifest m; char err[200];

    CHECK(upd_manifest_verify(man, mn, msig, 64, pk, &m, err, sizeof err));
    CHECK(!strcmp(m.version, "0.6.2") && m.protocol == 2 && !strcmp(m.zip, "b4bcoop-0.6.2.zip") && m.size == sizeof zip);
    CHECK(upd_zip_verify(zip, sizeof zip, zsig, 64, &m, pk, err, sizeof err));
    // bad signatures
    memcpy(bad, msig, 64); bad[10] ^= 1;
    CHECK(!upd_manifest_verify(man, mn, bad, 64, pk, &m, err, sizeof err) && strstr(err, "signature"));
    CHECK(!upd_manifest_verify(man, mn, msig, 64, pk2, &m, err, sizeof err));             // another key
    CHECK(!upd_manifest_verify(man, mn, msig, 63, pk, &m, err, sizeof err));              // short signature
    CHECK(!upd_manifest_verify(man, mn, NULL, 0, pk, &m, err, sizeof err));               // none
    char man2[400]; memcpy(man2, man, mn); man2[mn - 3] ^= 1;                              // manifest edited
    CHECK(!upd_manifest_verify(man2, mn, msig, 64, pk, &m, err, sizeof err));
    // the zip: signed manifest ok, then bad hash / truncated / bad zip signature
    CHECK(upd_manifest_verify(man, mn, msig, 64, pk, &m, err, sizeof err));
    uint8_t z2[sizeof zip]; memcpy(z2, zip, sizeof zip); z2[1234] ^= 0x40;
    CHECK(!upd_zip_verify(z2, sizeof z2, zsig, 64, &m, pk, err, sizeof err) && strstr(err, "SHA-256"));
    CHECK(!upd_zip_verify(zip, sizeof zip - 1, zsig, 64, &m, pk, err, sizeof err) && strstr(err, "size"));
    CHECK(!upd_zip_verify(zip, 0, zsig, 64, &m, pk, err, sizeof err));
    memcpy(bad, zsig, 64); bad[63] ^= 0x80;
    CHECK(!upd_zip_verify(zip, sizeof zip, bad, 64, &m, pk, err, sizeof err) && strstr(err, "signature"));
    uint8_t zsig2[64]; crypto_ed25519_sign(zsig2, sk2, zip, sizeof zip);                    // signed by someone else
    CHECK(!upd_zip_verify(zip, sizeof zip, zsig2, 64, &m, pk, err, sizeof err));
    // malformed manifests (signature ok, content not)
    const char *badm[] = {
        "b4bcoop-update 2\nversion=0.6.2\nprotocol=2\nzip=a.zip\nsize=1\nsha256=" ,
        "b4bcoop-update 1\nversion=0.6.2\nprotocol=2\nzip=a.zip\nsize=1\n",                          // no sha256
        "b4bcoop-update 1\nversion=v0.6.2\nprotocol=2\nzip=a.zip\nsize=1\nsha256=00",
        "b4bcoop-update 1\nversion=0.6.2\nprotocol=0\nzip=a.zip\nsize=1\nsha256=00",
        "b4bcoop-update 1\nversion=0.6.2\nprotocol=2\nzip=../a.zip\nsize=1\nsha256=00",
        "",
    };
    for (size_t i = 0; i < sizeof badm / sizeof *badm; i++) {
        char t[400]; snprintf(t, sizeof t, "%s%s", badm[i], i == 1 || i == 5 ? "" : "0000000000000000000000000000000000000000000000000000000000000000\n");
        uint8_t s[64]; crypto_ed25519_sign(s, sk, (uint8_t *)t, strlen(t));
        CHECK(!upd_manifest_verify(t, strlen(t), s, 64, pk, &m, err, sizeof err));
    }
    // CRLF manifest is fine
    char crlf[500]; int cn = snprintf(crlf, sizeof crlf, "b4bcoop-update 1\r\nversion=0.6.2\r\nprotocol=2\r\nzip=b4bcoop-0.6.2.zip\r\nsize=%zu\r\nsha256=%s\r\n", sizeof zip, hex);
    CHECK(upd_manifest_parse(crlf, cn, &m, err, sizeof err) && m.size == sizeof zip);
}

typedef struct { int n, dll, ini, installable; char names[16][260]; } ZipSeen;
static int zip_cb(const char *name, const uint8_t *data, size_t n, void *ctx) {
    ZipSeen *s = ctx;
    if (s->n < 16) snprintf(s->names[s->n], 260, "%s", name);
    s->n++;
    if (!strcmp(name, "Gobi/Binaries/Win64/X3DAudio1_7.dll") && n > 2 && data[0] == 'M' && data[1] == 'Z') s->dll = 1;
    if (strstr(name, "b4bcoop.ini")) s->ini = 1;
    if (upd_install_path(name)) s->installable++;
    return 0;
}

static void test_zip(const char *dir) {
    size_t n;
    uint8_t *z = slurp(dir, "release.zip", &n);
    if (!z) return;
    ZipSeen s = {0}; char err[200];
    size_t hn; uint8_t *want = slurp(dir, "release.zip.sha256", &hn);        // sha256sum's answer
    uint8_t h[32]; char hex[65];
    upd_sha256(z, n, h); upd_hex(h, 32, hex);
    CHECK(want && hn >= 64 && !memcmp(hex, want, 64));
    free(want);
    CHECK(upd_zip_each(z, n, zip_cb, &s, err, sizeof err) == 0);
    CHECK(s.n == 6 && s.dll && s.ini && s.installable == 5);   // everything but the ini
    ZipSeen t = {0};
    CHECK(upd_zip_each(z, n - 1, zip_cb, &t, err, sizeof err) < 0);          // truncated: no end record
    CHECK(upd_zip_each(z, n / 2, zip_cb, &t, err, sizeof err) < 0);
    uint8_t *c = malloc(n); memcpy(c, z, n);
    c[200] ^= 0x55;                                                          // inside the first entry's data
    ZipSeen u = {0};
    CHECK(upd_zip_each(c, n, zip_cb, &u, err, sizeof err) < 0 && strstr(err, "damaged"));
    free(c);
    size_t en; uint8_t *evil = slurp(dir, "evil.zip", &en);                  // "../evil.txt" entry
    ZipSeen v = {0};
    if (evil) CHECK(upd_zip_each(evil, en, zip_cb, &v, err, sizeof err) < 0 && strstr(err, "unsafe"));
    size_t sn; uint8_t *st = slurp(dir, "stored.zip", &sn);                  // stored (no compression) works too
    ZipSeen w = {0};
    if (st) CHECK(upd_zip_each(st, sn, zip_cb, &w, err, sizeof err) == 0 && w.n == 1);
    free(z); free(evil); free(st);
}

// OpenSSL-made signatures (run.sh signs with `openssl pkeyutl -sign -rawin`, exactly like release.yml)
static void test_openssl_signatures(const char *dir) {
    size_t pn, mn, msn, zn, zsn;
    uint8_t *pubhex = slurp(dir, "pub.hex", &pn), *man = slurp(dir, "b4bcoop-update.txt", &mn),
            *msig = slurp(dir, "b4bcoop-update.txt.sig", &msn), *zip = slurp(dir, "release.zip", &zn),
            *zsig = slurp(dir, "release.zip.sig", &zsn);
    if (!pubhex || !man || !msig || !zip || !zsig) return;
    while (pn && (pubhex[pn - 1] == '\n' || pubhex[pn - 1] == '\r')) pubhex[--pn] = 0;
    uint8_t pk[32];
    CHECK(upd_hex_decode((char *)pubhex, pk, 32));
    UpdManifest m; char err[200];
    CHECK(upd_manifest_verify((char *)man, mn, msig, msn, pk, &m, err, sizeof err));
    if (fails) printf("  manifest: %s\n", err);
    CHECK(upd_zip_verify(zip, zn, zsig, zsn, &m, pk, err, sizeof err));
    if (fails) printf("  zip: %s\n", err);
    zip[zn / 3] ^= 1;
    CHECK(!upd_zip_verify(zip, zn, zsig, zsn, &m, pk, err, sizeof err));
    free(pubhex); free(man); free(msig); free(zip); free(zsig);
}

static void test_sha_stream(void) {
    char *a = malloc(300001);
    for (int i = 0; i < 300001; i++) a[i] = (char)(i * 31 + i / 7);
    uint8_t one[32], st[32];
    upd_sha256((uint8_t *)a, 300001, one);
    UpdSha s; upd_sha256_init(&s);
    for (size_t o = 0, k = 1; o < 300001; o += k, k = k * 3 % 70001 + 1) upd_sha256_update(&s, a + o, o + k > 300001 ? 300001 - o : k);
    upd_sha256_final(&s, st);
    CHECK(!memcmp(one, st, 32));
    upd_sha256_init(&s); upd_sha256_final(&s, st);
    upd_sha256((uint8_t *)"", 0, one);
    CHECK(!memcmp(one, st, 32));
    free(a);
}

static int read_key(const char *dir, const char *name, uint8_t k[32]) {   // a `sign-release.sh pubhex` file
    size_t n; uint8_t *h = slurp(dir, name, &n);
    if (!h) return 0;
    while (n && (h[n - 1] == '\n' || h[n - 1] == '\r')) h[--n] = 0;
    int ok = upd_hex_decode((char *)h, k, 32);
    free(h);
    return ok;
}

#define H64 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
static void test_catalog(const char *dir) {
    const char *j =
        "{\"b4bcoop-shop\": 1, \"updated\": \"2026-09-27\", \"future\": {\"x\": [1, 2]}, \"addons\": ["
        "{\"id\": \"casual_joe\", \"name\": \"Casual Joe\", \"author\": \"b4bcoop\", \"license\": \"CC0-1.0\","
        " \"class\": \"cosmetic\", \"adds\": [\"outfit casual_joe (mom)\", \"x\"], \"replaces\": [], \"size\": 83436658,"
        " \"sha256\": \"" H64 "\", \"url\": \"https://github.com/o/r/releases/download/t/casual_joe.pak\","
        " \"thumb\": \"https://github.com/o/r/releases/download/t/casual_joe.png\", \"thumb_sha256\": \"" H64 "\","
        " \"min_b4bcoop\": \"0.8.0\", \"content_id\": \"94464dba3971fcf44b75446e4321d03867ea3341\"},"
        "{\"id\": \"Bad Id\", \"name\": \"x\", \"license\": \"MIT\", \"size\": 1, \"sha256\": \"" H64 "\", \"url\": \"https://e/x\"},"
        "{\"id\": \"nolicense\", \"name\": \"x\", \"size\": 1, \"sha256\": \"" H64 "\", \"url\": \"https://e/x\"},"
        "{\"id\": \"plainhttp\", \"name\": \"x\", \"license\": \"MIT\", \"size\": 1, \"sha256\": \"" H64 "\", \"url\": \"http://e/x\"},"
        "{\"id\": \"holly\", \"name\": \"Holly\", \"license\": \"CC-BY-4.0\", \"class\": \"weird\", \"replaces\": \"Holly's portrait\","
        " \"size\": 2, \"sha256\": \"" H64 "\", \"url\": \"http://127.0.0.1:8000/holly.pak\", \"thumb\": \"https://e/t.png\"},"
        "{\"id\": \"holly\", \"name\": \"dup\", \"license\": \"MIT\", \"size\": 1, \"sha256\": \"" H64 "\", \"url\": \"https://e/x\"}"
        "]}";
    ShopCatalog c; char err[200];
    CHECK(upd_catalog_parse(j, strlen(j), &c, err, sizeof err));
    CHECK(c.format == 1 && c.n == 2 && c.n_bad == 4 && !strcmp(c.updated, "2026-09-27"));
    if (c.n == 2) {
        CHECK(!strcmp(c.items[0].id, "casual_joe") && c.items[0].size == 83436658 && c.items[0].sha256[0] == 0x01);
        CHECK(!strcmp(c.items[0].adds, "outfit casual_joe (mom); x") && !c.items[0].replaces[0] && c.items[0].thumb[0]);
        CHECK(!strcmp(c.items[0].min_version, "0.8.0") && !strcmp(c.items[0].cls, "cosmetic") && c.items[0].content_id[0]);
        CHECK(!strcmp(c.items[1].cls, "unknown") && !strcmp(c.items[1].replaces, "Holly's portrait"));
        CHECK(!c.items[1].thumb[0]);   // a thumbnail without its sha256 is dropped
    }
    upd_catalog_free(&c);
    CHECK(!upd_catalog_parse(j, strlen(j) - 3, &c, err, sizeof err));                  // truncated
    CHECK(!upd_catalog_parse("{\"addons\": []}", 14, &c, err, sizeof err));            // no format key
    CHECK(!upd_catalog_parse("{\"b4bcoop-shop\": 2, \"addons\": []}", 33, &c, err, sizeof err) && strstr(err, "newer"));
    CHECK(upd_shop_id_ok("a") && upd_shop_id_ok("mod_ak47-2") && !upd_shop_id_ok("-x") && !upd_shop_id_ok("A") &&
          !upd_shop_id_ok("../x") && !upd_shop_id_ok("") && !upd_shop_id_ok("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    // signed with Monocypher, then an edited byte
    uint8_t sk[64], pk[32], seed[32], sig[64];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 5 + 1);
    crypto_ed25519_key_pair(sk, pk, seed);
    crypto_ed25519_sign(sig, sk, (const uint8_t *)j, strlen(j));
    CHECK(upd_catalog_verify(j, strlen(j), sig, 64, pk, &c, err, sizeof err) && c.n == 2);
    upd_catalog_free(&c);
    char *e = strdup(j); e[40] ^= 1;
    CHECK(!upd_catalog_verify(e, strlen(e), sig, 64, pk, &c, err, sizeof err) && strstr(err, "signature"));
    free(e);
    if (!dir) return;
    size_t cn, sn;   // OpenSSL-signed with the shop key (tools/sign-release.sh sign, what the shop repo's workflow runs)
    uint8_t shop_pk[32], rel_pk[32], *cat = slurp(dir, "catalog.json", &cn), *csig = slurp(dir, "catalog.json.sig", &sn);
    int keys = read_key(dir, "shoppub.hex", shop_pk) && read_key(dir, "pub.hex", rel_pk);
    CHECK(keys);
    if (keys && cat && csig) {
        CHECK(upd_catalog_verify((char *)cat, cn, csig, sn, shop_pk, &c, err, sizeof err) && c.n == 1 && !strcmp(c.items[0].id, "casual_joe"));
        upd_catalog_free(&c);
        // the release key never passes a catalog (whoever holds the shop key can't sign releases, and vice versa)
        CHECK(!upd_catalog_verify((char *)cat, cn, csig, sn, rel_pk, &c, err, sizeof err) && strstr(err, "signature"));
    }
    free(cat); free(csig);
}

// the public keys built into the agent are the committed PEMs, and they are two different keys
static void test_builtin_keys(const char *dir) {
    static const uint8_t rel[32] = {B4B_RELEASE_PUBKEY_BYTES}, shop[32] = {B4B_SHOP_PUBKEY_BYTES};
    uint8_t k[32];
    CHECK(read_key(dir, "release-committed.hex", k) && !memcmp(k, rel, 32));
    CHECK(read_key(dir, "shop-committed.hex", k) && !memcmp(k, shop, 32));
    CHECK(memcmp(rel, shop, 32) != 0);
}

static void test_json(const char *dir) {
    const char *j =
        "{\"url\":\"x\",\"author\":{\"login\":\"a\",\"name\":\"not the release\",\"site_admin\":false},"
        "\"tag_name\":\"v0.6.2\",\"name\":\"b4b-coop v0.6.2\",\"draft\":false,\"prerelease\":false,"
        "\"html_url\":\"https://github.com/actuallydan/b4b-coop/releases/tag/v0.6.2\","
        "\"assets\":[{\"url\":\"u\",\"id\":1,\"name\":\"b4bcoop-0.6.2.zip\",\"label\":null,\"uploader\":{\"login\":\"b\",\"name\":\"x\"},"
        "\"size\":685901,\"digest\":\"sha256:ab\",\"browser_download_url\":\"https://github.com/actuallydan/b4b-coop/releases/download/v0.6.2/b4bcoop-0.6.2.zip\"},"
        "{\"name\":\"b4bcoop-update.txt\",\"size\":120,\"browser_download_url\":\"https://e/m\"}],"
        "\"body\":\"## New\\n- caf\\u00e9 \\\"quoted\\\" \\ud83d\\ude00\\r\\n\",\"reactions\":{\"+1\":2,\"x\":[1,2.5e3,-1,true,null]}}";
    UpdRelease r; char err[200];
    CHECK(upd_release_parse(j, strlen(j), &r, err, sizeof err));
    CHECK(!strcmp(r.tag, "v0.6.2") && !strcmp(r.title, "b4b-coop v0.6.2") && r.n_assets == 2);
    CHECK(!strcmp(r.body, "## New\n- caf\xc3\xa9 \"quoted\" \xf0\x9f\x98\x80\r\n"));
    const UpdAsset *a = upd_release_asset(&r, "b4bcoop-0.6.2.zip");
    CHECK(a && a->size == 685901 && strstr(a->url, "/releases/download/v0.6.2/"));
    CHECK(upd_release_asset(&r, "b4bcoop-update.txt") && !upd_release_asset(&r, "nope"));
    CHECK(!upd_release_parse(j, strlen(j) - 5, &r, err, sizeof err));          // truncated reply
    CHECK(!upd_release_parse("{\"message\":\"Not Found\"}", 23, &r, err, sizeof err));
    CHECK(!upd_release_parse("[]", 2, &r, err, sizeof err));
    size_t n; uint8_t *real = slurp(dir, "release-latest.json", &n);           // a real api.github.com reply (v0.6.1)
    if (real) {
        CHECK(upd_release_parse((char *)real, n, &r, err, sizeof err) && !strcmp(r.tag, "v0.6.1") && upd_release_asset(&r, "SHA256SUMS"));
        free(real);
    }
}

// the launcher's automatic update (native/launcher/redirect.c): what to ask GitHub for, what to install, what never
static void test_auto(void) {
    char v[40], pr[16], t[40];
    // Steam's join string as presence.c writes it, inside the stub's command line
    CHECK(upd_join_parse("\"C:\\B4B\\start_protected_game.exe\" Gobi -SaveToUserDir +b4bcoop_join steam:76561198000000001 proto:3 ver:0.9.12 addons:any", v, pr)
          && !strcmp(v, "0.9.12") && !strcmp(pr, "3"));
    CHECK(upd_join_parse(" Gobi +b4bcoop_join \"steam:1 proto:2 ver:0.6.1-test.1\" -windowed", v, pr) && !strcmp(v, "0.6.1-test.1") && !strcmp(pr, "2"));
    CHECK(upd_join_parse(" +b4bcoop_join steam:1 -foo ver:9.9.9", v, pr) && !v[0] && !pr[0]);     // stops at the next option
    CHECK(upd_join_parse(" +b4bcoop_join steam:1 proto:x ver:v1.2.3", v, pr) && !v[0] && !pr[0]);   // malformed: ignored
    CHECK(upd_join_parse(" +b4bcoop_join steam:1", v, pr) && !v[0] && !pr[0]);                      // an older host
    CHECK(!upd_join_parse(" Gobi -SaveToUserDir ver:1.2.3", v, pr) && !v[0]);
    CHECK(!upd_join_parse(" +b4bcoop_joined ver:1.2.3", v, pr));
    CHECK(!upd_join_parse(NULL, v, pr));
    // the plan
    CHECK(upd_auto_plan("0.9.10", 3, NULL, NULL, t) == UPD_AUTO_LATEST && !t[0]);
    CHECK(upd_auto_plan("0.9.10", 3, "", "3", t) == UPD_AUTO_LATEST);
    CHECK(upd_auto_plan("0.9.10", 3, "0.9.10", "3", t) == UPD_AUTO_NONE);                 // the host's = ours: no check
    CHECK(upd_auto_plan("0.9.10", 3, "0.9.12", "3", t) == UPD_AUTO_TAG && !strcmp(t, "0.9.12"));   // newer: the host's
    CHECK(upd_auto_plan("0.9.10", 3, "1.0.0", "4", t) == UPD_AUTO_TAG && !strcmp(t, "1.0.0"));
    CHECK(upd_auto_plan("0.9.10", 3, "0.9.8", "3", t) == UPD_AUTO_NONE);                  // older, same protocol: joins as is
    CHECK(upd_auto_plan("0.9.10", 3, "0.9.8", "", t) == UPD_AUTO_NONE);                   // protocol unknown: no downgrade
    CHECK(upd_auto_plan("0.9.10", 3, "0.8.0", "2", t) == UPD_AUTO_TAG && !strcmp(t, "0.8.0"));   // older, other protocol
    // what GitHub answered
    CHECK(upd_auto_wanted(UPD_AUTO_LATEST, "0.9.10", "", "v0.9.11"));
    CHECK(upd_auto_wanted(UPD_AUTO_LATEST, "0.9.10", "", "0.10.0"));
    CHECK(!upd_auto_wanted(UPD_AUTO_LATEST, "0.9.10", "", "v0.9.10"));                     // same: nothing
    CHECK(!upd_auto_wanted(UPD_AUTO_LATEST, "0.9.10", "", "v0.9.9"));                      // older: never by itself
    CHECK(upd_auto_wanted(UPD_AUTO_LATEST, "0.9.10-test", "", "v0.9.10"));                 // a release after its pre-release
    CHECK(!upd_auto_wanted(UPD_AUTO_LATEST, "0.9.10", "", "nightly"));
    CHECK(upd_auto_wanted(UPD_AUTO_TAG, "0.9.10", "0.8.0", "v0.8.0"));
    CHECK(!upd_auto_wanted(UPD_AUTO_TAG, "0.9.10", "0.8.0", "v0.8.1"));                    // not the version asked for
    CHECK(!upd_auto_wanted(UPD_AUTO_TAG, "0.9.10", "0.9.10", "v0.9.10"));
    CHECK(!upd_auto_wanted(UPD_AUTO_NONE, "0.9.10", "", "v1.0.0"));
    CHECK(!strcmp(upd_tag_version("v1.2.3"), "1.2.3") && !strcmp(upd_tag_version("1.2.3"), "1.2.3") && !strcmp(upd_tag_version(NULL), ""));
    // versions that were put back are not installed by themselves
    CHECK(upd_auto_blocked("0.9.11", "0.9.11", ""));
    CHECK(!upd_auto_blocked("0.9.12", "0.9.11", ""));
    CHECK(upd_auto_blocked("0.9.11", "", "you went back from 0.9.11 to 0.9.10"));          // an older agent's Go back
    CHECK(!upd_auto_blocked("0.9.1", "", "you went back from 0.9.11 to 0.9.10"));
    CHECK(!upd_auto_blocked("0.9.11", "", "0.9.11 started fine"));
    CHECK(!upd_auto_blocked("0.9.11", NULL, NULL));
    CHECK(upd_auto_blocked("", NULL, NULL));
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : NULL;
    test_sha256();
    test_versions();
    test_auto();
    test_install_filter();
    test_manifest_and_download();
    test_json(dir ? dir : ".");
    test_sha_stream();
    test_catalog(dir);
    if (dir) { test_zip(dir); test_openssl_signatures(dir); test_builtin_keys(dir); }
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails != 0;
}
