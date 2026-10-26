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
