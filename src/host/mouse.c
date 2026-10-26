/*
 * mouse.c: the game's DirectInput mouse device, seen from the host.
 *
 * The game moves its own cursor by the raw deltas DirectInput reports, so it
 * ignores Windows' pointer speed and acceleration and counts in 640x480 game
 * pixels whatever size the window is (owner, 2026-10-26: "faster than my mouse
 * settings, and affected by the window size"). cnc-ddraw does not rescale
 * those deltas (src/directinput.c only zeroes them while the mouse is not
 * locked). The host is where they can be replaced.
 *
 * DirectInputCreateA is a host shim. It creates the real object (through
 * cnc-ddraw, which hooks DirectInputCreateA inline once dinput.dll is loaded
 * through its LoadLibraryA hook) and wraps IDirectInputA::CreateDevice in the
 * object's vtable. When the game creates GUID_SysMouse, the device's shared
 * vtable gets GetDeviceState and GetDeviceData wrappers, which pass every other
 * device straight through. The wrappers sit on top of cnc-ddraw's own.
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define DIRECTINPUT_VERSION 0x0500
#include <windows.h>
#include <dinput.h>
#include <stdio.h>

#include "native32.h"
#include "mouse.h"

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))

int mouse_log;                        /* --mouse-log: report what the game asks for */

static LPDIRECTINPUTDEVICEA g_mouse;  /* the game's GUID_SysMouse device */

typedef HRESULT (STDMETHODCALLTYPE *create_device_fn)(LPDIRECTINPUTA, REFGUID, LPDIRECTINPUTDEVICEA*, LPUNKNOWN);
typedef HRESULT (STDMETHODCALLTYPE *get_state_fn)(LPDIRECTINPUTDEVICEA, DWORD, LPVOID);
typedef HRESULT (STDMETHODCALLTYPE *get_data_fn)(LPDIRECTINPUTDEVICEA, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *set_coop_fn)(LPDIRECTINPUTDEVICEA, HWND, DWORD);
static create_device_fn prev_create_device;
static get_state_fn prev_get_state;
static get_data_fn prev_get_data;
static set_coop_fn prev_set_coop;

int mouse_nonexclusive;

/* IDirectInputA: QueryInterface, AddRef, Release, CreateDevice (3).
 * IDirectInputDeviceA: ... Acquire (7), Unacquire (8), GetDeviceState (9),
 * GetDeviceData (10), SetDataFormat (11), SetEventNotification (12),
 * SetCooperativeLevel (13). */
enum { SLOT_CREATE_DEVICE = 3, SLOT_GET_STATE = 9, SLOT_GET_DATA = 10, SLOT_SET_COOP = 13 };

/* Replace one vtable slot, returning what was there. The vtables live in
 * dinput.dll's read-only data and are shared by every object of the class. */
static void* patch_slot(void* obj, int slot, void* fn) {
    void** vt = *(void***)obj;
    void* old = vt[slot];
    if (old == fn) return NULL;           /* already ours */
    DWORD prot;
    VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &prot);
    vt[slot] = fn;
    VirtualProtect(&vt[slot], sizeof(void*), prot, &prot);
    return old;
}

static LPDIRECTINPUTDEVICEA g_kbd;    /* the game's GUID_SysKeyboard device, if it makes one */
static struct { void** vt; get_state_fn prev; } g_state_prev[4];   /* GetDeviceState originals, per vtable */
static void inject_keys(void* data, DWORD cb);

static get_state_fn state_prev(LPDIRECTINPUTDEVICEA self) {
    void** vt = *(void***)self;
    for (int i = 0; i < 4; i++) if (g_state_prev[i].vt == vt) return g_state_prev[i].prev;
    return prev_get_state;
}

static HRESULT STDMETHODCALLTYPE hook_get_state(LPDIRECTINPUTDEVICEA self, DWORD cb, LPVOID data) {
    HRESULT hr = state_prev(self)(self, cb, data);
    if (self == g_kbd && SUCCEEDED(hr)) inject_keys(data, cb);
    if (self == g_mouse && mouse_log) {
        static LONG n;
        LONG k = InterlockedIncrement(&n);
        const DIMOUSESTATE* s = (const DIMOUSESTATE*)data;
        if (k <= 5 || (SUCCEEDED(hr) && cb >= sizeof(DIMOUSESTATE) && (s->lX || s->lY) && k % 50 == 0))
            fprintf(stderr, "[mouse] GetDeviceState #%ld cb=%lu hr=%08lX dx=%ld dy=%ld buttons=%02X%02X\n", k, cb,
                    (unsigned long)hr, cb >= 8 ? s->lX : 0, cb >= 8 ? s->lY : 0,
                    cb >= 13 ? s->rgbButtons[0] : 0, cb >= 14 ? s->rgbButtons[1] : 0);
    }
    return hr;
}

/* ------------------------------------------------- following the Windows cursor
 *
 * Measured with --mouse-log: the game reads the mouse buffered,
 * GetDeviceData(cbObjectData 16, one event per call, flags 0), until a call
 * returns none. So each polling cycle starts with the first call after an
 * empty one. There the host takes the target, the Windows cursor in game
 * pixels, and queues X and Y events for the difference between it and the
 * game's own cursor. Calls hand out the queued events first, then the real
 * button and wheel events. DirectInput's own X/Y events, raw counts that
 * ignore Windows' pointer speed and the window's size, are dropped.
 *
 * The target comes from GetCursorPos through the host's import table, which
 * cnc-ddraw has hooked: fake_GetCursorPos already turns the Windows cursor into
 * game coordinates for a windowed, stretched or letterboxed window. Nothing
 * is fed while the game's window is not in the foreground. --raw-mouse turns
 * all of this off. */
int mouse_raw;

/* How the game spends the events (sub_00427070 polls, sub_00426F40 applies
 * each one): a poll starts with its last deltas at zero, an X or Y event
 * overwrites that axis' last delta, and after every event, buttons included,
 * the cursor moves by BOTH last deltas, then is clamped to 0..639 x 0..479. No
 * scaling, no acceleration. So X=+5 then Y=+3 moves X by 10. To place the
 * cursor exactly, each axis' delta is followed by a zero for that axis:
 * X=dx, X=0, Y=dy, Y=0, and the real button events come after them. */
static HWND g_game_hwnd;
static int g_cycle_start = 1;
static struct { DWORD ofs; LONG data; } g_queue[8];
static int g_queued, g_head;
static DWORD g_sequence = 0x40000000u;  /* apart from DirectInput's own numbers */

/* The game's cursor, from its own variables (mouse_set_cursor_vars: int32 x at
 * 0x00485464, y at 0x00485468), or else the position fed so far. */
static uint32_t g_cursor_x_va, g_cursor_y_va;
static int g_track_x = 320, g_track_y = 200;

void mouse_set_cursor_vars(uint32_t x_va, uint32_t y_va) {
    g_cursor_x_va = x_va;
    g_cursor_y_va = y_va;
}

static void game_cursor(int* x, int* y) {
    if (g_cursor_x_va && g_cursor_y_va) {
        *x = (int)(int32_t)MEM32(g_cursor_x_va);
        *y = (int)(int32_t)MEM32(g_cursor_y_va);
    } else {
        *x = g_track_x;
        *y = g_track_y;
    }
}


/* ------------------------------------------------- unattended test scripts
 *
 * PLASTIC_SCRIPT="t:x,y[,click];..." drives the game's cursor: from the first entry whose time
 * (seconds since the host process started) has passed, the target is that entry's game pixel
 * (x,y), whatever window has the foreground. With ",click" a left click (button 0 down, then up
 * on the next poll cycle) follows once the game's cursor has arrived.
 * PLASTIC_KEYS="t:KEY;..." holds a DirectInput key (a DIK number, decimal or 0x.., or ESC, ENTER,
 * SPACE) down for 3 keyboard polls from time t. Both are for diagnosis and scripted tests only. */
typedef struct { double t; int x, y, click, fired; } script_ev_t;
typedef struct { double t; int dik, polls; } key_ev_t;
static script_ev_t g_script[64];
static key_ev_t g_keys[64];
static int g_script_n, g_keys_n, g_script_init;
static int g_script_cur = -1;               /* last entry reached */
static int g_click_wait, g_click_up;        /* cycles waited for the cursor / button up owed */

static double host_seconds(void) {
    static LARGE_INTEGER freq, t0;
    LARGE_INTEGER now;
    if (!freq.QuadPart) {
        FILETIME created, ex, kern, user, nowft;
        QueryPerformanceFrequency(&freq);
        GetProcessTimes(GetCurrentProcess(), &created, &ex, &kern, &user);
        GetSystemTimeAsFileTime(&nowft);
        ULONGLONG c = ((ULONGLONG)created.dwHighDateTime << 32) | created.dwLowDateTime;
        ULONGLONG n = ((ULONGLONG)nowft.dwHighDateTime << 32) | nowft.dwLowDateTime;
        QueryPerformanceCounter(&t0);
        t0.QuadPart -= (LONGLONG)((n - c) / 1e7 * (double)freq.QuadPart);   /* the process began then */
    }
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
}

static int dik_by_name(const char* s) {
    static const struct { const char* n; int d; } names[] = {
        { "ESC", 1 }, { "ESCAPE", 1 }, { "ENTER", 0x1C }, { "RETURN", 0x1C }, { "SPACE", 0x39 },
        { "MINUS", 0x0C }, { "EQUALS", 0x0D }, { "PLUS", 0x0D }, { "F1", 0x3B }, { "P", 0x19 },
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!_stricmp(s, names[i].n)) return names[i].d;
    return (int)strtol(s, NULL, 0);
}

static void script_init(void) {
    if (g_script_init) return;
    g_script_init = 1;
    const char* sc = getenv("PLASTIC_SCRIPT");
    const char* ks = getenv("PLASTIC_KEYS");
    char buf[2048];
    if (sc && *sc) {
        strncpy(buf, sc, sizeof buf - 1); buf[sizeof buf - 1] = 0;
        for (char* tok = strtok(buf, ";"); tok && g_script_n < 64; tok = strtok(NULL, ";")) {
            script_ev_t e = { 0 };
            int x, y;
            char* c = strchr(tok, ':');
            if (!c || sscanf(c + 1, "%d,%d", &x, &y) != 2) continue;
            e.t = atof(tok), e.x = x, e.y = y;
            e.click = strstr(c, "click") != NULL;
            g_script[g_script_n++] = e;
        }
        fprintf(stderr, "[script] PLASTIC_SCRIPT: %d step(s)\n", g_script_n);
    }
    if (ks && *ks) {
        strncpy(buf, ks, sizeof buf - 1); buf[sizeof buf - 1] = 0;
        for (char* tok = strtok(buf, ";"); tok && g_keys_n < 64; tok = strtok(NULL, ";")) {
            char* c = strchr(tok, ':');
            if (!c) continue;
            key_ev_t k = { atof(tok), dik_by_name(c + 1), 3 };
            if (k.dik > 0 && k.dik < 256) g_keys[g_keys_n++] = k;
        }
        fprintf(stderr, "[script] PLASTIC_KEYS: %d key(s)\n", g_keys_n);
    }
}

/* The script's cursor target now, if its first step has been reached. */
static int script_target(POINT* t, int* click_now) {
    script_init();
    *click_now = 0;
    if (!g_script_n) return 0;
    double now = host_seconds();
    while (g_script_cur + 1 < g_script_n && g_script[g_script_cur + 1].t <= now) {
        g_script_cur++;
        g_click_wait = 0;
        fprintf(stderr, "[script] %.1f s: cursor to (%d,%d)%s\n", now, g_script[g_script_cur].x,
                g_script[g_script_cur].y, g_script[g_script_cur].click ? " + click" : "");
    }
    if (g_script_cur < 0) return 0;
    script_ev_t* e = &g_script[g_script_cur];
    t->x = e->x, t->y = e->y;
    if (e->click && !e->fired) *click_now = 1;
    return 1;
}

static void inject_keys(void* data, DWORD cb) {
    script_init();
    if (cb != 256) return;
    double now = host_seconds();
    for (int i = 0; i < g_keys_n; i++) {
        if (g_keys[i].t > now || g_keys[i].polls <= 0) continue;
        ((BYTE*)data)[g_keys[i].dik] |= 0x80;
        if (--g_keys[i].polls == 2) fprintf(stderr, "[script] %.1f s: key DIK 0x%02X down\n", now, g_keys[i].dik);
    }
}

int (*mouse_target)(POINT* p);

static void queue_motion(void) {
    POINT t;
    int scripted, click_now;
    g_queued = g_head = 0;
    if (g_click_up) {                       /* the button goes up one cycle after it went down */
        g_click_up = 0;
        g_queue[g_queued].ofs = DIMOFS_BUTTON0, g_queue[g_queued++].data = 0;
    }
    scripted = script_target(&t, &click_now);
    if (!scripted) {
        g_queued = g_head = 0;
        if (!g_game_hwnd) g_game_hwnd = FindWindowA("Armymen2", NULL);
        if (!g_game_hwnd || GetForegroundWindow() != g_game_hwnd) return;
        /* the virtual DirectDraw's mapping, or cnc-ddraw's GetCursorPos hook */
        if (mouse_target ? !mouse_target(&t) : !GetCursorPos(&t)) return;
    }
    int ax, ay;
    game_cursor(&ax, &ay);
    LONG dx = t.x - ax, dy = t.y - ay;
    if (dx) {
        g_queue[g_queued].ofs = DIMOFS_X, g_queue[g_queued++].data = dx;
        g_queue[g_queued].ofs = DIMOFS_X, g_queue[g_queued++].data = 0;
    }
    if (dy) {
        g_queue[g_queued].ofs = DIMOFS_Y, g_queue[g_queued++].data = dy;
        g_queue[g_queued].ofs = DIMOFS_Y, g_queue[g_queued++].data = 0;
    }
    if (scripted && click_now) {
        /* press once the game's cursor is on the target (or after 10 cycles, if it never gets there) */
        if ((!dx && !dy) || ++g_click_wait > 10) {
            g_queue[g_queued].ofs = DIMOFS_BUTTON0, g_queue[g_queued++].data = 0x80;
            g_script[g_script_cur].fired = 1;
            g_click_up = 1;
            fprintf(stderr, "[script] click at (%ld,%ld), game cursor was (%d,%d)\n", t.x, t.y, ax, ay);
        }
    }
    g_track_x = t.x < 0 ? 0 : t.x > 639 ? 639 : t.x;
    g_track_y = t.y < 0 ? 0 : t.y > 479 ? 479 : t.y;
    if (mouse_log && (dx || dy)) {
        static LONG n;
        if (InterlockedIncrement(&n) <= 40)
            fprintf(stderr, "[mouse] target (%ld,%ld) game (%d,%d): feed dx=%ld dy=%ld\n", t.x, t.y, ax, ay, dx, dy);
    }
}

static HRESULT feed_events(LPDIRECTINPUTDEVICEA self, DWORD cb, LPDIDEVICEOBJECTDATA od, LPDWORD inout, DWORD flags) {
    DWORD want = *inout, filled = 0;
    if (g_cycle_start) {
        g_cycle_start = 0;
        queue_motion();
    }
    while (filled < want) {
        DIDEVICEOBJECTDATA* e = (DIDEVICEOBJECTDATA*)((BYTE*)od + filled * cb);
        if (g_head < g_queued) {
            e->dwOfs = g_queue[g_head].ofs;
            e->dwData = (DWORD)g_queue[g_head].data;
            e->dwTimeStamp = GetTickCount();
            e->dwSequence = g_sequence++;
            g_head++;
            filled++;
            continue;
        }
        DIDEVICEOBJECTDATA real;
        DWORD one = 1;
        HRESULT hr = prev_get_data(self, sizeof real, &real, &one, flags);
        if (FAILED(hr)) {
            if (filled) break;
            g_cycle_start = 1;
            return hr;
        }
        if (!one) break;
        if (real.dwOfs == DIMOFS_X || real.dwOfs == DIMOFS_Y) continue;   /* replaced */
        *e = real;
        filled++;
    }
    *inout = filled;
    if (!filled) g_cycle_start = 1;
    return DI_OK;
}

static HRESULT STDMETHODCALLTYPE hook_get_data(LPDIRECTINPUTDEVICEA self, DWORD cb, LPDIDEVICEOBJECTDATA od,
                                               LPDWORD inout, DWORD flags) {
    DWORD asked = inout ? *inout : 0;
    HRESULT hr;
    /* only plain reads: a flush (no buffer) or a peek goes through untouched */
    if (self == g_mouse && !mouse_raw && od && inout && asked && !(flags & DIGDD_PEEK) &&
        cb >= sizeof(DIDEVICEOBJECTDATA))
        hr = feed_events(self, cb, od, inout, flags);
    else
        hr = prev_get_data(self, cb, od, inout, flags);
    if (self == g_mouse && mouse_log) {
        static LONG n;
        LONG k = InterlockedIncrement(&n);
        if (k <= 5 || (inout && *inout && k % 50 == 0))
            fprintf(stderr, "[mouse] GetDeviceData #%ld cbObject=%lu asked=%lu got=%lu flags=%lX hr=%08lX\n",
                    k, cb, asked, inout ? *inout : 0, flags, (unsigned long)hr);
    }
    return hr;
}

/* The game asks for DISCL_EXCLUSIVE|DISCL_FOREGROUND, which hides the Windows
 * cursor and stops it moving: the feed above follows that cursor. cnc-ddraw's
 * hook forced non-exclusive; without cnc-ddraw (the virtual DirectDraw) this does. */
static HRESULT STDMETHODCALLTYPE hook_set_coop(LPDIRECTINPUTDEVICEA self, HWND hwnd, DWORD flags) {
    if (self == g_mouse && mouse_nonexclusive)
        flags = DISCL_NONEXCLUSIVE | DISCL_FOREGROUND;
    return prev_set_coop(self, hwnd, flags);
}

static HRESULT STDMETHODCALLTYPE hook_create_device(LPDIRECTINPUTA self, REFGUID guid, LPDIRECTINPUTDEVICEA* dev,
                                                   LPUNKNOWN outer) {
    HRESULT hr = prev_create_device(self, guid, dev, outer);
    if (SUCCEEDED(hr) && dev && *dev && IsEqualGUID(guid, &GUID_SysMouse)) {
        g_mouse = *dev;
        void* p = patch_slot(g_mouse, SLOT_GET_DATA, (void*)hook_get_data);
        if (p) prev_get_data = (get_data_fn)p;
        p = patch_slot(g_mouse, SLOT_SET_COOP, (void*)hook_set_coop);
        if (p) prev_set_coop = (set_coop_fn)p;
        if (mouse_log) fprintf(stderr, "[mouse] game created GUID_SysMouse: device %p\n", (void*)g_mouse);
    }
    if (SUCCEEDED(hr) && dev && *dev && IsEqualGUID(guid, &GUID_SysKeyboard)) {
        g_kbd = *dev;
        if (mouse_log || getenv("PLASTIC_KEYS")) fprintf(stderr, "[mouse] game created GUID_SysKeyboard: device %p\n", (void*)g_kbd);
    }
    if (SUCCEEDED(hr) && dev && *dev && (*dev == g_kbd || *dev == g_mouse)) {
        /* GetDeviceState hooks, remembering the original for each distinct vtable */
        void** vt = *(void***)*dev;
        int i = 0;
        while (i < 4 && g_state_prev[i].vt && g_state_prev[i].vt != vt) i++;
        if (i < 4 && !g_state_prev[i].vt) {
            g_state_prev[i].vt = vt;
            g_state_prev[i].prev = (get_state_fn)vt[SLOT_GET_STATE];
        }
        patch_slot(*dev, SLOT_GET_STATE, (void*)hook_get_state);
    }
    return hr;
}

/* DirectInputCreateA(hinst, version, out, outer), a host shim inside the
 * lifted model: arguments from the guest stack, stdcall, so it pops them. */
void shim_DirectInputCreateA(void) {
    typedef HRESULT (WINAPI *create_fn)(HINSTANCE, DWORD, LPDIRECTINPUTA*, LPUNKNOWN);
    HINSTANCE hinst = (HINSTANCE)(uintptr_t)ARG(0);
    DWORD version = ARG(1);
    LPDIRECTINPUTA* out = (LPDIRECTINPUTA*)(uintptr_t)ARG(2);
    LPUNKNOWN outer = (LPUNKNOWN)(uintptr_t)ARG(3);
    /* Loaded through the host's import of LoadLibraryA, which cnc-ddraw has
     * hooked: that load is what makes it hook DirectInputCreateA inline. */
    create_fn create = (create_fn)GetProcAddress(LoadLibraryA("dinput.dll"), "DirectInputCreateA");
    LPDIRECTINPUTA di = NULL;
    HRESULT hr = create ? create(hinst, version, &di, outer) : E_FAIL;
    if (SUCCEEDED(hr) && di) {
        void* p = patch_slot(di, SLOT_CREATE_DEVICE, (void*)hook_create_device);
        if (p) prev_create_device = (create_device_fn)p;
    }
    if (out) *out = di;
    if (mouse_log) fprintf(stderr, "[mouse] DirectInputCreateA(version 0x%04lX) = %08lX\n", version, (unsigned long)hr);
    g_eax = (uint32_t)hr;
    g_esp += 4 + 4 * 4;
}
