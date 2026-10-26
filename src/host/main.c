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
 * usage: plasticrecomp [--game DIR] [--watchdog S] [--dry] [--native-trace] [--callbacks]
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "recomp_trace.h"

#define AM2_IMAGE_BASE 0x00400000u
#define AM2_ENTRY      0x004664C0u   /* the PE entry point: MSVC 6's WinMainCRTStartup */
#define AM2_EXE_STAMP  0x36B7AE36u   /* PE timestamp of the build the C was lifted from */
#define AM2_EXE_SIZE   585728u

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

static native32_shim_t g_shims[] = {
    { "GetModuleFileNameA", shim_GetModuleFileNameA },
    { "GetCommandLineA", shim_GetCommandLineA },
    { "SetUnhandledExceptionFilter", shim_SetUnhandledExceptionFilter },
    { "MessageBoxA", shim_MessageBoxA },
    { "timeSetEvent", shim_timeSetEvent },
    { "timeKillEvent", shim_timeKillEvent },
};

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

/* --watchdog S: where the guest is after S seconds, then the run ends. For
 * runs nobody watches: a hang reports itself instead of waiting forever. */
static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count);
    native32_dump_icalls(8);
    recomp_trace_flush();
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

/* ---------------------------------------------------------------- setup */

static int check_exe(void) {
    uint8_t head[4096] = { 0 };
    FILE* f = fopen(g_guest_exe, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", g_guest_exe);
        return 0;
    }
    fread(head, 1, sizeof head, f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    uint32_t nt = *(const uint32_t*)(head + 0x3C);
    uint32_t stamp = nt < sizeof head - 12 ? *(const uint32_t*)(head + nt + 8) : 0;
    if (stamp != AM2_EXE_STAMP || (uint32_t)size != AM2_EXE_SIZE) {
        fprintf(stderr, "%s is not the build the C was lifted from (timestamp 0x%08X, %ld bytes; "
                "wanted 0x%08X, %u)\n", g_guest_exe, stamp, size, AM2_EXE_STAMP, AM2_EXE_SIZE);
        return 0;
    }
    return 1;
}

static DWORD WINAPI run(LPVOID unused) {
    (void)unused;
    native32_init();
    SetUnhandledExceptionFilter(crash);
    printf("plasticrecomp host: %u lifted functions\n", recomp_dispatch_count);

    /* smackw32.dll and the game's DirectDraw wrapper (cnc-ddraw, as ddraw.dll)
     * ship in the game folder, so imports bind from there first. */
    SetDllDirectoryA(g_game_dir);
    uint32_t span = native32_map(g_guest_exe, AM2_IMAGE_BASE);
    if (!span) {
        fprintf(stderr, "cannot map %s at 0x%08X\n", g_guest_exe, AM2_IMAGE_BASE);
        return 1;
    }
    printf("  mapped %s: 0x%08X-0x%08X\n", g_guest_exe, AM2_IMAGE_BASE, AM2_IMAGE_BASE + span);
    if (native32_bind(AM2_IMAGE_BASE, g_shims, (int)(sizeof g_shims / sizeof g_shims[0]))) return 1;
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
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else {
            fprintf(stderr, "usage: %s [--game DIR] [--watchdog S] [--dry] [--native-trace] [--callbacks]\n",
                    argv[0]);
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
