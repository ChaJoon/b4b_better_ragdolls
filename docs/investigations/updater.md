# In-game updater (#34)

The `~` window's **Updates** tab checks GitHub for a release, downloads it, verifies it and swaps the files in; the
new version runs from the next game start. In the game nothing happens without a click (no background check),
nothing is hot-reloaded. Before the game starts, the launcher does the same by itself on co-op starts (§7, ini
`auto_update`). Code: `native/src/updater.c` (tab, start check), `native/src/updinst.c` (Windows: HTTP, check,
download, files, state, revert; shared with the launcher), `native/src/updcore.c` (portable: SHA-256, signature,
manifest, zip, JSON, versions, the launcher's decisions), `native/launcher/redirect.c` (§7), netguard scope in
`native/src/netguard.c`, signing in `tools/sign-release.sh` + `.github/workflows/release.yml`. Player docs:
`docs/COMMANDS.md` "Updates", README "Update".

## 1. Spike: HTTPS from the agent
- **WinHTTP** (loaded with `LoadLibraryW` on the first request, so no player loads it who never clicks; netguard's
  DLL notification hooks `WinHttpConnect` as it maps). Synchronous calls on a worker thread;
  `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY` (no WPAD lookups); timeouts 15/15/20/30 s.
- Works under Wine and Proton, TLS included, following GitHub's redirect by default (WinHTTP's default policy follows
  redirects except HTTPS → HTTP):
  - System Wine (spike exe `winhttp` GET, 2026-09-27): `api.github.com/.../releases/latest` 200 8053 B 200 ms;
    `github.com/.../releases/download/v0.6.1/SHA256SUMS` 200 373 B, final URL `release-assets.githubusercontent.com/...`;
    the 0.6.1 zip 200 685901 B.
  - Proton in the Flatpak Steam sandbox (lane 2, in the game, dev build): the real check (`update check`) and a real
    asset download through the redirect (`update fetch <github.com download URL>`: 373 B, SHA-256 equal to a
    Linux-side download). Log:
    ```
    netguard: updater scope open (GitHub release hosts allowed)
    netguard: allow http api.github.com via X3DAudio1_7.dll (updater)
    netguard: allow dns api.github.com via WINHTTP.dll (updater)
    netguard: allow tcp 140.82.114.6:443 via WINHTTP.dll (updater)
    update: Release v0.6.1 has no in-game update files (it is older than the updater). Get it from GitHub: ...
    netguard: updater scope closed
    ```
  - Native Windows: not run yet (see §6).
- **GitHub**: `GET https://api.github.com/repos/actuallydan/b4b-coop/releases/latest` (or `/releases/tags/v<ver>` for
  a host's version), headers `User-Agent: b4bcoop-updater/<version>` (GitHub requires one), `Accept:
  application/vnd.github+json`, `X-GitHub-Api-Version: 2022-11-28`. Assets by `browser_download_url`
  (`github.com/<repo>/releases/download/<tag>/<name>` → 302 → `release-assets.githubusercontent.com`, earlier
  `objects.githubusercontent.com`). Unauthenticated limit: 60 API requests/hour per IP (`x-ratelimit-remaining`,
  `x-ratelimit-reset`); a check is 1 API request (asset downloads don't count). 403/429 with remaining 0 → "limit
  used up; try again after HH:MM".
- **netguard scope** (`netguard_updater_scope(1/0)` around each check/download): while open, `api.github.com`,
  `github.com` and `*.githubusercontent.com` pass the WinHttpConnect and DNS hooks (reason `updater`), and the IPs they
  resolve to take TCP (flagged `ok_upd[]`: valid only while the scope is open, forgotten for TCP after). Windows'
  WinHTTP may resolve asynchronously (`GetAddrInfoExW` with OVERLAPPED: nothing remembered), so while the scope is open
  a TCP connect to port 443 made by `winhttp.dll`/`webio.dll` itself also passes (`updater-winhttp`): during those
  seconds another WinHTTP user in the process would pass too (the game's services go through libcurl, blocked by
  name as before). Outside the scope the same download fails:
  `update fetch <url> noscope` → `can't reach github.com (no internet, or blocked)`. Dev builds add the `update_api=`
  host to the scope (127.0.0.1 needs nothing: loopback is always allowed).

## 2. What a release carries (release.yml, from 0.6.2 on)
| Asset | What |
|---|---|
| `b4bcoop-<v>.zip` | the player zip (unchanged) |
| `b4bcoop-update.txt` | the manifest (`launch/package.sh` → `tools/sign-release.sh manifest`): `b4bcoop-update 1`, `version=`, `protocol=`, `zip=`, `size=`, `sha256=` |
| `b4bcoop-update.txt.sig`, `b4bcoop-<v>.zip.sig`, `SHA256SUMS.sig` | ed25519 signatures, 64 raw bytes (`openssl pkeyutl -sign -rawin`) |

The check downloads the manifest + its signature (~300 B): the version and protocol shown in the tab are signed. The
download takes the zip + its `.sig`: size and SHA-256 must equal the signed manifest, and the zip's own signature must
verify, before anything is written. Releases without a manifest (0.6.1 and older) are shown with a link to the
release page.

## 3. Signing
- ed25519. CI signs with OpenSSL 3 (`tools/sign-release.sh sign`), the agent verifies with **Monocypher 4.0.2**
  (`crypto_ed25519_check`, `src/optional/monocypher-ed25519.c`: ed25519 with SHA-512, i.e. RFC 8032, compatible with
  OpenSSL; `native/test/run.sh` signs its fixtures with OpenSSL and checks them with Monocypher). Licence: CC0 or
  BSD-2 (we take CC0: no notice needed). Inflate: **zlib's contrib/puff** (v1.3.1, zlib licence, no notice needed in
  binaries). Both pinned in `tools/fetch-deps.sh` (commit + sha256 of the compiled files), like ImGui.
- Key: generated 2026-09-27 with `openssl genpkey -algorithm ed25519`.
  - Private: `~/.local/share/b4b-coop/keys/release-signing.key` on Dan's machine (chmod 600, dir 700, never committed)
    and the GitHub Actions secret `B4B_RELEASE_SIGNING_KEY` (the PEM). Keep one more offline copy (password manager).
  - Public: `docs/release-signing.pub.pem`, raw `3faca059...789a31` in `native/src/signkeys.h` (`B4B_RELEASE_PUBKEY_BYTES`,
    `RELEASE_PUBKEY` in updater.c). It verifies releases only: the add-on shop has its own key (shop.md §2).
    `ci.yml` checks the player DLL carries the committed key; `release.yml` fails without the secret and verifies its
    own signatures against the committed key (a wrong secret can't publish).
- Humans: `tools/sign-release.sh verify docs/release-signing.pub.pem b4bcoop-<v>.zip` (next to its `.sig`).
- **Key rotation** (planned, key still safe): generate a new key; release N+1 is still signed with the old key but its
  agent contains the new public key (then `B4B_RELEASE_PUBKEY_BYTES` becomes the new one, `docs/release-signing.pub.pem` too);
  update the secret before tagging N+2. Players on N update to N+1 with the old key and from then on accept the new
  key. (Accepting two keys at once is a small change in `upd_sig_ok` callers if ever needed.)
- **Key lost or leaked**: generate a new key, put its public half into the agent, update the secret, release. Players
  can't get that release through the tab (their agent rejects the new signature, which is the point); they update
  once by hand (extract the zip), from then on the tab works again. A leaked key additionally means: say so in the
  release notes; an attacker would still need to get players to download from the attacker (the updater only talks
  to GitHub's release of this repo).

## 4. Applying on restart
- The agent is `Gobi\Binaries\Win64\X3DAudio1_7.dll`, mapped by the running game. Windows refuses to overwrite a
  mapped DLL but allows renaming it (the loader opens images with FILE_SHARE_DELETE); Wine allows both. So the swap
  runs right after the verified download, by renames, and the running game simply keeps its mapped old code:
  1. installable zip entries (`upd_install_path`: `xinput1_3.dll` and `b4bcoop-*.txt` in the root,
     `Gobi/Binaries/Win64/X3DAudio1_7.dll`; never `b4bcoop.ini`, bans, add-ons, logs, anything else) are written to
     `<game>\b4bcoop-update\staged\<ver>\`;
  2. each running file moves to `b4bcoop-update\backup\<running ver>\` (older backups deleted: one is kept);
  3. each staged file moves into place; any failure undoes steps 2-3 ("Your current version is unchanged");
  4. `b4bcoop-update\state.txt`: `installed=<new> previous=<old> pending=1 starts=0 files=<list>`.
  One install or go-back per game session (the tab greys out until a restart).
- Rejected alternatives: swapping at the next start from DllMain can't replace the DLL that is running that DllMain
  (it would take two restarts); the root `xinput1_3.dll` launcher ran only on Windows when this was written (Wine
  ignores it: builtin xinput wins), so it couldn't be the mechanism for Proton/Steam Deck. Since #47 the agent sets
  the DllOverride that makes Proton load it too, and the launcher installs updates before the game starts (§7).
- **Start check / automatic revert** (`updater_early`, DllMain right after the log opens and the `-b4bcoop=off`
  check; kernel32 file calls only): if `pending=1` and `installed` is this version, `starts++`. 15 s of game ticks
  later (`updater_tick`) → `pending=0` ("started fine"). If a third start finds `starts` already at 2, the new
  version crashed/hung twice before that: it moves its files to `b4bcoop-update\failed\<ver>\` (renaming its own
  mapped DLL), puts the backup back, writes `result=<new> did not start properly 2 times, so <old> was put back` and
  starts nothing this session; the next start runs the old version, whose tab shows that line. If `installed` differs
  from the running version (someone extracted a zip over it), the pending state is dropped.
- **Go back** (tab, two clicks): the same revert by hand, `result=you went back from X to Y`.
- Not covered: a new DLL that Windows can't load at all (the game wouldn't start; no b4bcoop code runs). Builds come
  from the pinned CI toolchain, so unlikely; recovery by hand: move `b4bcoop-update\backup\<old>\...` back or extract
  a release zip. The Windows launcher could check `state.txt` before starting the game; not done (the launcher
  itself is still unverified on Windows).

## 5. Tests
- **Unit** (`native/test/run.sh`, also in CI): 67 checks. SHA-256 vectors (incl. 1M 'a', 56-byte padding edge), version
  order (pre-releases), install filter (ini/bans/add-ons/paths refused), manifest (+ CRLF, 6 malformed), download
  check: good, **bad hash**, **truncated** (n-1, 0), **bad zip signature**, other key, short signature, edited manifest;
  zip reader: deflate + stored, truncated and corrupted zips refused, `../` entry refused; GitHub JSON (escapes,
  surrogates, nested objects, truncated, error object, the real v0.6.1 reply in `native/test/fixtures/`); OpenSSL-made
  signatures verified by Monocypher.
- **Live** (`tools/update-test.py`, lane 2, Flatpak Steam, 2026-09-27): **39/39 PASS**
  (`/tmp/b4b-update-20260927-152633`). A 0.6.2-test dev build (`B4B_BUILD_VERSION`, `B4B_BUILD_OUT` in
  `native/build.sh`) packed like a release, signed with a throwaway key, served by a fake GitHub API on 127.0.0.1
  (asset links 302 like github.com); the instance uses dev ini `update_pubkey=` + `update_api=` (live). Steps: real
  GitHub check over HTTPS + real asset download through the redirect, the same download blocked outside the scope;
  bad manifest signature / bad zip hash / bad zip signature / truncated zip → refused, all files, both inis, bans,
  add-ons unchanged and no backup/staged folder; good release through the tab's button → new DLLs in place, old one in
  the backup, the zip's ini not installed, both inis and add-ons unchanged, this game still logs 0.6.1; restart →
  `b4bcoop 0.6.2-test (protocol 2)`, `start 1 of the freshly installed`, `started fine`; Go back → restart → 0.6.1;
  install again with dev `update_never_healthy=1` → starts 1 and 2 not healthy, start 3 logs `reverted 0.6.2-test ->
  0.6.1 (0.6.2-test did not start properly 2 times, so 0.6.1 was put back)` and starts nothing, failed DLL in
  `failed\0.6.2-test\`, next start 0.6.1 with the result in `update status`.
- Version-mismatch hint (lane 2, `B4B_INI_EXTRA2=b4bcoop_protocol_override=3`): the refused client logs `update: noted
  the host's version 0.6.1 (protocol 2)`; its chat line gets " Press ~, tab Updates, to get the same version." (shown
  after the next map load; not seen in that short run). The host adds a hint only when the joiner's version is newer.
- e2e `--quick` on lane 2 with this build: 14/14.

## 6. Open
- Native Windows: WinHTTP + schannel, renaming the mapped `X3DAudio1_7.dll` (expected to work: FILE_SHARE_DELETE),
  netguard's async-resolve fallback. Test: any Windows PC with the release after this one: Updates → Check.
- The first real signed release (0.6.2) is the first end-to-end run against GitHub's real assets + real key.
- The modkit zip is out of scope (#34).

## 7. Automatic update before the game starts (launcher, `auto-update` branch, 2026-10-16)
Goal: no "start, download, quit, start again" for a small update. The root `xinput1_3.dll` (launcher,
`native/launcher/redirect.c`) runs in Steam's launcher stub before the game process exists, so nothing of ours is
mapped there except the launcher itself; it now installs a newer release on **co-op starts** (never on the Online
path: no request at all when the player picks Online).

**Shared code.** The tab's HTTP, release check, download, staging + swap + undo, `state.txt` and revert moved from
`updater.c` into `native/src/updinst.c/.h` (Windows, no game code, no netguard: the agent opens netguard's scope
around each call itself). Both the agent (`src/*.c`) and the launcher (`native/build.sh` adds `updinst.c`,
`updcore.c`, Monocypher, puff to `xinput1_3.dll`, ~175 KB now) link it, so both apply the same rules: signed manifest,
zip size / SHA-256 / ed25519 with the key in `signkeys.h`, `upd_install_path` (never ini, bans, add-ons, logs),
running files → `backup\<ver>\`, any failed move undone, `state.txt` `installed= previous= pending=1 starts=0`.
WinHTTP is loaded at run time (`LoadLibraryW`), User-Agent `b4bcoop-launcher/<ver>`, default proxy settings (no
WPAD), no revocation checks (WinHTTP's default): the stub's only connections are `api.github.com`, `github.com` and
GitHub's release-asset host (or the dev `update_api=`).

**Flow** (`auto_update()` in redirect.c, right after the co-op path's unpark, before the game's `CreateProcessW`):
1. On? ini `auto_update` (player builds default on; **dev builds default off**, so a release never replaces a
   developer's build by itself, e.g. a lane install or online-test.py's stub starts; `auto_update=1` or env
   `B4B_LAUNCHER_UPDATE=on` turns it on). Needs `Gobi\Binaries\Win64\X3DAudio1_7.dll` (a legacy `dwmapi.dll` dev
   install is skipped). Skipped while `state.txt` has `pending=1` for this version: a fresh update that hasn't had its
   15 s of play keeps its backup (installing again would replace the backup of the known-good version and break the
   automatic revert).
2. Plan (`upd_join_parse`, `upd_auto_plan` in updcore.c, unit-tested): the stub's command line carries a Steam join
   (`+b4bcoop_join steam:<id> proto:<n> ver:<x.y.z> ...`) → the host's version, `releases/tags/v<ver>`, when it is newer
   than ours, or older **with another protocol** (the join would be refused); our version, or older with our protocol
   (or an unknown protocol) → no request at all. No join (or a join without `ver:`) → `releases/latest`.
3. Check: one request on a worker thread; the stub waits **2 s at most** for all of it (WinHTTP loading, DNS, TLS,
   reply; WinHTTP's own timeouts are 2 s each too, and a cancel flag stops the abandoned worker). No answer, an error
   (404, rate limit, no network), or a release that isn't wanted (`upd_auto_wanted`: latest must be newer; a tag must
   be exactly the host's version) → the game starts as before, nothing shown, one log line. A release without
   `b4bcoop-update.txt` (≤ 0.6.1) is skipped the same way.
4. Never again by itself (`upd_auto_blocked`): a version that was put back, i.e. `state.txt skip=<ver>` (new field,
   written by every revert: the automatic one after two failed starts and Go back; older agents ignore unknown keys),
   `b4bcoop-update\failed\<ver>\`, or an older agent's `result=you went back from <ver> to ...`. Without this, a
   release that crashes would loop (install, crash twice, revert, install). The tab still installs it on a click
   (which clears `skip=`).
5. Window: a task dialog (comctl32 v6 through the launcher's manifest, like the prompt, 260 dialog units wide)
   "Updating b4bcoop to X" with a progress bar, the state as one line of text (`Getting b4bcoop X from GitHub...`,
   `Downloading: n of m KB`, `Installing...`, `b4bcoop X is installed. Starting the game...` or `Couldn't update:
   <reason>. Starting b4bcoop <ours>...`), one button **Skip update** (Esc, the window's X and controller **B** too;
   greyed out while installing and when done) and the footer "Skip update plays b4bcoop <ours> now. Automatic
   updates can be turned off in the ~ window, tab Updates." The job starts in `TDN_CREATED`, so
   no window = no download; without task dialogs (very old comctl32) there is no update (never silent). The worker:
   manifest + `.sig` (verified, must name the tag's version) → zip + `.sig` (progress) → verify → install. Phases move
   by compare-and-swap; Cancel only wins during the network part (FETCH, DL), so a cancelled job never writes a file,
   and once the renames start the window answers Skip with "Installing, one moment..." until they finish (or are
   undone). Done → "installed, starting the game" (1.5 s, readable even when the download took a second) → the game starts with the new agent, whose `updater_early`
   counts start 1 of the fresh update as for a tab install. Failure → the dialog says why for 4 s, the current version
   starts. The launcher's own mapped DLL is renamed into `backup\<ours>\` like the agent's DLL in a tab install.
6. Result: the launcher log (`Gobi\Binaries\Win64\b4bcoop-launcher.log`, `update: ...` lines) and
   `b4bcoop-update\auto.txt` (`when=`, `result=`), written only when an update was found; the Updates tab shows it
   ("Last automatic update") and has the switch **Update b4bcoop automatically when the game starts**
   (`ov_setting("auto_update")`; the agent detects a launcher that can do it by the marker `auto-update-1` in its
   `b4bcoop-launcher-features` string).

Dev: `update_api=`, `update_repo=`, `update_pubkey=` in the ini apply to the launcher as to the tab (dev builds only;
plain `http://127.0.0.1` allowed there, so `tools/update-test.py`'s fake API works). Env `B4B_LAUNCHER_UPDATE=off|on|
cancel` (cancel = press Skip update once the download runs), `B4B_LAUNCHER_SHOT=<Windows path prefix>` (the prompt and
each phase of the update window saved as BMP, as drawn). The launcher log's lines start with `[<ms since load> ms]`.
Live test: `tools/autoupdate-test.py` (below). `rundll32 xinput1_3.dll,b4bcoop_update_test <game
root>\[|<launch options>]` runs `auto_update` alone, with no stub and no game (logs to the root's
`Gobi\Binaries\Win64\b4bcoop-launcher.log`); e.g. `|+b4bcoop_join steam:1 proto:3 ver:0.6.2-test` for the join plan.
CI: the player launcher must carry the release key and the marker, and no `B4B_LAUNCHER_UPDATE` / `update_pubkey`.

Linux/Steam Deck: the launcher only loads once the agent has set the `xinput1_3` DllOverride (online.c, first start),
so the automatic update runs from the second start on. A downgrade to a host's release older than this feature
leaves the player on a launcher without it; the tab (or the next host's version) brings them back.

**Verified offline (2026-10-16):** `native/build.sh` and `native/build.sh --release` build all three DLLs without
warnings; `native/test/run.sh` passes (122 + 32 checks; new: join-string parsing, plan, wanted, blocked); the release
launcher contains the release key and `auto-update-1`, the dev-only strings only in the dev build.

**Verified live (2026-10-17, lane 2, Flatpak Steam, Proton Experimental, B4B_GPU=4090 headless gamescope, dev build;
`tools/autoupdate-test.py` 56/56):**
- The stub under Proton loads WinHTTP and gets HTTPS through: the real `api.github.com` `releases/latest` (v0.9.10 =
  this version) answered in 160-180 ms, "nothing to install", no window; the same with the **player** launcher
  (`native/out/release`, no ini line: "on by default"), 86 ms from the choice to the game's start.
- No answer (`https://10.255.255.1`): the game starts 2001 ms after the choice, no window. Name not resolved
  (`.invalid`): 16 ms. A fake API that answers after 6 s: 2001 ms. (A machine with no network at all fails at DNS,
  like the `.invalid` case.)
- Newer release, remembered `launch=coop`: window, installed, the stub's own mapped `xinput1_3.dll` renamed into
  `backup\0.9.10\`, User-Agent `b4bcoop-launcher/0.9.10`, the game logs `b4bcoop 0.9.11-test`, `start 1 of the freshly
  installed`, then `started fine`; `auto.txt` and the Updates tab show the result. On localhost the whole update took
  1.8 s from the choice to the game's start (download + verify + install ~0.15 s, the rest is the window's 1.5 s
  "installed" pause and the first frame). With the co-op prompt first: the same.
- ~40 KB/s download (gamescope frames every 0.5 s in `frames/`): the window for the whole 26.6 s, the progress line
  and bar move, Skip update greyed out once installing.
- Refused: bad manifest signature, bad zip hash, bad zip signature, truncated zip (under Wine it arrives short without
  a WinHTTP error; the size check refuses it) → "Couldn't update: <reason>. Starting b4bcoop 0.9.10..." for 4 s,
  nothing changed, no backup/staged folder, the old version starts.
- Skip update during a slow download (dev `B4B_LAUNCHER_UPDATE=cancel`): nothing changed, `auto.txt` "you skipped it".
- Steam join strings: host newer → `tags/v0.9.11-test` installed; host older with another protocol → `tags/v0.9.9-test`
  installed (downgrade); older with our protocol and equal → no request at all (0-1 ms).
- `pending=1` → no request; `skip=0.9.11-test` and `failed\0.9.11-test\` → not installed, no window; Go back in the
  game (writes `skip=`) → restart: not installed again.
- Automatic revert: an auto-installed 0.9.11-test with `update_never_healthy=1`: start 1 (installed), start 2 (launcher
  0.9.11-test: no check while pending), start 3 put 0.9.10 back and ran nothing, start 4 (launcher 0.9.10 again): "was
  put back before", not reinstalled.
- `auto_update=0`: "automatic update off", no request. The tab's checkbox writes `auto_update=1` and removes it again
  (dev default off).
- Regressions on the final build: `tools/update-test.py` 39/39 (after refreshing its test version and real-key check),
  `tools/shop-test.py` 75/75, `tools/e2e.py --quick` 14/14, `tools/online-test.py` 43/43.

**UX found and fixed in the live run** (gamescope frames = what a Steam Deck player sees; the dialog is scaled to the
screen there):
- the default width (360 px) wrapped the status line and Wine left stale bands in it after a text change ("is ...
  lled") → 260 dialog units wide, a full repaint after each text change;
- "Cancel: start b4bcoop 0.9.10" overflowed its button (Wine sizes buttons badly) → **Skip update**, explained in the
  footer; greyed out while installing and when done;
- a fast update was a ~1 s flash → the "installed, starting the game" state stays 1.5 s;
- failure text "not safe to install: download damaged: ..." → "Couldn't update: download damaged: ...";
- the Updates tab had the new switch below the fold (960x540) → the switch and its last result first, the by-hand
  check under "Update by hand".

**Not verified:** native Windows (the stub's WinHTTP + schannel, renaming the mapped launcher, the task dialog's look on
Windows 10/11); a controller's **B** in the update window (no virtual pad here; the code path is the prompt's); a real
GitHub release newer than the running version (every "newer" case used the fake API); a real network outage (simulated
by a blackhole address, a bad name and a slow API).
