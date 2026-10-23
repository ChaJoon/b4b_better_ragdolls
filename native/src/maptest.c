// /map: load a map by its path (fork module, for the custom-map tests). Host only. Nothing is sent to other players
// beyond the game's own server travel (clients follow it like a mission start and need the same add-on to load).
//
//   /map                                 the current map: package path, game mode class, the options it was opened with
//   /map <path> [difficulty] [?opts]     a mission travel, the same URL as the war table's (minus the campaign run):
//                                        servertravel <path>?Difficulty=..?PoolConfig=Coop?game=HeroGameMode_BP?RunOwner=1
//                                        ?listen (upstream's developer command `mission raw`). Difficulty: easy|normal|
//                                        hard|veryhard or recruit|veteran|nightmare|nohope (default easy)
//   /map <path> plain [?opts]            servertravel <path>?listen: the map's own game mode (its world settings', else
//                                        the game's default)
//   /map back                            to the map /map last left, with that map's game mode and options
//
// <path> is a package path (/Game/Folder/MAP_Name, or /Game/Folder/MAP_Name.MAP_Name). Extra tokens starting with '?'
// are appended to the URL as they are (e.g. ?game=/Script/Engine.GameModeBase). The package must exist in the
// engine's file system (the game's paks, an add-on, loose files) before anything happens: a server travel to a missing
// map fails and drops the game to the main menu.
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "ue.h"
#include "log.h"
#include "cmds.h"

static const char *HERO_MODE = "/Game/Modes/HeroGameMode_BP.HeroGameMode_BP_C";   // testing.c `mission raw`
static const struct { const char *name, *alias, *url; } DIFF[] = {
    {"easy", "recruit", "Easy"}, {"normal", "veteran", "Normal"}, {"hard", "nightmare", "Hard"}, {"veryhard", "nohope", "VeryHard"}};

static char back_url[2048];   // the map /map last left: package + its options (with ?game=)

static void fs_ascii(const FString *s, char *buf, size_t len) {
    size_t i = 0;
    if (s && s->data && s->num > 0)
        for (; i < len - 1 && (int32_t)i < s->num && s->data[i]; i++) buf[i] = s->data[i] < 128 ? (char)s->data[i] : '?';
    buf[i] = 0;
}

// the current map's game mode class (as a ?game= value) and the options string it was opened with
static void current(UObject *w, char *pkg, size_t pl, char *mode, size_t ml, char *opts, size_t ol) {
    pkg[0] = mode[0] = opts[0] = 0;
    ue_world_package(w, pkg, pl);
    UObject *gm = ue_get_ptr(w, "AuthorityGameMode");
    if (!gm) return;
    ue_full_path((UObject *)U_CLASS(gm), mode, ml);
    int32_t off = ue_prop_offset(gm, "OptionsString");
    if (off >= 0) fs_ascii((FString *)((char *)gm + off), opts, ol);
}

// ?key= present in an options string (case-insensitive)
static int has_opt(const char *opts, const char *key) {
    size_t kl = strlen(key);
    for (const char *p = opts; (p = strchr(p, '?')); p++)
        if (!_strnicmp(p + 1, key, kl) && p[1 + kl] == '=') return 1;
    return 0;
}

static int path_ok(const char *p) {
    if (_strnicmp(p, "/Game/", 6) || strlen(p) > 400) return 0;
    for (const char *c = p; *c; c++) if (!isalnum((unsigned char)*c) && !strchr("/_-.", *c)) return 0;
    return 1;
}

// "/Game/A/MAP_X" -> "gobi/content/a/map_x" + ext (paks_file_exists' key)
static int pkg_exists(const char *pkg, const char *ext) {
    char k[600];
    snprintf(k, sizeof k, "gobi/content/%s%s", pkg + 6, ext);
    for (char *c = k; *c; c++) *c = (char)tolower((unsigned char)*c);
    return paks_file_exists(k);
}

static void travel(const char *url, Out *o) {
    char cmd[2100];
    snprintf(cmd, sizeof cmd, "servertravel %s", url);
    LOG("maptest: %s", cmd);
    game_exec(cmd);
    out_printf(o, "/map: travelling: %s\n", url);
}

void maptest_slash(char *rest, Out *o) {
    UObject *w = ue_world();
    if (!w || !ue_get_ptr(w, "AuthorityGameMode")) { out_printf(o, "/map: not now (no game mode: loading, or not the host)\n"); return; }
    char pkg[512], mode[512], opts[700];
    current(w, pkg, sizeof pkg, mode, sizeof mode, opts, sizeof opts);
    char *tok[8] = {0};
    int n = 0;
    for (char *t = rest ? strtok(rest, " ") : NULL; t && n < 8; t = strtok(NULL, " ")) tok[n++] = t;
    if (!n) {
        out_printf(o, "/map: %s\n  game mode %s\n  options %s\n", pkg, mode[0] ? mode : "(none)", opts[0] ? opts : "(none)");
        if (back_url[0]) out_printf(o, "  /map back: %s\n", back_url);
        LOG("maptest: current %s, game mode %s, options %s", pkg, mode, opts);
        return;
    }
    if (!_stricmp(tok[0], "back")) {
        if (!back_url[0]) { out_printf(o, "/map back: nowhere to go back to (no /map travel since the game started)\n"); return; }
        static char url[2048];
        snprintf(url, sizeof url, "%s", back_url);
        travel(url, o);
        return;
    }
    // the path: strip an object name ("/Game/X/MAP_Y.MAP_Y" -> "/Game/X/MAP_Y")
    char path[512];
    snprintf(path, sizeof path, "%s", tok[0]);
    char *slash = strrchr(path, '/'), *dot = slash ? strchr(slash, '.') : NULL;
    if (dot) *dot = 0;
    if (!path_ok(path)) { out_printf(o, "/map: expected a package path like /Game/Folder/MAP_Name, got %s\n", tok[0]); return; }
    int plain = 0, diff = 0;
    char extra[500] = "";
    for (int i = 1; i < n; i++) {
        int found = 0;
        if (!_stricmp(tok[i], "plain")) { plain = 1; continue; }
        if (!_stricmp(tok[i], "mission")) continue;
        for (int d = 0; d < 4; d++)
            if (!_stricmp(tok[i], DIFF[d].name) || !_stricmp(tok[i], DIFF[d].alias)) { diff = d; found = 1; }
        if (found) continue;
        if (tok[i][0] == '?' && !strchr(tok[i], ' ') && strlen(extra) + strlen(tok[i]) < sizeof extra) { strcat(extra, tok[i]); continue; }
        out_printf(o, "usage: /map [<path> [easy|normal|hard|veryhard|plain] [?option=value...]|back]\n");
        return;
    }
    int as_map = pkg_exists(path, ".umap"), as_asset = !as_map && pkg_exists(path, ".uasset");
    if (!as_map && !as_asset) {
        out_printf(o, "/map: %s isn't in the game's files or any add-on (is the add-on installed and on? /addons)\n", path);
        LOG("maptest: %s: no .umap or .uasset", path);
        return;
    }
    if (as_asset) out_printf(o, "/map: note: %s is a .uasset, not a .umap (maps are .umap); trying anyway\n", path);
    static char url[2048];
    if (plain) snprintf(url, sizeof url, "%s%s?listen", path, extra);
    else snprintf(url, sizeof url, "%s?Difficulty=%s?PoolConfig=Coop%s%s?RunOwner=1%s?listen", path, DIFF[diff].url,
                  has_opt(extra, "game") ? "" : "?game=", has_opt(extra, "game") ? "" : HERO_MODE, extra);
    // where /map back goes: this map with the options it was opened with (minus listen, added again), and its game
    // mode as ?game= unless the options already name one
    char o2[700] = "", copy[700];
    snprintf(copy, sizeof copy, "%s", opts);
    for (char *t = strtok(copy, "?"); t; t = strtok(NULL, "?"))
        if (_stricmp(t, "listen") && strlen(o2) + strlen(t) + 2 < sizeof o2) { strcat(o2, "?"); strcat(o2, t); }
    int add_mode = mode[0] && !has_opt(o2, "game");
    snprintf(back_url, sizeof back_url, "%s%s%s%s?listen", pkg, o2, add_mode ? "?game=" : "", add_mode ? mode : "");
    int nc = ue_num_clients(w);
    if (nc > 0) out_printf(o, "/map: %d player(s) will follow; they need the same add-on or their load fails\n", nc);
    LOG("maptest: from %s (game mode %s, options %s) to %s (%s)", pkg, mode, opts, path, as_map ? ".umap" : ".uasset");
    travel(url, o);
}
