# plasticrecomp

A local, private test of static recompilation for Army Men II (The 3DO Company, 1999) with
[pcrecomp](https://github.com/sp00nznet/pcrecomp). Unofficial; not affiliated with or endorsed
by Take-Two Interactive, 2K Games or The 3DO Company. "Army Men" is Take-Two's trademark and is
used here only to say which game this is.

The original 1999 code, lifted to C, runs natively on a 32-bit host. The host adds:
- its own DirectDraw and a Direct3D 11 presenter, in a resizable window;
- a mouse that follows the Windows pointer;
- a HUD that scales separately from the battlefield;
- a battlefield that fills the window, however big, with zoom (Plus / Minus).

Where the work stands: `docs/HANDOFF.md`. What was measured: `NOTES.md`. The resolution plan:
`docs/R_hires.md`.

## Rules

1. **Bring your own copy.** The input is the owner's own retail disc: the English CD's files,
   copied by the owner from their disc (`G:\Licensed copies\Army Men II ENG`, see
   `docs/PROVENANCE.md`). `game/` is a local working copy of that folder, because the game writes
   `Options.cfg` and its saves into its working folder. It is gitignored and never shared.
2. **The lifted C never leaves this machine.** It is a derivative of the original program, so
   `src/recomp/gen/`, `work/` and every build output are gitignored, and nothing built from them
   is ever distributed.
3. **No copy protection is bypassed.** The lift is of the retail exe as it shipped, CD check
   included. That check runs when SINGLE PLAYER or BOOT CAMP is chosen and wants the disc (or an
   image of the owner's disc, mounted) in a drive. pcrecomp's DRM tools are not used, and an exe
   wrapped in Steam's DRM is refused.
4. **Run `audit_repo.py` before anything is shared.**
   `python third_party/pcrecomp/tools/audit_repo.py .` must report clean.

## Layout

| path | what | in git |
|---|---|---|
| `src/host/main.c` | the host: maps the guest at 0x00400000 and runs it on pcrecomp's native32; import shims, options, diagnostics | yes |
| `src/host/vddraw.c` | the virtual DirectDraw (COM objects, DIB-section surfaces), the window, the presenter thread and the HUD/battlefield layout | yes |
| `src/host/present.c` | the D3D11 presenter: palette lookup in the shader, nearest/sharp-bilinear, regions at different scales | yes |
| `src/host/hires.c` | step 4: host wrappers that run in front of lifted functions (map load, HUD create, panel update, HudPaint, cursor draw, edge scroll, state requests) and make the battlefield as big as the window: the game's screen geometry, zoom, the HUD column | yes |
| `src/host/mouse.c` | the game's DirectInput mouse, fed from the Windows cursor | yes |
| `scripts/` | lift (`run_lift.py`), setup for disc/GOG/Steam owners (`setup.ps1`), host build, presenter test, run, screenshot, key and hashing tools, the unattended Boot Camp run (`auto_bootcamp.ps1`, `compare_shots.py`) | yes |
| `docs/` | `HANDOFF.md`, `R_hires.md` (resolution research and plan), `PROVENANCE.md` (the disc copies) | yes |
| `third_party/pcrecomp/` | pcrecomp checkout (MIT), commit in `NOTES.md` | no |
| `third_party/cnc-ddraw/` | cnc-ddraw v7.1.0.0 (MIT), used only with `--cnc-ddraw` | no |
| `src/recomp/gen/`, `work/`, `build/` | lifted C, analysis output, objects and the host exe | no |
| `game/` | working copy of the disc's files | no |

## Build and run

From `C:\plasticrecomp`, in PowerShell:

```powershell
.venv\Scripts\python.exe scripts\run_lift.py game\ArmyMen2.exe src\recomp\gen --report work\table_arms.json
cmd /c scripts\build_host.bat
cmd /c "C:\plasticrecomp\build\plasticrecomp.exe --game C:\plasticrecomp\game > C:\plasticrecomp\build\user_run.log 2>&1"
```

- **Don't relink while the game runs.** Windows locks a running exe, so `build_host.bat` fails to
  link while the game is open.
- **Only one copy at a time.** The game allows a single copy (its mutex `ArmyMenMutex`), and the host
  says so if one is already running.
- **First-time setup for any owner** (disc, GOG or Steam): `pwsh scripts\setup.ps1 -From <game folder>`.
  It copies the game into `game\`, puts cnc-ddraw in place, lifts that exe and builds the host. Without
  `-From` it looks up GOG's and Steam's install records. On the owner's PC always pass `-From`,
  because it would otherwise find a GOG install the owner does not want used.
- **`scripts/run_lift.py`** is pcrecomp's default pipeline plus the jump-table arms it misses. Plain
  `python -m tools` output crashes in memmove. It also records which exe it lifted
  (`recomp_target.h`), and the host refuses to run any other.
  It also renames the lifted definitions of the functions the host wraps (`WRAP` in the script: map load, HUD create,
  HudPaint, cursor draw, edge scroll, state request) to `sub_X_lifted`
  and writes `recomp_wrap.h` (their split entries), which `src/host/hires.c` uses: re-lift after pulling.

## In the game

| key | what |
|---|---|
| Shift+Plus / Shift+Minus | in a mission: HUD smaller / larger, in half steps from 1x up to the window's fit size |
| Plus / Minus | in a mission: battlefield zoom in / out through 1x, 1.5x, 2x, 3x, 4x (1x: one game pixel is one screen pixel, so a 1080p window shows 1920x1050 pixels of map) |
| Escape | as in the original (skips movies, opens the mission menu) |

## Host options

| option | what |
|---|---|
| `--game DIR` | the game folder (default `game`) |
| `--watchdog S` | report where the guest is after S seconds, then end the run |
| `--dry` | map and bind only |
| `--cnc-ddraw` | draw through the game folder's ddraw.dll (cnc-ddraw) instead of the host's virtual DirectDraw and D3D11 presenter |
| `--multicore` | don't pin the process to one CPU core (movies then stall at random frames: smackw32's audio races) |
| `--raw-mouse` | the game gets DirectInput's raw mouse counts, as in 1999 (default: it follows the Windows cursor) |
| `--mouse-log` | log the game's mouse device calls and each cursor correction |
| `--movie-log` | every 2 s: movie frames decoded, SmackWait go/hold, frames presented, foreground |
| `--hires-log` | every 2 s in a mission: view size, zoom, game frame time (Flip to Flip), presenter fps, the camera's eye (0x514E0C) |
| `--dd-trace` | log the virtual DirectDraw's surfaces and window messages |
| `--imports` | with `--watchdog`: where each USER32/GDI32/DINPUT import slot really points |
| `--no-route` | with `--cnc-ddraw`: leave USER32 as native32 bound it, skipping cnc-ddraw's hooks |
| `--route-gdi` | with `--cnc-ddraw`: also route GDI32 (breaks movie colours; for experiments) |
| `--native-trace`, `--callbacks` | native32's per-call traces (huge) |
| `--dplay-check` | create the game's DirectPlay object and list its connections, then exit |

Test switches (environment variables, for diagnosis only):
- `PLASTIC_HIRES=0`: the battlefield stays 640x480 as in the original (the old layout; `1` turns the feature on).
  `PLASTIC_ZOOM=n`: the first zoom index (0..4 = 1x, 1.5x, 2x, 3x, 4x).
- `PLASTIC_DUMP_LOAD=dir`: while a mission loads, write the presented frame as `dir\load_n_WxH_stateS.bmp` every 100 ms.
- `PLASTIC_FORCE_LAYOUT=1`: the mission layout applies everywhere.
- `PLASTIC_UI_HALVES=n`: preset the UI scale to n/2.
- `PLASTIC_NO_PRESENT=1`, `PLASTIC_NO_SUBCLASS=1`, `PLASTIC_NO_RESTYLE=1`, `PLASTIC_NO_DPI=1`:
  switch one piece of the virtual path off.
- `PLASTIC_PRESENT_WARP=1`, `PLASTIC_PRESENT_BLT=1`: presenter fallbacks.
- `PLASTIC_PRESENTER_PRIO=0`: the presenter thread keeps normal priority (default: one step above, so it preempts the
  game on their shared core and holds 60 fps while the game loop spins at ~2900 Flips/s; with normal priority it fell to ~30 fps
  while the camera scrolled). `PLASTIC_FLIP_SLEEP=ms`: the game thread sleeps that long after each Flip (a throttle; off).
- `HOST_ONLY=1 cmd /c scriptsuild_host.bat`: rebuild only the host files, reusing the lifted C's objects.
- `PLASTIC_LOGICAL_PROBE=WxH`: once the game has made its world surface, set the primary, back buffer and
  world surface's logical size to W x H (within the capacity), to check that menus, movies and the HUD
  still show the 640x480 picture.
- `PLASTIC_SCRIPT="t:x,y[,click];..."`: drives the game's cursor with no real mouse. From the first
  step whose time `t` (seconds since the host process started) has passed, the cursor target is game
  pixel x,y (the game's frame: 640x480 in menus; in a mission with the hires layout the frame is bigger, the
  battlefield at 0,21), whichever window has the foreground. `t:@x,y` is a window client pixel instead,
  mapped as the Windows cursor would be (negative: from the right / bottom edge, so `@-1,500` is the
  right edge and exercises the edge snapping). With `,click` a left click (button 0 down, then up on
  the next poll) follows once the cursor has arrived; `,rclick` is the right button.
- `PLASTIC_KEYS="t:KEY;..."`: holds a DirectInput key down for 3 keyboard polls from time `t`. `KEY` is a
  DIK scancode (decimal or `0x..`) or ESC, ENTER, SPACE, MINUS, EQUALS, F1, P. ESC skips the intro movies.
  ZOOMIN / ZOOMOUT are no keys: they step the battlefield zoom once, like Plus / Minus in the window.

Unattended test run (needs the disc image mounted at `H:` and no `plasticrecomp.exe` running):

```powershell
pwsh scripts\auto_bootcamp.ps1 -Tag before -Duration 80 -Shots 12,28,40,60,75
python scripts\compare_shots.py before after      # Pillow; compares build\screens\before_*s.png with after_*s.png
```

`scripts\auto_bootcamp.ps1` starts the game with a watchdog, skips the movies (ESC at 6 s and 20 s),
clicks BOOT CAMP (game pixel 307,142), clicks OK on the "Welcome to Boot Camp!" box (478,225), closes
the instruction sheet with Space, parks the pointer, takes screenshots at the `-Shots` times into
`build\screens\<Tag>_<time>s.png`, and kills the game at `-Duration`. The mission is on screen about 35 s
after launch. Two runs of the same build give pixel-identical screenshots, apart from a few animated pixels.
