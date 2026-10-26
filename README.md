# plasticrecomp

A local, private test of static recompilation for Army Men II (The 3DO Company, 1999) with
[pcrecomp](https://github.com/sp00nznet/pcrecomp). Unofficial; not affiliated with or endorsed
by Take-Two Interactive, 2K Games or The 3DO Company. "Army Men" is Take-Two's trademark and is
used here only to say which game this is.

## Rules

1. **Bring your own copy.** The input is the owner's own retail disc: the English CD's files,
   copied by the owner from their disc (`G:\Licensed copies\Army Men II ENG`, see
   `docs/PROVENANCE.md`). `game/` is a local working copy of that folder, because the game writes
   `Options.cfg` and its saves into its working folder. It is gitignored and never shared.
2. **The lifted C never leaves this machine.** It is a derivative of the original program, so
   `src/recomp/gen/`, `work/` and every build output are gitignored, and nothing built from them
   is ever distributed.
3. **No copy protection is bypassed.** The lift is of the retail exe as it shipped, CD check
   included. That check runs when SINGLE PLAYER or BOOT CAMP is chosen and wants the disc in a
   drive. pcrecomp's DRM tools are not used.
4. **Run `audit_repo.py` before anything is shared.**
   `python third_party/pcrecomp/tools/audit_repo.py .` must report clean.

## Layout

| path | what | in git |
|---|---|---|
| `src/host/main.c` | the host: maps the guest at 0x00400000 and runs it on pcrecomp's native32 | yes |
| `scripts/` | lift scoring, compile check, host build, run and screenshot tools | yes |
| `third_party/pcrecomp/` | pcrecomp checkout (MIT), commit in `NOTES.md` | no |
| `third_party/cnc-ddraw/` | cnc-ddraw v7.1.0.0 (MIT), the windowed DirectDraw wrapper copied into `game/` | no |
| `src/recomp/gen/`, `work/`, `build/` | lifted C, analysis output, objects and the host exe | no |
| `game/` | working copy of the disc's files | no |

## Build and run

```bat
.venv\Scripts\python.exe scripts\run_lift.py game\ArmyMen2.exe src\recomp\gen --report work\table_arms.json
scripts\build_host.bat
build\plasticrecomp.exe --watchdog 30
pwsh scripts\boot_check.ps1          (run in the background, screenshots at 5/12/20/30 s)
```

`scripts/run_lift.py` is pcrecomp's default pipeline plus the jump-table arms it misses
(see `NOTES.md`); plain `python -m tools` output crashes in memmove. It also records which exe it
lifted (`recomp_target.h`), and the host refuses to run any other: lift the exe you own, retail or GOG.

Host options:

| option | what |
|---|---|
| `--game DIR` | the game folder (default `game`) |
| `--watchdog S` | report where the guest is after S seconds, then end the run |
| `--dry` | map and bind only |
| `--imports` | with `--watchdog`: where each USER32/GDI32/DINPUT import slot really points |
| `--no-route` | leave USER32 as native32 bound it, skipping cnc-ddraw's window and mouse hooks |
| `--route-gdi` | also route GDI32 (breaks movie colours; for experiments) |
| `--raw-mouse` | the game gets DirectInput's raw mouse counts, as in 1999 (default: it follows the Windows cursor) |
| `--mouse-log` | log the game's mouse device calls and each cursor correction |
| `--dplay-check` | create the game's DirectPlay object and list its connections, then exit |

First-time setup for any owner (disc, GOG or Steam): `pwsh scripts\setup.ps1 -From <game folder>`.
It copies the game into `game\`, puts cnc-ddraw in place, lifts that exe and builds the host. Without
`-From` it looks up GOG's and Steam's install records.

See `NOTES.md` for what was measured.
