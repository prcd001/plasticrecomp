/* hires.h: host wrappers around lifted game functions (hires.c), step 4 of docs/R_hires.md. */
#ifndef PLASTICRECOMP_HIRES_H
#define PLASTICRECOMP_HIRES_H
#include <stdint.h>

typedef void (*hires_fn)(void);

/* Indirect-call dispatch, called first by every RECOMP_ICALL / RECOMP_ITAIL (main.c's
 * recomp_lookup_manual): the host's replacement for the lifted function at va, or NULL to leave
 * the call to the lifted dispatch table. */
hires_fn hires_lookup(uint32_t va);

/* Start-up: reads PLASTIC_HIRES (0 turns the feature off, 1 on; unset = the built-in default) and
 * PLASTIC_ZOOM (the first zoom, a value: 0.25 0.5 1 1.5 2 3 4), starts the --hires-log thread when log != 0. */
void hires_init(int log);
/* Before hires_init: the settings from plastic.ini (PLASTIC_HIRES / PLASTIC_ZOOM still override them). */
void hires_set_enabled(int on);
/* The zoom as a value in x (the nearest step of 0.25 0.5 1 1.5 2 3 4), plastic.ini's `zoom_x`. */
void   hires_set_zoom_value(double z);
double hires_get_zoom_value(void);
/* plastic.ini's older `zoom = index` (0..4 = 1x 1.5x 2x 3x 4x): the same zoom value as it always meant. The getter gives
 * that index for the nearest step at or above 1x, so an older build reading the file still gets a sane zoom. */
void hires_set_zoom_v1(int index);
int  hires_get_zoom_v1(void);
/* The feature off for the whole run (e.g. with --cnc-ddraw, which has no virtual surfaces). */
void hires_disable(void);

/* The battlefield's view as the presenter and the cursor mapping need it (a consistent snapshot).
 * Returns nonzero when the hires geometry is in force in a mission; then:
 *   ww, wh    world view size in game pixels (the world is src (0,21,ww,wh) of the frame)
 *   a, hc     the frame's logical size (HUD column [a-160 .. a) at the right)
 *   hud_right left edge of the panel in the frame (a-160 open .. a-16 closed)
 *   capped    the view is smaller than the window needs at this zoom (the map is smaller): the world is then
 *             drawn at one uniform scale instead of filling the window. (The surfaces' limit, 3840x2160, is no
 *             "capped": the zoom is raised in both directions until the view fits, so the window stays filled)
 *   dialog    a widget screen of the mission (game menu, audio, controls) is up: the game is in the 640x480
 *             geometry and the frame's 640x480 corner is the picture
 *   zoom      the zoom the view was made for: the one asked for, or the larger one the surfaces' limit forces
 *   eff       the zoom the window really shows: window / view (below 1 the presenter shrinks the view)
 *   hud_ok    the HUD's widget tree is the one the bottom layout's slices were measured on (single player: a 144x459 panel at
 *             panel_x with the minimap, SARGE, SQUAD and COMMANDS widgets at their known places, the 16x459 edge strip at a-16);
 *             without it the bottom layout is not offered. panel_x is the panel's left edge in the frame (hud_right when open)
 *   map_rows  rows of the view (from its top) that lie on the map: wh, or fewer when the camera has run past the map's bottom edge under the
 *             bottom console (the game draws stale tiles there; the presenter covers them) */
typedef struct { int ww, wh, a, hc, hud_right, capped, dialog; double zoom, eff; int hud_ok, panel_x; int map_rows; } hires_view_t;
int hires_view(hires_view_t* v);

/* The view's dialog flag, set and cleared by vddraw.c's vdd_dialog_begin / vdd_dialog_end under the presenter's lock (any thread),
 * so the frame, the key and the flag are one moment for the presenter. */
void hires_set_dialog_flag(int on);

/* Nonzero from a mission's map load until its first panel update (the loading screen; test dumps). */
int hires_loading(void);
/* A mission is being played now (loaded, not in a dialog): the mouse is confined to the window only then. */
int hires_play_live(void);
/* Counts the mission map loads so far (test scripts' state clock, mouse.c). */
int hires_mission_serial(void);

/* Nonzero when Plus / Minus (and Shift+Plus / Minus) belong to the host: no text field has the focus and no
 * mission dialog is up. Also tells mouse.c which keys to hide from the game's keyboard. */
int hires_zoom_keys_ok(void);

/* Zoom one step along {0.25, 0.5, 1, 1.5, 2, 3, 4} (dir +1 zooms in), skipping the steps that would show the same
 * effective zoom as now (the view is capped at 3840x2160 and at the map's size): no dead key presses. A notice names
 * the result (vdd_notice). Applied at the next panel update. */
void hires_zoom_step(int dir);

/* From the mouse poll (game thread): goes back to 640x480 geometry once a mission has ended. */
void hires_poll_state(void);
/* ... and the end of that poll cycle (the cursor clamp is done). */
void hires_poll_end(void);

#endif
