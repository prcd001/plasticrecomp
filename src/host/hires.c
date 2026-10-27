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
 *   0x004609D0, 0x00460CA0, 0x00460FD0  the evictors of row pool A (decals, cap 450), row pool B (troopers, vehicles,
 *               roaches, cap 90) and of a SEQ context (flames and other effect records): counted, for the limits
 *               measurement (milestone 7) in --hires-log
 *   0x0041DCE0  empties the dirty-rectangle list (500 records): its fill is read before it goes
 *   0x0041E440  DrawMapObjects, which fills the depth list (500 nodes): its fill is read after it
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

/* The zoom steps (the battlefield view is the window / zoom). Below 1 the picture is shrunk by the presenter: the
 * view is bigger than the window, up to the surface capacity (3840x2160, see vddraw.c). plastic.ini stores the value
 * (zoom_x), not the index, so adding steps never changes what a saved setting means. */
static const double k_zoom[] = { 0.25, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0 };
#define N_ZOOM ((int)(sizeof k_zoom / sizeof *k_zoom))
#define ZOOM_DEFAULT 2                  /* index of 1x */
static const double k_zoom_v1[] = { 1.0, 1.5, 2.0, 3.0, 4.0 };     /* what plastic.ini's old `zoom = index` meant */
#define N_ZOOM_V1 5

static int g_enabled = HIRES_DEFAULT;
static int g_log;
static volatile LONG g_zoom_want = ZOOM_DEFAULT;   /* zoom index asked for (keys, PLASTIC_ZOOM) */

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
static int g_force_w, g_force_h;        /* PLASTIC_VIEW=WxH: the view is sized as if the window's client area were that (test) */
/* timing, game thread writes, log thread reads */
static volatile LONG g_loops;

/* The limits (docs/R_hires.md section 3b; the caps are the original's, sized for a 640x480 view): all
 * counters restart with each map load. Peaks are what the game thread saw at the call that matters. */
#define ADDR_POOL_A     0x00662938u     /* row pool A: count (decals and seq step 0), cap 450 + budget 50 */
#define ADDR_POOL_B     0x006640C8u     /* row pool B: count (troopers, vehicles, roaches), cap 90 + budget 10 */
#define ADDR_DEPTH_CNT  0x0050B1D4u     /* depth list fill, 0..500 */
#define ADDR_DIRTY_TAIL 0x00508AC0u     /* dirty list: index of the last record, 0..499 (500 and over: a full redraw) */
#define ADDR_OBJ_CAP    0x00514F00u     /* the object table: capacity and count */
#define ADDR_OBJ_COUNT  0x00514F04u
static struct {
    volatile LONG a_calls, a_dropped, a_peak, b_calls, b_dropped, b_peak, seq_calls, seq_dropped, seq_peak;
    volatile LONG depth_peak, depth_full, depth_calls, depth_splits, dirty_peak, dirty_full, dirty_resets;
} g_lim;
static void lim_reset(void) { memset((void*)&g_lim, 0, sizeof g_lim); }
static void peak(volatile LONG* p, LONG v) { if (v > *p) *p = v; }

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
        fprintf(stderr, "[hires-log] %s view %dx%d zoom %.2f (shown %.3f) frame %.2f ms (max %.1f; %.1f game frames/s, %ld loops/s) "
                "presenter %.1f fps eye %d,%d (%08X %08X) state %d/%d/%d\n",
                act ? "on " : "off", act ? v.ww : 0, act ? v.wh : 0, act ? v.zoom : 0.0, act ? v.eff : 0.0, n ? us / 1000.0 / n : 0.0,
                mx / 1000.0, n / 2.0, (long)((lp - last_loops) / 2), (pres - last_pres) / 2.0,
                (int)(int16_t)MEM16(ADDR_EYE), (int)(int16_t)MEM16(ADDR_EYE + 2), MEM32(ADDR_EYE), MEM32(ADDR_EYE + 4),
                (int)MEM32(ADDR_STATE), (int)MEM32(0x00511DBCu), (int)MEM32(0x00511DC0u));
        if (MEM32(ADDR_STATE) == STATE_MISSION)
            fprintf(stderr, "[hires-log] limits: pool A %d (peak %ld, cap 450; evictions %ld calls, %ld dropped) "
                    "pool B %d (peak %ld, cap 90; %ld calls, %ld dropped) seq evict %ld calls, %ld dropped (peak fill %ld) "
                    "depth peak %ld/500 (full %ld of %ld; %ld splits) dirty peak %ld/500 (full %ld of %ld frames) objects %d/%d\n",
                    (int)MEM32(ADDR_POOL_A), g_lim.a_peak, g_lim.a_calls, g_lim.a_dropped, (int)MEM32(ADDR_POOL_B), g_lim.b_peak,
                    g_lim.b_calls, g_lim.b_dropped, g_lim.seq_calls, g_lim.seq_dropped, g_lim.seq_peak, g_lim.depth_peak, g_lim.depth_full,
                    g_lim.depth_calls, g_lim.depth_splits, g_lim.dirty_peak, g_lim.dirty_full, g_lim.dirty_resets, (int)MEM32(ADDR_OBJ_COUNT), (int)MEM32(ADDR_OBJ_CAP));
        last_pres = pres, last_frames = fr, last_loops = lp;
    }
}

void hires_init(int log) {
    const char* e = getenv("PLASTIC_HIRES");
    if (e && *e) g_enabled = *e != '0';
    e = getenv("PLASTIC_ZOOM");
    if (e && *e) hires_set_zoom_value(atof(e));       /* a value in x: 0.25, 0.5, 1, 1.5, 2, 3, 4 (nearest step) */
    g_log = log;
    e = getenv("PLASTIC_VIEW");
    if (e && sscanf(e, "%dx%d", &g_force_w, &g_force_h) != 2) g_force_w = g_force_h = 0;
    lock(); unlock();
    if (g_log) CloseHandle(CreateThread(NULL, 0, log_thread, NULL, 0, NULL));
    if (g_force_w) fprintf(stderr, "[hires] PLASTIC_VIEW: the view is sized for a %dx%d window\n", g_force_w, g_force_h);
    fprintf(stderr, "[hires] %s (PLASTIC_HIRES=%s), first zoom %gx\n", g_enabled ? "enabled" : "disabled",
            getenv("PLASTIC_HIRES") ? getenv("PLASTIC_HIRES") : "unset", k_zoom[g_zoom_want]);
}

void hires_disable(void) { g_enabled = 0; }
void hires_set_enabled(int on) { g_enabled = on != 0; }
void hires_set_zoom_value(double z) {
    int best = ZOOM_DEFAULT;
    double bd = 1e9;
    for (int i = 0; i < N_ZOOM; i++) {
        double d = fabs(k_zoom[i] - z);
        if (d < bd) bd = d, best = i;
    }
    g_zoom_want = best;
}
void hires_set_zoom_v1(int index) { hires_set_zoom_value(k_zoom_v1[index < 0 ? 0 : index >= N_ZOOM_V1 ? N_ZOOM_V1 - 1 : index]); }
double hires_get_zoom_value(void) { return k_zoom[g_zoom_want]; }
int  hires_get_zoom_v1(void) {          /* the older builds' index of the nearest step at or above 1x */
    for (int i = 0; i < N_ZOOM_V1; i++) if (k_zoom_v1[i] >= k_zoom[g_zoom_want] - 1e-9) return i;
    return N_ZOOM_V1 - 1;
}

int hires_view(hires_view_t* v) {
    int act;
    if (!g_cs_ready) return 0;
    lock();
    act = g_active;
    if (v) *v = g_view;
    unlock();
    return act;                         /* restore_640 decides when the geometry ends (state requests, the mouse poll) */
}

/* Plus / Minus (and Shift+Plus / Minus) are the host's unless the game has a text field in focus (its WM_CHAR
 * consumer, 0x005125B8, is set: chat, the name and save-game fields) or one of the mission's dialogs is up (the
 * Controls screen can then bind them). */
int hires_zoom_keys_ok(void) {
    return !g_dialog && !MEM32(0x005125B8u);
}

/* The battlefield view for a client area and a zoom step (the one place that decides it):
 *  - the zoom asked for, ze = z, is raised to what keeps the view within the surfaces (Wmax x Hmax = 3840x2160),
 *    jointly in both directions, so the view keeps the window's shape and still fills it (0.25x on a 1080p window
 *    is 0.5x: 3840x2160, not a smaller picture in black bars);
 *  - the view is ceil(window / ze), made even, and shrunk to the map (-2) if the map is smaller;
 *  - eff is what the window really shows: window / view (the scale of the fitted picture when the map caps it). */
typedef struct { int ww, wh; double ze, eff; int surf_capped, map_capped; } calc_t;
static int even(int v);

static int g_cap_w, g_cap_h;            /* the surfaces' capacity (all threads read it; set by the first call) */
static void calc_view(int cw, int ch, int zi, calc_t* c) {
    double z = k_zoom[zi], ze = z;
    int wmax = g_wmax, hmax = g_hmax;
    if (!wmax) {                        /* before the first map load: the capacity the surfaces will have */
        if (!g_cap_w) vdd_capacity(&g_cap_w, &g_cap_h);
        wmax = g_cap_w - COLUMN_W, hmax = g_cap_h - STRIP_H;
    }
    if (cw / ze > wmax + 1e-6) ze = (double)cw / wmax;
    if (ch / ze > hmax + 1e-6) ze = (double)ch / hmax;
    int ww = (int)ceil(cw / ze - 1e-6), wh = (int)ceil(ch / ze - 1e-6);
    int map_w = (int)MEM32(ADDR_MAP_MIN_X + 8) - (int)MEM32(ADDR_MAP_MIN_X);
    int map_h = (int)MEM32(ADDR_MAP_MIN_X + 12) - (int)MEM32(ADDR_MAP_MIN_X + 4);
    int want_w = ww, want_h = wh;
    ww = (ww + 1) & ~1, wh = (wh + 1) & ~1;
    if (ww > wmax) ww = wmax;
    if (wh > hmax) wh = hmax;
    if (map_w > 64 && ww > map_w - 2) ww = map_w - 2;
    if (map_h > 64 && wh > map_h - 2) wh = map_h - 2;
    ww = even(ww), wh = even(wh);
    if (ww < 64) ww = 64;
    if (wh < 64) wh = 64;
    c->ww = ww, c->wh = wh, c->ze = ze;
    c->surf_capped = ze > z + 1e-9;
    c->map_capped = ww < want_w - 1 || wh < want_h - 1;
    if (c->map_capped) {                /* one uniform scale, centred (the presenter's fit) */
        double a = (double)cw / ww, b = (double)ch / wh;
        c->eff = a < b ? a : b;
    } else {
        c->eff = ((double)cw / ww + (double)ch / wh) / 2;
    }
}

/* The client area the view is sized for (PLASTIC_VIEW overrides it); 0 when there is no window yet. */
static int client_size(int* cw, int* ch) {
    RECT rc = { 0, 0, 0, 0 };
    HWND w = vdd_window();
    if (!w || !GetClientRect(w, &rc) || rc.right <= 0 || rc.bottom <= 0) return 0;
    if (g_force_w > 0) rc.right = g_force_w, rc.bottom = g_force_h;
    *cw = rc.right, *ch = rc.bottom;
    return 1;
}

static void fmt_zoom(char* out, size_t n, double z) {        /* 0.5, 0.44, 1.5, 2 */
    char t[24];
    _snprintf(t, sizeof t - 1, "%.2f", z);
    t[sizeof t - 1] = 0;
    size_t l = strlen(t);
    while (l > 1 && t[l - 1] == '0' && strchr(t, '.')) t[--l] = 0;
    if (l > 1 && t[l - 1] == '.') t[--l] = 0;
    _snprintf(out, n - 1, "%s", t);
    out[n - 1] = 0;
}

/* One step along the list, skipping the steps that would show the same effective zoom as now (a 1080p window can show
 * 0.5x at most, so there 0.25x is no step; a small map caps the view the same way): no dead key presses.
 * Applied at the next panel update. */
void hires_zoom_step(int dir) {
    LONG cur = g_zoom_want, n;
    int cw, ch;
    calc_t c0, cn;
    char msg[160], a[24], b[24];
    if (!client_size(&cw, &ch)) {       /* no window yet: plain steps */
        n = cur + dir;
        if (n >= 0 && n < N_ZOOM) InterlockedExchange(&g_zoom_want, n);
        return;
    }
    calc_view(cw, ch, (int)cur, &c0);
    for (n = cur + dir; n >= 0 && n < N_ZOOM; n += dir) {
        calc_view(cw, ch, (int)n, &cn);
        if (fabs(cn.eff - c0.eff) > 0.004 * c0.eff) break;
    }
    if (n < 0 || n >= N_ZOOM) {
        fmt_zoom(a, sizeof a, c0.eff);
        _snprintf(msg, sizeof msg - 1, "Zoom %sx: the %s", a, dir < 0 ? "furthest out" : "closest in");
        msg[sizeof msg - 1] = 0;
        vdd_notice(msg);
        return;
    }
    InterlockedExchange(&g_zoom_want, n);
    fmt_zoom(a, sizeof a, cn.eff);
    fmt_zoom(b, sizeof b, k_zoom[n]);
    if (cn.surf_capped)
        _snprintf(msg, sizeof msg - 1, "Zoom %sx (view %dx%d, the most there is room for)", a, cn.ww, cn.wh);
    else if (cn.map_capped)
        _snprintf(msg, sizeof msg - 1, "Zoom %sx (%sx asked: the map ends)", a, b);
    else
        _snprintf(msg, sizeof msg - 1, "Zoom %sx (view %dx%d)", a, cn.ww, cn.wh);
    msg[sizeof msg - 1] = 0;
    vdd_notice(msg);
    fprintf(stderr, "[hires] zoom key: step %ld (%sx) -> effective %sx, view %dx%d\n", n, b, a, cn.ww, cn.wh);
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
static volatile LONG g_mission_serial;  /* map loads so far */
static LONG g_mission_serial_n(void) { return g_mission_serial; }

int hires_loading(void) { return g_loading; }

/* A mission is being played: the state-2 frame runs, its map has loaded and no dialog is up (sub-state 33). */
int hires_play_live(void) {
    return (int)MEM32(ADDR_STATE) == STATE_MISSION && g_mission_serial_n() > 0 && !g_loading && MEM32(0x00511DBCu) == 33;
}
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
    lim_reset();
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
    calc_t c;
    calc_view(cw, ch, zi, &c);
    int ww = c.ww, wh = c.wh;
    MEM32(ADDR_SCREEN_W) = (uint32_t)ww;
    MEM32(ADDR_SCREEN_H) = (uint32_t)(STRIP_H + wh);
    set_mem_rect(ADDR_BLIT, 0, STRIP_H, ww, STRIP_H + wh);
    g_vw = ww, g_vh = wh;
    clip_view();
    g_geo = 1;
    MEM32(ADDR_REDRAW) = 1;
    lock();
    g_view.ww = ww, g_view.wh = wh, g_view.a = g_a, g_view.hc = g_hc, g_view.hud_right = g_hud_right;
    g_view.capped = c.map_capped;
    g_view.zoom = c.ze;                 /* the zoom the view was made for: the asked one unless the surfaces cap it */
    g_view.eff = c.eff;
    g_active = 1;
    unlock();
    g_applied_zoom = zi, g_applied_cw = cw, g_applied_ch = ch;
    fprintf(stderr, "[hires] M3 geometry: client %dx%d, zoom %g -> view %dx%d, effective zoom %.3f%s%s (map %dx%d, A %d Hc %d, panel %d)\n",
            cw, ch, k_zoom[zi], ww, wh, c.eff, c.surf_capped ? " (surfaces cap)" : "", c.map_capped ? " (map caps)" : "",
            (int)MEM32(ADDR_MAP_MIN_X + 8) - (int)MEM32(ADDR_MAP_MIN_X), (int)MEM32(ADDR_MAP_MIN_X + 12) - (int)MEM32(ADDR_MAP_MIN_X + 4),
            g_a, g_hc, g_hud_right);
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
        int zi = (int)g_zoom_want, cw, ch;
        if (client_size(&cw, &ch) &&
            (!g_active || zi != g_applied_zoom || cw != g_applied_cw || ch != g_applied_ch))
            apply_geometry(cw, ch, zi);
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

/* ------------------------------------------------------------------- limits (counters only) */

void sub_004609D0(void) {                                  /* RowPoolAEvict */
    LONG b = (LONG)MEM32(ADDR_POOL_A);
    sub_004609D0_lifted();
    InterlockedIncrement(&g_lim.a_calls);
    peak(&g_lim.a_peak, b);
    if (b > (LONG)MEM32(ADDR_POOL_A)) InterlockedExchangeAdd(&g_lim.a_dropped, b - (LONG)MEM32(ADDR_POOL_A));
}

void sub_00460CA0(void) {                                  /* RowPoolBEvict */
    LONG b = (LONG)MEM32(ADDR_POOL_B);
    sub_00460CA0_lifted();
    InterlockedIncrement(&g_lim.b_calls);
    peak(&g_lim.b_peak, b);
    if (b > (LONG)MEM32(ADDR_POOL_B)) InterlockedExchangeAdd(&g_lim.b_dropped, b - (LONG)MEM32(ADDR_POOL_B));
}

void sub_00460FD0(void) {                                  /* SeqEvict(ctx): ctx->count is its first dword */
    uint32_t ctx = MEM32(g_esp + 4);
    LONG b = (LONG)MEM32(ctx);
    sub_00460FD0_lifted();
    InterlockedIncrement(&g_lim.seq_calls);
    peak(&g_lim.seq_peak, b);
    if (b > (LONG)MEM32(ctx)) InterlockedExchangeAdd(&g_lim.seq_dropped, b - (LONG)MEM32(ctx));
}

void sub_0041DCE0(void) {                                  /* ResetDirtyList */
    LONG t = (LONG)(MEM32(ADDR_DIRTY_TAIL) & 0xFFFF);
    InterlockedIncrement(&g_lim.dirty_resets);
    peak(&g_lim.dirty_peak, t);
    if (t >= 499) InterlockedIncrement(&g_lim.dirty_full);
    sub_0041DCE0_lifted();
}

/* DrawMapObjects(world rect *, map desc *, deep) collects the objects of a region into the depth list, which holds
 * 500, and draws them. When the list overflows it splits the region in two along the longer side and calls itself
 * on the halves (docs/R_step4_plan.md: nothing at the original's 640x480 view reaches this; a view of ~2500x1400
 * and up does). Two things in it are wrong at the sizes the hires view makes:
 *  - the VERTICAL split (a region wider than tall) never stores the second half's bottom, so the right half gets
 *    whatever lay on the stack (seen: 8759320): the half then reaches down the whole map, takes the first 500
 *    objects of that column and drops the rest, and sprites are cut at the split line;
 *  - every split of a region wider than 32 passes deep = 1 to its halves, which then never split again but drop
 *    what does not fit, so a redraw holds at most 2 x 500 objects.
 * The calls to itself go through this wrapper (the lift renamed the body), so a nested call's arguments are
 * repaired here, in guest memory, before the body reads them: every half is clamped into its parent's rectangle,
 * and a half bigger than 64 px either way gets deep = 0, i.e. may split again; and the second half, which starts at
 * the first half's right (bottom) + 1 although the right (bottom) edge is exclusive, starts there instead (the
 * 1 px line along the split, through every sprite on it). Not done for the outermost call, so
 * a view that fits (the original's behaviour) is untouched. PLASTIC_NO_DRAWFIX=1 turns the repair off (test). */
static int g_dm_depth;
static int g_dm_rect[32][4];            /* the rectangle each level of the recursion is working on */
static int g_dm_sib[33][4], g_dm_sib_ok[33];   /* the last finished sibling at each level, to close the 1 px gap between halves */
void sub_0041E440(void) {
    static int log_calls = -1, fix = -1;
    uint32_t wr = MEM32(g_esp + 4);
    if (log_calls < 0) log_calls = getenv("PLASTIC_LOG_DRAWOBJ") ? 0 : 1000000;
    if (fix < 0) fix = !getenv("PLASTIC_NO_DRAWFIX");
    int l = (int)MEM32(wr), t = (int)MEM32(wr + 4), r = (int)MEM32(wr + 8), b = (int)MEM32(wr + 12), deep = (int)MEM32(g_esp + 12);
    int d = g_dm_depth, logit = log_calls < 200 && (int)MEM32(ADDR_STATE) == STATE_MISSION;
    if (d > 0 && d < 32) {
        InterlockedIncrement(&g_lim.depth_splits);
        if (fix) {
            const int* pr = g_dm_rect[d - 1];
            int nl = l < pr[0] ? pr[0] : l, nt = t < pr[1] ? pr[1] : t, nr = r > pr[2] ? pr[2] : r, nb = b > pr[3] ? pr[3] : b;
            if (logit && (nl != l || nt != t || nr != r || nb != b))
                fprintf(stderr, "[drawobj] half %d,%d-%d,%d clamped to %d,%d-%d,%d\n", l, t, r, b, nl, nt, nr, nb);
            if (nl != l) MEM32(wr) = (uint32_t)nl;
            if (nt != t) MEM32(wr + 4) = (uint32_t)nt;
            if (nr != r) MEM32(wr + 8) = (uint32_t)nr;
            if (nb != b) MEM32(wr + 12) = (uint32_t)nb;
            l = nl, t = nt, r = nr, b = nb;
            if (g_dm_sib_ok[d]) {                /* the second half: no gap after the first */
                const int* sb = g_dm_sib[d];
                if (l == sb[2] + 1 && t == sb[1] && b == sb[3]) l = sb[2], MEM32(wr) = (uint32_t)l;
                else if (t == sb[3] + 1 && l == sb[0] && r == sb[2]) t = sb[3], MEM32(wr + 4) = (uint32_t)t;
            }
            if (deep && (r - l > 64 || b - t > 64)) MEM32(g_esp + 12) = 0, deep = 0;
        }
    }
    if (d < 32) g_dm_rect[d][0] = l, g_dm_rect[d][1] = t, g_dm_rect[d][2] = r, g_dm_rect[d][3] = b;
    if (d < 32) g_dm_sib_ok[d + 1] = 0;          /* a new parent: its halves start afresh */
    g_dm_depth = d + 1;
    sub_0041E440_lifted();
    g_dm_depth = d;
    if (d < 32) memcpy(g_dm_sib[d], g_dm_rect[d], sizeof g_dm_sib[d]), g_dm_sib_ok[d] = 1;
    LONG n = (LONG)MEM32(ADDR_DEPTH_CNT);
    if (logit) { log_calls++; fprintf(stderr, "[drawobj] depth %d world %d,%d-%d,%d (%dx%d) deep %d -> list %ld\n", d, l, t, r, b, r - l, b - t, deep, (long)n); }
    InterlockedIncrement(&g_lim.depth_calls);
    peak(&g_lim.depth_peak, n);
    if (n >= 500) InterlockedIncrement(&g_lim.depth_full);
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
    case 0x004609D0u: return sub_004609D0;
    case 0x00460CA0u: return sub_00460CA0;
    case 0x00460FD0u: return sub_00460FD0;
    case 0x0041DCE0u: return sub_0041DCE0;
    case 0x0041E440u: return sub_0041E440;
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
