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
(see `NOTES.md`); plain `python -m tools` output crashes in memmove.

See `NOTES.md` for what was measured.
