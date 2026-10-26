# Notes

## 2026-10-26: input switched to the owner's disc copy

The first test lift used a GOG-installer copy whose origin the owner doubts. It was replaced the
same day: `game/` is now a copy of `G:\Licensed copies\Army Men II ENG`, the owner's English
retail CD (see `docs/PROVENANCE.md`), and everything lifted from the GOG copy was deleted and
redone. Only the retail-exe results below count.

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

- **Past the title menu.** SINGLE PLAYER and BOOT CAMP run the retail CD check, which wants the disc
  (volume label `ARMYMEN2`) in a drive. Nothing here bypasses it.
- **Guest C++ exceptions.** The default lifter emits no `RECOMP_EH_ENTER` landing pads, and the CRT is
  static (no `_CxxThrowException` import for native32's eh32 to catch), so a guest `throw` would end
  the process. None was hit during boot.
- **Input under the host.** Mouse and keyboard reach the game through real DirectInput. Only the
  boot was watched, not menu clicks.

## Tooling

`scripts/run.ps1` (run, log to `build\run.log`), `scripts/screenshot.ps1` (capture the game window
to `build\screens\`), `scripts/boot_check.ps1` (run plus screenshots at 5/12/20/30 s) and
`scripts/sendkey.ps1` (press keys in the game window). `docs/PROVENANCE.md` records both disc copies
and compares them with the GOG data: every game asset is identical, only `ArmyMen2.exe` and
`SMACKW32.DLL` differ.
