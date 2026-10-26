/* mouse.h: the game's DirectInput mouse device, seen from the host (mouse.c). */
#ifndef PLASTICRECOMP_MOUSE_H
#define PLASTICRECOMP_MOUSE_H
#include <stdint.h>

/* Host shim for DIRECTINPUT's DirectInputCreateA (native32_shim_t). */
void shim_DirectInputCreateA(void);

/* --mouse-log: report the device calls the game makes. */
extern int mouse_log;

/* --raw-mouse: the game gets DirectInput's own deltas, as it did in 1999. */
extern int mouse_raw;

/* Where the game keeps its cursor (guest VAs of two int32s), for exact
 * following; without them the host tracks the position it has fed. */
void mouse_set_cursor_vars(uint32_t x_va, uint32_t y_va);

#endif
