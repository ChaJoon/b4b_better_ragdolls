# Add-ons: L4D-style loader, format and enable list (#20)

Status 2026-09-25, build 14216215, Proton. Epic #23. Code: `native/src/addons.c` (loader, `/addons`),
`native/src/paks.c` (mount + signature exemptions, see model-mods-paks.md), `modkit/addon.py` (packer).
Player page: docs/COMMANDS.md "Add-ons".

## TL;DR
- Drop-in: `<game>\b4bcoop-addons\<name>.pak` (next to `Back4Blood.exe`, outside `Gobi\Content\Paks`, which the
  engine mounts itself with signature checks). An add-on zip holds `b4bcoop-addons/<name>.pak`: unzip into the game
  folder. Restart to apply; no in-game browser.
- One file per add-on: our pak format (modkit/b4bpak.py) with an extra root entry `b4bcoop-addoninfo.txt`
  (title, author, version, category, description, content). A pak without it still loads (title = file name).
- `addonlist.txt` in the folder: `<file>.pak=1|0`, top to bottom = load order, later wins. New paks are appended,
  switched on, sorted by name; the agent rewrites the file then. `/addons on|off` edits it.
- Conflicts (same file path in two enabled add-ons) and "mixed" packages (a package's .uasset/.uexp/.ubulk ending up
  from different add-ons: can crash) are logged, listed by `/addons`, and one chat notice is shown after the first
  map load.
- Player builds: paks.c installs **no hook** unless at least one add-on will be mounted. Retail paks keep every
  signature check either way.
- Multiplayer (#22, §6-§7): add-ons stay local (each player sees their own, others see vanilla). Each add-on is
  classified from its files as cosmetic or gameplay-affecting. The host announces its
  `addons_policy=any|cosmetic|none|match` (default `any` since #43, before that `cosmetic`); since #35 the joiner checks its own add-ons against
  it and doesn't join if they fail, telling only its own player. Nothing about a player's add-ons is sent. No
  protocol bump (§7).

## 1. Format
Folder (default `<DLL dir>\..\..\..\b4bcoop-addons`, normalized with GetFullPathName; ini `addons_dir=<windows path>`,
`addons=0` = off). Only `*.pak` files directly in it.

Add-on pak = pak v9 (or v8, §1a), magic 0x18772, uncompressed, plain index, mount point `../../../`, names `Gobi/Content/...`
(or `Engine/Content/...`), plus `b4bcoop-addoninfo.txt` at the root (`../../../b4bcoop-addoninfo.txt` in the
engine's file system: harmless, never read by the game, excluded from conflicts):
```
title=Holly magenta portrait
author=...
version=1.0
category=ui
description=...
content=cosmetic (textures)
```
`content=` is written by `addon.py pack` from the files (§6); the agent derives it again and only logs a mismatch.
`outfit=<name>|<survivor>|<3P mesh>|<FP mesh>|<title>` lines (any number) name added outfits for `/model <name>`
(new-assets.md §8; written by `b4bmod survivor --as`).
The parser also takes L4D KeyValues lines (`addontitle "..."`, `addonauthor ...`; the `addon` prefix is dropped, braces
ignored), so an L4D-style addoninfo.txt mostly works as is. Suggested categories (free text):
survivors ridden weapons items ui sounds maps misc.

Content id for #22: the index SHA1 from the pak footer (it covers every entry's SHA1), logged as `id <sha1>` and
printed by `addon.py pack/info`.

### 1a. Pak v8 (older community paks)
Some community B4B paks are **v8**: same B4B format (magic 0x18772, same field order, entry layout, uncompressed,
plain index), but a **221-byte footer** with no `bIndexIsFrozen` byte (compression names at +61 instead of +62).
The engine accepts them as is: `FPakFile::Initialize` probes footer sizes 9..1. The loader does the same
(`native/src/pakfmt.c`, unit tests `native/test/pakfmt_test.c`): 222-byte window with v9, else 221 with v8; any other
version, v8 with a 222 footer or v9 with 221, stock magic, encrypted or frozen index, index outside the file ->
the usual `damaged, or not a Back 4 Blood add-on pak (magic .. v..)` / `encrypted pak index` / `bad index position`.
The index SHA1 check and content id are unchanged (a v8 pak and its v9 re-headered copy have the same id). The
per-add-on log line ends in `, pak v8`. `modkit/b4bpak.py` reads v8 footers (always writes v9); `addon.py info/check`
apply the same checks with the same messages, and `addon.py pack <v8.pak>` repacks as v9.

Live (dev build, lane 2, 2026-09-27): three original v8 character paks (270/162/158 files, 1.1 GB) dropped unchanged
into `b4bcoop-addons`: `addons: 1. ... id 76211819..., pak v8`, `paks: mount ... order 1000 -> ok (unsigned)` for all
three, `no .sig lookup for mod pak ...`, `precacher: first read from a mod pak`; no `Fatal`/`Corrupt file`. In Fort
Hope, `/model holly_elite_00` showed the replacement model in third person (screenshot kept local).

## 2. Loader (addons.c)
- `addons_scan()` runs in DllMain from `paks_early_init` (before the engine exists; kernel32 file I/O only): reads
  `b4bcoop.ini` (`addons`, `addons_dir`), `addonlist.txt`, lists `*.pak`, appends new ones, rewrites the list if it
  changed (`.tmp` + MoveFileEx), then for each present add-on in order: footer checks (`pakfmt.c`: magic, version 9 or 8
  (§1a), index not encrypted/frozen, bounds), reads the index and **verifies its SHA1** (a corrupt index would be an engine Fatal at
  mount), walks the entries (bounds-checked), reads the addoninfo entry (uncompressed, data at entry offset + 53 bytes
  of in-data FPakEntry). Returns the number of add-ons to mount. Non-ASCII file or folder names are refused (the
  exemption matching compares narrowed paths).
- Read order: 1000 + position among present add-ons (retail 4, +100 per `_P` patch level; dev `modpaks=` 3000+,
  `mountpak` default 3500). `FindFileInPakFiles` takes the highest read order, so later = wins.
- Conflicts: a hash map file path (lower case, `../` stripped) -> last add-on; every overwrite by another add-on
  counts for that (loser, winner) pair. Then package stems (path minus `.uasset/.uexp/.ubulk/.uptnl/.umap`) -> owner;
  two owners for one stem = "mixed". Logged as `addons: conflict: ...`; `addons_init()` (init_thread) queues one
  `chat_local_later` notice (mixed first, then a duplicate, then a single conflict by title, else a count, else
  unloadable add-ons).
- Duplicates (2026-10-05): two add-ons on at start with the same content id (one add-on under two file names, e.g. a
  player's `batman.pak` and the shop's `batman-any.pak`) or declaring the same outfit/weapon look name (only the later
  one's is worn). Logged `addons: duplicate: a.pak and b.pak are the same add-on ...` / `... both add outfit x: b.pak's
  is used`; notice `Add-on "X" is installed twice (a.pak and b.pak): switch one off.` / `Add-ons a.pak and b.pak both
  add outfit x ...`; `/addons` and Installed (`~` window) say the same instead of "overrides" (identical paks also share every
  file path, which alone read as a plain conflict).
- `addons_mount()` from the `FPakPlatformFile::Initialize` hook, right after the retail paks:
  `paks_mount_unsigned(path, order)` (bSigned cleared for that Mount only, path recorded for the other two
  exemptions).
- `/addons` (`/addon`) is a chat command for everyone (admin.c table), game thread: `list` (default), `on|off <#>`,
  `info <#>`; `<#>` = number, file name, title or a unique substring. States: on, off, on/off after restart,
  on NOT LOADED (why in `info`), MOUNT FAILED, missing (listed but file gone: kept in the list so its setting
  survives).

## 3. Packer (modkit/addon.py; mod makers use it through `b4bmod pack/check/install`, modkit/docs/addons.md)
- `pack <src> [-o out.pak] [--title ... --zip]`: `<src>` = folder of cooked files laid out like the game
  (`Gobi/Content/...`: what `dumpassets` writes and the modkit tools produce) with optional `addoninfo.txt` at its
  top, or one of our uncompressed paks (repacked). Refuses files outside `Gobi/`/`Engine/` and non-ASCII names; warns
  about a `.uasset` without `.uexp` and a lone `.uexp/.ubulk` (partial package = mixes with the game's own).
  Deterministic output (sorted entries, fixed zip timestamps). `--zip` = `b4bcoop-addons/<name>.pak` in a zip.
- `info <pak>`, `check <addons dir>`: the same order/on-off/conflict rules as the agent, offline.

## 4. Verification
Offline (Proton 10 wine, harness linking addons.c with stubbed LOG/mount/chat): new add-ons appended sorted and
switched on; truncated pak -> `damaged, or not a Back 4 Blood add-on pak`; text file -> `not a pak file`; order from
the list; `off` add-on not mounted; conflict (2 files) reported with the later add-on winning; `.uexp`-only add-on
after a full one -> "mixed" warning; `/addons on 1` rewrites the list and says `applies after restart`.

## 5. Live results (player build, test instance 1, add-ons in the real default folder)
Player builds have no `offline=1`; a no-click sign-in came from the command line:
`launch/instance.sh 1 -Port=7787 +b4bcoop_join proto:2 ver:0.3.0 addr:127.0.0.1:7799` (auto sign-in Offline, 6 failed
joins to a dead loopback port, then `auto: hosting` in its own Fort Hope). Test content: the #17 magenta Holly
HUD portrait, and a green copy (BC7 mode-6 block `4000e0ff0700feff0100000000000000` over the same 320x208 mip),
packed with `addon.py pack` (evidence under `~/.local/share/b4b-coop/addons-test/evidence/`).

| Run | addonlist.txt | Result |
|---|---|---|
| 1 | magenta=1, green=0 | `addons: ... 2 add-on(s), 1 to mount, 0 conflict(s)`; only magenta mounted (order 1000); `paks: precacher: first read from a mod pak (...holly_magenta.pak)`; HUD portrait **magenta** in Fort Hope |
| 2 | magenta=1, green=1 | `addons: conflict: holly_magenta.pak and holly_green.pak both change 2 file(s) ...: holly_green.pak wins`; both mounted (1000, 1001); chat `[local coop, delayed] Add-on conflict: "Holly green portrait" overrides "Holly magenta portrait". /addons`; portrait **green** |
| 3 | no folder | `addons: no add-ons folder (...)`, no `paks:` line at all: no hook installed |

No `Fatal`/`Corrupt file` in any run. This also re-verifies the #17 path-matching refactor: the exemptions match
the absolute `Z:\...\b4bcoop-addons\*.pak` paths (`no .sig lookup for mod pak ...`, precacher line above), with
spaces in the path. Not run live: `/addons` typed in chat (same code as the harness, which printed the outputs
above; player builds have no `type` command), `addons_dir=`, a damaged pak.

## 6. Content class (#22): cosmetic vs. gameplay-affecting
Code: `native/src/addonclass.c` (agent, at load, from the pak) and `classify()` in `modkit/addon.py` (at pack,
written as `content=`): same rules, kept in sync. The author's label is never trusted.
- Every `.uasset` is read (header only: summary, name map, import map, export map; B4B = legacy -7, unversioned,
  104-byte export entries, like `modkit/upkg.py`). Each export's class: import -> (module = the class import's
  outer package, class name); an export class (index > 0) = a blueprint's default object.
- **Cosmetic** when every export's class is one of: textures (`Texture2D`, `TextureCube`, arrays, volume, render
  targets, light profiles), materials (`Material`, `MaterialInstanceConstant`, `MaterialFunction*`,
  `SubsurfaceProfile`), meshes (`SkeletalMesh` + `SkeletalMeshSocket`, `MorphTarget`, LOD settings, `StaticMesh`,
  `StaticMeshSocket` and its own `BodySetup`/`NavCollision`, `/Script/ClothingSystemRuntime{Common,Nv}`), animations
  (`AnimSequence`, montages, composites, blend spaces, pose assets), sounds (Engine sound classes, `SoundNode*`, all of
  `/Script/AkAudio`), UI (all of `/Script/UMG`, `/Script/MovieScene{,Tracks}`, fonts, `StringTable`, Slate style
  assets, a widget blueprint's class/functions/default object), effects (Cascade `Particle*`/`Distribution*`, all of
  `/Script/Niagara`). A `SkeletalMesh` package must import a `Skeleton` (it uses one of the game's skeletons).
- **Gameplay** for anything else; named reasons: physics asset, physical material, data/curve tables, curves,
  blueprint, animation blueprint, skeleton, map (`.umap`, `World`), level sequence, instance of another package's
  blueprint class, unknown class.
- Static meshes (with their collision) and animations are cosmetic by decision: players read "cosmetic" as "what it
  looks like", and a prop or animation swap is that to them. On a joiner (the only one the policy judges) the host's
  collision and the host-side effects of anim notifies are authoritative, so the residual effect is local.
- Other files: `.ubulk`/`.uptnl` (bulk data) cosmetic; `.uexp` without its `.uasset` in the same add-on gameplay
  (class unknown); `.locres`/`.ufont` ui, `.bnk`/`.wem` sounds, shader libraries materials; `.ini` gameplay (config);
  any other type gameplay.
- Checked on 1477 packages under `~/.local/share/b4b-coop/{extract,meshes,texwork}`: C (built natively with a small
  driver) and Python give identical results. Extracted game packages: 447 textures, 151 materials, 619 meshes
  (all 601 mesh-mod outputs incl. 12 with cloth) cosmetic; blueprints, `GuidDataTable` (`*_Customization_DT`),
  skeletons, physics assets gameplay. (Animations and static meshes were gameplay in that run; reclassified since.)
- Logged per add-on: `addons:    content: cosmetic (textures)` / `content: gameplay (1 file): data table:
  Walker_Customization_DT.uasset`; `its addoninfo says content=cosmetic; the files say gameplay (the files decide)`.
  `/addons` shows `[on, cosmetic]`, `/addons info` the kind, the first gameplay file and the short id.

## 7. Multiplayer (#22; private since #35)
L4D-like: add-ons are local content, nothing is downloaded, and since #35 nothing about a player's add-ons leaves
their PC (no ids, names or counts; Dan: "I don't want other clients to know what I have installed"). A player sees
their own add-ons on everyone, the others see vanilla. Code: `native/src/addons_mp.c`; gate in `admin.c` PreLogin;
join side in `cmds.c` `cmd_join`, `presence.c` `handle_connect`, `chat.c` `chat_on_join_failed`.
- **Policy** (host): ini `addons_policy=` / chat `/addons policy <x>` (session only) / `~` Looks & add-ons, Host rules, default =
  `ADDONS_POLICY_DEFAULT` in addons_mp.c (`any` since #43, 0.8.1 and before `cosmetic`): `any` = everyone;
  `cosmetic` = no gameplay add-ons; `none` = no add-ons; `match` = gameplay add-ons exactly the host's (cosmetic free).
  A change during a session (chat, `~` tab, live ini) seats the remote players present at that moment (`seated[]`,
  their `steam:<id64>` keys, until the host quits): their logins pass the gate, so the reconnect of a mission start
  doesn't drop them back to their own Fort Hope (2026-10-04: `/addons policy none` then `mission Easy` with a client
  running add-ons: refused 3x and stranded before; now `was in the session when the policy changed, welcome back`).
- **Announce** (host): the rich presence connect string carries ` addons:<policy>`, e.g. `+b4bcoop_join steam:<id64>
  proto:2 ver:0.7.1 addons:cosmetic`. `match` adds the host's own gameplay add-on ids (8 hex digits of the content
  id): `addons:match:23a4441c.ab12cd34` (max 16 = `MATCH_MAX`; the value is 256 chars max). Choosing `match` is the
  host's opt-in to show joiners those ids (the `~` tab and `/addons policy match` say so); the other policies reveal
  nothing about the host's add-ons.
- **Joiner checks itself** against the announced policy (`addons_join_refused`): Steam Join Game / invite / launch
  command line (`handle_connect`: before arming the join, so no connection at all), and `steam:<id64>` joins from the
  CLI/ini/overlay read the host's presence from Steam's friends cache (`presence_note_host_addons`). Fail -> local
  chat only, e.g. `Could not join: This host allows only cosmetic add-ons; turn off Walker data table test (~ window:
  Looks & add-ons, Installed; or /addons off <#>) and restart the game.` / `This host allows no add-ons; turn off ...` / `This host
  requires the same gameplay add-ons as theirs. Turn off: X. Turn on or install: Y (id; not installed here). Then
  restart the game.`; auto-join and the session join target stop (only a restart changes add-ons). Pass -> the login
  carries `?b4bcoopaddonsok=<policy>` ("checked against your policy": nothing the host wouldn't learn from the join).
- **Add-ons loaded during a session** (Get more, then the Browse tab; `/addons on`; 2026-10-11, shop.md §1a): as a client the joiner
  applies the same check to the add-on it is about to load (`addons_session_allows`, the policy this join learned):
  cosmetic ones load at once under `any`/`cosmetic`/`match` (so its claim for the mission-start reconnect stays true),
  under `none` they wait for the next start ("this host allows no add-ons"). Nothing is sent; gameplay add-ons never
  load during a session.
- **Policy unknown** (IP join, `host_ip=1` or loopback tests; a `steam:` target whose presence Steam doesn't have): no
  option. A new host with a policy other than `any` refuses that login with `Host allows cosmetic add-ons only. Your
  b4bcoop checks your add-ons and joins again. [addons:cosmetic]` (log `addons: login X: asked to check its add-ons
  against addons_policy=cosmetic`); the client (`addons_on_refusal`) checks: pass -> joins again 1 s later from its
  camp with the claim (`cmds_join_retry`, quietly), fail -> the local message, no retry. The host then only learns
  "asked, didn't come back", like any aborted join. A note from a refusal only feeds the claim, never a local refusal
  (the host may change its policy); presence notes are re-read at every join. Two refusals within 60 s after passing
  -> stop ("keeps asking"). The refusal costs one round trip (no popup: a failed join stays in the camp, #41). A
  failing check shows the reason in a popup ("COULD NOT JOIN") and the chat, and also ends the rejoin window of a
  mission-start reconnect at once (join-timing.md "Refusals with a reason").
- **Host keeps and shows nothing**: `/addons players` and the Add-ons tab's players table are gone (`/addons players`
  answers that it's gone). Log: only `checked its add-ons against addons_policy=<p>` / `asked to check` / for an older
  client `refused by addons_policy=<p> (an older b4bcoop; its add-on list isn't logged)`. `log_redact_addons` (log.c)
  cuts any `b4bcoopaddons=` value to `-` in our lines and in captured engine lines (the dev `PreLogin options` line,
  LogNet/LogGameMode login URLs). The engine logs the login error (`PreLogin failure: ...`), so errors to older
  clients carry only a count, never their titles.
- **Compatibility, no protocol bump** (both directions keep joining):
  - older client (0.6.0-0.7.0) -> new host: still sends `?b4bcoopaddons=<C>c<G>g,g<id8>-<Title>,...`; the host judges
    it as before (`summary_fails`: counts, `match` by ids) and refuses with `Host allows cosmetic add-ons only; you
    have 1 gameplay add-on(s). Switch them off ...` (the old client stops on " add-ons"). Its list crosses the wire
    because the old client sends it; updating stops that.
  - new client -> older host (0.6.0-0.7.0): the older host's presence has no `addons:` -> the client assumes `cosmetic`
    (every older version's default) and checks itself (`This host allows only cosmetic add-ons (an older b4bcoop: its
    default); ...`). An older host with `any` therefore can't be joined with gameplay add-ons until it updates (safe
    side). IP join to an older host: the client sends nothing and the older host sees "no summary" = accepted: the
    policy is not enforced there (IP joins are the advanced opt-in; see the next line).
  - A modified client can ignore the policy or lie; that was always true (the summary was self-reported). The policy
    keeps friends consistent, it isn't anti-cheat.
- Not private by design: wearing an **added outfit** or **weapon look** (`/model`) sends that row name
  (`b4bcoop.outfit.<name>`, `b4bcoop.weapon.<name>`) through the game's replicated customization, so players with the
  same add-on see it (models.c, weaponlooks.c; new-assets.md §8-9). That happens only when the player puts one on.

### Live results #35 (2026-09-27, lane 2, Flatpak Steam, dev build, `multi.sh 2`; client add-ons via `addons_dir=`,
test paks from `~/.local/share/b4b-coop/addons-mp/`; screenshots in `~/.local/share/b4b-coop/addonprivacy/`, not
committed)
- Host presence: `connect="+b4bcoop_join steam:76561198994546085 proto:2 ver:0.7.0 addons:cosmetic"`.
- Client with `walker_dt_test` (gameplay), host default: loopback join -> host `addons: login dreamsofants: asked to
  check its add-ons against addons_policy=cosmetic`, nothing else; client `addons: host 127.0.0.1:7887 asks us to
  check ...`, `auto: join 127.0.0.1:7887 stopped`, game popup, after closing it chat `Could not join: This host allows
  only cosmetic add-ons; turn off Walker data table test (~ window, tab Add-ons, or /addons off <#>) and restart the
  game.`
- Same client, `steamjoin "+b4bcoop_join steam:<id> proto:2 ver:0.7.1 addons:cosmetic addr:127.0.0.1:7887"`
  (presence path): `addons: not joining steam:...: our add-ons don't pass its policy cosmetic (told only to us)` +
  the chat line at once; the host log got no login line at all. Without `addons:` (older host): the same with
  `(an older b4bcoop: its default)`.
- Older client simulated (`exec open 127.0.0.1:7887?...?b4bcoopaddons=0c1g,g23a4441c-Walker_data_table_test`): host
  `PreLogin options: ...?b4bcoopaddons=-?...`, `refused by addons_policy=cosmetic (an older b4bcoop; its add-on list
  isn't logged)`, error `... you have 1 gameplay add-on(s) ...`; no id or title anywhere in the host log.
- `/addons policy match` (host without add-ons) -> refusal `[addons:match]` -> client `This host requires the same
  gameplay add-ons as theirs. Turn off: Walker data table test. Then restart the game.` `/addons policy any` -> the
  same client `join 127.0.0.1:7887` joined (`client_conns=1`), the stale match note didn't block it, host log: only
  `admin: login ... ok`. `/addons players` -> "gone".
- Client with `walker_checker` (cosmetic): asked -> `auto: joining 127.0.0.1:7887 again in 1s (add-on check passed)`
  -> login `?b4bcoopaddonsok=cosmetic`, host `checked its add-ons against addons_policy=cosmetic`, joined; the host
  log has no `33c6d9d8` and nothing of the add-on.
- `B4B_LANE=2 B4B_STEAM=flatpak tools/e2e.py --quick --no-lock`: 14/14 PASS (/tmp/b4b-e2e-l2-20260927-170032), incl. the new add-ons check (presence `addons:cosmetic`, loopback join asked -> `?b4bcoopaddonsok=cosmetic`, 0 leaks, `/addons players` gone).

### Live results before #35 (the summary design; 2026-09-25, Proton, local copies, `launch/multi.sh 2`, add-on folders per instance via
`addons_dir=`; test add-ons built with `addon.py pack` under `~/.local/share/b4b-coop/addons-mp/`, screenshots in
`addons-mp/evidence/`, not committed)
- Test content: `walker_checker.pak` = the #18 Walker Elite 00 body/arms/gear textures (checker, blue), cosmetic
  (textures), id 33c6d9d8; `walker_dt_test.pak` = an unmodified copy of `Walker_Customization_DT`, gameplay (data
  table), id 23a4441c.
- Dev build, host without add-ons, client with `walker_checker`: login `?b4bcoop=2?b4bcoopver=0.3.0?b4bcoopaddons=
  1c0g,c33c6d9d8?Name=...`, host `addons: login Hergmgurk: 1c0g,c33c6d9d8`, `/addons players` lists it. Both
  `/model walker_elite_00`: the client sees the host's hero in the checker/blue outfit, the host sees the client's
  hero in the normal outfit (Fort Hope). Mission Evansburgh B (re-login after the server travel carries the summary
  again), `endmission 1`, seamless chapter transition to C: both in C with heroes, still connected, `/addons players`
  still maps the client (same connection). No errors on either side.
- Dev build, host with `walker_checker`, client with `walker_dt_test`: refused with the `cosmetic` message (host log +
  chat line; client `NetworkFailure ... 'Host allows cosmetic add-ons only; ...'`, `auto: join ... stopped`, the game's
  "Unable to join" popup, then chat `Could not join: Host allows ...`). `/addons policy match` -> `Switch off: Walker
  data table test`; `/addons policy any` -> joined, `/addons players`: `0 cosmetic, 1 gameplay / gameplay 23a4441c
  Walker data table test`.
- **Player build** (no-click start: `instance.sh 1 -Port=7787 +b4bcoop_join proto:2 ver:0.3.0 addr:127.0.0.1:7799`,
  client `instance.sh 2 +b4bcoop_join ... addr:127.0.0.1:7787`), host without add-ons, default policy: client with
  `walker_dt_test` refused (`addons: login ... refused: Host allows cosmetic add-ons only; ...`, client `auto: join
  127.0.0.1:7787 stopped`); client restarted with `walker_checker` instead: `addons: login Hergmgurk: 1c0g,c33c6d9d8`,
  `Join succeeded`.
- Offline harness (Proton 10 wine; addons.c + addonclass.c + addons_mp.c with stubbed engine): the four policies
  against 7 summaries (none, empty, cosmetic only, gameplay, unlisted `+n`, junk), `/addons players|policy`, a pak
  whose addoninfo claims `content=cosmetic` for a data table (logged, classified gameplay).
- `tools/e2e.py --quick` checked the summary in the login and `/addons players` on the host (replaced in #35).
- `tools/e2e.py --quick` on this branch's dev build: 13/13 PASS (incl. the new add-ons check).

## 8. `~` window: the Add-ons tab (2026-10-13; #26)
Code: `looks.c` draws one table of the installed add-ons and the shop's list (overlay.md "Add-ons tab"); addons.c gives
it `addons_slots()` / `addons_info()` (file, title, version, author, description, why it can't load, file size, time
installed = newest of created / written, on / on at start / mounted / valid / gameplay, outfit and weapon-look
counts), `addons_refresh()` (once a second while the tab is shown: hand edits of `addonlist.txt` re-read as before,
file sizes and times), `addons_look_buttons(file, after)` (Wear: `models_open_screen`, the customization screen with the
outfit on; Use / Stop: a weapon look), `addons_tab_header()` (add-ons off / restart warnings) and
`addons_tab_footer()` ("Load add-ons" = ini `addons`, "Copy folder path").
- No load order and no on/off toggle in the window (Dan: two add-ons changing one file is the player's problem, not a
  mod manager). `ord[]` and `addonlist.txt` are unchanged (`/addons` lists and numbers in that order; a hand-edited
  file is followed); `addons_move` (Up/Down) is gone. An add-on that is off shows **Turn on** (`/addons on <file>`,
  loaded at once when the live rules allow it); **Remove** (shop.c, any `.pak`) is the way out.
- No details view and no conflicts list in the window: the conflict / duplicate log lines and the chat notice at
  start stay, `/addons info` has the rest. The base survivor of an added outfit is no longer shown (it was "outfit x:
  title (made on mom)" in the details).
- Policy: `addons_mp_panel()` is one combo ("Who may join with add-ons") under the tab's Host rules at the bottom.
- The join refusals now say `turn off <add-ons> (/addons off <#>, or Remove in the ~ window's Add-ons tab)`.

### Before 2026-10-13: Installed (Looks & add-ons tab; the Add-ons tab until 2026-10-12)
Code: `addons_installed_panel()` in `addons.c`, drawn by `looks.c` as the **Installed** mode of the **Looks & add-ons**
tab (overlay.md "Looks & add-ons"); the policy is `addons_mp_panel()` in `addons_mp.c`, under that tab's **Host rules**
header. Same code paths as the chat: the row checkbox runs `/addons on|off <file>` (`ov_run`), "Show in the log"
`/addons info`, `/addons`; the policy radios are `ov_setting("addons_policy")` = `addons_live()` (new: `addons_policy`
also applies live from a hand-edited `b4bcoop.ini`) + the ini writer.
- Load order: `A[]` stays in mounted order after the scan (conflicts `C[]` hold indices into it, outfits `OF[]`
  pointers); `ord[]` is the order of `addonlist.txt`. Up/Down (`addons_move`) swap with the next present add-on in
  `ord[]` and rewrite the file (reverted if the write fails). `/addons` lists and numbers in `ord[]` order, so `<#>`
  matches what the list shows (the rows say "#. title" too). A `<#>` that is a file name starting with digits is no
  longer read as a number.
- Switching on (checkbox, `/addons on`) an add-on that isn't loaded loads it at once when the live rules allow it
  (shop.md §1a: `addons_load_now`, `<title>: on, ready now`), else `on, applies after a restart (<why>)`.
- Pending changes: `on != on_at_start` or a present add-on's place != `pos0` -> banner "addonlist.txt differs from what
  is loaded now (N switched, load order changed): restart the game to apply"; rows say `off after restart`,
  `moves after restart`. Hand edits of `addonlist.txt` while the game runs are re-read (mtime, checked once a second
  while Installed is shown: `addons: addonlist.txt changed outside the game, re-read`), so the next write keeps them.
- Row: kind from the add-on's own lines and class: `gameplay`, `outfit` / `N outfits` (`outfit=`), `weapon look (AR02)`
  (`weapon=`), else `replacement`; Wear / Use while it is loaded (`addons_look_buttons`, also on Get more's rows: one
  look runs `/model <name>`, several show "Wear (n)", which opens Wear searched for the add-on's title); Update / Undo
  and the shop's last message (shop.c `shop_installed_actions`, `shop_item_note`). The tab's search box filters rows
  (title, file, author, category, description, version, outfit and weapon look names).
- Details (click the title; drawn under the row: the table ends and a second one with the same id continues, so the
  columns stay aligned): author/category/description, file, state, load order, `not loaded: <why>`, content class and
  first gameplay file, author's `content=` label if it disagrees, id (Copy), the outfits / weapon looks it adds with
  Wear / Use, its conflicts both ways, `/addons info`, **Remove** (two clicks; `shop_remove`: any add-on with a plain
  `.pak` file name, written to `.shop\pending.txt` as `remove <file>`; the next start's `shop_early` deletes it before
  anything is read). "Folder and troubleshooting" header: folder path with Copy (no "open folder": the game is full
  screen), "Load add-ons" = ini `addons` (restart), `/addons`. With `addons=0` the checkbox is at the top instead.
- Client: the policy radios are greyed out (`ov_begin_perm(CMD_HOST)`: "Host only: you are in someone else's
  session."). (Before #35 a players' section showed each login's summary; removed.)

Live (2026-09-25, lane 2, dev build, `multi.sh 2`; host: game folder `b4bcoop-addons` with broken.pak (text file),
casual_joe, holly_green, holly_magenta, walker_dt_test; client `addons_dir=` with walker_checker; screenshots in
`~/.local/share/b4b-coop/addons-tab/evidence/`, not committed):
- Host tab: 5 rows, `broken` "not loaded: not a pak file", walker_dt_test "gameplay", conflict "Holly magenta portrait"
  overrides "Holly green portrait" (2 files), Up disabled on #1, Down on #5.
- `overlay set ##on:holly_magenta.pak 0` -> `> /addons off holly_magenta.pak` / `off, applies after restart`;
  `overlay press Down##holly_green.pak` -> `addonlist.txt` = broken, casual_joe, holly_magenta=0, holly_green,
  walker_dt_test; banner "(1 switched, load order changed)"; `/addons` in chat lists the same order + "load order
  changed ... applies after restart".
- `sed` on `addonlist.txt` (casual_joe=0) while running -> re-read, `/addons` shows `off after restart`; the tab's
  checkbox back on rewrote it with the hand edit order intact.
- Policy `none##pol` -> `b4bcoop.ini: addons_policy=none`, `/addons policy` = none. Client: same radio disabled
  (the press stayed pending: disabled controls can't be driven), its ini unchanged.
- Restart: `3 to mount, 0 conflict(s)`, casual_joe 1001, holly_green 1003, walker_dt_test 1004, magenta not mounted;
  HUD portrait green. Details of Casual Joe: `58 file(s)`, cosmetic (textures, materials, meshes), outfit
  `casual_joe: Casual Joe (mom)`. Host players table: `#1 Hergmgurk: 1 cosmetic, 0 gameplay / cosmetic 33c6d9d8 (not
  installed here)`.
- Lane 2's `lane-restore.sh` now backs up the `b4bcoop-addons` folder state and restores it (removed when the player
  had none).
- `B4B_LANE=2 tools/e2e.py --quick --no-lock`: 14/14 PASS (/tmp/b4b-e2e-l2-20260925-170946). A first run failed 11/14:
  the client's sign-in rejected its profile (`PlayerProfileSettings version invalid - HydraPublicId mismatch -
  Local:offline.<id> Saved:p64...`), the game wrote a blank profile, so no burn card and the profile diff failed.
  Not add-on code (sign-in, before any command; same `.sav` publicId loads fine on the host and in earlier runs).
  Restored lane2/test2's profile from lane2/test1 (reset one kept in `~/.local/share/b4b-coop/addons-tab/`); rerun
  passed. Cause unknown; possibly the SIGKILL of the earlier instances.
