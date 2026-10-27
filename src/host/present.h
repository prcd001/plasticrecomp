/* present.h - puts the game's 8-bit indexed frames on screen with Direct3D 11.
 *
 * The frame (w x h palette indices) and its 256-entry palette are uploaded as two textures and
 * drawn in one full-screen pass, letterboxed to keep the frame's w:h aspect, with nearest or
 * sharp-bilinear filtering (the filter runs on colours, after the palette lookup, never on
 * indices). The swap chain follows the window's client size. Call everything from one thread.
 * Links against d3d11.lib, dxgi.lib, d3dcompiler.lib (d3dcompiler_47.dll) and user32.lib. */
#ifndef PLASTIC_PRESENT_H
#define PLASTIC_PRESENT_H

#include <stdint.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create the device and swap chain on an existing window. 0 on failure (reason on stderr). */
int  present_init(HWND hwnd);

/* Upload a frame (rows of w indices, pitch bytes apart) and its palette, and present it with
 * vsync. The palette texture is only rewritten when pal differs from the last one. */
void present_frame(const uint8_t* pixels, int pitch, int w, int h, const PALETTEENTRY pal[256]);

/* A piece of the frame drawn at its own place and scale: source rect in frame pixels,
 * destination rect in client pixels. */
typedef struct {
    int sx, sy, sw, sh; int dx, dy, dw, dh;
    int   use_key, key;      /* use_key: texels whose index is `key` are not drawn (the region is transparent there) */
    float dim;               /* 0..1: darkens the region's colours (1 = black) */
    int   map_only;          /* not drawn: it only maps window points to frame pixels, for any point of the window
                              * (clamped into its source rect), as the topmost region of present_window_to_frame */
} present_region_t;

/* As present_frame, but drawing only the given regions, in order (later ones on top;
 * sampling never crosses a region's source rect). NULL/0 regions = the whole frame,
 * letterboxed. present_window_to_frame then maps through the topmost region under the
 * point, or the whole-frame letterbox when the point is in none of them. */
void present_frame_regions(const uint8_t* pixels, int pitch, int w, int h, const PALETTEENTRY pal[256],
                           const present_region_t* regions, int nregions);

/* Scaling modes (F12 cycles them: sharp, nearest, smooth, scanlines). They apply to every region, the
 * world and the HUD alike, and take effect on the next present_frame. */
enum { PRESENT_NEAREST = 0, PRESENT_SHARP = 1, PRESENT_SMOOTH = 2, PRESENT_SCANLINES = 3, PRESENT_NMODES = 4 };
void        present_set_mode(int mode);          /* out of range: ignored */
int         present_get_mode(void);
int         present_cycle_mode(int dir);         /* next (+1) / previous (-1) in the cycle; returns the new mode */
const char* present_mode_name(int mode);         /* "sharp-bilinear", "nearest", "smooth bilinear", "scanlines" */
int         present_mode_from_name(const char* s);   /* a digit or the name's first letters; -1 if unknown */
void        present_set_filter(int mode);        /* old API: 0 nearest, otherwise sharp-bilinear */

/* The scale at which a w x h picture is shown in a cw x ch window: the largest that fits, and in nearest
 * mode the largest whole multiple (bars take the rest; a window smaller than the picture still gets the
 * fractional fit). The presenter's own letterbox and the layouts in vddraw.c use it. */
double present_fit_scale(int cw, int ch, int w, int h);

/* vsync on (default): Present waits for the vertical blank. Off: Present returns at once (tearing possible). */
void present_set_vsync(int on);
int  present_get_vsync(void);

/* Ask for a PNG of the next presented frame, as shown (the swap chain's back buffer: palette lookup and
 * scaling done), in directory dir (created if missing), named plastic_YYYYMMDD_HHMMSS[_n].png.
 * present_screenshot_result copies the path of the last file saved (or "" if it failed) into out, once
 * per request, and returns 1; 0 while nothing new is there. */
void present_request_screenshot(const char* dir);
int  present_screenshot_result(char* out, int n);

/* Client pixel (cx, cy) -> pixel of the last presented frame, clamped to 0..w-1 / 0..h-1.
 * Returns 1 if the point is on the picture, 0 if it is on a bar (or nothing was presented). */
int  present_window_to_frame(int cx, int cy, int* fx, int* fy);

/* The index of the topmost drawn region under a client pixel (-1: none), and the frame pixel a client pixel maps to through region 0
 * alone, whatever is drawn over it (the bottom HUD layout's edge scrolling over the console). */
int  present_region_at(int cx, int cy);
int  present_window_to_frame_region0(int cx, int cy, int* fx, int* fy);

void present_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
