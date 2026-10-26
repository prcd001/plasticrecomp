# sendkey.ps1 - bring the game's window to the front and press keys in it, the way a player
# would (SendInput). Used to skip the intro movies (Escape) during a boot check.
#   pwsh scripts/sendkey.ps1 [-Keys Escape,Enter] [-ProcessName plasticrecomp] [-DelayMs 300]
# Key names are System.ConsoleKey names (Escape, Enter, Spacebar, UpArrow, ...).
param(
    [string[]]$Keys = @('Escape'),
    [string]$ProcessName = 'plasticrecomp',
    [int]$DelayMs = 300
)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class KeyPress {
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit, Size = 28)] struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(4)] public KEYBDINPUT ki; }
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] static extern uint MapVirtualKey(uint code, uint mapType);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    const uint KEYUP = 2, SCANCODE = 8;
    // Scan codes as well as virtual keys: DirectInput keyboards read the scan code.
    public static void Press(ushort vk) {
        ushort scan = (ushort)MapVirtualKey(vk, 0);
        INPUT[] down = { new INPUT { type = 1, ki = new KEYBDINPUT { wVk = vk, wScan = scan, dwFlags = SCANCODE } } };
        INPUT[] up = { new INPUT { type = 1, ki = new KEYBDINPUT { wVk = vk, wScan = scan, dwFlags = SCANCODE | KEYUP } } };
        SendInput(1, down, Marshal.SizeOf(typeof(INPUT)));
        System.Threading.Thread.Sleep(80);
        SendInput(1, up, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@

$window = Get-Process $ProcessName -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
if (-not $window) { Write-Error "no window owned by a '$ProcessName' process"; exit 1 }
[KeyPress]::SetForegroundWindow($window.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 200
foreach ($key in $Keys) {
    $vk = [uint16][int][Enum]::Parse([System.ConsoleKey], $key, $true)
    [KeyPress]::Press($vk)
    Write-Output "pressed $key in '$($window.MainWindowTitle)' (pid $($window.Id))"
    Start-Sleep -Milliseconds $DelayMs
}
