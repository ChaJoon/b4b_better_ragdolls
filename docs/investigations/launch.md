# Loading the mod with the fewest steps (Windows + Linux/Steam Deck)

> Since #47 the root `xinput1_3.dll` (§3 A) also asks **b4bcoop co-op or Online** at every Steam start, and loads under
> Proton too through a Wine DllOverride the agent sets: docs/investigations/online-mode.md. The pass-through rules in
> §3 A are replaced by that choice (Online moves the agent out of `Gobi\Binaries\Win64` before EAC starts).

Goal: players copy files and press Play in Steam. No `.cmd`/shell scripts, no launch options, nothing alarming.
Build analysed: Steam buildid 14216215. Tools: `tools/appinfo.py` (Steam appinfo.vdf), `tools/pe.py` (static analysis).

## Result

| | Install | Launch | Status |
|---|---|---|---|
| Linux / Steam Deck | copy the zip into the game folder | Steam Play | **verified** (see §4) |
| Windows | same zip, same step | Steam Play | **hypothesis**, static analysis + the redirect mechanics exercised under Proton (§3, §4); test script in §6 |

The zip mirrors the game folder:

```
xinput1_3.dll                          Windows only: loaded by Steam's launcher stub, starts the game without EAC
Gobi/Binaries/Win64/X3DAudio1_7.dll    the agent (a proxy of the real X3DAudio1_7), both OSes
Gobi/Binaries/Win64/b4bcoop.ini
b4bcoop-README.txt, b4bcoop-COMMANDS.txt, b4bcoop-LICENSE.txt
```
All three DLLs come from the player build (`native/build.sh --release` → `native/out/release/`); the dev build
(`native/build.sh` → `native/out/`) produces the same three names with the command server and test commands.

The zip is `dist/b4bcoop-<version>.zip` (`VERSION`). The legacy layout (`dwmapi.dll` + Linux launch option + Windows
`Play B4B co-op.cmd`) is no longer shipped (0.3.0); `native/build.sh` still builds `dwmapi.dll` and
`launch/install.sh [--release] --legacy` installs it, for dev comparisons only.

## 1. Steam's launch config for app 924970

`tools/appinfo.py 924970 appinfo/config` (binary appinfo.vdf v29, parser in the tool). `config/launch`:

| # | executable | arguments | description | gated by |
|---|---|---|---|---|
| 0 | `Gobi-Win64-Test.exe` | `-SaveToUserDir -NOSCREENMESSAGES` | Test | ownsdlc 1142380 |
| 1 | `Gobi-Win64-Shipping.exe` | `-SaveToUserDir` | Shipping | ownsdlc 1142380 |
| 2 | `Gobi.exe` | `-SaveToUserDir -NOSCREENMESSAGES` | Development | ownsdlc 1142380 |
| 3 | `Gobi\Binaries\Win64\Gobi.exe` | `... -windowed -ResX=1280 -ResY=720 -ExecCmds="SCALABILITY 0"` | SafeMode | ownsdlc 1142420 |
| 4 | `Gobi\Binaries\Win64\Gobi.exe` | `-SaveToUserDir -TRSPixelStreamingOnly -NOSCREENMESSAGES` | PixelStreaming Development | ownsdlc 1142420 |
| 5 | `Gobi\Binaries\Win64\Gobi.exe` | `-SaveToUserDir` | Development OnScreenMessages | ownsdlc 1142420 |
| 6 | `Gobi-Win64-Test.exe` | `-NOSCREENMESSAGES` | Usability Test Only | beta `usability` + dlc |
| **8** | **`Back4Blood.exe`** (root) | – | **Retail** | **none (oslist windows, osarch 64)** |
| 9 | `Gobi\Binaries\Win64\Gobi.exe` | `-ExecCmds="WebServices.Environment qua, mm.bucket TEST3"` | – | beta `method` + dlc |
| 10 | `Gobi\Binaries\Win64\Gobi.exe` | `-ExecCmds="mm.bucket WBQA6" -SaveToUserDir -NOSCREENMESSAGES` | Hit Detection | beta `hitdetection` + dlc |
| 11 | `Gobi\Binaries\Win64\Gobi.exe` | – | Development without EasyAntiCheat | ownsdlc 1142380 |
| 12 | `Gobi\Binaries\Win64\Gobi-Win64-Test.exe` | – | Test without EasyAntiCheat | ownsdlc 1142380 |
| 13 | `Gobi\Binaries\Win64\Gobi-Win64-Debug.exe` | – | Debug without EasyAntiCheat | ownsdlc 1142380 |
| 14 | `Gobi\Binaries\Win64\Back4Blood.exe` | – | **Retail without EasyAntiCheat** | ownsdlc 1142380 |
| 15 | `Gobi\Binaries\Win64\Gobi-Win64-Shipping.exe` | – | Shipping without EasyAntiCheat | ownsdlc 1142380 |

- DLC 1142380/1142420 are internal (dev/QA) packages; no player owns them, so Steam hides every entry but #8. There is
  no public non-EAC or offline entry; #14 is exactly what we want but is unreachable. Steam shows no Play menu.
- There is **no Linux entry**: every entry is `oslist windows`, Proton runs the same #8. Windows and Linux start the
  same root `Back4Blood.exe` with no arguments.
- `common/steam_deck_compatibility` category 2 (Playable), `recommended_runtime proton-stable`.
- Shared-install depots 228988/228990 (Steamworks Common Redistributables): Steam installs DirectX June 2010 and the
  VC++ runtime for this game on both OSes (the Proton prefix has the native MS `x3daudio1_7.dll`/`xinput1_3.dll`
  from it in `system32`). `SteamInstall/installscript.vdf` only installs the EAC service and a firewall rule for
  `Gobi\Binaries\Win64\Back4Blood.exe`.

### The root `Back4Blood.exe` stub (504 KB, "InternalName UnrealEngine")
UE's BootstrapPackagedGame with a TRS addition. `wWinMain` = 0x140002F70 (called straight from the CRT):
- RT_RCDATA 201 = `Gobi\Binaries\Win64\Back4Blood.exe`, 202 = `Gobi -SaveToUserDir`, **203 =
  `start_protected_game.exe`**. If 203 exists (it does) the target is 203, so the command line is
  `"<root>\start_protected_game.exe" Gobi -SaveToUserDir <stub args>` (`"%s\%s" %s %s`; with `/timing` a variant).
- 0x140002290 prerequisites: `LoadLibraryW("MSVCP140.DLL")`, `("ucrtbase.dll")`, `("XINPUT1_3.DLL")` by bare name
  (normal search order, app dir first; retried as `<target dir>\name`), never freed; any missing → message box +
  `Engine\Extras\Redist\en-us\UE4PrereqSetup_x64.exe`. Then `HKLM\SOFTWARE\WOW6432Node\EasyAntiCheat_EOS`
  `ProductsInstalled` must contain product `3bc138d9...`, else it runs `EasyAntiCheat\EasyAntiCheat_EOS_Setup.exe
  install 3bc138d9...`.
- 0x140001320: rewrites `EasyAntiCheat\Settings.json` `"executable"` to resource 201 every launch (clears read-only).
- `CreateProcessW(NULL, cmdline, ..., cwd NULL)`, waits for it, returns its exit code.

So the stub always starts the EAC bootstrapper, on Linux too. Under Proton, `start_protected_game.exe` then starts
`Gobi\Binaries\Win64\Back4Blood.exe Gobi -SaveToUserDir` and the agent loads (the EAC runtime doesn't block it); on
Windows EAC protects the game process and the agent doesn't load (observed; that is why the old `Play B4B co-op.cmd` existed).
The stub imports no `SetDefaultDllDirectories`/`SetDllDirectory`, so its bare-name loads really search its own dir
first.

## 2. A DLL name both OSes load from the game dir without overrides

Wine rule (what made `dwmapi` need `WINEDLLOVERRIDES`): with no override, a DLL Wine has a builtin for is loaded as
the builtin, **unless the builtin is marked prefer-native** (`IMAGE_DLLCHARACTERISTICS 0x0010`, a Wine extension set
with `-Wb,--prefer-native`); then a native copy found by the normal search (app dir first) wins. DLLs with no builtin
at all load natively. Checked all 264 prefer-native builtins in `Proton - Experimental/files/lib/wine/x86_64-windows`
and the prefix's registry/Proton's default overrides (nothing for these names).

Imports of `Gobi/Binaries/Win64/Back4Blood.exe` (the dir holds only the exe and `libScePad.dll`):

| candidate | how loaded | Wine builtin | Windows | verdict |
|---|---|---|---|---|
| `dwmapi.dll` (current) | static | yes, **not** prefer-native | System32, not a KnownDLL | needs the Linux launch option |
| **`X3DAudio1_7.dll`** | **static** (X3DAudioCalculate/Initialize) | **prefer-native** | System32 from the DX June 2010 redist, which Steam installs for this game; not a KnownDLL | **chosen**: 2 exports, loaded at process start like dwmapi, only this exe uses it |
| `XAPOFX1_5.dll` | static (CreateFX) | prefer-native | DX redist | works the same; spare |
| `D3DCOMPILER_43.dll` | static | prefer-native | DX redist | many exports, also used by d3d paths; no |
| `UIAutomationCore.DLL` | static | prefer-native | System32 on every Windows | system component other DLLs may load; ~100 exports; no |
| `MF/MFPlat/MFReadWrite.dll` | delay | prefer-native | System32 (missing on N editions) | late, media stack; no |
| `MSVCP140/VCRUNTIME140*.dll` | static | not prefer-native, but VC redist sets `native,builtin` in the prefix | System32 | C++ data exports can't be forwarded by jmp thunks; no |
| `api-ms-win-crt-*` | static | API sets | API sets | never searched on disk; no |
| EOSSDK, steam_api64, PhysX*, vivoxsdk, NvCloth, libvorbisfile, XAudio2_9Redist, GFSDK_Aftermath | delay | none | not in System32 | UE preloads them by full path from `Engine/Binaries/ThirdParty` (not in the exe dir), so a proxy there would likely be bypassed; loaded late anyway; not tested |
| `libScePad.dll` (ships with the game) | delay (9 functions) | none | game dir | see below |

`libScePad.dll` proxy (original renamed, e.g. `libScePad_orig.dll`): loads on both OSes, but Steam's "Verify
integrity" and every game update restore the original over our proxy (the mod silently disappears and our renamed
copy is left behind), it modifies a game file (Steam flags it), and it is delay-loaded when UE initialises the
DualSense plugin, i.e. after the exe starts, too late for `netguard` which must be in place before any game code
runs. Rejected; `X3DAudio1_7.dll` adds a file and changes none.

Caveat for `X3DAudio1_7.dll`: if the DirectX runtime is missing on a Windows machine, the game itself used to fail
with "X3DAudio1_7.dll not found"; with our proxy it starts and 3D audio calls return E_NOTIMPL instead. Steam
installs the runtime for this game, and the root stub checks for XINPUT1_3 (same redist) before every launch.

The agent builds under both names from the same sources (`native/build.sh`, proxies generated into `native/proxy/` by
`tools/gen-proxy.py` from the native MS DLLs in the prefix; dwmapi regenerated from its earlier `.def`). If both are
installed, one agent runs per process (named mutex `Local\b4bcoop-agent-<pid>`; a `dwmapi.dll` from the same dir
wins, because an older dwmapi build doesn't know the mutex).

## 3. Windows without the `.cmd`

The problem is only the EAC bootstrapper that the stub starts. Options, best first:

### A. Root `xinput1_3.dll` launch redirect (implemented) — confidence: medium-high
The stub `LoadLibraryW("XINPUT1_3.DLL")`s by bare name every launch, from wWinMain, before `CreateProcessW`
(§1). `xinput1_3` is not a KnownDLL, so Windows loads `<game root>\xinput1_3.dll` first. Ours
(`native/launcher/redirect.c`, a generated proxy of all 12 xinput1_3 exports):
1. DllMain, only when the host exe is `<our dir>\Back4Blood.exe` with a `Gobi\Binaries\Win64` next to it: pins
   itself, opens `Gobi\Binaries\Win64\b4bcoop-launcher.log`, patches the stub's IAT entry for `CreateProcessW`.
2. When the stub starts `"...\start_protected_game.exe" Gobi -SaveToUserDir <launch options>`, it starts
   `"<root>\Gobi\Binaries\Win64\Back4Blood.exe" Gobi -SaveToUserDir <launch options>` instead, cwd = that dir, env
   inherited from Steam (SteamAppId/SteamGameId set if missing). The stub waits for it as it would for EAC, so Steam
   shows the game as running and Steam-initiated launches (Join Game, `+b4bcoop_join`) go the same way.
3. Pass-through (EAC as usual) if the agent (`X3DAudio1_7.dll` or `dwmapi.dll`) is not in the Gobi dir, or the
   launch options contain `-b4bcoop=off`.

Same arguments as the EAC path and the same direct start the `.cmd` does (verified on Windows in a real session).
Since #47 it also asks co-op / Online (online-mode.md), and co-op starts install a newer b4bcoop first
(updater.md §7).
The stub has already rewritten Settings.json and checked the EAC install, both harmless. Under Proton the file is
inert: Wine's builtin `xinput1_3` is not prefer-native.
Risks: Windows Defender / Smart App Control can flag unsigned DLLs (applies equally to the agent itself); a stub
update that stops loading XINPUT1_3 or starts using safe DLL search would silently fall back to EAC (the launcher log
then doesn't exist).

### B. Launch option `"<game>\Gobi\Binaries\Win64\Back4Blood.exe" Gobi -SaveToUserDir %command%` — medium
Steam substitutes `%command%` with the quoted root stub path (entry #8 has no arguments), so the game gets
`Gobi -SaveToUserDir "C:\...\Back 4 Blood\Back4Blood.exe"`. UE strips the first token when it is the project name
(`Gobi`); `UGameInstance::StartGameInstance` (0x143CA5xxx: inline `FParse::Token`, then `**PackageName == '-'` →
default map) takes the next token, `-SaveToUserDir`, so the trailing path is ignored. The shipping-only "no map
override on the command line" block isn't compiled in (no `BENCHMARK` string in the exe), so the **plain
`"<exe>" %command%` form is risky**: the stub path becomes the first token, `FURL` treats `X:\...` as a map file,
the load fails and the game shows "The map specified on the commandline ... could not be found. Would you like to load
the default map instead?" (string at 0x145C428B0, referenced from the same function). Needs the full path typed per
library, so worse UX than A; keep as the documented fallback.

### Rejected
- `EasyAntiCheat/Settings.json` `"executable"`: the stub rewrites it every launch (0x140001320), and whatever it
  names still runs under EAC protection (and EAC checks it against the deployment).
- A DLL loaded by `start_protected_game.exe` (it imports `VERSION`, `WINMM`, `CRYPT32`, not KnownDLLs, and bare
  `LoadLibrary`s SDL video/audio DLLs): works around the anti-cheat's own bootstrapper, which EAC updates and
  integrity-checks, and looks like cheating. Unnecessary given A.
- `Play B4B co-op.cmd` / `launch/run.cmd` (both removed in 0.3.0): worked (verified), but the owner doesn't want to
  ship scripts, and Steam Join Game can't use it.

## 4. Prototype results on Linux (Proton Experimental)

All on 2026-09-24 with `launch/install.sh` of this branch: `Gobi/Binaries/Win64/X3DAudio1_7.dll` +
`<game>/xinput1_3.dll`, **no `dwmapi.dll`** in the game dir.

| # | Test | Result |
|---|---|---|
| T1 | Gobi exe directly on test prefix 1, `WINEDLLOVERRIDES` unset (only Proton's own defaults, which don't name x3daudio/dwmapi) | **Loads.** Wine trace: `X3DAudio1_7.dll has prefer-native flag, ignoring builtin` → mapped from the game dir; log `b4bcoop loaded as X3DAudio1_7.dll`, all hooks, Wwise audio device initialised. |
| T2 | **Real Steam client**, `steam://rungameid/924970`, real prefix | **Loads.** Chain: reaper → `proton waitforexitandrun` → root `Back4Blood.exe` → `start_protected_game.exe` → `S:\...\Gobi\Binaries\Win64\Back4Blood.exe Gobi -SaveToUserDir`; `b4bcoop-556.log`: `loaded as X3DAudio1_7.dll`, `server: listening`, warning/title screen reached, then killed by PID (no sign-in, no progression). Repeated with the final build (`b4bcoop-552.log`, same result). The user's launch option `WINEDLLOVERRIDES="dwmapi=n,b" %command%` was left as it was (not touched); with no `dwmapi.dll` in the game dir it only affects the builtin dwmapi, so this is the no-launch-option case for our DLL. No `b4bcoop-launcher.log`: the root `xinput1_3.dll` is inert under Proton, as intended. |
| T3 | Windows redirect mechanics, forced under Proton: root stub on test prefix 1 with `WINEDLLOVERRIDES=xinput1_3=n,b`, extra arg `-b4bcoop_testarg` | **Works.** `b4bcoop-launcher.log` below; no `start_protected_game.exe` process; the game ran as `...\Back4Blood.exe Gobi -SaveToUserDir -b4bcoop_testarg`, agent loaded, auto sign-in and hosting in Fort Hope (`status`: FortHope world, PacketRelayNetDriver). |
| T4 | Same with `-b4bcoop=off`; and with `X3DAudio1_7.dll` removed | `pass-through to Easy Anti-Cheat: -b4bcoop=off in the launch options` / `... agent DLL not installed`; the stub starts `start_protected_game.exe` unchanged. |
| T5 | Regression: `tools/e2e.py --quick` (`multi.sh 2`: join, mission follow, flashlight, burn cards, chat, rewards, chapter transition, profile diffs, no public peers) | **10/10 PASS** (361 s), both instances `loaded as X3DAudio1_7.dll`. |

```
b4bcoop launcher (xinput1_3.dll) 2026-09-24 13:19:40, game root Z:\home\dan\.local\share\Steam\steamapps\common\Back 4 Blood\
hooked CreateProcessW
stub: CreateProcessW(NULL, "Z:\...\Back 4 Blood\start_protected_game.exe" Gobi -SaveToUserDir -b4bcoop_testarg)
redirect (no EAC): "Z:\...\Back 4 Blood\Gobi\Binaries\Win64\Back4Blood.exe" Gobi -SaveToUserDir -b4bcoop_testarg   cwd Z:\...\Gobi\Binaries\Win64\
started pid 328
```

Harness note: running the root stub with a bare `proton run` (outside the Steam client) makes the game crash inside
Wine's loader (`ntdll` `load_dll`, NULL file-id while walking the module list) as soon as it statically imports any
native DLL from the game dir, the legacy `dwmapi.dll` build included; the same chain started by Steam (T2) is fine.
So test the stub path through Steam, not `proton run` (T3 isn't affected: the redirect skips `start_protected_game`).

## 5. Final install flow

Both OSes, one zip (`launch/package.sh` → `dist/b4bcoop-<version>.zip`, layout mirrors the game folder):
1. Steam → Back 4 Blood → Manage → Browse local files; copy the zip's contents in (the `Gobi` folder merges; nothing
   is replaced).
2. Press Play, sign in Offline: hosting is the default, friends join with Steam "Join Game" (the ini is optional).

Uninstall: the README's "Remove" list (`launch/uninstall.sh` does the same). Upgrading from the dwmapi version:
delete `dwmapi.dll`, the `.cmd` and the `steam_appid.txt` it wrote; Linux: remove the launch option (harmless if
left, but pointless). A normal (online) game without uninstalling: pick **Online** at the start, or launch option
`-b4bcoop=off` (since #47 the launcher moves the agent out of the game's folders first; before, the agent stayed in
`Gobi\Binaries\Win64` as a plain proxy, which an EAC-protected game would still have loaded: online-mode.md).

| | Verified | Hypothesis |
|---|---|---|
| Linux/Steam Deck | X3DAudio1_7 loads with no override via direct run and via the real Steam launch; hosting/joining regression (T5). Steam Deck itself not run, same Proton mechanism. | – |
| Windows | Static analysis only: stub loads XINPUT1_3 by bare name before CreateProcessW; redirect code path exercised under Proton (T3/T4); X3DAudio1_7 is a non-KnownDLL static import resolved app-dir-first exactly like dwmapi, which already worked on Windows. | That Windows loads the root `xinput1_3.dll` into the stub, that the redirected start behaves like the `.cmd` one, Defender/Smart App Control reactions. §6. |

Dev only since 0.3.0: `native/out/dwmapi.dll` (same agent), `launch/install.sh --legacy`. The legacy zip and
`launch/run.cmd` are gone; if the redirect fails on Windows, §3 B is the fallback.

## 6. Windows test script (owner's laptop)

Prep: `launch/package.sh` → copy `dist/b4bcoop-<version>.zip` (and, for W5 only, `native/out/release/dwmapi.dll`) to
the laptop. In the game folder
remove the old install first: `Gobi\Binaries\Win64\dwmapi.dll`, `Play B4B co-op.cmd`, `Gobi\Binaries\Win64\steam_appid.txt`
(its presence would hide a Steam relaunch problem), and clear the game's Steam launch options. Keep Task Manager
(Details tab, add the "Command line" column) open. `N` below = the game's PID.

**W1: main flow (the one that matters).** Extract `b4bcoop-<version>.zip` into the game folder, press Play in Steam.
- Expect `Gobi\Binaries\Win64\b4bcoop-launcher.log`:
  `hooked CreateProcessW` → `stub: CreateProcessW(NULL, "...\start_protected_game.exe" Gobi -SaveToUserDir )` →
  `redirect (no EAC): "...\Gobi\Binaries\Win64\Back4Blood.exe" Gobi -SaveToUserDir` → `started pid N`.
- Expect `Gobi\Binaries\Win64\b4bcoop-N.log` starting with `b4bcoop loaded (player build) as X3DAudio1_7.dll`,
  `b4bcoop <version> (protocol N)`, then `init: tick hooked`, `netguard: armed`.
- No Windows Firewall prompt (the game's UDP socket is bound to 127.0.0.1: `steamnet: game UDP socket bound to
  127.0.0.1 port 7777`).
- No EAC splash (the Back 4 Blood splash from `EasyAntiCheat\SplashScreen.png`), no `start_protected_game.exe` /
  `EasyAntiCheat*.exe` in Task Manager; `Back4Blood.exe` command line ends in `Gobi -SaveToUserDir`.
- Steam shows the game as running and back to Play after quitting; no second launch/relaunch loop.
- Sign in Offline: same profile/progression as before (saves in `%LOCALAPPDATA%\Back4Blood\Steam\Saved`); 3D audio
  (positional zombie sounds) and controller work.
- Then one co-op session as host (the default) or as a client via Steam Join Game, as in the README.

If W1 fails, what the logs say:
- no `b4bcoop-launcher.log` at all → the stub didn't load our `xinput1_3.dll` (Defender quarantine? check Protection
  history; else Process Monitor, filter `Path contains xinput1_3`, see which path the stub opened).
- `CreateProcessW import not found` → IAT patch failed (send the log).
- launcher log ends in `redirect ...` + `CreateProcessW failed: <code>` → send the code.
- redirect OK but no `b4bcoop-N.log` → `X3DAudio1_7.dll` not loaded (Process Monitor `Path contains X3DAudio`); if
  the game says "X3DAudio1_7.dll not found / entry point" → the DirectX runtime isn't installed.
- game starts and Steam immediately starts it again → steam_api relaunch; send the launcher log.

**W2: opt-out.** Launch option `-b4bcoop=off`, Play: EAC splash appears, launcher log says
`pass-through to Easy Anti-Cheat: -b4bcoop=off in the launch options`, no new `b4bcoop-N.log`. Clear the option.

**W3: Steam Join Game (needs a second account, optional).** Host on another PC (hosting is the default); from the laptop's Steam
friends list "Join Game" while B4B is closed. Expect a launcher log whose redirected command line carries
`+b4bcoop_join steam:...`, and the agent joining (steam-invites.md).

**W4: only if W1 fails: launch-option fallback (§3 B).** Delete the root `xinput1_3.dll`. Launch options:
`"<game>\Gobi\Binaries\Win64\Back4Blood.exe" Gobi -SaveToUserDir %command%` (full path of this PC's install,
quotes included). Expect `b4bcoop-N.log` (`loaded as X3DAudio1_7.dll`), no EAC splash, no map dialog, Task Manager
command line `... Gobi -SaveToUserDir "<game>\Back4Blood.exe"`. Optional: the short form `"<exe path>" %command%` is
predicted to show "The map specified on the commandline ... could not be found"; click OK and note what happens.

**W5: both names installed (upgrade case).** With W1's files in place, also copy `dwmapi.dll` (player build,
`native/out/release/dwmapi.dll`) into `Gobi\Binaries\Win64`. Play: exactly one new `b4bcoop-N.log`, first line
`loaded as dwmapi.dll`. Delete `dwmapi.dll` again.

Report back: the two logs of W1 (and of any failing step), plus yes/no for splash, relaunch, audio, co-op.
