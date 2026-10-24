// Client: one voice line per cutscene cue (doubled cutscene subtitles, model-swap.md #37 "doubled subtitle").
//
// Cutscene radio lines (Rogers over the comms in the CDC_D escape, ...) are played by the level on every machine with
// Comms_DialogueSpeaker::PlayClientCinematicVO(ResponseGroup): it picks a line of the group and has the speaker's
// GobiDialogueComponent play it (vtable +0x410, which also shows the subtitle). On the machine with authority that
// play is also written into the component's replicated DialogueInfo, so on a listen server (our host) the host's pick
// reaches every client through OnRep_DialogueInfo (DialogueComponent 0x140EDBA20, which plays the newest entry),
// while the client's own level already played its own pick: two lines (two different ones, or the same one twice)
// and two subtitles on clients. Retail never had a listen-server host with clients in a mission (online co-op ran on
// dedicated servers), so this is ours to fix: for a Comms_DialogueSpeaker, a client keeps the first of the two and
// drops the other one when it arrives within VO_WINDOW seconds (whichever comes first: the host's sequence can run a
// moment ahead or behind). Other speakers (heroes, NPCs) are untouched. Host side and protocol unchanged.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "MinHook.h"
#include "ue.h"
#include "log.h"
#include "cmds.h"

#define ADDR_ONREP_DLG  VA(0x140EDBA20ull)   // void UDialogueComponent::OnRep_DialogueInfo(this) (vtable +0x460)
#define ADDR_CLIENT_VO  VA(0x142017040ull)   // exec thunk Comms_DialogueSpeaker::PlayClientCinematicVO(ctx, FFrame&, result)
#define ADDR_PLAY_LINE  VA(0x141620240ull)   // UGobiDialogueComponent play line (vtable +0x410)(this, const line params*)
static const uint8_t SIG_ONREP_DLG[] = {0x48,0x63,0x91,0xf8,0x00,0x00,0x00,0x45,0x33,0xc9,0x48,0x8b,0x81,0xf0,0x00,0x00,
                                        0x00,0x4c,0x8b,0xd1,0x4c,0x8d,0x04,0x92};
static const uint8_t SIG_CLIENT_VO[] = {0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x48,0x8b,0xec,0x48,0x83,0xec,0x70,0x48,
                                        0x8b,0x42,0x20,0x33,0xf6,0x48,0x89,0x75};
static const uint8_t SIG_PLAY_LINE[] = {0x48,0x89,0x5c,0x24,0x08,0x56,0x48,0x83,0xec,0x20,0x48,0x8b,0xf2,0x48,0x8b,0xd9};
#define VO_WINDOW 4.0

typedef void (*OnRepFn)(UObject *comp);
typedef void (*ExecFn)(UObject *ctx, void *frame, void *result);
typedef void (*PlayFn)(UObject *comp, const void *params);
static OnRepFn orig_onrep;
static ExecFn orig_client_vo;
static PlayFn orig_play;
static int skip_play;   // set around a PlayClientCinematicVO call whose line we drop
static struct { UObject *actor; double local_at, rep_at; } spk[8];
static unsigned n_dropped;

static double now_s(void) { return (double)GetTickCount64() / 1000.0; }
static int slot_of(UObject *actor) {
    int free = -1, oldest = 0;
    for (int i = 0; i < 8; i++) {
        if (spk[i].actor == actor) return i;
        if (!spk[i].actor && free < 0) free = i;
        if (spk[i].local_at + spk[i].rep_at < spk[oldest].local_at + spk[oldest].rep_at) oldest = i;
    }
    int k = free >= 0 ? free : oldest;
    memset(&spk[k], 0, sizeof spk[k]);
    spk[k].actor = actor;
    spk[k].local_at = spk[k].rep_at = -100;
    return k;
}
static int is_comms_speaker(UObject *actor) {
    static UClass *c;
    if (!c) c = ue_find_class("Comms_DialogueSpeaker");
    return c && actor && ue_is_a(actor, c);
}

// the host's pick, replicated
static void onrep_detour(UObject *comp) {
    UObject *actor = comp ? U_OUTER(comp) : NULL;
    if (admin_is_client() && is_comms_speaker(actor)) {
        int k = slot_of(actor);
        double t = now_s();
        if (t - spk[k].local_at < VO_WINDOW) {
            n_dropped++;
            LOG("dialogue: cutscene line from the host dropped: this client's level already played one %.1fs ago", t - spk[k].local_at);
            return;
        }
        spk[k].rep_at = t;
    }
    orig_onrep(comp);
}

// this machine's own pick (the level's cue). The thunk still runs (it reads its parameter from the bytecode frame);
// only the play it leads to is skipped (skip_play, play_detour).
static void play_detour(UObject *comp, const void *params) {
    if (skip_play) return;
    if (barks_filter(comp, params)) return;   // fork: survivor barks muted on this machine (barks.c)
    orig_play(comp, params);
}
static void client_vo_detour(UObject *ctx, void *frame, void *result) {
    int skip = 0;
    if (admin_is_client() && is_comms_speaker(ctx)) {
        int k = slot_of(ctx);
        double t = now_s();
        if (t - spk[k].rep_at < VO_WINDOW) {
            skip = 1;
            n_dropped++;
            LOG("dialogue: cutscene line from our level skipped: the host's arrived %.1fs ago", t - spk[k].rep_at);
        } else spk[k].local_at = t;
    }
    skip_play = skip;
    orig_client_vo(ctx, frame, result);
    skip_play = 0;
}

int dialogue_init(void) {
    if (memcmp((void *)ADDR_ONREP_DLG, SIG_ONREP_DLG, sizeof SIG_ONREP_DLG) ||
        memcmp((void *)ADDR_CLIENT_VO, SIG_CLIENT_VO, sizeof SIG_CLIENT_VO) ||
        memcmp((void *)ADDR_PLAY_LINE, SIG_PLAY_LINE, sizeof SIG_PLAY_LINE)) { LOG("dialogue: signature mismatch, not hooked"); return -1; }
    if (MH_CreateHook((void *)ADDR_ONREP_DLG, (void *)onrep_detour, (void **)&orig_onrep) != MH_OK ||
        MH_CreateHook((void *)ADDR_CLIENT_VO, (void *)client_vo_detour, (void **)&orig_client_vo) != MH_OK ||
        MH_CreateHook((void *)ADDR_PLAY_LINE, (void *)play_detour, (void **)&orig_play) != MH_OK ||
        MH_EnableHook((void *)ADDR_ONREP_DLG) != MH_OK || MH_EnableHook((void *)ADDR_CLIENT_VO) != MH_OK ||
        MH_EnableHook((void *)ADDR_PLAY_LINE) != MH_OK) {
        LOG("dialogue: hook failed"); return -1;
    }
    LOG("dialogue: cutscene VO de-duplication on (clients)");
    return 0;
}

unsigned dialogue_dropped(void) { return n_dropped; }
