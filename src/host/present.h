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
typedef struct { int sx, sy, sw, sh; int dx, dy, dw, dh; } present_region_t;

/* As present_frame, but drawing only the given regions, in order (later ones on top;
 * sampling never crosses a region's source rect). NULL/0 regions = the whole frame,
 * letterboxed. present_window_to_frame then maps through the topmost region under the
 * point, or the whole-frame letterbox when the point is in none of them. */
void present_frame_regions(const uint8_t* pixels, int pitch, int w, int h, const PALETTEENTRY pal[256],
                           const present_region_t* regions, int nregions);

/* 0 nearest, 1 sharp-bilinear (default). Takes effect on the next present_frame. */
void present_set_filter(int mode);

/* Client pixel (cx, cy) -> pixel of the last presented frame, clamped to 0..w-1 / 0..h-1.
 * Returns 1 if the point is on the picture, 0 if it is on a bar (or nothing was presented). */
int  present_window_to_frame(int cx, int cy, int* fx, int* fy);

void present_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
