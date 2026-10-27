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
 * PLASTIC_ZOOM (first zoom index, 0..4), starts the --hires-log thread when log != 0. */
void hires_init(int log);
/* The feature off for the whole run (e.g. with --cnc-ddraw, which has no virtual surfaces). */
void hires_disable(void);

/* The battlefield's view as the presenter and the cursor mapping need it (a consistent snapshot).
 * Returns nonzero when the hires geometry is in force in a mission; then:
 *   ww, wh    world view size in game pixels (the world is src (0,21,ww,wh) of the frame)
 *   a, hc     the frame's logical size (HUD column [a-160 .. a) at the right)
 *   hud_right left edge of the panel in the frame (a-160 open .. a-16 closed)
 *   capped    the view is smaller than the window needs at this zoom (map or surface limit):
 *             the world is then drawn at one uniform scale instead of filling the window
 *   zoom      the zoom factor */
typedef struct { int ww, wh, a, hc, hud_right, capped; double zoom; } hires_view_t;
int hires_view(hires_view_t* v);

/* Nonzero from a mission's map load until its first panel update (the loading screen; test dumps). */
int hires_loading(void);

/* From the mouse poll (game thread): goes back to 640x480 geometry once a mission has ended. */
void hires_poll_state(void);
/* ... and the end of that poll cycle (the cursor clamp is done). */
void hires_poll_end(void);

#endif
