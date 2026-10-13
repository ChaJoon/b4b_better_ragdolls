// Add-ons in multiplayer (#22, private since #35). docs/investigations/addons.md §7, player page docs/COMMANDS.md
// "Add-ons".
//
// L4D-like: add-ons stay local, and which add-ons a player has never leaves their PC (#35: no ids, names or counts
// go to anyone). The host announces its addons_policy; the joiner checks its own add-ons against it and, if they
// don't pass, doesn't join, telling only its own player what to switch off.
//   policies (host, ini addons_policy=, /addons policy <x> for the session):
//     any       (default since #43, ADDONS_POLICY_DEFAULT; was cosmetic) everyone may join, whatever they run
//     cosmetic  no gameplay-affecting add-ons (addonclass.c decides, not the author)
//     none      no add-ons at all
//     match     cosmetic ones are free; gameplay ones must be exactly the host's. Choosing it is the host's opt-in to
//               reveal its OWN gameplay add-on ids (first 8 hex digits of the content id), which the joiner needs.
//   announce (host): the Steam rich presence connect string carries " addons:<policy>" ("addons:match:<id8>.<id8>..."
//     for match, up to MATCH_MAX ids) (presence.c, addons_presence_token); a login that doesn't say it checked gets
//     a login error ending in the same token in brackets, "[addons:<policy>]".
//   joiner: the policy is known from the presence it joined through (or read from the host's presence for a
//     steam:<id64> target), else from that refusal. Passes -> the login carries ?b4bcoopaddonsok=<policy> ("I checked
//     against your policy": the host learns nothing it wouldn't learn from a successful join). Fails -> no connection
//     (presence path) or no second attempt (refusal path), and a local chat line naming the joiner's own add-ons.
//     After a refusal that passes, the client joins again at once (cmds_join_retry).
//   compatibility (no protocol bump, addons.md §7): an older host (0.6.0-0.7.0) announces nothing: the joiner assumes
//     cosmetic (every older version's default), and an IP join to it isn't checked (the older host sees no summary and
//     lets it in). An older client (0.6.0-0.7.0) still sends its summary ?b4bcoopaddons=<C>c<G>g,...: the host checks
//     it as before, but never logs, keeps or shows it; the refusal (with the joiner's own add-on names) goes only to
//     the joiner. A modified client can ignore the policy; that was always true (the summary was self-reported).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>
#include "ue.h"
#include "log.h"
#include "cmds.h"
#include "overlay.h"

enum { POL_ANY, POL_COSMETIC, POL_NONE, POL_MATCH, POL_UNKNOWN };
#define ADDONS_POLICY_DEFAULT POL_ANY        // the one place to change the default (docs: COMMANDS.md, addons.md)
static const char *POLICY_NAMES[] = {"any", "cosmetic", "none", "match"};
static int policy = ADDONS_POLICY_DEFAULT;

#define MATCH_MAX 16     // host gameplay ids announced for match (the rich presence value is 256 chars max)
#define TITLE_MAX 24
#define MAX_ENTRIES 64

static int policy_of(const char *v, size_t n) {
    for (int i = 0; i < 4; i++) if (strlen(POLICY_NAMES[i]) == n && !_strnicmp(v, POLICY_NAMES[i], n)) return i;
    return -1;
}
int addons_policy_set(const char *v) {
    int p = policy_of(v, strlen(v));
    if (p < 0) return -1;
    policy = p;
    return 0;
}
const char *addons_policy_name(void) { return POLICY_NAMES[policy]; }

// A policy changed during a session is for new joiners: the players in the session at that moment keep playing.
// Without this they were refused at the next map change (a mission start reconnects every client) and dropped back
// to their own Fort Hope. Their keys (steam:<id64>) are kept until the host quits.
#define SEATED_MAX 32
static char seated[SEATED_MAX][80];
static int n_seated;
static void seat_current_players(void) {
    UObject **pa; int n = admin_player_array(&pa);
    UClass *pcc = ue_find_class("PlayerController");
    for (int i = 0; i < n && pcc; i++) {
        UObject *pc = ue_get_ptr(pa[i], "Owner");
        if (!pc || pc == ue_local_pc() || !ue_is_a(pc, pcc)) continue;
        char key[80];
        admin_ps_key(pa[i], key, sizeof key);
        int have = 0;
        for (int k = 0; k < n_seated; k++) have |= !strcmp(seated[k], key);
        if (have || n_seated == SEATED_MAX) continue;
        snprintf(seated[n_seated++], sizeof seated[0], "%s", key);
        LOG("addons: %s is in the session: keeps playing under the new policy", key);
    }
}
static int is_seated(const char *key) {
    for (int k = 0; key && k < n_seated; k++) if (!strcmp(seated[k], key)) return 1;
    return 0;
}

// b4bcoop.ini addons_policy= changed while the game runs (or set from the ~ window): NULL = back to the default
int addons_live(const char *key, const char *v) {
    if (strcmp(key, "addons_policy")) return 0;
    int was = policy;
    if (!v) policy = ADDONS_POLICY_DEFAULT;
    else if (addons_policy_set(v)) LOG("addons: bad addons_policy=%s (any|cosmetic|none|match), keeping %s", v, addons_policy_name());
    LOG("addons: policy %s (b4bcoop.ini)", POLICY_NAMES[policy]);
    if (policy != was && !admin_is_client()) seat_current_players();
    return 1;
}

static int own(AddonRef *r, int max) { return addons_active(r, max); }

// ---- announcing (host) ----
// "addons:cosmetic", or "addons:match:<id8>.<id8>..." with our own gameplay add-ons
static void policy_token(char *out, size_t n) {
    size_t k = (size_t)snprintf(out, n, "addons:%s", POLICY_NAMES[policy]);
    if (policy != POL_MATCH) return;
    AddonRef r[MAX_ADDONS];
    int m = own(r, MAX_ADDONS), shown = 0;
    for (int i = 0; i < m; i++) {
        if (!r[i].gameplay) continue;
        if (shown == MATCH_MAX) {
            static int told;
            if (!told++) LOG("addons: match announces only %d gameplay add-ons; joiners can't match the rest", MATCH_MAX);
            break;
        }
        k += (size_t)snprintf(out + k, k < n ? n - k : 0, "%c%.8s", shown ? '.' : ':', r[i].hash);
        shown++;
    }
}

// presence.c: appended to the rich presence connect string while we host
void addons_presence_token(char *out, size_t n) {
    if (n < 2) return;
    out[0] = ' ';
    policy_token(out + 1, n - 1);
}

// ---- the host's policy as the joiner knows it, per join target ("steam:<id64>" or "ip[:port]", as joined) ----
// src: 1 = the host's rich presence (current: re-read at every Join Game / steam: join), 2 = its login refusal (may be
// stale: the host can change its policy, so it only decides the ?b4bcoopaddonsok= claim, never a local refusal)
typedef struct { char target[112]; int pol, nid, assumed, tries, src; unsigned long long last; char ids[MATCH_MAX][9]; } Note;
static Note notes[8];
static int n_notes;

static Note *note_of(const char *target, int create) {
    for (int i = 0; i < n_notes; i++) if (!strcmp(notes[i].target, target)) return &notes[i];
    if (!create) return NULL;
    Note *n = n_notes < 8 ? &notes[n_notes++] : &notes[7];
    memset(n, 0, sizeof *n);
    snprintf(n->target, sizeof n->target, "%s", target);
    return n;
}

// "cosmetic" / "match:ab12cd34.ef567890" (the part after "addons:") into a note; NULL = an older host (assumed cosmetic)
static void parse_token(const char *tok, Note *n) {
    n->nid = 0; n->assumed = !tok;
    if (!tok) { n->pol = POL_COSMETIC; return; }
    size_t pl = strcspn(tok, ":] ");
    int p = policy_of(tok, pl);
    n->pol = p < 0 ? POL_UNKNOWN : p;
    if (n->pol != POL_MATCH || tok[pl] != ':') return;
    for (const char *q = tok + pl + 1; n->nid < MATCH_MAX;) {
        int k = 0;
        while (k < 8 && isxdigit((unsigned char)q[k])) { n->ids[n->nid][k] = (char)tolower((unsigned char)q[k]); k++; }
        n->ids[n->nid][k] = 0;
        if (k == 8) n->nid++;
        q += k;
        if (*q != '.') break;
        q++;
    }
}

// presence.c: the host's connect string said "addons:<tok>" (tok NULL: no addons: token, an older b4bcoop). targets =
// the comma-separated join targets from that connect string.
void addons_note_host(const char *targets, const char *tok) {
    char list[300];
    snprintf(list, sizeof list, "%s", targets ? targets : "");
    for (char *t = strtok(list, ","); t; t = strtok(NULL, ",")) {
        while (*t == ' ') t++;
        if (!*t) continue;
        Note *n = note_of(t, 1);
        parse_token(tok, n);
        n->src = 1;
        LOG("addons: host %s announces add-on policy %s%s", t, tok ? tok : "cosmetic", tok ? "" : " (assumed: older b4bcoop)");
    }
}

// "A, B, C and 2 more": our add-ons that fail (gameplay only, or all), skipping ids the host has (match)
static int list_own(const AddonRef *r, int n, int gameplay_only, const Note *skip, char *out, size_t on) {
    size_t k = 0; int all = 0;
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        if (gameplay_only && !r[i].gameplay) continue;
        int has = 0;
        for (int j = 0; skip && j < skip->nid; j++) if (!strncmp(skip->ids[j], r[i].hash, 8)) has = 1;
        if (has) continue;
        if (all++ < 3) k += (size_t)snprintf(out + k, k < on ? on - k : 0, "%s%.40s", k ? ", " : "", r[i].title);
    }
    if (all > 3) snprintf(out + k, k < on ? on - k : 0, " and %d more", all - 3);
    return all;
}

// Do our add-ons pass the host's policy? 1 = they don't (msg: the line for our own player), 0 = they do
static int fails(const Note *n, char *msg, size_t mn) {
    AddonRef r[MAX_ADDONS];
    int m = own(r, MAX_ADDONS);
    char list[240];
    msg[0] = 0;
    switch (n->pol) {
    case POL_ANY: return 0;
    case POL_COSMETIC:
        if (!list_own(r, m, 1, NULL, list, sizeof list)) return 0;
        snprintf(msg, mn, "This host allows only cosmetic add-ons%s; turn off %s (/addons off <#>, or Remove in the ~ window's Add-ons tab) "
                 "and restart the game.", n->assumed ? " (an older b4bcoop: its default)" : "", list);
        return 1;
    case POL_NONE:
        if (!list_own(r, m, 0, NULL, list, sizeof list)) return 0;
        snprintf(msg, mn, "This host allows no add-ons; turn off %s (/addons off <#>, or Remove in the ~ window's Add-ons tab) and "
                 "restart the game.", list);
        return 1;
    case POL_MATCH: {
        int extra = list_own(r, m, 1, n, list, sizeof list), lack = 0;
        char need[240]; size_t k = 0;
        need[0] = 0;
        for (int j = 0; j < n->nid; j++) {   // the host's gameplay add-ons we don't run
            int has = 0;
            for (int i = 0; i < m; i++) if (r[i].gameplay && !strncmp(n->ids[j], r[i].hash, 8)) has = 1;
            if (has) continue;
            const char *t = addons_title_of(n->ids[j]);   // installed here but off?
            if (lack++ < 3) k += (size_t)snprintf(need + k, k < sizeof need ? sizeof need - k : 0, "%s%.40s%s", k ? ", " : "",
                                                   t ? t : n->ids[j], t ? "" : " (id; not installed here)");
        }
        if (lack > 3) snprintf(need + k, k < sizeof need ? sizeof need - k : 0, " and %d more", lack - 3);
        if (!extra && !lack) return 0;
        k = (size_t)snprintf(msg, mn, "This host requires the same gameplay add-ons as theirs.");
        if (extra) k += (size_t)snprintf(msg + k, k < mn ? mn - k : 0, " Turn off: %s.", list);
        if (lack) k += (size_t)snprintf(msg + k, k < mn ? mn - k : 0, " Turn on or install: %s.", need);
        snprintf(msg + k, k < mn ? mn - k : 0, " Then restart the game.");
        return 1;
    }
    default:
        snprintf(msg, mn, "This host uses an add-on rule this b4bcoop doesn't know; update b4bcoop (~ window, tab Updates).");
        return 1;
    }
}

// cmds.c (every join attempt) and presence.c (before arming a Steam join): 1 = our add-ons don't pass the policy
// the host announced in its rich presence; msg = what to tell our player. Unknown policy = 0 (the host asks at login).
int addons_join_refused(const char *target, char *msg, size_t mn) {
    char first[112];
    snprintf(first, sizeof first, "%.*s", (int)strcspn(target, ","), target);
    Note *n = note_of(first, 0);
    msg[0] = 0;
    if (!n || n->src != 1 || !fails(n, msg, mn)) return 0;
    LOG("addons: not joining %s: our add-ons don't pass its policy %s (told only to us)", first,
        n->pol < POL_UNKNOWN ? POLICY_NAMES[n->pol] : "unknown");
    return 1;
}

// addons.c: an add-on would be loaded now (~ window: Add-ons tab; /addons on) as a client in someone's session. The
// host's policy as we know it from this join (its rich presence or its login refusal; nothing known = it didn't ask,
// i.e. any, or an older host reached by IP, which doesn't check) decides, the same way it would decide our next join.
// 1 = allowed; 0 = not (msg: why, for our own player).
int addons_session_allows(int gameplay, char *msg, size_t mn) {
    msg[0] = 0;
    if (!admin_is_client()) return 1;
    const char *t = cmds_last_join_target();
    char first[112];
    snprintf(first, sizeof first, "%.*s", (int)strcspn(t ? t : "", ","), t ? t : "");
    Note *n = first[0] ? note_of(first, 0) : NULL;
    int pol = n ? n->pol : POL_ANY;
    if (pol == POL_ANY || (!gameplay && (pol == POL_COSMETIC || pol == POL_MATCH))) return 1;
    if (pol == POL_NONE) snprintf(msg, mn, "this host allows no add-ons");
    else if (pol == POL_UNKNOWN) snprintf(msg, mn, "this host uses an add-on rule this b4bcoop doesn't know");
    else snprintf(msg, mn, "this host allows %s", pol == POL_COSMETIC ? "only cosmetic add-ons" : "only its own gameplay add-ons");
    LOG("addons: not loading an add-on now: %s (policy %s, as announced for %s)", msg, pol < POL_UNKNOWN ? POLICY_NAMES[pol] : "unknown", first);
    return 0;
}

// cmds.c: appended to the join URL: "?b4bcoopaddonsok=<policy>" once we know the host's policy and pass it
const char *addons_login_option(const char *target) {
    static char opt[48];
    Note *n = note_of(target, 0);
    char msg[8];
    opt[0] = 0;
    if (n && !n->assumed && n->pol != POL_ANY && n->pol != POL_UNKNOWN && !fails(n, msg, sizeof msg))
        snprintf(opt, sizeof opt, "?b4bcoopaddonsok=%s", POLICY_NAMES[n->pol]);
    return opt;
}

// chat.c: a join to `target` failed with `error`. Ours = the host asked us to check ("[addons:<policy>]").
// Returns 0 (not ours), 1 (we pass: join again, cmds_join_retry), 2 (we don't pass or it keeps asking: msg).
int addons_on_refusal(const char *error, const char *target, char *msg, size_t mn) {
    const char *t = strstr(error, "[addons:");
    if (!t || !target || !*target) return 0;
    Note *n = note_of(target, 1);
    parse_token(t + 8, n);
    n->src = 2;
    LOG("addons: host %s asks us to check our add-ons against its policy %.*s", target, (int)strcspn(t + 8, "]"), t + 8);
    if (fails(n, msg, mn)) return 2;
    unsigned long long now = GetTickCount64();   // a host that asks again right after we checked: stop, don't loop
    n->tries = now - n->last < 60000 ? n->tries + 1 : 1;
    n->last = now;
    if (n->tries > 2) {
        snprintf(msg, mn, "The host keeps asking for an add-on check (policy %.*s); join again later.", (int)strcspn(t + 8, "]:"), t + 8);
        return 2;
    }
    return 1;
}

// ---- the gate (host, admin.c PreLogin) ----
// Older clients' summary: "<C>c<G>g[,g<id8>-<Title>...][,c<id8>...][,+<n>]"
typedef struct { char g; char id[9]; char title[TITLE_MAX + 1]; } Entry;
typedef struct { int nc, ng, n; Entry e[MAX_ENTRIES]; } Summary;

static void parse_summary(const char *v, Summary *s) {
    memset(s, 0, sizeof *s);
    if (sscanf(v, "%dc%dg", &s->nc, &s->ng) != 2) { s->nc = s->ng = 0; }
    if (s->nc < 0 || s->nc > 9999) s->nc = 0;
    if (s->ng < 0 || s->ng > 9999) s->ng = 0;
    for (const char *p = strchr(v, ','); p; p = strchr(p + 1, ',')) {
        const char *q = p + 1;
        if ((*q != 'g' && *q != 'c') || s->n == MAX_ENTRIES) continue;
        Entry *e = &s->e[s->n];
        e->g = *q == 'g';
        int k = 0;
        for (q++; k < 8 && isxdigit((unsigned char)*q); q++) e->id[k++] = (char)tolower((unsigned char)*q);
        e->id[k] = 0;
        if (k != 8) continue;
        if (*q == '-') {
            k = 0;
            for (q++; *q && *q != ',' && k < TITLE_MAX; q++) e->title[k++] = isalnum((unsigned char)*q) ? *q : ' ';
            e->title[k] = 0;
        }
        s->n++;
    }
}

// An older client's summary against our policy: 1 = refused, err = its login error (its own add-ons, for it only)
static int summary_fails(const char *v, char *err, size_t en) {
    Summary s;
    parse_summary(v, &s);
    if (policy == POL_COSMETIC && s.ng > 0)
        snprintf(err, en, "Host allows cosmetic add-ons only; you have %d gameplay add-on(s). Switch them off (/addons off <#>) "
                 "and restart the game.", s.ng);
    else if (policy == POL_NONE && s.nc + s.ng > 0)
        snprintf(err, en, "Host allows no add-ons; you have %d. Switch them off (/addons off <#>) and restart the game.", s.nc + s.ng);
    else if (policy == POL_MATCH) {
        AddonRef r[MAX_ADDONS];
        int n = own(r, MAX_ADDONS), bad = 0, listed = 0;
        for (int i = 0; i < n; i++) {   // our gameplay add-ons it lacks
            if (!r[i].gameplay) continue;
            int have = 0;
            for (int j = 0; j < s.n; j++) if (s.e[j].g && !strncmp(s.e[j].id, r[i].hash, 8)) have = 1;
            bad += !have;
        }
        for (int j = 0; j < s.n; j++) {   // its gameplay add-ons we don't have
            if (!s.e[j].g) continue;
            listed++;
            int have = 0;
            for (int i = 0; i < n; i++) if (r[i].gameplay && !strncmp(s.e[j].id, r[i].hash, 8)) have = 1;
            bad += !have;
        }
        if (s.ng > listed || bad)
            snprintf(err, en, "Host requires the same gameplay add-ons as theirs; yours differ. Update b4bcoop to see which, "
                     "then restart the game.");
    }
    return err[0] != 0;
}

// Returns 1 = refuse (err = the joiner's login error). claim = ?b4bcoopaddonsok= (a b4bcoop that checked itself),
// summary = ?b4bcoopaddons= (an older b4bcoop). Nothing about the joiner's add-ons is logged or kept.
int addons_login_check(const char *claim, const char *summary, const char *name, const char *key, char *err, size_t en) {
    err[0] = 0;
    if (policy == POL_ANY) return 0;
    if (is_seated(key)) { LOG("addons: login %s: was in the session when the policy changed, welcome back", name); return 0; }
    if (claim && *claim) {
        if (!_stricmp(claim, POLICY_NAMES[policy])) { LOG("addons: login %s: checked its add-ons against addons_policy=%s", name, POLICY_NAMES[policy]); return 0; }
    } else if (summary && *summary) {   // an older b4bcoop sends its list; judged as before, never logged
        if (!summary_fails(summary, err, en)) return 0;
        LOG("addons: login %s: refused by addons_policy=%s (an older b4bcoop; its add-on list isn't logged)", name, POLICY_NAMES[policy]);
        return 1;
    }
    char tok[200];
    policy_token(tok, sizeof tok);
    snprintf(err, en, "Host allows %s. Your b4bcoop checks your add-ons and joins again. [%s]",
             policy == POL_COSMETIC ? "cosmetic add-ons only" : policy == POL_NONE ? "no add-ons" : "only its own gameplay add-ons", tok);
    LOG("addons: login %s: asked to check its add-ons against addons_policy=%s%s", name, POLICY_NAMES[policy],
        claim && *claim ? " (its check was for another policy)" : "");
    return 1;
}

// /addons policy [x]; /addons players is gone (#35). Returns 1 if handled.
int addons_mp_slash(const char *sub, char *arg, Out *o) {
#ifndef B4B_RELEASE
    if (!strcmp(sub, "unseat")) {   // dev: forget who was seated, so the next reconnect (mission start) meets the policy
        out_printf(o, "addons: %d seated player(s) forgotten\n", n_seated);
        n_seated = 0;
        return 1;
    }
#endif
    if (!strcmp(sub, "players") || !strcmp(sub, "who")) {
        out_printf(o, "/addons players: gone. Which add-ons someone has stays on their PC (joiners check themselves against "
                      "the host's policy). Yours: /addons\n");
        return 1;
    }
    if (!strcmp(sub, "policy")) {
        while (arg && *arg == ' ') arg++;
        if (!arg || !*arg) {
            out_printf(o, "add-ons policy: %s (any | cosmetic | none | match; ini addons_policy=, default %s)\n",
                       POLICY_NAMES[policy], POLICY_NAMES[ADDONS_POLICY_DEFAULT]);
            return 1;
        }
        if (admin_is_client()) { out_printf(o, "/addons policy: host only (you are a client)\n"); return 1; }
        int was = policy;
        if (addons_policy_set(arg)) { out_printf(o, "usage: /addons policy any|cosmetic|none|match\n"); return 1; }
        if (policy != was) seat_current_players();
        LOG("addons: policy %s (chat)", POLICY_NAMES[policy]);
        out_printf(o, "add-ons policy: %s for this session (new joiners; players here now keep playing; ini addons_policy= keeps it)%s\n", POLICY_NAMES[policy],
                   policy == POL_MATCH ? ". match shows joiners the ids of your own gameplay add-ons" : "");
        return 1;
    }
    return 0;
}

// ~ window, Host rules at the bottom of the Add-ons tab (looks.c): the host's policy (saved to b4bcoop.ini, like
// editing addons_policy=), one combo. A client sees it greyed out, with the reason.
void addons_mp_panel(void) {
    static const char *ITEMS[] = {"anyone", "cosmetic add-ons only", "no add-ons", "same gameplay add-ons"};
    ov_begin_perm(CMD_HOST);
    int cur = policy;
    ov_width(11);
    if (ov_combo("Who may join with add-ons##pol", &cur, ITEMS, 4) && cur != policy)
        ov_setting("addons_policy", cur == ADDONS_POLICY_DEFAULT ? NULL : POLICY_NAMES[cur], 1);
    ov_tooltip("addons_policy in b4bcoop.ini (any, cosmetic, none, match); applies to the next player who joins. Joiners "
               "check their own add-ons against it: nobody's add-ons are sent to anyone. Same gameplay add-ons (match): "
               "cosmetic ones are free, gameplay add-ons must be the same as yours (joiners see their ids).");
    ov_end_perm();
}
