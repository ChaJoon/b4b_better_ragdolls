// updcore: see updcore.h. No Windows calls here (native/test/updcore_test.c builds this file for Linux).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "updcore.h"
#include "monocypher-ed25519.h"   // vendor/monocypher (tools/fetch-deps.sh): crypto_ed25519_check
#include "puff.h"                 // vendor/zlib/contrib/puff: inflate

#define ERR(...) do { if (err && en) snprintf(err, en, __VA_ARGS__); return 0; } while (0)

// ---- SHA-256 (FIPS 180-4) ----
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_block(uint32_t h[8], const uint8_t *b) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
void upd_sha256(const uint8_t *p, size_t n, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t i = 0;
    for (; i + 64 <= n; i += 64) sha256_block(h, p + i);
    uint8_t last[128] = {0};
    size_t r = n - i;
    if (r) memcpy(last, p + i, r);
    last[r] = 0x80;
    size_t blocks = r + 9 <= 64 ? 1 : 2;
    uint64_t bits = (uint64_t)n * 8;
    for (int k = 0; k < 8; k++) last[blocks * 64 - 1 - k] = (uint8_t)(bits >> (8 * k));
    for (size_t k = 0; k < blocks; k++) sha256_block(h, last + 64 * k);
    for (int k = 0; k < 8; k++) { out[4 * k] = (uint8_t)(h[k] >> 24); out[4 * k + 1] = (uint8_t)(h[k] >> 16); out[4 * k + 2] = (uint8_t)(h[k] >> 8); out[4 * k + 3] = (uint8_t)h[k]; }
}

void upd_sha256_init(UpdSha *s) {
    static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, h0, sizeof h0); s->n = 0; s->k = 0;
}
void upd_sha256_update(UpdSha *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->n += n;
    if (s->k) {
        size_t t = 64 - s->k < n ? 64 - s->k : n;
        memcpy(s->buf + s->k, p, t); s->k += t; p += t; n -= t;
        if (s->k < 64) return;
        sha256_block(s->h, s->buf); s->k = 0;
    }
    for (; n >= 64; n -= 64, p += 64) sha256_block(s->h, p);
    memcpy(s->buf, p, n); s->k = n;
}
void upd_sha256_final(UpdSha *s, uint8_t out[32]) {
    uint8_t last[128] = {0};
    memcpy(last, s->buf, s->k);
    last[s->k] = 0x80;
    size_t blocks = s->k + 9 <= 64 ? 1 : 2;
    uint64_t bits = s->n * 8;
    for (int k = 0; k < 8; k++) last[blocks * 64 - 1 - k] = (uint8_t)(bits >> (8 * k));
    for (size_t k = 0; k < blocks; k++) sha256_block(s->h, last + 64 * k);
    for (int k = 0; k < 8; k++) { out[4 * k] = (uint8_t)(s->h[k] >> 24); out[4 * k + 1] = (uint8_t)(s->h[k] >> 16); out[4 * k + 2] = (uint8_t)(s->h[k] >> 8); out[4 * k + 3] = (uint8_t)s->h[k]; }
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
int upd_hex_decode(const char *hex, uint8_t *out, size_t n) {
    if (!hex || strlen(hex) != 2 * n) return 0;
    for (size_t i = 0; i < n; i++) {
        int a = hexval(hex[2 * i]), b = hexval(hex[2 * i + 1]);
        if (a < 0 || b < 0) return 0;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return 1;
}
void upd_hex(const uint8_t *p, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = d[p[i] >> 4]; out[2 * i + 1] = d[p[i] & 15]; }
    out[2 * n] = 0;
}

int upd_sig_ok(const uint8_t *msg, size_t n, const uint8_t *sig, size_t sig_len, const uint8_t pub[32]) {
    return sig && sig_len == 64 && crypto_ed25519_check(sig, pub, msg, n) == 0;
}

// ---- versions ----
static int parse_ver(const char *v, unsigned long x[3], const char **pre) {
    const char *p = v;
    for (int i = 0; i < 3; i++) {
        if (*p < '0' || *p > '9') return 0;
        if (*p == '0' && p[1] >= '0' && p[1] <= '9') return 0;   // no leading zeros
        char *e; x[i] = strtoul(p, &e, 10); p = e;
        if (i < 2) { if (*p != '.') return 0; p++; }
    }
    if (*p == '-') {
        if (!p[1]) return 0;
        for (const char *c = p + 1; *c; c++)
            if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '.' || *c == '-')) return 0;
        *pre = p + 1;
        return 1;
    }
    *pre = NULL;
    return *p == 0;
}
int upd_version_valid(const char *v) { unsigned long x[3]; const char *pre; return v && strlen(v) < 40 && parse_ver(v, x, &pre); }
int upd_version_cmp(const char *a, const char *b) {
    unsigned long x[3], y[3]; const char *pa, *pb;
    int va = a && parse_ver(a, x, &pa), vb = b && parse_ver(b, y, &pb);
    if (!va || !vb) return va - vb;
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    if (!pa || !pb) return (pa ? -1 : 0) + (pb ? 1 : 0);   // a release sorts after its pre-releases
    while (*pa && *pb) {   // dot-separated identifiers: numeric ones by value and before alphanumeric ones
        size_t la = strcspn(pa, "."), lb = strcspn(pb, ".");
        int na = strspn(pa, "0123456789") == la, nb = strspn(pb, "0123456789") == lb, c;
        if (na && nb) c = la != lb ? (la < lb ? -1 : 1) : strncmp(pa, pb, la);
        else if (na != nb) c = na ? -1 : 1;
        else { c = strncmp(pa, pb, la < lb ? la : lb); if (!c && la != lb) c = la < lb ? -1 : 1; }
        if (c) return c < 0 ? -1 : 1;
        pa += la; pb += lb;
        if (*pa) pa++;
        if (*pb) pb++;
    }
    return *pa ? 1 : *pb ? -1 : 0;
}

const char *upd_tag_version(const char *tag) { return tag && tag[0] == 'v' ? tag + 1 : tag ? tag : ""; }

// ---- automatic update at the game start (updcore.h) ----
int upd_join_parse(const char *cmdline, char ver[40], char proto[16]) {
    static const char tokname[] = "+b4bcoop_join";
    ver[0] = proto[0] = 0;
    const char *p = cmdline ? strstr(cmdline, tokname) : NULL;
    if (!p) return 0;
    p += sizeof tokname - 1;
    if (*p && *p != ' ' && *p != '\t' && *p != '"') return 0;   // "+b4bcoop_joinx" is something else
    for (;;) {   // the same token rules as presence.c's parse_connect: up to the next +option / -option
        while (*p == ' ' || *p == '\t' || *p == '"') p++;
        if (!*p || *p == '+' || *p == '-') break;
        char tok[160]; size_t k = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '"') { if (k < sizeof tok - 1) tok[k++] = *p; p++; }
        tok[k] = 0;
        if (!strncmp(tok, "ver:", 4) && upd_version_valid(tok + 4)) snprintf(ver, 40, "%s", tok + 4);
        else if (!strncmp(tok, "proto:", 6) && tok[6] && strlen(tok + 6) < 6 && strspn(tok + 6, "0123456789") == strlen(tok + 6))
            snprintf(proto, 16, "%s", tok + 6);
    }
    return 1;
}
int upd_auto_plan(const char *ours, int our_proto, const char *host_ver, const char *host_proto, char target[40]) {
    target[0] = 0;
    if (!host_ver || !upd_version_valid(host_ver)) return UPD_AUTO_LATEST;
    int c = upd_version_cmp(host_ver, ours);
    if (c == 0) return UPD_AUTO_NONE;
    if (c < 0 && (!host_proto || !host_proto[0] || atoi(host_proto) == our_proto)) return UPD_AUTO_NONE;
    snprintf(target, 40, "%s", host_ver);
    return UPD_AUTO_TAG;
}
int upd_auto_wanted(int plan, const char *ours, const char *target, const char *tag) {
    const char *v = upd_tag_version(tag);
    if (!upd_version_valid(v)) return 0;
    if (plan == UPD_AUTO_LATEST) return upd_version_cmp(v, ours) > 0;
    if (plan == UPD_AUTO_TAG) return target && !strcmp(v, target) && strcmp(v, ours);
    return 0;
}
int upd_auto_blocked(const char *ver, const char *state_skip, const char *state_result) {
    if (!ver || !*ver) return 1;
    if (state_skip && !strcmp(state_skip, ver)) return 1;
    char pre[80];
    snprintf(pre, sizeof pre, "you went back from %s to ", ver);
    return state_result && !strncmp(state_result, pre, strlen(pre));
}

// ---- manifest ----
int upd_manifest_parse(const char *txt, size_t n, UpdManifest *m, char *err, size_t en) {
    memset(m, 0, sizeof *m);
    if (n > 4096) ERR("update manifest too large");
    char buf[4097];
    memcpy(buf, txt, n); buf[n] = 0;
    if (strlen(buf) != n) ERR("update manifest is not text");
    int got = 0, first = 1;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        size_t l = strlen(line);
        if (l && line[l - 1] == '\r') line[--l] = 0;
        if (first) { if (strcmp(line, "b4bcoop-update 1")) ERR("unknown update manifest format"); first = 0; continue; }
        char *v = strchr(line, '=');
        if (!v) continue;
        *v++ = 0;
        if (!strcmp(line, "version")) { if (!upd_version_valid(v)) ERR("bad version in the update manifest"); snprintf(m->version, sizeof m->version, "%s", v); got |= 1; }
        else if (!strcmp(line, "protocol")) { char *e; long p = strtol(v, &e, 10); if (*e || p < 1 || p > 100000) ERR("bad protocol in the update manifest"); m->protocol = (int)p; got |= 2; }
        else if (!strcmp(line, "zip")) {
            if (strlen(v) >= sizeof m->zip || strspn(v, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_") != strlen(v) || !*v)
                ERR("bad zip name in the update manifest");
            snprintf(m->zip, sizeof m->zip, "%s", v); got |= 4;
        } else if (!strcmp(line, "size")) { char *e; unsigned long long s = strtoull(v, &e, 10); if (*e || !*v || !s || s > UPD_MAX_ZIP) ERR("bad size in the update manifest"); m->size = s; got |= 8; }
        else if (!strcmp(line, "sha256")) { if (!upd_hex_decode(v, m->sha256, 32)) ERR("bad sha256 in the update manifest"); got |= 16; }
    }
    if (first) ERR("empty update manifest");
    if (got != 31) ERR("incomplete update manifest");
    return 1;
}
int upd_manifest_verify(const char *txt, size_t n, const uint8_t *sig, size_t sig_len, const uint8_t pub[32],
                        UpdManifest *m, char *err, size_t en) {
    memset(m, 0, sizeof *m);
    if (!upd_sig_ok((const uint8_t *)txt, n, sig, sig_len, pub)) ERR("the update manifest's signature is not valid (not signed with the b4bcoop release key)");
    return upd_manifest_parse(txt, n, m, err, en);
}
int upd_zip_verify(const uint8_t *zip, size_t n, const uint8_t *sig, size_t sig_len, const UpdManifest *m,
                   const uint8_t pub[32], char *err, size_t en) {
    if (n != m->size) ERR("download incomplete or wrong size (%llu of %llu bytes)", (unsigned long long)n, (unsigned long long)m->size);
    uint8_t h[32];
    upd_sha256(zip, n, h);
    if (memcmp(h, m->sha256, 32)) ERR("download damaged: its SHA-256 does not match the release");
    if (!upd_sig_ok(zip, n, sig, sig_len, pub)) ERR("the download's signature is not valid (not signed with the b4bcoop release key)");
    return 1;
}

// ---- zip ----
static uint32_t crc_tab[256];
static uint32_t crc32_of(const uint8_t *p, size_t n) {
    if (!crc_tab[1])
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_tab[i] = c; }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int safe_name(const char *s) {
    if (!*s || *s == '/' || strchr(s, '\\') || strchr(s, ':')) return 0;
    for (const char *p = s; *p; p++) if ((unsigned char)*p < 0x20) return 0;
    for (const char *p = s; *p; ) {   // no "." or ".." path components
        size_t l = strcspn(p, "/");
        if ((l == 1 && p[0] == '.') || (l == 2 && p[0] == '.' && p[1] == '.')) return 0;
        p += l; if (*p) p++;
    }
    return 1;
}

int upd_zip_each(const uint8_t *z, size_t n, UpdZipFn fn, void *ctx, char *err, size_t en) {
#undef ERR
#define ERR(...) do { if (err && en) snprintf(err, en, __VA_ARGS__); return -1; } while (0)
    if (n < 22 || n > UPD_MAX_ZIP) ERR("not a zip");
    size_t eocd = 0; int found = 0;
    for (size_t i = n - 22 + 1; i-- > 0 && n - i <= 22 + 65535; )
        if (le32(z + i) == 0x06054b50) { eocd = i; found = 1; break; }
    if (!found) ERR("not a zip (no end record)");
    uint32_t count = le16(z + eocd + 10), cd_size = le32(z + eocd + 12), cd_off = le32(z + eocd + 16);
    if (le16(z + eocd + 4) || le16(z + eocd + 6) || count != le16(z + eocd + 8)) ERR("multi-part zips are not supported");
    if (cd_off > eocd || cd_size > eocd - cd_off) ERR("zip directory out of range");
    size_t p = cd_off;
    for (uint32_t k = 0; k < count; k++) {
        if (p + 46 > cd_off + cd_size || le32(z + p) != 0x02014b50) ERR("zip directory damaged");
        uint32_t flags = le16(z + p + 8), method = le16(z + p + 10), crc = le32(z + p + 16), csize = le32(z + p + 20),
                 usize = le32(z + p + 24), nl = le16(z + p + 28), xl = le16(z + p + 30), cl = le16(z + p + 32),
                 lho = le32(z + p + 42);
        if (p + 46 + nl + xl + cl > cd_off + cd_size) ERR("zip directory damaged");
        char name[260];
        if (nl == 0 || nl >= sizeof name) ERR("zip entry name too long");
        memcpy(name, z + p + 46, nl); name[nl] = 0;
        if (strlen(name) != nl || !safe_name(name)) ERR("unsafe zip entry name");
        p += 46 + nl + xl + cl;
        if (name[nl - 1] == '/') continue;   // directory
        if (flags & 1) ERR("encrypted zip entry %s", name);
        if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu || lho == 0xFFFFFFFFu) ERR("zip64 is not supported");
        if (usize > UPD_MAX_FILE) ERR("zip entry %s too large", name);
        if ((size_t)lho + 30 > cd_off || le32(z + lho) != 0x04034b50) ERR("zip entry %s damaged", name);
        size_t data = (size_t)lho + 30 + le16(z + lho + 26) + le16(z + lho + 28);
        if (data > cd_off || csize > cd_off - data) ERR("zip entry %s out of range", name);
        uint8_t *out = malloc(usize ? usize : 1);
        if (!out) ERR("out of memory");
        if (method == 0) {
            if (csize != usize) { free(out); ERR("zip entry %s damaged", name); }
            memcpy(out, z + data, usize);
        } else if (method == 8) {
            unsigned long dl = usize, sl = csize;
            int r = puff(out, &dl, z + data, &sl);
            if (r || dl != usize) { free(out); ERR("zip entry %s damaged (inflate %d)", name, r); }
        } else { free(out); ERR("zip entry %s: compression %u not supported", name, method); }
        if (crc32_of(out, usize) != crc) { free(out); ERR("zip entry %s damaged (CRC)", name); }
        int r = fn ? fn(name, out, usize, ctx) : 0;
        free(out);
        if (r) return r;
    }
    return 0;
}
#undef ERR
#define ERR(...) do { if (err && en) snprintf(err, en, __VA_ARGS__); return 0; } while (0)

int upd_install_path(const char *s) {
    if (!strcmp(s, "xinput1_3.dll") || !strcmp(s, "Gobi/Binaries/Win64/X3DAudio1_7.dll")) return 1;
    size_t l = strlen(s);
    return l > 12 && l < 64 && !strncmp(s, "b4bcoop-", 8) && !strcmp(s + l - 4, ".txt") &&
           strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_") == l;
}

// ---- JSON (just enough for a GitHub release object) ----
typedef struct { const char *p, *e; int depth; } J;
static void ws(J *j) { while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++; }
static void put_utf8(char *out, size_t n, size_t *k, uint32_t c) {
    char b[4]; int m;
    if (c < 0x80) { b[0] = (char)c; m = 1; }
    else if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 63)); m = 2; }
    else if (c < 0x10000) { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | (c >> 6 & 63)); b[2] = (char)(0x80 | (c & 63)); m = 3; }
    else { b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | (c >> 12 & 63)); b[2] = (char)(0x80 | (c >> 6 & 63)); b[3] = (char)(0x80 | (c & 63)); m = 4; }
    if (out && *k + m < n) { memcpy(out + *k, b, m); *k += m; }
}
static int hex4(const char *p, uint32_t *v) {
    *v = 0;
    for (int i = 0; i < 4; i++) { int h = hexval(p[i]); if (h < 0) return 0; *v = *v << 4 | (uint32_t)h; }
    return 1;
}
// a string at j->p (the opening quote); out may be NULL (skip); truncates to n-1 bytes
static int jstr(J *j, char *out, size_t n) {
    size_t k = 0;
    if (j->p >= j->e || *j->p != '"') return 0;
    j->p++;
    while (j->p < j->e && *j->p != '"') {
        char c = *j->p++;
        if ((unsigned char)c < 0x20) return 0;
        if (c != '\\') { if (out && k + 1 < n) out[k++] = c; continue; }
        if (j->p >= j->e) return 0;
        c = *j->p++;
        uint32_t u;
        switch (c) {
        case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break; case 'b': c = '\b'; break;
        case 'f': c = '\f'; break; case '"': case '\\': case '/': break;
        case 'u':
            if (j->e - j->p < 4 || !hex4(j->p, &u)) return 0;
            j->p += 4;
            if (u >= 0xD800 && u < 0xDC00 && j->e - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                uint32_t lo;
                if (hex4(j->p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); j->p += 6; }
            }
            if (u >= 0xD800 && u < 0xE000) u = 0xFFFD;
            put_utf8(out, n, &k, u);
            continue;
        default: return 0;
        }
        if (out && k + 1 < n) out[k++] = c;
    }
    if (j->p >= j->e) return 0;
    j->p++;
    if (out && n) out[k] = 0;
    return 1;
}
static int jskip(J *j);
static int jnum(J *j, uint64_t *v) {
    const char *s = j->p;
    uint64_t x = 0; int digits = 0;
    if (j->p < j->e && *j->p == '-') j->p++;
    while (j->p < j->e && ((*j->p >= '0' && *j->p <= '9') || *j->p == '.' || *j->p == 'e' || *j->p == 'E' || *j->p == '+' || *j->p == '-')) {
        if (*j->p >= '0' && *j->p <= '9' && digits < 19) { x = x * 10 + (uint64_t)(*j->p - '0'); digits++; }
        j->p++;
    }
    if (v) *v = x;
    return j->p > s;
}
static int jskip(J *j) {
    ws(j);
    if (j->p >= j->e) return 0;
    char c = *j->p;
    if (c == '"') return jstr(j, NULL, 0);
    if (c == '{' || c == '[') {
        if (++j->depth > 64) return 0;
        char close = c == '{' ? '}' : ']';
        j->p++; ws(j);
        if (j->p < j->e && *j->p == close) { j->p++; j->depth--; return 1; }
        for (;;) {
            if (c == '{') { ws(j); if (!jstr(j, NULL, 0)) return 0; ws(j); if (j->p >= j->e || *j->p++ != ':') return 0; }
            if (!jskip(j)) return 0;
            ws(j);
            if (j->p >= j->e) return 0;
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == close) { j->p++; j->depth--; return 1; }
            return 0;
        }
    }
    static const char *lit[] = {"true", "false", "null"};
    for (int i = 0; i < 3; i++) {
        size_t l = strlen(lit[i]);
        if ((size_t)(j->e - j->p) >= l && !memcmp(j->p, lit[i], l)) { j->p += l; return 1; }
    }
    return jnum(j, NULL);
}
// iterate an object's members: key into k, j->p at the value; the callback consumes the value
typedef int (*MemberFn)(J *j, const char *key, void *ctx);
static int jobject(J *j, MemberFn fn, void *ctx) {
    ws(j);
    if (j->p >= j->e || *j->p != '{') return 0;
    j->p++; ws(j);
    if (j->p < j->e && *j->p == '}') { j->p++; return 1; }
    for (;;) {
        char key[64];
        ws(j);
        if (!jstr(j, key, sizeof key)) return 0;
        ws(j);
        if (j->p >= j->e || *j->p++ != ':') return 0;
        ws(j);
        if (!fn(j, key, ctx)) return 0;
        ws(j);
        if (j->p >= j->e) return 0;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; return 1; }
        return 0;
    }
}
static int jstr_or_null(J *j, char *out, size_t n) {
    if (j->p < j->e && *j->p == '"') return jstr(j, out, n);
    if (n) out[0] = 0;
    return jskip(j);
}
static int asset_member(J *j, const char *key, void *ctx) {
    UpdAsset *a = ctx;
    if (!strcmp(key, "name")) return jstr_or_null(j, a->name, sizeof a->name);
    if (!strcmp(key, "browser_download_url")) return jstr_or_null(j, a->url, sizeof a->url);
    if (!strcmp(key, "size") && j->p < j->e && *j->p >= '0' && *j->p <= '9') return jnum(j, &a->size);
    return jskip(j);
}
static int release_member(J *j, const char *key, void *ctx) {
    UpdRelease *r = ctx;
    if (!strcmp(key, "tag_name")) return jstr_or_null(j, r->tag, sizeof r->tag);
    if (!strcmp(key, "name")) return jstr_or_null(j, r->title, sizeof r->title);
    if (!strcmp(key, "html_url")) return jstr_or_null(j, r->page, sizeof r->page);
    if (!strcmp(key, "body")) return jstr_or_null(j, r->body, sizeof r->body);
    if (!strcmp(key, "assets") && j->p < j->e && *j->p == '[') {
        j->p++; ws(j);
        if (j->p < j->e && *j->p == ']') { j->p++; return 1; }
        for (;;) {
            UpdAsset a; memset(&a, 0, sizeof a);
            if (!jobject(j, asset_member, &a)) return 0;
            if (a.name[0] && r->n_assets < (int)(sizeof r->assets / sizeof *r->assets)) r->assets[r->n_assets++] = a;
            ws(j);
            if (j->p >= j->e) return 0;
            if (*j->p == ',') { j->p++; ws(j); continue; }
            if (*j->p == ']') { j->p++; return 1; }
            return 0;
        }
    }
    return jskip(j);
}
int upd_release_parse(const char *json, size_t n, UpdRelease *r, char *err, size_t en) {
    memset(r, 0, sizeof *r);
    J j = {json, json + n, 0};
    if (!jobject(&j, release_member, r)) ERR("GitHub's reply is not a release (bad JSON)");
    if (!r->tag[0]) ERR("GitHub's reply has no release tag");
    return 1;
}
const UpdAsset *upd_release_asset(const UpdRelease *r, const char *name) {
    for (int i = 0; i < r->n_assets; i++) if (!strcmp(r->assets[i].name, name)) return &r->assets[i];
    return NULL;
}

// ---- add-on shop catalog (updcore.h) ----
int upd_shop_id_ok(const char *id) {
    size_t l = id ? strlen(id) : 0;
    if (!l || l > 32 || !((id[0] >= 'a' && id[0] <= 'z') || (id[0] >= '0' && id[0] <= '9'))) return 0;
    return strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789_-") == l;
}
static int jlist(J *j, char *out, size_t n) {   // ["a", "b"] -> "a; b" (or a plain string)
    if (n) out[0] = 0;
    if (j->p < j->e && *j->p == '"') return jstr(j, out, n);
    if (j->p >= j->e || *j->p != '[') return jskip(j);
    j->p++; ws(j);
    if (j->p < j->e && *j->p == ']') { j->p++; return 1; }
    for (;;) {
        char s[200];
        ws(j);
        if (j->p < j->e && *j->p == '"') {
            if (!jstr(j, s, sizeof s)) return 0;
            size_t k = strlen(out);
            if (s[0] && k + strlen(s) + 3 < n) snprintf(out + k, n - k, "%s%s", k ? "; " : "", s);
        } else if (!jskip(j)) return 0;
        ws(j);
        if (j->p >= j->e) return 0;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == ']') { j->p++; return 1; }
        return 0;
    }
}
typedef struct { ShopItem it; char sha[80], tsha[80]; int has_size; } ItemTmp;
static int item_member(J *j, const char *key, void *ctx) {
    ItemTmp *t = ctx;
    ShopItem *i = &t->it;
    if (!strcmp(key, "id")) return jstr_or_null(j, i->id, sizeof i->id);
    if (!strcmp(key, "name")) return jstr_or_null(j, i->name, sizeof i->name);
    if (!strcmp(key, "author")) return jstr_or_null(j, i->author, sizeof i->author);
    if (!strcmp(key, "license")) return jstr_or_null(j, i->license, sizeof i->license);
    if (!strcmp(key, "license_url")) return jstr_or_null(j, i->license_url, sizeof i->license_url);
    if (!strcmp(key, "version")) return jstr_or_null(j, i->version, sizeof i->version);
    if (!strcmp(key, "class")) return jstr_or_null(j, i->cls, sizeof i->cls);
    if (!strcmp(key, "kinds")) return jstr_or_null(j, i->kinds, sizeof i->kinds);
    if (!strcmp(key, "description")) return jstr_or_null(j, i->desc, sizeof i->desc);
    if (!strcmp(key, "adds")) return jlist(j, i->adds, sizeof i->adds);
    if (!strcmp(key, "replaces")) return jlist(j, i->replaces, sizeof i->replaces);
    if (!strcmp(key, "url")) return jstr_or_null(j, i->url, sizeof i->url);
    if (!strcmp(key, "thumb")) return jstr_or_null(j, i->thumb, sizeof i->thumb);
    if (!strcmp(key, "min_b4bcoop")) return jstr_or_null(j, i->min_version, sizeof i->min_version);
    if (!strcmp(key, "content_id")) return jstr_or_null(j, i->content_id, sizeof i->content_id);
    if (!strcmp(key, "sha256")) return jstr_or_null(j, t->sha, sizeof t->sha);
    if (!strcmp(key, "thumb_sha256")) return jstr_or_null(j, t->tsha, sizeof t->tsha);
    if (!strcmp(key, "size") && j->p < j->e && *j->p >= '0' && *j->p <= '9') { t->has_size = 1; return jnum(j, &i->size); }
    return jskip(j);
}
static int url_ok(const char *u) {   // https:// (http:// only for the dev test server; the downloader refuses it in player builds)
    if (strncmp(u, "https://", 8) && strncmp(u, "http://127.0.0.1", 16)) return 0;
    for (const unsigned char *c = (const unsigned char *)u; *c; c++) if (*c <= 32 || *c >= 127 || *c == '"' || *c == '\\') return 0;
    return strlen(u) < 399;
}
static int item_ok(ItemTmp *t) {
    ShopItem *i = &t->it;
    if (!upd_shop_id_ok(i->id) || !i->name[0] || !i->license[0] || !url_ok(i->url)) return 0;
    if (!t->has_size || !i->size || i->size > SHOP_MAX_PAK || !upd_hex_decode(t->sha, i->sha256, 32)) return 0;
    if (strcmp(i->cls, "cosmetic") && strcmp(i->cls, "gameplay")) snprintf(i->cls, sizeof i->cls, "unknown");
    if (i->min_version[0] && !upd_version_valid(i->min_version)) return 0;
    if (i->thumb[0] && (!url_ok(i->thumb) || !upd_hex_decode(t->tsha, i->thumb_sha256, 32))) i->thumb[0] = 0;   // no picture
    if (i->content_id[0] && (strlen(i->content_id) != 40 || strspn(i->content_id, "0123456789abcdef") != 40)) i->content_id[0] = 0;
    return 1;
}
static int catalog_member(J *j, const char *key, void *ctx) {
    ShopCatalog *c = ctx;
    if (!strcmp(key, "b4bcoop-shop") && j->p < j->e && *j->p >= '0' && *j->p <= '9') { uint64_t f; if (!jnum(j, &f)) return 0; c->format = (int)(f > 1000 ? 1000 : f); return 1; }
    if (!strcmp(key, "updated")) return jstr_or_null(j, c->updated, sizeof c->updated);
    if (!strcmp(key, "addons") && j->p < j->e && *j->p == '[') {
        j->p++; ws(j);
        if (j->p < j->e && *j->p == ']') { j->p++; return 1; }
        for (;;) {
            ItemTmp *t = calloc(1, sizeof *t);
            if (!t) return 0;
            ws(j);
            if (!jobject(j, item_member, t)) { free(t); return 0; }
            int dup = 0;
            for (int k = 0; k < c->n && !dup; k++) dup = !strcmp(c->items[k].id, t->it.id);
            if (item_ok(t) && !dup && c->n < SHOP_MAX_ITEMS) c->items[c->n++] = t->it;
            else c->n_bad++;
            free(t);
            ws(j);
            if (j->p >= j->e) return 0;
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == ']') { j->p++; return 1; }
            return 0;
        }
    }
    return jskip(j);
}
int upd_catalog_parse(const char *json, size_t n, ShopCatalog *c, char *err, size_t en) {
    memset(c, 0, sizeof *c);
    if (n > SHOP_MAX_CATALOG) ERR("add-on list too large");
    c->items = calloc(SHOP_MAX_ITEMS, sizeof *c->items);
    if (!c->items) ERR("out of memory");
    J j = {json, json + n, 0};
    if (!jobject(&j, catalog_member, c)) { upd_catalog_free(c); ERR("the add-on list is damaged (bad JSON)"); }
    if (!c->format) { upd_catalog_free(c); ERR("not a b4bcoop add-on list"); }
    if (c->format > SHOP_FORMAT) { upd_catalog_free(c); ERR("the add-on list needs a newer b4bcoop (Updates tab)"); }
    return 1;
}
int upd_catalog_verify(const char *json, size_t n, const uint8_t *sig, size_t sig_len, const uint8_t pub[32],
                       ShopCatalog *c, char *err, size_t en) {
    memset(c, 0, sizeof *c);
    if (!upd_sig_ok((const uint8_t *)json, n, sig, sig_len, pub)) ERR("the add-on list's signature is not valid (not signed with the add-on shop's key)");
    return upd_catalog_parse(json, n, c, err, en);
}
void upd_catalog_free(ShopCatalog *c) { free(c->items); c->items = NULL; c->n = 0; }
