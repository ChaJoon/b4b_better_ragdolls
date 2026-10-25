#include <windows.h>
#include "ue.h"
#include <string.h>
#include <stdio.h>

typedef void (*ProcessEventFn)(UObject *, UFunction *, void *);

static const uint8_t SIG_PE[]   = {0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0xf0};
static const uint8_t SIG_TRAVEL[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x30,0x48,0x8b,0x81,0x58,0x0c,0x00,0x00};
static const uint8_t SIG_TICK[] = {0x48,0x8b,0xc4,0x44,0x88,0x40,0x18,0x48,0x89,0x48,0x08,0x55,0x41,0x54,0x41,0x56};

uint64_t g_base_delta;

int ue_init(char *err, size_t errlen) {
    g_base_delta = (uint64_t)GetModuleHandleW(NULL) - 0x140000000ull;
    if (memcmp((void *)ADDR_PROCESSEVENT, SIG_PE, sizeof SIG_PE) ||
        memcmp((void *)ADDR_GAMEENGINETICK, SIG_TICK, sizeof SIG_TICK) ||
        memcmp((void *)ADDR_SETCLIENTTRAVEL, SIG_TRAVEL, sizeof SIG_TRAVEL)) {
        snprintf(err, errlen, "unsupported game build (signature mismatch)");
        return -1;
    }
    return 0;
}

static char *oa(void) { return (char *)ADDR_GUOBJECTARRAY; }
int32_t ue_num_objects(void) { return *(int32_t *)(oa() + 0x3C); }

UObject *ue_object_at(int32_t i) {
    if (i < 0 || i >= ue_num_objects()) return NULL;
    char **chunks = (char **)(*(uint64_t *)(oa() + 0x48) ^ OBJECTS_XOR);
    char *chunk = chunks[i >> 16];
    if (!chunk) return NULL;
    return *(UObject **)(chunk + (i & 0xFFFF) * 0x18 + 8);
}

// FUObjectItem.Flags (EInternalObjectFlags) Unreachable 1<<28 | PendingKill 1<<29: set by the GC's mark phase, the
// object is destroyed over the next frames (incremental purge); RF_BeginDestroyed 0x8000 | RF_FinishDestroyed 0x10000
int ue_object_usable(UObject *o, int32_t i) {
    if (!o || i < 0 || i >= ue_num_objects()) return 0;
    char **chunks = (char **)(*(uint64_t *)(oa() + 0x48) ^ OBJECTS_XOR);
    char *chunk = chunks[i >> 16];
    if (!chunk) return 0;
    char *item = chunk + (i & 0xFFFF) * 0x18;
    if (*(UObject **)(item + 8) != o || (*(uint32_t *)item & (3u << 28))) return 0;
    return !(U_FLAGS(o) & 0x18000u);
}

// FWeakObjectPtr {ObjectIndex, SerialNumber}: the object if that slot still holds the same object
UObject *ue_weak_get(const void *wp) {
    int32_t idx = ((const int32_t *)wp)[0], serial = ((const int32_t *)wp)[1];
    if (idx < 0 || idx >= ue_num_objects() || !serial) return NULL;
    char **chunks = (char **)(*(uint64_t *)(oa() + 0x48) ^ OBJECTS_XOR);
    char *chunk = chunks[idx >> 16];
    if (!chunk) return NULL;
    char *item = chunk + (idx & 0xFFFF) * 0x18;
    return *(int32_t *)(item + 0x10) == serial ? *(UObject **)(item + 8) : NULL;
}

const char *ue_name(FName n, char *buf, size_t len) {
    char **blocks = (char **)(ADDR_NAMEPOOL + 0x10);
    const uint8_t *e = (const uint8_t *)blocks[n.idx >> 18] + (n.idx & 0xFFFF) * 2;
    uint16_t hdr = *(const uint16_t *)e;
    int wide = hdr & 1, l = hdr >> 6;
    size_t k = 0;
    for (int i = 0; i < l && k + 1 < len; i++)
        buf[k++] = wide ? (char)((const uint16_t *)(e + 2))[i] : (char)e[2 + i];
    if (n.num && k + 12 < len) k += snprintf(buf + k, len - k, "_%u", n.num - 1);
    buf[k] = 0;
    return buf;
}

const char *ue_obj_name(UObject *o, char *buf, size_t len) { return ue_name(U_NAME(o), buf, len); }

const char *ue_full_path(UObject *o, char *buf, size_t len) {
    UObject *chain[32]; int n = 0;
    for (UObject *x = o; x && n < 32; x = U_OUTER(x)) chain[n++] = x;
    size_t k = 0; char tmp[256];
    buf[0] = 0;
    for (int i = n - 1; i >= 0 && k + 1 < len; i--)
        k += snprintf(buf + k, len - k, "%s%s", i == n - 1 ? "" : ".", ue_obj_name(chain[i], tmp, sizeof tmp));
    return buf;
}

int ue_is_a(UObject *o, UClass *cls) {
    for (UStruct *s = U_CLASS(o); s; s = US_SUPER(s)) if (s == cls) return 1;
    return 0;
}

static int name_eq(FName n, const char *s) { char b[256]; return strcmp(ue_name(n, b, sizeof b), s) == 0; }

UClass *ue_find_class(const char *name) {
    int32_t n = ue_num_objects();
    for (int32_t i = 0; i < n; i++) {
        UObject *o = ue_object_at(i);
        if (!o || !U_CLASS(o)) continue;
        if (name_eq(U_NAME(o), name) && name_eq(U_NAME(U_CLASS(o)), "Class")) return o;
    }
    return NULL;
}

UObject *ue_find_first_of(const char *class_name) {
    UClass *c = ue_find_class(class_name);
    if (!c) return NULL;
    int32_t n = ue_num_objects();
    for (int32_t i = 0; i < n; i++) {
        UObject *o = ue_object_at(i);
        if (!o || (U_FLAGS(o) & 0x10 /*RF_ClassDefaultObject*/)) continue;
        if (ue_is_a(o, c)) return o;
    }
    return NULL;
}

UFunction *ue_find_function(UClass *cls, const char *name) {
    for (UStruct *s = cls; s; s = US_SUPER(s))
        for (UObject *f = US_CHILDREN(s); f; f = UF_NEXT(f))
            if (name_eq(U_NAME(f), name)) return f;
    return NULL;
}

FField *ue_find_prop(UStruct *st, const char *name) {
    for (UStruct *s = st; s; s = US_SUPER(s))
        for (FField *f = US_CHILDPROPS(s); f; f = FF_NEXT(f))
            if (name_eq(FF_NAME(f), name)) return f;
    return NULL;
}

int32_t ue_prop_offset(UObject *o, const char *name) {
    FField *f = ue_find_prop(U_CLASS(o), name);
    return f ? FP_OFFSET(f) : -1;
}

void *ue_get_ptr(UObject *o, const char *prop) {
    if (!o) return NULL;
    int32_t off = ue_prop_offset(o, prop);
    return off < 0 ? NULL : *(void **)((char *)o + off);
}

void ue_process_event(UObject *o, UFunction *fn, void *params) {
    ((ProcessEventFn)U_VTBL(o)[VTIDX_PROCESSEVENT])(o, fn, params);
}

UObject *ue_engine(void) {
    static UObject *eng;
    if (!eng) eng = ue_find_first_of("GameEngine");
    return eng;
}

UObject *ue_world(void) {
    UObject *vp = ue_get_ptr(ue_engine(), "GameViewport");
    return vp ? ue_get_ptr(vp, "World") : NULL;
}

UObject *ue_local_pc(void) {
    UObject *w = ue_world();
    UObject *gi = w ? ue_get_ptr(w, "OwningGameInstance") : NULL;
    if (!gi) return NULL;
    TArray *lp = (TArray *)((char *)gi + ue_prop_offset(gi, "LocalPlayers"));
    if (lp->num < 1) return NULL;
    return ue_get_ptr(((UObject **)lp->data)[0], "PlayerController");
}

int ue_is_listen_server(UObject *world) {
    UObject *nd = world ? ue_get_ptr(world, "NetDriver") : NULL;
    return nd && !ue_get_ptr(nd, "ServerConnection");
}

int ue_num_clients(UObject *world) {
    UObject *nd = world ? ue_get_ptr(world, "NetDriver") : NULL;
    if (!nd) return 0;
    return ((TArray *)((char *)nd + ue_prop_offset(nd, "ClientConnections")))->num;
}

const char *ue_world_package(UObject *world, char *buf, size_t len) {
    return ue_obj_name(U_OUTER(world), buf, len);
}

// ---- enums (fork: ragdoll's helpers, shared with cheats.c's /give, build 42) ----
// Layout as tools/sdkdump.py (which dumped all 1442 enums) and docs/NOTES.md: FByteProperty::Enum and
// FEnumProperty::UnderlyingProp at +0x78, FEnumProperty::Enum at +0x80; UEnum::Names (TArray<TPair<FName, int64>>,
// 16-byte entries) at +0x48, stock 0x40 + 8 for this engine's bigger UObject. (Ragdoll build 27 read +0x40, CppType's
// length fields, as the array and crashed the game at the first body.)
UObject *ue_prop_enum(FField *p) {
    char t[64], b[96];
    if (!p) return NULL;
    ue_name(*(FName *)FF_CLASS(p), t, sizeof t);
    UObject *e = !strcmp(t, "EnumProperty") ? *(UObject **)((char *)p + 0x80) :
                 !strcmp(t, "ByteProperty") ? *(UObject **)((char *)p + 0x78) : NULL;
    if (!e || U_INDEX(e) < 0 || U_INDEX(e) >= ue_num_objects() || ue_object_at(U_INDEX(e)) != e || !U_CLASS(e)) return NULL;
    ue_obj_name(U_CLASS(e), b, sizeof b);
    return !strcmp(b, "Enum") || !strcmp(b, "UserDefinedEnum") ? e : NULL;
}
const TArray *ue_enum_names(UObject *e) {
    const TArray *names = e ? (const TArray *)((const char *)e + 0x48) : NULL;
    return names && names->data && names->num > 0 && names->num <= 256 && names->max >= names->num ? names : NULL;
}
const char *ue_enum_entry(const TArray *names, int i, char *b, size_t n, int64_t *v) {
    const uint8_t *pr = (const uint8_t *)names->data + (size_t)i * 16;
    ue_name(*(const FName *)pr, b, n);
    if (v) *v = *(const int64_t *)(pr + 8);
    const char *sh = strstr(b, "::");
    return sh ? sh + 2 : b;
}
const char *ue_enum_value_name(FField *p, int64_t v, char *b, size_t n) {
    const TArray *names = ue_enum_names(ue_prop_enum(p));
    for (int i = 0; names && i < names->num; i++) {
        int64_t x;
        const char *s = ue_enum_entry(names, i, b, n, &x);
        if (x == v) { if (s != b) memmove(b, s, strlen(s) + 1); return b; }
    }
    return NULL;
}
