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

/* Frames the presenter has shown so far. */
extern volatile LONG vdd_presented;

/* --dd-trace: log surface creation and unimplemented calls. */
extern int vdd_trace;

#endif
