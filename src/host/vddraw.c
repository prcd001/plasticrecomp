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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "present.h"
#include "vddraw.h"

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))

int vdd_trace;                          /* --dd-trace */

/* ------------------------------------------------------------------ objects */

typedef struct pal {
    const void* const* vtbl;
    LONG refs;
    PALETTEENTRY e[256];
} pal_t;

typedef struct surf {
    const void* const* vtbl;
    LONG refs;
    int w, h, pitch;
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

static surf_t* new_surface(int w, int h, DWORD caps) {
    surf_t* s = (surf_t*)calloc(1, sizeof *s);
    s->vtbl = g_surf_vtbl;
    s->refs = 1;
    s->w = w, s->h = h;
    s->pitch = (w + 3) & ~3;
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
        s = new_surface(g_mode_w, g_mode_h, caps);
        if (!s) return DDERR_OUTOFMEMORY;
        s->primary = 1;
        if ((d->dwFlags & DDSD_BACKBUFFERCOUNT) && d->dwBackBufferCount) {
            s->back = new_surface(g_mode_w, g_mode_h, DDSCAPS_BACKBUFFER);
            if (!s->back) return DDERR_OUTOFMEMORY;
        }
        g_primary = s;
        if (!InterlockedExchange(&g_running, 1)) CloseHandle(CreateThread(NULL, 0, presenter, NULL, 0, NULL));
    } else {
        int w = (d->dwFlags & DDSD_WIDTH) ? (int)d->dwWidth : g_mode_w;
        int h = (d->dwFlags & DDSD_HEIGHT) ? (int)d->dwHeight : g_mode_h;
        s = new_surface(w, h, caps);
        if (!s) return DDERR_OUTOFMEMORY;
    }
    if (vdd_trace) fprintf(stderr, "[ddraw] CreateSurface caps 0x%lX -> %dx%d %p\n", caps, s->w, s->h, (void*)s);
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
static LRESULT CALLBACK keep_active(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (vdd_trace && (msg == WM_ACTIVATEAPP || msg == WM_ACTIVATE || msg == WM_SETFOCUS || msg == WM_KILLFOCUS))
        fprintf(stderr, "[ddraw] window msg 0x%X wp=%lX\n", msg, (unsigned long)wp);
    if (msg == WM_ACTIVATEAPP && !wp) return 0;
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) return 0;
    return CallWindowProcA(g_guest_proc, hwnd, msg, wp, lp);
}

/* The game's window becomes an ordinary resizable window, sized to the largest
 * whole multiple of the mode that fits 90% of the work area. */
static void make_window(HWND hwnd) {
    g_guest_proc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)keep_active);
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

/* ---------------------------------------------------------------- presenter */

/* Shows the primary about 60 times a second (present_frame waits for vsync). It
 * owns the D3D device, created here, on this thread only. The pixels are copied
 * under the lock, so a Flip never happens halfway through a copy. */
static DWORD WINAPI presenter(LPVOID unused) {
    (void)unused;
    while (!g_hwnd) Sleep(5);
    if (!present_init(g_hwnd)) {
        fprintf(stderr, "[ddraw] presenter could not start\n");
        return 1;
    }
    static uint8_t frame[1600 * 1200];
    PALETTEENTRY pal[256];
    for (;;) {
        int w, h;
        EnterCriticalSection(&g_lock);
        surf_t* p = g_primary;
        w = p->w, h = p->h;
        if ((size_t)w * h <= sizeof frame)
            for (int y = 0; y < h; y++) memcpy(frame + y * w, p->px + y * p->pitch, (size_t)w);
        if (p->pal) memcpy(pal, p->pal->e, sizeof pal); else memset(pal, 0, sizeof pal);
        LeaveCriticalSection(&g_lock);
        if (IsIconic(g_hwnd)) { Sleep(50); continue; }
        present_frame(frame, w, w, h, pal);
        InterlockedIncrement(&vdd_presented);
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
int vdd_cursor(POINT* p) {
    POINT c;
    if (!g_hwnd || !GetCursorPos(&c) || !ScreenToClient(g_hwnd, &c)) return 0;
    int fx = -1, fy = -1;
    present_window_to_frame(c.x, c.y, &fx, &fy);
    if (fx < 0 || fy < 0) return 0;     /* nothing presented yet */
    p->x = fx, p->y = fy;
    return 1;
}
