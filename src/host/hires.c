/*
 * hires.c: host functions that run in front of lifted game functions (step 4 of docs/R_hires.md).
 *
 * Two mechanisms, because the lifter emits two kinds of call:
 *  - `call X` becomes a plain C call of sub_X. scripts/run_lift.py renames the lifted definition of
 *    each function in its WRAP list to sub_X_lifted; the host defines sub_X here, which runs first
 *    and then calls sub_X_lifted.
 *  - `call [reg]` / `jmp [reg]` (vtable slots) go through recomp_lookup_manual before the lifted
 *    dispatch table; hires_lookup, called from main.c's recomp_lookup_manual, returns the wrapper.
 *
 * A wrapper keeps the calling convention of a lifted function: its caller has already pushed the
 * return address and stored the registers in the g_* globals, and the lifted body pops it and stores
 * them back, so a wrapper must not touch g_esp or the registers around the call.
 *
 * Wrapped here:
 *   0x0042C440  map load (called by the mission start, sub_00425300, at 0x0042550A)
 *   0x00413480  HUD create (called at 0x004255A6 and 0x0040ADAC)
 *   0x004193C0  panel update, vtable slot 2 of the panel at 0x0046F944; reached only through
 *               HudUpdate's `call [edx+8]` (0x00414383)
 * For now all three are pass-through: they log the first time they run and call the lifted body. The
 * geometry writes of docs/R_step4_plan.md (M1..M3) go here.
 *
 * sub_0042C440 is copied into other, dead functions (split entries, listed in recomp_wrap.h by the lift)
 * wherever another function jumps into its middle; a wrapper around the main entry would not see a
 * call to one of those, so an indirect call to one is logged. (A direct call would not be caught; the
 * lift shows none.)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#include "recomp_funcs.h"
#include "recomp_wrap.h"
#include "hires.h"

static void first_hit(volatile LONG* flag, const char* what) {
    if (!InterlockedExchange(flag, 1)) fprintf(stderr, "[hires] %s first entered\n", what);
}

void sub_0042C440(void) {
    static volatile LONG seen;
    first_hit(&seen, "map load 0x0042C440");
    sub_0042C440_lifted();
}

void sub_00413480(void) {
    static volatile LONG seen;
    first_hit(&seen, "HUD create 0x00413480");
    sub_00413480_lifted();
}

static void panel_update(void) {
    static volatile LONG seen;
    first_hit(&seen, "panel update 0x004193C0");
    sub_004193C0();
}

static uint32_t g_split_lo = 0xFFFFFFFFu, g_split_hi;
static volatile LONG g_split_ready;

static void init_splits(void) {
    for (const uint32_t* p = am2_split_entries; *p; p++) {
        if (*p < g_split_lo) g_split_lo = *p;
        if (*p > g_split_hi) g_split_hi = *p;
    }
    g_split_ready = 1;
}

hires_fn hires_lookup(uint32_t va) {
    switch (va) {
    case 0x0042C440u: return sub_0042C440;
    case 0x00413480u: return sub_00413480;
    case 0x004193C0u: return panel_update;
    default: break;
    }
    if (!g_split_ready) init_splits();
    if (va >= g_split_lo && va <= g_split_hi) {
        for (const uint32_t* p = am2_split_entries; *p; p++)
            if (*p == va) {
                static volatile LONG n;
                if (InterlockedIncrement(&n) <= 20)
                    fprintf(stderr, "[hires] TRAP: indirect call to split entry 0x%08X of a wrapped function\n", va);
                break;
            }
    }
    return NULL;
}
