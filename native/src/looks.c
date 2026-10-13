// The `~` window's "Add-ons" tab: one table of every add-on, the add-on shop's (shop.c, its list fetched once per game
// start in Fort Hope, and on Refresh) and the ones in the add-ons folder (addons.c, also hand-installed ones). It
// replaced the Models, Add-ons and Browse tabs, then the Wear / Installed / Get more modes (docs/investigations/overlay.md
// "Add-ons tab"). Columns: thumbnail | name + file size | installed state | Add, Update, Undo, Turn on, Wear / Use,
// Remove. Installed add-ons first, then by name or by the time they were installed (either way round); a search box
// and a filter (All / Installed / Available). No load order and no on/off here: Remove is the way out (an add-on that
// is off in addonlist.txt shows Turn on). Wear opens the game's customization screen with the outfit on (models.c);
// weapon looks: Use / Stop (weaponlooks.c). Host rules (models.c, addons_mp.c) at the bottom.
// Dev: `overlay tab Models|Browse|Get more|Wear|Looks` shows this tab (filter All), `overlay tab Installed` with the
// filter Installed (overlay_add_alias).
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "log.h"
#include "cmds.h"
#include "overlay.h"

#define TAB "Add-ons"
#define MAX_ROWS (MAX_ADDONS + 256)   // + SHOP_MAX_ITEMS (updcore.h)
#define THUMB 6.0f                    // thumbnail box, font heights (78 px at text size 1, 98 px at 1.25)
#define PREVIEW 20.f                  // the hovered thumbnail's large picture, font heights

static char search[64];
static int filter;        // 0 all, 1 installed, 2 available
static int sort_by;       // 0 name, 1 time installed
static int sort_rev;      // the other way round

typedef struct { int a, c, inst; unsigned long long time; const char *name; } Row;   // a: addons slot, c: shop entry (-1 none)
static Row rows[MAX_ROWS];

static int has(const char *hay, const char *needle) {   // case-insensitive substring
    size_t n = strlen(needle);
    if (!n) return 1;
    for (; hay && *hay; hay++) if (!_strnicmp(hay, needle, n)) return 1;
    return 0;
}
static void size_str(unsigned long long b, char *out, size_t n) {
    if (b >= 1048576) snprintf(out, n, "%.1f MB", b / 1048576.0);
    else snprintf(out, n, "%llu KB", (b + 1023) / 1024);
}
static int cmp_rows(const void *x, const void *y) {
    const Row *a = x, *b = y;
    if (a->inst != b->inst) return b->inst - a->inst;   // installed first, always
    int r = 0;
    if (sort_by == 1 && a->inst) r = a->time < b->time ? 1 : a->time > b->time ? -1 : 0;   // newest first
    if (!r) r = _stricmp(a->name, b->name);
    return sort_rev && (sort_by == 0 || a->inst) ? -r : r;   // by time: the others stay A-Z
}
static int installed_state(int s) { return s == SHOP_INSTALLED || s == SHOP_NEWER || s == SHOP_REMOVING || s == SHOP_UPDATING; }

// every add-on in the folder (its shop entry when the list has one: same file, or the same content under another
// file name), then the shop's other entries
static int build_rows(void) {
    int n = 0;
    AddonInfo ai;
    ShopRow sr;
    static char used[256];
    int ns = shop_rows();
    memset(used, 0, sizeof used);
    for (int i = 0; i < addons_slots() && n < MAX_ROWS; i++) {
        if (!addons_info(i, &ai)) continue;
        int c = shop_index_of(ai.file);
        if (c >= 0 && c < 256) used[c] = 1;
        rows[n++] = (Row){i, c, 1, ai.time, c >= 0 ? shop_name(c) : ai.title};   // the shop's name when it lists it
    }
    for (int c = 0; c < ns && c < 256 && n < MAX_ROWS; c++) {
        if (used[c] || !shop_row(c, &sr)) continue;
        if (sr.state == SHOP_INSTALLED_AS && sr.like_why && !strcmp(sr.like_why, "same content")) {
            int k = 0;
            while (k < n && !(rows[k].a >= 0 && addons_info(rows[k].a, &ai) && !_stricmp(ai.file, sr.like))) k++;
            if (k < n && rows[k].c < 0) { rows[k].c = c; rows[k].name = sr.name; continue; }
        }
        rows[n++] = (Row){-1, c, installed_state(sr.state), 0, sr.name};
    }
    return n;
}

static void note(const char *e) { if (e) overlay_note(e); }

static void top_bar(void) {
    static const char *F[] = {"All", "Installed", "Available"}, *S[] = {"Name", "Installed on"};
    ov_width(10);
    ov_input_text("##search", search, sizeof search, "search");
    ov_same_line_fit(NULL, 6.5f);
    ov_width(6.5f);
    ov_combo("##filter", &filter, F, 3);
    ov_tooltip("Installed: the add-ons in your add-ons folder. Available: the shop's others.");
    ov_same_line_fit(NULL, 8);
    ov_width(8);
    ov_combo("##sort", &sort_by, S, 2);
    ov_tooltip("Sort by name or by when you installed it (the file's time). Installed add-ons always come first.");
    const char *dir = sort_by ? (sort_rev ? "Oldest" : "Newest") : (sort_rev ? "Z-A" : "A-Z");
    char lab[32];
    snprintf(lab, sizeof lab, "%s##sortdir", dir);
    ov_same_line_fit(lab, 0);
    if (ov_button(lab)) sort_rev = !sort_rev;
    ov_tooltip("The other way round.");
    if (!shop_enabled()) return;
    char m[320];
    int ph = shop_phase(m, sizeof m);
    ov_same_line_fit("Refresh", 0);
    ov_begin_disabled(shop_busy(), "busy");
    if (ov_button("Refresh##shop")) note(shop_fetch());
    ov_end_disabled();
    ov_tooltip("Gets the add-on shop's list again (github.com/actuallydan/back4blood-shop). Every file is checked against "
               "the signed list; nobody learns what you have.");
    if (ph == 1 || ph == 3 || (ph == 2 && !shop_rows())) {   // fetching, failed, or an empty list: say so
        if (ph == 3) ov_text_warn("%s", m); else ov_text_dim("%s", m);
    }
}

static void cell_name(const Row *r, const AddonInfo *ai, const ShopRow *sr) {
    char sz[32], tip[900];
    size_str(r->a >= 0 ? ai->size : sr->size, sz, sizeof sz);
    ov_text("%s", r->name);
    ov_text_dim_line("%s", sz);
    const char *author = r->c >= 0 ? sr->author : ai->author, *desc = r->c >= 0 ? sr->desc : ai->desc;
    int k = snprintf(tip, sizeof tip, "%s%s%s", author && *author ? "by " : "", author ? author : "", author && *author ? "\n" : "");
    if (desc && *desc) k += snprintf(tip + k, sizeof tip - k, "%s\n", desc);
    if (r->c >= 0 && sr->license && *sr->license) k += snprintf(tip + k, sizeof tip - k, "License: %s\n", sr->license);
    snprintf(tip + k, sizeof tip - k, "File: %s", r->a >= 0 ? ai->file : sr->file);
    ov_tooltip(tip);
}

static void cell_state(const Row *r, const AddonInfo *ai, const ShopRow *sr) {
    const char *file = r->a >= 0 ? ai->file : sr->file;
    int pend = shop_pending(file), live = shop_pending_live(file);
    const char *ver = r->a >= 0 && *ai->version ? ai->version : r->c >= 0 ? sr->version : "";
    if (r->a >= 0) {
        if (pend == 1) ov_text_warn("removed at restart");
        else if (pend == 2 && !live) ov_text_warn("updated at restart");
        else if (!*ai->why && !ai->on) ov_text_dim("off");
        else if (*ai->why) { ov_text_warn("can't load"); ov_tooltip(ai->why); }
        else if (ai->mounted <= 0) ov_text("loads at restart");
        else ov_text("%s%s", *ver ? "v" : "installed", ver);
        if (r->c >= 0 && sr->state == SHOP_NEWER) ov_text_warn("v%s available", sr->version);
    } else if (r->c >= 0) {
        if (sr->state == SHOP_DOWNLOADING) ov_text_dim("downloading");
        else if (sr->state == SHOP_INSTALLED_AS) {
            ov_text("as %s", sr->like);
            ov_tooltip("Already in your add-ons folder under another file name (it adds the same look), so it isn't added twice.");
        } else if (installed_state(sr->state)) ov_text("loads at restart");
        else if (*sr->too_old) { ov_text_dim("needs newer b4bcoop"); ov_tooltip(sr->too_old); }
    }
    if (r->c >= 0 && sr->msg[0] && sr->state != SHOP_DOWNLOADING) {
        if (sr->msg_err) ov_text_warn("%s", sr->msg);
        else ov_tooltip(sr->msg);
    }
}

static void cell_actions(const Row *r, const AddonInfo *ai, const ShopRow *sr) {
    char lab[120];
    int n = 0, busy = shop_busy();
    const char *file = r->a >= 0 ? ai->file : sr->file;
    int pend = shop_pending(file), live = shop_pending_live(file);
#define NEXT(l) do { if (n++) ov_same_line_fit((l), 0); } while (0)
    if (r->c >= 0 && sr->state == SHOP_DOWNLOADING) {
        char t[64];
        snprintf(t, sizeof t, "%llu / %llu KB", sr->got / 1024, sr->size / 1024);
        ov_progress(sr->size ? (float)sr->got / (float)sr->size : 0, 0, t);
        return;
    }
    if (r->a < 0 && r->c >= 0 && sr->state == SHOP_ADD) {
        snprintf(lab, sizeof lab, "Add##%s", sr->id);
        NEXT(lab);
        ov_begin_disabled(busy || *sr->too_old || !addons_enabled(), *sr->too_old ? sr->too_old : !addons_enabled() ? "Add-ons are off (Load add-ons, below)." : "busy");
        if (ov_button(lab)) note(shop_add(r->c));
        ov_tooltip("Downloads it into your add-ons folder; most add-ons are ready at once, others after a restart.");
        ov_end_disabled();
    }
    if (pend && !(pend == 2 && live)) {
        snprintf(lab, sizeof lab, "Undo##%s", file);
        NEXT(lab);
        if (ov_button(lab)) note(shop_undo(file));
        ov_tooltip(pend == 1 ? "Keep it (switched back on)." : "Keep the current version (the downloaded one is deleted).");
    }
    if (r->a >= 0 && !ai->on && pend != 1) {
        snprintf(lab, sizeof lab, "Turn on##%s", file);
        NEXT(lab);
        if (ov_button(lab)) ov_run("addons on %s", file);
        ov_tooltip("It is off in addonlist.txt: on again (/addons on); loaded at once when that is safe, else at the next start.");
    }
    if (r->a >= 0 && pend != 1 && addons_look_buttons(file, n)) n++;
    if (r->c >= 0 && sr->state == SHOP_NEWER) {
        snprintf(lab, sizeof lab, "Update##%s", sr->id);
        NEXT(lab);
        ov_begin_disabled(busy || *sr->too_old, *sr->too_old ? sr->too_old : "busy");
        if (ov_button(lab)) note(shop_add(r->c));
        ov_tooltip("The shop has a newer file: downloaded now, ready at once when nothing of the old version is in use, "
                   "else after a restart.");
        ov_end_disabled();
    }
    if (r->a >= 0 && pend != 1) {
        snprintf(lab, sizeof lab, "Remove##%s", file);
        NEXT(lab);
        if (ov_button_confirm(lab, "Sure?")) note(shop_remove(file));
        ov_tooltip("Switched off now; its file is deleted when the game next starts (Undo until then).");
    }
#undef NEXT
}

static int visible(const Row *r, const AddonInfo *ai, const ShopRow *sr) {
    if (filter == 1 && !r->inst) return 0;
    if (filter == 2 && r->inst) return 0;
    return !search[0] || has(r->name, search) || has(ai->file, search) || has(ai->author, search) || has(ai->desc, search) ||
           has(sr->id, search) || has(sr->author, search) || has(sr->desc, search);
}
static void fill(const Row *r, AddonInfo *ai, ShopRow *sr) {
    if (r->a < 0 || !addons_info(r->a, ai)) memset(ai, 0, sizeof *ai);
    if (r->c < 0 || !shop_row(r->c, sr)) memset(sr, 0, sizeof *sr);
}
// The table fills the tab down to what follows it (below: its height last frame) and scrolls by itself.
static void table(int n, float below) {
    static const float W[] = {-THUMB, 1.0f, 0.45f, 0.62f};
    static const char *H[] = {"", "Add-on", "Installed", ""};
    AddonInfo ai; ShopRow sr;
    int shown = 0;
    for (int k = 0; k < n && !shown; k++) { fill(&rows[k], &ai, &sr); shown = visible(&rows[k], &ai, &sr); }
    if (!shown) { ov_text_dim(n ? "Nothing matches." : "No add-ons yet."); return; }
    if (!ov_table_begin_scroll("addons", 4, below, 3 * (THUMB + 1))) return;
    ov_table_columns(H, W, 4, 1);
    for (int k = 0; k < n; k++) {
        const Row *r = &rows[k];
        fill(r, &ai, &sr);
        if (!visible(r, &ai, &sr)) continue;
        ov_push_id(k);
        ov_table_next();
        if (sr.tex) {
            float tw = THUMB, th = THUMB;
            if (sr.tw >= sr.th) th = THUMB * sr.th / sr.tw; else tw = THUMB * sr.tw / sr.th;
            ov_image(sr.tex, tw, th);
            int pw, ph, pt = ov_hovered() ? shop_preview(r->c, &pw, &ph) : 0;
            if (pt && pw > 0 && ph > 0) {   // its longer side PREVIEW font heights
                float k = PREVIEW / (float)(pw > ph ? pw : ph);
                ov_tooltip_image(pt, k * pw, k * ph, pw / (float)SHOP_PREVIEW_SIDE, ph / (float)SHOP_PREVIEW_SIDE);
            }
        }
        ov_table_next();
        cell_name(r, &ai, &sr);
        ov_table_next();
        cell_state(r, &ai, &sr);
        ov_table_next();
        cell_actions(r, &ai, &sr);
        ov_pop_id();
    }
    ov_table_end();
}

static void panel(void) {
    static ULONGLONG t_refresh;
    static float below;   // the height of everything under the table, measured last frame
    if (GetTickCount64() - t_refresh > 1000) { t_refresh = GetTickCount64(); addons_refresh(); }
    int on = addons_tab_header();
    if (on) {
        int n = build_rows();
        qsort(rows, (size_t)n, sizeof rows[0], cmp_rows);
        top_bar();
        shop_show_thumbs();
        table(n, below);
    }
    float y0 = ov_cursor_y();
    addons_tab_footer();
    ov_heading("Host rules");
    models_host_panel();
    ov_same_line_fit(NULL, 25);
    addons_mp_panel();
    below = ov_cursor_y() - y0;
}

static void alias(int f) { filter = f; search[0] = 0; }

void looks_init(void) {
    overlay_add_panel(TAB, 60, panel);
    overlay_panel_size(TAB, 52, 1.f, 3);   // wide, as tall as the screen allows, a short log: the list is the point
    static const struct { const char *name; int filter; } A[] = {
        {"Models", 0}, {"Browse", 0}, {"Get more", 0}, {"Wear", 0}, {"Looks", 0}, {"Looks & add-ons", 0}, {"Installed", 1}};
    for (size_t i = 0; i < sizeof A / sizeof A[0]; i++) overlay_add_alias(A[i].name, TAB, alias, A[i].filter);
}
