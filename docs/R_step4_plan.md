# Step 4 plan: battlefield at window size / zoom (architect, 2026-10-27)

Addresses were checked against `src\recomp\gen` by the architect. Items marked UNVERIFIED were not.

## 0. Main decision: no 640x480 swap around HudPaint/HudUpdate; the HUD gets its own column

Why a swap fails:
- HudUpdate 0x00414370 calls the HUD objects' update methods (vtable+8 on [0x4FCF00], [0x4FCF54], [0x4FCF4C]),
  then the world-mouse handler sub_00413E70 (PointInRect(BLIT_RECT) at 0x413EBD), then tail-jumps into 0x412190.
  The lifter inlined that tail (sub_00414370 shares 53 instructions with sub_00412190). A swap around HudUpdate breaks world clicks.
- One cursor value cannot be both a HUD coordinate and a world coordinate: the cursor is int32 at 0x485464/68; its int16 copy
  at 0x48546C/6E is what every hit test and edge scroll reads.

What the game already does: the panel anchors itself to SCREEN_W:
- Panel constructor sub_00418FB0: x = SCREEN_W - width (minus 16 in single player), y = 0x15, then BLIT_RECT.right = x (0x0041904C).
- Second right-hand piece, constructor sub_004195B0: x = SCREEN_W - width.
- Panel update sub_004193C0: slide clamps to SCREEN_W - sub_0041A170(); writes BLIT_RECT.right at 0x0041941D and 0x00419475.
- Top strip ([0x4FCF00], ctor sub_00417580) never reads SCREEN_W: fixed (0,0)-(640,21).

Layout in game space (the back buffer):
- Top strip [0,640) x [0,21).
- World [0,Ww) x [21, 21+Wh).
- HUD column [Wmax, A) x [21,480), A = Wmax + 160, Hc = max(480, 21+Hmax).
- Nothing overlaps, so hit tests, cursor clamp and edge scroll work unchanged. A and Hc stay constant for a whole mission.
  Zoom only changes Ww/Wh. No HUD object fields are ever patched.
- Real SCREEN_W = Ww, so the "screen" rect in world coords (0x514E14..20: SeqEvict, map drawing) is exactly the view.

Capacity, fixed once at process start from the monitor (physical pixels):
- Wmax = min(mon_w, 3840), Hmax = min(mon_h, 2160), both rounded up to a multiple of 16. CAP_W = A, CAP_H = Hc.
- Per zoom z: Ww = min(ceil(cw/z), Wmax, mapW-2), Wh = min(ceil(ch/z), Hmax, mapH-2), rounded to even.
  mapW = [0x514E00]-[0x514DF8], mapH = [0x514E04]-[0x514DFC]. Camera sub_0042B5A0 clamps the eye to [min+vw/2, max-vw/2-1],
  so a view over ~3968 flips. 3840 cap is safe.

## 1. Hook mechanism and geometry writes

(a) Indirect calls: RECOMP_ICALL/ITAIL call `recomp_lookup_manual(va)` first (third_party\pcrecomp\runtime\recomp32\recomp_types.h:1161,1189).
Host's version returns NULL (src\host\main.c:342). Return a host wrapper for 0x004193C0 (panel update; vtable slot 2 at .rdata 0x0046F94C;
vtable 0x46F944 = {0x419340, 0x4194E0 paint, 0x4193C0 update}); reached only through HudUpdate's `call [edx+8]` (0x00414383).
The wrapper is `void w(void)`, calls `sub_004193C0()` directly (gen\recomp_funcs.h), only writes MEM32, never touches g_esp/g_eax/flags.

(b) Direct calls (`RECOMP_CALL(sub_X)` = plain C call): rename in the lift. Add a post-pass in scripts\run_lift.py after
generate_recomp_files (line ~150): for each VA in WRAP = [0x0042C440, 0x00413480], rewrite the single definition line
`^void sub_X\(void\) \{` to `void sub_X_lifted(void) {`; add `void sub_X_lifted(void);` to recomp_funcs.h. The host then defines `sub_X`.
Assert exactly one definition per VA. Bodies untouched. Re-lift ~20 s.

Safety (dupcheck): no sharing: sub_004193C0, sub_00413480, sub_004143A0, sub_00424CA0, sub_00425EE0, sub_0042DA30, sub_00426F40.
sub_0042C440 is copied into 12 split entries (e.g. sub_0042C561), sub_00425300 into 4; none referenced by any call or data pointer (dead).
Add a recomp_lookup_manual trap that logs if any is reached. Do NOT wrap sub_0042BEA0, sub_00425300, sub_00424BF0/0041AC60.

Surface sizing:
- sub_0041AA10 runs once at startup (called from 0x0040B73E): SetDisplayMode(SCREEN_W,SCREEN_H) at 0x41AB2A, primary + back buffer
  (back buffer 0x4FE08C via GetAttachedSurface at 0x41ABD2), world surface 0x503100 = sub_0041B850(SCREEN_W,SCREEN_H,0x40,-1) at 0x41ABEA.
  Created at 640x480 before any hook, so the virtual DirectDraw must allocate them at capacity up front.
- sub_0042BEA0 (map load, called at 0x0042C4B5 inside sub_0042C440): tile cache 0x514E94 = 0x41B850(((W>>4)+2)*16, ((H>>4)+2)*16);
  tile counts at 0x514EA0/A4 (0x42C067). SCREEN_W/H must hold the maximum before map load.

Write sequence (game thread):
- M1. Wrapper of sub_0042C440 (map load; called at 0x0042550A in mission start sub_00425300): SCREEN_W 0x4852D8 = A; SCREEN_H 0x4852DC = Hc;
  SCREEN_CLIP 0x485310 = {0,0,A,Hc}; BLIT_RECT 0x485320 = {0,21,Wmax,21+Hmax}; vdd_set_logical(A,Hc); call original.
- M2. Wrapper of sub_00413480 (HUD create; also called from 0x0040ADAC in sub_0040A6B0): pre: save SCREEN_W, set SCREEN_W = A; call original;
  post: g_hud_right = MEM32(0x485328); restore BLIT_RECT.right = Ww; restore SCREEN_W.
- M3. Wrapper of sub_004193C0 (every mission frame, after edge scroll 0x425FA3 and before camera 0x42608C and draw):
  pre, on first mission frame or pending zoom/resize: compute Ww/Wh; SCREEN_W = Ww; SCREEN_H = 21+Wh; BLIT_RECT = {0,21,Ww,21+Wh};
  full redraw 0x512460 = 1 (read at 0x42DA3D, cleared at 0x42DB31; the camera recomputes the view every frame; ComposeFrame BltFasts the whole BLIT_RECT each frame).
  Around the original call: SCREEN_W = A; call; g_hud_right = MEM32(0x485328); BLIT_RECT.right = Ww; SCREEN_W = Ww.
- M4. Back to menus: at the start of each mouse poll cycle (mouse.c feed_events `g_cycle_start`, also runs in menus): if MEM32(0x511DA4) != 2
  and hires geometry is active, restore SCREEN 640x480, SCREEN_CLIP {0,0,640,480}, BLIT_RECT {0,21,624,480} (.data originals), vdd_set_logical(640,480).
  Safe because sub_00426F40 re-reads SCREEN_CLIP after each GetDeviceData.

Zoom keys: in keep_active (vddraw.c:~471) handle Plus/Minus without Shift: atomic pending zoom index over {1, 1.5, 2, 3, 4}; M3 applies it.
Keys still reach the game via DirectInput; the game's direct key tests use DIK 0x1C/0x39/0x1D/0x9D/0x3B/0x36/0x2A (state [0x5127C8], sub_00427450).
Its configurable bindings are UNVERIFIED: check Options.cfg for = and - before release.

Virtual DirectDraw (vddraw.c):
- new_surface (~line 99): allocate DIB at capacity pitch and height (+64 spare rows). s->w/h stay the logical size.
- dd_CreateSurface (~line 422): primary, its back buffer, and the first offscreen created after the primary (the world surface) get CAP_W x CAP_H.
- `vdd_set_logical(w,h)`: under g_lock set w/h on primary, back buffer and the surface at MEM32(0x503100). If that one lacks capacity, log and disable hires.
- Pitch is read on every Lock (0x502AD0) and by copy(), so changing it is safe.
- Logical w/h must grow in missions: copy() and clip_rect() clip blits to d->w/h (else HUD-column blits at x>=640 are cut).

## 2. HUD
- BLIT_RECT.right writers covered: 0x0041904C (inside sub_00418FB0, only via sub_00413480 at 0x413546) by M2; 0x0041941D/0x00419475 by M3.
  The panel's value goes to g_hud_right, which the presenter uses (0 = open, slide, closed). The world view never shrinks.
- HudPaint 0x004143A0 and HudUpdate 0x00414370 need no wrapper. Paint methods (0x4194E0, 0x419AC0) clip to SCREEN_CLIP 0x485310 (push at 0x419AD8), now [0,A).
  HUD drawing goes to draw target [0x507128] (set by sub_0041AC40).

## 3. Input
- vdd_cursor (vddraw.c:~697) maps the Windows cursor through the presenter's topmost region to frame pixels; with the section-4 regions it gives
  correct game-space pixels for all three pieces.
- Edge snapping in vdd_cursor, missions only (edge scroll sub_00424CA0 uses the int16 copy 0x48546C/6E: scrolls when x > CLIP.right-3 (0x424DF7),
  x<3, y > CLIP.bottom-3 (0x424E32), y<3): client x<=1 -> x=0; x>=cw-2 -> x=A-1; y<=1 -> y=0; y>=ch-2 -> y=Hc-1.
- Cursor clamp sub_00426F40 uses SCREEN_CLIP (0x426F65-0x426F9F), follows automatically. mouse.c:149-150 g_track clamp 639/479 should use SCREEN_CLIP (fallback only).
- The five PointInRect(BLIT_RECT) sites need no change: 0x413934 (sub_004137D0), 0x413EBD/ECC (sub_00413E70), 0x44A451 and 0x44A895 (sub_0044A420), 0x45C32F (sub_0045C050).
  World = cursor + origin 0x514E14, origin = viewleft - BLIT_RECT.left (0x42B789).
- Edge-scroll speed 0x4852E0 = 1000: leave alone (camera follow reads it too, 0x42B687).

## 4. Compositing (layout(), vddraw.c:~585)
- Presenter's `static uint8_t frame[1600*1200]` (~637) becomes a CAP_W x CAP_H allocation. Copy only rows 0..Hc, columns 0..A.
- Drop the `w != 640` early-out (~594). In a mission with hires active:
  World: src (0,21,Ww,Wh) -> dst (0,0,cw,ch); integer z exact when window divides, else sharp-bilinear.
  Strip: src (0,0,640,21) -> dst (0,0,cw,21u), as now.
  Panel: src (g_hud_right, 21, A-g_hud_right, 459) -> right edge at scale u, as now, with `w` replaced by A and the BLIT_RIGHT read replaced by g_hud_right.
- Menus, movies, loading screen (state != 2): one region, src (0,0,640,480) of the primary, letterboxed, even while the primary's logical size is A x Hc.
- Read Ww/Wh/A/g_hud_right with Interlocked access or under g_lock (s_regions is already read across threads without a lock).

## 5. Limits
Leave pools (90 units/450 objects) and lists (500 dirty/500 depth) alone at first. Measure: `--hires-log` line every 2 s (Ww x Wh, zoom, frame ms, presenter fps);
locate pool/list counters; count SeqEvict sub_00460FD0 calls via a wrapper (dupcheck first). Decide caps after Boot Camp at 1080p z=1 and 4K.

## 6. Unattended tests
- Before any run: no plasticrecomp.exe running; disc image mounted at H:.
- New test switches (mouse.c): `PLASTIC_SCRIPT="t:x,y[,click];..."` (game-time seconds; set feed target to game pixel x,y regardless of foreground; click = button-0 down/up after X/Y);
  `PLASTIC_KEYS="t:DIK;..."` (wrap GUID_SysKeyboard GetDeviceState in hook_create_device, OR 0x80 into DIK for 3 polls; Escape = 0x01 skips movies);
  `PLASTIC_ZOOM=<index>`.
- Boot Camp route: Escape at 6 s and 20 s; screenshot the title once to measure the BOOT CAMP button's 640x480 coordinates; click via PLASTIC_SCRIPT. Load waits 20-40 s.
- Per milestone: boot_check.ps1-style run, --watchdog 120, screenshots at 60/90/110 s. Check: world fills window, strip/panel placed right; no [ddraw] errors;
  `[hires]` log lines match Ww/Wh/A; scripted click at a world point moves Sarge, panel click works, pointer at each window edge scrolls (log eye 0x514E0C); zoom steps -> no tile garbage.

## 7. Risks
- R1 tile cache stays at max size while SCREEN_W/H shrink with zoom; assumed safe (counts in 0x514EA0/A4 stay). Test scrolling at z=2, 3. If tiles tear: architect.
- R2 unclassified SCREEN_W/H readers: sub_00454B00 (full-screen widget ctor, vtable 0x46FC84), sub_00425C10 and sub_00425AF0 (called from state machine sub_004260C0 at 0x42611A-0x4261A7),
  sub_0040B070 and sub_0040B600 (DirectDraw re-init; 0x40B600 calls sub_0041AA10). Classify; if shown in a mission, wrap with 640x480 geometry.
- R3 pause/notice image [0x5122C4] centred on SCREEN_CLIP (0x424C6B): lands at A/2 when zoomed in; cosmetic.
- R4 HUD re-creation outside mission start (sub_0040A6B0, multiplayer/restart) covered by M2, but M1 may not have run there. Log it.
- R5 guest-memory writes from mouse.c (M4) assume the poll runs on the game thread inside sub_00427070. UNVERIFIED how native32 holds the machine lock around COM calls.
- R6 performance: software blitters at 9x pixels on one pinned core; presenter upload up to 9.7 MB/frame at 4K; copy only used rects.
- R7 gameplay: more map visible; SeqEvict flame survival; sounds silent beyond 600-800 px from view centre; Plus/Minus may be bound in key config.

## 8. Milestones (one commit each)
1. Capacity surfaces, vdd_set_logical, dynamic presenter frame, 640-region rule outside missions. No visible change.
2. run_lift.py WRAP rename pass + recomp_lookup_manual dispatch; src\host\hires.c holds pass-through wrappers for 0x42C440, 0x413480, 0x4193C0 and split-entry traps. Bit-identical behaviour.
3. Fixed-size hires: M1-M4 at z=1 (or PLASTIC_ZOOM), new layout and region mapping, edge snap, --hires-log. Architect review of the diff.
4. Runtime zoom keys + window resize via the M3 pending path, full redraw, map-bounds clamp.
5. PLASTIC_SCRIPT, PLASTIC_KEYS and automated Boot Camp entry (can go before 3).
6. Classify R2 functions; wrap any HUD-like ones.
7. Pool and performance measurement at 1080p and 4K; cap decision.
8. Docs: R_hires.md step 4, README keys table, HANDOFF; audit_repo.py clean.

Files: scripts\run_lift.py, src\host\main.c (recomp_lookup_manual), new src\host\hires.c/.h, src\host\vddraw.c, src\host\mouse.c, scripts\build_host.bat (add hires.c), docs.


## 9. Results: dialogs (R2, milestone 6) and limits (milestone 7), 2026-10-27

### Mission screens (R2)
Sub-states of the state-2 dispatch (0x00511DBC, table at 0x00426230, index = sub-state - 22; 33 = play):
- 22: the instruction sheet / F1's strategic map (a full-screen bitmap, modal).
- 23: the game menu (RETURN TO GAME, CONTROLS, AUDIO, ABORT MISSION; campaign missions add SAVE GAME and LOAD GAME).
- 24: the HQ message box ("Message from HQ", OK).
- 25: SAVE GAME, 26: LOAD GAME (campaign), 27: AUDIO, 28: CONTROLS. 29..31: delete / overwrite dialogs (not reached).
- ABORT MISSION is a state change to the title (restore_640 already handled it); the loading screen is state 1.

All of them are built for 640x480: a widget screen's base rect is a full SCREEN_W x SCREEN_H (read back: 0,0,1280,981 in
hires), backdrops are centred on it, children sit at fixed positions. In hires the Controls and Audio screens came out torn
(backdrop mid-view, fields and buttons in the corner) and the menu, sheet and HQ box were tiny at 1:1. Fix: wrap the state-2
dispatch (0x004260C0): while the sub-state is 22..31 the game gets SCREEN_W/H = 640x480, SCREEN_CLIP and BLIT_RECT back,
and the presenter shows the frame's 640x480 corner as a letterboxed picture (the menu rule); leaving re-applies the view's
geometry and a full redraw. The backdrop behind a small dialog is the top-left 640x459 of the world at that moment (the
battlefield is not recomposed while a dialog is up). Verified by screenshots against PLASTIC_HIRES=0: Controls identical,
Audio, menu, HQ box, sheet, F1 map, Save and Load fine, return to play restores the hires view.

### Limits (milestone 7)
Counters in `--hires-log` ("limits" line, reset at each map load): row pool A (decals, cap 450 + budget 50) and pool B (troopers,
vehicles, roaches, cap 90 + 10) fill, their eviction calls and entries dropped, SEQ evictions, depth list peak, dirty list
peak, object table. Wrapped for it: RowPoolAEvict 0x4609D0, RowPoolBEvict 0x460CA0, SeqEvict 0x460FD0, ResetDirtyList
0x41DCE0, DrawMapObjects 0x41E440 (none has split entries).

Measured (Boot Camp: 1609 of 1700 objects; campaign map1 missions: 330 of 400):
- Pools A and B stay at the 36 / 4 they start with (Boot Camp) or 0-1 / 0 (campaign): no eviction at any view size, so
  nothing pops from the pools. SeqEvict never ran either (no sustained fire in the unattended runs: UNVERIFIED under heavy combat).
- The dirty list peaks at 22..72 of 500 (idle or scrolling), never full, at 4K too.
- The DEPTH LIST is the limit (500 nodes, filled by DrawMapObjects per redrawn region): peak 129 at 1280x960, 261 at 1920x1080,
  483 at 2560x1440, over 500 from about 2600x1450 in Boot Camp's start area (density dependent).
- What the game does when it overflows (read from the lift, confirmed by DrawMapObjects logs and 1:1 dumps): it splits the
  region in two along the longer side and calls itself on the halves. Two faults at big views: (1) the vertical split (a region
  wider than tall: every 16:9 view) never stores the second half's bottom, so it gets a stale stack value (8759320,
  11049264 seen): the right half then covers the whole map column down, takes the first 500 objects and drops the rest, and
  sprites are cut at the split line (at 3840x2160 the lower right lost trees, fences and signs; before/after dumps differ in
  190k pixels there; the original 640x480 view never reaches it); (2) every split passes deep = 1 to the
  halves, which then never split again but drop objects beyond 500, so a redraw holds at most 1000.
- Repair, in the DrawMapObjects wrapper (nested calls only, so a view that fits is untouched): every half is clamped into its
  parent's rectangle, a half over 64 px may split again (deep = 0), and the 1 px gap between halves is closed (second half
  starts at the first's right edge). 3840x2160 then needs 424 objects at most per leaf (12 splits), the picture is complete (checked
  against 1:1 dumps), no seam. PLASTIC_NO_DRAWFIX=1 turns it off.
- Remaining cap: a leaf region of 64 px or less still holds 500 objects; a mission denser than Boot Camp at 4K zoom 1 is not
  covered (no such save to run: UNVERIFIED). The object table (1700 in Boot Camp) is the game's own limit.
- Time at 3840x2160 zoom 1 (PLASTIC_CAP=3840x2160 PLASTIC_VIEW=3840x2160, a 1280x800 window): presenter 57..61 fps throughout,
  scrolling included; game thread per Flip 0.35 ms (1280x960), 0.41 (1920x1080), 0.47 (2560x1440), 0.54 (3200x1800),
  0.57-0.60 (3840x2160) idle and 0.44 / 0.78 ms while scrolling at 1080p / 4K (max 2.5 ms; one 30..45 ms hitch at the first full
  redraw after a load or zoom). The game loop spins (2900 loops/s at 1280x960, 1300 at 4K scrolling), so these are per-frame costs,
  not a frame rate: far under 16.7 ms.
- Not measured: the real 4K display (this PC is 1080p; the presenter uploads 4000x2181 = 8.7 MB per frame and held 60 fps).


## 10. Zoom out: 0.5x and 0.25x (2026-10-27)

Steps are now {0.25, 0.5, 1, 1.5, 2, 3, 4}. What changed and why:
- **Capacity** (vddraw.c compute_capacity): always 3840x2160 (+160 column, +21 strip = 4000x2181), not taken from the monitor: 0.5x on a
  1080p window is already a 3840x2160 view. Cost: primary, back buffer and world surface 8.7 MB each, the game's tile cache about the same,
  the presenter's frame copy and its texture 8.7 MB each, about 55 MB, allocated once. Measured on this PC (1080p-class window): the game's
  Flip interval is unchanged (0.37-0.40 ms at 1920x1080 as before), presenter 58-62 fps, the mission is up at the same time after launch.
- **View** (hires.c calc_view, the one place that decides it): the zoom asked for is raised, jointly in both directions, until the view fits
  3840x2160, so the view keeps the window's shape and the window stays filled (before, Ww and Hh were capped one by one and the fit left black
  bars). A 1080p window: 0.5x = 3840x2160, 0.25x -> 0.5x; 1280x960: 0.25x -> 0.444x (2880x2160); a maximised 2560x1411: 0.25x and 0.5x -> 0.666x
  (3840x2118). The map caps the view too (map - 2) and is then shown as before, one uniform scale, centred.
- **Effective zoom** = window / view (hires_view_t.eff); in the title notice ("Zoom 0.44x (view 2880x2160, the most there is room for)"),
  in F3's line, in `--hires-log` ("zoom 0.25 (shown 0.444)"), and in the `M3 geometry` log line. hires_zoom_step skips steps whose effective
  zoom equals the present one (tolerance 0.4 %), both ways, so there is no dead key press; at the end it says "furthest out" / "closest in".
- **Presenter** (present.c): a region shown smaller than its source (inv_scale > 1) takes the exact AREA average of the source texels a
  screen pixel covers, after the palette lookup, in sharp-bilinear, smooth and scanlines modes (plain bilinear would read 2x2 of every 4x4 at 0.25x and
  shimmer on scroll). At 0.5x aligned it is a 2x2 box. A box of width 1 is bilinear, so there is no seam at scale 1. Nearest stays nearest (one texel).
  Up to 25 palette lookups a pixel; no measurable cost (presenter 58-62 fps at 1080p and 2560x1411 windows). The HUD regions use it too if a
  window is small enough to shrink them.
- **plastic.ini**: `zoom_x = <value>` is the setting now; `zoom = <old index>` is still read when `zoom_x` is absent (old index 0..4 = 1, 1.5, 2, 3, 4)
  and is written as the older builds' index of the nearest step at or above 1x. `PLASTIC_ZOOM` is a value now, not an index.
- **Limits at these views**: depth list peaks 424 at 3840x2160 (1080p window, 0.5x), 466 at 3840x2118; no leaf ever full (the repair in the DrawMapObjects
  wrapper does its splits: 4..12 per redraw); pools and dirty list as before. Frame time 0.55-0.60 ms per Flip at 3840x2160.
- **Input**: the mouse mapping is the presenter's regions, which already took any scale: a click at client (631,762) at 0.444x landed on game pixel
  (1420,1736) = 631/0.444, 762/0.444+21, and Sarge walked there with the camera following; a click on the minimap (panel region, unaffected
  by zoom) moved the camera to that spot. At 0.5x one game pixel is two screen pixels, so the pointer's resolution on the map is two screen pixels.
  Edge scrolling stays at the game's 1000 world px/s, i.e. 500 screen px/s at 0.5x and 444 at 0.444x (not changed).
- **Known**: the game draws its pointer into the battlefield, so it shrinks with the zoom (a few pixels at 0.25x; at 4x it was always big). A cursor
  overlay at constant size would need the presenter to draw the sprite separately (its pixels cannot be told from the map's): not done.


## 11. Dialogs over the frozen battlefield, and the pointer smear (2026-10-27)

### What a dialog frame is (read from the Blt log of the ESC menu)
Dialog geometry is 640x480 (section 9). A screen is built by its builder (7 callers of 0x0044D6D0), which copies the back buffer's corner
(SCREEN_CLIP, 0,0-640,480) into the primary as the backdrop, then draws the widgets (BltFast of 152x25 plates, text) onto the primary. Every frame
after that (sub_00425AF0): back <- primary corner, widgets on the back buffer, the cursor's restore / save-under / draw, primary <- back, the cursor restored on
the back buffer. There is no page flip in dialogs. The frame that opens a dialog still runs the play frame's tail: the world's compose (world surface -> back) at the
dialog geometry, the HUD's strip and the selected unit's health bar; that is what the backdrop copy captures.

### Layout (vddraw.c layout / presenter, hires.c)
- The world behind is a snapshot of the back buffer taken from the cursor-draw wrapper in a play frame (sub-state 33), at most every 150 ms, before the cursor is drawn:
  the frame in which ESC is pressed is partial (the units are being redrawn), so the last complete play frame is used. Fallback: the shown frame.
- The wrapper of 0x0044D6D0 (WRAP list in run_lift.py: re-lift needed once) clears the 640x480 corner of both pages, the world surface and the cursor's
  32x32 save-under surface to a key index right after the backdrop copy, i.e. before the widgets. The key is the palette entry nearest magenta (255,0,255 = index
  255 in this game). So the live corner is key pixels plus exactly what the widgets and the cursor draw. A change of dialog screen (menu -> Controls -> menu)
  announces itself the same way (the next builder clears again).
- The presenter's frame buffer gets 480 rows below the logical frame: rows 0..h are the snapshot (copied once per dialog), rows h..h+479 the live corner
  (copied each frame). Regions: the world and the two HUD pieces from the snapshot exactly as in play; the live corner, 640x480 at the HUD scale `u`, centred
  in the window, with the key index not drawn (present.c: `use_key`; the key's colour is replaced by the pixel's own, so no pink fringe in a blend); and a map-only
  region over the same rectangle that maps any window point into the picture (clamped), so the pointer works over the whole window and clicks outside the picture land on its edge.
  A full-screen sheet (fewer than 2 % key pixels) dims the world and the HUD behind (`dim` 0.55); a small dialog hides the picture's top 21 rows (the HUD's strip, which the
  snapshot has). The picture shows only once its builder has run (400 ms fallback).
- Checked by screenshots at zoom 3 and 1 (default window), zoom 2 maximised (2560x1411): HQ box, instruction sheet, ESC menu (Boot Camp and campaign: SAVE / LOAD GAME), Controls,
  Audio, F1 map, the save dialog, clicks on the buttons through the mapping, return to play.
- `PLASTIC_DUMP_CORNER=dir` writes the live corner for diagnosis.

### Pointer smear (bug B), root cause
The cursor draw saves the pixels under the pointer into a 32x32 surface with `Blt(save, rect(0,0,32,32), back, pointer rect)` and restores them with
`Blt(back, pointer rect, save, rect(0,0,32,32))`; the arrow is 12x16. DirectDraw stretches a Blt with unequal rectangles, so the round trip is lossy (columns shifted by one
pixel except every third) and the damaged pixels are what the next save-under takes: where the picture under the pointer does not change (a dialog over the frozen world),
every pass leaves more garbled patches behind. It is the 1999 game's bug (real DirectDraw stretches identically); it is invisible in play because the world is redrawn each frame.
vddraw.c's Blt now copies the smaller of the two rectangles 1:1 when the sizes differ (the game never stretches on purpose; `PLASTIC_BLT_STRETCH=1` brings the stretch back).
Reproduction (PLASTIC_HIRES=0, ESC menu, scripted pointer over the hangar texture, screenshot before and after): 3120 pixels differ with the stretch, none with the fix.


## 12. The bottom HUD layout: built, then removed (2026-10-27)

A StarCraft-style console along the window's bottom edge (F7, `hud_layout = bottom`) was built in the presenter from slices of the HUD column's own pixels (minimap, one squad panel, weapons over commands) and removed the same day (commit "Remove the bottom HUD layout"). The owner found it too much to fix: the pieces have different heights, so black spaces and gaps remained along the bottom, and every game widget that changes size (the squad grid) needed its own probe. What stays from it: the generic opaque-rectangle / insets mechanism (section 13) and the presenter's flat fill and 32 regions. Gone: `build_bottom`, the slices, the squad probe, F7, `--hud-layout`, `PLASTIC_HUD_LAYOUT` / `PLASTIC_HUD_PIX` / `PLASTIC_HUD_DUMP` / `PLASTIC_TEST_CLOSE`, `hud_ok` / `panel_x` in the view. An old `hud_layout` line in plastic.ini is ignored (unknown keys are kept in the file). The lesson, if a console is tried again: pieces of one HUD column cannot be rearranged without making every piece the same height or drawing the frame between them anew.

## 13. Map edges and the opaque UI: insets, the camera's overscroll, adding a layout (2026-10-27)

### Problem
The battlefield is drawn over the whole window and the UI over it (the original's view ended where its panel began). The game's camera keeps the view on the map, so a map edge sits at the window's edge, behind the panel: objects at the edge (the boat at the lake's right edge in map2_mission2) were cut by the UI, in the bottom layout the world stopped at a line above a dark bar, and at the lowest camera position the covered strip grew.

### What happens now (hires.c `sub_0042B5A0`, vddraw.c `ui_add` / `ui_insets`, `layout_impl`)
- A layout declares its OPAQUE rectangles in window pixels with `ui_add(x, y, w, h, edges)`, `edges` = the window sides the piece stands on (UI_TOP / BOTTOM / LEFT / RIGHT). `ui_insets` derives from the list how far the opaque UI reaches into the window from each side (the largest reach of the pieces on that side); `layout_impl` converts them to game pixels of the view and hands them to the camera (`hires_set_insets`, every frame). Classic (the only layout using it now): the top strip, the column and the edge strip on the right. Zero insets: the game's camera is untouched.
- The camera wrapper keeps a virtual target Tv (map coordinates): set by the follow object or by the minimap's jump (the game's flag 0x00511E38), moved by the same step the game's edge scroll moved the target by (read from the eye), and clamped to the range that lets the UI's boundary reach the map's edge: [min - l + W/2, max + r - W/2 - 1] (x; y alike), after shifting it by half the difference of opposite insets so Sarge (the follow target) is in the middle of what the UI leaves free. The game still clamps its own eye to the map as ever (widening its map bounds made it paint junk tiles past the edge: tried and dropped); the difference (`shift_x/y` in `hires_view_t`) is how far the picture is moved in the presenter.
- The presenter moves the game's picture by the shift (a smaller source rectangle drawn at the matching side) and fills the strip that is left with a flat band in the console's colour (`fill_idx`, the grid's background; the darkest palette entry before the squad probe has seen it). So at a map edge the map ends exactly at the UI's boundary and nothing at the edge is hidden; in the middle of the map the world shows between and behind the pieces (no dark base bar any more). A click on a band maps to the picture's edge pixel next to it.
- `PLASTIC_NO_INSETS=1` turns the camera change and the shift off (test).

### Verified (map2_mission2 lake, scripted minimap clicks; screenshots)
Classic (default window 1280x960 zoom 1 and 0.5): all four edges and corners, the boat fully visible at the right and bottom edges (before: cut by the column). (The bottom layout was checked the same way before it was removed.) PLASTIC_HIRES=0: unchanged. Unverified: nearest at a whole zoom (the band is whole pixels, not whole zooms), the top-left and top-right corners at 0.5 and maximised, multiplayer.

### Adding a layout (opaque rect list -> insets automatic)
1. In vddraw.c `layout_impl` (the `hires` branch) build your pieces as presenter regions in drawing order (`put_region(rg, n, sx, sy, sw, sh, dx, dy, dw, dh)`: source rectangle in the frame, destination in window pixels; `use_key`, `use_fill`, `dim` for see-through and flat regions). The mapping of a click through a region (present.c) is automatic, so each piece must show the rectangle of the game's own frame that the game's hit tests expect (the column's rows, section 12).
2. Call `ui_add(x, y, w, h, edges)` for every piece that is opaque (not for gaps or see-through regions); `edges` names the window sides it stands on. Nothing else: the insets, the camera's overscroll, the bands and the shifted picture follow from the list. The call must come before the insets are computed (the block after the layout's pieces in `layout_impl`); a layout that makes no calls has zero insets, i.e. the plain full view.
3. Mouse code that needs to know (edge scrolling over a piece) goes in `cursor_from_client`.
4. Check with screenshots at the four map edges and corners (scripted minimap clicks `@x,y,click` in PLASTIC_SCRIPT) in every layout, zoom 1, 2 and 0.5, default window and maximised.


## 14. The game thread's pace: 60 Hz (2026-10-27) - why vehicles spun in place

### Symptom and cause
Owner: in Boot Camp a vehicle (jeep, tank, truck) turned but never drove. The game's loop has no clock of its own: the 1999 DirectDraw's `Flip` waited for the vertical blank, so the loop ran at the monitor's rate. The virtual DirectDraw's Flip costs microseconds and the loop spun at about 3100 frames a second (`--hires-log`: 0.3 ms per Flip). The game keeps time in whole milliseconds (`sub_00426CD0` QueryPerformanceCounter, dt in 0x00511E08, dt * 0.001 in 0x00511E10), so at that rate dt is 0 or 1 ms in almost every frame, and the vehicle movement, which turns by whole steps, then never integrates a translation (infantry and the camera cope).

### Evidence (map: Boot Camp; Sarge walks to the jeep, boards it, a destination is clicked; the unit table 0x00514F0C dumped with the host's PLASTIC_OBJDUMP / PLASTIC_OBJTRACE)
- unlimited (PLASTIC_GAME_FPS=0, 3094 game frames/s): vehicle 1008's position (object +0x30,+0x34) stays 881,2781 for 10 s after the order, its heading (block +0x50) jitters 172 / 180 / 188 around the target direction;
- 60 fps (default now, 60.0 game frames/s, Flip interval 16.67 ms, max 17.9): the same run goes 881,2781 -> 931,2794 (0.8 s) -> 1051,2826 -> 1079,2831 (arrived, 2 s after the order), heading steady at 180.
- Not the cause: the virtual DirectDraw, hires (PLASTIC_HIRES=0 and `--cnc-ddraw` behaved the same without pacing), the trig / atan tables the game builds at 0x0042DC30 (read back from guest memory and compared with the code's own formulas: 0 mismatches).
- An earlier A/B with PLASTIC_FLIP_SLEEP=16 looked negative only because Sarge had not yet reached the jeep in that run (a click on a vehicle sends Sarge to it; he enters it on arrival, about 40 s of walking from the start).

### The pace (vddraw.c `pace_flip`, called from every Flip)
A steady slot every 1 / game_fps s on the performance counter: a high-resolution waitable timer for all but the last 0.5 ms, a spin for the rest (no Sleep(1) jitter); a missed slot is not caught up (the schedule restarts from now after a hitch), so there is never a burst. Default 60; `game_fps` in plastic.ini, `PLASTIC_GAME_FPS` (0 = unlimited). Menus run their own pace (about 30 loops a second, no Flip wait added there), the intro movies keep their 15 fps, the single-core pin and the presenter's priority are unchanged. PLASTIC_FLIP_SLEEP stays as an extra throttle.

### The camera's drift (owner log): two causes
1. Part of the drift in the owner's log was the unpaced loop (the follow easing is `speed * dt` with dt 0 or 1 ms).
2. A bug of mine in the camera wrapper (section 13): the edge scroll's step was taken as `target - eye` instead of `target - the target I left`, so every scroll step added the whole easing gap and the virtual target ran away (the "moved by 0,180 / 0,179 ..." lines that changed by 1 per step). Fixed. Checked: cursor at rest in the middle: the eye stays put for 30 s; the right edge scrolls the eye to the map's end (shift 320, the column's inset) and it stops; the bottom edge likewise; back to the middle, no further movement.

### Test tools added
`PLASTIC_OBJDUMP=dir`, `PLASTIC_OBJDUMP_AT="t1,t2,.."` (host seconds since the process started): the object table (id, pointer, 0x180 bytes; units 1000..1099 also the two blocks their +0x74 / +0x90 point at) to `dir\objs_<n>.bin`, the trig tables to `trig.txt`, the atan tables to `atan.bin`. `PLASTIC_OBJTRACE="id,from,to"`: that unit's block, every 50 ms, to the log.
