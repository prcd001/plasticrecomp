/*
 * vddraw.c: a virtual DirectDraw for the game, in host memory, shown by present.c.
 *
 * The game asks DirectDraw for its fullscreen path (DDSCL_EXCLUSIVE|FULLSCREEN, a
 * 640x480x8 mode, a primary flip chain with one back buffer, an ALLOW256 palette),
 * because the host never passes it "-w" (docs/R_hires.md; DirectDraw inventory,
 * 2026-10-26). That path is the simple one to provide: no clipper, no desktop
 * coordinates, no GDI palette realisation. So DirectDrawCreate is a host shim and
 * everything behind it is here: one DirectDraw object with an IDirectDraw and an
 * IDirectDraw2 face, IDirectDrawSurface (v1: nothing ever asks for a later one),
 * and IDirectDrawPalette. cnc-ddraw is never loaded.
 *
 * The objects are real COM objects with native vtables (stdcall C functions), not
 * shims inside the lifted model: smackw32.dll, native code, calls GetPixelFormat on a
 * surface itself, and lifted code reaches native methods through native32's bridge.
 *
 * Surfaces are 8-bit DIB sections, top-down, so the font builder's
 * GetDC/TextOutA/ReleaseDC on the back buffer (RenderGlyph 0x004465E0) draws into
 * the same pixels it then Locks. A presenter thread shows the primary through
 * present.c about 60 times a second: the game draws straight into the primary too
 * (menus, movies, ClearSurface), not only through Flip.
 *
 * Only what the game uses is implemented; every other slot logs once and fails
 * with the right argument count (stdcall: the callee pops). IsLost never reports
 * a loss, so the game's lost-surface paths stay dead, as they do on real hardware.
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <ddraw.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "present.h"
#include "vddraw.h"
#include "hires.h"

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))

int vdd_trace;                          /* --dd-trace */
static int g_flip_sleep;                /* PLASTIC_FLIP_SLEEP=ms: the game thread sleeps this long after each Flip (test switch) */
volatile LONG vdd_flips, vdd_flip_us_sum, vdd_flip_us_max;   /* Flips, and their intervals (hires.c logs them) */

/* ------------------------------------------------------------------ objects */

typedef struct pal {
    const void* const* vtbl;
    LONG refs;
    PALETTEENTRY e[256];
} pal_t;

typedef struct surf {
    const void* const* vtbl;
    LONG refs;
    int w, h, pitch;                    /* w x h: the logical size; the DIB holds cap_w x cap_h (>= w x h) */
    int cap_w, cap_h;
    uint8_t* px;
    HBITMAP dib;
    HDC dc;
    DWORD caps;
    int has_key;
    DWORD key_lo, key_hi;
    pal_t* pal;
    struct surf* back;                  /* the primary's attached back buffer */
    int primary;
} surf_t;

typedef struct { const void* const* vtbl; } dd_face_t;

static dd_face_t g_dd1, g_dd2;
static LONG g_dd_refs = 1;
static surf_t* g_primary;
static HWND g_hwnd;
static int g_mode_w = 640, g_mode_h = 480;
static CRITICAL_SECTION g_lock;         /* the primary's pixels and palette, vs the presenter */
static volatile LONG g_running;

static void trace_unimplemented(const char* what) {
    static char seen[64][48];
    static int n;
    for (int i = 0; i < n; i++)
        if (!strcmp(seen[i], what)) return;
    if (n < 64) strncpy(seen[n++], what, 47);
    fprintf(stderr, "[ddraw] not implemented: %s\n", what);
}

/* Stubs for slots the game never calls, by argument count (this included). */
#define STUB(n, ...) static HRESULT WINAPI stub##n(__VA_ARGS__) { trace_unimplemented("slot with " #n " args"); return DDERR_UNSUPPORTED; }
STUB(1, void* a)
STUB(2, void* a, void* b)
STUB(3, void* a, void* b, void* c)
STUB(4, void* a, void* b, void* c, void* d)
STUB(5, void* a, void* b, void* c, void* d, void* e)
STUB(6, void* a, void* b, void* c, void* d, void* e, void* f)

/* ----------------------------------------------------------------- surfaces */

static const void* const g_surf_vtbl[36];

/* Capacity: the big surfaces (primary, its back buffer, the world surface) are allocated once at
 * the largest size the host will ever show (g_cap_w x g_cap_h, set from the monitor when the
 * primary is created), and their logical w x h then change with vdd_set_logical. Everything the
 * game reads back is the logical size; the pitch is the capacity's, as on real hardware, where
 * the game must (and does) use lPitch. */
static int g_cap_w, g_cap_h;
static int g_world_pending;             /* the next offscreen surface created is the world surface */
static surf_t* g_world;                 /* the world surface (the big offscreen one), once the game has made it */

/* The capacity is the largest battlefield the host ever lays out: 3840x2160 (what the game's camera and tile
 * cache take safely, docs/R_step4_plan.md section 0), whatever the monitor. The zoom steps below 1x make the view
 * window / zoom, which on a 1080p monitor is already 3840x2160 at 0.5x, so a capacity taken from the monitor
 * would cap them. Cost: the three big surfaces (primary, back buffer, world surface) are (3840+160) x (2160+21)
 * = 8.7 MB each (plus 64 spare rows), the game's tile cache about as much, the presenter's frame copy and its
 * texture 8.7 MB each: about 55 MB in all, allocated once and mostly untouched while the view is smaller
 * (the pages of a DIB section are committed when first written). PLASTIC_CAP=WxH (test switch) sets a different
 * "largest battlefield" (at least 640x480, at most 3840x2160). */
static void compute_capacity(void) {
    int wmax = 3840, hmax = 2160;
    {   /* test switch PLASTIC_CAP=WxH */
        int cw, ch;
        const char* e = getenv("PLASTIC_CAP");
        if (e && sscanf(e, "%dx%d", &cw, &ch) == 2 && cw >= 640 && ch >= 480) {
            fprintf(stderr, "[ddraw] PLASTIC_CAP: the largest battlefield is %dx%d\n", cw, ch);
            wmax = cw < 3840 ? cw : 3840, hmax = ch < 2160 ? ch : 2160;
        }
    }
    wmax = (wmax + 15) & ~15, hmax = (hmax + 15) & ~15;
    if (wmax < 640) wmax = 640;
    if (hmax < 480) hmax = 480;
    g_cap_w = wmax + 160;               /* the HUD column beside the largest battlefield */
    g_cap_h = hmax + 21 > 480 ? hmax + 21 : 480;
    fprintf(stderr, "[ddraw] capacity %dx%d (largest battlefield %dx%d)\n", g_cap_w, g_cap_h, wmax, hmax);
}

static surf_t* new_surface(int w, int h, DWORD caps, int cap_w, int cap_h) {
    surf_t* s = (surf_t*)calloc(1, sizeof *s);
    s->vtbl = g_surf_vtbl;
    s->refs = 1;
    s->w = w, s->h = h;
    s->cap_w = cap_w > w ? cap_w : w;
    s->cap_h = cap_h > h ? cap_h : h;
    s->pitch = (s->cap_w + 3) & ~3;
    h = s->cap_h;                       /* the DIB below is allocated at capacity */
    s->caps = caps;
    struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } bi = { 0 };
    bi.h.biSize = sizeof bi.h;
    bi.h.biWidth = s->pitch;
    /* top-down, with spare rows behind the last one: 1990s blitters run past it */
    bi.h.biHeight = -(h + 64);
    bi.h.biPlanes = 1;
    bi.h.biBitCount = 8;
    bi.h.biClrUsed = 256;
    for (int i = 0; i < 256; i++) bi.c[i].rgbRed = bi.c[i].rgbGreen = bi.c[i].rgbBlue = (BYTE)i;
    void* bits = NULL;
    s->dib = CreateDIBSection(NULL, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    s->px = (uint8_t*)bits;
    if (!s->px) {
        fprintf(stderr, "[ddraw] CreateDIBSection %dx%d failed (%lu)\n", w, h, GetLastError());
        free(s);
        return NULL;
    }
    return s;
}

static ULONG WINAPI sf_AddRef(surf_t* s) { return (ULONG)InterlockedIncrement(&s->refs); }

static ULONG WINAPI sf_Release(surf_t* s) {
    LONG n = InterlockedDecrement(&s->refs);
    if (n > 0 || s->primary || (g_primary && g_primary->back == s)) return n > 0 ? (ULONG)n : 0;
    if (s == g_world) g_world = NULL;
    if (s->dc) DeleteDC(s->dc);
    if (s->dib) DeleteObject(s->dib);
    free(s);
    return 0;
}

static void clip_rect(const surf_t* s, const RECT* in, RECT* out) {
    if (in) *out = *in; else SetRect(out, 0, 0, s->w, s->h);
    if (out->left < 0) out->left = 0;
    if (out->top < 0) out->top = 0;
    if (out->right > s->w) out->right = s->w;
    if (out->bottom > s->h) out->bottom = s->h;
}

/* Copy src rect (sl,st,w,h) to dst (dl,dt), keyed or not. Overlap-safe: the game
 * scrolls the world surface and the tile cache onto themselves. */
static void copy(surf_t* d, int dl, int dt, surf_t* s, int sl, int st, int w, int h, int keyed) {
    if (dl < 0) { sl -= dl; w += dl; dl = 0; }
    if (dt < 0) { st -= dt; h += dt; dt = 0; }
    if (sl < 0) { dl -= sl; w += sl; sl = 0; }
    if (st < 0) { dt -= st; h += st; st = 0; }
    if (dl + w > d->w) w = d->w - dl;
    if (dt + h > d->h) h = d->h - dt;
    if (sl + w > s->w) w = s->w - sl;
    if (st + h > s->h) h = s->h - st;
    if (w <= 0 || h <= 0) return;
    int down = d == s && dt > st;       /* rows overlap downward: go bottom-up */
    for (int i = 0; i < h; i++) {
        int y = down ? h - 1 - i : i;
        uint8_t* dp = d->px + (dt + y) * d->pitch + dl;
        const uint8_t* sp = s->px + (st + y) * s->pitch + sl;
        if (!keyed) { memmove(dp, sp, (size_t)w); continue; }
        for (int x = 0; x < w; x++) {
            uint8_t v = sp[x];
            if (v < s->key_lo || v > s->key_hi) dp[x] = v;
        }
    }
}

static HRESULT WINAPI sf_Blt(surf_t* d, RECT* dr, surf_t* s, RECT* sr, DWORD flags, DDBLTFX* fx) {
    RECT r;
    clip_rect(d, dr, &r);
    if (flags & DDBLT_COLORFILL) {
        uint8_t c = fx ? (uint8_t)fx->dwFillColor : 0;
        for (int y = r.top; y < r.bottom; y++) memset(d->px + y * d->pitch + r.left, c, (size_t)(r.right - r.left));
        return DD_OK;
    }
    if (!s) return DDERR_INVALIDPARAMS;
    RECT q;
    if (sr) q = *sr; else SetRect(&q, 0, 0, s->w, s->h);
    int keyed = (flags & DDBLT_KEYSRC) && s->has_key;
    int dw = (dr ? dr->right - dr->left : d->w), dh = (dr ? dr->bottom - dr->top : d->h);
    int sw = q.right - q.left, sh = q.bottom - q.top;
    if (dw == sw && dh == sh) {
        copy(d, dr ? dr->left : 0, dr ? dr->top : 0, s, q.left, q.top, sw, sh, keyed);
        return DD_OK;
    }
    /* Sizes differ. The game never stretches on purpose: its cursor draw saves the pixels under the pointer into a 32x32
     * surface and puts them back with the rectangles of the whole surface (0,0,32,32) on one side and the pointer's own
     * (the arrow is 12x16) on the other. DirectDraw stretches such a Blt, so the round trip saves a 12x16 patch stretched
     * to 32x32 and restores it by shrinking: every column but each third comes back shifted by a pixel, and the repaired
     * background is what the next save-under takes. Where the picture under the pointer stays put (a dialog over the
     * frozen world) the pointer then leaves a smear of shifted columns behind it, more with every pass: a bug of the 1999 game
     * (real hardware stretches the same way), invisible while the world is redrawn each frame. What the game means is a
     * 1:1 copy of the smaller of the two rectangles at the origin of each: done so, unless PLASTIC_BLT_STRETCH=1. */
    {
        static int keep = -1;
        if (keep < 0) keep = getenv("PLASTIC_BLT_STRETCH") != NULL;
        if (!keep) {
            int cw = dw < sw ? dw : sw, ch = dh < sh ? dh : sh;
            copy(d, dr ? dr->left : 0, dr ? dr->top : 0, s, q.left, q.top, cw, ch, keyed);
            return DD_OK;
        }
    }
    /* stretch: nearest. The game never asks for it; here for safety */
    int dl = dr ? dr->left : 0, dt = dr ? dr->top : 0;
    for (int y = 0; y < dh; y++) {
        int yy = dt + y, sy = q.top + y * sh / dh;
        if (yy < 0 || yy >= d->h || sy < 0 || sy >= s->h) continue;
        for (int x = 0; x < dw; x++) {
            int xx = dl + x, sx = q.left + x * sw / dw;
            if (xx < 0 || xx >= d->w || sx < 0 || sx >= s->w) continue;
            uint8_t v = s->px[sy * s->pitch + sx];
            if (!keyed || v < s->key_lo || v > s->key_hi) d->px[yy * d->pitch + xx] = v;
        }
    }
    return DD_OK;
}

static HRESULT WINAPI sf_BltFast(surf_t* d, DWORD x, DWORD y, surf_t* s, RECT* sr, DWORD trans) {
    if (!s) return DDERR_INVALIDPARAMS;
    RECT q;
    if (sr) q = *sr; else SetRect(&q, 0, 0, s->w, s->h);
    int keyed = (trans & DDBLTFAST_SRCCOLORKEY) && s->has_key;
    copy(d, (int)x, (int)y, s, q.left, q.top, q.right - q.left, q.bottom - q.top, keyed);
    return DD_OK;
}

/* Flip swaps the two buffers' pixels, as hardware page flipping does: the game's
 * dirty-rect redraw is written for swap semantics. */
static HRESULT WINAPI sf_Flip(surf_t* s, surf_t* target, DWORD flags) {
    (void)target; (void)flags;
    if (!s->primary || !s->back) return DDERR_NOTFLIPPABLE;
    EnterCriticalSection(&g_lock);
    surf_t* b = s->back;
    uint8_t* p = s->px; s->px = b->px; b->px = p;
    HBITMAP h = s->dib; s->dib = b->dib; b->dib = h;
    HDC c = s->dc; s->dc = b->dc; b->dc = c;
    LeaveCriticalSection(&g_lock);
    {                                   /* --hires-log: the game's frame rate is its Flip rate */
        static LARGE_INTEGER freq, last;
        LARGE_INTEGER now;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        if (last.QuadPart) {
            LONG us = (LONG)((now.QuadPart - last.QuadPart) * 1000000 / freq.QuadPart);
            if (us < 2000000) {
                InterlockedExchangeAdd(&vdd_flip_us_sum, us);
                if (us > vdd_flip_us_max) vdd_flip_us_max = us;
                InterlockedIncrement(&vdd_flips);
            }
        }
        last = now;
    }
    if (g_flip_sleep > 0) Sleep((DWORD)g_flip_sleep);
    return DD_OK;
}

static HRESULT WINAPI sf_GetAttachedSurface(surf_t* s, DDSCAPS* caps, surf_t** out) {
    (void)caps;
    if (!s->back) return DDERR_NOTFOUND;
    sf_AddRef(s->back);
    *out = s->back;
    return DD_OK;
}

/* The back buffer's DC, for the font builder: the DIB selected into a memory DC,
 * its colour table the primary's palette, so TextOutA's white maps to an index. */
static HRESULT WINAPI sf_GetDC(surf_t* s, HDC* out) {
    if (!s->dc) s->dc = CreateCompatibleDC(NULL);
    SelectObject(s->dc, s->dib);
    pal_t* p = s->pal ? s->pal : g_primary ? g_primary->pal : NULL;
    if (p) {
        RGBQUAD q[256];
        for (int i = 0; i < 256; i++)
            q[i].rgbRed = p->e[i].peRed, q[i].rgbGreen = p->e[i].peGreen, q[i].rgbBlue = p->e[i].peBlue, q[i].rgbReserved = 0;
        SetDIBColorTable(s->dc, 0, 256, q);
    }
    *out = s->dc;
    return DD_OK;
}

static HRESULT WINAPI sf_ReleaseDC(surf_t* s, HDC dc) {
    (void)dc;
    GdiFlush();                         /* the game Locks and reads the glyph next */
    (void)s;
    return DD_OK;
}

static HRESULT WINAPI sf_GetPalette(surf_t* s, pal_t** out) {
    if (!s->pal) return DDERR_NOPALETTEATTACHED;
    InterlockedIncrement(&s->pal->refs);
    *out = s->pal;
    return DD_OK;
}

static void put_pf(DDPIXELFORMAT* pf) {
    memset(pf, 0, sizeof *pf);
    pf->dwSize = sizeof *pf;
    pf->dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
    pf->dwRGBBitCount = 8;
}

static HRESULT WINAPI sf_GetPixelFormat(surf_t* s, DDPIXELFORMAT* pf) {
    (void)s;
    put_pf(pf);
    return DD_OK;
}

static void put_desc(const surf_t* s, DDSURFACEDESC* d) {
    memset(d, 0, sizeof *d);
    d->dwSize = sizeof *d;
    d->dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
    d->dwWidth = (DWORD)s->w;
    d->dwHeight = (DWORD)s->h;
    d->lPitch = s->pitch;
    d->ddsCaps.dwCaps = s->caps;
    put_pf(&d->ddpfPixelFormat);
}

static HRESULT WINAPI sf_GetSurfaceDesc(surf_t* s, DDSURFACEDESC* d) { put_desc(s, d); return DD_OK; }
static HRESULT WINAPI sf_IsLost(surf_t* s) { (void)s; return DD_OK; }
static HRESULT WINAPI sf_Restore(surf_t* s) { (void)s; return DD_OK; }

static HRESULT WINAPI sf_Lock(surf_t* s, RECT* r, DDSURFACEDESC* d, DWORD flags, HANDLE ev) {
    (void)flags; (void)ev;
    put_desc(s, d);
    d->dwFlags |= DDSD_LPSURFACE;
    d->lpSurface = r ? s->px + r->top * s->pitch + r->left : s->px;
    return DD_OK;
}

static HRESULT WINAPI sf_Unlock(surf_t* s, void* p) { (void)s; (void)p; return DD_OK; }

static HRESULT WINAPI sf_SetColorKey(surf_t* s, DWORD flags, DDCOLORKEY* k) {
    if (!(flags & DDCKEY_SRCBLT)) return DD_OK;
    if (!k) { s->has_key = 0; return DD_OK; }
    s->has_key = 1;
    s->key_lo = k->dwColorSpaceLowValue & 0xFF;
    s->key_hi = k->dwColorSpaceHighValue & 0xFF;
    return DD_OK;
}

static HRESULT WINAPI sf_SetPalette(surf_t* s, pal_t* p) {
    EnterCriticalSection(&g_lock);
    if (p) InterlockedIncrement(&p->refs);
    pal_t* old = s->pal;
    s->pal = p;
    LeaveCriticalSection(&g_lock);
    if (old) InterlockedDecrement(&old->refs);   /* palettes are kept: the presenter may hold one */
    return DD_OK;
}

static HRESULT WINAPI sf_QueryInterface(surf_t* s, REFIID iid, void** out) {
    (void)s; (void)iid;
    *out = NULL;
    trace_unimplemented("IDirectDrawSurface::QueryInterface");
    return E_NOINTERFACE;
}

static const void* const g_surf_vtbl[36] = {
    sf_QueryInterface, sf_AddRef, sf_Release,
    stub2,              /*  3 AddAttachedSurface */
    stub2,              /*  4 AddOverlayDirtyRect */
    sf_Blt,             /*  5 */
    stub4,              /*  6 BltBatch */
    sf_BltFast,         /*  7 */
    stub3,              /*  8 DeleteAttachedSurface */
    stub3,              /*  9 EnumAttachedSurfaces */
    stub4,              /* 10 EnumOverlayZOrders */
    sf_Flip,            /* 11 */
    sf_GetAttachedSurface, /* 12 */
    stub2,              /* 13 GetBltStatus */
    stub2,              /* 14 GetCaps */
    stub2,              /* 15 GetClipper */
    stub3,              /* 16 GetColorKey */
    sf_GetDC,           /* 17 */
    stub2,              /* 18 GetFlipStatus */
    stub3,              /* 19 GetOverlayPosition */
    sf_GetPalette,      /* 20 */
    sf_GetPixelFormat,  /* 21 */
    sf_GetSurfaceDesc,  /* 22 */
    stub3,              /* 23 Initialize */
    sf_IsLost,          /* 24 */
    sf_Lock,            /* 25 */
    sf_ReleaseDC,       /* 26 */
    sf_Restore,         /* 27 */
    stub2,              /* 28 SetClipper */
    sf_SetColorKey,     /* 29 */
    stub3,              /* 30 SetOverlayPosition */
    sf_SetPalette,      /* 31 */
    sf_Unlock,          /* 32 */
    stub6,              /* 33 UpdateOverlay */
    stub2,              /* 34 UpdateOverlayDisplay */
    stub3,              /* 35 UpdateOverlayZOrder */
};

/* ----------------------------------------------------------------- palettes */

static ULONG WINAPI pl_AddRef(pal_t* p) { return (ULONG)InterlockedIncrement(&p->refs); }
static ULONG WINAPI pl_Release(pal_t* p) {
    LONG n = InterlockedDecrement(&p->refs);
    return n > 0 ? (ULONG)n : 0;        /* never freed: cheap, and the presenter may hold it */
}
static HRESULT WINAPI pl_SetEntries(pal_t* p, DWORD flags, DWORD start, DWORD count, PALETTEENTRY* e) {
    (void)flags;
    if (start > 256) return DDERR_INVALIDPARAMS;
    if (count > 256 - start) count = 256 - start;
    EnterCriticalSection(&g_lock);
    memcpy(p->e + start, e, count * sizeof *e);
    LeaveCriticalSection(&g_lock);
    return DD_OK;
}
static HRESULT WINAPI pl_GetEntries(pal_t* p, DWORD flags, DWORD start, DWORD count, PALETTEENTRY* e) {
    (void)flags;
    if (start > 256) return DDERR_INVALIDPARAMS;
    if (count > 256 - start) count = 256 - start;
    memcpy(e, p->e + start, count * sizeof *e);
    return DD_OK;
}
static HRESULT WINAPI pl_QueryInterface(pal_t* p, REFIID iid, void** out) {
    (void)p; (void)iid; *out = NULL; return E_NOINTERFACE;
}
static const void* const g_pal_vtbl[7] = {
    pl_QueryInterface, pl_AddRef, pl_Release, stub2 /* GetCaps */, pl_GetEntries, stub4 /* Initialize */, pl_SetEntries,
};

/* -------------------------------------------------------------- DirectDraw */

static ULONG WINAPI dd_AddRef(void* t) { (void)t; return (ULONG)InterlockedIncrement(&g_dd_refs); }
static ULONG WINAPI dd_Release(void* t) { (void)t; LONG n = InterlockedDecrement(&g_dd_refs); return n > 0 ? (ULONG)n : 0; }

static HRESULT WINAPI dd_QueryInterface(void* t, REFIID iid, void** out) {
    (void)t;
    if (IsEqualGUID(iid, &IID_IDirectDraw2)) { *out = &g_dd2; dd_AddRef(t); return DD_OK; }
    if (IsEqualGUID(iid, &IID_IDirectDraw) || IsEqualGUID(iid, &IID_IUnknown)) { *out = &g_dd1; dd_AddRef(t); return DD_OK; }
    *out = NULL;
    trace_unimplemented("IDirectDraw::QueryInterface (other interface)");
    return E_NOINTERFACE;
}

static HRESULT WINAPI dd_CreatePalette(void* t, DWORD flags, PALETTEENTRY* e, pal_t** out, IUnknown* outer) {
    (void)t; (void)flags; (void)outer;
    pal_t* p = (pal_t*)calloc(1, sizeof *p);
    p->vtbl = g_pal_vtbl;
    p->refs = 1;
    if (e) memcpy(p->e, e, sizeof p->e);
    *out = p;
    return DD_OK;
}

static DWORD WINAPI presenter(LPVOID unused);
extern volatile LONG vdd_presented;

static HRESULT WINAPI dd_CreateSurface(void* t, DDSURFACEDESC* d, surf_t** out, IUnknown* outer) {
    (void)t; (void)outer;
    DWORD caps = (d->dwFlags & DDSD_CAPS) ? d->ddsCaps.dwCaps : 0;
    surf_t* s;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        if (!g_cap_w) compute_capacity();
        s = new_surface(g_mode_w, g_mode_h, caps, g_cap_w, g_cap_h);
        if (!s) return DDERR_OUTOFMEMORY;
        s->primary = 1;
        if ((d->dwFlags & DDSD_BACKBUFFERCOUNT) && d->dwBackBufferCount) {
            s->back = new_surface(g_mode_w, g_mode_h, DDSCAPS_BACKBUFFER, g_cap_w, g_cap_h);
            if (!s->back) return DDERR_OUTOFMEMORY;
        }
        g_primary = s;
        g_world_pending = 1;
        if (!InterlockedExchange(&g_running, 1)) CloseHandle(CreateThread(NULL, 0, presenter, NULL, 0, NULL));
    } else {
        int w = (d->dwFlags & DDSD_WIDTH) ? (int)d->dwWidth : g_mode_w;
        int h = (d->dwFlags & DDSD_HEIGHT) ? (int)d->dwHeight : g_mode_h;
        /* the first offscreen surface after the primary, at the mode's size, is the world surface
         * (sub_0041AA10 creates it right after the flip chain): capacity too */
        int big = g_world_pending && w == g_mode_w && h == g_mode_h;
        g_world_pending = 0;
        s = new_surface(w, h, caps, big ? g_cap_w : 0, big ? g_cap_h : 0);
        if (!s) return DDERR_OUTOFMEMORY;
        if (big) g_world = s;
    }
    if (vdd_trace) fprintf(stderr, "[ddraw] CreateSurface caps 0x%lX -> %dx%d (capacity %dx%d) %p\n", caps, s->w, s->h, s->cap_w, s->cap_h, (void*)s);
    *out = s;
    return DD_OK;
}

static HRESULT WINAPI dd_FlipToGDISurface(void* t) { (void)t; return DD_OK; }
static HRESULT WINAPI dd_GetGDISurface(void* t, surf_t** out) {
    (void)t;
    if (!g_primary) return DDERR_NOTFOUND;
    sf_AddRef(g_primary);
    *out = g_primary;
    return DD_OK;
}
static HRESULT WINAPI dd_RestoreDisplayMode(void* t) { (void)t; return DD_OK; }

/* The game runs its fullscreen path, which pauses everything when it loses
 * focus: WM_ACTIVATEAPP(FALSE) stops RunFrame (flag 0x004FA02C) and a movie only
 * advances while GetFocus() is its window (0x004453C0). In a window that froze
 * the intro the moment the owner clicked elsewhere (2026-10-26). cnc-ddraw kept
 * the game active; so does this subclass, by swallowing the deactivations. The
 * guest's window procedure is called through CallWindowProcA, natively: the
 * fetch faults and native32 runs the lift. GetFocus is shimmed in main.c. */
static WNDPROC g_guest_proc;
static void ui_step(int dir);           /* layout, below */

/* ---- host hotkeys and the title bar -------------------------------------------------------------
 * F12 scaling mode, F11 / Alt+Enter borderless fullscreen, F3 FPS readout, F10 / PrintScreen screenshot.
 * They run on the window's thread (this window procedure). The title bar carries the readouts: a notice
 * (the mode's name, the screenshot's file) for 2 s after its event, the FPS line while F3 is on; a timer
 * (WM_TIMER, swallowed here) refreshes it. Borderless fullscreen has no title bar to show them in. */
#define HOST_TIMER 0x7A51
static char g_title_base[128], g_title_cur[320], g_notice[200];
static DWORD g_notice_until;
static int g_show_fps;
static char g_shot_dir[MAX_PATH] = "screenshots";   /* main.c sets <game folder>\screenshots: the game changes the working folder */
void vdd_set_screenshot_dir(const char* dir) { strncpy(g_shot_dir, dir, MAX_PATH - 1); g_shot_dir[MAX_PATH - 1] = 0; }
static char g_fps_text[160];
static volatile LONG g_dump_req;        /* F10 with PLASTIC_DUMP_VIEW set: the presenter also writes the raw view (below) */
volatile LONG vdd_present_us_max;       /* the longest gap between two presents, microseconds (reset when read) */
static int g_fullscreen, g_fs_wanted;
static WINDOWPLACEMENT g_wp;
static LONG g_wstyle;

static void notice(const char* fmt, const char* a) {
    _snprintf(g_notice, sizeof g_notice - 1, fmt, a);
    g_notice[sizeof g_notice - 1] = 0;
    g_notice_until = GetTickCount() + 2000;
    fprintf(stderr, "[host] %s\n", g_notice);
}

void vdd_notice(const char* text) { notice("%s", text); }

static void update_title(void) {
    static DWORD t0;
    static LONG p0, f0;
    char buf[sizeof g_title_cur], shot[MAX_PATH];
    DWORD now = GetTickCount();
    if (!g_hwnd) return;
    if (!g_title_base[0]) {
        GetWindowTextA(g_hwnd, g_title_base, sizeof g_title_base);
        if (!g_title_base[0]) strcpy(g_title_base, "Armymen II");
    }
    if (present_screenshot_result(shot, sizeof shot)) {
        if (shot[0]) { const char* b = strrchr(shot, '\\'); notice("Screenshot: %s", b ? b + 1 : shot); }
        else notice("%s", "Screenshot failed");
    }
    if (g_show_fps && now - t0 >= 500) {
        LONG p = vdd_presented, f = vdd_flips, mx = InterlockedExchange(&vdd_present_us_max, 0);
        double dt = (now - t0) / 1000.0;
        if (t0 && dt < 5.0)
        {
            hires_view_t hv;
            char zt[64] = "";
            if (hires_view(&hv) && !hv.dialog) _snprintf(zt, sizeof zt - 1, "  zoom %.3gx view %dx%d", hv.eff, hv.ww, hv.wh);
            _snprintf(g_fps_text, sizeof g_fps_text - 1, "%.1f fps  %.1f ms (max %.1f)  game %.0f loops/s%s",
                      (p - p0) / dt, (p - p0) ? dt * 1000.0 / (p - p0) : 0.0, mx / 1000.0, (f - f0) / dt, zt);
        }
        t0 = now, p0 = p, f0 = f;
    }
    strcpy(buf, g_title_base);
    if (now < g_notice_until) { strncat(buf, "  -  ", sizeof buf - strlen(buf) - 1); strncat(buf, g_notice, sizeof buf - strlen(buf) - 1); }
    if (g_show_fps) { strncat(buf, "  -  ", sizeof buf - strlen(buf) - 1); strncat(buf, g_fps_text, sizeof buf - strlen(buf) - 1); }
    if (strcmp(buf, g_title_cur)) {
        strcpy(g_title_cur, buf);
        SetWindowTextA(g_hwnd, buf);
    }
}

/* Borderless fullscreen on the monitor the window is on; leaving restores the style and the window rect
 * (maximised or not). The hires layout follows the client size on its own (hires.c panel_update). */
static void set_fullscreen(int on) {
    if (!g_hwnd) { g_fs_wanted = on; return; }
    if (on == g_fullscreen) return;
    if (on) {
        MONITORINFO mi = { sizeof mi };
        g_wp.length = sizeof g_wp;
        GetWindowPlacement(g_hwnd, &g_wp);
        g_wstyle = GetWindowLongA(g_hwnd, GWL_STYLE);
        if (IsZoomed(g_hwnd)) ShowWindow(g_hwnd, SW_SHOWNORMAL);
        GetMonitorInfoA(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongA(g_hwnd, GWL_STYLE, (LONG)(WS_POPUP | WS_VISIBLE));
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongA(g_hwnd, GWL_STYLE, g_wstyle);
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        SetWindowPlacement(g_hwnd, &g_wp);
    }
    g_fullscreen = on;
    notice("%s", on ? "Borderless fullscreen" : "Windowed");
}

int vdd_get_fullscreen(void) { return g_fullscreen || (!g_hwnd && g_fs_wanted); }
void vdd_set_fullscreen(int on) { set_fullscreen(on); }

/* ---- mouse confine (F8) and edge scroll (F9) ------------------------------------------------------
 * The confine is ClipCursor to the window's client area, held only while a mission is played (hires_play_live)
 * and the game's window is the foreground one; the 250 ms timer applies and drops it, window messages drop it
 * at once, the ExitProcess shim and the crash and watchdog reports release it. A clip left behind by a dead
 * process is reset by Windows, but the cursor is the user's, so every exit path lets go itself. */
static volatile LONG g_confine = 1, g_edge_scroll = 1;
static volatile LONG g_clip_on;
static RECT g_clip_rc;
int  vdd_get_confine(void) { return (int)g_confine; }
void vdd_set_confine(int on) { InterlockedExchange(&g_confine, on != 0); }
int  vdd_get_edge_scroll(void) { return (int)g_edge_scroll; }
void vdd_set_edge_scroll(int on) { InterlockedExchange(&g_edge_scroll, on != 0); }

void vdd_release_cursor(void) {
    if (g_clip_on) {
        ClipCursor(NULL);
        g_clip_on = 0;
        fprintf(stderr, "[host] mouse released\n");
    }
}

static void update_confine(void) {
    RECT rc;
    POINT a, b;
    int want = g_confine && g_hwnd && !IsIconic(g_hwnd) && GetForegroundWindow() == g_hwnd && hires_play_live();
    if (!want || !GetClientRect(g_hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) { vdd_release_cursor(); return; }
    a.x = rc.left, a.y = rc.top, b.x = rc.right, b.y = rc.bottom;
    ClientToScreen(g_hwnd, &a);
    ClientToScreen(g_hwnd, &b);
    RECT r = { a.x, a.y, b.x, b.y };
    if (!g_clip_on || memcmp(&r, &g_clip_rc, sizeof r)) {
        if (ClipCursor(&r)) {
            if (!g_clip_on) fprintf(stderr, "[host] mouse confined to %ld,%ld-%ld,%ld\n", r.left, r.top, r.right, r.bottom);
            g_clip_on = 1, g_clip_rc = r;
        }
    }
}

/* The host's own keys. Returns 1 when the message is taken. */
static int host_key(UINT msg, WPARAM wp, LPARAM lp) {
    int rep = (lp & 0x40000000) != 0;
    if (msg == WM_KEYUP && wp == VK_SNAPSHOT) { present_request_screenshot(g_shot_dir); InterlockedExchange(&g_dump_req, 1); return 1; }
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return 0;
    if (wp == VK_F12) {
        if (!rep) notice("Scaling: %s", present_mode_name(present_cycle_mode((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1)));
        return 1;
    }
    if (wp == VK_F11 || (wp == VK_RETURN && msg == WM_SYSKEYDOWN && (lp & (1 << 29)))) {
        if (!rep) set_fullscreen(!g_fullscreen);
        return 1;
    }
    if (wp == VK_F3) {
        if (!rep) { g_show_fps = !g_show_fps; if (!g_show_fps) g_fps_text[0] = 0; notice("FPS readout %s", g_show_fps ? "on" : "off"); }
        return 1;
    }
    if (wp == VK_F10) {
        if (!rep) { present_request_screenshot(g_shot_dir); InterlockedExchange(&g_dump_req, 1); }
        return 1;
    }
    if (wp == VK_F8) {
        if (!rep) { vdd_set_confine(!g_confine); notice("Mouse confine %s", g_confine ? "on" : "off"); update_confine(); }
        return 1;
    }
    if (wp == VK_F9) {
        if (!rep) { vdd_set_edge_scroll(!g_edge_scroll); notice("Edge scroll %s", g_edge_scroll ? "on" : "off"); }
        return 1;
    }
    return 0;
}
static LRESULT CALLBACK keep_active(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (vdd_trace && (msg == WM_ACTIVATEAPP || msg == WM_ACTIVATE || msg == WM_SETFOCUS || msg == WM_KILLFOCUS))
        fprintf(stderr, "[ddraw] window msg 0x%X wp=%lX\n", msg, (unsigned long)wp);
    if (msg == WM_CLOSE || msg == WM_DESTROY || msg == WM_QUIT || (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_CLOSE))
        fprintf(stderr, "[ddraw] window msg 0x%X wp=%lX (closing)\n", msg, (unsigned long)wp);
    if (msg == WM_TIMER && wp == HOST_TIMER) { update_title(); update_confine(); return 0; }
    if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATEAPP && !wp) || (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) ||
        (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_MINIMIZE))
        vdd_release_cursor();                                            /* Alt-Tab, a click elsewhere, minimise */
    if (host_key(msg, wp, lp)) { update_title(); return 0; }
    if (msg == WM_KEYDOWN && (wp == VK_OEM_PLUS || wp == VK_ADD || wp == VK_OEM_MINUS || wp == VK_SUBTRACT)) {
        int dir = (wp == VK_OEM_PLUS || wp == VK_ADD) ? +1 : -1;
        if (!hires_zoom_keys_ok()) { /* typing in a text field, or a dialog: not ours */ }
        else if (GetKeyState(VK_SHIFT) & 0x8000) ui_step(dir);        /* the HUD's scale */
        else if (!(lp & 0x40000000)) hires_zoom_step(dir);            /* the battlefield's zoom (not on auto-repeat) */
    }
    if (msg == WM_ACTIVATEAPP && !wp) return 0;
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) return 0;
    return CallWindowProcA(g_guest_proc, hwnd, msg, wp, lp);
}

/* The game's window becomes an ordinary resizable window, sized to the largest
 * whole multiple of the mode that fits 90% of the work area. */
static void make_window(HWND hwnd) {
    if (!getenv("PLASTIC_NO_SUBCLASS")) {   /* test switch */
        g_guest_proc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)keep_active);
        SetTimer(hwnd, HOST_TIMER, 250, NULL);
    }
    if (getenv("PLASTIC_NO_RESTYLE")) return;
    RECT work;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    int ww = work.right - work.left, wh = work.bottom - work.top, k = 1;
    while ((k + 1) * g_mode_w <= ww * 9 / 10 && (k + 1) * g_mode_h <= wh * 9 / 10) k++;
    DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    SetWindowLongA(hwnd, GWL_STYLE, (LONG)style);
    RECT r = { 0, 0, k * g_mode_w, k * g_mode_h };
    AdjustWindowRectEx(&r, style, FALSE, (DWORD)GetWindowLongA(hwnd, GWL_EXSTYLE));
    int w = r.right - r.left, h = r.bottom - r.top;
    SetWindowPos(hwnd, HWND_NOTOPMOST, work.left + (ww - w) / 2, work.top + (wh - h) / 2, w, h,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

static HRESULT WINAPI dd_SetCooperativeLevel(void* t, HWND hwnd, DWORD flags) {
    (void)t;
    if (vdd_trace) fprintf(stderr, "[ddraw] SetCooperativeLevel(%p, 0x%lX)\n", (void*)hwnd, flags);
    if (hwnd && !g_hwnd) {
        g_hwnd = hwnd;
        make_window(hwnd);
        if (g_fs_wanted) { g_fs_wanted = 0; set_fullscreen(1); }
    }
    return DD_OK;
}

static HRESULT WINAPI dd1_SetDisplayMode(void* t, DWORD w, DWORD h, DWORD bpp) {
    (void)t;
    if (bpp != 8) return DDERR_UNSUPPORTEDMODE;
    g_mode_w = (int)w, g_mode_h = (int)h;
    return DD_OK;
}
static HRESULT WINAPI dd2_SetDisplayMode(void* t, DWORD w, DWORD h, DWORD bpp, DWORD rate, DWORD flags) {
    (void)rate; (void)flags;
    return dd1_SetDisplayMode(t, w, h, bpp);
}
static HRESULT WINAPI dd2_GetAvailableVidMem(void* t, DDSCAPS* caps, DWORD* total, DWORD* freemem) {
    (void)t; (void)caps;
    if (total) *total = 256u << 20;
    if (freemem) *freemem = 256u << 20;
    return DD_OK;
}

#define DD_COMMON(SETMODE) \
    dd_QueryInterface, dd_AddRef, dd_Release, \
    stub1,                  /*  3 Compact */ \
    stub4,                  /*  4 CreateClipper */ \
    dd_CreatePalette,       /*  5 */ \
    dd_CreateSurface,       /*  6 */ \
    stub3,                  /*  7 DuplicateSurface */ \
    stub5,                  /*  8 EnumDisplayModes */ \
    stub5,                  /*  9 EnumSurfaces */ \
    dd_FlipToGDISurface,    /* 10 */ \
    stub3,                  /* 11 GetCaps */ \
    stub2,                  /* 12 GetDisplayMode */ \
    stub3,                  /* 13 GetFourCCCodes */ \
    dd_GetGDISurface,       /* 14 */ \
    stub2,                  /* 15 GetMonitorFrequency */ \
    stub2,                  /* 16 GetScanLine */ \
    stub2,                  /* 17 GetVerticalBlankStatus */ \
    stub2,                  /* 18 Initialize */ \
    dd_RestoreDisplayMode,  /* 19 */ \
    dd_SetCooperativeLevel, /* 20 */ \
    SETMODE,                /* 21 */ \
    stub3                   /* 22 WaitForVerticalBlank */
static const void* const g_dd1_vtbl[23] = { DD_COMMON(dd1_SetDisplayMode) };
static const void* const g_dd2_vtbl[24] = { DD_COMMON(dd2_SetDisplayMode), dd2_GetAvailableVidMem };

/* ------------------------------------------------------------------ layout */

/* In a mission (g_gameState 0x00511DA4 == 2) the HUD is three opaque, fixed
 * rectangles the game paints over the battlefield every frame: the top strip
 * (0,0)-(640,21), and the panel plus the 16-px edge strip from the
 * battlefield's right edge BLIT_RECT.right (0x00485328: 480 with the panel
 * open, 624 closed) to 640, from y 21 down. So the frame is presented as
 * regions: the battlefield at the world scale, where it always was, and the
 * two HUD pieces at their own UI scale, anchored to the picture's top-left and
 * top-right. Menus and movies are the whole frame. No lifted code changes.
 *
 * UI scale: 0 follows the world's fit scale (the picture as it always was).
 * Shift+Plus / Shift+Minus step it by halves between 1 (native pixels) and the
 * fit scale; it never exceeds the fit, so the 459-px-tall panel always fits.
 * (Plus / Minus alone are kept for the battlefield zoom of step 4.) */
#define ADDR_GAME_STATE 0x00511DA4u
#define ADDR_BLIT_RIGHT 0x00485328u
#define STATE_MISSION 2
#define STRIP_Y 21                      /* height of the top strip */

static volatile LONG g_ui_halves;       /* UI scale x2; 0 = follow the world */
int  vdd_get_ui_halves(void) { return (int)g_ui_halves; }
void vdd_set_ui_halves(int h) { InterlockedExchange(&g_ui_halves, h < 0 ? 0 : h); }

static void ui_step(int dir) {
    RECT rc;
    if (!g_hwnd || !GetClientRect(g_hwnd, &rc)) return;
    double fit = (double)rc.right / g_mode_w < (double)rc.bottom / g_mode_h
               ? (double)rc.right / g_mode_w : (double)rc.bottom / g_mode_h;
    int max_h = (int)floor(fit * 2), cur = g_ui_halves ? (int)g_ui_halves : max_h;
    cur += dir;
    if (cur < 2) cur = 2;
    if (cur >= max_h) cur = 0;          /* back to following the world */
    InterlockedExchange(&g_ui_halves, cur);
    if (vdd_trace) fprintf(stderr, "[ddraw] UI scale %s\n", cur ? "set" : "follows the world");
}

static int layout(int cw, int ch, int w, int h, present_region_t* rg) {
    /* test switches: PLASTIC_FORCE_LAYOUT=1 applies the mission layout anywhere,
     * PLASTIC_UI_HALVES=n presets the UI scale (n/2) */
    static int force = -1;
    if (force < 0) {
        const char* f = getenv("PLASTIC_FORCE_LAYOUT"), *u = getenv("PLASTIC_UI_HALVES");
        force = f && *f == '1';
        if (u) g_ui_halves = atoi(u);
    }
    /* Outside missions (menus, movies, loading) the picture is the 640x480 corner of the primary,
     * letterboxed whole, however large the primary's logical size is. A frame of exactly 640x480
     * takes present_frame_regions' own whole-frame path. */
    const int MW = 640, MH = 480;
    if (w < MW || h < MH) return 0;
    hires_view_t hv;
    int hires = hires_view(&hv) && hv.a == w && hv.hc == h && !hv.dialog;   /* a dialog is the 640x480 corner, like a menu */
    if (!hires && (w != MW || h != MH || (!force && (int)MEM32(ADDR_GAME_STATE) != STATE_MISSION))) {
        if (w == MW && h == MH) return 0;
        double f = present_fit_scale(cw, ch, MW, MH);
        rg[0].sx = 0, rg[0].sy = 0, rg[0].sw = MW, rg[0].sh = MH;
        rg[0].dw = (int)(MW * f + 0.5), rg[0].dh = (int)(MH * f + 0.5);
        rg[0].dx = (cw - rg[0].dw) / 2, rg[0].dy = (ch - rg[0].dh) / 2;
        return 1;
    }
    /* the UI scale: the HUD pieces' size; never beyond what makes the 640x480 mission screen fit */
    double s = present_fit_scale(cw, ch, MW, MH);
    double u = g_ui_halves ? g_ui_halves / 2.0 : s;
    if (u > s) u = s;
    int n = 0;
    if (hires) {
        /* step 4: the battlefield fills the window (src (0,21,Ww,Wh) of the frame), the HUD is drawn over it:
         * the top strip across the window's top, the panel column anchored to the right edge */
        rg[n].sx = 0, rg[n].sy = STRIP_Y, rg[n].sw = hv.ww, rg[n].sh = hv.wh;
        double zr = floor(hv.zoom + 0.5);
        if (!hv.capped && present_get_mode() == PRESENT_NEAREST && zr >= 1.0 && fabs(hv.zoom - zr) < 1e-6) {
            /* nearest at a whole zoom: every game pixel is zr x zr screen pixels, the odd rest of the window is
             * a border (at most zr - 1 pixels a side) and the view's last row and column may be cut */
            int z = (int)zr;
            rg[n].sw = hv.ww < cw / z ? hv.ww : cw / z;
            rg[n].sh = hv.wh < ch / z ? hv.wh : ch / z;
            rg[n].dw = rg[n].sw * z, rg[n].dh = rg[n].sh * z;
            rg[n].dx = (cw - rg[n].dw) / 2, rg[n].dy = (ch - rg[n].dh) / 2;
        } else if (!hv.capped) {
            rg[n].dx = 0, rg[n].dy = 0, rg[n].dw = cw, rg[n].dh = ch;
        } else {                        /* the view cannot cover the window: one uniform scale, centred */
            double k = present_fit_scale(cw, ch, hv.ww, hv.wh);
            rg[n].dw = (int)(hv.ww * k + 0.5), rg[n].dh = (int)(hv.wh * k + 0.5);
            rg[n].dx = (cw - rg[n].dw) / 2, rg[n].dy = (ch - rg[n].dh) / 2;
        }
        n++;
        rg[n].sx = 0, rg[n].sy = 0, rg[n].sw = MW, rg[n].sh = STRIP_Y;
        rg[n].dx = 0, rg[n].dy = 0, rg[n].dw = cw, rg[n].dh = (int)(STRIP_Y * u + 0.5);
        n++;
        int right = hv.hud_right;
        if (right < 1 || right > hv.a - 1) right = hv.a - 16;
        rg[n].sx = right, rg[n].sy = STRIP_Y, rg[n].sw = hv.a - right, rg[n].sh = MH - STRIP_Y;
        rg[n].dw = (int)((hv.a - right) * u + 0.5), rg[n].dh = (int)((MH - STRIP_Y) * u + 0.5);
        rg[n].dx = cw - rg[n].dw, rg[n].dy = (int)(STRIP_Y * u + 0.5);
        n++;
        return n;
    }
    int pw = (int)(MW * s + 0.5), ph = (int)(MH * s + 0.5);
    int px = (cw - pw) / 2, py = (ch - ph) / 2;
    int right = (int)MEM32(ADDR_BLIT_RIGHT);
    if (right < 1 || right > MW) right = MW - 16;
    /* the battlefield, where it always was */
    rg[n].sx = 0, rg[n].sy = STRIP_Y, rg[n].sw = right, rg[n].sh = MH - STRIP_Y;
    rg[n].dx = px, rg[n].dy = py + (int)(STRIP_Y * s + 0.5);
    rg[n].dw = (int)(right * s + 0.5), rg[n].dh = py + ph - rg[n].dy;
    n++;
    /* the top strip (the message/chat line): the full width of the window,
     * stretched sideways, at the UI scale's height (owner, 2026-10-26) */
    rg[n].sx = 0, rg[n].sy = 0, rg[n].sw = MW, rg[n].sh = STRIP_Y;
    rg[n].dx = 0, rg[n].dy = 0, rg[n].dw = cw, rg[n].dh = (int)(STRIP_Y * u + 0.5);
    n++;
    /* the panel and edge strip, anchored to the window's right edge, under the strip */
    rg[n].sx = right, rg[n].sy = STRIP_Y, rg[n].sw = MW - right, rg[n].sh = MH - STRIP_Y;
    rg[n].dw = (int)((MW - right) * u + 0.5), rg[n].dh = (int)((MH - STRIP_Y) * u + 0.5);
    rg[n].dx = cw - rg[n].dw, rg[n].dy = (int)(STRIP_Y * u + 0.5);
    n++;
    (void)px; (void)pw;
    return n;
}

/* ---------------------------------------------------------------- presenter */

/* Test switch PLASTIC_DUMP_LOAD=<dir>: while the game is loading a mission (from the map load to the
 * first panel update, hires.c), the presenter writes the frame it is about to show to
 * <dir>\load_<n>_<w>x<h>_state<s>.bmp every 100 ms (at most 30 of them), to see what the loading
 * screen looks like whatever the geometry. */
static void dump_load_frame(const uint8_t* frame, int w, int h, const PALETTEENTRY* pal) {
    static const char* dir = (const char*)1;
    static int n;
    static DWORD last;
    if (dir == (const char*)1) dir = getenv("PLASTIC_DUMP_LOAD");
    if (!dir || !*dir || n >= 30 || !hires_loading()) return;
    DWORD now = GetTickCount();
    if (n && now - last < 100) return;
    last = now;
    char path[MAX_PATH];
    _snprintf(path, sizeof path - 1, "%s\\load_%02d_%dx%d_state%d.bmp", dir, n++, w, h, (int)MEM32(ADDR_GAME_STATE));
    path[sizeof path - 1] = 0;
    FILE* f = fopen(path, "wb");
    if (!f) return;
    int rowb = (w + 3) & ~3;
    BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(14 + 40 + 1024 + rowb * h), 0, 0, 14 + 40 + 1024 };
    BITMAPINFOHEADER ih = { sizeof ih, w, h, 1, 8, BI_RGB, (DWORD)(rowb * h), 0, 0, 256, 0 };
    RGBQUAD q[256];
    for (int i = 0; i < 256; i++) q[i].rgbRed = pal[i].peRed, q[i].rgbGreen = pal[i].peGreen, q[i].rgbBlue = pal[i].peBlue, q[i].rgbReserved = 0;
    fwrite(&fh, 1, sizeof fh, f), fwrite(&ih, 1, sizeof ih, f), fwrite(q, 1, sizeof q, f);
    uint8_t* row = (uint8_t*)calloc(1, (size_t)rowb);
    for (int y = h - 1; y >= 0; y--) {
        memcpy(row, frame + (size_t)y * w, (size_t)w);
        fwrite(row, 1, (size_t)rowb, f);
    }
    free(row);
    fclose(f);
}

/* Test switch PLASTIC_DUMP_VIEW=<dir>: F10 (and PrintScreen) also write the battlefield as the game drew it, one
 * pixel per game pixel with no scaling, to <dir>\view_<n>_<w>x<h>.bmp (8-bit, with the palette of that moment), to
 * look for objects the game did not draw (the depth list's 500 nodes) at a view the window cannot show whole. */
static void dump_view(const uint8_t* frame, int fw, int fh, const PALETTEENTRY* pal) {
    static int n;
    const char* dir = getenv("PLASTIC_DUMP_VIEW");
    hires_view_t hv;
    if (!dir || !*dir || !InterlockedExchange(&g_dump_req, 0) || !hires_view(&hv) || hv.dialog || hv.a != fw || hv.hc != fh) return;
    int x0 = 0, y0 = STRIP_Y, w = hv.ww, h = hv.wh;
    char path[MAX_PATH];
    _snprintf(path, sizeof path - 1, "%s\\view_%02d_%dx%d.bmp", dir, n++, w, h);
    path[sizeof path - 1] = 0;
    FILE* f = fopen(path, "wb");
    if (!f) return;
    int rowb = (w + 3) & ~3;
    BITMAPFILEHEADER fhd = { 0x4D42, (DWORD)(14 + 40 + 1024 + rowb * h), 0, 0, 14 + 40 + 1024 };
    BITMAPINFOHEADER ih = { sizeof ih, w, h, 1, 8, BI_RGB, (DWORD)(rowb * h), 0, 0, 256, 0 };
    RGBQUAD q[256];
    for (int i = 0; i < 256; i++) q[i].rgbRed = pal[i].peRed, q[i].rgbGreen = pal[i].peGreen, q[i].rgbBlue = pal[i].peBlue, q[i].rgbReserved = 0;
    fwrite(&fhd, 1, sizeof fhd, f), fwrite(&ih, 1, sizeof ih, f), fwrite(q, 1, sizeof q, f);
    uint8_t* row = (uint8_t*)calloc(1, (size_t)rowb);
    for (int y = h - 1; y >= 0; y--) {
        memcpy(row, frame + (size_t)(y0 + y) * fw + x0, (size_t)w);
        fwrite(row, 1, (size_t)rowb, f);
    }
    free(row);
    fclose(f);
    fprintf(stderr, "[host] view dump %s\n", path);
}

/* Shows the primary about 60 times a second (present_frame waits for vsync). It
 * owns the D3D device, created here, on this thread only. The pixels are copied
 * under the lock, so a Flip never happens halfway through a copy. */
static DWORD WINAPI presenter(LPVOID unused) {
    (void)unused;
    while (!g_hwnd) Sleep(5);
    /* The game's loop is unthrottled (thousands of Flips a second) and shares one core with this thread
     * (main.c): at equal priority the presenter woke up late and showed ~30 fps. A slightly higher
     * priority lets it preempt the game when its frame is due; it sleeps in vsync the rest of the time.
     * PLASTIC_PRESENTER_PRIO=0 keeps the old behaviour (test switch). */
    {
        const char* pp = getenv("PLASTIC_PRESENTER_PRIO");
        if (!pp || *pp != '0') SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        const char* fs = getenv("PLASTIC_FLIP_SLEEP");
        if (fs && *fs) g_flip_sleep = atoi(fs);
    }
    if (getenv("PLASTIC_NO_PRESENT")) {   /* test switch: no Direct3D at all */
        for (;;) { Sleep(16); InterlockedIncrement(&vdd_presented); }
    }
    if (!present_init(g_hwnd)) {
        fprintf(stderr, "[ddraw] presenter could not start\n");
        return 1;
    }
    /* the largest frame the primary can hold; the logical size inside it changes */
    size_t frame_size = (size_t)(g_cap_w ? g_cap_w : 1600) * (size_t)(g_cap_h ? g_cap_h : 1200);
    uint8_t* frame = (uint8_t*)malloc(frame_size);
    if (!frame) { fprintf(stderr, "[ddraw] presenter: no memory for the frame\n"); return 1; }
    PALETTEENTRY pal[256];
    /* test switch: PLASTIC_LOGICAL_PROBE=WxH sets the big surfaces' logical size once the game has made
     * its world surface, to prove the 640x480 picture does not depend on it */
    int probe_w = 0, probe_h = 0, probe_done = 0;
    if (getenv("PLASTIC_LOGICAL_PROBE")) sscanf(getenv("PLASTIC_LOGICAL_PROBE"), "%dx%d", &probe_w, &probe_h);
    for (;;) {
        int w, h;
        if (probe_w && !probe_done && g_world) probe_done = vdd_set_logical(probe_w, probe_h) ? 1 : -1;
        EnterCriticalSection(&g_lock);
        surf_t* p = g_primary;
        w = p->w, h = p->h;
        int fits = (size_t)w * h <= frame_size;
        if (fits)
            for (int y = 0; y < h; y++) memcpy(frame + y * w, p->px + y * p->pitch, (size_t)w);
        if (p->pal) memcpy(pal, p->pal->e, sizeof pal); else memset(pal, 0, sizeof pal);
        LeaveCriticalSection(&g_lock);
        if (!fits) { Sleep(16); continue; }     /* a mode beyond the capacity: nothing sane to show */
        if (IsIconic(g_hwnd)) { Sleep(50); continue; }
        /* at most 60 frames a second: the game shares one core with this thread
         * (main.c), and it never draws faster than that */
        {
            static LARGE_INTEGER freq, last;
            LARGE_INTEGER now;
            if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&now);
            double ms = last.QuadPart ? (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart : 1000.0;
            if (ms < 16.0) Sleep((DWORD)(16.0 - ms));
            QueryPerformanceCounter(&last);
        }
        dump_load_frame(frame, w, h, pal);
        if (g_dump_req) dump_view(frame, w, h, pal);
        present_region_t rg[4];
        RECT rc;
        int n = GetClientRect(g_hwnd, &rc) ? layout(rc.right, rc.bottom, w, h, rg) : 0;
        present_frame_regions(frame, w, w, h, pal, n ? rg : NULL, n);
        InterlockedIncrement(&vdd_presented);
        {
            static LARGE_INTEGER freq, lastp;
            LARGE_INTEGER now;
            if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&now);
            if (lastp.QuadPart) {
                LONG us = (LONG)((now.QuadPart - lastp.QuadPart) * 1000000 / freq.QuadPart);
                if (us > vdd_present_us_max) vdd_present_us_max = us;
            }
            lastp = now;
        }
    }
}

volatile LONG vdd_presented;

/* ------------------------------------------------------------------- shims */

/* DirectDrawCreate(guid, out, outer), inside the lifted model. */
void shim_DirectDrawCreate(void) {
    void** out = (void**)(uintptr_t)ARG(1);
    static int once;
    if (!once) {
        once = 1;
        InitializeCriticalSection(&g_lock);
        g_dd1.vtbl = g_dd1_vtbl;
        g_dd2.vtbl = g_dd2_vtbl;
    }
    if (out) *out = &g_dd1;
    if (vdd_trace) fprintf(stderr, "[ddraw] DirectDrawCreate: virtual DirectDraw\n");
    g_eax = DD_OK;
    g_esp += 4 + 3 * 4;
}

int vdd_mode(int* w, int* h) {
    *w = g_mode_w, *h = g_mode_h;
    return g_hwnd != NULL;
}

HWND vdd_window(void) { return g_hwnd; }

/* The Windows cursor in frame pixels, through the presenter's letterbox mapping:
 * the mouse feed's target (mouse.c) when there is no cnc-ddraw to ask. */
static int cursor_from_client(POINT c, POINT* p) {
    int fx = -1, fy = -1;
    present_window_to_frame(c.x, c.y, &fx, &fy);
    if (fx < 0 || fy < 0) return 0;     /* nothing presented yet */
    p->x = fx, p->y = fy;
    /* Edge scroll (sub_00424CA0) acts on the game cursor within 3 px of its clamp rectangle, which in
     * the hires frame is bigger than the world's picture: the window's border pixels stand for it. With
     * the edge scroll switched off (F9) the cursor is kept 4 px inside the clamp rectangle instead. */
    hires_view_t hv;
    RECT rc;
    if (g_edge_scroll && hires_view(&hv) && !hv.dialog && GetClientRect(g_hwnd, &rc)) {
        if (c.x <= 1) p->x = 0; else if (c.x >= rc.right - 2) p->x = hv.a - 1;
        if (c.y <= 1) p->y = 0; else if (c.y >= rc.bottom - 2) p->y = hv.hc - 1;
    }
    if (!g_edge_scroll && (int)MEM32(ADDR_GAME_STATE) == STATE_MISSION) {
        int l = (int)MEM32(0x00485310u), t = (int)MEM32(0x00485314u), r = (int)MEM32(0x00485318u), b = (int)MEM32(0x0048531Cu);
        if (r - l > 10 && b - t > 10) {
            if (p->x < l + 4) p->x = l + 4; else if (p->x > r - 5) p->x = r - 5;
            if (p->y < t + 4) p->y = t + 4; else if (p->y > b - 5) p->y = b - 5;
        }
    }
    return 1;
}

int vdd_cursor(POINT* p) {
    POINT c;
    if (!g_hwnd || !GetCursorPos(&c) || !ScreenToClient(g_hwnd, &c)) return 0;
    return cursor_from_client(c, p);
}

int vdd_cursor_at(int cx, int cy, POINT* p) {
    RECT rc;
    if (!g_hwnd || !GetClientRect(g_hwnd, &rc)) return 0;
    POINT c = { cx < 0 ? rc.right + cx : cx, cy < 0 ? rc.bottom + cy : cy };
    return cursor_from_client(c, p);
}

/* The big surfaces' logical size (the game's SCREEN_W/H world). Needs capacity: the primary and its
 * back buffer always have it; the world surface (guest pointer at 0x00503100) has it when it is
 * the first offscreen surface the game made after the primary. Returns 1 on success, 0 (and logs)
 * when a surface lacks the capacity, in which case nothing is changed. */
int vdd_set_logical(int w, int h) {
    if (!g_primary) return 0;
    surf_t* world = g_world;
    int ok = w > 0 && h > 0 && w <= g_primary->cap_w && h <= g_primary->cap_h &&
             (!g_primary->back || (w <= g_primary->back->cap_w && h <= g_primary->back->cap_h));
    if (ok && world && (w > world->cap_w || h > world->cap_h)) ok = 0;
    if (!ok) {
        fprintf(stderr, "[ddraw] vdd_set_logical(%d,%d) refused: primary %dx%d, world surface %s%dx%d\n", w, h,
                g_primary->cap_w, g_primary->cap_h, world ? "" : "none ", world ? world->cap_w : 0, world ? world->cap_h : 0);
        return 0;
    }
    EnterCriticalSection(&g_lock);
    g_primary->w = w, g_primary->h = h;
    if (g_primary->back) g_primary->back->w = w, g_primary->back->h = h;
    if (world) world->w = w, world->h = h;
    LeaveCriticalSection(&g_lock);
    if (vdd_trace) fprintf(stderr, "[ddraw] logical size %dx%d (world surface %p)\n", w, h, (void*)world);
    return 1;
}

void vdd_get_logical(int* w, int* h) {
    EnterCriticalSection(&g_lock);
    *w = g_primary ? g_primary->w : g_mode_w;
    *h = g_primary ? g_primary->h : g_mode_h;
    LeaveCriticalSection(&g_lock);
}

int vdd_capacity(int* w, int* h) {
    if (!g_cap_w) compute_capacity();
    *w = g_cap_w, *h = g_cap_h;
    return 1;
}
