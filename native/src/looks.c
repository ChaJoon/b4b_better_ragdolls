// The `~` window's "Looks & add-ons" tab: one place to find add-ons, get them, wear or use what they add, and manage
// what is installed. It replaces the Models, Add-ons and Browse tabs (docs/investigations/overlay.md); the code of each
// part stays in its module:
//   Wear       models.c models_wear_panel (+ weaponlooks.c wlooks_panel): your look, the game's survivors' outfits and
//              pieces, NPC bodies, add-on outfits and weapon looks; the host's "change the look of"; everyone's look.
//   Installed  addons.c addons_installed_panel: the add-ons in load order, on/off, kind and state, Wear / Use, Update
//              and Remove (shop.c), Up / Down, details under the row, conflicts, the folder.
//   Get more   shop.c shop_get_panel: the add-on shop (ini shop=0 hides it); a row offers Wear / Use once an add-on is
//              ready.
//   One search box for all three. Host rules (collapsed, under every mode): model swaps (models.c) and who may join
//   with add-ons (addons_mp.c). Every action is the same chat command / function the old tabs used.
// Dev: `overlay tab Models|Add-ons|Browse|Wear|Installed|Get more` shows the tab in that mode (overlay_add_alias).
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "cmds.h"
#include "overlay.h"

#define TAB "Looks & add-ons"

static int mode = LOOKS_WEAR, wear_list, get_filter;
static char search[64];

void looks_show(int m, const char *s, int list) {
    mode = m;
    if (s) snprintf(search, sizeof search, "%s", s);
    if (list >= 0) wear_list = list;
}
static void alias(int m) { mode = m; }

static void panel(void) {
    static const char *MODES[] = {"Wear", "Installed", "Get more"};
    int shop = shop_enabled();
    if (!shop && mode == LOOKS_GET) mode = LOOKS_INSTALLED;
    ov_tabs("##looksmode", MODES, shop ? 3 : 2, &mode);
    static const char *HINT[] = {"search looks", "search your add-ons", "search the shop"};
    ov_width(14);
    ov_input_text("##looksearch", search, sizeof search, HINT[mode]);
    ov_tooltip("One search for Wear, Installed and Get more.");
    if (search[0]) {
        ov_same_line();
        if (ov_button("Clear##looksearch")) search[0] = 0;
    }
    if (mode == LOOKS_GET) {
        static const char *F[] = {"All", "New looks (ready at once)", "Replacements", "Not installed yet"};
        ov_same_line();
        ov_width(13);
        ov_combo("##shopfilter", &get_filter, F, 4);
    }
    if (mode == LOOKS_WEAR) models_wear_panel(search, &wear_list);
    else if (mode == LOOKS_INSTALLED) addons_installed_panel(search);
    else shop_get_panel(search, get_filter);
    if (ov_header("Host rules###hostrules", 0)) {
        models_host_panel();
        addons_mp_panel();
    }
}

void looks_init(void) {
    overlay_add_panel(TAB, 60, panel);
    static const struct { const char *name; int mode; } A[] = {
        {"Models", LOOKS_WEAR}, {"Wear", LOOKS_WEAR}, {"Add-ons", LOOKS_INSTALLED}, {"Installed", LOOKS_INSTALLED},
        {"Browse", LOOKS_GET}, {"Get more", LOOKS_GET}, {"Looks", LOOKS_WEAR}};
    for (size_t i = 0; i < sizeof A / sizeof A[0]; i++) overlay_add_alias(A[i].name, TAB, alias, A[i].mode);
}
