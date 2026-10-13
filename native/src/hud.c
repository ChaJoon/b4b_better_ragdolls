// Teammate lists with more than 4 heroes (teamsize=5..8) fitted on the screen.
//
// In-game HUD: the party panel (GameHUDScreen_WBP -> PartyStatus, a PartyStatus_WBP_C : UPartyStatusUserWidget) is a
// bottom-left canvas box (673x588 design units; the HUD is laid out for 2160p: 0.25 px/unit at 540p, 0.5 at 1080p)
// holding VerticalBox_119 = {PushAllToBottomSpacer (fill), PlayerList (auto)}. PlayerList is a VerticalBox with one
// PartyStatusEntry_WBP_C per teammate (the local player has its own panel below the box), 116 units tall and 176
// apart: 3 teammates fit, 4 overflow a little, 7 run about 600 units past the bottom of the box, over the local
// player's panel and off the screen.
// Character select / loadout: the player list (MissionLoadoutScreen_WBP.PlayerLoadouts, PlayerLoadouts_WBP_C, its
// VerticalBox PlayerList; lineup.c adds rows past the designer's 4, #28) is top-anchored, one row per hero slot
// (a bot's row is Hidden but keeps its place), 288 units apart: 8 rows run off the bottom of the screen.
//
// Fix (local, every machine, only while the hero team has more than 4 slots), render transforms only (no layout,
// slot or widget-tree change, so the game's own SetPlayerAt / OnSlotsUpdated indexing stays as it is), re-evaluated
// every frame from the cached layout geometry (entries added, players joining/leaving, bots taken over, objectives
// appearing); the widgets are looked up once a second. HUD (4+ teammates): still one column at the game's x, scaled
// uniformly about its top-left corner and moved so that it ends where the game's bottom-aligned list ends (above
// your own panel, which is untouched) and starts under the objectives block's real bottom (HUDMissions, 1-3
// objectives) + a margin; scale 1 when it fits, never below 0.6 (then the top entries overlap the objectives, never
// your panel). Character select: the designer's 4 rows stay, rows 5-8 go to a second column beside them (under row
// 4 are the hero portraits). A hero team of 4 or less: nothing is touched (a list changed earlier is put back once).
// Dev: `hud` (state), `hud floor <scale>|margin <units>|gap <units>`, `hudtree <class|path suffix> [depth]` (widget
// tree with pixel rects, slots, render transforms), `hudxf <class|path suffix> <sx> <sy> [px py [tx ty]]`.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ue.h"
#include "log.h"
#include "cmds.h"

#define TEAMSLOTS_SZ 0x20
#define MAX_PANELS 8      // lists tracked (a HUD and a player list per local player)
#define MAX_ENTRIES 16

static float fit_floor = 0.6f;   // HUD: smallest scale of the teammate column (names stay readable)
static float fit_margin = 48;    // HUD: design units kept free under the objectives
static float col_gap = 80;       // character select: design units between the columns
static int n_applied;            // dev: transforms written

static TArray *arr(UObject *o, const char *prop) { int32_t off = ue_prop_offset(o, prop); return off >= 0 ? (TArray *)((char *)o + off) : NULL; }

// hero team size from the (replicated) slot manager
static int hero_slots(void) {
    UObject *w = ue_world(), *gs = w ? ue_get_ptr(w, "GameState") : NULL, *psm = gs ? ue_get_ptr(gs, "PlayerSlotManager") : NULL;
    TArray *teams = psm ? arr(psm, "TeamSlots") : NULL;
    if (!teams) return 0;
    for (int t = 0; t < teams->num; t++) {
        uint8_t *ts = (uint8_t *)teams->data + t * TEAMSLOTS_SZ;
        if (ts[0] == 0) return ((TArray *)(ts + 8))->num;   // EGobiTeam 0 = heroes
    }
    return 0;
}

// ---- widget access (offsets and functions resolved once on UWidget / UPanelSlot) ----
static int32_t o_slot = -1, o_vis, o_rt, o_pivot, o_slots, o_content, o_wt, o_root;
static UFunction *f_geom, *f_translate, *f_xform, *f_pivot;
static int32_t p_geom_rv, p_translate_v, p_xform_v, p_pivot_v;
static UClass *c_panel, *c_wtree, *c_hud, *c_loadouts;
static int resolve(void) {
    if (o_slot >= 0) return 0;
    UClass *w = ue_find_class("Widget"), *pw = ue_find_class("PanelWidget"), *ps = ue_find_class("PanelSlot"),
           *uw = ue_find_class("UserWidget"), *wt = ue_find_class("WidgetTree");
    if (!w || !pw || !ps || !uw || !wt) return -1;
    FField *a = ue_find_prop(w, "Visibility"), *b = ue_find_prop(w, "RenderTransform"), *c = ue_find_prop(pw, "Slots"),
           *d = ue_find_prop(ps, "Content"), *e = ue_find_prop(uw, "WidgetTree"), *f = ue_find_prop(wt, "RootWidget"),
           *pv = ue_find_prop(w, "RenderTransformPivot"), *sl = ue_find_prop(w, "Slot");
    f_geom = ue_find_function(w, "GetCachedGeometry");
    f_translate = ue_find_function(w, "SetRenderTranslation");
    f_xform = ue_find_function(w, "SetRenderTransform");
    f_pivot = ue_find_function(w, "SetRenderTransformPivot");
    FField *gr = f_geom ? ue_find_prop(f_geom, "ReturnValue") : NULL, *tv = f_translate ? ue_find_prop(f_translate, "Translation") : NULL,
           *xv = f_xform ? ue_find_prop(f_xform, "InTransform") : NULL, *pp = f_pivot ? ue_find_prop(f_pivot, "Pivot") : NULL;
    if (!a || !b || !c || !d || !e || !f || !pv || !sl || !gr || !tv || !xv || !pp || UFN_PARMSSIZE(f_geom) > 0x40) {
        LOG("hud: UMG reflection lookup failed");
        return -1;
    }
    o_vis = FP_OFFSET(a); o_rt = FP_OFFSET(b); o_slots = FP_OFFSET(c); o_content = FP_OFFSET(d); o_wt = FP_OFFSET(e);
    o_root = FP_OFFSET(f); o_pivot = FP_OFFSET(pv);
    p_geom_rv = FP_OFFSET(gr); p_translate_v = FP_OFFSET(tv); p_xform_v = FP_OFFSET(xv); p_pivot_v = FP_OFFSET(pp);
    c_panel = pw; c_wtree = wt;
    c_hud = ue_find_class("PartyStatusUserWidget");
    c_loadouts = ue_find_class("PlayerLoadoutsUserWidget");
    o_slot = FP_OFFSET(sl);
    return 0;
}
#define W_SLOT(x)    (*(UObject **)((char *)(x) + o_slot))
#define W_VIS(x)     (*((uint8_t *)(x) + o_vis))        // ESlateVisibility: 1 Collapsed (no room in the layout), 2 Hidden
#define W_RT(x)      ((float *)((char *)(x) + o_rt))    // FWidgetTransform: Translation[2], Scale[2], Shear[2], Angle
#define W_PIVOT(x)   ((float *)((char *)(x) + o_pivot)) // FVector2D, 0..1 of the widget's size (UMG default 0.5, 0.5)

// the widget's layout geometry from its last paint: FGeometry (0x38) Size[2] +0, Scale +8, AbsolutePosition[2] +0xC
// (layout only: render transforms are in AccumulatedRenderTransform, not here), as floats g[0..4]. A widget that was
// never painted (Hidden) has an empty one.
static void geom(UObject *x, float g[14]) {
    uint8_t p[0x40] = {0};
    ue_process_event(x, f_geom, p);
    memcpy(g, p + p_geom_rv, 0x38);
}
static void translate(UObject *x, float tx, float ty) {
    float *t = W_RT(x);
    if (t[0] == tx && t[1] == ty) return;
    uint8_t p[32] = {0};
    float v[2] = {tx, ty};
    memcpy(p + p_translate_v, v, 8);
    ue_process_event(x, f_translate, p);
    n_applied++;
}
// uniform scale about the top-left corner, then a vertical move (design units of the parent)
static void scale_move(UObject *x, float s, float ty) {
    float *pv = W_PIVOT(x), *t = W_RT(x);
    if (pv[0] != 0 || pv[1] != 0) {
        uint8_t p[32] = {0};
        ue_process_event(x, f_pivot, p);   // (0, 0)
    }
    if (t[0] == 0 && fabsf(t[1] - ty) < 0.5f && fabsf(t[2] - s) < 0.002f && fabsf(t[3] - s) < 0.002f && !t[4] && !t[5] && !t[6]) return;
    uint8_t p[64] = {0};
    float v[7] = {0, ty, s, s, 0, 0, 0};
    memcpy(p + p_xform_v, v, sizeof v);
    ue_process_event(x, f_xform, p);
    n_applied++;
}

// a named widget in a UserWidget's own tree (not inside nested UserWidgets)
static UObject *find_named(UObject *w, const char *name, int depth) {
    char b[64];
    if (!w || depth > 8) return NULL;
    if (!strcmp(ue_name(U_NAME(w), b, sizeof b), name)) return w;
    if (!ue_is_a(w, c_panel)) return NULL;
    TArray *s = (TArray *)((char *)w + o_slots);
    for (int i = 0; i < s->num; i++) {
        UObject *sl = ((UObject **)s->data)[i], *c = sl ? *(UObject **)((char *)sl + o_content) : NULL;
        UObject *r = find_named(c, name, depth + 1);
        if (r) return r;
    }
    return NULL;
}

// ---- the tracked lists ----
enum { K_HUD, K_LOADOUTS };
typedef struct {
    UObject *w, *list, *mis;   // the list's UserWidget, its PlayerList, HUD: the objectives (GameHUDScreen's HUDMissions)
    int32_t idx, list_idx, mis_idx;
    int kind, touched, n, rows;
    float scale, dy;
} Panel;
static Panel panels[MAX_PANELS];
static int n_panels;

static int alive(UObject *o, int32_t idx) { return ue_object_usable(o, idx); }   // not freed or being destroyed by the GC

// the live HUD party panels and player lists (their Blueprint classes derive from the native ones directly: two
// reads per object); a list already tracked keeps its state
static void scan(void) {
    Panel found[MAX_PANELS];
    int nf = 0;
    char path[512];
    for (int32_t i = 0, k = ue_num_objects(); i < k && nf < MAX_PANELS; i++) {
        UObject *x = ue_object_at(i);
        UClass *k = x ? U_CLASS(x) : NULL, *sup = k ? US_SUPER(k) : NULL;
        int kind = k && c_hud && (k == c_hud || sup == c_hud) ? K_HUD : k && c_loadouts && (k == c_loadouts || sup == c_loadouts) ? K_LOADOUTS : -1;
        if (kind < 0 || (U_FLAGS(x) & 0x30) || !alive(x, i)) continue;
        if (strncmp(ue_full_path(x, path, sizeof path), "/Engine/Transient.", 18)) continue;   // live, not a template
        int j = 0;
        while (j < n_panels && !(panels[j].w == x && panels[j].idx == i)) j++;
        if (j < n_panels && alive(panels[j].list, panels[j].list_idx)) { found[nf++] = panels[j]; continue; }
        UObject *wt = *(UObject **)((char *)x + o_wt), *root = wt ? *(UObject **)((char *)wt + o_root) : NULL;
        UObject *list = find_named(root, "PlayerList", 0), *mis = NULL;
        if (!list) continue;
        if (kind == K_HUD) {   // PartyStatus -> WidgetTree -> GameHUDScreen_WBP, whose tree has HUDMissions (top left)
            UObject *t = U_OUTER(x), *scr = t && ue_is_a(t, c_wtree) ? U_OUTER(t) : NULL;
            UObject *swt = scr && ue_find_prop((UStruct *)U_CLASS(scr), "WidgetTree") ? *(UObject **)((char *)scr + o_wt) : NULL;
            mis = find_named(swt ? *(UObject **)((char *)swt + o_root) : NULL, "HUDMissions", 0);
        }
        found[nf++] = (Panel){x, list, mis, i, U_INDEX(list), mis ? U_INDEX(mis) : -1, kind, 0, 0, 0, 1, 0};
    }
    memcpy(panels, found, nf * sizeof *found);
    n_panels = nf;
}

// HUD: one column, scaled about its top-left corner and moved so that its bottom stays where the game's own
// bottom-aligned list ends (the box's bottom, above your own panel) and its top under the objectives' real bottom
// (+ fit_margin). Scale 1 when it fits (only moved up), never below fit_floor (then the top entries overlap the
// objectives, not your panel). on = hero team > 4 and 4+ teammates.
static void fit_hud(Panel *p, int on) {
    float lg[14], wg[14], mg[14];
    TArray *s = (TArray *)((char *)p->list + o_slots);
    int n = 0;
    for (int i = 0; i < s->num; i++) {
        UObject *sl = ((UObject **)s->data)[i], *c = sl ? *(UObject **)((char *)sl + o_content) : NULL;
        if (!c || !alive(c, U_INDEX(c))) return;   // the list is being torn down: leave it
        if (W_VIS(c) != 1) n++;
    }
    p->n = n; p->rows = n;
    if (!on || n < 4) {   // the game's own list: put back what we changed
        if (!p->touched) return;
        scale_move(p->list, 1, 0);
        p->touched = 0; p->scale = 1; p->dy = 0;
        LOG("hud: teammate list back to the game's (%d entries)", n);
        return;
    }
    geom(p->list, lg);
    geom(p->w, wg);
    if (lg[2] <= 0 || wg[2] <= 0 || lg[1] <= 0) return;   // not laid out yet
    float sc = lg[2], list_h = lg[1] * sc;                 // pixels
    float bottom = wg[4] + wg[1] * wg[2];                  // the box's bottom
    float top = wg[4];                                     // no objectives found: the box's top
    if (p->mis && alive(p->mis, p->mis_idx)) {
        geom(p->mis, mg);
        if (mg[2] > 0) top = W_VIS(p->mis) == 1 || W_VIS(p->mis) == 2 ? mg[4] : mg[4] + mg[1] * mg[2];
    }
    top += fit_margin * sc;
    float k = bottom > top ? (bottom - top) / list_h : 0;
    if (k > 1) k = 1;
    if (k < fit_floor) k = fit_floor;
    float ty = (bottom - k * list_h - lg[4]) / sc;   // from the list's laid-out top to its new top, design units
    if (!p->touched || fabsf(k - p->scale) > 0.05f)
        LOG("hud: teammate list fitted: %d entries, scale %.2f (room %.0f px for %.0f)", n, k, bottom - top, list_h);
    p->touched = 1; p->scale = k; p->dy = ty;
    scale_move(p->list, k, ty);
}

// character select: lineup.c's rows 5-8 as a second column next to the designer's 4 (the space under row 4 holds
// the hero portraits). Every row is the same widget at a fixed pitch that is more than its height (the gap is not in
// the UMG slots), so the pitch and the rows' offsets come from the cached layout geometry of painted rows (a Hidden
// bot row has none). on = hero team > 4.
static void layout_loadouts(Panel *p, int on) {
    TArray *s = (TArray *)((char *)p->list + o_slots);
    UObject *ent[MAX_ENTRIES];
    float lg[14], g[14], y0 = 0, y1 = 0, ew = 0;
    int n = 0, i0 = -1, i1 = -1;
    geom(p->list, lg);
    for (int i = 0; i < s->num && n < MAX_ENTRIES; i++) {
        UObject *sl = ((UObject **)s->data)[i], *c = sl ? *(UObject **)((char *)sl + o_content) : NULL;
        if (!c || !alive(c, U_INDEX(c))) return;   // the list is being torn down: leave it
        if (W_VIS(c) == 1) continue;           // collapsed: no room in the list
        ent[n] = c;
        if (lg[2] > 0) {
            geom(c, g);
            if (g[2] > 0 && g[1] > 0) {   // painted: its offset in the list, design units
                float y = (g[4] - lg[4]) / lg[2];
                if (i0 < 0) { i0 = n; y0 = y; ew = g[0]; }
                else { i1 = n; y1 = y; }
            }
        }
        n++;
    }
    int rows = on && n > 4 ? (n > 8 ? (n + 1) / 2 : 4) : n;
    p->n = n; p->rows = rows;
    if (rows == n) {   // one column: put back what we moved
        if (!p->touched) return;
        for (int i = 0; i < n; i++) translate(ent[i], 0, 0);
        p->touched = 0;
        LOG("hud: player list back to one column (%d entries)", n);
        return;
    }
    if (i1 < 0 || y1 <= y0 || ew <= 0) return;   // fewer than two rows laid out yet: next frame
    float pitch = (y1 - y0) / (i1 - i0);
    if (!p->touched) LOG("hud: player list in two columns: %d entries, %d rows", n, rows);
    p->touched = 1;
    for (int i = 0; i < n; i++) translate(ent[i], i < rows ? 0 : ew + col_gap, i < rows ? 0 : -rows * pitch);
}

static int was_active;
void hud_tick(float dt) {
    static float scan_acc = 1, team_acc = 1;
    static int big;
    if ((team_acc += dt) >= 0.5f) { team_acc = 0; big = hero_slots() > 4; }
    if (!big && !was_active) return;   // a hero team of 4 or less: nothing at all
    if (resolve()) return;
    if ((scan_acc += dt) >= 1.f) { scan_acc = 0; scan(); }   // lists come and go with screens and maps
    was_active = big;
    for (int i = 0; i < n_panels; i++) {
        Panel *p = &panels[i];
        if (!alive(p->w, p->idx) || !alive(p->list, p->list_idx)) continue;
        if (p->kind == K_HUD) fit_hud(p, big); else layout_loadouts(p, big);
        was_active |= p->touched;
    }
}

int hud_init(void) { return 0; }

#ifndef B4B_RELEASE
static int call_parms(UObject *o, const char *fn, void *p, size_t cap) {
    UFunction *f = o ? ue_find_function(U_CLASS(o), fn) : NULL;
    if (!f || UFN_PARMSSIZE(f) > cap) return -1;
    ue_process_event(o, f, p);
    return 0;
}
static int32_t poff(UObject *o, const char *fn, const char *prm) {
    UFunction *f = o ? ue_find_function(U_CLASS(o), fn) : NULL;
    FField *p = f ? ue_find_prop(f, prm) : NULL;
    return p ? FP_OFFSET(p) : -1;
}

// pixel rect of a widget (viewport pixels): cached geometry -> SlateBlueprintLibrary::LocalToViewport
static int widget_rect(UObject *w, float r[4]) {
    uint8_t g[0x40] = {0};
    int32_t gr = poff(w, "GetCachedGeometry", "ReturnValue");
    if (gr < 0) return -1;
    uint8_t p[0x80] = {0};
    if (call_parms(w, "GetCachedGeometry", p, sizeof p)) return -1;
    memcpy(g, p + gr, 0x38);
    UClass *sbl = ue_find_class("SlateBlueprintLibrary");
    UObject *cdo = sbl ? UC_CDO(sbl) : NULL;
    if (!cdo) return -1;
    int32_t ow = poff(cdo, "LocalToViewport", "WorldContextObject"), og = poff(cdo, "LocalToViewport", "Geometry"),
            ol = poff(cdo, "LocalToViewport", "LocalCoordinate"), opx = poff(cdo, "LocalToViewport", "PixelPosition"),
            sg = poff(cdo, "GetLocalSize", "Geometry"), sr = poff(cdo, "GetLocalSize", "ReturnValue");
    if (ow < 0 || og < 0 || ol < 0 || opx < 0 || sg < 0 || sr < 0) return -1;
    uint8_t q[0x100];
    memset(q, 0, sizeof q);
    memcpy(q + sg, g, 0x38);
    if (call_parms(cdo, "GetLocalSize", q, sizeof q)) return -1;
    float sz[2];
    memcpy(sz, q + sr, 8);
    float px[2][2];
    for (int k = 0; k < 2; k++) {
        memset(q, 0, sizeof q);
        *(UObject **)(q + ow) = w;
        memcpy(q + og, g, 0x38);
        float lc[2] = {k ? sz[0] : 0, k ? sz[1] : 0};
        memcpy(q + ol, lc, 8);
        if (call_parms(cdo, "LocalToViewport", q, sizeof q)) return -1;
        memcpy(px[k], q + opx, 8);
    }
    r[0] = px[0][0]; r[1] = px[0][1]; r[2] = px[1][0] - px[0][0]; r[3] = px[1][1] - px[0][1];
    return 0;
}

static void dump_node(Out *o, UObject *w, int depth, int maxdepth) {
    char cn[128], nm[128], sn[64] = "-";
    UClass *uw = ue_find_class("UserWidget"), *pw = ue_find_class("PanelWidget");
    UObject *slot = ue_get_ptr(w, "Slot");
    if (slot) ue_obj_name(U_CLASS(slot), sn, sizeof sn);
    int32_t vo = ue_prop_offset(w, "Visibility"), rto = ue_prop_offset(w, "RenderTransform");
    float r[4] = {0};
    int hr = widget_rect(w, r) == 0;
    out_printf(o, "%*s%s %s vis=%d slot=%s", depth * 2, "", ue_obj_name(U_CLASS(w), cn, sizeof cn), ue_obj_name(w, nm, sizeof nm),
               vo >= 0 ? *((uint8_t *)w + vo) : -1, sn);
    if (hr) out_printf(o, " px=(%.0f,%.0f %.0fx%.0f)", r[0], r[1], r[2], r[3]);
    if (rto >= 0) {
        float *t = (float *)((char *)w + rto);
        if (t[0] || t[1] || t[2] != 1 || t[3] != 1) out_printf(o, " rt=(%.0f,%.0f s%.2f,%.2f)", t[0], t[1], t[2], t[3]);
    }
    if (slot) {
        int32_t lo = ue_prop_offset(slot, "LayoutData"), pd = ue_prop_offset(slot, "Padding"), sz = ue_prop_offset(slot, "Size"),
                ha = ue_prop_offset(slot, "HorizontalAlignment"), va = ue_prop_offset(slot, "VerticalAlignment");
        if (lo >= 0) {
            float *l = (float *)((char *)slot + lo);
            out_printf(o, " off=(%.0f,%.0f,%.0f,%.0f) anch=(%.2f,%.2f,%.2f,%.2f) al=(%.2f,%.2f)", l[0], l[1], l[2], l[3], l[4], l[5], l[6], l[7], l[8], l[9]);
        }
        if (pd >= 0) { float *l = (float *)((char *)slot + pd); out_printf(o, " pad=(%.0f,%.0f,%.0f,%.0f)", l[0], l[1], l[2], l[3]); }
        if (sz >= 0 && lo < 0) out_printf(o, " size=(%.2f,%d)", *(float *)((char *)slot + sz), *((uint8_t *)slot + sz + 4));
        if (ha >= 0) out_printf(o, " ha=%d va=%d", *((uint8_t *)slot + ha), va >= 0 ? *((uint8_t *)slot + va) : -1);
    }
    out_printf(o, "\n");
    if (depth >= maxdepth) return;
    if (uw && ue_is_a(w, uw)) {
        UObject *wt = ue_get_ptr(w, "WidgetTree"), *root = wt ? ue_get_ptr(wt, "RootWidget") : NULL;
        if (root) dump_node(o, root, depth + 1, maxdepth);
    } else if (pw && ue_is_a(w, pw)) {
        TArray *s = arr(w, "Slots");
        for (int i = 0; s && i < s->num; i++) {
            UObject *sl = ((UObject **)s->data)[i], *c = sl ? ue_get_ptr(sl, "Content") : NULL;
            if (c) dump_node(o, c, depth + 1, maxdepth);
        }
    }
}

// matches a live widget by class name (exact) or full-path suffix
static int match(UObject *x, const char *what, char *path, size_t pl) {
    char cn[128];
    if (!strcmp(ue_obj_name(U_CLASS(x), cn, sizeof cn), what)) { ue_full_path(x, path, pl); return 1; }
    ue_full_path(x, path, pl);
    size_t a = strlen(path), b = strlen(what);
    return a >= b && !strcmp(path + a - b, what);
}

static void cmd_tree(const char *what, int maxdepth, Out *o) {
    UClass *wc = ue_find_class("Widget");
    char path[1024];
    int n = 0;
    for (int i = 0, m = ue_num_objects(); i < m && n < 6; i++) {
        UObject *x = ue_object_at(i);
        if (!x || !U_CLASS(x) || (U_FLAGS(x) & 0x30) || !ue_is_a(x, wc)) continue;
        if (!match(x, what, path, sizeof path)) continue;
        out_printf(o, "== %s\n", path);
        dump_node(o, x, 0, maxdepth);
        n++;
    }
    out_printf(o, "%d root(s)\n", n);
}

// hudxf <class|path suffix> <sx> <sy> [px py [tx ty]]: render transform on live widgets
static void cmd_xf(char *rest, Out *o) {
    char *what = strtok(rest, " ");
    float v[6] = {1, 1, 0, 0, 0, 0};
    int k = 0;
    for (char *t = strtok(NULL, " "); t && k < 6; t = strtok(NULL, " ")) v[k++] = (float)atof(t);
    if (!what) return;
    UClass *wc = ue_find_class("Widget");
    char path[1024];
    for (int i = 0, m = ue_num_objects(); i < m; i++) {
        UObject *x = ue_object_at(i);
        if (!x || !U_CLASS(x) || (U_FLAGS(x) & 0x30) || !ue_is_a(x, wc) || !match(x, what, path, sizeof path)) continue;
        uint8_t p[64] = {0};
        int32_t a = poff(x, "SetRenderScale", "Scale");
        memcpy(p + a, v, 8); call_parms(x, "SetRenderScale", p, sizeof p);
        memset(p, 0, sizeof p); a = poff(x, "SetRenderTransformPivot", "Pivot");
        memcpy(p + a, v + 2, 8); call_parms(x, "SetRenderTransformPivot", p, sizeof p);
        memset(p, 0, sizeof p); a = poff(x, "SetRenderTranslation", "Translation");
        memcpy(p + a, v + 4, 8); call_parms(x, "SetRenderTranslation", p, sizeof p);
        out_printf(o, "xf %s\n", path);
    }
}


static void cmd_hud(char *rest, Out *o) {
    char *a = rest ? strtok(rest, " ") : NULL, *v = a ? strtok(NULL, " ") : NULL;
    if (a && !strcmp(a, "floor") && v) fit_floor = (float)atof(v);
    else if (a && !strcmp(a, "margin") && v) fit_margin = (float)atof(v);
    else if (a && !strcmp(a, "gap") && v) col_gap = (float)atof(v);
    else if (a) { out_printf(o, "usage: hud [floor <scale> | margin <units> | gap <units>]\n"); return; }
    char b[256];
    out_printf(o, "hud: floor=%.2f margin=%.0f gap=%.0f hero_slots=%d panels=%d applied=%d active=%d\n", fit_floor, fit_margin,
               col_gap, hero_slots(), n_panels, n_applied, was_active);
    for (int i = 0; i < n_panels; i++) {
        Panel *p = &panels[i];
        int live = alive(p->w, p->idx);
        out_printf(o, "  [%d] %s live=%d entries=%d rows=%d touched=%d scale=%.2f list_dy=%.0f objectives=%d\n", i,
                   live ? ue_full_path(p->w, b, sizeof b) : "-", live, p->n, p->rows, p->touched, p->scale, p->dy, p->mis != NULL);
        if (!live || !alive(p->list, p->list_idx)) continue;
        TArray *s = (TArray *)((char *)p->list + o_slots);
        for (int j = 0; j < s->num; j++) {
            UObject *sl = ((UObject **)s->data)[j], *c = sl ? *(UObject **)((char *)sl + o_content) : NULL;
            if (!c) continue;
            float g[14], lg[14], *t = W_RT(c);
            geom(c, g);
            geom(p->list, lg);
            out_printf(o, "    %s vis=%d size=(%.0f,%.0f) y=%.0f translation=(%.0f,%.0f)\n", ue_obj_name(c, b, sizeof b), W_VIS(c),
                       g[0], g[1], lg[2] > 0 ? (g[4] - lg[4]) / lg[2] : -1, t[0], t[1]);
        }
    }
}
int hud_cmd(const char *verb, char *rest, Out *o) {
    if (!strcmp(verb, "hudtree") && rest) {
        char *what = strtok(rest, " "), *d = strtok(NULL, " ");
        cmd_tree(what, d ? atoi(d) : 12, o);
        return 1;
    }
    if (!strcmp(verb, "hudxf") && rest) { cmd_xf(rest, o); return 1; }
    if (!strcmp(verb, "hud")) { cmd_hud(rest, o); return 1; }
    return 0;
}
#endif  // !B4B_RELEASE
