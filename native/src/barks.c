// Fork (ChaJoon/b4b_better_ragdolls, builds 41-42, 45): survivor barks muted on this machine.
//
// Joon (24 Oct) hates the survivors' combat barks ("She shoots, she scores!"): the lines after headshots, melee kills,
// shoves and shooting explosive items; 25 Oct also picking up copper. The game's bark events are Wwise events named
// Dx_B_<Voice>_<Bark>_<NN>; his asset index (b4bmod find, 25 Oct: 539 bark names) and the build 41 log give these
// names: Headshot (175 events), Kill_Bat (melee kills, 49), Shoot_Explosive (25), Bash (184: the line after every
// shove and melee hit; build 41 log, Fist_ and BaseballBat_DamageType hits, 5 ms before it), PickUp_Copper (47) and
// Trigger_Bleed (Tala's alone, 3 lines in TU15's AKB_Dx_Tala_TU15 bank: "Keep bleeding!" after her bow kills, Joon;
// the bow's bleed; no bark in either list is named for bow kills). Build 45 (Joon, 28 Oct): the lines after a horde,
// Horde_Finish (90) and Horde_Finish_Injured (24; the entry matches both, its words in a row). Horde_Start (74),
// Horde_Start_Injured and Horde_Wave (59), the survivors' warnings, stay.
// Not muted by default: Exert_Big (the grunt often just before a Bash; also climbing and the like), Got_Item (other
// pickups), FriendlyFire_Melee / FriendlyFire_Arrow (a survivor hit by a teammate's melee or arrow), Kill_Count_L1..L4
// (kill streaks), Good_Shot (a teammate's praise), Ping_* (pings). Any of them: /dialogue barks mute <list>.
// Build 42: only survivors' lines (heroes and bots: a HeroDialogueComponent) are muted; enemies and NPCs have lines of
// similar names (the cultists' A_Bash / B_Bash) that the list must not reach.
//
// Where: every dialogue line a GobiDialogueComponent plays goes through its virtual "play line" (vtable +0x410,
// 0x141620240, which also shows the subtitle; dialogue.c hooks it for the cutscene radio lines and asks barks_filter
// first). Heroes' dialogue components may override that slot: the first hero seen gets its component's +0x410 checked
// and hooked too if it is another function. What the line is: the play call's parameter block is a game struct we have
// no layout for, so barks_filter looks through it for anything that names the line: FNames, UObjects (an AkAudioEvent
// asset would be named Dx_B_Holly_Headshot_07) and FStrings, also one pointer deep, every read checked first (name
// pool bounds, VirtualQuery, the object array). A name matches a mute entry when the entry's words (split at '_')
// appear in it in a row, any case: "Kill_Bat" matches Dx_B_Holly_Kill_Bat_04, not Ping_Melee. Muted = not played (no
// sound, no subtitle). Each machine filters what it plays itself (the host's own lines and the ones it receives), so
// it is a local setting, like the rest of the fork's.
//
// The first 40 lines per game start are logged with what was found in them ("barks: ... played|MUTED"), the first 3
// with their raw bytes too: if the barks are still heard, that log shows where the line's name is (or that it isn't
// in there), for the next build.
// Ini: barks_mute=0|1 (default 1), barks_mute_list=<Name,Name> (default below). Chat: /dialogue barks [on|off|mute
// <A,B>|log].
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "MinHook.h"
#include "ue.h"
#include "log.h"
#include "cmds.h"

#define BARKS_DEF        "Headshot,Kill_Bat,Shoot_Explosive,Bash,PickUp_Copper,Trigger_Bleed,Horde_Finish"
#define ADDR_PLAY_LINE   VA(0x141620240ull)   // UGobiDialogueComponent play line (vtable +0x410), as dialogue.c
#define PLAY_SLOT        (0x410 / 8)
#define LINE_LOGS        40
#define RAW_LOGS         3
#define MAX_CAND         24

static int mute_on = 1;
static char mute_list[256] = BARKS_DEF;
static unsigned n_lines, n_muted, line_logs, raw_logs;
static int hero_checked;              // 0 not yet, 1 done (same function or hooked), -1 gave up
static uintptr_t hero_play;           // the heroes' own +0x410 when it isn't ADDR_PLAY_LINE
typedef void (*PlayFn)(UObject *comp, const void *params);
static PlayFn orig_hero_play;

static int readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mb;
    uintptr_t v = (uintptr_t)p;
    if (v < 0x10000 || v > 0x7FFFFFFFFFFFull) return 0;
    if (!VirtualQuery(p, &mb, sizeof mb) || mb.State != MEM_COMMIT || (mb.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    if (!(mb.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                        PAGE_EXECUTE_WRITECOPY))) return 0;
    return v + n <= (uintptr_t)mb.BaseAddress + mb.RegionSize;
}

// an FName that really is one: its block exists and its entry fits the block (0x10000 entries of 2 bytes, as
// ue_name reads them); the text printable. 1 = name in buf
static int name_try(FName n, char *buf, size_t len) {
    if (!n.idx || n.num > 100000 || len < 8) return 0;
    uint32_t blk = n.idx >> 18, off = (n.idx & 0xFFFF) * 2;
    if (blk >= 8192) return 0;
    const uint8_t *const *blocks = (const uint8_t *const *)(ADDR_NAMEPOOL + 0x10);
    const uint8_t *b = blocks[blk];
    if (!b || !readable(b + off, 2)) return 0;
    uint16_t hdr = *(const uint16_t *)(b + off);
    int wide = hdr & 1, l = hdr >> 6;
    if (l < 2 || l > 120 || off + 2u + (uint32_t)l * (wide ? 2u : 1u) > 0x20000u || !readable(b + off, 2u + (size_t)l * 2u)) return 0;
    size_t k = 0;
    for (int i = 0; i < l && k + 1 < len; i++) {
        unsigned c = wide ? ((const uint16_t *)(b + off + 2))[i] : b[off + 2 + i];
        if (c < 32 || c > 126) return 0;
        buf[k++] = (char)c;
    }
    buf[k] = 0;
    return 1;
}

static UObject *obj_try(uintptr_t v) {
    if ((v & 7) || !readable((const void *)v, 0x28)) return NULL;
    int32_t idx = U_INDEX((UObject *)v);
    if (idx < 0 || idx >= ue_num_objects()) return NULL;
    return ue_object_at(idx) == (UObject *)v ? (UObject *)v : NULL;
}

static int fstring_try(const uint8_t *at, char *buf, size_t len) {   // {wchar_t *data, int num, int max}
    const wchar_t *d = *(const wchar_t *const *)at;
    int32_t num = *(const int32_t *)(at + 8), max = *(const int32_t *)(at + 12);
    if (num < 3 || num > 200 || max < num || ((uintptr_t)d & 1) || !readable(d, (size_t)num * 2)) return 0;
    size_t k = 0;
    for (int i = 0; i < num - 1 && k + 1 < len; i++) {
        if (d[i] < 32 || d[i] > 126) return 0;
        buf[k++] = (char)d[i];
    }
    if (d[num - 1] != 0) return 0;
    buf[k] = 0;
    return 1;
}

typedef struct { char s[MAX_CAND][100]; int n; } Cands;
static void cand_add(Cands *c, const char *pre, const char *s) {
    if (c->n == MAX_CAND) return;
    for (int i = 0; i < c->n; i++) if (!strcmp(c->s[i] + strlen(pre), s)) return;
    snprintf(c->s[c->n++], sizeof c->s[0], "%s%s", pre, s);
}
// FNames (4-byte steps), objects and FStrings (8-byte steps) in `n` bytes at p; deeper: pointers that are no object
// are looked through once more
static void scan_block(const uint8_t *p, size_t n, int deeper, Cands *c) {
    char b[100];
    if (!readable(p, n)) return;
    for (size_t o = 0; o + 8 <= n; o += 4) {
        FName nm = *(const FName *)(p + o);
        if (name_try(nm, b, sizeof b)) cand_add(c, "name:", b);
        if (o & 7) continue;
        uintptr_t v = *(const uintptr_t *)(p + o);
        UObject *ob = obj_try(v);
        if (ob) { ue_obj_name(ob, b, sizeof b); cand_add(c, "obj:", b); continue; }
        if (o + 16 <= n && fstring_try(p + o, b, sizeof b)) { cand_add(c, "str:", b); continue; }
        if (deeper && v && readable((const void *)v, 0x40)) scan_block((const uint8_t *)v, 0x40, 0, c);
    }
}

// the entry's words (split at '_') in a row in s, any case
int barks_name_match(const char *s, const char *entry) {
    char sw[24][32], ew[8][32];
    int ns = 0, ne = 0;
    for (const char *p = s; *p && ns < 24; ) {
        while (*p == '_' || *p == ':' || *p == ' ' || *p == '.' || *p == '/') p++;
        int k = 0;
        while (*p && *p != '_' && *p != ':' && *p != ' ' && *p != '.' && *p != '/') { if (k < 31) sw[ns][k++] = (char)tolower((unsigned char)*p); p++; }
        if (k) { sw[ns][k] = 0; ns++; }
    }
    for (const char *p = entry; *p && ne < 8; ) {
        while (*p == '_' || *p == ' ') p++;
        int k = 0;
        while (*p && *p != '_' && *p != ' ' && *p != ',') { if (k < 31) ew[ne][k++] = (char)tolower((unsigned char)*p); p++; }
        if (k) { ew[ne][k] = 0; ne++; }
        if (*p == ',') break;
    }
    if (!ne) return 0;
    for (int i = 0; i + ne <= ns; i++) {
        int j = 0;
        while (j < ne && !strcmp(sw[i + j], ew[j])) j++;
        if (j == ne) return 1;
    }
    return 0;
}
// the first list entry that one of the names matches, into hit; 0 = none
int barks_list_match(const char *list, const char *const *names, int n, char *hit, size_t hn) {
    for (const char *p = list; *p; ) {
        while (*p == ',' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        if (e > p) {
            char entry[64];
            size_t l = (size_t)(e - p) < sizeof entry - 1 ? (size_t)(e - p) : sizeof entry - 1;
            memcpy(entry, p, l);
            entry[l] = 0;
            for (int i = 0; i < n; i++)
                if (barks_name_match(names[i], entry)) { snprintf(hit, hn, "%s", entry); return 1; }
        }
        p = e;
    }
    return 0;
}

// a survivor's (hero's or bot's) dialogue component: the only lines the list mutes (build 42)
static int survivor_comp(UObject *comp) {
    static UClass *hdc;
    static int looked;
    if (!looked) { looked = 1; hdc = ue_find_class("HeroDialogueComponent"); }
    return comp && hdc && ue_is_a(comp, hdc);
}

// dialogue.c's play hook (and ours below) asks first: 1 = don't play this line
int barks_filter(UObject *comp, const void *params) {
    char a[160], b[160], hit[64] = "";
    if (!params || (!mute_on && line_logs >= LINE_LOGS)) return 0;
    Cands c;
    c.n = 0;
    scan_block((const uint8_t *)params, 0x80, 1, &c);
    const char *names[MAX_CAND];
    for (int i = 0; i < c.n; i++) names[i] = c.s[i];
    int survivor = survivor_comp(comp);
    int mute = mute_on && survivor && barks_list_match(mute_list, names, c.n, hit, sizeof hit);
    n_lines++;
    if (mute) n_muted++;
    if (line_logs < LINE_LOGS) {
        line_logs++;
        char found[1200];
        size_t k = 0;
        found[0] = 0;
        for (int i = 0; i < c.n && k + 110 < sizeof found; i++) k += (size_t)snprintf(found + k, sizeof found - k, "%s%s", i ? " " : "", c.s[i]);
        UObject *spk = comp ? U_OUTER(comp) : NULL;
        LOG("barks: %s (%s) line: %s -> %s%s", spk ? ue_obj_name(spk, a, sizeof a) : "-",
            comp && U_CLASS(comp) ? ue_obj_name(U_CLASS(comp), b, sizeof b) : "-", c.n ? found : "(nothing that names it)",
            mute ? "MUTED: " : "played", mute ? hit : survivor ? "" : " (not a survivor)");
        if (raw_logs < RAW_LOGS) {
            raw_logs++;
            char hex[0x80 * 3 + 8];
            size_t h = 0;
            if (readable(params, 0x80)) for (int i = 0; i < 0x80; i++) h += (size_t)sprintf(hex + h, "%02x%s", ((const uint8_t *)params)[i], (i & 7) == 7 ? " " : "");
            hex[h] = 0;
            LOG("barks:   its parameter block: %s", h ? hex : "(not readable)");
        }
    }
    return mute;
}

static void hero_play_detour(UObject *comp, const void *params) {
    if (barks_filter(comp, params)) return;
    orig_hero_play(comp, params);
}

static UObject *hero_dialogue(void) {   // a hero's DialogueComponent (first one found)
    UClass *hc = ue_find_class("HeroCharacter"), *dc = ue_find_class("DialogueComponent");
    if (!hc || !dc) return NULL;
    for (int32_t i = 0, n = ue_num_objects(); i < n; i++) {
        UObject *x = ue_object_at(i);
        if (!x || (U_FLAGS(x) & 0x30) || !ue_is_a(x, dc)) continue;
        UObject *o = U_OUTER(x);
        if (o && !(U_FLAGS(o) & 0x30) && ue_is_a(o, hc)) return x;
    }
    return NULL;
}

void barks_tick(float dt) {
    static float wait;
    char a[128], chain[400];
    if (hero_checked) return;
    if ((wait -= dt) > 0) return;
    wait = 2.f;
    UObject *comp = hero_dialogue();
    if (!comp) return;
    UClass *gdc = ue_find_class("GobiDialogueComponent");
    size_t k = 0;
    chain[0] = 0;
    for (UObject *s = U_CLASS(comp); s && k + 64 < sizeof chain; s = *(UObject **)((char *)s + 0x48))
        k += (size_t)snprintf(chain + k, sizeof chain - k, "%s%s", k ? " < " : "", ue_obj_name(s, a, sizeof a));
    if (!gdc || !ue_is_a(comp, gdc)) {
        hero_checked = -1;
        LOG("barks: the heroes' dialogue component is no GobiDialogueComponent (%s): only dialogue.c's hook filters lines", chain);
        return;
    }
    uintptr_t f = (uintptr_t)U_VTBL(comp)[PLAY_SLOT];
    MEMORY_BASIC_INFORMATION mb;
    HMODULE exe = GetModuleHandleW(NULL);
    int in_exe = VirtualQuery((void *)f, &mb, sizeof mb) && mb.AllocationBase == (void *)exe;
    if (f == ADDR_PLAY_LINE) {
        hero_checked = 1;
        LOG("barks: heroes' dialogue lines go through 0x%llx (%s), the hooked play: filtered there",
            (unsigned long long)(f - g_base_delta), chain);
        return;
    }
    if (!in_exe) {
        hero_checked = -1;
        LOG("barks: the heroes' play line (%s) is at 0x%llx, outside the game: not hooked", chain, (unsigned long long)f);
        return;
    }
    hero_play = f;
    if (MH_CreateHook((void *)f, (void *)hero_play_detour, (void **)&orig_hero_play) != MH_OK || MH_EnableHook((void *)f) != MH_OK) {
        hero_checked = -1;
        LOG("barks: the heroes' own play line 0x%llx (%s) could not be hooked", (unsigned long long)(f - g_base_delta), chain);
        return;
    }
    hero_checked = 1;
    LOG("barks: the heroes' own play line 0x%llx (%s, not the cutscene speakers' 0x141620240): hooked too",
        (unsigned long long)(f - g_base_delta), chain);
}

static void set_list(const char *v) {   // names of letters, digits and '_' ('-' read as '_'); anything else separates
    size_t m = 0;
    for (; v && *v && m + 1 < sizeof mute_list; v++) {
        char ch = *v == '-' ? '_' : *v;
        if (isalnum((unsigned char)ch) || ch == '_') mute_list[m++] = ch;
        else if (m && mute_list[m - 1] != ',') mute_list[m++] = ',';
    }
    while (m && mute_list[m - 1] == ',') m--;
    mute_list[m] = 0;
}
static int setting(const char *k, const char *v) {
    if (!strcmp(k, "barks_mute")) mute_on = !v ? 1 : (!_stricmp(v, "off") || !_stricmp(v, "false") || !strcmp(v, "0")) ? 0 : 1;
    else if (!strcmp(k, "barks_mute_list")) set_list(v ? v : BARKS_DEF);
    else return 0;
    return 1;
}
int barks_live(const char *key, const char *val) { return !strncmp(key, "barks_", 6) && setting(key, val); }
static void ini_pair(const char *k, const char *v, void *ctx) { (void)ctx; setting(k, v); }

void barks_slash(char *rest, Out *o) {
    char *what = rest ? strtok(rest, " ") : NULL, *arg = what ? strtok(NULL, " ") : NULL, *arg2 = arg ? strtok(NULL, "") : NULL;
    int fail = 0;
    if (what && _stricmp(what, "barks")) { out_printf(o, "usage: /dialogue barks [on|off|mute <Name,Name>|log]\n"); return; }
    if (arg && (!_stricmp(arg, "on") || !_stricmp(arg, "off"))) {
        mute_on = !_stricmp(arg, "on");
        fail = cmds_ini_set("barks_mute", mute_on ? NULL : "0");
    } else if (arg && !_stricmp(arg, "mute") && arg2) {
        set_list(arg2);
        fail = cmds_ini_set("barks_mute_list", strcmp(mute_list, BARKS_DEF) ? mute_list : NULL);
    } else if (arg && !_stricmp(arg, "log")) {
        line_logs = 0;
        raw_logs = RAW_LOGS - 1;
        out_printf(o, "/dialogue barks: the next %d lines go into the log (\"barks:\")\n", LINE_LOGS);
    } else if (arg) { out_printf(o, "usage: /dialogue barks [on|off|mute <Name,Name>|log]\n"); return; }
    if (arg && !_stricmp(arg, "mute") && !arg2) { out_printf(o, "usage: /dialogue barks mute <Name,Name> (e.g. %s)\n", BARKS_DEF); return; }
    if (arg) LOG("barks: mute %s, list %s", mute_on ? "on" : "off", mute_list);
    out_printf(o, "/dialogue barks: %s (default on; this machine): survivors' lines named %s are not played (sound and "
                  "subtitle); %u lines seen, %u muted%s\n", mute_on ? "muted" : "NOT muted", mute_list[0] ? mute_list : "(none)",
               n_lines, n_muted, hero_checked < 0 ? "; heroes' own play line NOT hooked (see log)" : "");
    if (fail) out_printf(o, "/dialogue: could not write b4bcoop.ini (see log): this change lasts until the game closes\n");
}

int barks_init(void) {
    cmds_ini_each(ini_pair, NULL);
    LOG("barks: survivor barks %s (this machine), list %s", mute_on ? "muted" : "not muted", mute_list);
    return 0;
}
