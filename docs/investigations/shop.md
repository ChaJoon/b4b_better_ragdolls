# Add-on shop: the `~` window's Add-ons tab (#36; the Browse tab until 2026-10-12, then Get more until 2026-10-13)

Status 2026-09-27, build 14216215, Proton (lane 2, Flatpak Steam). Code: `native/src/shop.c` (the list, downloads,
pending changes, Add/Update/Remove/Undo), `native/src/looks.c` (the tab's table), `native/src/updcore.c` (catalog JSON +
signature, streaming SHA-256; unit tests `native/test/run.sh`), `native/src/addons.c` (`addons_add_runtime`),
`native/src/paks.c` (runtime mount), `native/src/imgdecode.c` (thumbnails), `native/src/overlay.cpp`
(`ov_texture`/`ov_image`), `tools/shop-catalog.py` (build + sign the catalog), `tools/shop-test.py` (live test).
Player page: docs/COMMANDS.md "Add-ons from the shop". Below, "the Browse tab" / "Get more" in dated results = the
shop's rows of today's Add-ons tab.

## TL;DR
- A "Workshop"-like list (the Add-ons tab, one table with the installed add-ons; overlay.md "Add-ons tab"):
  `catalog.json` + its ed25519 signature (the shop key, not the release key: §2) are fetched by themselves once per
  game start, when the player first stands in a Fort Hope (theirs or a host's) for 3 s (`auto_fetch`), and on
  **Refresh** (until 2026-10-13 only on a click); shown with thumbnail, name and size (author, license, description as
  a tooltip), search, filter (All / Installed / Available) and sort. **Add** downloads the pak, checks size + SHA-256 against the signed list,
  puts it into `b4bcoop-addons\<id>.pak`, appends it to `addonlist.txt` and mounts it **right away** when that is
  safe; the row then offers **Wear** (the customization screen with the outfit on) / **Use**. **Remove** (any add-on)
  switches it off and deletes it at the next start. **Update** (newer file in the list) is swapped in at the next
  start, and loaded at once when nothing of the old version is in memory. `shop=0`: no list, no downloads (the tab
  shows the installed add-ons).
- **Live mounting works** (§1, §1a) for cosmetic add-ons: added outfits and weapon looks are usable right after Add
  (the row's Wear / Use, `/model`, the customization screen), in Fort Hope, in a running mission and as a client in someone's
  session (within the host's `addons_policy`); replacements too when the game has nothing of what they replace in
  memory; updates when nothing of the old version is loaded. The agent decides from the files, not from the catalog.
  Mounted paks are never unmounted.
- Thumbnails are reasonable (§3): stb_image (PNG + JPEG only), bytes decoded only after their SHA-256 matched the
  signed list, scaled to 96 px, uploaded through ImGui 1.92's own texture list (no D3D12 code of ours).
- Privacy: nothing about installed add-ons is sent to anyone. GitHub sees the catalog fetch (once per game start now,
  also when the player never opens the tab), the thumbnails (when the tab is opened) and each download, like any
  download from GitHub; other players see nothing (the #35 rules are unchanged). e2e's socket check lists HTTPS to
  GitHub's raw.githubusercontent.com addresses (185.199.108.0/22, 2606:50c0::/32) apart, as expected traffic.

## 1. Spike: mounting at runtime
`FPakPlatformFile::Mount` (0x143913E60) on the game thread, the same call the Initialize hook uses at start, with our
three signature exemptions. Results on the dev build (`tools/shop-test.py`, §5):

| Case | Result |
|---|---|
| New-path add-on (outfit, `Gobi/Content/b4bcoop/outfits/casual_joe/...`) in Fort Hope | `addons: casual_joe.pak -> mounted (read order 1000, at runtime)`; `/model casual_joe` right after: `models: hero slot N wears outfit casual_joe`, the outfit on the hero in 3P (meshes load through the new pak) |
| New-path weapon look (`ak47`) during a mission (match running) | mounted at runtime, `/model ak47` → `your AR02 now looks like AK-47`; game alive and playing 20 s later |
| Replacement (`holly_green`: `UI/Textures/.../Image_Holly_PartyPortrait`) | (2026-09-27 rule, superseded by §1a) not mounted live: `applies after a restart (it replaces files of the game or of another add-on)`; mounted at the next start |
| Removal | never live: switched off + deleted by the next start, before anything is mounted |

Why replacements wait for a restart (not "after a map change"): a package already loaded keeps its objects; a later
load of the same path would come from the new pak, but its bulk data does not: textures stream mips from `.ubulk`
long after the `.uasset` was read, by offsets from the package that was loaded. Mounting a replacement under a loaded
texture mixes the old package's offsets with the new file (the "mixed package" crash addons.md warns about, created
live). A map change doesn't reliably unload such packages (UI textures, hero assets are kept). So the rule is simple
and safe: only a pak whose **every file path is new** (`FileExists` on the pak layer, vtable slot 14, is false for
all of them) and that is **cosmetic** is mounted live; nothing loaded can refer to a path that didn't exist.
`addons_add_runtime()` checks that; the catalog's `adds`/`replaces` are only shown to players. As a client in
someone else's session nothing is mounted live either (the host's `addons_policy` was checked against the add-ons
mounted at join, #35): it applies at the next start.

Visual check of a live-mounted replacement after a map change (dev `mountpak holly_green.pak` in a mission, then
`/restart`): not observed, the random bot team had no Holly (screenshots in the scratch folder only). The rule above
doesn't depend on it.

Hooks: player builds used to install no pak hook at all without add-ons. With the shop on (default) they now
install only the `Initialize` hook (it just remembers the `FPakPlatformFile`); the two signature exemptions
(GetPakSignatureFile, the precacher's chunk check) go in with the first runtime mount (MinHook at runtime, game
thread). Dev builds do the same (`mountpak` installs them on first use), so the dev test exercises the player path.
`shop=0` or `addons=0` restores the old behavior (no hook without add-ons).

Outfits and weapon looks of a runtime add-on: `models_outfits_refresh()` / `wlooks_refresh()` append the new
entries (existing ones, with their loaded meshes, stay as they are).

## 1a. Without a restart in more cases (2026-10-11, "use their models right after they download")
Before, `addons_add_runtime` mounted live only a cosmetic pak with only new paths, and never as a client; Updates
always waited. Dan's own host logs showed every one of ~110 shop outfits "Added and ready now" (all 127 shop add-ons
are cosmetic and add-only, no two share a path: `addon.py check` over his folder), so the restart cases a player
actually met were: being a **client** in someone's Fort Hope or mission, **Updates** (republished paks, e.g. the fixed
L4D survivors), and the rare **replacement**. Now (`addons.c` "loading an add-on now"):

| Case | Now | Why it is safe / why not |
|---|---|---|
| Client in someone's session | live, within the host's policy as this join learned it (`addons_session_allows`: its rich presence or login refusal; nothing known = the host didn't ask, i.e. `any`, or an older host over IP that doesn't check). Cosmetic passes `any`, `cosmetic`, `match`; `none` keeps it for the next start ("this host allows no add-ons") | the same rule the host would apply at our next join (the mission-start reconnect reuses the login URL with its `?b4bcoopaddonsok=` claim, which stays true). Nothing to send: outfits replicate as the made-up/real row `b4bcoop.outfit.<name>`, which the host relays whether it has the add-on or not (new-assets.md §8): players with the add-on see it, the others the survivor |
| Replacement of the game's or another add-on's files | live when every replaced file belongs to a package (`.uasset/.uexp/.ubulk/.uptnl/.umap`), the add-on carries every file of that package that exists now (`paks_file_exists` per extension), none of those packages is in memory (`paks_package_loaded`: GUObjectArray scan for `UPackage` objects with no outer, also unreachable ones not yet purged) and the engine's loader is idle; else next start, with the reason and an example (`it replaces an asset the game is using now: /game/ui/.../image_holly_partyportrait`, `it replaces only part of an asset`, `it replaces a file the game reads only when it starts`) | a package not in memory has no objects, no linker and no bulk-data reader holding offsets into the old file, so its next load reads the whole package from the new pak (highest read order), exactly as after a restart. The loader check closes the race of a load starting between the check and the mount: `IsAsyncLoading` is the engine's global function pointer (`bool (*IsAsyncLoading)()`), found through the first instruction of `FAsyncLoadingThread::FlushLoading` (0x142529160: `call [rip+disp]` → 0x146402338; the static value is a `return false` stub 0x140BA60E0, treated as unknown); live it read 0 in an idle Fort Hope and 1 during a mission load (2 of 40 samples). New loads are queued from the game thread, where the check and the mount run back to back. Busy → `addons_*` return 3 and the shop asks again every frame for 15 s |
| Another add-on's files | a new add-on (last in the load order) may override add-ons below it, under the rules above; never one above it (an add-on switched on in the middle of the list whose files a later add-on also has: that one would keep parts of the asset) | owner table `own_*` (hash of each mounted path → add-on, highest read order) |
| Update | `.shop\<id>.pak.new` is mounted **above every add-on** (it must not touch any other add-on's files) when it passes the replacement rules against its own older version (so: nothing of the old version in memory, e.g. the outfit not worn by anyone, no customization screen showing it); the file swap stays for the next start (`pending.txt`); the add-on's outfit=/weapon= lines, title, version, id come from the new file; `models_outfits_refresh`/`wlooks_refresh` take changed paths/titles (rows on the customization screen repointed). A second update in the same session waits for a restart. In use → "Updated file downloaded: it replaces the old one when you restart the game (not now: ...)" | same as a replacement; the `.new` path is one of our mod paks for the signature exemptions |
| `/addons on` / Add-ons tab checkbox (an add-on that was off at start) | live under the same rules (its file list is read again: freed after the start's checks) | at its own read order (its place in `addonlist.txt`), as after a restart |
| Gameplay add-ons | next start | a host's `match` announces the host's gameplay ids; a joiner was checked at join |
| Files read only at start (shader libraries, `.locres`, `.bnk/.wem`, `.ini`, anything not a package) | next start | read once at start / at mount time of their own pak |
| Remove, switching off, load order | next start | mounted paks are never unmounted |

Visibility right after a live mount: Wear (`~` window) and `/model list outfits` read the refreshed list; the
customization-screen rows are added at the next frame (`rows_acc = 0`, before: within 5 s); the screen asks the table
each time it opens (`GetCustomizations`), so a screen opened before the Add lists the new outfit when opened again
(live: "9 b4bcoop.outfit.casual_joe "CASUAL JOE" locked=0", Holly's count 6/10). Outfit names in two add-ons now
resolve by read order (not by the order they were read), so a live-mounted add-on wins like after a restart.

## 2. Catalog format and trust chain
One JSON file, `catalog.json`, with a detached signature `catalog.json.sig` (64 raw bytes, ed25519 over the exact
bytes, `tools/sign-release.sh sign`), fetched from
`https://raw.githubusercontent.com/actuallydan/back4blood-shop/main/catalog.json` (+ `.sig`; raw.githubusercontent.com
is inside netguard's updater scope; no API call, so no 60/hour limit).
```json
{"b4bcoop-shop": 1, "updated": "2026-09-27", "addons": [
 {"id": "casual_joe", "name": "Casual Joe", "author": "...", "license": "CC0-1.0", "license_url": "https://...",
  "version": "1.0", "class": "cosmetic", "kinds": "textures, materials, meshes",
  "adds": ["outfit casual_joe (mom)"], "replaces": [], "description": "...",
  "size": 83436658, "sha256": "<64 hex>", "content_id": "<pak index SHA1>",
  "url": "https://github.com/actuallydan/back4blood-shop/releases/download/casual_joe-v1.0/casual_joe.pak",
  "thumb": "https://raw.githubusercontent.com/actuallydan/back4blood-shop/main/thumbs/casual_joe.png",
  "thumb_sha256": "<64 hex>", "min_b4bcoop": "0.8.0"}]}
```
- Trust: shop key → catalog signature → each pak's and thumbnail's SHA-256. The download host doesn't matter for
  integrity (a pak that doesn't match is deleted: "Download damaged ... Nothing was changed.").
- Parser rules (`upd_catalog_parse`): format > 1 → "needs a newer b4bcoop"; entries missing id/name/license/url/size/
  sha256, bad id (`[a-z0-9][a-z0-9_-]{0,31}`, it becomes the file name `<id>.pak`), non-https URL (http only to
  127.0.0.1, and the downloader refuses plain HTTP in player builds), duplicates → skipped and counted; a thumbnail
  without its SHA-256 is dropped; unknown keys ignored. Limits: catalog 4 MB, 256 entries, pak 1 GB, thumbnail 1 MB.
- `min_b4bcoop`: Add is greyed out with "needs b4bcoop X or newer (Updates tab)".
- Stale-catalog replay (an old signed list served again) is possible in principle but only through GitHub's TLS
  endpoints; the tab shows `updated`.

### Shop repo: actuallydan/back4blood-shop (public, 2026-09-27)
- Layout: `entries.json` (the `tools/shop-catalog.py build` manifest: `{"addons": [{id, version, license,
  license_url, author, thumb, min_b4bcoop, ...}]}`), `thumbs/`, `shop-signing.pub.pem`, `scripts/fetch-paks.py`,
  `catalog.json` + `catalog.json.sig` (generated by the workflow, committed on `main`: the agent's URL).
- An add-on = one GitHub release `<id>-v<version>` of that repo with `<id>.pak` as its asset (the modkit's `b4bmod
  pack` output).
- Workflow `.github/workflows/catalog.yml` on push to `entries.json`/`thumbs/`, on release published/edited/deleted
  and by hand: `fetch-paks.py` downloads each entry's release asset, `tools/shop-catalog.py build entries.json` from
  b4b-coop checked out at a pinned commit (computes size/SHA-256/content id/class/adds/replaces from each pak; any
  refused entry fails the run), signs with the secret `SHOP_SIGNING_KEY`, verifies against `shop-signing.pub.pem`,
  commits `catalog.json` + `.sig` only when the add-on list changed (not for the date alone; by hand always).
- Review: add-ons arrive as PRs to `entries.json` (license check by a human; the tool only checks the string). The
  README states the rules (every file must be shareable under the stated license; modkit add-ons contain copies of
  game material instances/skeleton data, the submitter's responsibility; no ripped content).
- First run: an empty signed list (`"addons": []`); the dev build with no override fetches it and shows 0 add-ons.

### Shop signing key (2026-09-27)
- Its own ed25519 key, separate from the release key (Dan's decision): whoever can change the shop repo's workflows
  or read its secret can sign catalogs, never b4bcoop releases. Each built-in key verifies one thing
  (`native/src/signkeys.h`: `B4B_RELEASE_PUBKEY_BYTES` for updater.c, `B4B_SHOP_PUBKEY_BYTES` for shop.c; the Browse
  tab has no path to the release key). Generated with `openssl genpkey -algorithm ed25519`, like the release key.
  - Private: `~/.local/share/b4b-coop/keys/shop-signing.key` on Dan's machine (chmod 600, dir 700, never committed)
    and the secret `SHOP_SIGNING_KEY` of `actuallydan/back4blood-shop`. Keep one more offline copy.
  - Public: `docs/shop-signing.pub.pem` (also in the shop repo as `shop-signing.pub.pem`), raw `42195847...198f74`.
  - Checks: `native/test/run.sh` compares both built-in keys with the committed PEMs, checks they differ and that a
    catalog signed with one key fails with the other; `ci.yml` checks the player DLL carries both keys; the shop
    workflow verifies its own signature against the committed public key before it commits.
- Humans: `tools/shop-catalog.py verify catalog.json` (default `--pub docs/shop-signing.pub.pem`); signing by hand:
  `tools/shop-catalog.py sign catalog.json` (default `--key ~/.local/share/b4b-coop/keys/shop-signing.key`).
- Dev builds: `shop_pubkey=<64 hex>` still replaces the shop key (tools/shop-test.py's throwaway key).
- **Rotation** (key still safe): release N carries the new public key in `signkeys.h` + `docs/shop-signing.pub.pem`;
  once most players run N, update the secret and the shop repo's `shop-signing.pub.pem`, then re-run its workflow.
  Players on older builds see "the add-on list's signature is not valid" until they update (Updates tab), nothing
  worse. (Accepting two keys for a transition is a small change in `upd_catalog_verify`'s caller if ever wanted.)
- **Lost or leaked**: new key, public half into `signkeys.h` + both PEMs, update the secret, release b4bcoop, re-run
  the workflow. Older agents refuse the new list (the point: a leaked key's lists stop working once players update).
  A leaked key alone lets an attacker sign a list, but players only fetch it from this repo's raw URL over TLS, and
  every pak is still checked against the signed SHA-256 (the attacker would need both the key and that URL).

## 3. Thumbnails
- Decoder: stb_image v2.30 (public domain), pinned in `tools/fetch-deps.sh` (commit 2c980bb, sha256 of
  `stb_image.h`), compiled with `STBI_ONLY_PNG`, `STBI_ONLY_JPEG`, no stdio/HDR, `STBI_MAX_DIMENSIONS 2048`. Its
  input is only bytes whose SHA-256 the signed list names, so a hostile picture needs the signing key.
- Box-filtered to fit 96×96 (36 KB RGBA each), at most 128 per session, cached by SHA-256 across list refreshes.
- Fetched (2026-10-13) by their own worker when the tab first shows a list (`want_thumbs`, once per list: the list
  itself now arrives without the player asking, the pictures only when someone looks).
- Upload: ImGui 1.92's texture system: `ov_texture()` creates an `ImTextureData` (RGBA32), `RegisterUserTexture`;
  the DX12 backend creates and uploads it inside the next `RenderDrawData` on the render thread (the overlay's
  snapshot already carries `Textures`). Textures are never destroyed during the session (a frame in flight could
  still use one); the SRV heap went from 64 to 256 slots, `OV_MAX_TEX` = 160. Worst case ~6 MB of GPU memory.
- Live: PNG and JPEG thumbnails `uploaded` in the tab (screenshot §5).

## 4. Files, next start, updates
- `b4bcoop-addons\<id>.pak` (the add-on), `b4bcoop-addons\.shop\` (hidden): `<id>.pak.part` while downloading
  (deleted on any failure), `<id>.pak.new` (a downloaded update), `pending.txt` (`remove <file>` / `update <file>`).
- `shop_early()` runs from `addons_scan()` in DllMain before `addonlist.txt` is read: deletes removed paks (their
  addonlist line is dropped), renames `.new` over the old pak, deletes `pending.txt`. Only `<shop id>.pak` names are
  accepted. Undo before the restart rewrites `pending.txt` (and switches the add-on back on).
- Installed state: the file `<id>.pak` in the folder; its SHA-256 (computed after the list arrives) against the list
  tells "installed" from "update available". The row shows the add-on's state as Installed does (`on`, `on after restart`, ...).
- The same add-on under another file name (2026-10-05; players had `batman.pak` before the shop listed `batman-any`):
  no `<id>.pak`, but another present add-on has the entry's `content_id` (= the agent's pak index SHA1, the same value
  `tools/shop-catalog.py` writes) or adds an outfit/weapon look the entry's `adds` names (`outfit batman (any)`) →
  the row says **Installed (as batman.pak)** ("the same add-on" / "batman.pak adds the same outfit batman"), the
  last column that add-on's state, no Add (since 2026-10-13: same content = that file's row of the table, with the
  shop's picture and name; a same-named look = the shop's row says "as batman.pak"); `shop add`
  refuses with "already installed as batman.pak"; dev `shop status` row `[installed-as] ... msg=installed as
  batman.pak: outfit batman` (`addons_installed_like()`, re-checked after each Add). If both are present anyway, the
  Add-ons tab reports them as a duplicate (addons.md "Duplicates").

## 5. Tests
- Unit (`native/test/run.sh`): 84 checks (+17; 88 with the shop key checks): streaming SHA-256 against one-shot, catalog parse (skipped entries,
  lists, thumbnails without hash, truncated JSON, no format, newer format), ids, Monocypher- and OpenSSL-signed
  catalogs, an edited byte refused.
- Live (`B4B_LANE=2 B4B_STEAM=flatpak B4B_GPU=4090 tools/shop-test.py`, dev build, one instance, local catalog on
  127.0.0.1 signed with a throwaway key, own `addons_dir=`): **32/32** (before and after merging #35;
  `/tmp/b4b-shop-20260927-171732`, screenshots kept in `~/.local/share/b4b-coop/shop-test/evidence/`): no add-ons
  folder at start; bad catalog signature → "Not shown: the add-on list's signature is not valid"; 4 add-ons listed,
  PNG + JPEG thumbnails uploaded; `needs b4bcoop 9.0.0 or newer`; damaged pak → "Download damaged (its SHA-256 does
  not match the signed list). Nothing was changed.", truncated → "Download incomplete (4183199 of 12549599 bytes)",
  nothing left behind; Add casual_joe (the tab's button) in Fort Hope → mounted at runtime, worn with `/model`;
  holly_green → "applies after a restart (it replaces files of the game or of another add-on)", not mounted; in a
  running mission Add ak47 → mounted, `your AR02 now looks like AK-47`, playing 20 s later; Remove ak47 (two clicks)
  → switched off; restart → `shop: removed ak47.pak`, gone from addonlist.txt, holly_green + casual_joe mounted at
  start; a newer holly_green in the list → Update → `.shop\holly_green.pak.new` → restart → `shop: updated
  holly_green.pak`, the new file in place.
- `tools/e2e.py --quick` (lane 2, Flatpak Steam) with this build after merging main: 14/14.
- Shop key (2026-09-27, lane 2): unit tests 88; `shop-test.py` 32/32 with the throwaway key override (bad
  signature → "... not valid (not signed with the add-on shop's key)"); `shop-test.py --real` 4/4: no override,
  `list=https://raw.githubusercontent.com/actuallydan/back4blood-shop/main/catalog.json key=shop`, "0 add-on(s) in
  the list (updated 2026-09-27)." (screenshot `evidence/real-empty-catalog.png`); `e2e.py --quick` 14/14.
- Test paks (never committed): `~/.local/share/b4b-coop/shop-test/src/` (casual_joe = CC0 test outfit, ak47 = the CC0
  loafbrr AK, holly_green/magenta = the #17 portrait tests).

- #38 (2026-10-04, lane 2): "the Browse tab doesn't reach the shop" with no add-ons and no `b4bcoop-addons` folder.
  Not reproduced: the **player build** (driven through the headless gamescope's Xwayland with XTest input, frames via
  `gamescopectl screenshot`; the overlay's cursor follows relative raw input, so moves are relative from a 0,0 clamp)
  and the dev build both fetch and verify the real list with the folder absent (`netguard: allow tcp
  185.199.108.133:443 via WINHTTP.dll (updater)`). The list itself is empty: the shop repo has 125 `<id>-v1`
  releases, but its catalog workflow only lists what `entries.json` names, and that is `[]`. The tab said only
  "0 add-on(s) in the list", which reads like a failure; it now says "Reached the shop, but its list is empty ...: no
  add-ons are published there yet", and a failed fetch points to the log's `shop:`/`netguard:` lines.

- Real shop, 127 add-ons (2026-10-05, lane 2, Flatpak Proton, own empty `addons_dir=` + a copy of Dan's `batman.pak`):
  - **Player build**, driven with real input (XTest into the headless gamescope's Xwayland `:2`; frames from
    `gamescopectl screenshot`, `XDG_RUNTIME_DIR=<rt>/b4b-lane2 WAYLAND_DISPLAY=gamescope-0`; the overlay cursor
    follows relative motion 1:1 from a 0,0 clamp): Sign in → Play Offline → `~` → Browse → Get the add-on list: "127
    add-on(s) in the list", thumbnails; search box typed: `batman` → Batman (any cleaner) "Installed (as batman.pak)
    / the same add-on" (Dan's batman.pak has the shop pak's content id), `coach` → Add → `shop: coach verified (size,
    SHA-256 ...)`, `coach.pak -> mounted (read order 1001, at runtime)`, chat `/model coach` → `hero slot 0 wears
    outfit coach`; Remove twice → "removed after restart"; restart → `shop: removed coach.pak`, gone from
    addonlist.txt.
  - Dev build (`shop status`): 127 items, 127 rows `thumb=uploaded` (126 textures: two entries share a picture),
    search `coach` shows Add##coach and hides Add##bill; `batman-any [installed-as] ... same content`; a different
    `coach.pak` build saved as `old_coach.pak` → coach `installed as old_coach.pak: outfit coach`; with the shop's
    coach.pak next to it → `addons: duplicate: old_coach.pak and coach.pak both add outfit coach: coach.pak's is used`
    + the notice; batman.pak + a copy → "installed twice". `tools/shop-test.py` 37/37 (+5: step 8).

- Without a restart in more cases (§1a, 2026-10-11, lane 2, Flatpak Proton, `B4B_GPU=4090`, dev build): `tools/shop-test.py`
  **56/56** (`/tmp/b4b-shop-20261011-190458`, screenshots in `~/.local/share/b4b-coop/shop-test/evidence/2026-10-11-addons-now/`):
  - customization screen opened and closed, then Add casual_joe: `12 customization-screen row(s) added` at once,
    `/model list outfits` has it, the screen opened again lists `b4bcoop.outfit.casual_joe "CASUAL JOE" locked=0`
    (Holly 6/10, `3-customization-screen.png`);
  - Add holly_green in Fort Hope (`paks loaded <portrait>`: in memory): `Added: applies after a restart (it replaces an
    asset the game is using now: /game/ui/textures/common/characters/heroes/image_holly_partyportrait).`;
  - Updates after a restart: casual_joe v1.1 (a repack with another addoninfo, same 58 files) `casual_joe.pak replaces
    58 existing file(s) (23 asset(s)), none of them loaded now`, `update of casual_joe.pak (...\.shop\casual_joe.pak.new)
    -> mounted (read order 1002, at runtime ...)`, `/addons info` = `Casual Joe v1.1 1.1`, `/model casual_joe` worn; a
    second Update refused until a restart; holly_green's Update `not now: it replaces an asset the game is using now`;
    restart → both files swapped;
  - `/addons on ak_off` (off at start): `AK-47 (loafbrr, CC0): on, ready now`, the look in `/model list weapons`;
  - `multi.sh 3`, host `addons_policy=cosmetic` with casual_joe, client 2 (asked to check at join, passed) Adds it in
    the host's Fort Hope: `Added and ready now.`, `/model casual_joe` worn on client 2, the host's log `hero slot N
    wears outfit casual_joe` and its `face` mesh `/Game/b4bcoop/outfits/casual_joe/3P_Mom_Elite_04_SKM` on client 2's
    hero (`9-host-sees-client-outfit.png`), client 3 without the add-on: row `b4bcoop.outfit.casual_joe` in `mdl dump`,
    only retail meshes on its heroes;
  - `multi.sh 2`, host `addons_policy=none`: client 2's Add → `Added: applies after a restart (this host allows no
    add-ons).`, not mounted, `casual_joe.pak=1` in its addonlist.txt.
  - Loader check: `paks` read `async loading now: 0` in an idle Fort Hope and 1 in 2 of 40 one-second samples during a
    mission load.

- One tab (2026-10-12, overlay.md "Looks & add-ons"): Browse became Get more in the Looks & add-ons tab; Remove moved to
  Installed and works for any add-on file (`pending.txt` keyed by file name, `shop_early` accepts plain `.pak` names
  for `remove`, shop ids only for `update`); Update also in Installed ("Check for updates" gets the list). Lane 2,
  Flatpak Proton, `B4B_GPU=4090`: `tools/shop-test.py` **58/58** (/tmp/b4b-shop-20261012-021647; Get more's Wear right
  after Add, Remove/Update through Installed, Remove + Undo of a hand-installed pak, client 2 through its own window).
- One table (2026-10-13, overlay.md "Add-ons tab"): the list fetched by itself on the first arrival in Fort Hope
  (`shop: in Fort Hope for the first time this game: getting the add-on list`), thumbnails when the tab shows the list,
  Refresh; the shop's rows and the installed add-ons in one table. `tools/shop-test.py` adapted: the automatic fetch
  served with a bad signature (refused), Refresh for the good list, row order checked with `overlay locate`
  (installed first, A-Z / Z-A, time installed both ways, filter, search), Wear = the customization screen + Esc twice
  (`key 0x1B`; worn, view back on the hero), Use / Stop of the weapon look in a mission, Remove / Update / Undo /
  Turn on from the rows, client 2 gets the list by itself, adds and wears; the test add-on's picks in each prefix's
  `b4bcoop-outfits.txt` are dropped before every start (a remembered outfit is worn at the start, so its Update would
  wait for a restart). Lane 2 with the native Steam client, `B4B_GPU=4090`: **68/68** (/tmp/b4b-shop-20261013-024727).

## 6. Open
- Still a restart (§1a): gameplay add-ons; replacements/updates of assets in memory (e.g. an outfit someone wears, a
  UI texture on screen); files read only at start; any add-on as a client of a host with `addons_policy=none`;
  Remove, off, load order; a second Update of one add-on in a session; add-ons dropped into the folder by hand (the
  folder is read at start). Not tried live: a replacement mounted live in a running mission (the same check), the
  loader-busy retry path (needs a load at the moment of the Add).
- Native Windows: WinHTTP to raw.githubusercontent.com, renames in the add-ons folder while a pak is mounted (the
  engine keeps pak handles open: Remove/Update never touch a mounted file before the next start).
- The shop repo is empty: Dan decides what gets published.
