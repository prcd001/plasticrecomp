/* present_test.c - standalone check of src/host/present.c.
 *
 *   present_test [seconds] [--script]
 *
 * Opens a resizable window (client 1280x960) and presents a 640x480 indexed test frame at
 * 60 fps for `seconds` (default 5): index bars, a 1-px checker, a 1-px border, a moving square
 * and a palette entry that cycles colour. F toggles the filter, Esc quits. With --script the run
 * is split into 8 equal phases, (1280x960, 1920x800, 800x1000, 1500x1100) x (sharp, nearest),
 * each announced on stdout with its start time, so screenshots can be taken in each phase.
 * At every phase (and at the end) present_window_to_frame is probed at the picture corners,
 * its centre and a bar. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../src/host/present.h"

#pragma comment(lib, "winmm.lib")

#define FW 640
#define FH 480

static uint8_t     g_frame[FH][FW];
static PALETTEENTRY g_pal[256];
static int         g_filter = 1;
static int         g_quit;
static HWND        g_hwnd;

/* Indices: 0 black, 1..16 bar colours, 17 white, 18 black (checker), 19 border, 20 square,
 * 200 the animated entry. */
static void build_palette(void)
{
    static const BYTE bars[16][3] = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {0, 255, 255}, {255, 0, 255},
        {255, 128, 0}, {128, 0, 255}, {0, 128, 64}, {128, 64, 0}, {255, 160, 160}, {160, 255, 160},
        {160, 160, 255}, {128, 128, 128}, {64, 64, 64}, {192, 192, 192} };
    int i;
    memset(g_pal, 0, sizeof g_pal);
    for (i = 0; i < 16; i++) {
        g_pal[1 + i].peRed = bars[i][0];
        g_pal[1 + i].peGreen = bars[i][1];
        g_pal[1 + i].peBlue = bars[i][2];
    }
    g_pal[17].peRed = g_pal[17].peGreen = g_pal[17].peBlue = 255;
    g_pal[19].peRed = 255; g_pal[19].peGreen = 255; g_pal[19].peBlue = 0;   /* border: yellow */
    g_pal[20].peRed = 255; g_pal[20].peGreen = 255; g_pal[20].peBlue = 255; /* square: white */
}

static void animate_palette(int frame)
{
    /* Entry 200 runs around the hue circle once every 2 s. */
    double hue = fmod(frame / 120.0, 1.0) * 6.0;
    double x = 1.0 - fabs(fmod(hue, 2.0) - 1.0);
    double r = 0, g = 0, b = 0;
    if (hue < 1) { r = 1; g = x; } else if (hue < 2) { r = x; g = 1; } else if (hue < 3) { g = 1; b = x; }
    else if (hue < 4) { g = x; b = 1; } else if (hue < 5) { r = x; b = 1; } else { r = 1; b = x; }
    g_pal[200].peRed = (BYTE)(r * 255);
    g_pal[200].peGreen = (BYTE)(g * 255);
    g_pal[200].peBlue = (BYTE)(b * 255);
}

static void build_frame(int frame)
{
    int x, y, sx, sy;
    /* Top 300 rows: 16 bars of 40 px, indices 1..16. */
    for (y = 0; y < 300; y++)
        for (x = 0; x < FW; x++)
            g_frame[y][x] = (uint8_t)(1 + x / 40);
    /* Bottom-left 320x180: 1-px checker of white/black. */
    for (y = 300; y < FH; y++)
        for (x = 0; x < 320; x++)
            g_frame[y][x] = ((x ^ y) & 1) ? 17 : 18;
    /* Bottom-right: black with the animated patch. */
    for (y = 300; y < FH; y++)
        for (x = 320; x < FW; x++)
            g_frame[y][x] = (x >= 360 && x < 600 && y >= 330 && y < 450) ? 200 : 0;
    /* Moving square (48x48), bouncing across the bars. */
    sx = (frame * 4) % (2 * (FW - 48));
    if (sx > FW - 48) sx = 2 * (FW - 48) - sx;
    sy = 100 + (int)(80 * sin(frame / 30.0));
    for (y = sy; y < sy + 48; y++)
        for (x = sx; x < sx + 48; x++)
            g_frame[y][x] = 20;
    /* 1-px border round the whole frame: shows nothing is cropped. */
    for (x = 0; x < FW; x++) g_frame[0][x] = g_frame[FH - 1][x] = 19;
    for (y = 0; y < FH; y++) g_frame[y][0] = g_frame[y][FW - 1] = 19;
}

static void set_title(void)
{
    char t[128];
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    sprintf(t, "present_test %ldx%ld %s", rc.right, rc.bottom, g_filter ? "sharp-bilinear" : "nearest");
    SetWindowTextA(g_hwnd, t);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_KEYDOWN:
        if (wp == 'F') { g_filter ^= 1; present_set_filter(g_filter); set_title(); }
        if (wp == VK_ESCAPE) g_quit = 1;
        return 0;
    case WM_SIZE:
        if (g_hwnd) set_title();
        return 0;
    case WM_GETMINMAXINFO: {  /* allow windows larger than the screen */
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMaxTrackSize.x = mm->ptMaxTrackSize.y = 8192;
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        g_quit = 1;
        return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

static void set_client_size(int w, int h)
{
    RECT r = {0, 0, w, h};
    DWORD style = (DWORD)GetWindowLongA(g_hwnd, GWL_STYLE);
    UINT dpi = GetDpiForWindow(g_hwnd);
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    SetWindowPos(g_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

static void probe(int cx, int cy, const char* what)
{
    int fx, fy, in = present_window_to_frame(cx, cy, &fx, &fy);
    printf("    %-14s client (%4d,%4d) -> frame (%3d,%3d) %s\n", what, cx, cy, fx, fy,
           in ? "inside" : "outside");
}

static void probe_all(void)
{
    RECT rc;
    int cw, ch;
    GetClientRect(g_hwnd, &rc);
    cw = rc.right;
    ch = rc.bottom;
    printf("  window_to_frame at client %dx%d:\n", cw, ch);
    probe(0, 0, "client TL");
    probe(cw - 1, ch - 1, "client BR");
    probe(cw / 2, ch / 2, "centre");
    probe(cw / 2 + 1, ch / 2 + 1, "centre+1");
    /* Picture corners from the letterbox rectangle (same maths as present.c). */
    {
        double s = (double)cw / FW < (double)ch / FH ? (double)cw / FW : (double)ch / FH;
        int dw = (int)(FW * s + 0.5), dh = (int)(FH * s + 0.5);
        int ox, oy;
        if (dw > cw) dw = cw;
        if (dh > ch) dh = ch;
        ox = (cw - dw) / 2;
        oy = (ch - dh) / 2;
        probe(ox, oy, "picture TL");
        probe(ox + dw - 1, oy + dh - 1, "picture BR");
        probe(ox - 1, oy + dh / 2, "left of pic");
        probe(ox + dw / 2, oy - 1, "above pic");
        if (ox > 0) probe(ox / 2, ch / 2, "pillar bar");
        if (oy > 0) probe(cw / 2, oy / 2, "letter bar");
    }
}

int main(int argc, char** argv)
{
    static const int sizes[4][2] = { {1280, 960}, {1920, 800}, {800, 1000}, {1500, 1100} };
    double seconds = 5.0;
    int script = 0, i, frames = 0, phase = -1;
    LARGE_INTEGER freq, t0, now, ta, tb;
    double present_ms = 0, max_present_ms = 0;
    WNDCLASSA wc;
    MSG msg;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--script")) script = 1;
        else seconds = atof(argv[i]);
    }
    if (seconds <= 0) seconds = 5.0;
    setvbuf(stdout, NULL, _IONBF, 0);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "present_test";
    RegisterClassA(&wc);
    g_hwnd = CreateWindowExA(0, "present_test", "present_test", WS_OVERLAPPEDWINDOW,
                             40, 40, 100, 100, NULL, NULL, wc.hInstance, NULL);
    set_client_size(1280, 960);
    ShowWindow(g_hwnd, SW_SHOW);
    set_title();

    if (!present_init(g_hwnd)) { printf("present_init failed\n"); return 1; }
    present_set_filter(g_filter);
    build_palette();

    /* Before any frame: no picture, so nothing is inside. */
    {
        int fx, fy, in = present_window_to_frame(10, 10, &fx, &fy);
        printf("before first frame: (10,10) -> (%d,%d) %s\n", fx, fy, in ? "inside" : "outside");
    }

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (;;) {
        double t, next;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        QueryPerformanceCounter(&now);
        t = (double)(now.QuadPart - t0.QuadPart) / freq.QuadPart;
        if (g_quit || t >= seconds) break;

        if (script) {
            int p = (int)(t / (seconds / 8));
            if (p > 7) p = 7;
            if (p != phase) {
                if (phase >= 0) probe_all();
                phase = p;
                set_client_size(sizes[p / 2][0], sizes[p / 2][1]);
                g_filter = (p & 1) ? 0 : 1;
                present_set_filter(g_filter);
                set_title();
                printf("phase %d at %.2f s: client %dx%d %s\n", p, t, sizes[p / 2][0], sizes[p / 2][1],
                       g_filter ? "sharp-bilinear" : "nearest");
            }
        }

        animate_palette(frames);
        build_frame(frames);
        QueryPerformanceCounter(&ta);
        present_frame(&g_frame[0][0], FW, FW, FH, g_pal);
        QueryPerformanceCounter(&tb);
        {
            double ms = 1000.0 * (tb.QuadPart - ta.QuadPart) / freq.QuadPart;
            present_ms += ms;
            if (ms > max_present_ms) max_present_ms = ms;
        }
        frames++;

        /* 60 fps pacing on top of vsync (the display may run faster than 60 Hz). */
        next = (double)frames / 60.0;
        for (;;) {
            QueryPerformanceCounter(&now);
            t = (double)(now.QuadPart - t0.QuadPart) / freq.QuadPart;
            if (t >= next || t >= seconds) break;
            if (next - t > 0.002) Sleep(1);
        }
    }
    QueryPerformanceCounter(&now);
    probe_all();
    {
        double t = (double)(now.QuadPart - t0.QuadPart) / freq.QuadPart;
        printf("presented %d frames in %.2f s (%.1f fps); present_frame avg %.2f ms, max %.2f ms\n",
               frames, t, frames / t, present_ms / (frames ? frames : 1), max_present_ms);
    }
    present_shutdown();
    DestroyWindow(g_hwnd);
    timeEndPeriod(1);
    return 0;
}
