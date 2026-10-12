// Bots open pinged toolkit doors (supply-stash doors that need a toolkit): host only, ini bot_toolkit_doors=0 turns it
// off. Why retail bots often ignore such a ping and what the host changes: the "the fix" section below and
// docs/investigations/bot-pings.md.
//
// Dev commands (`botdoor ...`; `bots` is the chat command /bots): `botdoor` (bot controllers: hero, ping-usable query,
// interact target, toolkits of door 0's row), `botdoor doors [all]` (toolkit doors / all Door actors: state, usable,
// required item), `botdoor tags <door#>`, `botdoor ping <door#>` (the local player pings the door: ServerSpawnPing with
// its PingableComponent, on a client a real RPC), `botdoor look <door#>` / `lookat <hex> [dz]` (aim, then `face ping`
// for a real ping), `botdoor tp <door#> [+|-] [me|bots]` (heroes onto the navmesh in front of / behind it),
// `botdoor far <door#> <min m> <max m> [me|bots]` (heroes onto a reachable point that far away), `botdoor give
// <bot#|me>` (the door's required item), `botdoor target <bot#> <door#> [score[u]]` (SetInteractTarget, door or usable),
// `botdoor move <bot#> <door#> goto|walk|tether|untether|clear|script [radius]` (BotController's static helpers),
// `botdoor send <door#>` (the fix's bot choice without a ping), `botdoor fix [on|off]`, `botdoor watch` (log every
// ClearInteractTarget with its reason and caller), `botdoor call <hex> <Func>`.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "MinHook.h"
#include "ue.h"
#include "log.h"
#include "cmds.h"

typedef struct { UObject *table; FName row; FString display; } RowHandle;   // FDataTableRowHandle (0x20)

static int is_live(UObject *o) { return o && !(U_FLAGS(o) & 0x30); }
static UFunction *fn_of(UObject *o, const char *name) { return o ? ue_find_function(U_CLASS(o), name) : NULL; }
static int32_t parm_off(UFunction *f, const char *p) { FField *x = f ? ue_find_prop(f, p) : NULL; return x ? FP_OFFSET(x) : -1; }

static UObject *in_world(UObject *o, UObject *w) {   // the object's world-level outer chain ends in this world
    for (UObject *p = U_OUTER(o); p; p = U_OUTER(p)) if (p == w) return o;
    return NULL;
}

// an actor of the loaded map: the persistent level or a streamed sublevel (whose own UWorld is not ue_world())
static int in_map(UObject *o) {
    char fp[300];
    return !strncmp(ue_full_path(o, fp, sizeof fp), "/Game/Maps/", 11);
}

static int actor_loc(UObject *a, float out[3]) {
    UFunction *f = fn_of(a, "K2_GetActorLocation");
    uint8_t p[32] = {0};
    if (!f) return 0;
    ue_process_event(a, f, p);
    memcpy(out, p + parm_off(f, "ReturnValue"), 12);
    return 1;
}

static float dist3(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// ---- doors ----
// reflected offsets (sdk: Door, UsableComponent); FUsableInventoryCondition: RequiredInventoryItem +0x28,
// RequiredItemCount +0x48 (handle + 0x20)
#define DOOR_REQUEST_LOCKED(d) (*((uint8_t *)(d) + 0x4C9))   // bRequestLocked
#define DOOR_DESIRED(d)        (*((uint8_t *)(d) + 0x4C8))   // DesiredDoorState
#define USABLE_INV_CONDS(u)    ((TArray *)((char *)(u) + 0x538))   // InventoryConditions (0x58 each)
#define USABLE_ENABLED(u)      (*((uint8_t *)(u) + 0x280))   // bIsEnabled
#define USABLE_REQ_QUICKUSE(u) (*((uint8_t *)(u) + 0x284))   // bRequireQuickUseItem

static int door_state(UObject *d) {
    UFunction *f = fn_of(d, "GetDoorState");
    uint8_t p[16] = {0};
    if (!f) return -1;
    ue_process_event(d, f, p);
    return p[0];
}

// the door's usable that needs an inventory item (toolkit doors); NULL for plain doors
static UObject *door_item_usable(UObject *d, RowHandle **req) {
    static UClass *uc;
    if (!uc) uc = ue_find_class("UsableComponent");
    for (int32_t i = 0, n = ue_num_objects(); uc && i < n; i++) {
        UObject *x = ue_object_at(i);
        if (!x || U_OUTER(x) != d || !is_live(x) || !ue_is_a(x, uc)) continue;
        TArray *c = USABLE_INV_CONDS(x);
        if (c->num > 0 && c->data) { if (req) *req = (RowHandle *)((char *)c->data + 0x28); return x; }
    }
    return NULL;
}

static UObject *inventory_of(UObject *pawn) {
    UFunction *gi = fn_of(pawn, "GetInventoryComponent");
    uint8_t p[16] = {0};
    if (!gi) return NULL;
    ue_process_event(pawn, gi, p);
    return *(UObject **)p;
}

static int item_count(UObject *pawn, const RowHandle *h) {
    UObject *inv = pawn ? inventory_of(pawn) : NULL;
    UFunction *f = fn_of(inv, "ItemCountByRowHandle");
    int32_t oh = parm_off(f, "RowHandle"), orv = parm_off(f, "ReturnValue");
    uint8_t p[64] = {0};
    if (oh < 0 || orv < 0 || !h) return -1;
    RowHandle *q = (RowHandle *)(p + oh);
    q->table = h->table; q->row = h->row;
    ue_process_event(inv, f, p);
    return *(int32_t *)(p + orv);
}

static int toolkit_doors = 1;          // ini bot_toolkit_doors=0 turns it off
static UObject *scripting_target(UObject *scr);
static const char *send_bot_to_door(UObject *door, UObject *pinger, char *why, size_t wl);

#ifndef B4B_RELEASE
// first component of class `cls` owned by actor (UActorComponent outer = the actor for SCS/native components)
static UObject *component_of(UObject *actor, UClass *cls) {
    if (!actor || !cls) return NULL;
    for (int32_t i = 0, n = ue_num_objects(); i < n; i++) {
        UObject *x = ue_object_at(i);
        if (x && U_OUTER(x) == actor && is_live(x) && ue_is_a(x, cls)) return x;
    }
    return NULL;
}

static UObject *nth_door(int want, int all, UObject **usable_out) {
    static UClass *dc;
    if (!dc) dc = ue_find_class("Door");
    UObject *w = ue_world();
    int k = 0;
    for (int32_t i = 0, n = ue_num_objects(); dc && w && i < n; i++) {
        UObject *x = ue_object_at(i);
        if (!x || !is_live(x) || !ue_is_a(x, dc) || !in_map(x)) continue;
        UObject *u = door_item_usable(x, NULL);
        if (!all && !u) continue;
        if (k++ == want) { if (usable_out) *usable_out = u; return x; }
    }
    return NULL;
}

// ---- bots ----
static UObject *nth_bot(int want) {
    static UClass *bc;
    if (!bc) bc = ue_find_class("BotController");
    UObject *w = ue_world();
    int k = 0;
    for (int32_t i = 0, n = ue_num_objects(); bc && w && i < n; i++) {
        UObject *x = ue_object_at(i);
        if (!x || !is_live(x) || !ue_is_a(x, bc) || !in_world(x, w) || !ue_get_ptr(x, "Pawn")) continue;
        if (k++ == want) return x;
    }
    return NULL;
}

static UObject *scripting_of(UObject *ctl) {
    static UClass *sc;
    if (!sc) sc = ue_find_class("BotAIScriptingComponent");
    UObject *c = component_of(ctl, sc);
    return c ? c : component_of(ue_get_ptr(ctl, "Pawn"), sc);
}

static UObject *interact_target(UObject *ctl) {
    UObject *s = scripting_of(ctl);
    UFunction *f = fn_of(s, "GetInteractTarget");
    uint8_t p[16] = {0};
    if (!f) return NULL;
    ue_process_event(s, f, p);
    return *(UObject **)p;
}

static void cmd_bots_list(Out *o) {
    char a[160], b[160], c[160], e[160];
    RowHandle *req = NULL;
    UObject *u0 = NULL, *d0 = nth_door(0, 0, &u0);
    if (u0) door_item_usable(d0, &req);
    for (int i = 0; ; i++) {
        UObject *ctl = nth_bot(i);
        if (!ctl) break;
        UObject *pawn = ue_get_ptr(ctl, "Pawn"), *t = interact_target(ctl);
        const uint8_t *q = (const uint8_t *)ctl + 0x3468;   // active playstyle config +0x3000: AwarenessConfig.UsableActorFromPingTagQuery
        float at[3] = {0};
        actor_loc(pawn, at);
        out_printf(o, "#%d %p %s pawn=%s at=(%.0f %.0f %.0f) pingUsableQuery: dict=%d tokens=%d  interact=%s%s%s toolkit(door0 row)=%d\n",
                   i, (void *)ctl, ue_obj_name(ctl, a, sizeof a), ue_obj_name(pawn, b, sizeof b), at[0], at[1], at[2],
                   *(int32_t *)(q + 0x10), *(int32_t *)(q + 0x20), t ? ue_obj_name(t, c, sizeof c) : "-",
                   t && U_OUTER(t) ? " on " : "", t && U_OUTER(t) ? ue_obj_name(U_OUTER(t), e, sizeof e) : "",
                   req ? item_count(pawn, req) : -2);
    }
}

static void cmd_bots_doors(Out *o, int all) {
    char a[200], b[160], r[80];
    for (int i = 0; ; i++) {
        UObject *u = NULL, *d = nth_door(i, all, &u);
        if (!d) break;
        RowHandle *req = NULL;
        if (u) door_item_usable(d, &req);
        float at[3] = {0};
        actor_loc(d, at);
        out_printf(o, "[%d] %s (%s) at=(%.0f %.0f %.0f) state=%d desired=%d reqLocked=%d", i, ue_obj_name(d, a, sizeof a),
                   ue_obj_name(U_CLASS(d), b, sizeof b), at[0], at[1], at[2], door_state(d), DOOR_DESIRED(d), DOOR_REQUEST_LOCKED(d));
        if (u) out_printf(o, " usable=%s enabled=%d reqQuickUse=%d item=%s/%s x%d", ue_obj_name(u, b, sizeof b), USABLE_ENABLED(u),
                          USABLE_REQ_QUICKUSE(u), req && req->table ? ue_obj_name(req->table, a, sizeof a) : "-",
                          req ? ue_name(req->row, r, sizeof r) : "-", req ? *(int32_t *)((char *)req + 0x20) : 0);
        out_printf(o, "\n");
    }
}

// the local player pings the door: PlayerWaypointsComponent::ServerSpawnPing(PC, Transform, PingActor, PingableComponent)
static void cmd_bots_ping(Out *o, int di, int all) {
    UObject *d = nth_door(di, all, NULL), *pc = ue_local_pc(), *pw = pc ? ue_get_ptr(pc, "PlayerWaypoints") : NULL;
    UObject *pg = d ? ue_get_ptr(d, "PingableComponent") : NULL;
    UFunction *f = fn_of(pw, "ServerSpawnPing");
    if (!d || !pg || !f) { out_printf(o, "no door %d / PingableComponent / PlayerWaypoints\n", di); return; }
    static uint8_t p[256];
    memset(p, 0, sizeof p);
    float at[3];
    actor_loc(d, at);
    *(UObject **)(p + parm_off(f, "OwnerController")) = pc;
    float *t = (float *)(p + parm_off(f, "Transform"));
    t[3] = 1.f; memcpy(t + 4, at, 12); t[8] = t[9] = t[10] = 1.f;
    p[parm_off(f, "Type")] = 1;   // EWaypointType::PingActor
    *(UObject **)(p + parm_off(f, "PingTarget")) = pg;
    LOG("bots: ServerSpawnPing on door %d", di);
    ue_process_event(pw, f, p);
    char a[160];
    out_printf(o, "bots: ServerSpawnPing(PingActor, %s) by the local player\n", ue_obj_name(pg, a, sizeof a));
}

static void cmd_bots_target(Out *o, int bi, int di, float score, int usable) {
    UObject *ctl = nth_bot(bi), *u = NULL, *d = nth_door(di, 0, &u), *s = ctl ? scripting_of(ctl) : NULL;
    UFunction *f = fn_of(s, "SetInteractTarget");
    if (!f || !u) { out_printf(o, "no bot %d scripting component / toolkit door %d\n", bi, di); return; }
    uint8_t p[32] = {0};
    *(UObject **)(p + parm_off(f, "InteractTarget")) = usable ? u : d;
    *(float *)(p + parm_off(f, "TargetScore")) = score;
    ue_process_event(s, f, p);
    out_printf(o, "bots: SetInteractTarget(door %d %s, %.1f) on bot %d -> %d\n", di, usable ? "usable" : "actor", score, bi,
               p[parm_off(f, "ReturnValue")]);
}

// botdoor give <bot#|me>: one of door 0's required item (the toolkit) into that hero's inventory
static void cmd_bots_give(Out *o, const char *who) {
    UObject *u = NULL, *d = nth_door(0, 0, &u), *pc = ue_local_pc();
    RowHandle *req = NULL;
    if (u) door_item_usable(d, &req);
    UObject *pawn = !strcmp(who, "me") ? (pc ? ue_get_ptr(pc, "Pawn") : NULL) : ue_get_ptr(nth_bot(atoi(who)), "Pawn");
    UObject *inv = pawn ? inventory_of(pawn) : NULL;
    UFunction *f = fn_of(inv, "ServerAddItemsOfHandle");
    if (!req || !f) { out_printf(o, "no toolkit door / hero %s\n", who); return; }
    uint8_t p[64] = {0};
    RowHandle *h = (RowHandle *)(p + parm_off(f, "ItemHandle"));
    h->table = req->table; h->row = req->row;
    *(int32_t *)(p + parm_off(f, "NumItems")) = 1;
    ue_process_event(inv, f, p);
    out_printf(o, "botdoor: gave the toolkit to %s: count now %d\n", who, item_count(pawn, req));
}

static int nav_project(const float in[3], float out[3]) {
    UClass *k = ue_find_class("NavigationSystemV1");
    UFunction *f = k ? ue_find_function(k, "K2_ProjectPointToNavigation") : NULL;
    static uint8_t p[128];
    if (!f) return 0;
    memset(p, 0, sizeof p);
    *(UObject **)(p + parm_off(f, "WorldContextObject")) = ue_local_pc();
    memcpy(p + parm_off(f, "Point"), in, 12);
    float ext[3] = {100.f, 100.f, 250.f};
    memcpy(p + parm_off(f, "QueryExtent"), ext, 12);
    ue_process_event(UC_CDO(k), f, p);
    memcpy(out, p + parm_off(f, "ProjectedLocation"), 12);
    return p[parm_off(f, "ReturnValue")];
}

static void actor_vec(UObject *a, const char *fn, float out[3]) {
    UFunction *f = fn_of(a, fn);
    uint8_t p[32] = {0};
    out[0] = out[1] = out[2] = 0;
    if (f) { ue_process_event(a, f, p); memcpy(out, p + parm_off(f, "ReturnValue"), 12); }
}

static void set_loc(UObject *a, const float v[3]) {
    UFunction *f = fn_of(a, "K2_SetActorLocation");
    static uint8_t p[512];
    memset(p, 0, sizeof p);
    memcpy(p + parm_off(f, "NewLocation"), v, 12);
    p[parm_off(f, "bTeleport")] = 1;
    ue_process_event(a, f, p);
}

// botdoor tp <door#> [+|-] [me|bots|all]: heroes onto the navmesh 150-250 cm in front of (+) / behind (-) the door
static void cmd_bots_tp(Out *o, int di, const char *side, const char *who) {
    UObject *d = nth_door(di, 0, NULL), *pc = ue_local_pc(), *me = pc ? ue_get_ptr(pc, "Pawn") : NULL;
    float at[3], fw[3], rt[3];
    if (!d || !actor_loc(d, at)) { out_printf(o, "no toolkit door %d\n", di); return; }
    actor_vec(d, "GetActorForwardVector", fw);
    actor_vec(d, "GetActorRightVector", rt);
    float sg = side && side[0] == '-' ? -1.f : 1.f;
    for (int s = -1; s <= 1; s += 2) {
        float c[3] = {at[0] + fw[0] * 200.f * s, at[1] + fw[1] * 200.f * s, at[2] + 50.f}, q[3];
        int ok = nav_project(c, q);
        out_printf(o, "side %c: nav %s (%.0f %.0f %.0f)\n", s > 0 ? '+' : '-', ok ? "ok" : "NONE", q[0], q[1], q[2]);
    }
    static UClass *hc;
    if (!hc) hc = ue_find_class("HeroCharacter");
    int k = 0;
    for (int32_t i = 0, n = ue_num_objects(); hc && i < n; i++) {
        UObject *h = ue_object_at(i);
        if (!h || !is_live(h) || !ue_is_a(h, hc) || !in_map(h)) continue;
        if (who && !strcmp(who, "me") && h != me) continue;
        if (who && !strcmp(who, "bots") && h == me) continue;
        float dist = 160.f + 60.f * (float)(k / 3), lat = 70.f * (float)((k % 3) - 1);
        float c[3] = {at[0] + fw[0] * dist * sg + rt[0] * lat, at[1] + fw[1] * dist * sg + rt[1] * lat, at[2] + 50.f}, q[3];
        if (!nav_project(c, q)) { out_printf(o, "  hero %d: no navmesh there\n", k); k++; continue; }
        q[2] += 100.f;
        set_loc(h, q);
        char nm[128];
        out_printf(o, "  %s -> (%.0f %.0f %.0f)\n", ue_obj_name(h, nm, sizeof nm), q[0], q[1], q[2]);
        k++;
    }
}

// botdoor look <door#>: the local player looks at the door (pings aim with the camera)
static void cmd_bots_look(Out *o, int di) {
    UObject *d = nth_door(di, 1, NULL), *pc = ue_local_pc(), *me = pc ? ue_get_ptr(pc, "Pawn") : NULL;
    UObject *u = NULL, *td = nth_door(di, 0, &u);
    if (td) d = td;
    float at[3], el[3];
    if (!d || !me || !actor_loc(d, at)) { out_printf(o, "no door %d / pawn\n", di); return; }
    UFunction *f = fn_of(me, "GetActorEyesViewPoint");
    static uint8_t p[128];
    memset(p, 0, sizeof p); ue_process_event(me, f, p); memcpy(el, p + parm_off(f, "OutLocation"), 12);
    float v[3] = {at[0] - el[0], at[1] - el[1], at[2] + 110.f - el[2]}, n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    float r[3] = {n > 0 ? asinf(v[2] / n) * 57.29578f : 0.f, atan2f(v[1], v[0]) * 57.29578f, 0.f};
    f = fn_of(pc, "SetControlRotation");
    memset(p, 0, sizeof p); memcpy(p + parm_off(f, "NewRotation"), r, 12); ue_process_event(pc, f, p);
    out_printf(o, "botdoor: looking at door %d from %.0f cm (pitch %.1f yaw %.1f)\n", di, n, r[0], r[1]);
}

// botdoor lookat <hexaddr> [dz]: the local player looks at that actor from where it stands
static void cmd_bots_lookat(Out *o, UObject *a, float dz) {
    UObject *pc = ue_local_pc(), *me = pc ? ue_get_ptr(pc, "Pawn") : NULL;
    float at[3], el[3];
    if (!a || !me || !actor_loc(a, at)) { out_printf(o, "no actor / pawn\n"); return; }
    UFunction *f = fn_of(me, "GetActorEyesViewPoint");
    static uint8_t p[128];
    memset(p, 0, sizeof p); ue_process_event(me, f, p); memcpy(el, p + parm_off(f, "OutLocation"), 12);
    float v[3] = {at[0] - el[0], at[1] - el[1], at[2] + dz - el[2]}, n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    float r[3] = {n > 0 ? asinf(v[2] / n) * 57.29578f : 0.f, atan2f(v[1], v[0]) * 57.29578f, 0.f};
    f = fn_of(pc, "SetControlRotation");
    memset(p, 0, sizeof p); memcpy(p + parm_off(f, "NewRotation"), r, 12); ue_process_event(pc, f, p);
    out_printf(o, "botdoor: looking at (%.0f %.0f %.0f) from %.0f cm\n", at[0], at[1], at[2] + dz, n);
}

// botdoor call <hexaddr> <Func>: a parameterless function on that object, the parameter block as hex
static void cmd_bots_call(Out *o, UObject *x, const char *fn) {
    UFunction *f = x ? fn_of(x, fn) : NULL;
    static uint8_t p[512];
    if (!f || UFN_PARMSSIZE(f) > sizeof p) { out_printf(o, "no %s\n", fn); return; }
    memset(p, 0, sizeof p);
    ue_process_event(x, f, p);
    out_printf(o, "%s:", fn);
    for (int i = 0; i < UFN_PARMSSIZE(f) && i < 48; i++) out_printf(o, " %02x", p[i]);
    out_printf(o, "\n");
}

// botdoor move <bot#> <door#> goto|tether|script [radius]: BotController's static helpers on that bot
//   goto: GoToLocation(BotChar, nav point in front of the door); tether: AddTether(BotChar, door, Hero, loc, radius, 60 s);
//   script: SetScriptTarget(BotChar, door, bForce 1, bForceImmediate 1); untether: RemoveTether(BotChar, door)
static void cmd_bots_move(Out *o, int bi, int di, const char *how, float radius) {
    UObject *ctl = nth_bot(bi), *pawn = ctl ? ue_get_ptr(ctl, "Pawn") : NULL, *d = nth_door(di, 0, NULL);
    UClass *bc = ue_find_class("BotController");
    float at[3], fw[3], q[3];
    if (!pawn || !d || !bc || !actor_loc(d, at)) { out_printf(o, "no bot %d / door %d\n", bi, di); return; }
    actor_vec(d, "GetActorForwardVector", fw);
    float c[3] = {at[0] + fw[0] * 120.f, at[1] + fw[1] * 120.f, at[2] + 50.f};
    if (!nav_project(c, q)) memcpy(q, c, 12);
    if (!strcmp(how, "walk")) {   // BotAIScriptingComponent::SetGoToLocation(loc, bShouldTether 1, bShouldTeleportOnFail 0)
        UObject *scr = scripting_of(ctl);
        UFunction *g = fn_of(scr, "SetGoToLocation");
        uint8_t gp[32] = {0};
        if (!g) { out_printf(o, "no SetGoToLocation\n"); return; }
        memcpy(gp + parm_off(g, "GoToLocation"), q, 12);
        gp[parm_off(g, "bShouldTether")] = radius > 0;
        ue_process_event(scr, g, gp);
        out_printf(o, "botdoor: SetGoToLocation(bot %d -> %.0f %.0f %.0f, tether %d)\n", bi, q[0], q[1], q[2], radius > 0);
        return;
    }
    const char *fn = !strcmp(how, "goto") ? "GoToLocation" : !strcmp(how, "tether") ? "AddTether" :
                     !strcmp(how, "untether") ? "RemoveTether" : !strcmp(how, "clear") ? "ClearTethers" : "SetScriptTarget";
    UFunction *f = ue_find_function(bc, fn);
    static uint8_t p[256];
    if (!f) { out_printf(o, "no %s\n", fn); return; }
    memset(p, 0, sizeof p);
    *(UObject **)(p + parm_off(f, "BotChar")) = pawn;
    int32_t k;
    if ((k = parm_off(f, "DestLoc")) >= 0) memcpy(p + k, q, 12);
    if ((k = parm_off(f, "TetherActor")) >= 0) *(UObject **)(p + k) = d;
    if ((k = parm_off(f, "TetherLoc")) >= 0) memcpy(p + k, q, 12);
    if ((k = parm_off(f, "Radius")) >= 0) *(float *)(p + k) = radius;
    if ((k = parm_off(f, "Lifetime")) >= 0) *(float *)(p + k) = 60.f;
    if ((k = parm_off(f, "ScriptTarget")) >= 0) *(UObject **)(p + k) = d;
    if ((k = parm_off(f, "bForce")) >= 0) p[k] = 1;
    if ((k = parm_off(f, "bForceImmediate")) >= 0) p[k] = 1;
    ue_process_event(UC_CDO(bc), f, p);
    k = parm_off(f, "ReturnValue");
    out_printf(o, "botdoor: %s(bot %d, door %d) at (%.0f %.0f %.0f) -> %d\n", fn, bi, di, q[0], q[1], q[2], k >= 0 ? p[k] : -1);
}

// botdoor far <door#> <min m> <max m> [me|bots|all]: heroes onto a navmesh point reachable from the door's front,
// between min and max metres away from it (random tries, same floor within 2 m)
static void cmd_bots_far(Out *o, int di, float mn, float mx, const char *who) {
    UObject *d = nth_door(di, 0, NULL), *pc = ue_local_pc(), *me = pc ? ue_get_ptr(pc, "Pawn") : NULL;
    UClass *k = ue_find_class("NavigationSystemV1");
    UFunction *f = k ? ue_find_function(k, "K2_GetRandomReachablePointInRadius") : NULL;
    float at[3], fw[3], q[3];
    if (!d || !f || !actor_loc(d, at)) { out_printf(o, "no door %d / nav\n", di); return; }
    actor_vec(d, "GetActorForwardVector", fw);
    float c[3] = {at[0] + fw[0] * 150.f, at[1] + fw[1] * 150.f, at[2] + 50.f}, front[3];
    if (!nav_project(c, front)) { out_printf(o, "door front not on navmesh\n"); return; }
    static uint8_t p[128];
    int found = 0;
    for (int t = 0; t < 400 && !found; t++) {
        memset(p, 0, sizeof p);
        *(UObject **)(p + parm_off(f, "WorldContextObject")) = pc;
        memcpy(p + parm_off(f, "Origin"), front, 12);
        *(float *)(p + parm_off(f, "Radius")) = mx * 100.f;
        ue_process_event(UC_CDO(k), f, p);
        if (!p[parm_off(f, "ReturnValue")]) continue;
        memcpy(q, p + parm_off(f, "RandomLocation"), 12);
        float dd = dist3(q, front);
        if (dd >= mn * 100.f && dd <= mx * 100.f && fabsf(q[2] - front[2]) < 200.f) found = 1;
    }
    if (!found) { out_printf(o, "no reachable point %.0f-%.0f m from door %d\n", mn, mx, di); return; }
    static UClass *hc;
    if (!hc) hc = ue_find_class("HeroCharacter");
    int n = 0;
    for (int32_t i = 0, m = ue_num_objects(); hc && i < m; i++) {
        UObject *h = ue_object_at(i);
        if (!h || !is_live(h) || !ue_is_a(h, hc) || !in_map(h)) continue;
        if (who && !strcmp(who, "me") && h != me) continue;
        if (who && !strcmp(who, "bots") && h == me) continue;
        float v[3] = {q[0] + 60.f * (float)(n % 3) - 60.f, q[1] + 60.f * (float)(n / 3), q[2] + 100.f};
        set_loc(h, v);
        n++;
    }
    out_printf(o, "botdoor: %d hero(es) to (%.0f %.0f %.0f), %.1f m from door %d's front\n", n, q[0], q[1], q[2],
               dist3(q, front) / 100.f, di);
}

// botdoor watch: log every UBotAIScriptingComponent::ClearInteractTarget (0x1415B6A50, this, const FString& reason) with
// the reason and the caller (static VA) while a target is set
#define ADDR_CLEAR_INTERACT VA(0x1415B6A50ull)
static const uint8_t SIG_CLEAR_INTERACT[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20,0x48,
                                             0x89,0x54,0x24,0x10,0x55,0x41,0x56,0x41,0x57};
typedef void (*ClearInteractFn)(UObject *scr, const FString *reason);
static ClearInteractFn orig_clear_interact;
static void clear_interact_detour(UObject *scr, const FString *reason) {
    UObject *t = scripting_target(scr);
    if (t) {
        char r[200], nm[128], ow[128];
        size_t j = 0;
        for (int i = 0; reason && reason->data && i < reason->num && reason->data[i] && j + 1 < sizeof r; i++)
            r[j++] = reason->data[i] < 128 ? (char)reason->data[i] : '?';
        r[j] = 0;
        void *bt[4];
        USHORT nb = RtlCaptureStackBackTrace(1, 4, bt, NULL);
        LOG("bots: ClearInteractTarget(%s) on %s (target %s) from %llx %llx", r, ue_obj_name(U_OUTER(scr), ow, sizeof ow),
            ue_obj_name(t, nm, sizeof nm), nb > 0 ? (unsigned long long)((uintptr_t)bt[0] - g_base_delta) : 0ull,
            nb > 1 ? (unsigned long long)((uintptr_t)bt[1] - g_base_delta) : 0ull);
    }
    orig_clear_interact(scr, reason);
}
static void cmd_bots_watch(Out *o) {
    if (!orig_clear_interact) {
        if (memcmp((void *)ADDR_CLEAR_INTERACT, SIG_CLEAR_INTERACT, sizeof SIG_CLEAR_INTERACT)) { out_printf(o, "signature mismatch\n"); return; }
        if (MH_CreateHook((void *)ADDR_CLEAR_INTERACT, (void *)clear_interact_detour, (void **)&orig_clear_interact) != MH_OK ||
            MH_EnableHook((void *)ADDR_CLEAR_INTERACT) != MH_OK) { orig_clear_interact = NULL; out_printf(o, "hook failed\n"); return; }
    }
    out_printf(o, "botdoor: logging ClearInteractTarget\n");
}

// botdoor tags <door#>: the door's gameplay tags (GameplayTagsComponent) and the bots' ping-usable query tags
static void print_tags(Out *o, const TArray *t) {
    char nm[128];
    for (int i = 0; t && t->data && i < t->num && i < 32; i++) out_printf(o, " %s", ue_name(((FName *)t->data)[i], nm, sizeof nm));
}
static void cmd_bots_tags(Out *o, int di) {
    static UClass *tc;
    if (!tc) tc = ue_find_class("GameplayTagsComponent");
    UObject *d = nth_door(di, 0, NULL), *gt = component_of(d, tc), *ctl = NULL;
    if (gt) { out_printf(o, "door %d tags:", di); print_tags(o, (TArray *)((char *)gt + 0x130)); out_printf(o, "\n"); }
    else out_printf(o, "door %d: no GameplayTagsComponent\n", di);
    for (int i = 0; (ctl = nth_bot(i)); i++) {
        const uint8_t *q = (const uint8_t *)ctl + 0x3468;
        out_printf(o, "bot %d ping-usable query dict:", i); print_tags(o, (const TArray *)(q + 8));
        out_printf(o, "  tokens:");
        const TArray *tk = (const TArray *)(q + 0x18);
        for (int j = 0; tk->data && j < tk->num && j < 32; j++) out_printf(o, " %d", ((uint8_t *)tk->data)[j]);
        out_printf(o, "\n");
    }
}

int bots_cmd(const char *verb, char *rest, Out *o) {
    if (strcmp(verb, "botdoor")) return 0;
    char *a = rest ? strtok(rest, " ") : NULL, *b = a ? strtok(NULL, " ") : NULL, *c = b ? strtok(NULL, " ") : NULL;
    char *d = c ? strtok(NULL, " ") : NULL;
    if (!a) cmd_bots_list(o);
    else if (!strcmp(a, "doors")) cmd_bots_doors(o, b && !strcmp(b, "all"));
    else if (!strcmp(a, "ping") && b) cmd_bots_ping(o, atoi(b), c && !strcmp(c, "all"));
    else if (!strcmp(a, "target") && c) cmd_bots_target(o, atoi(b), atoi(c), d ? (float)atof(d) : 1.f, d && strchr(d, 'u'));
    else if (!strcmp(a, "tp") && b) cmd_bots_tp(o, atoi(b), c, d);
    else if (!strcmp(a, "look") && b) cmd_bots_look(o, atoi(b));
    else if (!strcmp(a, "tags") && b) cmd_bots_tags(o, atoi(b));
    else if (!strcmp(a, "give") && b) cmd_bots_give(o, b);
    else if (!strcmp(a, "lookat") && b) cmd_bots_lookat(o, (UObject *)(uintptr_t)strtoull(b, NULL, 16), c ? (float)atof(c) : 0.f);
    else if (!strcmp(a, "call") && c) cmd_bots_call(o, (UObject *)(uintptr_t)strtoull(b, NULL, 16), c);
    else if (!strcmp(a, "move") && d) {
        char *e = strtok(NULL, " ");
        cmd_bots_move(o, atoi(b), atoi(c), d, e ? (float)atof(e) : 300.f);
    }
    else if (!strcmp(a, "far") && d) {
        char *e = strtok(NULL, " ");
        cmd_bots_far(o, atoi(b), (float)atof(c), (float)atof(d), e);
    }
    else if (!strcmp(a, "watch")) cmd_bots_watch(o);
    else if (!strcmp(a, "fix")) {
        if (b) toolkit_doors = !strcmp(b, "on");
        out_printf(o, "botdoor: pinged toolkit doors fix %s\n", toolkit_doors ? "on" : "off");
    } else if (!strcmp(a, "send") && b) {
        char why[200];
        UObject *dr = nth_door(atoi(b), 0, NULL);
        out_printf(o, "botdoor send: %s\n", dr ? send_bot_to_door(dr, ue_local_pc(), why, sizeof why) : "no toolkit door");
    }
    else out_printf(o, "usage: botdoor | doors [all] | tags|look|ping|tp|far|send <door#> ... | give <bot#|me> | "
                       "target <bot#> <door#> [score[u]] | move <bot#> <door#> goto|walk|tether|untether|clear|script | "
                       "fix [on|off] | watch | lookat <hex> [dz] | call <hex> <Func> (see bots.c)\n");
    return 1;
}
#endif

// ---- the fix: a pinged toolkit door gets a bot that carries a toolkit (host) ----
// Retail (docs/investigations/bot-pings.md): every bot evaluates a ping once, in its ping system (0x1415E1A60). Only an
// actor ping (EWaypointType::PingActor) of an actor inside the bot's tether counts (TetherSystem 0x1415E9AB0: 12 m, 7 m
// in combat, around its follow target, the teammate closest to the objective); the door's usable must then score > 0
// for that bot (it holds the toolkit; the score needs the door inside the tether too). So nothing happens when
//   - the door lies outside the tether when it is pinged (a ping from further away, or off the follow target's way);
//     the ping is not looked at again;
//   - the ping became a location ping: SpawnPing traces on the "Pingable" channel first, and a teammate between the
//     camera and the door takes the hit (not pingable), so the ping falls back to a location (on the teammate or the
//     door's surface; EWaypointType::PingLocation, no target). Bots ignore location pings (bPingAllowsMoves = 0).
// The host:
//   1. a location ping aimed at a closed toolkit door (the pinger's eyes -> ping point ray crosses the door, with only
//      teammates in the way) is handed to the game as that door's actor ping (the door's icon for everyone);
//   2. after the game's own ServerSpawnPing_Implementation, if no bot took the door, the nearest bot holding the door's
//      required item (40 m at most) gets the door as its interact target, as retail's accepted ping does (ping-system
//      count 0x1415E0A10, then SetInteractTarget, 1.0);
//   3. for that bot and door (until the door opens, 30 s at most) the door (actor 0x1415E9AB0, and any point within 4 m
//      of it, 0x1415E96E0) counts as inside the bot's tether, and the tether update's ClearAllTargets (0x1415B5D50
//      called at 0x1415DDD80, when bot and target are both outside the tether) is skipped: otherwise the bot turns
//      back halfway, is teleported back to the team, or drops the door on arrival.
// Host only: ServerSpawnPing_Implementation runs on the server, bots exist only there. ini bot_toolkit_doors=0 = retail.
#define ADDR_SPAWNPING_IMPL VA(0x141E887C0ull)   // UPlayerWaypointsComponent::ServerSpawnPing_Implementation(this, PC,
                                                   // const FTransform&, EWaypointType, UActorComponent* PingTarget)
static const uint8_t SIG_SPAWNPING[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,
                                        0x48,0x83,0xec,0x30,0x41,0x0f,0xb6,0xf9,0x49,0x8b,0xf0,0x48,0x8b,0xea};
#define ADDR_PING_COUNT VA(0x1415E0A10ull)       // FBotPingSystem: +1 for the actor (this, unused, AActor*)
static const uint8_t SIG_PING_COUNT[] = {0x4d,0x85,0xc0,0x0f,0x84,0xca,0x01,0x00,0x00,0x48,0x8b,0xc4,0x48,0x89,0x50,0x10,
                                         0x48,0x81,0xec,0x88,0x00,0x00,0x00};
#define ADDR_TETHER_ACTOR VA(0x1415E9AB0ull)     // bool FTetherSystem::IsActorInside(this, AActor*, float extra, out*)
static const uint8_t SIG_TETHER_ACTOR[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x50,0x0f,
                                           0x29,0x74,0x24,0x40,0x0f,0x28,0xf2,0x49,0x8b,0xf1,0x48,0x8b,0xda};
#define ADDR_TETHER_LOC VA(0x1415E96E0ull)       // bool FTetherSystem::IsLocationInside(this, const FVector*, float, out*)
static const uint8_t SIG_TETHER_LOC[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,
                                         0x48,0x83,0xec,0x50,0xf3,0x0f,0x10,0x0a,0x49,0x8b,0xe9};
#define TETHER_OWNER(t) (*(UObject **)((char *)(t) + 0x10))   // FTetherSystem.OwnerBotCtrl (reflected, +0x10)
#define ADDR_CLEAR_TARGETS VA(0x1415B5D50ull)    // void UBotAIScriptingComponent::ClearAllTargets(this, bool, bool)
#define ADDR_TETHER_CLEAR_RET VA(0x1415DDD85ull) // return address of its call in the bots' tether update
static const uint8_t SIG_CLEAR_TARGETS[] = {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x30,0x48,0x8b,0xd9,0x84,0xd2,0x75,0x24};

typedef void (*SpawnPingFn)(UObject *comp, UObject *pc, const void *xform, uint8_t type, UObject *target);
typedef void (*PingCountFn)(void *pingsys, void *unused, UObject *actor);
typedef uint8_t (*TetherActorFn)(void *tether, UObject *actor, float extra, void *out);
typedef uint8_t (*TetherLocFn)(void *tether, const float *loc, float extra, void *out);
typedef void (*ClearTargetsFn)(UObject *scr, uint8_t a, uint8_t b);
static SpawnPingFn orig_spawnping;
static PingCountFn ping_count;
static TetherActorFn orig_tether_actor;
static TetherLocFn orig_tether_loc;
static ClearTargetsFn orig_clear_targets;
static const float MAX_DIST = 4000.f;   // a bot further from the door is not sent (cm; bots teleport back beyond 35 m)
#define MAX_SENT 4
#define MAX_BOTS 16

// doors a bot was sent to: game-thread only (ping RPC, bot tick). Object indices guard against a map change.
static struct {
    UObject *ctl, *scr, *door, *usable;
    int32_t ctl_i, scr_i, door_i, usable_i;
    float at[3];   // the door's location
    ULONGLONG until;
} sent[MAX_SENT];
static volatile LONG n_sent;   // entries in use (hot-path early out)

static int still(UObject *o, int32_t i) { return o && ue_object_at(i) == o; }

static int job_open(int i) {   // the door is still closed and its usable enabled, within the time limit
    static int32_t o_desired = -1, o_enabled = -1;
    if (!sent[i].ctl) return 0;
    int alive = still(sent[i].ctl, sent[i].ctl_i) && still(sent[i].scr, sent[i].scr_i) &&
                still(sent[i].door, sent[i].door_i) && still(sent[i].usable, sent[i].usable_i);
    if (alive && o_desired < 0) o_desired = ue_prop_offset(sent[i].door, "DesiredDoorState");
    if (alive && o_enabled < 0) o_enabled = ue_prop_offset(sent[i].usable, "bIsEnabled");
    if (alive && GetTickCount64() < sent[i].until && o_desired >= 0 && o_enabled >= 0 &&
        *((uint8_t *)sent[i].door + o_desired) == 0 && *((uint8_t *)sent[i].usable + o_enabled)) return 1;
    memset(&sent[i], 0, sizeof sent[i]);
    InterlockedDecrement(&n_sent);
    return 0;
}

static void remember_sent(UObject *ctl, UObject *scr, UObject *door, UObject *usable) {
    int k = -1;
    for (int i = 0; i < MAX_SENT && k < 0; i++) if (sent[i].ctl == ctl || !job_open(i)) k = i;
    if (k < 0) k = 0;
    if (!sent[k].ctl) InterlockedIncrement(&n_sent);
    sent[k].ctl = ctl; sent[k].scr = scr; sent[k].door = door; sent[k].usable = usable;
    sent[k].ctl_i = U_INDEX(ctl); sent[k].scr_i = U_INDEX(scr); sent[k].door_i = U_INDEX(door); sent[k].usable_i = U_INDEX(usable);
    actor_loc(door, sent[k].at);
    sent[k].until = GetTickCount64() + 30000;
}

static uint8_t tether_actor_detour(void *tether, UObject *actor, float extra, void *out) {
    if (n_sent && actor && tether)
        for (int i = 0; i < MAX_SENT; i++)
            if (sent[i].door == actor && sent[i].ctl == TETHER_OWNER(tether) && job_open(i)) return 1;
    return orig_tether_actor(tether, actor, extra, out);
}

static uint8_t tether_loc_detour(void *tether, const float *loc, float extra, void *out) {
    if (n_sent && loc && tether)
        for (int i = 0; i < MAX_SENT; i++)
            if (sent[i].ctl && sent[i].ctl == TETHER_OWNER(tether) && dist3(loc, sent[i].at) < 400.f && job_open(i)) return 1;
    return orig_tether_loc(tether, loc, extra, out);
}

static UObject *scripting_target(UObject *scr) {
    UFunction *f = fn_of(scr, "GetInteractTarget");
    uint8_t p[16] = {0};
    if (!f) return NULL;
    ue_process_event(scr, f, p);
    return *(UObject **)p;
}

static void clear_targets_detour(UObject *scr, uint8_t a, uint8_t b) {
    if (n_sent)
        for (int i = 0; i < MAX_SENT; i++) {
            if (sent[i].scr != scr || !job_open(i)) continue;
            UObject *t = scripting_target(scr);   // the door, or its usable (the bots' own usable search picks that)
            int ours = t && (t == sent[i].door || U_OUTER(t) == sent[i].door);
            if (ours && __builtin_return_address(0) == (void *)ADDR_TETHER_CLEAR_RET) return;
#ifndef B4B_RELEASE
            LOG("bots: ClearAllTargets(%d, %d) on a sent bot from %llx, target %s", a, b,
                (unsigned long long)((uintptr_t)__builtin_return_address(0) - g_base_delta), ours ? "the door" : "other");
#endif
        }
    orig_clear_targets(scr, a, b);
}

// bots driving a survivor: AI controllers of the hero team's slot pawns (PlayerSlotManager.TeamSlots[0]; NPCs such
// as Emmett have no slot), with their BotAIScriptingComponent
static int collect_bots(UObject **ctl, UObject **scr, int max) {
    static UClass *bc, *sc;
    if (!bc) { bc = ue_find_class("BotController"); sc = ue_find_class("BotAIScriptingComponent"); }
    UObject *w = ue_world(), *gs = w ? ue_get_ptr(w, "GameState") : NULL;
    UObject *psm = gs ? ue_get_ptr(gs, "PlayerSlotManager") : NULL;
    int32_t ot = psm ? ue_prop_offset(psm, "TeamSlots") : -1;
    TArray *teams = ot >= 0 ? (TArray *)((char *)psm + ot) : NULL;
    TArray *slots = teams && teams->num ? (TArray *)((uint8_t *)teams->data + 8) : NULL;   // team 0's slot array
    int n = 0;
    if (!bc || !sc || !slots) return 0;
    for (int i = 0; i < slots->num && n < max; i++) {
        UObject *slot = ((UObject **)slots->data)[i], *pawn = slot ? ue_get_ptr(slot, "AssignedPawn") : NULL;
        UObject *c = pawn ? ue_get_ptr(pawn, "Controller") : NULL;
        if (c && is_live(c) && ue_is_a(c, bc)) { ctl[n] = c; scr[n] = NULL; n++; }
    }
    for (int32_t i = 0, m = ue_num_objects(); n && i < m; i++) {
        UObject *x = ue_object_at(i);
        if (!x || !is_live(x) || !ue_is_a(x, sc)) continue;
        for (int k = 0; k < n; k++) if (U_OUTER(x) == ctl[k] || U_OUTER(x) == ue_get_ptr(ctl[k], "Pawn")) scr[k] = x;
    }
    return n;
}

static int pawn_alive(UObject *pawn) {   // AGobiCharacter::IsAlive (BlueprintPure), else assume alive
    UFunction *f = fn_of(pawn, "IsAlive");
    uint8_t p[16] = {0};
    if (!f) return 1;
    ue_process_event(pawn, f, p);
    return p[parm_off(f, "ReturnValue")];
}

// the nearest bot holding the door's required item goes to open it; returns what happened (log, dev command)
static const char *send_bot_to_door(UObject *door, UObject *pinger, char *why, size_t wl) {
    RowHandle *req = NULL;
    UObject *u = door_item_usable(door, &req);
    (void)pinger;
    if (!u || !req || !USABLE_ENABLED(u) || door_state(door) != 0) return "not a closed toolkit door";
    if (!ping_count) return "off";
    int need = *(int32_t *)((char *)req + 0x20);   // FUsableInventoryCondition.RequiredItemCount
    if (need < 1) need = 1;
    UObject *ctl[MAX_BOTS], *scr[MAX_BOTS];
    int n = collect_bots(ctl, scr, MAX_BOTS), best = -1, holders = 0;
    float at[3], bd = 0;
    actor_loc(door, at);
    for (int k = 0; k < n; k++)
        if (scr[k] && scripting_target(scr[k]) == door) { snprintf(why, wl, "bot %d is already on it", k); return why; }
    for (int k = 0; k < n; k++) {
        UObject *pawn = ue_get_ptr(ctl[k], "Pawn");
        float bl[3];
        if (!scr[k] || !pawn || !pawn_alive(pawn) || item_count(pawn, req) < need || !actor_loc(pawn, bl)) continue;
        holders++;
        float dd = dist3(bl, at);
        if (dd <= MAX_DIST && (best < 0 || dd < bd)) { best = k; bd = dd; }
    }
    if (best < 0) { snprintf(why, wl, "no bot with the item within %.0f m (%d bot(s), %d holder(s))", MAX_DIST / 100.f, n, holders); return why; }
    UFunction *f = fn_of(scr[best], "SetInteractTarget");
    int32_t ps = ue_prop_offset(ctl[best], "PingSystem"), ot = parm_off(f, "InteractTarget"), os = parm_off(f, "TargetScore"),
            orv = parm_off(f, "ReturnValue");
    uint8_t p[32] = {0};
    if (ps < 0 || ot < 0 || os < 0 || orv < 0) return "no SetInteractTarget / PingSystem";
    remember_sent(ctl[best], scr[best], door, u);   // before: SetInteractTarget already asks the tether
    ping_count((char *)ctl[best] + ps, NULL, door);
    *(UObject **)(p + ot) = door;
    *(float *)(p + os) = 1.f;
    ue_process_event(scr[best], f, p);
    char nm[128];
    snprintf(why, wl, "sent %s (%.0f m away)%s", ue_obj_name(ue_get_ptr(ctl[best], "Pawn"), nm, sizeof nm), bd / 100.f,
             p[orv] ? "" : ", refused");
    return why;
}

// Nothing but the door or a hero between the two points (KismetSystemLibrary::LineTraceSingle, Visibility, the pinger
// ignored): a ping that hit a teammate in front of the pinger still "sees" a door further on.
static int sight_to_door(UObject *pawn, const float from[3], const float to[3], UObject *door) {
    static UClass *k, *hc;
    if (!k) { k = ue_find_class("KismetSystemLibrary"); hc = ue_find_class("HeroCharacter"); }
    UFunction *f = k ? ue_find_function(k, "LineTraceSingle") : NULL;
    static uint8_t p[512];
    static UObject *ignore[1];
    int32_t oh = parm_off(f, "OutHit"), oa = parm_off(f, "ActorsToIgnore"), orv = parm_off(f, "ReturnValue");
    if (!f || UFN_PARMSSIZE(f) > sizeof p || oh < 0 || oa < 0 || orv < 0) return 0;
    memset(p, 0, sizeof p);
    *(UObject **)(p + parm_off(f, "WorldContextObject")) = pawn;
    memcpy(p + parm_off(f, "Start"), from, 12);
    memcpy(p + parm_off(f, "End"), to, 12);
    p[parm_off(f, "TraceChannel")] = 0;   // TraceTypeQuery1 = Visibility
    ignore[0] = pawn;
    TArray *ig = (TArray *)(p + oa);
    ig->data = ignore; ig->num = 1; ig->max = 1;
    p[parm_off(f, "bIgnoreSelf")] = 1;
    ue_process_event(UC_CDO(k), f, p);
    if (!p[orv]) return 1;
    UObject *hit = ue_weak_get(p + oh + 0x68);   // FHitResult.Actor (weak)
    return hit && (hit == door || (hc && ue_is_a(hit, hc)));
}

// The closed toolkit door the pinger aimed at: the ray from the pinger's eyes through the location ping's point crosses
// the door's bounds (+20 cm) before the point (the location trace can pass the door's edge), up to 6 m past it (the
// point can be on a teammate or anything else in front of the door), or further on (25 m from the eyes at most) with
// nothing but teammates in the way.
static UObject *toolkit_door_aimed(UObject *pc, const float ping[3]) {
    static UClass *dc;
    if (!dc) dc = ue_find_class("Door");
    UObject *pawn = pc ? ue_get_ptr(pc, "Pawn") : NULL;
    UFunction *ev = fn_of(pawn, "GetActorEyesViewPoint");
    static uint8_t p[64];
    float eye[3], dir[3];
    if (!ev || !dc) return NULL;
    memset(p, 0, sizeof p);
    ue_process_event(pawn, ev, p);
    memcpy(eye, p + parm_off(ev, "OutLocation"), 12);
    float len = dist3(eye, ping);
    if (len < 1.f || len > 2500.f) return NULL;
    for (int k = 0; k < 3; k++) dir[k] = (ping[k] - eye[k]) / len;
    UObject *best = NULL;
    float best_t = 0;
    for (int32_t i = 0, n = ue_num_objects(); i < n; i++) {
        UObject *x = ue_object_at(i);
        if (!x || !is_live(x) || !ue_is_a(x, dc) || !in_map(x)) continue;
        UFunction *f = fn_of(x, "GetActorBounds");
        if (!f) continue;
        memset(p, 0, sizeof p);
        ue_process_event(x, f, p);   // bOnlyCollidingComponents = 0
        const float *c = (const float *)(p + parm_off(f, "Origin")), *e = (const float *)(p + parm_off(f, "BoxExtent"));
        if (dist3(c, eye) > 2700.f) continue;
        float t0 = 0.f, t1 = 2500.f;   // slab test
        for (int k = 0; k < 3 && t0 <= t1; k++) {
            float lo = c[k] - e[k] - 20.f, hi = c[k] + e[k] + 20.f;
            if (fabsf(dir[k]) < 1e-6f) { if (eye[k] < lo || eye[k] > hi) t0 = t1 + 1.f; continue; }
            float a = (lo - eye[k]) / dir[k], b = (hi - eye[k]) / dir[k];
            if (a > b) { float tmp = a; a = b; b = tmp; }
            if (a > t0) t0 = a;
            if (b < t1) t1 = b;
        }
        if (t0 > t1 || (best && t0 >= best_t)) continue;
        UObject *u = door_item_usable(x, NULL);
        if (!u || !USABLE_ENABLED(u) || door_state(x) != 0) continue;
        if (t0 > len + 600.f) {
            float from[3], to[3];
            for (int k = 0; k < 3; k++) { from[k] = ping[k] + dir[k] * 5.f; to[k] = eye[k] + dir[k] * (t0 + 20.f); }
            if (!sight_to_door(pawn, from, to, x)) continue;
        }
        best = x; best_t = t0;
    }
    return best;
}

static void spawnping_detour(UObject *comp, UObject *pc, const void *xform, uint8_t type, UObject *target) {
    char nm[128];
    if (toolkit_doors && type == 2 /*PingLocation*/ && !target && xform) {
        UObject *door = toolkit_door_aimed(pc, (const float *)xform + 4), *pg = door ? ue_get_ptr(door, "PingableComponent") : NULL;
        if (pg) {
            LOG("bots: location ping on toolkit door %s: made it the door's ping", ue_obj_name(door, nm, sizeof nm));
            type = 1;
            target = pg;
        }
    }
    orig_spawnping(comp, pc, xform, type, target);
#ifndef B4B_RELEASE
    {
        char a[128], b[128], c[128];
        UFunction *g = target ? fn_of(target, "GetOwner") : NULL;
        uint8_t q[16] = {0};
        if (g) ue_process_event(target, g, q);
        const float *t = (const float *)xform;
        LOG("bots: ServerSpawnPing by %s type %d target %s on %s at (%.0f %.0f %.0f)", pc ? ue_obj_name(pc, a, sizeof a) : "-", type,
            target ? ue_obj_name(target, b, sizeof b) : "null", *(UObject **)q ? ue_obj_name(*(UObject **)q, c, sizeof c) : "-",
            t ? t[4] : 0.f, t ? t[5] : 0.f, t ? t[6] : 0.f);
    }
#endif
    if (!toolkit_doors || type != 1 /*EWaypointType::PingActor*/ || !target) return;
    static UClass *dc;
    if (!dc) dc = ue_find_class("Door");
    UFunction *go = fn_of(target, "GetOwner");
    uint8_t p[16] = {0};
    if (!go || !dc) return;
    ue_process_event(target, go, p);
    UObject *door = *(UObject **)p;
    if (!door || !ue_is_a(door, dc) || !door_item_usable(door, NULL)) return;
    char why[200];
    LOG("bots: toolkit door %s pinged: %s", ue_obj_name(door, nm, sizeof nm), send_bot_to_door(door, pc, why, sizeof why));
}

static void ini_key(const char *k, const char *v, void *ctx) {
    (void)ctx;
    if (!strcmp(k, "bot_toolkit_doors")) toolkit_doors = atoi(v) != 0;
}

// For the ~ Cheats tab: -1 = not available (hooks off), else the setting
int bots_toolkit_doors(void) { return ping_count ? toolkit_doors : -1; }

int bots_live(const char *key, const char *val) {
    if (strcmp(key, "bot_toolkit_doors")) return 0;
    toolkit_doors = !val || atoi(val) != 0;
    return 1;
}

int bots_init(void) {
    cmds_ini_each(ini_key, NULL);
    const uint8_t *call = (const uint8_t *)ADDR_TETHER_CLEAR_RET - 5;   // e8 <rel32> to ClearAllTargets
    if (memcmp((void *)ADDR_SPAWNPING_IMPL, SIG_SPAWNPING, sizeof SIG_SPAWNPING) ||
        memcmp((void *)ADDR_PING_COUNT, SIG_PING_COUNT, sizeof SIG_PING_COUNT) ||
        memcmp((void *)ADDR_TETHER_ACTOR, SIG_TETHER_ACTOR, sizeof SIG_TETHER_ACTOR) ||
        memcmp((void *)ADDR_TETHER_LOC, SIG_TETHER_LOC, sizeof SIG_TETHER_LOC) ||
        memcmp((void *)ADDR_CLEAR_TARGETS, SIG_CLEAR_TARGETS, sizeof SIG_CLEAR_TARGETS) || call[0] != 0xe8 ||
        (uintptr_t)ADDR_TETHER_CLEAR_RET + *(const int32_t *)(call + 1) != (uintptr_t)ADDR_CLEAR_TARGETS) {
        LOG("bots: signature mismatch, pinged toolkit doors stay retail");
        return -1;
    }
    if (MH_CreateHook((void *)ADDR_SPAWNPING_IMPL, (void *)spawnping_detour, (void **)&orig_spawnping) != MH_OK ||
        MH_CreateHook((void *)ADDR_TETHER_ACTOR, (void *)tether_actor_detour, (void **)&orig_tether_actor) != MH_OK ||
        MH_CreateHook((void *)ADDR_TETHER_LOC, (void *)tether_loc_detour, (void **)&orig_tether_loc) != MH_OK ||
        MH_CreateHook((void *)ADDR_CLEAR_TARGETS, (void *)clear_targets_detour, (void **)&orig_clear_targets) != MH_OK ||
        MH_EnableHook((void *)ADDR_SPAWNPING_IMPL) != MH_OK || MH_EnableHook((void *)ADDR_TETHER_ACTOR) != MH_OK ||
        MH_EnableHook((void *)ADDR_TETHER_LOC) != MH_OK || MH_EnableHook((void *)ADDR_CLEAR_TARGETS) != MH_OK) {
        MH_DisableHook((void *)ADDR_SPAWNPING_IMPL); MH_DisableHook((void *)ADDR_TETHER_ACTOR);
        MH_DisableHook((void *)ADDR_TETHER_LOC); MH_DisableHook((void *)ADDR_CLEAR_TARGETS);
        LOG("bots: hook failed, pinged toolkit doors stay retail");
        return -1;
    }
    ping_count = (PingCountFn)ADDR_PING_COUNT;
    LOG("bots: pinged toolkit doors %s", toolkit_doors ? "on" : "off (bot_toolkit_doors=0)");
    return 0;
}
