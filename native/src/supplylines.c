// Supply lines on a client (issue #48): a player in someone else's Fort Hope got no new supply line after finishing
// one. docs/investigations/supply-lines.md.
//
// Supply lines (caravans): the tutorial line and the static chains (StaticCaravans; one line per chain at a time, the
// next once every item of the current one is unlocked; worked out from the player's unlocks, never saved) and the
// dynamic and skull-totem lines (generated with an expiry, saved in PlayerProfileData.Caravans). Each local player's
// UIScreenManager owns a UCaravanManager whose list (ProfileCaravanData) is what the supply-line screen shows.
// UpdateCaravans (0x141CB6740) rebuilds that list: it drops finished and expired lines, adds the next static line of
// each chain and generates dynamic ones (UpsertCaravan/RemoveCaravan on the player's own profile); it runs after every
// purchase or unlock (OnCommandExecuted) and on a 5 s timer, and returns at once while the manager is off (+0x78).
// Only the manager's profile-loaded handler (0x141CB65F0) turns it on: it is bound to the global "profile loaded"
// event when the manager is created and acts if the loaded component is the manager's own player's (0x141CB6590).
// On a network client the order is not fixed: the profile component (local) often loads before the screen manager and
// its caravan manager exist (log: "profile loaded", then the manager 0.3-2 s later), and that manager never hears
// the event and stays off for the whole visit: its list stays as it was (empty in a fresh camp visit), a bought-out
// line is never replaced, expired ones stay, no new line is generated. Same game build, two of three client joins
// seen here; on the host and in an offline camp the manager always comes first.
//
// Fix (client only, local, no protocol change): once the local manager has been off for 3 s while this machine is a
// network client and this player's profile is loaded (PPC +0x1B8, the flag set before that event), call the game's own
// profile-loaded handler for it, the call the event would have made. What follows is the game's own code on this
// player's own offline profile; nothing comes from the host. Kill switch: env B4BCOOP_NO_SUPPLYLINES=1 (repro).
// Dev: `supplylines` (manager state, saved and shown lines, static chain progress), `supplylines fix|off|update|
// auto on|off`, `supplylines buy <line#> [item#|all]` (the purchase the supply-line screen makes),
// `supplylines sp <N>`.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ue.h"
#include "log.h"
#include "cmds.h"

#define ADDR_CM_PROFILE_LOADED VA(0x141CB65F0ull)  // void UCaravanManager::HandleProfileLoaded(this, UGobiPlayerProfileComponent*)
#define ADDR_CM_OWN_PPC        VA(0x141CB6590ull)  // UGobiPlayerProfileComponent* UCaravanManager::GetProfileComponent(this)
#define ADDR_CM_UPDATE         VA(0x141CB6740ull)  // void UCaravanManager::UpdateCaravans(this)
#define ADDR_PPC_SET_LOADED    VA(0x141BBF2E0ull)  // void PPC::SetProfileLoaded(PPC, bool): +0x1B8, then the loaded/unloaded event
static const uint8_t SIG_LOADED[] = {0x48,0x89,0x5c,0x24,0x20,0x55,0x48,0x83,0xec,0x30,0x48,0x8b,0xda,0x48,0x8b,0xe9,
                                     0xe8,0x8b,0xff,0xff,0xff,0x48,0x3b,0xd8,0x0f,0x85,0x9a,0x00,0x00,0x00};
static const uint8_t SIG_ENABLE[] = {0xc6,0x45,0x78,0x01};   // HandleProfileLoaded +0x43: bEnabled = true
static const uint8_t SIG_OWN_PPC[] = {0x48,0x83,0xec,0x28,0x4c,0x8d,0x51,0x60,0x49,0x8b,0xca,0xe8,0x20,0x17,0xa4,0x00,
                                      0x84,0xc0,0x74,0x29};
static const uint8_t SIG_UPDATE[] = {0x40,0x55,0x41,0x55,0x48,0x8d,0xac,0x24,0x78,0xff,0xff,0xff,0x48,0x81,0xec,0x88,
                                     0x01,0x00,0x00,0x48,0x8b,0x05,0x86,0x10,0x74,0x04,0x48,0x33,0xc4,0x48,0x89,0x45,
                                     0x60,0x80,0x79,0x78,0x00};
static const uint8_t SIG_SET_LOADED[] = {0x40,0x53,0x48,0x83,0xec,0x50,0x48,0x8b,0xd9,0x38,0x91,0xb8,0x01,0x00,0x00};

#define CM_ENABLED(m)  (*((uint8_t *)(m) + 0x78))   // set by HandleProfileLoaded, cleared on profile unload
#define CM_UPDATING(m) (*((uint8_t *)(m) + 0x79))   // UpdateCaravans re-entrancy guard
#define CM_CACHED(m)   ((TArray *)((char *)(m) + 0x68))   // ProfileCaravanData.Caravans (reflected)
#define CM_STATIC(m)   (*(UObject **)((char *)(m) + 0x80)) // LoadedStaticCaravans (reflected)
#define SC_CHAINS(s)   ((TArray *)((char *)(s) + 0x68))   // StaticCaravans.CaravanChains
#define PPC_LOADED(p)  (*((uint8_t *)(p) + 0x1B8))         // set by PPC::SetProfileLoaded (0x141BBF2E0) before its broadcast
#define PROFILE_CARAVANS 0x218                             // PlayerProfileData.Caravans (TArray<FCaravanInstance>)
// FCaravanInstance (0x68): ID +0, Schema +8, Merchant +0x28 (row handles), Items +0x48 (TArray<FCaravanItem 0x28>:
// row handle +0, bool Purchased +0x20), Expiry +0x58 (int64, unix seconds)
typedef struct { UObject *table; FName row; FString display; } RowHandle;
#define CI_ID(c)       (*(int32_t *)((char *)(c) + 0x00))
#define CI_MERCHANT(c) ((RowHandle *)((char *)(c) + 0x28))
#define CI_ITEMS(c)    ((TArray *)((char *)(c) + 0x48))
#define CI_EXPIRY(c)   (*(int64_t *)((char *)(c) + 0x58))
#define PRODUCT_PRICE(r) (*(int32_t *)((char *)(r) + 0x28))   // ProductRow.Price, SkullTotemPrice +0x2C
#define PRODUCT_STP(r)   (*(int32_t *)((char *)(r) + 0x2C))
#define DT_ROWMAP(t)   ((TArray *)((char *)(t) + 0x38))

typedef void (*LoadedFn)(UObject *mgr, UObject *ppc);
typedef UObject *(*OwnPpcFn)(UObject *mgr);
typedef void (*UpdateFn)(UObject *mgr);

static int ok, auto_on = 1;
static UObject *seen_mgr;    // the manager the auto fix is watching
static int seen_idx = -1, off_secs, told_mismatch, age, was_on = -1;
static float acc;
static unsigned n_fixed;

static int is_net_client(void) {
    UObject *w = ue_world(), *nd = w ? ue_get_ptr(w, "NetDriver") : NULL;
    return nd && ue_get_ptr(nd, "ServerConnection");
}

// The local player's caravan manager: PlayerController.UIScreenManager (a MainMenuScreenManager in Fort Hope and the
// menus) -> CaravanManager. NULL elsewhere (missions).
static UObject *local_manager(UObject **pc_out) {
    UObject *pc = ue_local_pc(), *sm = pc ? ue_get_ptr(pc, "UIScreenManager") : NULL;
    if (pc_out) *pc_out = pc;
    return sm && ue_prop_offset(sm, "CaravanManager") >= 0 ? ue_get_ptr(sm, "CaravanManager") : NULL;
}

static UObject *local_ppc(UObject *pc) { return pc ? ue_get_ptr(pc, "GobiPlayerProfileComponent") : NULL; }

// The local player's offline profile (GobiLocalPlayer.PlayerProfileSettings.OfflineData), NULL if none.
static char *profile_data(UObject *pc) {
    UObject *lp = pc ? ue_get_ptr(pc, "Player") : NULL;
    UObject *s = lp && ue_prop_offset(lp, "PlayerProfileSettings") >= 0 ? ue_get_ptr(lp, "PlayerProfileSettings") : NULL;
    int32_t off = s ? ue_prop_offset(s, "OfflineData") : -1;
    return off < 0 ? NULL : (char *)s + off;
}

static int profile_loaded(UObject *ppc, UObject *pc) { return ppc && PPC_LOADED(ppc) && profile_data(pc); }

// 1 = the manager is on now (the game's own handler ran), 0 = not possible (why in *why)
static int enable(UObject *mgr, UObject *pc, const char **why) {
    UObject *mine = local_ppc(pc), *its = ((OwnPpcFn)ADDR_CM_OWN_PPC)(mgr);
    if (!profile_loaded(mine, pc)) { *why = "this player's profile is not loaded yet"; return 0; }
    if (its != mine) { *why = "the manager does not see this player's profile component yet"; return 0; }
    ((LoadedFn)ADDR_CM_PROFILE_LOADED)(mgr, mine);
    if (!CM_ENABLED(mgr)) { *why = "the game's handler did not turn it on"; return 0; }
    n_fixed++;
    return 1;
}

void supplylines_tick(float dt) {
    if (!ok || (acc += dt) < 1.f) return;
    acc = 0;
    int client = is_net_client();
    UObject *pc, *mgr = local_manager(&pc);
    if (!mgr || mgr != seen_mgr || U_INDEX(mgr) != seen_idx) {
        seen_mgr = mgr; seen_idx = mgr ? U_INDEX(mgr) : -1; off_secs = told_mismatch = 0; age = 0; was_on = -1;
        if (!mgr) return;
    }
    age++;
#ifndef B4B_RELEASE
    if (CM_ENABLED(mgr) != was_on) {   // dev: when the manager turns on (its profile-loaded event) after it appeared
        UObject *ps = pc ? ue_get_ptr(pc, "PlayerState") : NULL, *mine = local_ppc(pc);
        LOG("supplylines: %s manager %s %ds after it appeared (%d line(s) cached; its profile component %p, ours %p "
            "%s, PlayerState %p)", client ? "client" : "host/offline", CM_ENABLED(mgr) ? "on" : "off", age - 1,
            CM_CACHED(mgr)->num, (void *)((OwnPpcFn)ADDR_CM_OWN_PPC)(mgr), (void *)mine,
            profile_loaded(mine, pc) ? "loaded" : "not loaded", (void *)ps);
    }
#endif
    was_on = CM_ENABLED(mgr);
    if (!auto_on || !client) return;   // the host and an offline camp: the game's own order works
    if (CM_ENABLED(mgr) || CM_UPDATING(mgr)) { off_secs = 0; return; }
    if (++off_secs < 3) return;   // the game's own profile-loaded event gets the first chance
    const char *why = NULL;
    if (enable(mgr, pc, &why)) {
        LOG("supplylines: the supply-line manager missed this client's profile load; turned it on (%d line(s) now)",
            CM_CACHED(mgr)->num);
    } else if (!told_mismatch && off_secs >= 10) {
        told_mismatch = 1;
        LOG("supplylines: the supply-line manager is still off: %s", why);
    }
}

int supplylines_init(void) {
    if (memcmp((void *)ADDR_CM_PROFILE_LOADED, SIG_LOADED, sizeof SIG_LOADED) ||
        memcmp((void *)(ADDR_CM_PROFILE_LOADED + 0x43), SIG_ENABLE, sizeof SIG_ENABLE) ||
        memcmp((void *)ADDR_CM_OWN_PPC, SIG_OWN_PPC, sizeof SIG_OWN_PPC) ||
        memcmp((void *)ADDR_CM_UPDATE, SIG_UPDATE, sizeof SIG_UPDATE) ||
        memcmp((void *)ADDR_PPC_SET_LOADED, SIG_SET_LOADED, sizeof SIG_SET_LOADED)) {
        LOG("supplylines: signature mismatch, client supply-line fix off");
        return -1;
    }
    ok = 1;
    const char *e = getenv("B4BCOOP_NO_SUPPLYLINES");   // kill switch (and the bug's repro)
    if (e && *e && *e != '0') { auto_on = 0; LOG("supplylines: client fix off (B4BCOOP_NO_SUPPLYLINES)"); }
    return 0;
}

#ifndef B4B_RELEASE
#define ADDR_CM_FIRST_OPEN VA(0x141CBA0F0ull)  // int UCaravanManager::FirstIncompleteCaravanInChain(this, int chain)
#define ADDR_PPC_PURCHASE  VA(0x141BC1F40ull)  // void PPC::PurchaseCaravanItem(PPC, int caravan, int item, const FDataTableRowHandle*, int sp, int stp)
static const uint8_t SIG_FIRST_OPEN[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,
                                         0x48,0x89,0x7c,0x24,0x20,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x20,
                                         0x48,0x63,0xda,0x48,0x8b,0xe9};
static const uint8_t SIG_PURCHASE[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,
                                       0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x90,0x00,0x00,0x00};
typedef int (*FirstOpenFn)(UObject *mgr, int chain);
typedef void (*PurchaseFn)(UObject *ppc, int caravan, int item, const RowHandle *product, int sp, int stp);
#define ADDR_MM_BUSY VA(0x141AFE420ull)   // bool: a Matchmaking task is running (UpdateCaravans returns at once then)
static const uint8_t SIG_MM_BUSY[] = {0x48,0x8b,0x81,0x58,0x0b,0x00,0x00,0x48,0x63,0x89,0x60,0x0b,0x00,0x00};
typedef uint8_t (*MmBusyFn)(UObject *mm);
#define VT_ADJUST_SP VA(0x1454E8880ull)   // FAdjustSupplyPointsCommand vtable (cheats.c)

static int fname_eq(FName a, FName b) { return a.idx == b.idx && a.num == b.num; }

// TMap<FName, uint8*> RowMap: sparse array of {FName, row*, hash} (0x18) with its allocation bits
static uint8_t *dt_row(UObject *table, FName row) {
    TArray *m = DT_ROWMAP(table);
    const uint32_t *bits = *(uint32_t *const *)((char *)m + 0x20);
    if (!bits) bits = (const uint32_t *)((char *)m + 0x10);
    for (int i = 0; i < m->num && i < *(int32_t *)((char *)m + 0x28); i++) {
        if (!((bits[i >> 5] >> (i & 31)) & 1)) continue;
        char *e = (char *)m->data + (size_t)i * 0x18;
        if (fname_eq(*(FName *)e, row)) return *(uint8_t **)(e + 8);
    }
    return NULL;
}

static const char *handle_name(const RowHandle *h, char *buf, size_t n) {
    size_t k = 0;
    for (int i = 0; h->display.data && i < h->display.num && h->display.data[i] && k + 1 < n; i++)
        buf[k++] = h->display.data[i] < 128 ? (char)h->display.data[i] : '?';
    buf[k] = 0;
    return k ? buf : ue_name(h->row, buf, n);
}

static TArray *profile_lines(UObject *pc) {
    char *d = profile_data(pc);
    return d ? (TArray *)(d + PROFILE_CARAVANS) : NULL;
}

static void print_lines(Out *o, TArray *lines) {
    long long now = (long long)time(NULL);
    for (int i = 0; i < lines->num; i++) {
        char *c = (char *)lines->data + (size_t)i * 0x68;
        TArray *it = CI_ITEMS(c);
        int bought = 0;
        for (int k = 0; k < it->num; k++) bought += *((uint8_t *)it->data + (size_t)k * 0x28 + 0x20) != 0;
        char m[96];
        long long ex = CI_EXPIRY(c);
        out_printf(o, "  #%d id %d %s: %d/%d bought%s", i, CI_ID(c), handle_name(CI_MERCHANT(c), m, sizeof m), bought,
                   it->num, bought == it->num && it->num ? " (COMPLETE)" : "");
        if (ex > 0) out_printf(o, ", %s %lld min", ex < now ? "expired" : "expires in", (ex < now ? now - ex : ex - now) / 60);
        out_printf(o, "\n");
    }
}

static void status(Out *o) {
    UObject *pc, *mgr = local_manager(&pc);
    UObject *mine = local_ppc(pc);
    out_printf(o, "supplylines: %s, auto fix %s, fixed %u time(s)\n", is_net_client() ? "network client" : "host/offline",
               auto_on ? "on" : "off", n_fixed);
    if (!mgr) { out_printf(o, "no supply-line manager here (Fort Hope only)\n"); return; }
    UObject *its = ((OwnPpcFn)ADDR_CM_OWN_PPC)(mgr);
    out_printf(o, "manager %p: %s%s, %d line(s) cached; its profile component %p, this player's %p (%s)\n", (void *)mgr,
               CM_ENABLED(mgr) ? "ON" : "OFF", CM_UPDATING(mgr) ? " (updating)" : "", CM_CACHED(mgr)->num, (void *)its,
               (void *)mine, profile_loaded(mine, pc) ? "loaded" : "not loaded");
    UObject *gi = ue_find_first_of("GobiGameInstance"), *mm = gi ? ue_get_ptr(gi, "Matchmaking") : NULL;
    if (mm && !memcmp((void *)ADDR_MM_BUSY, SIG_MM_BUSY, sizeof SIG_MM_BUSY))
        out_printf(o, "matchmaking busy (UpdateCaravans waits): %d\n", ((MmBusyFn)ADDR_MM_BUSY)(mm));
    UObject *sc = CM_STATIC(mgr);
    if (sc && !memcmp((void *)ADDR_CM_FIRST_OPEN, SIG_FIRST_OPEN, sizeof SIG_FIRST_OPEN)) {
        TArray *ch = SC_CHAINS(sc);
        out_printf(o, "static chains: %d, first open line per chain:", ch->num);
        for (int i = 0; i < ch->num; i++) out_printf(o, " %d", ((FirstOpenFn)ADDR_CM_FIRST_OPEN)(mgr, i));
        out_printf(o, " (-1 = chain done)\n");
    }
    TArray *lines = profile_lines(pc);
    if (!lines) { out_printf(o, "no offline profile\n"); return; }
    int32_t *sp = (int32_t *)profile_data(pc);   // SupplyPoints {balanceFromInventory, acquired, spent}
    out_printf(o, "profile: %d SP (acquired %d, spent %d), %d line(s) saved:\n", sp[1] - sp[2], sp[1], sp[2], lines->num);
    print_lines(o, lines);
    out_printf(o, "the supply-line screen shows the manager's %d line(s) (static lines are never saved):\n", CM_CACHED(mgr)->num);
    print_lines(o, CM_CACHED(mgr));
}

// The purchase the supply-line screen makes (UCaravanProgressScreen::MakePurchaseRequest -> 0x141BC1F40): one
// PurchaseCaravanItem command on this player's profile, priced from the product row. <line#> as `supplylines` lists
// the screen's lines (the manager's), or the saved ones while the manager has none.
static void buy(char *rest, Out *o) {
    UObject *pc, *mgr = local_manager(&pc);
    UObject *mine = local_ppc(pc);
    int shown = mgr && CM_CACHED(mgr)->num > 0;   // what the screen shows; else the saved lines
    TArray *lines = shown ? CM_CACHED(mgr) : profile_lines(pc);
    char *a = rest ? strtok(rest, " ") : NULL, *b = a ? strtok(NULL, " ") : NULL;
    if (!a || !lines || !mine) { out_printf(o, "usage: supplylines buy <line#> [item#|all] (in Fort Hope)\n"); return; }
    if (memcmp((void *)ADDR_PPC_PURCHASE, SIG_PURCHASE, sizeof SIG_PURCHASE)) { out_printf(o, "signature mismatch\n"); return; }
    int li = atoi(a);
    if (li < 0 || li >= lines->num) { out_printf(o, "no line #%d\n", li); return; }
    char *c = (char *)lines->data + (size_t)li * 0x68;
    int id = CI_ID(c), n = CI_ITEMS(c)->num, all = b && !strcmp(b, "all"), only = b && !all ? atoi(b) : -1;
    FName merchant = CI_MERCHANT(c)->row;   // static lines all have id -1: the merchant tells them apart
    int done = 0;
    for (int k = 0; k < n; k++) {
        lines = shown ? CM_CACHED(mgr) : profile_lines(pc);   // each purchase may rebuild the array
        c = NULL;
        for (int j = 0; lines && j < lines->num; j++) {
            char *x = (char *)lines->data + (size_t)j * 0x68;
            if (CI_ID(x) == id && fname_eq(CI_MERCHANT(x)->row, merchant)) c = x;
        }
        if (!c) { out_printf(o, "line %d is gone\n", id); break; }
        if (k >= CI_ITEMS(c)->num) break;
        char *item = (char *)CI_ITEMS(c)->data + (size_t)k * 0x28;
        if (item[0x20] || (only >= 0 && k != only)) continue;
        RowHandle h = *(RowHandle *)item;   // the game copies it (FString included) into its command
        uint8_t *row = h.table ? dt_row(h.table, h.row) : NULL;
        char nm[96];
        handle_name(&h, nm, sizeof nm);
        int sp = row ? PRODUCT_PRICE(row) : 0, stp = row ? PRODUCT_STP(row) : 0;
        ((PurchaseFn)ADDR_PPC_PURCHASE)(mine, id, k, &h, sp, stp);
        LOG("supplylines: bought item %d (%s) of line %d for %d SP %d STP", k, nm, id, sp, stp);
        out_printf(o, "bought item %d (%s) of line %d for %d SP %d STP\n", k, nm, id, sp, stp);
        done++;
        if (!all && only < 0) break;   // default: the next item only
    }
    if (!done) out_printf(o, "nothing to buy\n");
}

int supplylines_cmd(const char *verb, char *rest, Out *o) {
    if (strcmp(verb, "supplylines")) return 0;
    if (!ok) { out_printf(o, "supplylines: not available (signature mismatch)\n"); return 1; }
    char *sub = rest ? strtok(rest, " ") : NULL, *arg = sub ? strtok(NULL, "") : NULL;
    UObject *pc, *mgr = local_manager(&pc);
    if (!sub) status(o);
    else if (!strcmp(sub, "auto") && arg) {
        auto_on = !strcmp(arg, "on");
        out_printf(o, "supplylines: auto fix %s\n", auto_on ? "on" : "off");
    } else if (!strcmp(sub, "fix")) {
        const char *why = NULL;
        if (!mgr) out_printf(o, "no supply-line manager here\n");
        else if (CM_ENABLED(mgr)) out_printf(o, "the manager is already on\n");
        else if (enable(mgr, pc, &why)) out_printf(o, "manager on, %d line(s)\n", CM_CACHED(mgr)->num);
        else out_printf(o, "not turned on: %s\n", why);
    } else if (!strcmp(sub, "off")) {   // repro: the state a missed profile load leaves (what the unload handler does)
        if (!mgr) out_printf(o, "no supply-line manager here\n");
        else { CM_ENABLED(mgr) = 0; out_printf(o, "manager off (%d line(s) stay cached)\n", CM_CACHED(mgr)->num); }
    } else if (!strcmp(sub, "update")) {
        if (!mgr) out_printf(o, "no supply-line manager here\n");
        else {
            ((UpdateFn)ADDR_CM_UPDATE)(mgr);
            out_printf(o, "UpdateCaravans ran (manager %s)\n", CM_ENABLED(mgr) ? "on" : "off: returns at once");
        }
    } else if (!strcmp(sub, "buy")) buy(arg, o);
    else if (!strcmp(sub, "sp") && arg) {   // test profiles: supply points to buy with (AdjustSupplyPoints on our own)
        const uint8_t *gt = *(const uint8_t *const *)(VT_ADJUST_SP + 8);   // GetType: mov al, 5; ret
        struct { uintptr_t vt; int32_t delta, pad; } cmd = {VT_ADJUST_SP, atoi(arg), 0};
        if (!gt || gt[0] != 0xb0 || gt[1] != 5 || gt[2] != 0xc3) out_printf(o, "signature mismatch\n");
        else if (rewards_execute_local(local_ppc(pc), &cmd)) out_printf(o, "not available\n");
        else { LOG("supplylines: dev: %+d SP on our own profile", cmd.delta); out_printf(o, "%+d SP\n", cmd.delta); }
    } else out_printf(o, "usage: supplylines [fix | off | update | auto on|off | buy <line#> [item#|all] | sp <N>]\n");
    return 1;
}
#endif
