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
| `src/host/present.c` | the D3D11 presenter: palette lookup in the shader, nearest/sharp-bilinear, area average when a region is shrunk (zoom below 1x), regions at different scales | yes |
| `src/host/hires.c` | step 4: host wrappers that run in front of lifted functions (map load, HUD create, panel update, HudPaint, cursor draw, edge scroll, state requests) and make the battlefield as big as the window: the game's screen geometry, zoom, the HUD column | yes |
| `src/host/config.c` | `plastic.ini`: read at start, written on exit |  yes |
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
  HudPaint, cursor draw, edge scroll, state request, the camera) to `sub_X_lifted`
  and writes `recomp_wrap.h` (their split entries), which `src/host/hires.c` uses: re-lift after pulling.

## In the game

| key | what |
|---|---|
| Shift+Plus / Shift+Minus | in a mission: HUD smaller / larger, in half steps from 1x up to the window's fit size |
| Plus / Minus | in a mission: battlefield zoom in / out through 0.25x, 0.5x, 1x, 1.5x, 2x, 3x, 4x (1x: one game pixel is one screen pixel, so a 1080p window shows 1920x1050 pixels of map; 0.5x shows four times the map, in a 3840x2160 view that the presenter shrinks to the window). The view never exceeds 3840x2160 (the surfaces' size) or the map, so a window can show 0.5x at most on 1080p (3840x2160 = 2 x 1920x1080): there 0.25x asks for more than there is room for, and the zoom is raised in both directions to what fits (a 1280x960 window: 0.44x, a 2560x1440 one: 0.67x), so the window stays filled. The title bar names the zoom you really get (window / view) for 2 s, and F3's line carries it with the view's size. A step that would show the same zoom as the present one is skipped (Minus on a 1080p window at 0.5x says "furthest out" and does nothing), so no key press is dead. Zooming out shrinks the game's pointer along with the map (it is drawn into the battlefield): at 0.25x it is a few pixels |
| Escape | as in the original (skips movies, opens the mission menu). The mission's own screens (game menu, audio, controls, save, load, the HQ message, the instruction sheet and F1's map) are shown as the 640x480 picture they are built for, centred at the HUD's scale (Shift+Plus / Minus) over the battlefield as it was when they opened, filling the window at the zoom in play with the HUD around it. Small ones (the menu, the HQ box, the sheet) leave the world visible around their widgets; the full-screen ones (Controls, Audio, F1's map) dim it. The pointer works over the whole window and is held to the picture |
| F12 (Shift+F12 backwards) | scaling mode, for the battlefield and the HUD alike: sharp-bilinear (default), nearest (whole-number scales only: the 640x480 picture of the menus, movies and dialogs and the HUD at the largest whole multiple that fits, black bars for the rest; the battlefield at a whole zoom (1x, 2x, 3x, 4x) in whole-pixel steps, up to zoom-1 black pixels at a window edge; the 1.5x zoom and a capped view stay smooth fit), smooth bilinear, scanlines (sharp-bilinear with each source row dimmed towards its edges; visible from 2x scale up). The title bar names the mode for 2 s |
| F7 | HUD layout: classic (the panel column at the right) / bottom console (StarCraft style, over the battlefield along the bottom edge: minimap, one squad panel (header over the member grid, or the selected unit's portrait), weapons over commands, the edge strip stays vertical at the right; it scales with the UI scale (Shift+Plus / Minus) up to 40 % of the window's height, and a window smaller than 640x480 or a different HUD (multiplayer) keeps the classic column; the camera is told the view is shorter by the console's height, so the map's bottom edge stays above it; details in `docs/R_step4_plan.md` section 12). `hud_layout` in plastic.ini, `--hud-layout`, `PLASTIC_HUD_LAYOUT`. Default classic |
| F8 | mouse confine on / off: in a mission, the Windows cursor is kept inside the window's client area (ClipCursor). It is let go in the menus, in the mission's dialogs (game menu, audio, controls, the sheet), when the window loses the foreground (Alt-Tab, a click elsewhere), when minimised, and when the game ends. On by default; the title bar says "Mouse confine on/off" |
| F9 | edge scrolling on / off. Off: the game's cursor is held 4 px inside its clamp rectangle, where the edge scroll (3 px) cannot fire, so the map scrolls only by the minimap and by following Sarge (the game has no keyboard scrolling). On by default |
| F11, Alt+Enter | borderless fullscreen on the window's monitor and back; leaving restores the window's rect (maximised or not). The battlefield re-fits the new size |
| F3 | FPS readout in the title bar: presenter fps, frame time (average, max), the game loop's rate and, in a mission, the effective zoom and the view's size. (Borderless fullscreen has no title bar, so the notices and this line are not visible there) |
| F10, PrintScreen | screenshot: a PNG of the picture as presented (palette lookup and scaling done, window-sized) in `game\screenshots\plastic_<date>_<time>.png`; the title bar names the file |

The host's keys never reach the game: they are cleared from the DirectInput keyboard state the game reads
(`src/host/mouse.c`, `swallow_host_keys`). The game's own keys are DIK 1 (Escape), 2..9, 0x0F (Tab), 0x1C
(Enter), 0x1D, 0x2A, 0x36, 0x39 (Space), 0x3B (F1), the arrows, and the bindings in `game\Options.cfg`
(shipped: W S A D Q E C F, Alt, Space, Shift, Tab, ` X, R G T Y H, Backspace, F1, Esc; the Controls screen lets
the player rebind). No shipped binding or fixed key collides with F3, F8, F10, F11, F12, PrintScreen, Plus or
Minus, so nothing is lost, but those keys cannot be bound in the Controls screen. Alt+Enter hides its Enter
(Alt stays, it is WALK). Plus and Minus (also Shift+, and the numpad's) are the host's only while no text field has
the focus (chat, names, save names) and no mission dialog is up, so they can be typed and bound there.

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
| `--hires-log` | every 2 s in a mission: view size, zoom, game frame time (Flip to Flip), presenter fps, the camera's eye (0x514E0C), the mission state; and a `limits` line: row pools A (decals, cap 450) and B (troopers, vehicles, roaches, cap 90) with their evictions, SEQ evictions, the depth list's peak (500) and the dirty list's (500), the object table |
| `--dd-trace` | log the virtual DirectDraw's surfaces and window messages |
| `--imports` | with `--watchdog`: where each USER32/GDI32/DINPUT import slot really points |
| `--no-route` | with `--cnc-ddraw`: leave USER32 as native32 bound it, skipping cnc-ddraw's hooks |
| `--route-gdi` | with `--cnc-ddraw`: also route GDI32 (breaks movie colours; for experiments) |
| `--native-trace`, `--callbacks` | native32's per-call traces (huge) |
| `--dplay-check` | create the game's DirectPlay object and list its connections, then exit |
| `--fullscreen`, `--windowed` | start in borderless fullscreen / in a window (overrides `plastic.ini`) |
| `--scaling MODE`, `--vsync 0\|1` | start with that scaling mode (`sharp`, `nearest`, `smooth`, `scanlines`) / vsync (overrides `plastic.ini`) |
| `--ini FILE`, `--no-ini` | use that settings file / neither read nor write one |

### plastic.ini

Settings that persist, read at start and written when the game exits (a run that is killed writes
nothing). The host looks for `plastic.ini` next to `plasticrecomp.exe` first, then in the game folder,
and writes back to the file it read (or next to the exe if there was none). `key = value` lines; `#` or
`;` start a comment; unknown keys are ignored and kept.

```ini
zoom_x = 1            # battlefield zoom as a value: 0.25 0.5 1 1.5 2 3 4 (the nearest step)
zoom = 0              # the older builds' index (0..4 = 1x 1.5x 2x 3x 4x), kept for them; read only when zoom_x is absent
ui_scale = 0          # HUD scale in half steps (2 = 1x, 3 = 1.5x ...); 0 = follows the window
scaling = sharp-bilinear   # nearest | sharp-bilinear | smooth | scanlines (or 0..3)
fullscreen = 0        # borderless fullscreen
hires = 1             # 0: the battlefield stays 640x480 as in the original
vsync = 1             # 0: Present does not wait for the vertical blank (the 60 fps cap stays)
confine = 1           # F8: keep the mouse in the window while a mission is played
edge_scroll = 1       # F9: scroll the map when the mouse is at the window's edge
```

For one run, environment variables override the file (`PLASTIC_ZOOM`, `PLASTIC_UI_HALVES`, `PLASTIC_HIRES`,
`PLASTIC_SCALING`, `PLASTIC_FULLSCREEN`, `PLASTIC_VSYNC`, `PLASTIC_CONFINE`, `PLASTIC_EDGE_SCROLL`), and command-line flags override both.

Test switches (environment variables, for diagnosis only):
- `PLASTIC_HIRES=0`: the battlefield stays 640x480 as in the original (the old layout; `1` turns the feature on).
  `PLASTIC_ZOOM=z`: the first zoom, a value (0.25, 0.5, 1, 1.5, 2, 3, 4; the nearest step; it was an index 0..4 before the zoom-out steps).
- `PLASTIC_DUMP_LOAD=dir`: while a mission loads, write the presented frame as `dir\load_n_WxH_stateS.bmp` every 100 ms.
- `PLASTIC_FORCE_LAYOUT=1`: the mission layout applies everywhere.
- `PLASTIC_UI_HALVES=n`: preset the UI scale to n/2.
- `PLASTIC_NO_PRESENT=1`, `PLASTIC_NO_SUBCLASS=1`, `PLASTIC_NO_RESTYLE=1`, `PLASTIC_NO_DPI=1`:
  switch one piece of the virtual path off.
- `PLASTIC_PRESENT_WARP=1`, `PLASTIC_PRESENT_BLT=1`: presenter fallbacks.
- `PLASTIC_PRESENTER_PRIO=0`: the presenter thread keeps normal priority (default: one step above, so it preempts the
  game on their shared core and holds 60 fps while the game loop spins at ~2900 Flips/s; with normal priority it fell to ~30 fps
  while the camera scrolled). `PLASTIC_FLIP_SLEEP=ms`: the game thread sleeps that long after each Flip (a throttle; off).
- `HOST_ONLY=1 cmd /c scripts\build_host.bat`: rebuild only the host files, reusing the lifted C's objects.
- `PLASTIC_LOGICAL_PROBE=WxH`: once the game has made its world surface, set the primary, back buffer and
  world surface's logical size to W x H (within the capacity), to check that menus, movies and the HUD
  still show the 640x480 picture.
- `PLASTIC_CAP=WxH`: the largest battlefield is W x H (at least 640x480, at most 3840x2160, the default whatever the
  monitor: the zoom-out steps need it even on a 1080p screen; the surfaces are made for that, the window keeps its size). `PLASTIC_VIEW=WxH`: size the battlefield as if the
  window's client area were W x H (the presenter then scales it into the real window). Together they run a 4K view on any
  PC: `PLASTIC_CAP=3840x2160 PLASTIC_VIEW=3840x2160`. `PLASTIC_DUMP_VIEW=dir`: F10 / PrintScreen also write the battlefield
  as the game drew it, 1:1, to `dir\view_<n>_<w>x<h>.bmp` (a window cannot be bigger than the monitor, so this is how a 4K
  view is looked at). `PLASTIC_LOG_DRAWOBJ=1`: log the first 200 `DrawMapObjects` calls (the region, its recursion level).
  `PLASTIC_NO_DRAWFIX=1`: leave the game's own splitting of a crowded region as it is (see "Limits" in `docs/R_step4_plan.md`).
- `PLASTIC_HUD_PIX=1`: log what the bottom layout's squad probe counts (every 2 s). `PLASTIC_TEST_CLOSE=s`: from s seconds on the layout sees the HUD panel closed. `PLASTIC_NO_COVER=1`: the camera is not told about the console's height. `PLASTIC_DUMP_VIEW` also writes the HUD column 1:1 (`column_<n>.bmp`). In a scripted run (`PLASTIC_KEYS`) the keys reach the game even when the window is not in the foreground.
- `PLASTIC_DUMP_CORNER=dir`: while a mission dialog is up, its 640x480 picture as the game drew it (the see-through pixels magenta) goes to `dir\corner_<n>.bmp` about every 0.7 s (at most 80). `PLASTIC_BLT_STRETCH=1`: a Blt between rectangles of different sizes stretches again, as DirectDraw does (default: a 1:1 copy of the smaller, see "Pointer smear" in `docs/R_step4_plan.md` section 11).
- `PLASTIC_SCRIPT="t:x,y[,click];..."`: drives the game's cursor with no real mouse. From the first
  step whose time `t` (seconds since the host process started) has passed, the cursor target is game
  pixel x,y (the game's frame: 640x480 in menus; in a mission with the hires layout the frame is bigger, the
  battlefield at 0,21), whichever window has the foreground. `t:@x,y` is a window client pixel instead,
  mapped as the Windows cursor would be (negative: from the right / bottom edge, so `@-1,500` is the
  right edge and exercises the edge snapping). With `,click` a left click (button 0 down, then up on
  the next poll) follows once the cursor has arrived; `,rclick` is the right button.
  A time `t` is seconds since the host process started, or relative to the mission's modal screens: `s24+1` is 1 s after
  the mission's sub-state 24 (0x00511DBC; Boot Camp's HQ box) was first seen, `s22+3` after 22 (the instruction sheet,
  F1's map), `s23` the game menu, `s27` audio, `s28` controls, `g+N` after 33 (play; seen first at the load, so
  rather use the state of the last dialog). The sub-states are cleared by each map load, and an event whose
  state has not come yet waits (a script is ordered), which makes a run independent of how long the load took.
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
