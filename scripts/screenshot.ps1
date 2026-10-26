<#
screenshot.ps1 - list and capture the visible top-level windows of the plasticrecomp host.

Every visible top-level window owned by any process named -ProcessName (default plasticrecomp:
the host re-launches itself, and the game window belongs to the child, so all PIDs with the
name are searched) or by the PIDs in -Id is printed (title, class, PID, rect in physical
screen pixels, window DPI) and saved as build\screens\<yyyyMMdd-HHmmss>_<n>.png.

Capture: PrintWindow(PW_RENDERFULLCONTENT) first, done in the window's own DPI context so the
bitmap is the size the window renders at. If that image is all one colour (cnc-ddraw GL/D3D
output can come back black), the window is restored and brought to the front (SetForegroundWindow,
else made topmost for the duration of the copy) and its visible frame is copied from the screen
(a BitBlt from the screen DC, i.e. what Graphics.CopyFromScreen does, in physical pixels).
  -Screen    skip PrintWindow, always copy from the screen
  -PassThru  also output one object per saved image (used by boot_check.ps1)
#>
param([string]$ProcessName = 'plasticrecomp', [int[]]$Id, [switch]$Screen, [switch]$PassThru)

$outDir = Join-Path (Split-Path $PSScriptRoot -Parent) 'build\screens'
Add-Type -AssemblyName System.Drawing
if (-not ('PlasticShot' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class PlasticShot {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] struct BITMAPINFOHEADER {
        public int biSize, biWidth, biHeight; public short biPlanes, biBitCount;
        public int biCompression, biSizeImage, biXPelsPerMeter, biYPelsPerMeter, biClrUsed, biClrImportant;
    }
    public class Shot { public IntPtr Hbitmap; public int Width, Height; public bool Uniform; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);

    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] static extern int GetWindowLong(IntPtr h, int index);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr GetWindowDpiAwarenessContext(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h, IntPtr dc);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
    [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
    [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
    [DllImport("gdi32.dll")] static extern IntPtr CreateDIBSection(IntPtr dc, ref BITMAPINFOHEADER bi, uint usage, out IntPtr bits, IntPtr section, uint offset);
    [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);
    [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr obj);
    [DllImport("gdi32.dll")] static extern bool BitBlt(IntPtr dst, int x, int y, int w, int h, IntPtr src, int sx, int sy, uint rop);
    [DllImport("gdi32.dll")] static extern bool GdiFlush();
    [StructLayout(LayoutKind.Sequential)] struct POINT { public int X, Y; }
    [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr h, uint flags);
    static readonly IntPtr PerMonitorV2 = new IntPtr(-4);

    public static List<IntPtr> Visible(int[] pids) {
        var list = new List<IntPtr>();
        EnumWindows((h, l) => { if (IsWindowVisible(h) && Array.IndexOf(pids, Pid(h)) >= 0) list.Add(h); return true; }, IntPtr.Zero);
        return list;
    }
    public static int Pid(IntPtr h) { uint pid; GetWindowThreadProcessId(h, out pid); return (int)pid; }
    public static string Title(IntPtr h) { var s = new StringBuilder(512); GetWindowText(h, s, s.Capacity); return s.ToString(); }
    public static string Class(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, s.Capacity); return s.ToString(); }

    // Window rect in physical screen pixels.
    public static RECT Rect(IntPtr h) {
        var old = SetThreadDpiAwarenessContext(PerMonitorV2);
        try { RECT r; GetWindowRect(h, out r); return r; } finally { SetThreadDpiAwarenessContext(old); }
    }

    // PrintWindow in the window's own DPI context, so the bitmap is the size the window renders at.
    public static Shot Print(IntPtr hwnd) {
        var old = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(hwnd));
        try {
            RECT r; GetWindowRect(hwnd, out r);
            return Capture(r.Right - r.Left, r.Bottom - r.Top, dc => PrintWindow(hwnd, dc, 2));  // PW_RENDERFULLCONTENT
        } finally { SetThreadDpiAwarenessContext(old); }
    }

    // Copy of the visible frame (DWM bounds, no invisible resize border) from the screen.
    public static Shot Screen(IntPtr hwnd) {
        var old = SetThreadDpiAwarenessContext(PerMonitorV2);
        try {
            RECT r;
            if (DwmGetWindowAttribute(hwnd, 9, out r, Marshal.SizeOf(typeof(RECT))) != 0) GetWindowRect(hwnd, out r);
            int w = r.Right - r.Left, h = r.Bottom - r.Top;
            return Capture(w, h, dc => {
                IntPtr screen = GetDC(IntPtr.Zero);
                BitBlt(dc, 0, 0, w, h, screen, r.Left, r.Top, 0x00CC0020 | 0x40000000);  // SRCCOPY | CAPTUREBLT
                ReleaseDC(IntPtr.Zero, screen);
            });
        } finally { SetThreadDpiAwarenessContext(old); }
    }

    // Restore and raise. Returns true if the window had to be made topmost (undo with Lower).
    public static bool Raise(IntPtr h) {
        if (IsIconic(h)) { ShowWindow(h, 9); System.Threading.Thread.Sleep(300); }  // SW_RESTORE
        if (GetForegroundWindow() == h) return false;
        SetForegroundWindow(h); System.Threading.Thread.Sleep(300);
        if (GetForegroundWindow() == h || (GetWindowLong(h, -20) & 0x8) != 0) return false;  // WS_EX_TOPMOST already
        // A background process is usually refused the foreground: put the window in the
        // topmost band without activating it (SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE).
        SetWindowPos(h, new IntPtr(-1), 0, 0, 0, 0, 0x13); System.Threading.Thread.Sleep(300);
        return true;
    }
    public static void Lower(IntPtr h) { SetWindowPos(h, new IntPtr(-2), 0, 0, 0, 0, 0x13); }

    // How many of 5 probe points (centre and inset corners of the frame) show this window on top.
    public static int OnTop(IntPtr hwnd) {
        var old = SetThreadDpiAwarenessContext(PerMonitorV2);
        try {
            RECT r;
            if (DwmGetWindowAttribute(hwnd, 9, out r, Marshal.SizeOf(typeof(RECT))) != 0) GetWindowRect(hwnd, out r);
            int dx = Math.Max(1, (r.Right - r.Left) / 10), dy = Math.Max(1, (r.Bottom - r.Top) / 10), hits = 0;
            int[,] pts = { { (r.Left + r.Right) / 2, (r.Top + r.Bottom) / 2 }, { r.Left + dx, r.Top + dy },
                           { r.Right - dx, r.Top + dy }, { r.Left + dx, r.Bottom - dy }, { r.Right - dx, r.Bottom - dy } };
            for (int i = 0; i < 5; i++)
                if (GetAncestor(WindowFromPoint(new POINT { X = pts[i, 0], Y = pts[i, 1] }), 2) == hwnd) hits++;  // GA_ROOT
            return hits;
        } finally { SetThreadDpiAwarenessContext(old); }
    }

    // Draws into a 32-bit top-down DIB section and checks whether every pixel has one RGB value.
    static Shot Capture(int w, int h, Action<IntPtr> paint) {
        if (w <= 0 || h <= 0) return null;
        var bi = new BITMAPINFOHEADER { biSize = 40, biWidth = w, biHeight = -h, biPlanes = 1, biBitCount = 32 };
        IntPtr screen = GetDC(IntPtr.Zero), mem = CreateCompatibleDC(screen), bits;
        IntPtr hbm = CreateDIBSection(screen, ref bi, 0, out bits, IntPtr.Zero, 0);
        IntPtr prev = SelectObject(mem, hbm);
        try {
            paint(mem);
            GdiFlush();
            var px = new int[w * h];
            Marshal.Copy(bits, px, 0, px.Length);
            bool uniform = true;
            foreach (int p in px) if (((p ^ px[0]) & 0xFFFFFF) != 0) { uniform = false; break; }
            return new Shot { Hbitmap = hbm, Width = w, Height = h, Uniform = uniform };
        } finally { SelectObject(mem, prev); DeleteDC(mem); ReleaseDC(IntPtr.Zero, screen); }
    }
}
'@
}

$pids = if ($Id) { $Id } else { @(Get-Process -Name $ProcessName -ErrorAction Ignore).Id }
if (-not $pids) { Write-Host "screenshot: no process named '$ProcessName'"; return }
$windows = [PlasticShot]::Visible([int[]]$pids)
Write-Host ("screenshot: {0} visible window(s) for PID(s) {1}" -f $windows.Count, ($pids -join ', '))
$null = New-Item -ItemType Directory -Force $outDir
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$i = 0; $n = 0

foreach ($hwnd in $windows) {
    $i++
    $r = [PlasticShot]::Rect($hwnd); $title = [PlasticShot]::Title($hwnd); $winPid = [PlasticShot]::Pid($hwnd)
    Write-Host ('[{0}] "{1}"  class={2}  pid={3}  rect=({4},{5})-({6},{7}) {8}x{9}  dpi={10}' -f $i, $title,
        [PlasticShot]::Class($hwnd), $winPid, $r.Left, $r.Top, $r.Right, $r.Bottom,
        ($r.Right - $r.Left), ($r.Bottom - $r.Top), [PlasticShot]::GetDpiForWindow($hwnd))
    if ($r.Right -le $r.Left -or $r.Bottom -le $r.Top) { Write-Host '    skipped: empty rect'; continue }

    $method = 'PrintWindow'
    $shot = if (-not $Screen) { [PlasticShot]::Print($hwnd) }
    if (-not $shot -or $shot.Uniform) {
        $method = if ($shot) { 'CopyFromScreen (PrintWindow image was uniform)' } else { 'CopyFromScreen' }
        if ($shot) { $null = [PlasticShot]::DeleteObject($shot.Hbitmap) }
        $raised = [PlasticShot]::Raise($hwnd)
        $onTop = [PlasticShot]::OnTop($hwnd)
        if ($onTop -lt 5) { Write-Host "    warning: window on top at only $onTop of 5 probe points; the copy may include other windows" }
        $shot = [PlasticShot]::Screen($hwnd)
        if ($raised) { [PlasticShot]::Lower($hwnd); $method += ', raised to topmost for the copy' }
    }
    if (-not $shot) { Write-Host '    skipped: nothing captured'; continue }

    do { $n++; $path = Join-Path $outDir "${stamp}_$n.png" } while (Test-Path $path)
    $image = [System.Drawing.Image]::FromHbitmap($shot.Hbitmap)
    $image.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $image.Dispose(); $null = [PlasticShot]::DeleteObject($shot.Hbitmap)
    Write-Host "    saved $path  $($shot.Width)x$($shot.Height)  $method  uniform=$($shot.Uniform)"
    if ($PassThru) { [pscustomobject]@{ Path = $path; Pid = $winPid; Title = $title; Method = $method; Uniform = $shot.Uniform } }
}
