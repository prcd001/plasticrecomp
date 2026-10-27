/*
 * hires.c: host functions that run in front of lifted game functions (step 4 of docs/R_hires.md,
 * geometry as in docs/R_step4_plan.md).
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
 * them back, so a wrapper must not touch g_esp or the registers around the call. They only write
 * guest memory (MEM32).
 *
 * Wrapped here:
 *   0x0042C440  map load (called by the mission start, sub_00425300, at 0x0042550A)          M1
 *   0x00413480  HUD create (called at 0x004255A6 and 0x0040ADAC)                              M2
 *   0x004193C0  panel update, vtable slot 2 of the panel at 0x0046F944; reached only through
 *               HudUpdate's `call [edx+8]` (0x00414383), every mission frame                  M3
 *   0x004143A0  HudPaint                                                                     clip
 *   0x00412FE0  the cursor's draw                                                            clip
 *   0x00424CA0  edge scroll                                                                  clip
 *   0x00424AD0  state change request (arg: the new state), M4: leaving the mission
 *   0x004260C0  the state-2 (mission) frame's dispatch: the mission's modal screens, see "dialogs" below
 * and, from the mouse poll, M4: back to 640x480 geometry when the mission is over.
 *
 * The geometry (the game's "screen" is the battlefield view; the HUD sits in its own column):
 *   frame (the back buffer / primary), A x Hc:   top strip [0,640) x [0,21)
 *                                                 world [0,Ww) x [21,21+Wh)
 *                                                 HUD column [Wmax,A) x [21,480), A = Wmax + 160
 *   M1 sets SCREEN_W/H = A x Hc, so the surfaces and the tile cache built by the map load have room
 *   for the largest view, and the logical size of the virtual surfaces to A x Hc. M2 and M3 run the
 *   HUD's own code with SCREEN_W = A, so the panel anchors itself to the column's right edge, and
 *   keep the panel's BLIT_RECT.right (= its left edge) in g_hud_right for the presenter. Everywhere
 *   else SCREEN_W/H and BLIT_RECT are the view (Ww x 21+Wh), so the camera, the world drawing and the
 *   world hit tests see exactly the battlefield.
 *   SCREEN_CLIP is the view too ({0,0,Ww,21+Wh}) for all drawing and for the code that centres things
 *   on it (the HQ message box's text, the pause notice), and the whole frame {0,0,A,Hc} only where the
 *   HUD column is in play: during HudPaint, the cursor's draw, the edge scroll (the cursor is clamped to
 *   it, and it scrolls within 3 px of its edge) and the mouse poll's cursor clamp (mouse.c calls
 *   hires_poll_state at the start of a poll cycle and hires_poll_end at its end).
 *
 * sub_0042C440 is copied into other, dead functions (split entries, listed in recomp_wrap.h by the lift)
 * wherever another function jumps into its middle; a wrapper around the main entry would not see a
 * call to one of those, so an indirect call to one is logged. (A direct call would not be caught; the
 * lift shows none.)
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "native32.h"
#include "recomp_funcs.h"
#include "recomp_wrap.h"
#include "hires.h"
#include "vddraw.h"

#define HIRES_DEFAULT 1                 /* the feature's default when PLASTIC_HIRES is not set */

#define ADDR_SCREEN_W   0x004852D8u
#define ADDR_SCREEN_H   0x004852DCu
#define ADDR_CLIP       0x00485310u     /* left, top, right, bottom */
#define ADDR_BLIT       0x00485320u     /* left, top, right (0x485328), bottom */
#define ADDR_REDRAW     0x00512460u     /* full redraw */
#define ADDR_STATE      0x00511DA4u     /* 2 = mission */
#define ADDR_MAP_MIN_X  0x00514DF8u     /* map bounds: min x, min y, max x, max y */
#define ADDR_EYE        0x00514E0Cu
#define STATE_MISSION   2
#define STRIP_H         21
#define COLUMN_W        160

static const double k_zoom[] = { 1.0, 1.5, 2.0, 3.0, 4.0 };
#define N_ZOOM ((int)(sizeof k_zoom / sizeof *k_zoom))

static int g_enabled = HIRES_DEFAULT;
static int g_log;
static volatile LONG g_zoom_want;       /* zoom index asked for (keys, PLASTIC_ZOOM) */

static CRITICAL_SECTION g_cs;           /* g_view and its flag, game thread vs presenter and mouse */
static volatile LONG g_cs_ready;
static hires_view_t g_view;
static int g_active;                    /* the hires geometry is in force (M3 has run in this mission) */
/* game-thread only */
static int g_m1, g_m2;                  /* the map load / HUD create of this mission ran with hires geometry */
static int g_map_hires;                 /* this map was loaded with hires geometry: cleared only by the next map load, not by
                                         * restore_640, so a side screen that re-creates the HUD (0x0040ADAC) gets it back */
static int g_wmax, g_hmax, g_a, g_hc;
static int g_hud_right;
static int g_geo, g_vw, g_vh;           /* game thread: the hires geometry is applied; the view in game pixels */
static int g_applied_zoom = -1, g_applied_cw, g_applied_ch;
static int g_non_mission_polls;
/* timing, game thread writes, log thread reads */
static volatile LONG g_loops;

static void first_hit(volatile LONG* flag, const char* what) {
    if (!InterlockedExchange(flag, 1)) fprintf(stderr, "[hires] %s first entered\n", what);
}

static void lock(void) {
    if (!g_cs_ready) {                  /* the first caller is the game thread, before any other thread looks */
        InitializeCriticalSection(&g_cs);
        g_cs_ready = 1;
    }
    EnterCriticalSection(&g_cs);
}
static void unlock(void) { LeaveCriticalSection(&g_cs); }

static void set_mem_rect(uint32_t addr, int l, int t, int r, int b) {
    MEM32(addr) = (uint32_t)l, MEM32(addr + 4) = (uint32_t)t, MEM32(addr + 8) = (uint32_t)r, MEM32(addr + 12) = (uint32_t)b;
}
static int g_dialog;                    /* game thread: a widget screen of the mission is up, in the 640x480 geometry (enter_dialog) */
static void clip_view(void) { if (g_dialog) set_mem_rect(ADDR_CLIP, 0, 0, 640, 480); else set_mem_rect(ADDR_CLIP, 0, 0, g_vw, STRIP_H + g_vh); }
static void clip_full(void) { if (g_dialog) set_mem_rect(ADDR_CLIP, 0, 0, 640, 480); else set_mem_rect(ADDR_CLIP, 0, 0, g_a, g_hc); }

static DWORD WINAPI log_thread(LPVOID unused) {
    (void)unused;
    LONG last_pres = vdd_presented, last_frames = 0, last_loops = 0;
    for (;;) {
        Sleep(2000);
        hires_view_t v;
        int act = hires_view(&v);
        LONG pres = vdd_presented, fr = vdd_flips, us = InterlockedExchange(&vdd_flip_us_sum, 0),
             mx = InterlockedExchange(&vdd_flip_us_max, 0), lp = g_loops;
        LONG n = fr - last_frames;
        fprintf(stderr, "[hires-log] %s view %dx%d zoom %.1f frame %.2f ms (max %.1f; %.1f game frames/s, %ld loops/s) "
                "presenter %.1f fps eye %d,%d (%08X %08X) state %d/%d/%d\n",
                act ? "on " : "off", act ? v.ww : 0, act ? v.wh : 0, act ? v.zoom : 0.0, n ? us / 1000.0 / n : 0.0,
                mx / 1000.0, n / 2.0, (long)((lp - last_loops) / 2), (pres - last_pres) / 2.0,
                (int)(int16_t)MEM16(ADDR_EYE), (int)(int16_t)MEM16(ADDR_EYE + 2), MEM32(ADDR_EYE), MEM32(ADDR_EYE + 4),
                (int)MEM32(ADDR_STATE), (int)MEM32(0x00511DBCu), (int)MEM32(0x00511DC0u));
        last_pres = pres, last_frames = fr, last_loops = lp;
    }
}

void hires_init(int log) {
    const char* e = getenv("PLASTIC_HIRES");
    if (e && *e) g_enabled = *e != '0';
    e = getenv("PLASTIC_ZOOM");
    if (e && *e) {
        int z = atoi(e);
        g_zoom_want = z < 0 ? 0 : z >= N_ZOOM ? N_ZOOM - 1 : z;
    }
    g_log = log;
    lock(); unlock();
    if (g_log) CloseHandle(CreateThread(NULL, 0, log_thread, NULL, 0, NULL));
    fprintf(stderr, "[hires] %s (PLASTIC_HIRES=%s), first zoom index %ld\n", g_enabled ? "enabled" : "disabled",
            getenv("PLASTIC_HIRES") ? getenv("PLASTIC_HIRES") : "unset", g_zoom_want);
}

void hires_disable(void) { g_enabled = 0; }
void hires_set_enabled(int on) { g_enabled = on != 0; }
void hires_set_zoom(int index) { g_zoom_want = index < 0 ? 0 : index >= N_ZOOM ? N_ZOOM - 1 : index; }
int  hires_get_zoom(void) { return (int)g_zoom_want; }

int hires_view(hires_view_t* v) {
    int act;
    if (!g_cs_ready) return 0;
    lock();
    act = g_active;
    if (v) *v = g_view;
    unlock();
    return act;                         /* restore_640 decides when the geometry ends (state requests, the mouse poll) */
}

void hires_zoom_step(int dir) {
    LONG cur = g_zoom_want, n = cur + dir;
    if (n < 0) n = 0;
    if (n >= N_ZOOM) n = N_ZOOM - 1;
    if (n != cur) {
        InterlockedExchange(&g_zoom_want, n);
        fprintf(stderr, "[hires] zoom key: index %ld (%.1fx)\n", n, k_zoom[n]);
    }
}

/* Back to the 640x480 geometry the menus and movies run in (.data's own values). */
static void restore_640(void) {
    MEM32(ADDR_SCREEN_W) = 640, MEM32(ADDR_SCREEN_H) = 480;
    set_mem_rect(ADDR_CLIP, 0, 0, 640, 480);
    set_mem_rect(ADDR_BLIT, 0, STRIP_H, 624, 480);
    vdd_set_logical(640, 480);
    lock();
    g_active = 0;
    g_view.dialog = 0;
    unlock();
    g_geo = 0;
    g_dialog = 0;
    g_m1 = g_m2 = 0;
    g_applied_zoom = -1;
}

void hires_poll_end(void) {
    if (g_geo && (int)MEM32(ADDR_STATE) == STATE_MISSION) clip_view();
}

void hires_poll_state(void) {
    if (!g_geo) return;                 /* only once a mission has been shown with the hires geometry */
    if ((int)MEM32(ADDR_STATE) == STATE_MISSION) clip_full();
    if ((int)MEM32(ADDR_STATE) == STATE_MISSION) { g_non_mission_polls = 0; return; }
    /* a few polls in a row, so a one-frame blip does not tear the mission down */
    if (++g_non_mission_polls < 3) return;
    fprintf(stderr, "[hires] mission over (state %d): back to 640x480 geometry\n", (int)MEM32(ADDR_STATE));
    g_non_mission_polls = 0;
    restore_640();
}

/* ------------------------------------------------------------------- M1 */

static volatile LONG g_loading;         /* from the map load to the first panel update */

int hires_loading(void) { return g_loading; }
static volatile LONG g_mission_serial;
int hires_mission_serial(void) { return (int)g_mission_serial; }

/* M1's writes: the screen is the whole frame A x Hc, so the surfaces and the tile cache are made for the
 * largest view. Returns 0 (nothing changed) when a virtual surface lacks the capacity. */
static int enter_map_geometry(void) {
    if (!vdd_set_logical(g_a, g_hc)) return 0;
    MEM32(ADDR_SCREEN_W) = (uint32_t)g_a, MEM32(ADDR_SCREEN_H) = (uint32_t)g_hc;
    set_mem_rect(ADDR_CLIP, 0, 0, g_a, g_hc);
    set_mem_rect(ADDR_BLIT, 0, STRIP_H, g_wmax, STRIP_H + g_hmax);
    g_m1 = 1, g_m2 = 0, g_geo = 0;
    lock();
    g_active = 0;
    unlock();
    g_applied_zoom = -1;
    return 1;
}

void sub_0042C440(void) {
    static volatile LONG seen;
    first_hit(&seen, "map load 0x0042C440");
    g_loading = 1;
    InterlockedIncrement(&g_mission_serial);
    g_map_hires = 0;
    if (g_enabled) {
        int cw, ch;
        vdd_capacity(&cw, &ch);
        g_a = cw, g_hc = ch;
        g_wmax = cw - COLUMN_W, g_hmax = ch - STRIP_H;
        if (enter_map_geometry()) {
            g_map_hires = 1;
            fprintf(stderr, "[hires] M1 map load: SCREEN %dx%d, state %d\n", g_a, g_hc, (int)MEM32(ADDR_STATE));
        } else {
            fprintf(stderr, "[hires] a surface lacks the capacity: hires disabled\n");
            g_enabled = 0;
        }
    }
    sub_0042C440_lifted();
}

/* ------------------------------------------------------------------- M2 */

void sub_00413480(void) {
    static volatile LONG seen;
    first_hit(&seen, "HUD create 0x00413480");
    if (g_enabled && !g_m1 && g_map_hires && enter_map_geometry())      /* a side screen's return: this map's geometry again */
        fprintf(stderr, "[hires] M1 again for a HUD re-create: SCREEN %dx%d, state %d\n", g_a, g_hc, (int)MEM32(ADDR_STATE));
    if (g_enabled && g_m1) {
        uint32_t save_w = MEM32(ADDR_SCREEN_W), save_right = MEM32(ADDR_BLIT + 8);
        MEM32(ADDR_SCREEN_W) = (uint32_t)g_a;
        sub_00413480_lifted();
        g_hud_right = (int)MEM32(ADDR_BLIT + 8);
        MEM32(ADDR_BLIT + 8) = save_right;
        MEM32(ADDR_SCREEN_W) = save_w;
        g_m2 = 1;
        fprintf(stderr, "[hires] M2 HUD create: panel left edge %d (A %d)\n", g_hud_right, g_a);
        return;
    }
    if (g_enabled) fprintf(stderr, "[hires] HUD created without the hires map load (state %d): hires stays off\n", (int)MEM32(ADDR_STATE));
    sub_00413480_lifted();
}

/* ------------------------------------------------------------------- M3 */

static int even(int v) { return v & ~1; }

static void apply_geometry(int cw, int ch, int zi) {
    double z = k_zoom[zi];
    int ww = (int)ceil(cw / z), wh = (int)ceil(ch / z);
    int map_w = (int)MEM32(ADDR_MAP_MIN_X + 8) - (int)MEM32(ADDR_MAP_MIN_X);
    int map_h = (int)MEM32(ADDR_MAP_MIN_X + 12) - (int)MEM32(ADDR_MAP_MIN_X + 4);
    int want_w = ww, want_h = wh;
    ww = (ww + 1) & ~1, wh = (wh + 1) & ~1;
    if (ww > g_wmax) ww = g_wmax;
    if (wh > g_hmax) wh = g_hmax;
    if (map_w > 64 && ww > map_w - 2) ww = map_w - 2;
    if (map_h > 64 && wh > map_h - 2) wh = map_h - 2;
    ww = even(ww), wh = even(wh);
    if (ww < 64) ww = 64;
    if (wh < 64) wh = 64;
    MEM32(ADDR_SCREEN_W) = (uint32_t)ww;
    MEM32(ADDR_SCREEN_H) = (uint32_t)(STRIP_H + wh);
    set_mem_rect(ADDR_BLIT, 0, STRIP_H, ww, STRIP_H + wh);
    g_vw = ww, g_vh = wh;
    clip_view();
    g_geo = 1;
    MEM32(ADDR_REDRAW) = 1;
    lock();
    g_view.ww = ww, g_view.wh = wh, g_view.a = g_a, g_view.hc = g_hc, g_view.hud_right = g_hud_right;
    g_view.capped = ww < want_w - 1 || wh < want_h - 1;
    g_view.zoom = z;
    g_active = 1;
    unlock();
    g_applied_zoom = zi, g_applied_cw = cw, g_applied_ch = ch;
    fprintf(stderr, "[hires] M3 geometry: client %dx%d, zoom %.1f -> view %dx%d%s (map %dx%d, A %d Hc %d, panel %d)\n", cw, ch, z,
            ww, wh, g_view.capped ? " (capped)" : "", map_w, map_h, g_a, g_hc, g_hud_right);
}

/* ------------------------------------------------------------------- dialogs
 *
 * The mission's modal screens (sub-states 22..31 of 0x00511DBC: the instruction sheet, the HQ message box,
 * the game menu, audio, controls, save and load) are built for 640x480: a widget screen's base rect is the
 * full SCREEN_W x SCREEN_H, backdrops and pictures are centred on it, and the buttons and fields sit at
 * fixed 640x480 positions. With the hires SCREEN_W/H the backdrop landed in the middle of the view and the
 * fields in the corner (docs/R_step4_plan.md R2). So while one is up the game gets the 640x480 geometry
 * back, and the presenter shows the frame's 640x480 corner as one letterboxed picture, as it does for the
 * menus. The surfaces keep their size and the world and HUD in them are untouched: leaving re-applies the
 * view's geometry and asks for a full redraw. Entered and left from the wrapper of the state-2 frame,
 * sub_004260C0, which builds the screen. (22 and 24 are the ones seen at Boot Camp's start; 23 the menu,
 * 27 audio, 28 controls; the rest is by the dispatch table at 0x00426230, untested.) */
#define SUB_DIALOG_FIRST 22
#define SUB_DIALOG_LAST  31
#define ADDR_SUBSTATE    0x00511DBCu

static void enter_dialog(void) {
    MEM32(ADDR_SCREEN_W) = 640, MEM32(ADDR_SCREEN_H) = 480;
    set_mem_rect(ADDR_CLIP, 0, 0, 640, 480);
    set_mem_rect(ADDR_BLIT, 0, STRIP_H, 624, 480);
    g_dialog = 1;
    lock();
    g_view.dialog = 1;
    unlock();
    fprintf(stderr, "[hires] dialog (sub-state %u): 640x480 geometry\n", MEM32(ADDR_SUBSTATE));
}

static void apply_geometry(int cw, int ch, int zi);
static void leave_dialog(void) {
    g_dialog = 0;
    lock();
    g_view.dialog = 0;
    unlock();
    if (g_applied_zoom >= 0) apply_geometry(g_applied_cw, g_applied_ch, g_applied_zoom);
    fprintf(stderr, "[hires] dialog closed (sub-state %u): view geometry again\n", MEM32(ADDR_SUBSTATE));
}

static void dialog_check(void) {
    unsigned sub = MEM32(ADDR_SUBSTATE);
    int want = (int)MEM32(ADDR_STATE) == STATE_MISSION && sub >= SUB_DIALOG_FIRST && sub <= SUB_DIALOG_LAST;
    if (want && !g_dialog) enter_dialog();
    else if (!want && g_dialog) leave_dialog();
}

void sub_004260C0(void) {
    static volatile LONG seen;
    first_hit(&seen, "state-2 frame 0x004260C0");
    if (g_enabled && g_geo) dialog_check();     /* before the dispatch builds the screen */
    sub_004260C0_lifted();
    if (g_enabled && g_geo) dialog_check();     /* the screen closed in it: the view's geometry for the rest of the frame */
}

/* HudPaint, the cursor's draw and the edge scroll work on the whole frame, HUD column included. */
void sub_004143A0(void) {
    static volatile LONG seen;
    first_hit(&seen, "HudPaint 0x004143A0");
    if (!g_geo) { sub_004143A0_lifted(); return; }
    clip_full();
    sub_004143A0_lifted();
    if (g_geo) clip_view();          /* not if restore_640 ran inside (via a state request) */
}

void sub_00412FE0(void) {
    static volatile LONG seen;
    first_hit(&seen, "cursor draw 0x00412FE0");
    if (!g_geo) { sub_00412FE0_lifted(); return; }
    clip_full();
    sub_00412FE0_lifted();
    if (g_geo) clip_view();          /* not if restore_640 ran inside (via a state request) */
}

void sub_00424CA0(void) {
    static volatile LONG seen;
    first_hit(&seen, "edge scroll 0x00424CA0");
    if (!g_geo) { sub_00424CA0_lifted(); return; }
    clip_full();
    sub_00424CA0_lifted();
    if (g_geo) clip_view();          /* not if restore_640 ran inside (via a state request) */
}

/* The game asks for a state change (sub_00424AD0(new state)) and applies it at the end of the frame;
 * the next frame builds the new state's screen, e.g. the title's full-screen widget from SCREEN_W/H
 * (sub_00454B00). So the 640x480 geometry must be back before that, i.e. now, when the mission is left;
 * the mouse poll's check (hires_poll_state) comes too late (the title came out half drawn). */
void sub_00424AD0(void) {
    static volatile LONG seen;
    first_hit(&seen, "state request 0x00424AD0");
    if (g_geo || g_m1) {
        int want = (int)MEM32(g_esp + 4);
        if (want != STATE_MISSION) {
            fprintf(stderr, "[hires] state %d requested (now %d): back to 640x480 geometry\n", want, (int)MEM32(ADDR_STATE));
            restore_640();
        }
    }
    sub_00424AD0_lifted();
}

static void panel_update(void) {
    static volatile LONG seen;
    first_hit(&seen, "panel update 0x004193C0");
    g_loading = 0;
    if (!g_enabled || !g_m1 || !g_m2 || (int)MEM32(ADDR_STATE) != STATE_MISSION) {
        sub_004193C0();
        return;
    }
    InterlockedIncrement(&g_loops);
    {
        RECT rc = { 0, 0, 0, 0 };
        HWND w = vdd_window();
        int zi = (int)g_zoom_want;
        if (w && GetClientRect(w, &rc) && rc.right > 0 && rc.bottom > 0 &&
            (!g_active || zi != g_applied_zoom || rc.right != g_applied_cw || rc.bottom != g_applied_ch))
            apply_geometry(rc.right, rc.bottom, zi);
        else if (!g_active)
            apply_geometry(640, 480, zi);   /* no window yet (cannot happen in a mission) */
    }
    {
        uint32_t ww = MEM32(ADDR_SCREEN_W), view_right = MEM32(ADDR_BLIT + 8);
        MEM32(ADDR_BLIT + 8) = (uint32_t)g_hud_right;
        MEM32(ADDR_SCREEN_W) = (uint32_t)g_a;
        sub_004193C0();
        if (g_geo) {                    /* restore_640 may have run inside: its values must stand */
            int hr = (int)MEM32(ADDR_BLIT + 8);
            MEM32(ADDR_BLIT + 8) = view_right;
            MEM32(ADDR_SCREEN_W) = ww;
            if (hr != g_hud_right) {
                g_hud_right = hr;
                lock();
                g_view.hud_right = hr;
                unlock();
            }
        }
    }
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
    case 0x004143A0u: return sub_004143A0;
    case 0x00412FE0u: return sub_00412FE0;
    case 0x00424CA0u: return sub_00424CA0;
    case 0x00424AD0u: return sub_00424AD0;
    case 0x004260C0u: return sub_004260C0;
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
