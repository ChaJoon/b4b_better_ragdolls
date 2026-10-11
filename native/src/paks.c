// The engine's pak layer (model mods, epic #23; spikes #16 extract, #17 mount; add-ons #20).
// docs/investigations/model-mods-paks.md, docs/investigations/addons.md.
// Both builds (add-ons, addons.c):
//  - FPakPlatformFile::Initialize is hooked from DllMain (before the engine creates the platform file chain); after
//    the retail paks are mounted we mount the enabled add-on paks with a higher read order than any retail pak, so
//    their files win. Our paks (unsigned, no .sig) are exempted from the three signature paths, by pak identity only
//    (the exact paths we passed to Mount): FPakPlatformFile::bSigned cleared for just their Mount call (sync FPakFile
//    reader), GetPakSignatureFile returns "none" for them without the pak-corrupt broadcast, and the precacher skips
//    its chunk-hash check for their async reads. Retail paks keep every check. Player builds install no hook at all
//    when there is no add-on to mount.
// Dev builds only:
//  - ini `modpaks=<windows dir>`: every *.pak in it is mounted raw (no add-on metadata, after the add-ons).
//  - `paks` lists mounted paks (read order, file); `mountpak <path> [order] [signed]` mounts one at runtime (game
//    thread; `signed` = no exemption, like a retail pak: an unsigned pak then dies with "Corrupt file").
//  - `dumpassets <glob> [outdir]` enumerates the pak directory index (IPlatformFile::IterateDirectory) and writes
//    every matching file as the engine reads it (IPlatformFile::OpenRead: decrypted, decompressed) to outdir
//    (default ~/.local/share/b4b-coop/extract). Runs on a worker thread; `dumpassets status` shows progress.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include "MinHook.h"
#include "ue.h"
#include "log.h"
#include "cmds.h"

// FPakPlatformFile (vtable 0x145BBD358). IPlatformFile slots match stock 4.25 up to OpenRead; one extra slot before
// OpenReadNoBuffering shifts the rest by one (OpenWrite 27, GetStatData 31, IterateDirectory(Visitor&) 33).
#define ADDR_PAKPF_VTBL     VA(0x145BBD358ull)
#define ADDR_PAKPF_INIT     VA(0x143912700ull)   // bool Initialize(this, IPlatformFile *Inner, const TCHAR *CmdLine)
#define ADDR_PAKPF_MOUNT    VA(0x143913E60ull)   // bool Mount(this, const TCHAR *Pak, uint32 Order, const TCHAR *MountPoint, bool bLoadIndex)
#define ADDR_PAKPF_OPENREAD VA(0x143916380ull)   // vtbl[24] IFileHandle *OpenRead(this, const TCHAR *, bool bAllowWrite)
#define ADDR_PAKPF_ITERDIR  VA(0x143900B80ull)   // vtbl[33] bool IterateDirectory(this, const TCHAR *Dir, FDirectoryVisitor &)
#define VT_FILEEXISTS 14
#define VT_OPENREAD   24
#define VT_ITERDIR    33
#define PAKPF_PAKFILES 0x10   // TArray<FPakListEntry {uint32 ReadOrder; FPakFile *}> (16 bytes each)
#define PAKPF_BSIGNED  0x30   // bool bSigned: new FPakFile(..., bSigned) in Mount (0x143913F74)
#define PAKFILE_NAME   0x08   // FPakFile::PakFilename (FString)
// IFileHandle: 0 scalar deleting dtor, 1 Tell, 2 Seek, 3 SeekFromEnd, 4 Read, 5 Write, 6 Flush, 7 Truncate, 8 Size
#define FH_READ 4
#define FH_SIZE 8
// FPakPlatformFile::GetPakSignatureFile(out TSharedPtr, const TCHAR *Pak): loads <pak>.sig and RSA-validates it; a missing
// or bad .sig broadcasts the pak-corrupt delegate, and the game's handler makes FEngineLoop::Tick die with
// "Corrupt file: <pak>" (LogEngine Fatal). The precacher calls it for every pak it registers (RegisterPakFile
// 0x1439048E5), signed or not; for our paks we return an empty pointer without broadcasting.
#define ADDR_GETPAKSIG      VA(0x143903560ull)
// FPakPrecacher (async reads, pakcache.Enable): its signature checks are global (bEnableSignatureChecks +0x2C0) and
// load every pak's .sig into FPakData::Signatures; a pak without one crashes in DoSignatureCheck (null ChunkHashes).
// The read-complete lambda {Precacher*, int32 IndexToFill, bool bDoCheck} (0x143923BE0) picks StartSignatureCheck or
// NewRequestsToLowerComplete; we clear bDoCheck for reads of our own paks only.
#define ADDR_PRECACHE_CB    VA(0x143923BE0ull)   // void Lambda::operator()(bool *bWasCanceled, IAsyncReadRequest **)
#define ADDR_DOSIGCHECK_MID VA(0x143908BB3ull)   // FPakPrecacher::DoSignatureCheck: the pak-index lookup we mirror
#define PC_REQ_BLOCK(i)   (0x170 + (i) * 0x20)   // RequestsToLower[i].BlockIndex (int32)
#define PC_BLOCKS         0xE0                   // CacheBlockAllocator items (0x30 bytes), index & ~[+0x104]
#define PC_BLOCK_MASK     0x104
#define PC_PAKDATA        0xA8                   // CachedPakData (0x98 bytes each)
#define PAKDATA_SIZE      0x98
#define PAKDATA_NAME      0x2C                   // FName Name (RegisterPakFile 0x14390451E)
#define PAKDATA_SIGS      0x88                   // TSharedPtr<const FPakSignatureFile> Signatures (object pointer)
#define MODPAKS_ORDER 3000    // dev modpaks=; add-ons use ADDON_ORDER 1000+ (addons.c). Retail: 4 (+100 per _P
                              // patch level); higher read order wins in FindFileInPakFiles

static const uint8_t SIG_INIT[] = {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20,0x55,
                                   0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
static const uint8_t SIG_MOUNT[] = {0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,
                                    0x48,0x8d,0xac,0x24,0x00,0xff,0xff,0xff};

static const uint8_t SIG_PRECACHE_CB[] = {0x48,0x8b,0x01,0x4d,0x8b,0x00,0x0f,0xb6,0x12,0x80,0xb8,0xc0,0x02,0x00,0x00,0x00,
                                          0x74,0x0f,0x80,0x79,0x0c,0x00,0x74,0x09,0x44,0x8b,0x49,0x08};
static const uint8_t SIG_DOSIGCHECK_MID[] = {0x49,0x63,0x88,0x70,0x01,0x00,0x00,0x48,0x23,0xd1,0x49,0xc1,0xfd,0x10,0x4c,0x89,
    0x6c,0x24,0x70,0x48,0x8d,0x04,0x52,0x41,0x8b,0xd6,0x48,0xc1,0xe0,0x04,0x48,0x03,0x86,0xe0,0x00,0x00,0x00,0x48,0x8b,0x00,
    0x48,0x8b,0xc8,0x48,0xc1,0xf8,0x10,0x48,0xc1,0xe9,0x30,0x4d,0x3b,0xea,0x48,0x89,0x4c,0x24,0x68,0x44,0x8b,0xf8,0x4d,0x0f,
    0x4e,0xd5,0x4d,0x85,0xd2,0x7e,0x43,0x48,0x8b,0x86,0xa8,0x00,0x00,0x00,0x4c,0x8d,0x45,0x70,0x48,0x69,0xc9,0x98,0x00,0x00,
    0x00,0x48,0x8b,0x8c,0x08,0x88,0x00,0x00,0x00,0x4c,0x8b,0x49,0x30};

static const uint8_t SIG_GETPAKSIG[] = {0x40,0x55,0x53,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0xf8,0xfe,0xff,0xff,0x48,0x81,
                                        0xec,0x08,0x02,0x00,0x00,0x48,0x8b,0x05};
typedef void **(*GetPakSigFn)(void **out, const wchar_t *pak);
static GetPakSigFn orig_getpaksig;
static volatile LONG g_sig_skips;
typedef void (*PrecacheCbFn)(void *lambda, uint8_t *canceled, void **request);
static PrecacheCbFn orig_precache_cb;
#define MAX_MODPAKS (MAX_ADDONS + 32)
static char g_modpak_names[MAX_MODPAKS][520];   // lower-case, backslashes: what we passed to Mount
static volatile LONG g_nmodpaks, g_exempt_reads;

typedef uint8_t (*InitFn)(void *pf, void *inner, const wchar_t *cmdline);
typedef uint8_t (*MountFn)(void *pf, const wchar_t *pak, uint32_t order, const wchar_t *mountpoint, uint8_t load_index);
typedef void *(*OpenReadFn)(void *pf, const wchar_t *name, uint8_t allow_write);
typedef uint8_t (*IterFn)(void *pf, const wchar_t *dir, void *visitor);
typedef uint8_t (*ExistsFn)(void *pf, const wchar_t *name);
typedef uint8_t (*ReadFn)(void *fh, uint8_t *dst, int64_t n);
typedef int64_t (*SizeFn)(void *fh);
typedef void (*DtorFn)(void *fh, uint32_t flags);

static InitFn orig_init;
static void *g_pf;              // the FPakPlatformFile
static int g_ok;                // signatures verified and the Initialize hook in (g_pf gets known)
static int g_exempt;            // the two signature exemption hooks are in (needed before any of our paks is mounted)
#ifndef B4B_RELEASE
static char g_modpaks[520];     // ini modpaks=<windows dir> (dev)
#endif

#define VT(o) (*(void ***)(o))

static int pf_ok(void) { return g_ok && g_pf && VT(g_pf) == (void **)ADDR_PAKPF_VTBL; }

// Mount one pak with bSigned off for this call only (the retail paks were mounted signed and stay so).
// keep_signed (dev experiment): mount it like a retail pak, with the signature checks.
static int mount_pak(const wchar_t *path, uint32_t order, int keep_signed) {
    volatile uint8_t *signed_flag = (uint8_t *)g_pf + PAKPF_BSIGNED;
    uint8_t was = *signed_flag;
    if (!keep_signed) *signed_flag = 0;
    LONG k = keep_signed ? -1 : InterlockedIncrement(&g_nmodpaks) - 1;   // before Mount: reads may start at once
    if (k >= MAX_MODPAKS) { InterlockedDecrement(&g_nmodpaks); *signed_flag = was; LOG("paks: too many mod paks"); return 0; }
    if (k >= 0) {
        char *d = g_modpak_names[k];
        snprintf(d, sizeof g_modpak_names[0], "%ls", path);
        for (char *c = d; *c; c++) *c = (char)tolower(*c == '/' ? '\\' : *c);
    }
    int ok = ((MountFn)ADDR_PAKPF_MOUNT)(g_pf, path, order, NULL, 1);
    *signed_flag = was;
    LOG("paks: mount %ls order %u -> %s (%s)", path, order, ok ? "ok" : "FAILED", keep_signed ? "signed" : "unsigned");
    return ok;
}

#ifndef B4B_RELEASE
static void mount_dir(const char *dir) {
    wchar_t pat[600];
    swprintf(pat, 600, L"%hs\\*.pak", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { LOG("paks: modpaks=%s: no *.pak (err %lu)", dir, GetLastError()); return; }
    uint32_t n = 0;
    do {
        wchar_t full[800];
        swprintf(full, 800, L"%hs\\%ls", dir, fd.cFileName);
        mount_pak(full, MODPAKS_ORDER + n++, 0);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
#endif

// Add-ons (addons.c): only from the Initialize hook, right after the retail paks.
int paks_mount_unsigned(const wchar_t *path, uint32_t order) { return pf_ok() && g_exempt && mount_pak(path, order, 0); }

static int is_mod_pak(char *path) {   // path is folded in place (lower case, backslashes)
    for (char *c = path; *c; c++) *c = (char)tolower(*c == '/' ? '\\' : *c);
    for (LONG i = 0; i < g_nmodpaks; i++) if (!strcmp(path, g_modpak_names[i])) return 1;
    return 0;
}
static int is_mod_pak_name(FName n) { char b[520]; return is_mod_pak((char *)ue_name(n, b, sizeof b)); }
static int is_mod_pak_path(const wchar_t *w) { char b[520]; snprintf(b, sizeof b, "%ls", w ? w : L""); return is_mod_pak(b); }

static void **getpaksig_detour(void **out, const wchar_t *pak) {
    if (g_nmodpaks && is_mod_pak_path(pak)) {
        out[0] = out[1] = NULL;   // no signatures: the precacher skips the chunk checks for it (precache_cb_detour)
        if (InterlockedIncrement(&g_sig_skips) <= MAX_MODPAKS) LOG("paks: no .sig lookup for mod pak %ls", pak);
        return out;
    }
    return orig_getpaksig(out, pak);
}

// Async read completed: before the precacher verifies its chunk hashes, skip that for reads of our own paks (they
// have no .sig: FPakData::Signatures is null). Retail paks are untouched. Mirrors DoSignatureCheck's pak lookup.
static void precache_cb_detour(void *lambda, uint8_t *canceled, void **request) {
    uint8_t *pc = *(uint8_t **)lambda;
    int32_t idx = *(int32_t *)((uint8_t *)lambda + 8);
    uint8_t *do_check = (uint8_t *)lambda + 0xC;
    uint8_t *blocks = *(uint8_t **)(pc + PC_BLOCKS);
    if (g_nmodpaks && *do_check && blocks) {
        int64_t blk = (int64_t)*(int32_t *)(pc + PC_REQ_BLOCK(idx)) & (int64_t)(int32_t)~*(uint32_t *)(pc + PC_BLOCK_MASK);
        uint64_t off_and_pak = *(uint64_t *)(blocks + blk * 0x30);
        uint8_t *pak = *(uint8_t **)(pc + PC_PAKDATA) + (off_and_pak >> 48) * PAKDATA_SIZE;
        int ours = !*(void **)(pak + PAKDATA_SIGS) && is_mod_pak_name(*(FName *)(pak + PAKDATA_NAME));
        static volatile LONG warned;
        if (!*(void **)(pak + PAKDATA_SIGS) && !ours && !InterlockedExchange(&warned, 1)) {
            char nb[520];
            LOG("paks: precacher: pak without signatures is not ours: %s", ue_name(*(FName *)(pak + PAKDATA_NAME), nb, sizeof nb));
        }
        if (ours) {
            *do_check = 0;
            if (InterlockedIncrement(&g_exempt_reads) == 1) {
                char nb[520];
                LOG("paks: precacher: first read from a mod pak (%s), chunk signature check skipped", ue_name(*(FName *)(pak + PAKDATA_NAME), nb, sizeof nb));
            }
        }
    }
    orig_precache_cb(lambda, canceled, request);
}

static uint8_t init_detour(void *pf, void *inner, const wchar_t *cmdline) {
    uint8_t r = orig_init(pf, inner, cmdline);
    g_pf = pf;
    LOG("paks: FPakPlatformFile::Initialize -> %d (pf %p, bSigned %d, %d paks)", r, pf, *((uint8_t *)pf + PAKPF_BSIGNED),
        ((TArray *)((char *)pf + PAKPF_PAKFILES))->num);
    if (r && pf_ok()) addons_mount();
#ifndef B4B_RELEASE
    if (r && g_modpaks[0]) mount_dir(g_modpaks);
#endif
    return r;
}

#ifndef B4B_RELEASE
static void load_config(void) {
    FILE *f = fopen(cmds_config_path(), "r");
    char line[600];
    while (f && fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
        if (!strncmp(line, "modpaks=", 8)) snprintf(g_modpaks, sizeof g_modpaks, "%s", line + 8);
    }
    if (f) fclose(f);
}

#endif

static int install_exemptions(void) {   // GetPakSignatureFile + the precacher's chunk check, for our paks only
    if (g_exempt) return 1;
    if (MH_CreateHook((void *)ADDR_PRECACHE_CB, (void *)precache_cb_detour, (void **)&orig_precache_cb) != MH_OK ||
        MH_EnableHook((void *)ADDR_PRECACHE_CB) != MH_OK ||
        MH_CreateHook((void *)ADDR_GETPAKSIG, (void *)getpaksig_detour, (void **)&orig_getpaksig) != MH_OK ||
        MH_EnableHook((void *)ADDR_GETPAKSIG) != MH_OK) return 0;
    g_exempt = 1;
    return 1;
}

// DllMain: hook before the engine builds its platform file chain.
void paks_early_init(void) {
    g_base_delta = (uint64_t)GetModuleHandleW(NULL) - 0x140000000ull;
    int n = addons_scan();   // b4bcoop.ini addons keys, the add-ons folder, addonlist.txt
#ifdef B4B_RELEASE
    // nothing to mount and no add-on shop: the engine's pak code stays untouched. With the shop (shop.c) only the
    // Initialize hook goes in (it just remembers the pak layer); the exemptions follow with the first add-on mounted.
    if (!n && !addons_runtime_wanted()) return;
#else
    load_config();
#endif
    if (memcmp((void *)ADDR_PAKPF_INIT, SIG_INIT, sizeof SIG_INIT) || memcmp((void *)ADDR_PAKPF_MOUNT, SIG_MOUNT, sizeof SIG_MOUNT) ||
        ((void **)ADDR_PAKPF_VTBL)[4] != (void *)ADDR_PAKPF_INIT || ((void **)ADDR_PAKPF_VTBL)[VT_OPENREAD] != (void *)ADDR_PAKPF_OPENREAD ||
        ((void **)ADDR_PAKPF_VTBL)[VT_ITERDIR] != (void *)ADDR_PAKPF_ITERDIR ||
        memcmp((void *)ADDR_PRECACHE_CB, SIG_PRECACHE_CB, sizeof SIG_PRECACHE_CB) ||
        memcmp((void *)ADDR_GETPAKSIG, SIG_GETPAKSIG, sizeof SIG_GETPAKSIG) ||
        memcmp((void *)ADDR_DOSIGCHECK_MID, SIG_DOSIGCHECK_MID, sizeof SIG_DOSIGCHECK_MID)) {
        LOG("paks: signature mismatch (unsupported game build), add-ons off");
        addons_unavailable("unsupported game build");
        return;
    }
    MH_STATUS st = MH_Initialize();
    int need_exempt = n;
#ifndef B4B_RELEASE
    if (g_modpaks[0]) need_exempt = 1;   // dev: mountpak installs them on its first use, like the shop in player builds
#endif
    if ((st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) ||
        (need_exempt && !install_exemptions()) ||
        MH_CreateHook((void *)ADDR_PAKPF_INIT, (void *)init_detour, (void **)&orig_init) != MH_OK ||
        MH_EnableHook((void *)ADDR_PAKPF_INIT) != MH_OK) {
        LOG("paks: hook failed, add-ons off");
        addons_unavailable("hook failed");
        return;
    }
    g_ok = 1;   // only now: an add-on mounted without all three exemptions would be a Fatal
#ifdef B4B_RELEASE
    LOG("paks: FPakPlatformFile::Initialize hooked (%d add-on(s) to mount%s)", n, g_exempt ? "" : "; exemptions with the first shop add-on");
#else
    LOG("paks: FPakPlatformFile::Initialize hooked (%d add-on(s) to mount)%s%s", n, g_modpaks[0] ? ", modpaks=" : "", g_modpaks);
#endif
}

// ---- runtime (game thread; add-on shop, shop.c/addons.c) ----
int paks_runtime_ready(void) { return pf_ok(); }
// A file in the engine's file system (paks and loose files): key = "gobi/content/..." as addons.c normalizes it
int paks_file_exists(const char *key) {
    if (!pf_ok()) return 0;
    wchar_t w[700];
    swprintf(w, 700, L"../../../%hs", key);
    return ((ExistsFn)VT(g_pf)[VT_FILEEXISTS])(g_pf, w) != 0;
}
// Mount one of our paks now (after the engine started): the exemptions go in first if they aren't yet.
int paks_mount_runtime(const wchar_t *path, uint32_t order, char *err, size_t en) {
    if (!pf_ok()) { snprintf(err, en, "the game's pak layer was not found"); return 0; }
    if (!g_exempt && !install_exemptions()) { snprintf(err, en, "hook failed"); LOG("paks: runtime exemption hooks FAILED"); return 0; }
    if (!mount_pak(path, order, 0)) { snprintf(err, en, "the game refused to mount it"); return 0; }
    return 1;
}

// The engine's global `bool (*IsAsyncLoading)()` (UObjectGlobals), read from FAsyncLoadingThread::FlushLoading
// (0x142529160), whose first call is `call [rip+disp]` through it. Its static value is a `return false` stub
// (0x140BA60E0) until the loader replaces it at start. 1 = packages are being loaded (queued or in flight), 0 = none,
// -1 = unknown (pattern changed, or still the stub: callers treat it as "busy").
#define ADDR_FLUSHLOADING_CALL VA(0x14252916Eull)
#define ADDR_ISASYNC_STUB      VA(0x140BA60E0ull)
int paks_async_loading(void) {
    const uint8_t *c = (const uint8_t *)ADDR_FLUSHLOADING_CALL;
    if (c[0] != 0xFF || c[1] != 0x15) return -1;
    uint8_t (**fp)(void) = (uint8_t (**)(void))(c + 6 + *(const int32_t *)(c + 2));
    if (!*fp || (void *)*fp == (void *)ADDR_ISASYNC_STUB) return -1;
    return (*fp)() != 0;
}

// Is any of these packages in memory (also one being loaded, or unreachable and not yet purged)? names: lower case
// "/game/ui/textures/x". Returns the index of the first one found, -1 none. Game thread.
int paks_package_loaded(const char *const *names, int n) {
    static UClass *pkg;
    if (!pkg) pkg = ue_find_class("Package");
    if (!pkg || n <= 0) return n > 0 ? 0 : -1;   // can't tell: as if loaded
    int32_t total = ue_num_objects();
    char b[300];
    for (int32_t i = 0; i < total; i++) {
        UObject *o = ue_object_at(i);
        if (!o || U_CLASS(o) != pkg || U_OUTER(o)) continue;
        ue_obj_name(o, b, sizeof b);
        for (int k = 0; k < n; k++) if (!_stricmp(b, names[k])) return k;
    }
    return -1;
}

#ifndef B4B_RELEASE
// ---- dumpassets ----
typedef struct { wchar_t **v; int n, cap; } WList;
static void wl_add(WList *l, const wchar_t *s) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 1024; l->v = realloc(l->v, l->cap * sizeof *l->v); }
    l->v[l->n++] = _wcsdup(s);
}
static void wl_free(WList *l) { for (int i = 0; i < l->n; i++) free(l->v[i]); free(l->v); memset(l, 0, sizeof *l); }

// FDirectoryVisitor: vtable {scalar deleting dtor, bool Visit(const TCHAR *, bool bIsDirectory)}, flags byte at +8
typedef struct Visitor { void **vtbl; uint64_t flags; WList *files, *dirs; } Visitor;
static void *visitor_dtor(Visitor *v, uint32_t flags) { (void)flags; return v; }
static uint8_t visitor_visit(Visitor *v, const wchar_t *name, uint8_t is_dir) {
    wl_add(is_dir ? v->dirs : v->files, name);
    return 1;
}
static void *visitor_vtbl[2] = {(void *)visitor_dtor, (void *)visitor_visit};

static int wglob(const wchar_t *p, const wchar_t *s) {   // '*' any run (crosses '/'), '?' one char; case-insensitive
    if (!*p) return !*s;
    if (*p == L'*') { for (;; s++) { if (wglob(p + 1, s)) return 1; if (!*s) return 0; } }
    if (!*s) return 0;
    if (*p != L'?' && towlower(*p) != towlower(*s)) return 0;
    return wglob(p + 1, s + 1);
}

static struct {
    volatile LONG running;
    wchar_t glob[520], outdir[520];
    volatile int listed, matched, written, failed;
    volatile int64_t bytes;
    char last[300];
} D;

static void to_engine_path(const char *in, wchar_t *out, size_t n) {
    // /Game/X -> ../../../Gobi/Content/X ; /Engine/X -> ../../../Engine/Content/X ; else as given
    if (!_strnicmp(in, "/Game/", 6)) swprintf(out, n, L"../../../Gobi/Content/%hs", in + 6);
    else if (!_strnicmp(in, "/Engine/", 8)) swprintf(out, n, L"../../../Engine/Content/%hs", in + 8);
    else swprintf(out, n, L"%hs", in);
    for (wchar_t *c = out; *c; c++) if (*c == L'\\') *c = L'/';
}

static int mkdirs_for(wchar_t *path) {   // create parent directories of a file path (backslashes)
    for (wchar_t *c = path + 3; *c; c++) {
        if (*c != L'\\') continue;
        *c = 0; CreateDirectoryW(path, NULL); *c = L'\\';
    }
    return 0;
}

static int dump_one(const wchar_t *name) {
    void *fh = ((OpenReadFn)VT(g_pf)[VT_OPENREAD])(g_pf, name, 0);
    if (!fh) return -1;
    int64_t size = ((SizeFn)VT(fh)[FH_SIZE])(fh);
    const wchar_t *rel = name;
    while (!wcsncmp(rel, L"../", 3)) rel += 3;
    wchar_t out[1200];
    swprintf(out, 1200, L"%ls\\%ls", D.outdir, rel);
    for (wchar_t *c = out; *c; c++) if (*c == L'/') *c = L'\\';
    mkdirs_for(out);
    HANDLE h = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    int rc = 0;
    if (h == INVALID_HANDLE_VALUE) rc = -2;
    static uint8_t buf[1 << 20];
    for (int64_t left = size; rc == 0 && left > 0;) {
        int64_t k = left < (int64_t)sizeof buf ? left : (int64_t)sizeof buf;
        if (!((ReadFn)VT(fh)[FH_READ])(fh, buf, k)) { rc = -3; break; }
        DWORD w; if (!WriteFile(h, buf, (DWORD)k, &w, NULL) || w != k) rc = -4;
        left -= k; D.bytes += k;
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    ((DtorFn)VT(fh)[0])(fh, 1);
    return rc;
}

static DWORD WINAPI dump_thread(LPVOID _) {
    // root directory = glob up to the last '/' before the first wildcard
    wchar_t root[520];
    wcscpy(root, D.glob);
    wchar_t *w = wcspbrk(root, L"*?");
    if (w) { *w = 0; wchar_t *s = wcsrchr(root, L'/'); if (s) s[1] = 0; }
    size_t rl = wcslen(root);
    if (rl > 1 && root[rl - 1] == L'/') root[rl - 1] = 0;
    WList files = {0}, dirs = {0};
    wl_add(&dirs, root);
    Visitor v = {visitor_vtbl, 0, &files, &dirs};
    int nfiles_before;
    for (int i = 0; i < dirs.n; i++) {
        nfiles_before = files.n;
        ((IterFn)VT(g_pf)[VT_ITERDIR])(g_pf, dirs.v[i], &v);
        (void)nfiles_before;
        D.listed = files.n;
    }
    LOG("dumpassets: %ls: %d dirs, %d files under %ls", D.glob, dirs.n, files.n, root);
    // a glob without wildcards naming one file: IterateDirectory lists nothing for it, try it directly
    if (!w && !files.n && ((ExistsFn)VT(g_pf)[VT_FILEEXISTS])(g_pf, D.glob)) wl_add(&files, D.glob);
    for (int i = 0; i < files.n; i++) {
        if (!wglob(D.glob, files.v[i])) continue;
        D.matched++;
        int rc = dump_one(files.v[i]);
        if (rc) { D.failed++; LOG("dumpassets: %ls failed (%d)", files.v[i], rc); }
        else D.written++;
        snprintf(D.last, sizeof D.last, "%ls", files.v[i]);
    }
    LOG("dumpassets: done: %d matched, %d written, %d failed, %lld bytes -> %ls", D.matched, D.written, D.failed,
        (long long)D.bytes, D.outdir);
    wl_free(&files); wl_free(&dirs);
    InterlockedExchange(&D.running, 0);
    return 0;
}

static void default_outdir(wchar_t *out, size_t n) {
    // Proton/Wine: WINEHOMEDIR=\??\Z:\home\<user> (the Unix HOME is not passed through to the Windows env)
    const char *home = getenv("WINEHOMEDIR");
    if (home && !strncmp(home, "\\??\\", 4)) home += 4;
    if (home && *home) {
        swprintf(out, n, L"%hs\\.local\\share\\b4b-coop\\extract", home);
    } else out[0] = 0;   // not under Wine: no default (never write into the game folder)
}

static void cmd_paks(Out *o) {
    if (!pf_ok()) { out_printf(o, "pak platform file not found (signature %s)\n", g_ok ? "ok" : "mismatch"); return; }
    TArray *pk = (TArray *)((char *)g_pf + PAKPF_PAKFILES);
    out_printf(o, "FPakPlatformFile %p bSigned=%d, %d paks (read order: higher wins)\n", g_pf, *((uint8_t *)g_pf + PAKPF_BSIGNED), pk->num);
    for (int i = 0; i < pk->num; i++) {
        uint8_t *e = (uint8_t *)pk->data + i * 16;
        uint8_t *pak = *(uint8_t **)(e + 8);
        FString *fn = (FString *)(pak + PAKFILE_NAME);
        out_printf(o, "  %5u %ls\n", *(uint32_t *)e, fn->num ? fn->data : L"?");
    }
}

int paks_cmd(const char *verb, char *rest, Out *o) {
    if (!strcmp(verb, "paks")) {
        char nm[300];
        if (rest && sscanf(rest, "loaded %299s", nm) == 1) {   // dev: is a package in memory (addons.c live checks)
            for (char *c = nm; *c; c++) *c = (char)tolower((unsigned char)*c);
            const char *v[1] = {nm};
            out_printf(o, "package %s: %s; async loading: %d\n", nm, paks_package_loaded(v, 1) == 0 ? "in memory" : "not loaded", paks_async_loading());
            return 1;
        }
        cmd_paks(o);
        out_printf(o, "async loading now: %d (1 yes, 0 no, -1 unknown)\n", paks_async_loading());
        return 1;
    }
    if (!strcmp(verb, "mountpak")) {
        if (!pf_ok()) { out_printf(o, "pak platform file not found\n"); return 1; }
        char path[520] = "", sig[16] = ""; unsigned order = MODPAKS_ORDER + 500;
        if (!rest || sscanf(rest, "%519s %u %15s", path, &order, sig) < 1) {
            out_printf(o, "usage: mountpak <windows path> [order] [signed]\n"); return 1;
        }
        wchar_t w[520]; swprintf(w, 520, L"%hs", path);
        int keep = !strcmp(sig, "signed");
        if (!keep && !install_exemptions()) { out_printf(o, "exemption hooks failed\n"); return 1; }
        out_printf(o, "mount %s order %u%s: %s\n", path, order, keep ? " signed" : "", mount_pak(w, order, keep) ? "ok" : "FAILED");
        return 1;
    }
    if (!strcmp(verb, "dumpassets")) {
        if (rest && !strncmp(rest, "status", 6)) {
            out_printf(o, "%s: glob %ls listed %d matched %d written %d failed %d bytes %lld last %s\n",
                       D.running ? "running" : "idle", D.glob, D.listed, D.matched, D.written, D.failed, (long long)D.bytes, D.last);
            return 1;
        }
        if (!pf_ok()) { out_printf(o, "pak platform file not found\n"); return 1; }
        char glob[520] = "", outdir[520] = "";
        if (!rest || sscanf(rest, "%519s %519s", glob, outdir) < 1) {
            out_printf(o, "usage: dumpassets <glob, e.g. /Game/Characters/Heroes/*> [windows outdir] | dumpassets status\n");
            return 1;
        }
        if (InterlockedCompareExchange(&D.running, 1, 0)) { out_printf(o, "a dump is already running\n"); return 1; }
        D.listed = D.matched = D.written = D.failed = 0; D.bytes = 0; D.last[0] = 0;
        to_engine_path(glob, D.glob, 520);
        if (outdir[0]) swprintf(D.outdir, 520, L"%hs", outdir); else default_outdir(D.outdir, 520);
        if (!D.outdir[0]) { InterlockedExchange(&D.running, 0); out_printf(o, "no default outdir here: pass one\n"); return 1; }
        CreateThread(NULL, 0, dump_thread, NULL, 0, NULL);
        out_printf(o, "dumping %ls -> %ls (dumpassets status)\n", D.glob, D.outdir);
        return 1;
    }
    return 0;
}
#endif  // !B4B_RELEASE
