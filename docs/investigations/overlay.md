# `~` overlay (#26)

Dear ImGui window over the game (`native/src/overlay.cpp`, API `native/src/overlay.h`), v1.92.9b pinned in
`tools/fetch-deps.sh` (commit + sha256), compiled as C++ objects into every agent flavor (`native/build.sh`; a C++
compile error now fails the build instead of linking a stale object). It replaces the chat commands: every chat
command has a control (checklist below); the chat commands stay.

## How it hooks (nothing is touched before the first open)
- B4B renders with D3D12 (vkd3d-proton under Proton). On the first `~`: a throw-away D3D12 device, queue and swap chain
  on a hidden window give the vtables; MinHook on IDXGISwapChain::Present (vtbl 8), ResizeBuffers (13) and
  ID3D12CommandQueue::ExecuteCommandLists (10). ExecuteCommandLists records the game's direct queue.
- **Threads (changed from the spike):** the ImGui frame is built on the **game thread** (`overlay_tick` ->
  `build_frame`), so panels read UE state and call module code like a chat handler. After `ImGui::Render()` the draw
  lists are cloned (`ImDrawList::CloneOutput`) into a snapshot; Present (RHI thread) renders the snapshot and uploads
  font-atlas changes. The ImGui lock is taken inside each ov_* call only, never while a panel's draw function runs
  (a panel action may make the game thread wait for the render threads: travel, blocking loads).
- Input: the game window's WndProc is subclassed; raw-input mouse deltas move a software cursor, buttons/wheel/keys/
  chars go to ImGui only; key and button releases still reach the game. `cmds_hotkey_down` returns 0 while open (no
  flashlight/third-person key). Key capture for bindings takes the next key down (Esc cancels, Backspace = none,
  mouse middle/4/5 too).

## Panel registry (overlay.h)
- `overlay_add_panel(name, order, draw)` from a module's init; a tab per panel, sorted by order (Session 10, Players 20,
  Camera 30, Flashlight 40, Cheats 50, Add-ons 60 (looks.c; was Looks & add-ons, before that Models 60, Add-ons 70,
  Browse 72), Settings 90, Updates 95, Help 100). `overlay_add_alias(alias, panel, select, arg)`: another name for
  `overlay tab` (dev), e.g. a tab's old name; `select(arg)` sets something inside it (the Add-ons tab's filter).
- Widgets: `ov_text/_dim/_warn`, `ov_heading`, `ov_button`, `ov_button_confirm` (second click within 3 s),
  `ov_checkbox`, `ov_radio`, `ov_selectable` (list row), `ov_slider(_int)` + `ov_edit_done`, `ov_input_text/int`, `ov_combo`, `ov_key`, tables,
  `ov_tooltip`, `ov_copy`, `ov_header` (collapsing; a dev press toggles it), `ov_table_columns` (fixed / stretch column
  widths in font heights, optional label row), `ov_same_line_fit` (next to the last control only if the next one still
  fits, else below: rows of buttons that wrap in a narrow column), `ov_progress`, `ov_image` / `ov_texture`;
  `ov_texture_set` (new pixels for a texture), `ov_tooltip_image`, `ov_hovered`, `ov_text_dim_line` (never wrapped),
  `ov_table_begin_scroll` + `ov_cursor_y` (a table that fills the tab), `overlay_panel_size` (a tab with its own window
  size and log height), `overlay_close()` (a button that opens a game screen). (`ov_tabs`, the mode row of the Looks &
  add-ons tab, is gone.)
- Permissions: `ov_begin_perm(CMD_HOST|CMD_CHEAT)` ... `ov_end_perm()` greys the block out on a client (or with cheats
  off) and shows the reason (a line, and a tooltip on each disabled control). `ov_allowed()` for single controls.
- Actions: `ov_run("kick #%d", i)` = `admin_slash()` (the chat path: same permission check, same handler, reply in the
  window's log). Settings: `ov_setting(key, val, save)` = the module's ini live handler (`cmds_ini_apply`) and, when
  the edit ends, `cmds_ini_set` for that key only (val NULL = default: the line is commented out). Live ini reload
  keeps the window in sync because panels read the modules' current values every frame.
- Look (#44): `apply_style()` sets padding and spacing above ImGui's defaults (rows 6, frame padding 6x4, table cells
  6x4, section headings 6, all times `overlay_scale`; re-applied when the text size changes), `ov_heading` adds half a
  line above a section (not at a panel's top), the log under the tabs keeps dense lines (2 x scale), and the first
  window is 36 x 42 font heights (clamped to the screen). Panels need nothing of their own for it.
- The window log shows ov_run replies and every local chat line (`chat.c show_text` -> `overlay_note`).
- Dev: `overlay open|close|status|log|tab <name>|press <label>|set <label> <value>|locate <label>|mouse <x> <y>|wheel <n>`
  (press/set drive a control by its ImGui label through the same draw code; disabled controls can't be driven; a
  label `##id` alone matches any control with that id part, for rows whose shown text varies, e.g. `##sel:ak47.pak`;
  one request at a time: wait until `overlay status` no longer says `last test: pending` before the next).

## Parity checklist (main's command set)
| Chat verb (perm) | Overlay control |
|---|---|
| `/help` (anyone) | Help tab: version, `/help` button, "Run a chat command" box |
| `/join` (anyone) | Session: Join box (SteamID64 or target) + Join; friend rows: Join (Steam Join Game path) |
| `/host`, `/leave` (anyone) | Session: Host, Leave (confirm) |
| `/players`, `/ping` (anyone) | Players: live table (ping, Steam ID on the host), Ping, List in the log |
| `/flashlight [on\|off\|toggle\|auto\|status]` (anyone; auto host) | Flashlight: status line, Toggle/On/Off, Automatic (greyed on a client) |
| `/thirdperson [on\|off\|status\|distance\|side\|height\|fov\|reset]` (anyone) | Camera: checkbox, sliders (Ctrl+click = number), Swap shoulder, Reset camera, aim correction |
| `/kick`, `/ban` (host) | Players: Kick / Ban (confirm) per row |
| `/unban`, `/bans` (host) | Players: ban list with Unban per row, Unban all (confirm) |
| `/lock`, `/unlock` (host) | Players: Locked checkbox |
| `/teamsize N` (host) | Players: Team size + Apply |
| `/bots on\|off\|default` (host) | Players: radio game default / on / off |
| `/ready [vote]` (host) | Players: Ready everyone, Ready post-round vote |
| `/restart` (host) | Players: Restart mission (confirm) |
| `/say` (host) | Players: message box + Say |
| `/cheats on\|off\|help` (host) | Cheats: Cheats on checkbox, state line; Help: `/cheats help` |
| `/god /heal /revive` (cheat) | Cheats: God on/off, Heal, Revive for the picked player (me / everyone / #n) |
| `/ammo infinite\|off` | Cheats: Infinite reserve ammo checkbox |
| `/copper +N [p]` | Cheats: amount + Give copper |
| `/card <name>\|list [p]` | Cheats: card name box + Give card, List cards (filter = the box) |
| `/fly /noclip /walk` | Cheats: Fly, Noclip (own hero only, greyed otherwise), Walk |
| `/tp [p] <p\|saferoom\|start>` | Cheats: destination combo + Teleport (who = the picker) |
| `/freecam`, `/size <x>` | Cheats: Free camera, size slider (applied on release) |
| `/horde /killall /freeze` | Cheats: Horde, Kill all ridden, Freeze/Unfreeze AI |
| `/director calm\|build\|peak\|fade\|recover` | Cheats: a button per phase |
| `/spawn <type> [n]` | Cheats: type combo + count slider + Spawn |
| `/slomo <x>` | Cheats: game speed slider (on release), Normal speed |
| `/win /lose` | Cheats: Win / Lose the mission (confirm) |
| `/supply +N`, `/unlockall` | Cheats: amount + Add supply points, Unlock all (both confirm); `/unlockall check` via the Help box |
| `/addons [list]`, `/addons info <#>` (anyone) | Add-ons: a row per add-on in the folder (name, size, installed / off / loads at restart / can't load); details as a tooltip on the name; chat only (Dan, 2026-10-13: no details view, no conflicts list): `/addons info`, `/addons` |
| `/addons on\|off <#>` (anyone) | Add-ons: **Turn on** on a row that is off (runs `/addons on <file>`); **Remove** (any add-on: off now, deleted at the next start, `.shop\pending.txt`) + **Undo**; chat only: `/addons off` (no on/off toggle, no load order in the window: Dan, 2026-10-13) |
| `/addons policy` (host) | Add-ons > Host rules (bottom): "Who may join with add-ons" combo (`addons_policy`, saved + live via `addons_live`); greyed on a client. (`/addons players` and the players' table are gone since #35: add-ons stay private) |
| `/model`, `/model list [<survivor>\|npc\|outfits\|weapons]` (anyone) | chat only since 2026-10-13 (the Wear lists were dropped: Dan wants outfits worn from the game's customization screen) |
| `/model <name>`, `/model reset` (anyone) | Add-ons rows: an add-on outfit's **Wear** (the customization screen with it on, `models_open_screen`), a weapon look's **Use** (`/model <name>`) / **Stop** (`wlooks_reset_code`); other survivors' outfits and NPC bodies: chat only |
| `/model <player> <name>\|reset` (host) | chat only since 2026-10-13 |
| `/models [on\|off]` (host) | Add-ons > Host rules: "Model swaps allowed" checkbox (client: greyed, shows the host's last announced state; `/models` for the state) |
| add-on shop (no chat command; ini `shop`) | Add-ons (`shop=0`: installed rows only): the list fetched by itself on the first arrival in Fort Hope, Refresh, thumbnails, Add (progress bar), Update, Undo, search, filter All / Installed / Available, sort name / installed on, both ways |
| ini keys | Settings: text size, overlay/flashlight/third-person keys, `launch` ("Game start", online.c, #47); Camera: `thirdperson*`; Flashlight: sticky; Session: `presence`, `allow_joins`, `allow_steamids` |

Beyond the chat: Steam friends list with Invite (was dev-only `invite`), SteamID Copy.

## Live (2026-09-25, lane 2, `multi.sh 2`)
- Spike (earlier): hooks `Present 00006FFFFCFBAFF0, ResizeBuffers 00006FFFFCFB9560, ExecuteCommandLists
  00006FFFF702AAA0`; renderer `D3D12, 3 buffers, format 24`.
- Game-thread frames: host `frames built=541 drawn=541` in the first seconds; an unfocused instance builds only a few
  frames per second (the game throttles in the background), still correct.
- Driven through each tab (`overlay press/set`), host in Fort Hope: Host ("already hosting"), /players, /ping, Say
  ("[host] hello from the overlay" on both), Lock/Unlock, bots on/default, team size, Ready everyone, third person on,
  Distance 250 + Swap shoulder -> `thirdperson_distance=250`, `thirdperson_side=-40` written; Reset camera -> both
  commented out, camera 180/40; Flashlight Toggle/Automatic; flashlight key K -> `flashlight_key=K` live (status
  `key=0x4b`) and back; Help box `thirdperson status`; Cheats: God refused before Cheats on (disabled), then on, god,
  heal, ammo, off (notices on the client).
- Mission (Evansburgh_B): host Cheats: god on #1, Horde, Spawn hocker, director calm, Kill all ridden (9), off; all
  notices reached the client. Client: Kick, God, Automatic greyed out with the reason (screenshots), Toggle ("requested
  on from host"), third person on. Host Kick on the client's row: `kicked Hergmgurk`, client back in its own camp.
- Real input (SendInput through the focused window): `locate Ping` -> cursor there -> left click 0.05-0.15 s ran
  `/ping` (the spike's missed short click is gone); `~` key opens; typing `h`,`i`,Enter in the Say box sent `/say hi`;
  W held 1 s while open: hero at the same spot, `L` didn't toggle the light; Esc closed; W held 1 s after: moved ~380.
- Live reload: `thirdperson_distance=320` appended to the ini -> slider shows 320, chat/log "applied".
- `B4B_LANE=2 tools/e2e.py --quick --no-lock`: 13/13 PASS incl. the new overlay smoke check (`#1 frames built=232
  drawn=232; #2 frames built=256 drawn=256`), /tmp/b4b-e2e-l2-20260925-163516.
- Not tried: window resize (ResizeBuffers), native Windows, gamepad.

## Models tab (models.c `models_panel`, since 2026-10-12 Wear in Looks & add-ons; 2026-09-25, lane 2, `multi.sh 2`, `casual_joe.pak` + `mod_ak47.pak` via `addons_dir=`)
- Driven with `overlay press/set`: search `walker_elite` + click `walker_elite_03` -> `/model walker_elite_03` applied;
  NPC bodies `emmett`, Add-on outfits `casual_joe`, `Use##ak47` (AR02 given with `giveitem`: FP/3P meshes from
  `/Game/b4bcoop/weapons/ak47/`), whole survivor `walker` -> `walker_elite_00`; per-type `Reset##w:AR02` -> row back,
  FP overrides 10.
- Host target combo `#1` + click `karlee_elite_05` -> `/model #1 karlee_elite_05` (client's slot shows it, notice on
  the client); Everyone's look `Reset##1` -> `/model #1 reset`.
- `Model swaps allowed` off -> `/models off`, 3 looks reset; the client's tab shows the refusal line and, in its greyed
  Host section, the announced state off. Client: `casual_joe` and `Use##ak47` from its own tab applied.
- `tools/e2e.py --quick` (lane 2): 14/14, /tmp/b4b-e2e-l2-20260925-180035. Screenshots:
  `~/.local/share/b4b-coop/models-tab/shots/` (not committed).
- Combo labels with spaces need quotes for the dev driver: `overlay set '"Change the look of##target"' "#1"`.

## Add-ons tab (looks.c, 2026-10-13): one table instead of Wear / Installed / Get more
Dan rejected the 2026-10-12 "Looks & add-ons" tab (three modes behind a row of tabs, below): "a waste of time and
space". Now one **Add-ons** tab (order 60), one table:
- Rows: every add-on in the folder (`addons_info`, also hand-dropped paks) and every entry of the shop's list
  (`shop_row`) not already there; a shop entry whose file is installed, or whose content id equals an installed
  file's (e.g. `my_joe.pak` = the shop's `casual_joe`), is that file's row (thumbnail and shop name from the list).
  Installed rows always first; then by name or by the time installed (file time: newest of created / written,
  `AddonInfo.time`), each way round (sort combo + A-Z / Z-A or Newest / Oldest button); search (name, file, id,
  author, description); filter All / Installed / Available.
- Columns (`ov_table_columns`: thumbnail fixed 6 em = 78 px at text size 1, 98 px at 1.25; then stretch 1 / 0.45 /
  0.62): thumbnail (hover: the large picture, 20 em on its longer side, `shop_preview` + `ov_tooltip_image`) | name,
  and the file size on its own line (`ov_text_dim_line`: never wrapped; author, description, license and file name as
  the tooltip, nothing else in the cell) | installed state, short: `v1.0`, `v1.1 available`, `off`, `loads at
  restart`, `can't load` (why as tooltip), `removed at restart`, `updated at restart`, `as <file>` (another file adds
  the same look), `needs newer b4bcoop`, a failed download's message | controls, wrapping in the column (`ov_same_line_fit`): Add / progress bar while downloading, Undo, Turn on
  (`/addons on`, an add-on that is off in `addonlist.txt`), Wear / Use / Stop (`addons_look_buttons`), Update, Remove
  (two clicks).
- No load order, no on/off toggle, no details view, no conflicts list (Dan: if two add-ons overwrite the same file one
  wins, that's the player's problem; not a mod manager). The backend is unchanged (`ord[]`, `addonlist.txt`,
  `/addons on|off`, conflict log lines and the chat notice); `addons_move` (Up/Down) is gone.
- Size (coordinator review, 2026-10-13): the table fills the tab down to the footer and scrolls by itself
  (`ov_table_begin_scroll`: ImGui ScrollY table, label row frozen; the footer's height is measured each frame with
  `ov_cursor_y`); when less than 3 rows fit (960x540) it is a plain table and the tab scrolls as a whole, so there is
  never a second scrollbar. The tab has a window size of its own (`overlay_panel_size(TAB, 52 em, the screen's
  height, 3 log lines)`: overlay.cpp `track_size` switches the window to it when the tab is shown and back to the
  usual size for the other tabs, keeping the player's resizing per side; the log under the tabs is 3 lines on this
  tab, 6 elsewhere).
- **Wear** (an add-on outfit): `models_open_screen(name)` (models.c, both builds; was dev `mdl cs open`):
  `EnterCharacterCustomization(player state)` on Fort Hope's `CharacterCustomizationManager`, `OpenScreen
  CharacterCustomization 0` (a GameInstance exec), `SetCharacter(current hero row)`, then `EquipCustomization(Outfit,
  b4bcoop.outfit.<name>)` so the mannequin wears it; the overlay closes. The game's Back (Esc) goes to its Cleaners menu,
  a second Back to Fort Hope; the screen's own close stores the choice (#33: `<hero> now wears <name> on the
  customization screen`, profile keeps its look). The usable that normally leads there restores the view; ours doesn't,
  so `tick_screen` (models_tick, 0.5 s) watches the screen (`UIScreen::IsOpen`) and when it closes calls
  `ExitCharacterCustomization` and, if the view target is still the screen's camera (`Character_CineCameraActor`),
  `SetViewTargetWithBlend(pawn)` (without it the camera stayed in the customization tent). Outside Fort Hope (no
  manager) or with `outfits_screen=0` the button is disabled with "Wear it from the character customization screen in
  Fort Hope." (resp. the ini reason). Several outfits in one add-on: a Wear per outfit ("Wear <title>").
- **Use / Stop** (a weapon look): `/model <name>` / `wlooks_reset_code(code)`; `wlooks_current(code)` tells which.
- The Wear mode's lists (other survivors' outfits and pieces, NPC bodies, "Change the look of", "Everyone's look") are
  gone; those stay chat commands (`/model`, `/model <player> <name>`). Base-survivor text ("made on mom") is no longer
  shown anywhere (it was in Wear's add-on outfits list and the Installed details).
- The shop's list is fetched by itself once per game start (`auto_fetch` in shop_tick: 3 s in a Fort Hope with a hero,
  signed in; host or client), Refresh at will; thumbnails are fetched when the tab first shows the list (their own
  worker, `want_thumbs`, once per list). `shop=0`: no list, installed rows only.
- Under the table: "Load add-ons" (`addons`) + "Copy folder path" (path as tooltip). **Host rules** at the bottom (one
  heading, not under every view): "Model swaps allowed" + "Who may join with add-ons" combo (any / cosmetic only / no
  add-ons / same gameplay add-ons), both `ov_begin_perm(CMD_HOST)`.
- Dev: `overlay tab Models|Browse|Get more|Wear|Looks|Looks & add-ons` = this tab (filter All, search cleared),
  `overlay tab Installed` = filter Installed; `overlay set ##filter Installed`, `overlay set ##sort "Installed on"`,
  `overlay press ##sortdir`, `overlay set ##search holly`, `overlay locate <label>` for row order (shop-test). New dev
  command `key <vk>` (chat.c: one key press to the game window; `key 0x1B` = Esc, the screen's Back).
- Live (2026-10-13, lane 2 with the native Steam client, `B4B_GPU=4090`, dev build, `multi.sh 2` with the shop-test
  catalog): host and client at 960x540 and 1920x1080, text size 1.25 and 0.75; the list fetched by itself on both
  (`thumbs=0` until the tab opened, then 3 uploaded); Add `casual_joe` -> installed row on top with Wear + Remove;
  Wear -> the customization screen with Casual Joe on Holly, Esc twice -> `holly now wears casual_joe on the
  customization screen`, `customization screen closed: left the customization view, view back on the hero`, third
  person shows Holly in the outfit; the client the same (Doc), the host sees it; Add `ak47`, Use -> `your AR02 now
  looks like AK-47`, the row shows Stop, Stop -> `your AR02 back to your own skin`; Z-A, filter, search; client: Host
  rules greyed with the reason. Screenshots: `~/.local/share/b4b-coop/addons-tab-shots/` (not committed).
- Real shop (2026-10-13, after the review: bigger thumbnails, hover picture, table filling the tab, short Installed
  cell; lane 2, `multi.sh 2`, no catalog override, empty add-ons folders): on arrival in Fort Hope `shop: in Fort Hope
  for the first time this game: getting the add-on list` -> `127 add-on(s) in the list` (netguard scope open/closed
  around it); opening the tab fetched the 127 thumbnails in ~6 s, overlay frames/s 57-67 meanwhile (61-67 idle);
  scrolling, search `ranger` (5 rows), Z-A, Available; Add `rangergreen` (2.5 MB) -> "Added and ready now", its row on
  top with Wear + Remove; Wear -> customization screen, Esc twice -> `holly now wears rangergreen`, view back on the
  hero, Holly in the Green Ranger outfit; Remove -> `removed at restart` + Undo, `pending.txt remove rangergreen.pak`;
  restart -> `shop: removed rangergreen.pak`. 960x540: about 2.5 rows visible, one scrollbar (the tab's); 1920x1080:
  the table fills a window as tall as the screen (8 rows at 1.25, 12 at 0.75), Host rules under it; the Session tab
  keeps its usual size. Screenshots: `~/.local/share/b4b-coop/addons-tab-shots/v2/`.
- `tools/shop-test.py` (adapted, see shop.md §5): **68/68** (/tmp/b4b-shop-20261013-032556). `tools/e2e.py --quick`
  (lane 2): **14/14** (/tmp/b4b-e2e-l2-20261013-033617); its overlay smoke check now checks the automatic list fetch
  and presses Refresh in this tab (was `Status (/model)` in Wear); its socket check lists the 6 HTTPS connections to
  GitHub (the shop's list on both instances) apart as expected.

## Looks & add-ons tab (looks.c, 2026-10-12, replaced 2026-10-13): Models + Add-ons + Browse in one tab
Dan: "there's three tabs for 'browse', 'addons', and 'models' and it feels like it could be one." One tab (order 60),
three modes in a row of tabs at its top (`ov_tabs`), one search box shared by all, and a collapsed **Host rules**
header under every mode. The player's flow: find (Get more) -> get (Add) -> wear (the row's Wear / Use, or Wear), and
manage what is installed (Installed), without changing tabs. Each mode stays in its module:
- **Wear** (`models_wear_panel`, models.c; `wlooks_panel`, weaponlooks.c): your look, then `Reset my look`,
  `Status (/model)` and your pick on one line; host with someone else in the game: "Change the look of" combo (+ "Reset
  their look"); lists by radio, in two rows (one row didn't fit the 36-em window): Survivors / NPC bodies, Add-on
  outfits (n) / Weapon looks (n). With text in the search box every list's matches show at once under their own
  headings (survivor pieces, NPC bodies, add-on outfits, weapon looks: name, title, add-on, weapon code). "Everyone's
  look (n)" is a collapsed header (Change / Reset per row, greyed on a client).
- **Installed** (`addons_installed_panel`, addons.c): "Check for updates" (fetches the shop's list, as Get more does;
  then "N update(s) in the shop's list"), then a row per add-on in load order: on/off checkbox | "#. title" (click =
  details under the row) + kind (outfit / N outfits / weapon look (AR02) / replacement / gameplay), state, a waiting
  Remove/Update, the shop's last message | Wear / Use (`addons_look_buttons`; several looks: "Wear (n)" opens Wear
  with the add-on's title as the search) + Update or Undo (`shop_installed_actions`) | Up / Down. Details: as the old
  Add-ons tab plus the looks it adds with Wear / Use and **Remove** (any add-on file, not only the shop's: `.shop\
  pending.txt` now keys by file name; `shop_early` accepts any plain `.pak` name for `remove`, shop ids only for
  `update`). Conflicts, then a collapsed "Folder and troubleshooting" header (path + Copy, how to install by hand,
  "Load add-ons" = `addons`, `/addons`); with `addons=0` the warning and the checkbox come first.
- **Get more** (`shop_get_panel`, shop.c; hidden with `shop=0`, the mode falls back to Installed): the shop list as
  before; the intro text becomes the Refresh button's tooltip once the list is there; filter (all / new looks /
  replacements / not installed yet; "In my add-ons" is Installed now) next to the search box; an installed row shows its
  state, **Wear / Use** as soon as the add-on is loaded (the restart-free path of 0.9.6) and Update; Remove moved to
  Installed; messages ("Added and ready now.") in the middle column; details under the row.
- **Host rules**: "Model swaps" (`models_host_panel`) and "Who may join with add-ons" (`addons_mp_panel`), both
  `ov_begin_perm(CMD_HOST)`.
- Overlay changes: `ov_tabs` (mode row; a mode change scrolls the panel to its top), `ov_header` drivable (dev press
  toggles), `overlay_add_alias` (`overlay tab Models|Add-ons|Browse|Wear|Installed|Get more|Looks` = the tab in that
  mode), `overlay press "##id"` matches a control by its id part (rows whose text has a number or version), details
  under a row = the table ends and a second `ov_table_begin` with the same id continues (ImGui keeps synced columns).
- Nothing dropped: every control of the three old tabs is there (checklist above). Changed: the host's "Change the look
  of" combo is hidden instead of greyed on a client and while the host is alone (it only had "you"); "Status in the
  log (/model)" is "Status (/model)"; Remove of a shop add-on is in Installed (details), not on its Get more row.
- Live (2026-10-12, lane 2, Flatpak Proton, `B4B_GPU=4090`, dev build): `multi.sh 1` and `multi.sh 2` at 960x540 and
  1920x1080 (`B4B_RES` + the prefixes' GameUserSettings.ini, put back after), text size 1.25 (default) and 0.75: every
  mode on the host and a client; real shop list (127 add-ons, thumbnails), search `coach` + `Add##coach` -> "Added and
  ready now.", the row's `Wear##coach` -> `/model coach` worn; Wear search `co` lists matches across lists; Installed:
  details under the row, Remove of `My Holly.pak` (a hand-installed file with a space) -> `pending.txt` `remove My
  Holly.pak`, `=0`, Undo -> both back; client: Host rules greyed with the reason, `Wear##casual_joe` in Installed ->
  the host logs `hero slot 1 wears outfit casual_joe`; host with a client: "Change the look of" `#1` shows their add-on
  outfit selected. Screenshots in `~/.local/share/b4b-coop/looks-tab/` (not committed).
- `tools/shop-test.py` (adapted: `overlay tab Get more|Installed`, the old names checked as aliases, Wear on the Get
  more row after Add, Remove from Installed's details, casual_joe's Update with Installed's button, Remove + Undo of a
  hand-installed add-on, client 2 fetches, adds and wears through its own window): **58/58**
  (/tmp/b4b-shop-20261012-021647). `tools/e2e.py`'s overlay smoke check now also opens this tab and presses
  `Status (/model)##status` (the `/model` reply in the log); run alone against `multi.sh 1`: pass.

## Dev screenshots of the presented frame (`screenshot <windows path>`)
- Why: engine screenshots (`exec shot`, UE's viewport grab) contain the 3D scene only: no ~ overlay, no UMG, and
  all black on UI-only screens. e2e's `duo-end-*.png` were 370-byte black PNGs (960x540, 1-bit): at `stop()` both
  instances sit on the next chapter's pre-round corruption-card screen (Evansburgh_C) or its loading screen. Sampled
  live every 10 s through the transition: engine mean 0.34 on Evansburgh_B, exactly 0 on Evansburgh_C's pre-round;
  the Present capture showed the "Supply points earned" / "Corruption cards" screens.
- How (overlay.cpp, `#ifndef B4B_RELEASE`): the command installs the Present/ECL hooks if the window was never opened
  and sets a request; the next Present (RHI thread, after the overlay has drawn) copies the current back buffer
  (PRESENT -> COPY_SOURCE -> PRESENT) into a READBACK buffer (`GetCopyableFootprints`, any size) on the game's direct
  queue with its own allocator/list/fence. A worker thread waits for the fence, converts to 8-bit RGB (R8G8B8A8,
  B8G8R8A8/X8, R10G10B10A2 = top 8 bits, R16G16B16A16_FLOAT scRGB -> sRGB, clamped; HDR10/PQ is not decoded) and
  writes an uncompressed PNG (stored deflate blocks, no dependency; `<path>.part`, then renamed). One capture in flight;
  `screenshot` alone prints the last result (`wrote ... (960x540, format 24)`; the game's back buffer is format 24 =
  R10G10B10A2_UNORM).
- `launch/shot.sh` uses it first (path in the prefix's Saved/Screenshots, moved out and resized), then the engine shot,
  then (no `B4B_GPU`) the X11 window grab.
- Live (2026-09-25, lane 2, `B4B_GPU=4090` headless gamescope, `multi.sh 2`): host and client in Fort Hope (HUD
  visible), with the overlay open on the Players tab (both), in Evansburgh_B character select (plus the client's
  Cheats tab, greyed), post-round and pre-round screens; `shot.sh` takes ~0.6 s. `e2e.py --quick --no-lock` 13/13 with
  real `overlay-1/2.png` and `duo-end-1/2.png` (/tmp/b4b-e2e-l2-20260925-203705). Windowed run (no `B4B_GPU`) not
  re-checked: the 5090 was busy.
