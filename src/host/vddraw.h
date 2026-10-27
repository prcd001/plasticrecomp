/* vddraw.h: the game's virtual DirectDraw, presented by present.c (vddraw.c). */
#ifndef PLASTICRECOMP_VDDRAW_H
#define PLASTICRECOMP_VDDRAW_H
#include <windows.h>

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
 * capacity they were allocated at (a function of the monitor: the largest battlefield plus a 160-px
 * HUD column wide, and the 21-px strip plus that battlefield high). vdd_set_logical changes it under
 * the presenter's lock; 0 if a surface cannot hold it (nothing is changed). */
int  vdd_set_logical(int w, int h);
void vdd_get_logical(int* w, int* h);
int  vdd_capacity(int* w, int* h);

/* Borderless fullscreen (F11, Alt+Enter): set before the window exists, it is applied when the game makes it. */
int  vdd_get_fullscreen(void);
void vdd_set_fullscreen(int on);

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
