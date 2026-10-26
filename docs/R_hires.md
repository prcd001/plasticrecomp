# R_hires: resolution scaling, battlefield zoom and UI scale (research, 2026-10-26)

**Goal (owner).**
- The main menu and movies stay 640x480, scaled like a picture.
- In gameplay the HUD scales like a picture, with its own scale on Shift+Plus / Shift+Minus for very big screens.
- The battlefield renders at the window's native resolution, with zoom on Plus / Minus.

Status: research only, nothing implemented. Tags: [L] lifted C, [R] reconstruction
(`ArmyMen2-master`), [I] inferred. Spot-checked by hand: the .data values below and the three
writers of `BLIT_RECT.right`.

## 1. How the game draws today

- **Screen size is data.** `SCREEN_W/H` at `0x004852D8/DC` = 640x480 in .data. No code writes them,
  and `SetDisplayMode` and every surface are sized from them (`sub_0041AA10`) [L].
- **Pitch is read on every lock** (`0x00502AD0`, 94 references), never a hard-coded 640 [L].
- **The battlefield already has its own surface**, `0x00503100`. The map tile cache `0x00514E94` is
  sized ((W>>4)+2)*16 square, and `ComposeFrame` `0x0042DA30` BltFasts the play-area rect from it onto
  the back buffer each frame. Then overlays, then `HudPaint` `0x004143A0`, the menu cursor, and present
  (`RefreshDraw` `0x00424BF0`) [L].
- **The geometry block** is at `0x00485310` [L]:
  - `SCREEN_CLIP` {0,0,640,480}: clipping, cursor clamp, edge scroll.
  - `BLIT_RECT` {0,21,624,480}: the play area. The camera `0x0042B5A0` derives the view from it, and
    four "is the click in the world" tests read it.
  - The HUD panel already rewrites `BLIT_RECT.right` at runtime (`0x0041904C`, `0x0041941D`,
    `0x00419475`), so the view width is not fixed even today.
- **About ten hard-coded layout sites** [L]: the top strip's 600-px chat width, the HUD message clamp,
  the 640x480 movie surface, `y=0x15` panel anchors, one cheat, and the shake/air-strike distances.
- **Limits sized for the 480x459 view** [R]: object pools capped at 90 units and 450 objects,
  evicted by view; 500-entry dirty and depth lists.

## 2. What exists to build on

- **cnc-ddraw 7.1.0.0**
  - Accepts any `SetDisplayMode` size (`dd.c:656-668`) and has no surface cap below the GPU limit.
  - It always presents the WHOLE frame with one scale, with no per-region scaling (`render_d3d9.c:545-575`).
    So it cannot scale the HUD and the world differently.
- **Prior recompilation projects**
  - redalert2-recomp: a virtual display plus its own D3D11 presenter, sharp-bilinear, 720p-4K, a second
    layer at another scale.
  - wolf3d-recomp: the old picture scaled, with the world redrawn native over it. That is this design.
  - themepark-recomp: swapped 195 immediates for width and height variables.
- **2D RTS hi-res mods** (RA95 hi-res and others) all patched the same things: screen W/H, view
  buffers, tactical size, HUD anchors, scroll zones, camera clamps, dialog centring and shake bounds.
- **No existing Army Men II widescreen, hi-res or zoom mod** was found. PCGamingWiki lists widescreen
  and 4K as unsupported.

## 3. Design

1. **A host-owned presenter replaces cnc-ddraw.** The host answers `DirectDrawCreate` with a virtual
   DirectDraw: surfaces in host memory, palette kept, in the pattern of pcrecomp's
   `runtime/win32hle/ddraw.c`. Its own D3D11 (or D3D9) window then composes two 8-bit layers:
   - **World layer:** `W_world = window / zoom`. The game renders it with `SCREEN_W/H`, `SCREEN_CLIP`
     and `BLIT_RECT` set to the world size. Shown scaled by `zoom` (>= 1, so it only ever magnifies),
     nearest for integer zoom and sharp-bilinear otherwise.
   - **UI layer:** a 640x480 surface that the HUD, menus, movies and cursor draw into, with geometry
     swapped back to 640x480 around `HudUpdate`/`HudPaint` `0x00414370/0x004143A0`. Shown at
     `ui_scale` (Shift+Plus/Minus, default "fit height"), anchored per widget so on wide screens the
     panel and edge strip hug the right edge and the top strip spans the width.
   - Palette lookup in the shader: R8 index textures plus a 256x1 palette, filtering after the lookup,
     and the UI's transparent index as the key.
2. **Menus and movies** (game state `0x00511DA4` != 2): only the UI layer, the whole 640x480 scaled as
   one picture, exactly as today.
3. **Input.** The mouse fix already feeds the game cursor. It gains two spaces:
   - over the UI it is UI pixels (/ `ui_scale`);
   - over the world it is world pixels (/ `zoom`, plus the play-area offset).
   The four `PointInRect(BLIT_RECT, cursor)` sites and the cursor clamp need the world-size values;
   HUD hit tests keep 640x480.
4. **Zoom keys.** Plus/Minus change `zoom` in steps (1, 1.5, 2, 3, 4), clamped so the world size never
   exceeds the map. A change re-sizes the world surface and cache (allocated once at the largest
   size), rewrites the geometry and sets full-redraw `0x00512460`. Shift+Plus/Minus change `ui_scale`
   the same way. The keys are handled by the host, not passed to the game.

## 3b. Game logic tied to the view (research completed 2026-10-26)

Spot-checked by hand: SeqEvict reads the view origin, edge scroll reads the clamp rect's right and
bottom, and every reader of the eye `0x00514E0C` is audio, camera, map cache/draw, edge scroll or the
cheat. None is AI.

- **Edge scroll** `sub_00424CA0`: fires within 3 px of the **cursor-clamp rect** `0x00485310` (the full
  screen, not the play area). The speed is 1000 world px/s (`0x004852E0` times frame seconds) [L]. For a
  bigger view, the clamp rect becomes the window size. The speed is in world pixels, so scaling it by
  zoom is a choice.
- **Mouse to world:** every site is `world = cursor + view origin 0x00514E14`, gated by
  `PointInRect(0x00485320, cursor)` at `0x413934`, `0x413ECC`, `0x44A456`, `0x44A89A` and `0x45C32F` [L].
  The drag box is kept in world coordinates. HUD widgets test their own rects, and the first to grab
  the mouse wins (`0x004854B4`). With a zoom, the cursor is converted before these sites.
- **Radar:** the view box is sized from the world view rect `0x00514E34`, and a click recentres on the
  point. Both adapt to any view size [L].
- **On screen only:**
  - **Flame eviction (gameplay):** `SeqEvict` `sub_00460FD0` evicts effect records outside the
    screen-sized view origin rect first, once more than 160 of 200 are in use. Flames call `rand` and do
    damage, so a larger view changes which flames survive under heavy load, and with that damage and
    the random-number sequence [L/R].
  - **Audio:** distance is measured from the eye. Fixed sounds play only within 600, dynamic ones within
    800, so the edges of a large view are silent [R]. Not gameplay.
  - **Shake:** visual only.
  - **"Duck and cover" cheat:** a hard-coded 620x480 area.
  - **No AI, pathfinding or script reads the view** [L]. The script language has no on-screen
    conditions; `setcamerafocus` only moves the camera [R].
- **Camera:** no keyboard scrolling (arrows move Sarge). Follow is Sarge only, and a snap happens on
  weapon, squad and vehicle changes. Limits are the map inset (64 px) ± half the view, so they adapt.
  **The view must stay under 3968 world px** in each direction (every map is 4096 px), or Clamp's
  bounds cross and the camera flips every frame [I].
- **Fog of war** is driven by unit sight, not the screen. It is unchanged [L/R].
- **The renderer must keep calling** `DrawEffectLayer` `sub_004123D0`, which calls `rand`, and
  `DrawSelection` `sub_00462120`, which prunes the selection, or gameplay and the random sequence change.

## 4. Risks and open questions

- **Gameplay effects of a bigger view** (section 3b): seeing more of the map, flame survival under
  heavy load, and silence at the far edges. AI and scripts are unaffected.
- **The HUD panel rewrites the view's right edge** (`0x0041904C`, `0x0041941D`, `0x00419475`). In the
  layered design those writes must land in a 640x480 copy of the geometry (the UI layer), not in the
  world's.
- **Seeing more map is a gameplay change** (difficulty and fairness, and in multiplayer every player
  should use the same view size). Option: cap the world size, or offer "original view" as a mode.
- **Pool caps (90/450) and the 500-entry lists** may make objects pop at big views. Raising them means
  patching array sizes, so measure first.
- **Speed:** the world is drawn by lifted software blitters. 1080p at zoom 1 is about 9x the original's
  pixels, so it needs measuring.
- **Patch safely.** The lifter duplicates code for mid-function entries, so change data or wrap whole
  functions, never edit inside lifted bodies.
- **A view wider or taller than 3968 world px** breaks the camera clamp (lo > hi). Every map is
  4096 px, so cap `window / zoom` there.
- **Dropping cnc-ddraw** means re-doing in the host what it gave: the window and the mouse lock. The
  mouse fix already reads the cursor itself.

## 5. Suggested order

1. ~~Finish the view-dependent logic research~~ (done, section 3b).
2. **Virtual DirectDraw + presenter** at 640x480 only, matching today's picture. Proves the presenter
   and drops cnc-ddraw. **Done 2026-10-26**:
   - `src/host/vddraw.c`: COM objects with native vtables, surfaces as 8-bit DIB sections, the
     fullscreen flip chain, and a presenter thread.
   - `src/host/present.c`: D3D11, palette lookup in the shader, nearest / sharp-bilinear, letterbox.
   - The window is resizable at the largest whole multiple of 640x480. It keeps running when
     unfocused (WM_ACTIVATE/ACTIVATEAPP swallowed, `GetFocus` shimmed). Without this the intro froze
     on focus loss.
   - Measured: logo, cinematic and title render; the same frames advance focused or unfocused.
     The owner played to the menus and a submenu: "works fine". `--cnc-ddraw` keeps the old path.
3. UI layer split plus `ui_scale` keys, still with a 640x480 world.
4. World size = window / zoom, plus zoom keys, the input spaces and the camera clamp.
5. Measure pools and performance at 1080p and 4K, then decide on caps.
