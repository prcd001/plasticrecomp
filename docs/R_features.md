# R_features: host-layer feature ideas from comparable projects (research, 2026-10-26)

Scope: things the host (`src/host/main.c`, `vddraw.c`, `present.c`, `mouse.c`) can add WITHOUT editing lifted
code or touching the CD check. Zoom/HUD scale are already planned in `R_hires.md` and not repeated, except
where a project suggests a refinement. Effort S = under a day, M = a few days, L = a week or more.
Evidence tags: [V] page fetched and read this session; [K] from background knowledge, URL given but not re-read.

## Prior art actually read ([V])

- redalert2-recomp (sp00nznet): D3D11 presenter in its own window, F11/Alt+Enter borderless fullscreen, F12 cycles
  sharp-bilinear / smooth / CRT / nearest / whole-multiple, F10 settings menu saved to an ini, "bars" beside 4:3 menus
  drawn as a blurred copy of the menu, modern click scheme, scripted playtests, `--original` comparison mode.
  https://github.com/sp00nznet/redalert2-recomp
- tiberiansun-recomp: same presenter family, plus `--headless`, `--watchdog`, `--seed` (identical skirmishes),
  `--record out.mp4` (ffmpeg), scripted input (clicks, drags, keys), `TS_PROFILE=1`, `--mute`, mod folders overlaying
  the install. https://github.com/sp00nznet/tiberiansun-recomp
- wolf3d-recomp: scaling modes + "natural/vivid" colour look, XInput gamepad mapped to the game's keys, mouse turn
  with sensitivity, headless deterministic timing, timed screenshots, MP4 + audio recording, call tracing.
  https://github.com/sp00nznet/wolf3d-recomp
- cnc-ddraw: borderless/windowed/fullscreen, Alt+Enter, saves window pos/size, FPS limiter, vsync, mouse sensitivity
  scaling, libretro/GLSL shaders, Ctrl+Tab or Right-Alt+Right-Ctrl unlocks the cursor, Alt+PageDown maximise.
  https://github.com/FunkyFr3sh/cnc-ddraw
- DxWnd: per-game profiles, hotkeys for fullscreen toggle / screenshot / pause / mouse lock, forced palette handling.
  https://www.dxwnd.com/
- OpenRCT2 options: window scale factor, borderless, uncap FPS (in-between frames above the 40 Hz sim), show FPS,
  vsync, edge scrolling toggle, zoom to cursor, trap cursor, remappable shortcuts, mute on focus loss.
  https://docs.openrct2.io/en/latest/setup/options.html
- devilutionX: FPS no longer tied to the 20 Hz sim, gamepad, touch, FPS counter, optional HUD bars, config file.
  https://github.com/diasurgical/devilutionX and https://www.gog.com/forum/diablo/devilutionx_100_gamepad_and_60fps_happy_new_year
- dgVoodoo2 (VOGONS threads, anecdotal): 8-bit palette path, forced 4:3, CRT-like scaling mode, resampling filter choice,
  vsync as the frame limiter. https://www.vogons.org/viewtopic.php?p=497833
- libretro slang-shaders (pixel-art-scaling, crt, interpolation, scanlines): Vulkan-GLSL `.slang`, needs SPIRV-Cross to
  reach HLSL; hand-porting two or three is easier. https://github.com/libretro/slang-shaders
- Dear ImGui: Win32 + DX11 backends, MIT. https://github.com/ocornut/imgui

## Ranked ideas (top 5 = build first)

### 1. [TOP] Scaling-mode cycle (F12): sharp-bilinear, integer-nearest, smooth, CRT, plus "pixel-perfect world / smooth HUD"
- (a) Cycle the existing presenter's filter live; add integer-only scale with letterbox, a CRT/scanline mode, and a
  bars-beside-menu fill (blurred copy of the 4:3 picture instead of black).
- (b) redalert2-recomp, wolf3d-recomp, cnc-ddraw shaders, dgVoodoo2 (links above).
- (c) Presenter-only. Filtering already happens after the palette lookup in the shader, so every mode is
  palette-correct for free; xBR/ScaleFX are NOT worth it on a 256-colour sprite game with dithered shadows (they need
  RGB neighbourhood logic and smear indexed dither), prefer sharp-bilinear and a light scanline pass.
- (d) S. (e) Low. (f) Yes: `present_test.c` plus `screenshot.ps1`, compare hashes per mode overnight.

### 2. [TOP] Screenshot hotkey, MP4 capture and a headless/scripted test mode
- (a) F9/PrintScreen saves the presented frame (PNG) to `game/screens/`; `--record out.mp4` pipes frames to ffmpeg;
  `--script file` injects clicks/keys at timed offsets; `--headless` renders without a visible window.
- (b) tiberiansun-recomp and wolf3d-recomp (scripted input, `--record`, `--headless`, timed screenshots).
- (c) Host-only: the presenter already owns the final image; input injection goes through `mouse.c` and the window proc.
- (d) M (S for screenshot alone). (e) Low; ffmpeg is an optional external binary, nothing redistributed.
- (f) This IS the overnight test harness. It makes every later item (zoom, HUD) verifiable without the owner.

### 3. [TOP] Borderless fullscreen (F11 / Alt+Enter), vsync + frame pacing, persistent `plastic.ini`
- (a) Borderless fullscreen toggle, vsync on/off, presenter FPS cap choice (60/refresh/uncapped), window pos/size and
  HUD scale and filter remembered across runs, one ini for all host options now scattered over env vars and flags.
- (b) redalert2-recomp (F11, F10 menu to ini), cnc-ddraw (savesettings), OpenRCT2 (vsync, FPS options).
- (c) Presenter + window code in `vddraw.c`; ini read in `main.c`. Keep the single-core and 60 fps movie fix as the
  default; only offer "refresh-rate" pacing for menus/missions, test movies separately.
- (d) S-M. (e) Medium: movie stall bug is pacing-sensitive (see NOTES), so any pacing change must re-run `--movie-log`.
- (f) Yes for toggling and ini round-trip by screenshot; pacing needs a long `--movie-log` run (unattended OK).

### 4. [TOP] Mouse confine/unlock, edge-scroll options and focus handling
- (a) ClipCursor to the client area while playing, Ctrl+Tab style unlock hotkey, option to disable edge scroll or widen
  its trigger band, mute/pause on focus loss, optional mouse sensitivity scale for `--raw-mouse`.
- (b) cnc-ddraw (unlock hotkeys), DxWnd (mouse lock hotkey), OpenRCT2 (trap cursor, edge scroll toggle).
- (c) Input-only (`mouse.c`, window proc). Edge scroll in the game fires within 3 px of the cursor-clamp rect, so a
  host option to hold the cursor 4 px inside the window gives a "no accidental scroll" mode without touching the game.
- (d) S. (e) Low; interacts with the pending world/UI cursor-space split, so land it before zoom input work.
- (f) Partly: ClipRect and cursor position can be asserted by a script (`GetClipCursor`); feel needs the owner.

### 5. [TOP] FPS/frame-time overlay, crash handler and structured log
- (a) F3 toggles an FPS/ms/guest-state overlay (drawn in the presenter, no ImGui needed); an unhandled-exception
  filter writes `crash.txt` with guest EIP, nearest lifted function and the last N logged events plus a minidump.
- (b) devilutionX FPS counter, OpenRCT2 "show FPS", pcrecomp recomp32 crash report; tiberiansun-recomp `TS_PROFILE`.
- (c) Host-only (`main.c` exception filter; HANDOFF already lists guest C++ exceptions as unhandled).
- (d) S-M. (e) Low. (f) Yes: force a crash with a test switch, check the file; overlay by screenshot.

### 6. Palette-aware accessibility: colourblind team colours, contrast and "vivid" look
- (a) A 256-entry palette remap or LUT applied inside the presenter's palette upload (e.g. separate tan/green/blue/grey
  teams by shifting hue in Lab), plus brightness/gamma and a "natural vs vivid" toggle.
- (b) wolf3d-recomp (natural/vivid look); colourblind filters are common in OpenTTD [K] https://www.openttd.org/ and
  Factorio-style games [K]; indexed output makes this nearly free here.
- (c) Presenter-only: transform the palette table on upload, game never sees it. Risk: movies and menus share the palette
  path, so key it on game state (`0x00511DA4`) or apply to all.
- (d) S. (e) Low (cosmetic). (f) Yes: screenshot diff on a fixed mission frame, plus a unit test on the LUT.

### 7. Dear ImGui overlay (F10): settings menu, hotkey list, log pane
- (a) In-game window for filter, fullscreen, vsync, HUD scale, volume, cursor options, diagnostics toggles.
- (b) redalert2-recomp F10 menu; ImGui has Win32 + DX11 backends (MIT). https://github.com/ocornut/imgui
- (c) Presenter hosts the draw; input must be swallowed while the overlay is open. Costly parts are the C host
  (ImGui is C++, needs a small C wrapper or cimgui) and the 32-bit build.
- (d) M. (e) Medium: C/C++ boundary and 32-bit ImGui build; do #3's ini first, menu second. (f) Screenshot diffs only.

### 8. Gamepad as pointer + buttons (XInput)
- (a) Left stick moves the cursor, right stick scrolls (as edge push), A/B = left/right click, triggers/bumpers = group
  keys, Start = Escape. Steam Deck friendly.
- (b) wolf3d-recomp (XInput mapped to game keys), devilutionX full gamepad. SDL2 alternative: SDL_GameController
  with mapping db https://wiki.libsdl.org/SDL2/CategoryGameController
- (c) Input-only: synthesise cursor moves and key events through `mouse.c`; XInput is a system DLL, no new dependency.
- (d) M. (e) Low-medium: cursor-space split from zoom work must be stable first. (f) No for feel; a fake-pad test can
  check cursor movement and button events overnight.

### 9. Fast-forward / game speed control
- (a) Hotkeys for 0.5x/1x/2x/4x mission speed; skip movies faster than Esc.
- (b) cnc-ddraw `maxgameticks` (limits ticks) https://github.com/FunkyFr3sh/cnc-ddraw; OpenTTD fast-forward [K]
  https://www.openttd.org/; OpenRA game speed setting [K].
- (c) Host import shim: scale the time source the guest reads (`timeGetTime`/`GetTickCount`/QPC shims in `main.c`).
  Does NOT touch lifted code, but game-time vs audio-time skew can break Smacker audio (see single-core note) so scale
  only inside missions, never during movies.
- (d) M. (e) Medium-high: unknown timers (DirectSound, smackw32) may not follow. (f) Yes: measure mission ticks vs wall.

### 10. High-refresh presentation / camera smoothing
- (a) Present at the monitor refresh and (optionally) interpolate camera scroll between game frames.
- (b) OpenRCT2 "uncap FPS" generates in-between frames, devilutionX decoupled FPS from the 20 Hz sim [V above].
- (c) Unit interpolation is not possible host-side (dirty-rect redraw); only a sub-pixel pan of the composed world layer,
  and only once R_hires step 4 exists.
- (d) L. (e) Poor payoff for the risk: unit motion stays at the game rate. Defer. (f) Frame diffs only.

### 11. Audio quality and music replacement
- (a) Better resampling/mixing in the host's DirectSound, optional replacement of music with files or a soundfont.
- (b) wolf3d-recomp (ymfm OPL, SDL audio), tiberiansun-recomp (SDL2 audio on Linux, `--mute`); source of music in this
  game (CD audio vs MIDI vs Smacker) is not established in the repo notes, so check what the lifted exe calls (MCI?
  waveOut? DirectSound) before scoping. The README only mentions smackw32's audio threading.
- (c) Host import shims if audio goes through winmm/dsound imports; nothing if it all goes through smackw32.
- (d) M-L. (e) Medium (unknown audio path); keep original audio as default. (f) Headless: capture the mixed buffer to WAV and
  hash it; subjective quality needs the owner.

### 12. Replay / quicksave of a run
- (a) Record host-level input (cursor, buttons, keys) with the frame counter, replay it for regression tests or sharing.
  True save-states (snapshot of guest memory and thread state) are the heavy variant.
- (b) OpenRA records orders for replays [K] https://github.com/OpenRA/OpenRA; tiberiansun-recomp `--seed` makes runs
  identical; deterministic input logs are the standard RTS technique.
- (c) Input log is host-only but only replays identically if the game's RNG and timer inputs are fixed: needs `rand`/time seed
  control through import shims. A memory snapshot is possible in principle (single address space at 0x00400000)
  but threads, DirectSound and Smacker state make it fragile.
- (d) Input log + seed M; save-state L. (e) High for save-state. Game's own saves already exist. (f) Replay: yes, ideal
  overnight regression; save-state: hash after restore.
## Considered and not recommended (yet)

- xBR/ScaleFX/HQx: poor fit for dithered 256-colour art; a light CRT/scanline mode (#1) is the cheap compromise.
- Linux/Steam Deck: D3D11 should run under Wine/Proton (DXVK), untested; not checkable overnight here.
  tiberiansun-recomp notes `--record` fails under Wine [V].
- Remappable game keys: only host hotkeys (F-keys, Shift+/-) in the ini for now.
- Widescreen movies: not possible without re-encoding the owner's files; #1 covers smooth scaling.

Order: #2 first (it makes the rest verifiable unattended), then 1, 3, 4, 5, 6. The top 5 are host-only and leave the
lifted C and the CD check untouched.
