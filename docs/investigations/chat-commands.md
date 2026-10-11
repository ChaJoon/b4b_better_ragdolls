# In-game chat commands

Build 14216215. Static analysis of `Back4Blood.exe` plus live runs with `launch/multi.sh 2` (2026-09-24).
Implementation: `native/src/chat.c` (hook, replies, notices), `native/src/admin.c` (commands, login gate, bans).

Players type `/command` in the normal chat box (Enter, or the team-chat key). The text is never sent; the reply is a
local chat line from `coop`.

## Commands

| Command | Who | Verified live | Notes |
|---|---|---|---|
| `/help` | all | yes (host camp, `host-help-fort-hope.jpg`) | host sees the admin list too |
| `/join steam:<id64>` | all | yes (client, camp) | `coop_join()`; `/join <ip[:port]>` only to 127.0.0.1 unless `host_ip=1` (0.3.0), else it replies why and points to Steam Join Game |
| `/host` | all | yes | only standalone in Fort Hope; turns on auto-host for the session (camp stays hosted after missions) |
| `/leave` | client | yes (from a mission) | disconnect, own offline camp, ini auto-join off for the session |
| `/players` | all | yes (`client-reply-mission.jpg`) | host also sees ping and Steam id; bots marked |
| `/flashlight [on\|off\|auto]` | all | yes (client in mission: host saw the client's light on, manual) | no argument = toggle |
| `/thirdperson [on\|off]` | all | yes (host and client, cheats off, 2026-09-25) | own camera only; `thirdperson.c`, docs/investigations/third-person.md |
| `/ping` | client | yes (56 ms locally) | own `PlayerState.Ping` × 4 |
| `/kick <name\|#>` | host | yes | player leaves at once; can come back with `/join` |
| `/ban <name\|#>` | host | yes, incl. host restart | `b4bcoop-bans.txt` next to the agent config; enforced at PreLogin |
| `/unban <name\|steam:id\|#n\|all>`, `/bans` | host | yes | |
| `/lock`, `/unlock` | host | yes | players present at `/lock` may still rejoin (map change), anyone else is refused |
| `/teamsize N` | host | yes (setting) | `teamsize.c`, from the next map |
| `/restart` | host | yes (Easy: chapter restarted in place, host + client) | see below |
| `/ready [vote]` | host | yes | `testing.c` `ready` |
| `/bots on\|off\|default` | host | yes (off: next chapter had 2 heroes, `bots-off-two-heroes.jpg`) | see below |
| `/say <msg>` | host | yes (`client-host-notice.jpg`) | shown on every client as `<host name>: [host] msg` |
| `//text` | all | - | sends `/text` as a normal message |

Who may run what: every command has a permission in the dispatcher (`cmds.h`): `CMD_ANYONE` (all players; acts on
their own game only), `CMD_HOST` (the "host" rows; a client gets `/<cmd>: host only (you are a client)`), `CMD_CHEAT`
(host and `/cheats on`, cheats.c's verbs via `cheats_perm()`; else `cheats are off`), `CMD_OWNSAVE` (own-save cheats,
#49: the host with cheats on, or a client whose host said `cheats on` in a data line; `cheats_own_save_ok()`).
`admin_slash` checks it once
before any handler, so a new personal command only needs a `CMD_ANYONE` row.

Dropped: **`/difficulty`**. A new run's difficulty is already picked in the war table (host's own UI, the path
`mission` uses: `Matchmaking::JoinRun(..., Difficulty, ...)`), and a run in progress keeps the difficulty stored with
the campaign run; switching it between chapters would mean rewriting run state the game never changes mid-run
(`CampaignRunData`, rewards and difficulty cards keyed on it). Nothing clean to add.

All commands also exist on the agent CLI: `kick ban unban bans lock unlock bots say restart` (plus the existing
`teamsize ready flashlight join host players`), `leave`, `who` (the `/players` listing), `admin` (gate state),
`slash /<cmd>` (run a chat command without the chat box).

## Security model

- **Only the local player's outgoing text is intercepted.** The chat box's send
  (`ChatBoxUserWidget::OnSendMessage`, native `0x14200B840`) calls `AGobiPlayerControllerBase::Say` (`0x141BA6CA0`)
  or `::SayTeam` (`0x141BA6EF0`) on its owning player; the `say`/`sayteam` console commands call the same two.
  They run on the typing machine before anything is sent. We hook both: text starting with `/` (not `//`) is queued
  and the original is not called, and only if the controller is `ue_local_pc()`.
- **Nothing a remote player sends is executed.** Other players' text never goes through Say on the host. The receive
  side (`ClientTeamMessage`) only ever displays text.
- **Admin commands are host-only by construction:** they act on the machine that typed them and need it to be the
  server. On a client they reply "host only" and nothing leaves the client (verified: client `/kick 0` produced no
  host log line and the client stayed connected).
- Bans/lock are enforced on the host at login (`AGobiGameMode::PreLogin` override `0x1419FE9C0`, the function that
  calls `GameSession->ApproveLogin`, i.e. slotguard's gate). Key = Steam id from the joining connection's
  `FUniqueNetIdRepl` (`steam:<id64>`), falling back to `name:<Name>` from the `?Name=` option. The error string
  goes back to the client as `NMT_Failure` ("You are banned from this session." / "The host locked the session.").
- A kicked or banned client is told with a notice of type `b4bcoopkick`; its agent then disconnects on the next tick
  (and stops ini auto-join). The host closes the connection anyway (`slotguard_kick`), so an unmodified client is
  removed too; it just notices only at its 90 s connection timeout.

## Findings

- **Retail chat does not work offline.** `Say` hands the text to the game's online chat service
  (`0x141B5E2E0` via the game instance); with no service it returns before even the local echo, so plain chat
  messages go nowhere in this mod. The `ServerSay` RPC is not used by the chat box. (Relaying plain chat through the
  host would be a separate feature; not done here.)
- **Displaying a local line:** the chat box binds its `OnChatMessageReceived(PlayerName, Message, bIsOnMyTeam,
  bIsGlobalMessage)` to the local PC's `OnChatMessageReceived` delegate (PC `+0x6C8`). We call the bound listeners
  (`FScriptDelegate` = {object index, serial, FName}) via ProcessEvent. Right after a Fort Hope load the chat box is
  sometimes not bound yet (binding needs its owning player); then we call the live chat box's handler directly.
  Lines show as `[ALL] coop: ...`.
- **Host → client text:** `APlayerController::ClientTeamMessage` (client RPC) reaches Gobi's override
  `0x141BA7420` (PC vtable `+0x790`), which **drops** the chat types (`Say`/`TeamSay` names at PC `+0x6D8/+0x6E0`)
  and passes other types to the engine, which shows nothing. We send our own types (`b4bcoop`, `b4bcoopkick`) and
  hook the override on the receiver to display them. A client without the agent ignores them.
- **Login error string:** `PreLogin`'s `FString& ErrorMessage` must be game-allocated (the engine frees it). We use
  the game's `TArray<TCHAR>::ResizeForCopy` (`0x140BAF260`, GMalloc) to allocate it.
- **`/restart`:** there is no reflected "restart chapter" call. `/restart` fails the mission the way a wipe does
  (`MissionGameMode::OnMissionEnd(false)`) and lets the game run its retry path; it first asks
  `GobiGameState::GetMissionEndedBehavior(false)` and refuses when the answer is `GameOver` (the run would end).
  On Easy the answer was `PreviousCheckpoint`: post-round screen, then `MissionReset` snapshots and the same chapter
  from its start (corruption cards, character select) for host and client, no map travel. It counts as a failed
  attempt (`NumTimesMissionRestarted`, continues), like a real wipe.
- **`/bots`:** `APlayerSlotManager.bSupportsBots` is copied from the game mode's `bSupportsBots` when the slot manager
  is created (`0x141A13CB0`), and `TeamSupportsBots()` (`0x141A1C640`) = that flag && `GobiWorldSettings.bSupportsBots`.
  We set both the slot manager's and the game mode's flag right before `InitSlots` (hook shared with `teamsize.c`).
  Verified: `/bots off`, next chapter (seamless travel) had slots 2-3 empty and a 2-hero HUD. Only tried on one
  chapter start; a full mission with 2 heroes was not played through.
- **Fort Hope popups:** a refused join used to show the game's generic "Unable to join the session" popup with our
  reason only as a chat line after it. Since a failed join stays in the camp (#41) the game shows no popup at all.
  Since 0.9.3 chat.c shows the host's reason in a popup (title "COULD NOT JOIN"): a game message popup that opens within
  30 s gets `MessagePopupUserWidget::SetText`, else our own (`UIBlueprintFunctionLibrary.OpenMessagePopup`) once the
  camp has settled; logged `chat: refusal popup: ...`. The chat line follows (`chat_local_soon`: no map change needed).

## Testing

Unattended typing: `type <text>` posts real key messages to the game window (Enter to open the chat input, one
`WM_CHAR` per character, Enter to send), so the whole UI path runs exactly as when a person types; the chat input is
not reopened if it is already open, and a second `type` is refused while one is running. `chat <text>` calls
`OnSendMessage` directly, `chatshow <text>` prints a local line, `chat status` lists the chat box and the delegate
listeners, `popup [close]` lists/closes open game popups.

Sequence used (2 instances, one Steam account, so both have the same `steam:` key):

1. Host `/help` in Fort Hope: reply lines in the chat box.
2. Client `/kick 0`: "host only (you are a client)" on the client (`client-kick-refused.jpg`); host log: nothing.
3. Host `/say ...`: shown on the client.
4. Host `/kick 1`: client disconnects immediately, back in its camp, sees "You were kicked by the host.".
5. Client `/join 127.0.0.1:7787`: back in. Host `/ban 1`: client leaves; `b4bcoop-bans.txt` written; client
   `/join`: `PreLogin failure: You are banned from this session.` Host restarted: the ban file is loaded and the
   client is refused again. `/unban all`: admitted.
6. Host `/lock` with the client present, `/kick 1`, client `/join`: refused ("The host locked the session.").
   `/unlock`: admitted. `/lock` again, then `mission Easy`: the client follows into the mission (allowed rejoin).
7. Mission: client `/flashlight on` (host: the client's hero light on, manual), client `/leave`, then `/host` in its
   own camp; host `/restart`, `/bots off` + next chapter.

## Limits

- All local copies share one Steam id, so "refuse a new player but let the old one back in while locked" could only
  be tested one way at a time; the key comparison is the same code either way.
- Bans are by Steam id; IP joins carry it too (the Steam OSS fills the login's unique id). Without one (e.g. a future
  non-Steam transport) the fallback key is the player name, which a player can change.
- `/kick` does not keep a player out; ini auto-join is turned off on the kicked client (with our agent), but they can
  `/join` again. Use `/ban` or `/lock`.
- Chat is only as available as the game's chat box: it can be opened in Fort Hope and in missions; the loading and
  post-round screens don't take input.
