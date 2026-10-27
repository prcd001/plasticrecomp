/* hires.h: host wrappers around lifted game functions (hires.c), step 4 of docs/R_hires.md. */
#ifndef PLASTICRECOMP_HIRES_H
#define PLASTICRECOMP_HIRES_H
#include <stdint.h>

typedef void (*hires_fn)(void);

/* Indirect-call dispatch, called first by every RECOMP_ICALL / RECOMP_ITAIL (main.c's
 * recomp_lookup_manual): the host's replacement for the lifted function at va, or NULL to leave
 * the call to the lifted dispatch table. */
hires_fn hires_lookup(uint32_t va);

#endif
