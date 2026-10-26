/*
 * plasticrecomp host: runs the lifted ArmyMen2.exe (GOG build) on pcrecomp's
 * runtime/native32, which does the hard parts (the native bridge, Windows ->
 * guest callbacks, the machine lock; see native32.h). What is here is only what
 * is specific to this game: where the image goes, which file it is, the few
 * imports that must answer for the guest instead of the host, and the reports.
 *
 * Linked at /BASE:0x60000000 with a small /STACK (scripts/build_host.bat), and
 * native32_hold_image keeps 0x00400000.. free from process start, so the
 * guest image maps at its own base. The lifted code then runs on a thread with
 * a large stack: a lifted frame is several times the original's.
 *
 * usage: plasticrecomp [--game DIR] [--watchdog S] [--dry] [--imports] [--no-route] [--native-trace] [--callbacks]
 *        plasticrecomp --dplay-check
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <objbase.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wincrypt.h>

#include "native32.h"
#include "recomp_trace.h"
#include "recomp_target.h"   /* the exe the C was lifted from: scripts/run_lift.py */
#include "mouse.h"
#include "vddraw.h"

#define AM2_IMAGE_BASE 0x00400000u
#define AM2_ENTRY      0x004664C0u   /* the PE entry point: MSVC 6's WinMainCRTStartup */

#define RUN_STACK      (64u << 20)

/* Argument n of the import being shimmed: [esp] is the dummy return address. */
#define ARG(n) MEM32(g_esp + 4 + 4 * (n))

static char g_game_dir[MAX_PATH], g_guest_exe[MAX_PATH], g_guest_cmdline[MAX_PATH + 3];
static int g_dry;
static DWORD g_watchdog_s;

/* ---------------------------------------------------------------- shims */

/* The process exe is the host, so a question about "this module" (a NULL
 * handle, or the host's own, which GetModuleHandleA(NULL) hands the guest as
 * its hInstance) is answered with the guest exe in the game folder. The CRT
 * builds argv[0] from it. */
static void shim_GetModuleFileNameA(void) {
    HMODULE h = (HMODULE)(uintptr_t)ARG(0);
    char* buf = (char*)(uintptr_t)ARG(1);
    DWORD size = ARG(2);
    if (!h || h == GetModuleHandleA(NULL) || (uintptr_t)h == AM2_IMAGE_BASE) {
        DWORD n = (DWORD)strlen(g_guest_exe);
        if (size) {
            DWORD k = n < size ? n : size - 1;
            memcpy(buf, g_guest_exe, k);
            buf[k] = 0;
            n = k;
        }
        g_eax = n;
    } else {
        g_eax = GetModuleFileNameA(h, buf, size);
    }
    g_esp += 4 + 3 * 4;
}

/* The host's command line carries the host's flags; the guest gets its own. */
static void shim_GetCommandLineA(void) {
    g_eax = (uint32_t)(uintptr_t)g_guest_cmdline;
    g_esp += 4;
}

/* A guest filter installed as the process's would be called for the host's
 * faults too, and would hide them: it is kept, not installed (crash below). */
static uint32_t g_guest_filter;
static void shim_SetUnhandledExceptionFilter(void) {
    uint32_t old = g_guest_filter;
    g_guest_filter = ARG(0);
    g_eax = old;
    g_esp += 4 + 1 * 4;
}

/* The game reports its fatal errors in a message box. The text goes to the
 * log too, where a run without anyone watching it can be read afterwards. */
static void shim_MessageBoxA(void) {
    const char* text = (const char*)(uintptr_t)ARG(1);
    const char* title = (const char*)(uintptr_t)ARG(2);
    fprintf(stderr, "[MessageBoxA] %s: %s\n", title ? title : "", text ? text : "");
    HWND hwnd = (HWND)(uintptr_t)ARG(0);
    UINT type = ARG(3);
    mach_leave();                     /* modal: other guest threads keep running */
    int r = MessageBoxA(hwnd, text, title, type);
    mach_enter();
    g_eax = (uint32_t)r;
    g_esp += 4 + 4 * 4;
}

/* Multimedia timers. A guest callback runs on winmm's thread, but only once
 * it gets the machine lock, and the game's own thread gives that up at its
 * next native call, which can be timeKillEvent itself. So a callback that
 * was already due would run after the kill returned, on state the game has
 * started tearing down. On Windows it would have run to its end first. Each
 * timer here counts its callbacks in flight, and timeKillEvent waits for them
 * with the machine given up. (The same race, and this fix, are documented in
 * redalert2-recomp's host.) */
#define MAX_TIMERS 32
static struct {
    uint32_t proc, user;
    volatile UINT id;
    volatile LONG inflight;
    volatile DWORD thread;
} g_timers[MAX_TIMERS];

static void CALLBACK timer_tramp(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR d1, DWORD_PTR d2) {
    int i = (int)user;
    InterlockedIncrement(&g_timers[i].inflight);
    if (g_timers[i].proc) {
        g_timers[i].thread = GetCurrentThreadId();
        /* A guest address: the fetch faults and native32 runs the lift. */
        ((LPTIMECALLBACK)(uintptr_t)g_timers[i].proc)(id, msg, g_timers[i].user, d1, d2);
        g_timers[i].thread = 0;
    }
    InterlockedDecrement(&g_timers[i].inflight);
}

static void shim_timeSetEvent(void) {
    UINT delay = ARG(0), resolution = ARG(1), flags = ARG(4);
    int i = -1;
    if (!(flags & (TIME_CALLBACK_EVENT_SET | TIME_CALLBACK_EVENT_PULSE)))   /* a function, not an event */
        for (int k = 0; k < MAX_TIMERS && i < 0; k++)
            if (!g_timers[k].proc && !g_timers[k].inflight) i = k;
    if (i < 0) {
        g_eax = timeSetEvent(delay, resolution, (LPTIMECALLBACK)(uintptr_t)ARG(2), ARG(3), flags);
    } else {
        g_timers[i].proc = ARG(2);
        g_timers[i].user = ARG(3);
        g_eax = g_timers[i].id = timeSetEvent(delay, resolution, timer_tramp, (DWORD_PTR)i, flags);
        if (!g_eax) g_timers[i].proc = 0;
    }
    g_esp += 4 + 5 * 4;
}

static void shim_timeKillEvent(void) {
    UINT id = ARG(0);
    int i = -1;
    for (int k = 0; k < MAX_TIMERS && i < 0; k++)
        if (g_timers[k].proc && g_timers[k].id == id) i = k;
    mach_leave();
    MMRESULT r = timeKillEvent(id);
    /* not when the callback kills its own timer: it is the one in flight */
    if (i >= 0 && g_timers[i].thread != GetCurrentThreadId())
        while (g_timers[i].inflight) Sleep(1);
    mach_enter();
    if (i >= 0) g_timers[i].proc = 0, g_timers[i].id = 0;
    g_eax = r;
    g_esp += 4 + 1 * 4;
}

/* The game's fullscreen path measures its screen with GetSystemMetrics on
 * WM_SIZE/WM_MOVE and uses the result as the primary's fill rect and a blit
 * source (winproc.cpp:317-331). cnc-ddraw's hook answered 640x480; with the
 * virtual DirectDraw the host does. */
static int g_cnc_ddraw;               /* --cnc-ddraw: the old path, real ddraw.dll from game\ */
static void shim_GetSystemMetrics(void) {
    int i = (int)ARG(0), w, h;
    if (!g_cnc_ddraw && (i == SM_CXSCREEN || i == SM_CYSCREEN)) {
        vdd_mode(&w, &h);
        g_eax = (uint32_t)(i == SM_CXSCREEN ? w : h);
    } else {
        g_eax = (uint32_t)GetSystemMetrics(i);
    }
    g_esp += 4 + 1 * 4;
}

/* Movies advance only while GetFocus() is the game's window (0x004453C0); in a
 * window that keeps running in the background (vddraw.c), it always is. */
static void shim_GetFocus(void) {
    HWND game = vdd_window();
    g_eax = (uint32_t)(uintptr_t)(game ? game : GetFocus());
    g_esp += 4;
}

static native32_shim_t g_shims[] = {
    { "GetModuleFileNameA", shim_GetModuleFileNameA },
    { "GetCommandLineA", shim_GetCommandLineA },
    { "SetUnhandledExceptionFilter", shim_SetUnhandledExceptionFilter },
    { "MessageBoxA", shim_MessageBoxA },
    { "timeSetEvent", shim_timeSetEvent },
    { "timeKillEvent", shim_timeKillEvent },
    { "DirectInputCreateA", shim_DirectInputCreateA },   /* mouse.c */
    /* the virtual DirectDraw; left out with --cnc-ddraw (they must stay last) */
    { "DirectDrawCreate", shim_DirectDrawCreate },       /* vddraw.c */
    { "GetSystemMetrics", shim_GetSystemMetrics },
    { "GetFocus", shim_GetFocus },
};
#define N_VDD_SHIMS 3

/* ---------------------------------------------------- routing through cnc-ddraw */

/* cnc-ddraw fixes the mouse and the window in windowed mode by patching the
 * import tables of the modules Windows loaded. The guest image is not one of
 * them: native32 fills its imports with plain GetProcAddress results, so
 * measured with --imports, every USER32 slot pointed at user32 itself, while
 * DirectInputCreateA (hooked inline, inside dinput.dll) did reach cnc-ddraw.
 *
 * So the host imports the same USER32 and GDI32 functions the guest does (the
 * table below keeps the linker from dropping them), which gives the host exe,
 * a module Windows loaded, an import-table cell for each, and every guest
 * slot for one of them is pointed at a six-byte `jmp [cell]`. The cell is read
 * at call time, so whatever cnc-ddraw puts there, whenever it puts it, is what
 * the guest calls. --no-route leaves the slots as native32 bound them. */
#define ROUTED_USER32 \
    X(PostMessageA) X(EndPaint) X(BeginPaint) X(PostQuitMessage) X(RedrawWindow) X(GetUpdateRect) \
    X(DefWindowProcA) X(SetRect) X(GetSystemMetrics) X(IsIconic) X(ClientToScreen) X(GetClientRect) \
    X(SetWindowPos) X(GetWindowRect) X(SystemParametersInfoA) X(SetWindowLongA) X(AdjustWindowRectEx) \
    X(GetMenu) X(TranslateMessage) X(GetWindowLongA) X(DispatchMessageA) X(GetFocus) X(SetFocus) \
    X(UpdateWindow) X(GetDC) X(ShowCursor) X(wsprintfA) X(ReleaseDC) X(GetActiveWindow) \
    X(RegisterClassA) X(LoadIconA) X(LoadCursorA) X(SetCursor) X(CreateWindowExA) X(IntersectRect) \
    X(PeekMessageA) X(DestroyWindow)
#define ROUTED_GDI32 \
    X(RealizePalette) X(CreatePalette) X(SetSystemPaletteUse) X(GetPixel) X(SelectObject) \
    X(GetSystemPaletteEntries) X(CreateFontA) X(TextOutA) X(GetTextExtentPoint32A) X(SetTextColor) \
    X(SetBkMode) X(DeleteObject) X(SelectPalette) X(GetDeviceCaps)
#define X(fn) (const void*)fn,
static const void* volatile g_routed_refs[] = { ROUTED_USER32 ROUTED_GDI32 };
#undef X

static int g_no_route;
/* --route-gdi: GDI32 too. Off by default: routed, the palette calls reach
 * cnc-ddraw's hooks and the movies' colours changed (owner, 2026-10-26). */
static int g_route_gdi;
static uint8_t* g_thunks;             /* the routing thunks' page */

/* The host's own import-table cell for dll!name, or NULL. */
static uint32_t* host_iat_cell(const char* dll, const char* name) {
    uint8_t* b = (uint8_t*)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(b + dd.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char*)(b + d->Name), dll)) continue;
        uint32_t* ilt = (uint32_t*)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        for (int i = 0; ilt[i]; i++)
            if (!(ilt[i] & 0x80000000u) && !strcmp((const char*)(b + ilt[i] + 2), name))
                return (uint32_t*)(b + d->FirstThunk) + i;
    }
    return NULL;
}

static void route_through_host_iat(void) {
    uint8_t* b = (uint8_t*)(uintptr_t)AM2_IMAGE_BASE;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(b +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    uint8_t* thunks = g_thunks = (uint8_t*)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int routed = 0, missing = 0, n = 0;
    (void)g_routed_refs[0];
    for (; d->Name; d++) {
        const char* dll = (const char*)(b + d->Name);
        if (_stricmp(dll, "USER32.dll") && (!g_route_gdi || _stricmp(dll, "GDI32.dll"))) continue;
        HMODULE real = GetModuleHandleA(dll);
        uint32_t* ilt = (uint32_t*)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        uint32_t* iat = (uint32_t*)(b + d->FirstThunk);
        for (; *ilt; ilt++, iat++) {
            if (*ilt & 0x80000000u) continue;
            const char* nm = (const char*)(b + *ilt + 2);
            /* only slots bound to the DLL itself: a host shim stays a shim */
            if (*iat != (uint32_t)(uintptr_t)GetProcAddress(real, nm)) continue;
            uint32_t* cell = host_iat_cell(dll, nm);
            if (!cell || n + 6 > 0x1000) { missing++; continue; }
            uint8_t* t = thunks + n;
            t[0] = 0xFF, t[1] = 0x25;                       /* jmp dword ptr [cell] */
            *(uint32_t*)(t + 2) = (uint32_t)(uintptr_t)cell;
            *iat = (uint32_t)(uintptr_t)t;
            n += 8;
            routed++;
        }
    }
    DWORD old;
    VirtualProtect(thunks, 0x1000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), thunks, 0x1000);
    printf("  routed %d %s imports through the host's import table (%d without a host cell)\n",
           routed, g_route_gdi ? "USER32+GDI32" : "USER32", missing);
}

recomp_func_t recomp_lookup_manual(uint32_t va) {
    (void)va;
    return NULL;
}

/* ---------------------------------------------------------------- reports */

static LONG WINAPI crash(EXCEPTION_POINTERS* ep) {
    static volatile LONG once;
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if (InterlockedExchange(&once, 1)) TerminateProcess(GetCurrentProcess(), 3);
    fprintf(stderr, "\n=== fault 0x%08lX at 0x%p, thread %lu ===\n", r->ExceptionCode,
            r->ExceptionAddress, GetCurrentThreadId());
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        fprintf(stderr, "  %s of 0x%08X%s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at,
                native32_in_guest(at) ? " (inside the guest image)" : at < 0x10000 ? " (null/low)" : "");
    }
    fprintf(stderr, "  in lifted sub_%08X, last native call %s\n", g_cur_func, g_cur_import);
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    native32_dump_icalls(12);
    recomp_trace_flush();
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* --imports: where each of the guest's USER32 and DINPUT import slots points
 * once the game is running, and whether the function there now starts with a
 * jmp (an inline hook). The guest image is mapped by native32, not by the
 * Windows loader, so a DLL that hooks by patching loaded modules' import
 * tables (cnc-ddraw's mouse fixes) never sees these slots; this shows whether
 * any hook reached them. */
static int g_report_imports;

static void report_imports(void) {
    uint8_t* b = (uint8_t*)(uintptr_t)AM2_IMAGE_BASE;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(b +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; d->Name; d++) {
        const char* dll = (const char*)(b + d->Name);
        if (_stricmp(dll, "USER32.dll") && _stricmp(dll, "DINPUT.dll") && _stricmp(dll, "GDI32.dll")) continue;
        HMODULE real = GetModuleHandleA(dll);
        uint32_t* ilt = (uint32_t*)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        uint32_t* iat = (uint32_t*)(b + d->FirstThunk);
        for (; *ilt; ilt++, iat++) {
            const char* nm = (*ilt & 0x80000000u) ? "(ordinal)" : (const char*)(b + *ilt + 2);
            uint32_t va = *iat;
            char path[MAX_PATH] = "(no module: a host shim)";
            HMODULE m = NULL;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)va, &m))
                GetModuleFileNameA(m, path, MAX_PATH);
            const char* file = strrchr(path, '\\');
            uint32_t now = real && !(*ilt & 0x80000000u) ? (uint32_t)(uintptr_t)GetProcAddress(real, nm) : 0;
            int jmp = m && *(const uint8_t*)(uintptr_t)va == 0xE9;
            /* a routing thunk, `jmp [host cell]`: follow the cell. Recognised by
             * address, never by reading: a host shim's VA is in native32's
             * reserved, no-access page, and reading it faults. */
            int thunk = g_thunks && va >= (uint32_t)(uintptr_t)g_thunks && va < (uint32_t)(uintptr_t)g_thunks + 0x1000;
            char to[MAX_PATH] = "";
            if (thunk || jmp) {               /* where the hook or the cell sends it */
                uint32_t dst = thunk ? **(const uint32_t**)(uintptr_t)(va + 2)
                                     : va + 5 + *(const int32_t*)(uintptr_t)(va + 1);
                HMODULE dm = NULL;
                char dp[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)dst, &dm))
                    GetModuleFileNameA(dm, dp, MAX_PATH);
                const char* df = strrchr(dp, '\\');
                _snprintf(to, sizeof to - 1, thunk ? "  routed: host cell -> 0x%08X (%s)"
                                                   : "  starts with jmp 0x%08X (%s): inline hook", dst, df ? df + 1 : dp);
            }
            fprintf(stderr, "[imports] %-10s %-22s -> 0x%08X %-14s%s%s\n", dll, nm, va,
                    thunk ? "(thunk)" : file ? file + 1 : path, to,
                    !thunk && now && now != va ? "  (GetProcAddress now differs)" : "");
        }
    }
}

/* --watchdog S: where the guest is after S seconds, then the run ends. For
 * runs nobody watches: a hang reports itself instead of waiting forever. */
static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count);
    if (!g_cnc_ddraw) fprintf(stderr, "[watchdog] presenter: %ld frames shown\n", vdd_presented);
    if (g_report_imports) report_imports();
    native32_dump_icalls(8);
    recomp_trace_flush();
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

/* --dplay-check: create the DirectPlay object the game asks for, in this 32-bit
 * process, and list the connections it offers. The game's .rdata holds
 * CLSID_DirectPlay (0x0046F6D8) and IID_IDirectPlay3A (0x0046F6C8), plus the
 * TCP/IP and IPX service-provider GUIDs. Current SDKs ship no dplay.h, so the
 * two GUIDs, DPNAME and EnumConnections' vtable slot (35: IUnknown's 3,
 * IDirectPlay2's 29, then IDirectPlay3's 4th; checked against Wine's dplay.h)
 * are declared here. Needs no game files. */
static const GUID CLSID_DirectPlay_ =
    { 0xD1EB6D20, 0x8923, 0x11D0, { 0x9D, 0x97, 0x00, 0xA0, 0xC9, 0x0A, 0x43, 0xCB } };
static const GUID IID_IDirectPlay3A_ =
    { 0x133EFE41, 0x32DC, 0x11D0, { 0x9C, 0xFB, 0x00, 0xA0, 0xC9, 0x0A, 0x43, 0xCB } };
typedef struct { DWORD dwSize, dwFlags; LPSTR lpszShortNameA, lpszLongNameA; } dpname_t;
typedef BOOL (CALLBACK *enum_conn_cb)(const GUID*, void*, DWORD, const dpname_t*, DWORD, void*);
typedef HRESULT (STDMETHODCALLTYPE *enum_conn_fn)(void* self, const GUID* app, enum_conn_cb cb, void* ctx, DWORD flags);
typedef ULONG (STDMETHODCALLTYPE *release_fn)(void* self);
#define DPCONNECTION_DIRECTPLAY 0x00000001

static BOOL CALLBACK dplay_connection(const GUID* sp, void* conn, DWORD size, const dpname_t* name, DWORD flags, void* ctx) {
    (void)conn; (void)flags;
    printf("  %-44s provider {%08lX-%04X-%04X-...}, %lu-byte address\n",
           name && name->lpszShortNameA ? name->lpszShortNameA : "(unnamed)",
           sp->Data1, sp->Data2, sp->Data3, size);
    ++*(int*)ctx;
    return TRUE;
}

static int dplay_check(void) {
    void** dp = NULL;
    CoInitialize(NULL);
    HRESULT hr = CoCreateInstance(&CLSID_DirectPlay_, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectPlay3A_, (void**)&dp);
    printf("CoCreateInstance(CLSID_DirectPlay, IID_IDirectPlay3A): 0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr) || !dp) {
        printf("DirectPlay is not available to this process. Enable it with, in an admin PowerShell:\n"
               "  Enable-WindowsOptionalFeature -Online -FeatureName DirectPlay -All\n");
        return 1;
    }
    void** vtbl = *(void***)dp;
    int n = 0;
    hr = ((enum_conn_fn)vtbl[35])(dp, NULL, dplay_connection, &n, DPCONNECTION_DIRECTPLAY);
    printf("EnumConnections: 0x%08lX, %d connection(s)\n", (unsigned long)hr, n);
    ((release_fn)vtbl[2])(dp);
    CoUninitialize();
    return SUCCEEDED(hr) && n > 0 ? 0 : 1;
}

/* ---------------------------------------------------------------- setup */

/* The lifted C is one exe's code, so the exe mapped must be that exe. Retail,
 * GOG and the Russian edition share the PE timestamp and size, and the GOG exe
 * differs from retail in code (six CD-check branches), so only a hash tells
 * them apart: recomp_target.h records the MD5 the lift was made from. */
static const struct { const char* md5; const char* build; } g_known_builds[] = {
    { "59180d8113d1d756e838b0dab1790b6b", "retail 1999, English CD" },
    { "78a6f5344559c2724f48862d22120b2f", "2017 re-release (GOG; Steam likely the same): CD check disabled by the publisher" },
    { "0c760f05a945d7276c67a753a838a295", "Russian Edition (retail code, translated data)" },
};

static int file_md5(const char* path, char out[33]) {
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    FILE* f = fopen(path, "rb");
    int ok = f && CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) &&
             CryptCreateHash(prov, CALG_MD5, 0, 0, &hash);
    uint8_t buf[65536];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, f)) > 0) ok = CryptHashData(hash, buf, (DWORD)n, 0);
    uint8_t digest[16];
    DWORD len = sizeof digest;
    ok = ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0);
    for (int i = 0; ok && i < 16; i++) sprintf(out + 2 * i, "%02x", digest[i]);
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    if (f) fclose(f);
    return ok;
}

static char g_exe_line[160];         /* printed by run(): the launcher checks too */

static int check_exe(void) {
    char md5[33] = "";
    if (!file_md5(g_guest_exe, md5)) {
        fprintf(stderr, "cannot read %s\n", g_guest_exe);
        return 0;
    }
    const char* build = "unknown build";
    for (size_t i = 0; i < sizeof g_known_builds / sizeof g_known_builds[0]; i++)
        if (!strcmp(md5, g_known_builds[i].md5)) build = g_known_builds[i].build;
    if (strcmp(md5, AM2_LIFT_MD5)) {
        fprintf(stderr, "%s is %s (md5 %s),\nbut the C was lifted from %s (md5 %s).\n"
                "Lift the exe you run: python scripts\\run_lift.py <game>\\ArmyMen2.exe src\\recomp\\gen, "
                "then scripts\\build_host.bat\n", g_guest_exe, build, md5, AM2_LIFT_BUILD, AM2_LIFT_MD5);
        return 0;
    }
    _snprintf(g_exe_line, sizeof g_exe_line - 1, "  guest exe: %s, md5 %s\n", build, md5);
    return 1;
}

static DWORD WINAPI run(LPVOID unused) {
    (void)unused;
    native32_init();
    /* Physical pixels for the presenter's window and the cursor mapping, so a
     * scaled desktop does not blur the picture a second time. */
    if (!g_cnc_ddraw) SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetUnhandledExceptionFilter(crash);
    printf("plasticrecomp host: %u lifted functions\n%s", recomp_dispatch_count, g_exe_line);

    /* smackw32.dll and the game's DirectDraw wrapper (cnc-ddraw, as ddraw.dll)
     * ship in the game folder, so imports bind from there first. */
    SetDllDirectoryA(g_game_dir);
    uint32_t span = native32_map(g_guest_exe, AM2_IMAGE_BASE);
    if (!span) {
        fprintf(stderr, "cannot map %s at 0x%08X\n", g_guest_exe, AM2_IMAGE_BASE);
        return 1;
    }
    printf("  mapped %s: 0x%08X-0x%08X\n", g_guest_exe, AM2_IMAGE_BASE, AM2_IMAGE_BASE + span);
    int nshims = (int)(sizeof g_shims / sizeof g_shims[0]) - (g_cnc_ddraw ? N_VDD_SHIMS : 0);
    if (native32_bind(AM2_IMAGE_BASE, g_shims, nshims)) return 1;
    if (g_cnc_ddraw) {
        /* cnc-ddraw's hooks reach the guest only through the host's import table */
        if (!g_no_route) route_through_host_iat();
    } else {
        /* no cnc-ddraw: the virtual DirectDraw's window and letterbox map the
         * mouse, and the mouse device must not be exclusive (mouse.c) */
        mouse_target = vdd_cursor;
        mouse_nonexclusive = 1;
    }
    /* the game's cursor: int32 x, y, written only by sub_00426F40 */
    mouse_set_cursor_vars(0x00485464u, 0x00485468u);
    {
        char path[MAX_PATH];
        HMODULE dd = GetModuleHandleA("ddraw.dll");
        if (dd && GetModuleFileNameA(dd, path, MAX_PATH)) printf("  DirectDraw from %s\n", path);
    }
    if (g_dry) {
        printf("\n(dry run: mapped and bound; without --dry the guest enters at 0x%08X)\n", AM2_ENTRY);
        return 0;
    }

    /* The game opens its data relative to its working folder, and writes
     * Options.cfg and its saves there. */
    if (!SetCurrentDirectoryA(g_game_dir)) {
        fprintf(stderr, "cannot enter %s\n", g_game_dir);
        return 1;
    }
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    printf("  entering 0x%08X\n\n", AM2_ENTRY);
    fflush(stdout);
    native32_call_guest(AM2_ENTRY, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    return g_eax;
}

int main(int argc, char** argv) {
    const char* game = "game";
    /* Both unbuffered: the log is the evidence, and the guest leaves through
     * ExitProcess and the watchdog through TerminateProcess, which flush nothing.
     * Redirected to a file, stdout would otherwise be fully buffered. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = (DWORD)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dry")) g_dry = 1;
        else if (!strcmp(argv[i], "--imports")) g_report_imports = 1;
        else if (!strcmp(argv[i], "--no-route")) g_no_route = 1;
        else if (!strcmp(argv[i], "--route-gdi")) g_route_gdi = 1;
        else if (!strcmp(argv[i], "--cnc-ddraw")) g_cnc_ddraw = 1;
        else if (!strcmp(argv[i], "--dd-trace")) vdd_trace = 1;
        else if (!strcmp(argv[i], "--mouse-log")) mouse_log = 1;
        else if (!strcmp(argv[i], "--raw-mouse")) mouse_raw = 1;
        else if (!strcmp(argv[i], "--dplay-check")) return dplay_check();
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else {
            fprintf(stderr, "usage: %s [--game DIR] [--watchdog S] [--dry] [--imports] [--no-route] [--route-gdi] "
                    "[--raw-mouse] [--mouse-log] [--native-trace] [--callbacks] | --dplay-check\n", argv[0]);
            return 1;
        }
    }
    GetFullPathNameA(game, MAX_PATH, g_game_dir, NULL);
    _snprintf(g_guest_exe, sizeof g_guest_exe - 1, "%s\\ArmyMen2.exe", g_game_dir);
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\"", g_guest_exe);
    if (!check_exe()) return 1;

    /* In the launching process this starts the host again with the image's
     * range reserved and never returns; in that child it returns 1. */
    if (!native32_hold_image(g_guest_exe, AM2_IMAGE_BASE))
        fprintf(stderr, "warning: could not hold 0x%08X from process start\n", AM2_IMAGE_BASE);

    HANDLE t = CreateThread(NULL, RUN_STACK, run, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t) {
        fprintf(stderr, "cannot start the run thread (error %lu)\n", GetLastError());
        return 1;
    }
    WaitForSingleObject(t, INFINITE);
    DWORD code = 1;
    GetExitCodeThread(t, &code);
    return (int)code;
}
