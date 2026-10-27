/* mouse.h: the game's DirectInput mouse device, seen from the host (mouse.c). */
#ifndef PLASTICRECOMP_MOUSE_H
#define PLASTICRECOMP_MOUSE_H
#include <stdint.h>

/* Host shim for DIRECTINPUT's DirectInputCreateA (native32_shim_t). */
void shim_DirectInputCreateA(void);

/* Replace one vtable slot of a COM object, returning what was there (NULL if it is already fn). */
void* patch_slot(void* obj, int slot, void* fn);

/* --mouse-log: report the device calls the game makes. */
extern int mouse_log;

/* --raw-mouse: the game gets DirectInput's own deltas, as it did in 1999. */
extern int mouse_raw;

/* Where the game keeps its cursor (guest VAs of two int32s), for exact
 * following; without them the host tracks the position it has fed. */
void mouse_set_cursor_vars(uint32_t x_va, uint32_t y_va);

/* Where the Windows cursor is in game pixels. NULL: GetCursorPos through the
 * host's import table, which cnc-ddraw hooks to answer in game pixels. */
#include <windows.h>
extern int (*mouse_target)(POINT* p);
/* The same mapping for the window client pixel (cx, cy) (test scripts: PLASTIC_SCRIPT "@x,y"). */
extern int (*mouse_target_client)(int cx, int cy, POINT* p);

/* Force the game's mouse device non-exclusive (cnc-ddraw does it when present). */
extern int mouse_nonexclusive;

#endif
