// Host: make remote players keep what they earn. See docs/investigations/client-rewards.md.
//
// The listen host runs the mission's reward code for every player: RewardSurvivorsForSuccess/Failure (supply points,
// skull totem points), RewardDuffelBags (unlocks, consumables) and the burn-card charge on leaving the start saferoom.
// Each reward is a PlayerProfileCommand passed to UGobiPlayerProfileComponent::ExecuteCommand on that player's
// component. ExecuteCommand only applies a command when the owning PlayerController is local (offline mode:
// ApplyCommandToOfflineData into that LocalPlayer's PlayerProfileSettings). For a remote player the non-local branch
// is an empty function, so the reward is dropped: nothing reaches the client, and nothing lands in the host's profile.
//
// The game still ships client RPCs for exactly these commands (ClientExecute*Command on the profile component). Their
// client side logs "[CLIENT RPC] ..." and runs ExecuteCommand on the client, where the controller is local, so the
// command goes through the native offline path into the client's own PlayerProfileSettings.json. Nothing in this
// build calls them any more. We call them: after the original ExecuteCommand, if the component belongs to a remote
// player and the command has a client RPC, send it to that player's client.
//
// Not forwarded on purpose: AdjustStatValue (the host already sends stat deltas via PlayerStatsComponent::
// ClientApplyStatDeltas, and the client reconciles them into its own profile) and UnlockStartingLocation (the client
// runs the unlock itself from GobiPlayerState::OnRep_UnlockedNewMap). CompleteAchievement has no client RPC; it is
// only logged for now.
#include <stdlib.h>
#include <string.h>
#include "MinHook.h"
#include "ue.h"
#include "log.h"
#include "cmds.h"

// void UGobiPlayerProfileComponent::ExecuteCommand(const FPlayerProfileCommand&, bool bPersist)
#define ADDR_PPC_EXECUTE VA(0x141BC4970ull)
static const uint8_t SIG_EXECUTE[] = {0x48,0x89,0x5c,0x24,0x18,0x56,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,
                                      0x99,0xd8,0x00,0x00,0x00,0x41,0x0f,0xb6,0xf0,0x4c,0x8b,0xfa,0x4c,0x8b,0xf1,0x48};
#define COMP_OWNER(c)      (*(UObject **)((char *)(c) + 0xD8))   // UActorComponent::OwnerPrivate
#define PPC_HYDRA_ID(c)    ((FString *)((char *)(c) + 0x1A8))    // GobiPlayerProfileComponent.HydraPublicId (unreflected)
#define CMD_TYPE(cmd)      (((uint8_t (*)(void *))(*(void ***)(cmd))[1])(cmd))  // FPlayerProfileCommand::GetType

typedef void (*ExecuteFn)(UObject *ppc, void *cmd, uint8_t persist);
static ExecuteFn orig_execute;

// EPlayerProfileCommandType -> client RPC on GobiPlayerProfileComponent
static const struct { uint8_t type; const char *name, *rpc; } FORWARD[] = {
    {5,  "AdjustSupplyPoints",       "ClientExecuteAdjustSupplyPointsCommand"},
    {6,  "UnlockProduct",            "ClientExecuteUnlockProductCommand"},
    {19, "AdjustConsumableQuantity", "ClientExecuteAdjustConsumableQuantityCommand"},
    {20, "AdjustSkullTotemPoints",   "ClientExecuteAdjustSkullTotemPointsCommand"},
};
#define NFWD (sizeof FORWARD / sizeof FORWARD[0])
static UFunction *rpc_fn[NFWD];
static UClass *cls_pc, *cls_lp;
static int forwarding, disabled, nlogged;

static int is_remote_pc(UObject *pc) {
    if (!cls_pc) cls_pc = ue_find_class("PlayerController");
    if (!cls_lp) cls_lp = ue_find_class("LocalPlayer");
    if (!pc || !cls_pc || !cls_lp || !ue_is_a(pc, cls_pc)) return 0;
    UObject *player = ue_get_ptr(pc, "Player");
    return player && !ue_is_a(player, cls_lp);  // UNetConnection => the controller of a remote client
}

static const char *hydra_id(UObject *ppc, char *buf, size_t len) {
    FString *s = PPC_HYDRA_ID(ppc);
    size_t k = 0;
    for (int i = 0; s->data && i < s->num && s->data[i] && k + 1 < len; i++) buf[k++] = s->data[i] < 128 ? (char)s->data[i] : '?';
    buf[k] = 0;
    return k ? buf : "?";
}

static void forward(UObject *ppc, void *cmd, int i) {
    if (!rpc_fn[i]) rpc_fn[i] = ue_find_function(U_CLASS(ppc), FORWARD[i].rpc);
    UFunction *fn = rpc_fn[i];
    FField *p = fn ? ue_find_prop(fn, "Command") : NULL;
    uint8_t parms[0x100];
    if (!p || UFN_PARMSSIZE(fn) > sizeof parms || FP_OFFSET(p) + FP_ELSIZE(p) > UFN_PARMSSIZE(fn)) {
        LOG("rewards: cannot forward %s (rpc %s missing or unexpected layout)", FORWARD[i].name, FORWARD[i].rpc);
        return;
    }
    // Shallow copy is fine: a remote RPC only serializes the parms; nothing destructs them (the caller keeps ownership).
    memset(parms, 0, sizeof parms);
    memcpy(parms + FP_OFFSET(p), cmd, FP_ELSIZE(p));
    forwarding = 1;
    ue_process_event(ppc, fn, parms);
    forwarding = 0;
}

static void execute_detour(UObject *ppc, void *cmd, uint8_t persist) {
    orig_execute(ppc, cmd, persist);
    if (disabled || forwarding || !ppc || !cmd || !is_remote_pc(COMP_OWNER(ppc))) return;
    uint8_t type = CMD_TYPE(cmd);
    char id[128];
    for (int i = 0; i < (int)NFWD; i++) {
        if (FORWARD[i].type != type) continue;
        int delta = (type == 5 || type == 20) ? *(int32_t *)((char *)cmd + 8) : (type == 19 ? *(int32_t *)((char *)cmd + 0x28) : 0);
        LOG("rewards: forwarding %s (%d) to remote player %s", FORWARD[i].name, delta, hydra_id(ppc, id, sizeof id));
        forward(ppc, cmd, i);
        return;
    }
    // stats / starting locations reach the client natively; anything else is worth seeing in a live test
    if (type != 7 && type != 8 && nlogged++ < 50)
        LOG("rewards: not forwarded: command type %d for remote player %s", type, hydra_id(ppc, id, sizeof id));
}

// cheats.c (/supply, /skulls, /burncards, /unlockall): run a profile command on this machine's own, local profile
// component (the original ExecuteCommand, persisted; never forwarded; on a client it never reaches rewardguard.c's RPC
// hooks either: nothing is sent to or from the host). -1 if the hook isn't in place or the component isn't local.
int rewards_execute_local(UObject *ppc, void *cmd) {
    if (!orig_execute || !ppc || !cmd || is_remote_pc(COMP_OWNER(ppc))) return -1;
    orig_execute(ppc, cmd, 1);
    return 0;
}

int rewards_init(void) {
    const char *e = getenv("B4BCOOP_NO_REWARDS");
    if (e && *e && *e != '0') { disabled = 1; LOG("rewards: disabled by B4BCOOP_NO_REWARDS"); return 0; }
    if (memcmp((void *)ADDR_PPC_EXECUTE, SIG_EXECUTE, sizeof SIG_EXECUTE)) { LOG("rewards: signature mismatch"); return -1; }
    if (MH_CreateHook((void *)ADDR_PPC_EXECUTE, (void *)execute_detour, (void **)&orig_execute) != MH_OK ||
        MH_EnableHook((void *)ADDR_PPC_EXECUTE) != MH_OK) { LOG("rewards: hook failed"); return -1; }
    LOG("rewards: ExecuteCommand hooked");
    return 0;
}
