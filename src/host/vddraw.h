/* vddraw.h: the game's virtual DirectDraw, presented by present.c (vddraw.c). */
#ifndef PLASTICRECOMP_VDDRAW_H
#define PLASTICRECOMP_VDDRAW_H
#include <windows.h>
#include <stdint.h>

/* Host shim for DDRAW's DirectDrawCreate (native32_shim_t). */
void shim_DirectDrawCreate(void);

/* The display mode the game set (640x480), and whether its window is known yet. */
int vdd_mode(int* w, int* h);

/* The game's window, once it has called SetCooperativeLevel; NULL before. */
HWND vdd_window(void);

/* The Windows cursor in frame pixels (the presenter's mapping); 0 if unknown. */
int vdd_cursor(POINT* p);
int vdd_cursor_at(int cx, int cy, POINT* p);   /* the same for a given client pixel (negative: from the right / bottom) */

/* The size of the big surfaces (primary, back buffer, world surface) as the game sees it, within the
 * capacity they were allocated at (3840x2160 for the battlefield, whatever the monitor, because the
 * smallest zoom, 0.25x, shows four times the window; plus a 160-px HUD column wide and the 21-px strip high). vdd_set_logical changes it under
 * the presenter's lock; 0 if a surface cannot hold it (nothing is changed). */
int  vdd_set_logical(int w, int h);
void vdd_get_logical(int* w, int* h);
int  vdd_capacity(int* w, int* h);

/* The mission's dialogs over the frozen battlefield (hires.c): vdd_dialog_snapshot keeps the back buffer (or, with
 * from_primary, the shown frame) as the world behind the dialog; vdd_dialog_begin makes the dialog's 640x480 corner
 * see-through (see vddraw.c) and gives the cursor's save-under surface (a guest pointer) the same treatment;
 * vdd_dialog_end goes back to the plain frame. Game thread. */
int  vdd_dialog_snapshot(int from_primary);
int  vdd_dialog_begin(uint32_t save_under);
void vdd_dialog_refill(void);
void vdd_dialog_clear(uint32_t save_under);
int  vdd_dialog_snapshot_valid(void);   /* a snapshot of the world exists (an earlier play frame's) */
/* The dialog is over: restore = the mission goes on, so the corner of both pages and the cursor's save-under surface at the pointer
 * (cx, cy) are put back from the snapshot. The key, the view's dialog flag and a short guard on the next frames change together. */
void vdd_dialog_end(int restore, uint32_t save_under, int cx, int cy);

/* A line in the title bar for 2 s (and the log), like the scaling mode's name. Any thread. */
void vdd_notice(const char* text);

/* Borderless fullscreen (F11, Alt+Enter): set before the window exists, it is applied when the game makes it. */
int  vdd_get_fullscreen(void);
void vdd_set_fullscreen(int on);

/* F8: keep the mouse inside the window while a mission is played (ClipCursor; released in menus, dialogs, when the
 * window loses the foreground, and on exit). F9: edge scrolling (off: the game's cursor never gets within 4 px of its
 * clamp rectangle's edge, so nothing scrolls). Both default to on; plastic.ini keeps them. */
int  vdd_get_confine(void);
void vdd_set_confine(int on);
int  vdd_get_edge_scroll(void);
void vdd_set_edge_scroll(int on);
void vdd_release_cursor(void);          /* any thread: ClipCursor(NULL) if the host set a clip */

/* The HUD layout: 0 classic (the panel column at the window's right), 1 the bottom console (F7, plastic.ini hud_layout). The name is for
 * notices; vdd_bottom_layout_possible says whether the present window and mission can show the console (else it stays classic). */
int  vdd_get_hud_layout(void);
void vdd_set_hud_layout(int layout);
const char* vdd_hud_layout_name(int layout);
int  vdd_bottom_layout_possible(void);
/* Game rows at the bottom of the battlefield view that the bottom console's tallest piece covers (0: classic layout, no console, a dialog).
 * The camera wrapper (hires.c) takes them out of the view height it reports to the game's camera. */
int  vdd_bottom_cover(void);

/* Where F10 / PrintScreen put their PNGs (default: "screenshots" in the working folder). */
void vdd_set_screenshot_dir(const char* dir);

/* The HUD scale in half steps (0 = follows the window), for plastic.ini. */
int  vdd_get_ui_halves(void);
void vdd_set_ui_halves(int halves);

/* Frames the presenter has shown so far. */
extern volatile LONG vdd_presented;

/* The game's Flips so far, and the sum and maximum (microseconds) of the intervals between them. */
extern volatile LONG vdd_flips, vdd_flip_us_sum, vdd_flip_us_max;

/* --dd-trace: log surface creation and unimplemented calls. */
extern int vdd_trace;

#endif
