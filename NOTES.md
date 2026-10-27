# Notes

## 2026-10-26: input switched to the owner's disc copy

The first test lift used a different copy of the game. It was replaced the same day: `game/` is
now a copy of the owner's English retail CD, and everything lifted from the earlier copy was
deleted and redone. Only the retail-exe results below count.

## Inputs

| what | value |
|---|---|
| exe | `game\ArmyMen2.exe`, md5 `59180d8113d1d756e838b0dab1790b6b` — the 1999 retail build, CD check included (MSVC 6.0, PE timestamp 0x36B7AE36, 585,728 B) |
| pcrecomp | `third_party/pcrecomp`, commit `54cb21f7110776af390bd7260a297ffcaa2a0b01` (2026-10-26), MIT |
| cnc-ddraw | v7.1.0.0 (MIT), `cnc-ddraw.zip` sha256 `0b13ab89a64c9918189b1dadd449ef6ed3cb3b7b19cabd96d8adbd95505bb908`, `ddraw.dll` sha256 `85e0f7d530dfda134793a57cb3e76b0287dcc96892ee57162dd68f47283b03a9`; copied into `game/` with `windowed=true`, `savesettings=0` and a `[plasticrecomp]` profile equal to its own `[ArmyMen2]` one |
| Python | 3.11 in `.venv`, capstone 5.0.7, pefile 2024.8.26 |
| compiler | MSVC x86, VS 2022 Build Tools 14.44 |

## Lift (retail exe)

- **Catalog** (`disasm32.py`): 3,083 entries, 270,223 instructions, 95.9% of the code range.
- **Scored against Reference A's 1,584 functions** (`scripts/refa_reference.py`, `score_recovery.py`):
  recall **100%**, precision 79.6%. The 405 false starts are **all splits** (inside a real
  function), **0 invented**. Ends: 1,282 exact, 283 short, 19 long.
- **Lift** (`python -m tools ... --all`, 19 s): 3,083 functions, 619,131 lines of C.
  9 `UNIMPLEMENTED` instructions (`int 0xfe`, `insd`, `sti`, `lcall`, `hlt`), every one in a
  split entry decoded from the middle of a real function.
- The default pipeline's output needs `/FIrecomp_funcs.h` (its files do not include the
  prototypes), and its `import_stubs.c` is not valid C (stdcall `@` names), so it is not built:
  native32 binds the imports itself.

### The memmove crash, and `scripts/run_lift.py`

The first long run (about two minutes in, as the opening cinematic ended) logged
`ITAIL: unresolved VA 0x004659AC from 0x00465710`, then a write to `0x406924B0` in
`sub_004459E0`. `0x00465710`-`0x00465A45` is the static CRT's memcpy/memmove. It dispatches through
inline jump tables, and the lifter turns each `jmp [reg*4 + table]` into a local switch over the
labels discovery found, with `RECOMP_ITAIL` as the default. The backward copy does
`neg ecx; jmp [ecx*4 + 0x004659A0]` (index -7..0), so its table lies **below** the displacement, and
disasm32 reads tables forward only: its 7 arms (`0x004659A4`-`0x004659D4`) had no label and no entry.
They run only for some overlapping copies, which is why it took two minutes to hit one.

`scripts/run_lift.py` is the default pipeline (pcrecomp's `translator.load_pe`, `discover_functions`,
`generate_recomp_files`) plus a pass that finds every `jmp [reg*4 + disp32]` by encoding inside each
function, reads its table both ways while the entries stay inside that function, and gives each arm
that is neither a block nor an entry a body of its own as an **alias**. A seed would not do: seeds
are kept as function starts, and a start inside memmove would clamp memmove short. It added 14 arms:
memmove's 7 and the same pattern in `sub_00466D80`, another CRT copy routine
(`work/table_arms.json`). 3,097 dispatch entries.

## Host (`src/host/main.c`, `scripts/build_host.bat`)

- pcrecomp's `runtime/native32`: the guest image maps 1:1 at 0x00400000, imports go to the real
  Windows DLLs through the native bridge, and Windows calls back into lifted code through the
  non-executable-fetch trampoline.
- Linked at `/BASE:0x60000000` with `/STACK:0x40000`. `native32_hold_image` reserves the guest
  range at process start (the host re-launches itself once), and the lifted code runs on a thread
  with a 64 MB stack.
- Shims, the only game-specific imports: `GetModuleFileNameA` and `GetCommandLineA` answer with
  the guest exe in `game\`, `SetUnhandledExceptionFilter` keeps the guest's filter instead of
  installing it, `MessageBoxA` also logs its text, and `timeSetEvent`/`timeKillEvent` count
  callbacks in flight so a kill waits for them (the race redalert2-recomp's host documents).
- `SetDllDirectoryA(game)` so `smackw32.dll` and cnc-ddraw's `ddraw.dll` bind from the game folder.
- Build: the lifted C at `/O1` plus the runtime and host, 10 s; `GEN_OPT=/Od` for a 4 s build.
  Links with no errors.

## Boot

| run | build | result |
|---|---|---|
| `--dry` | /Od | mapped 0x00400000-0x00667000; imports: 164 native, 7 shimmed, **0 unresolved** |
| `--watchdog 20` | /Od | window "Armymen II" up; 20 s with no fault, 8.0 M indirect calls, in the message loop and `SmackWait` (the 3DO logo movie) |
| `--watchdog 20` | /O1 | same place, 22.6 M indirect calls (2.8x the /Od rate) |
| `--watchdog 50` | /O1 | still playing Smacker frames (`SmackDoFrame`, `SmackToBuffer`): `act1.smk`, the 60 MB opening cinematic, follows the logo |
| `--watchdog 165` | /O1, default lift | **crash** as the cinematic ended: `ITAIL: unresolved VA 0x004659AC`, then a wild write (memmove, above) |
| `--watchdog 180` | /O1, `run_lift.py` | **no fault in 180 s**, 189 M indirect calls; screenshots: 3DO logo (4 s), cinematic (14 s), black fade (140 s), **the title menu** (160 s): logo, all seven buttons, the game's cursor, the copyright line. At 180 s the guest is in its menu loop (`IntersectRect`, `QueryPerformanceCounter`, DirectDraw) |

Movie lengths, from the Smacker headers: `3do.smk` 84 frames at 66.66 ms (5.6 s), `avi\act1.smk`
1,871 frames (124.7 s), so the title appears about 131 s in. Escape sent from a background script did
not skip the cinematic, probably because the window never got keyboard focus for DirectInput; a
player pressing a key was not tested.

## Not done yet

- **Past the title menu.** Works. The owner mounted an image of their own disc (label `ARMYMEN2`; it had to be mounted after a Windows restart,
  because cdrom.sys was stuck with Code 38). They played Boot Camp. Nothing here bypasses the CD check.
- **Guest C++ exceptions.** The default lifter emits no `RECOMP_EH_ENTER` landing pads, and the CRT is
  static (no `_CxxThrowException` import for native32's eh32 to catch), so a guest `throw` would end
  the process. None has been hit yet.
- **Input under the host.** Done for the mouse; see "Mouse" below. The owner confirmed the feel.

## Display: the host's own DirectDraw and presenter (default since 2026-10-26)

`--cnc-ddraw` keeps the earlier path. The design and its steps are in `docs/R_hires.md`. In short:
- **`src/host/vddraw.c`** answers `DirectDrawCreate`, so ddraw.dll and cnc-ddraw are never loaded.
  - Real COM objects with native vtables implement exactly the methods of the game's fullscreen
    path (a DirectDraw inventory subagent listed them).
  - Surfaces are 8-bit DIB sections, so the font builder's GDI text works. Flip swaps buffers, and
    blits are overlap-safe.
  - A presenter thread shows the primary at up to 60 fps.
- **`src/host/present.c`** (D3D11): an R8 frame plus a palette texture, the lookup in the shader,
  nearest or sharp-bilinear, aspect kept. It draws regions at different scales.
- **The window** is resizable, and opens at the largest whole multiple of 640x480 that fits.
  `GetSystemMetrics` answers 640x480, and the process is per-monitor DPI aware.
- **It keeps running unfocused.** Without that the intro froze on focus loss. The host swallows
  `WM_ACTIVATE`/`WM_ACTIVATEAPP` deactivation and shims `GetFocus`.
- **One CPU core**, as cnc-ddraw's `singlecpu` did: smackw32's 1999 audio code races on several
  cores, so movies stalled at random frames. Proven with `--movie-log`: in the virtual path 4 of 4
  runs stalled; pinned, 8 of 8 played, and the owner confirmed the intro and VIEW MOVIES. Game speed
  equals the cnc-ddraw path.
- **HUD layout in missions** (state `0x00511DA4` == 2), drawn by the presenter as regions with no
  lifted code changed:
  - the battlefield at the world scale;
  - the top strip stretched to the window's width;
  - the panel at the window's right edge.
  Shift+Plus/Minus scale the HUD.
- **Single copy:** the game quits silently (`ExitProcess(0)` from `sub_00469331`) if `ArmyMenMutex`
  exists. The host checks first and says so. Test runs started while the owner played looked like
  "crashes in the first movie" because of this.

## Mouse: cnc-ddraw's hooks and the recompiled game

cnc-ddraw 7.1.0.0 (source read at tag v7.1.0.0, commit 541b5de) fixes windowed mouse and window
behaviour by hooking. With `hook=4` it patches the import tables of modules in the host exe's folder
(`hook_create`, src/hook.c:527-607; modules found by `util_enumerate_modules`, src/utils.c:25-72),
skipping any module whose `GetModuleFileNameA` fails, which is every image the Windows loader does
not know. It hooks at its DllMain and again at `SetCooperativeLevel` (src/dd.c:1394-1398).
DirectInput it hooks inline (Detours on `DirectInputCreateA`), once dinput.dll is loaded through its
own `LoadLibraryA` hook (src/directinput.c:388-436, winapi_hooks.c:1603-1608).

Measured with `--imports`, before routing: `DirectInputCreateA` already started with a `jmp` into
DDRAW.dll (native32 loads dinput.dll through the host's patched `LoadLibraryA`), but **every one of the
game's 38 USER32 slots pointed at user32 itself**: the guest image, mapped by native32, was skipped.

The host now routes them (`route_through_host_iat`, `src/host/main.c`): the host imports the same
USER32 functions, so the host exe, a module cnc-ddraw does patch, has an import cell for each, and each
guest slot points at a six-byte `jmp [cell]`, read at call time. Measured after 15 s: 37 slots routed,
13 of them land in cnc-ddraw (`DefWindowProcA`, `GetSystemMetrics`, `ClientToScreen`, `GetClientRect`,
`SetWindowPos`, `GetWindowRect`, `SetWindowLongA`, `GetWindowLongA`, `ShowCursor`, `SetCursor`,
`CreateWindowExA`, `PeekMessageA`, `DestroyWindow`); the rest are not hooked by cnc-ddraw and stay
plain. `--no-route` turns it off for comparison.

**GDI32 is not routed.** The first version routed GDI32 too, and cnc-ddraw's palette hooks
(`RealizePalette`, `SelectPalette`, `GetSystemPaletteEntries`, `GetDeviceCaps`) gave the movies blue
speckle noise (owner report, confirmed by screenshots at 14 s: no routing and USER32-only identical and
clean, USER32+GDI32 speckled). `--route-gdi` keeps it available for experiments only.

Verified with routing on (`--watchdog 175 --imports`, 2026-10-26): intro and cinematic in clean
colours (no blue-dominant pixels at 18 s and 60 s), the title menu at 150 s and 165 s, exit 4 (the
watchdog), 0 faults, 0 unresolved jumps, 193.6 M indirect calls; the same 13 USER32 slots reach
cnc-ddraw, and every GDI32 slot reaches gdi32.dll.

What cnc-ddraw's DirectInput hook does (and does not): it forces every device to
`DISCL_BACKGROUND|DISCL_NONEXCLUSIVE`, and zeroes the mouse state until a click inside the window
locks the mouse. It does **not** scale or rewrite deltas.

Owner's feel test with routing on (2026-10-26): still wrong. The game cursor is faster than the
owner's (slow) Windows pointer setting, and its speed changes with the window's size. That is the
game turning raw DirectInput counts into a cursor in 640x480 game pixels.

### Feeding the game the Windows cursor (`src/host/mouse.c`)

`DirectInputCreateA` is a host shim: it creates the real object (cnc-ddraw's inline hook still runs,
because dinput.dll is loaded through the host's hooked `LoadLibraryA`) and wraps `CreateDevice`;
the game's `GUID_SysMouse` device gets `GetDeviceState`/`GetDeviceData` wrappers on top of cnc-ddraw's.
Measured with `--mouse-log`: DirectInput version 0x0500, and the game reads the mouse **buffered**,
`GetDeviceData(cbObjectData 16, 1 event per call, flags 0)` until a call returns none.

So at the first call of each polling cycle, with the game's window in the foreground, the host takes
the Windows cursor in game pixels (`GetCursorPos` through the host's import table: cnc-ddraw's
`fake_GetCursorPos`, which already handles windowed, stretched and letterboxed windows) and queues X/Y
events for its difference from the game's cursor. Calls hand those out first, then the real button
and wheel events; DirectInput's raw X/Y events are dropped. `--raw-mouse` restores the 1999
behaviour. Movement then follows Windows' pointer speed and acceleration and any window size.

How the game spends those events (lifted C; subagent read, 2026-10-26):
- Setup `sub_00426D30`: `DirectInputCreateA(..., 0x0500, ...)`, `GUID_SysMouse` into `[0x00512FD4]`,
  `c_dfDIMouse`, `DISCL_EXCLUSIVE|DISCL_FOREGROUND` (cnc-ddraw forces non-exclusive), a 32-event buffer.
- Poll `sub_00427070`, once per frame: zero the last deltas `[0x485458]`/`[0x48545C]`, then
  `GetDeviceData` one event at a time; offsets 0 X, 4 Y, 8 Z, 0xC-0xE buttons 0-2.
- Each event goes through `sub_00426F40`, which overwrites that axis' last delta, then adds **both**
  last deltas to the cursor and clamps it to 0..639 x 0..479. No scaling or acceleration. The cursor is
  `int32` X at **`0x00485464`** and Y at **`0x00485468`**, starting at (320,200), drawn by `0x00412FE0`.
- The quirk: X=+5 then Y=+3 moves X by 10, because the second event re-adds X's delta. So the host
  sends `X=dx, X=0, Y=dy, Y=0`, then the real button events, and reads the game's cursor from those two
  variables each cycle; the placement is exact, whatever the game did before.

Smoke test, no hand on the mouse: the first cycle fed `Y=+40, Y=0` (cnc-ddraw's cursor (320,240), the
game's (320,200)) and later cycles fed nothing, so the game's cursor read back equal to the target.
Owner's feel test (2026-10-26): "the mouse feels right now".

In the virtual display path there is no cnc-ddraw to ask. There the target comes from `vdd_cursor`
(the real cursor, `ScreenToClient`, then the presenter's region mapping), and `hook_set_coop` forces
the mouse device to `DISCL_NONEXCLUSIVE|DISCL_FOREGROUND`, which cnc-ddraw's hook used to do.

## Multiplayer: legacy DirectPlay

The game asks for `IDirectPlay3A` (`CLSID_DirectPlay` at 0x0046F6D8, `IID_IDirectPlay3A` at
0x0046F6C8, with the TCP/IP and IPX provider GUIDs beside them). `build\plasticrecomp.exe --dplay-check`
creates that object in the host's 32-bit process and lists its connections. On this PC, 2026-10-26:
`CoCreateInstance` S_OK, `EnumConnections` S_OK with 4 connections (TCP/IP, IPX, modem, serial). So
Windows' legacy DirectPlay is already enabled here (registered: `dplayx.dll`, four service
providers). On a PC where it is not: `Enable-WindowsOptionalFeature -Online -FeatureName DirectPlay -All`
in an admin PowerShell. DirectPlay does not cross NAT, so internet play needs a virtual LAN (Tailscale,
ZeroTier, Radmin VPN) between the players, each running their own build. Retail disc builds only: the GOG
release (and, by its store page, Steam) has multiplayer cut.

Tested on two real PCs on a LAN (2026-10-27, TCP/IP): the lobby, joining, the deployment screen, the mission,
ESC / ABORT all work. Findings:

- **Connection names.** The DirectPlay providers' names come from Windows in its UI language, and the
  game's lobby showed them as they were. The host hooks `EnumConnections` on the object `CoCreateInstance`
  returns (main.c) and swaps in the original English names by provider GUID (`PLASTIC_DPNAMES=0` turns
  it off).
- **Deployment phase.** In a network game the mission starts with a deployment screen, which the game
  builds with the HUD gone and never re-creates by itself afterwards. The `sub_004260C0` wrapper
  (hires.c) keeps the classic 640x480 geometry while the flag at 0x00511DD4 is set and, when it clears,
  re-creates the HUD (the flag's writer was seen as `sub_0040B000`).
- **Pink screen.** The key-colour fill of a dialog's corner showed between the lobby and the next
  screen; `vdd_dialog_end` now fills black.
- **Two copies on one PC.** The game's single-instance mutex `ArmyMenMutex` (0x0040B60F) is hidden from
  `OpenMutexA` when `PLASTIC_MULTI=1`, so a game against `127.0.0.1` can be tried alone.

## GOG buyers

The GOG `ArmyMen2.exe` (md5 `78a6f534...`) is the retail exe with six CD-check branches changed
(JNZ to JMP) by the publisher, and every game asset is byte-identical to the disc's
(compared by SHA-256); only `SMACKW32.DLL` (GOG's is 42,496 B, retail's 95,232 B) and GOG's own
`ddraw.dll` wrapper differ. So a GOG buyer is supported the same way a disc owner is: lift the exe you
own. A lift of the GOG exe simply has no CD check.

- `scripts/run_lift.py` writes `recomp_target.h` with the MD5 and build name of the exe it lifted.
- The host refuses any other exe (`check_exe`, MD5 by CryptoAPI): measured, it accepts the retail
  folder and refuses the Russian edition's exe with the build names and the command to lift it. The
  old check (PE timestamp and size) could not tell retail, GOG and Russian apart: they share both.
- `scripts/setup.ps1` (find the install, including GOG's registry entry for game id 1913968552,
  identify the build, copy it to `game\`, put cnc-ddraw in place of GOG's wrapper in the copy, lift,
  build).
- Multiplayer is cut from the GOG release, so a GOG install has no network game (the DirectPlay notes above
  apply to the retail disc build only). Steam's store page (read 2026-10-27) lists only "Single-player" and "Family
  Sharing" (PICS categories 2 and 62: no multiplayer, co-op or LAN category), and its system requirements say "Multiplayer is not supported"; the Steam exe is likely GOG's, so the
  same holds there.
- Tested with a GOG install (set up with `scripts\setup.ps1`, played), as with the English retail CD and the
  Russian edition (GSC, WebColl).

## Steam buyers (app 549170), from public data only

No Steam copy here, so this is from Steam's product info (`api.steamcmd.net/v1/info/549170`, a mirror
of Steam's PICS data), the Steam store API and PCGamingWiki. SteamDB's pages answer automated clients
with a browser check, so its per-file list was not read.

- Released 2017-12-20 by 2K; public build 2346976, built 2017-12-07. Store categories: "Single-player",
  "Family Sharing" (no multiplayer listed). Launch: `ArmyMen2.exe`, install dir `Army Men II`.
- Depots: **549172** (Windows, the game) 600,368,621 B; **549171** (no OS tag) 1,530,802 B, contents
  unknown; 228990 from app 228980 (Steamworks Common Redistributables, i.e. DirectX).
- **Size fit (inferred):** the GOG release's game files, without GOG's store packaging, the owner's
  save and any `ddraw.dll`, total 599,087,970 B (GOG-era manifest).
  Adding GOG's original 2017 DirectDraw wrapper (1,277,952 B) gives 600,365,922 B: **2,699 B short of
  Steam's depot**, about one small config file. So Steam most likely ships the same 2017 2K package as
  GOG: the CD-check-disabled exe, the 42,496 B `SMACKW32.DLL`, the same wrapper. A SteamStub-wrapped exe
  would be tens of KB larger, which the fit leaves no room for. Not confirmed by a file hash.
- PCGamingWiki lists the Steam release's DRM as "Steam" (the client) and GOG's as DRM-free; it lists
  the retail disc as SafeDisc, which this disc's exe is not (no protected sections, no packer,
  `analyze_sections.py`; its CD check is a volume-label test).

What this means: a Steam buyer is handled exactly like a GOG buyer, by lifting the exe they own. If
a Steam exe ever turns out to be wrapped, `run_lift.py` refuses it (`drm_wrapped`: a `.bind` section,
SteamStub's) instead of unwrapping it; tested on a scratch copy with a section renamed `.bind`. This
project removes no copy protection, and pcrecomp's `tools/drm/` scripts are not used.

## Tooling

- `scripts/setup.ps1`: owner's game (disc, GOG, Steam) to built host.
- `scripts/present_test.c`: the presenter, tested alone.
- `scripts/hash_compare.py`: hashes two copies of the game folder and compares them file by file.

`scripts/run.ps1` (run, log to `build\run.log`), `scripts/screenshot.ps1` (capture the game window
to `build\screens\`), `scripts/boot_check.ps1` (run plus screenshots at 5/12/20/30 s) and
`scripts/sendkey.ps1` (press keys in the game window). Comparing the owner's disc copies
with the GOG data found every game asset identical; only `ArmyMen2.exe` and
`SMACKW32.DLL` differ.
