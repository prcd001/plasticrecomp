<#
auto_bootcamp.ps1 - unattended path from launch to a Boot Camp mission screenshot.

  pwsh scripts\auto_bootcamp.ps1 [-Tag before] [-Duration 110] [-Shots '12,28,40,55,70,90'] [-HostArgs ...]

Starts build\plasticrecomp.exe from C:\plasticrecomp with the test switches PLASTIC_KEYS and
PLASTIC_SCRIPT (src\host\mouse.c) set, so no real keyboard or mouse is involved and the game
need not be in the foreground:
  - Escape (DIK 1) at 6 s and 20 s skips the two intro movies; Space closes the sheet below;
  - the cursor goes to the BOOT CAMP button of the title menu and clicks it (-ClickAt, game pixels;
    measured 2026-10-27 on the 640x480 menu: button spans x 231..383, y 130..155, centre 307,142);
  - the game then runs the CD check (disc image at H:) and loads the mission (reached about 34 s
    after launch on the owner's PC); a "Welcome to Boot Camp!" box is up, with OK at game pixel
    478,225 (-OkAt), clicked at -OkTime; a Boot Camp instruction sheet follows and Space (at
    -OkTime + 6) closes it; the pointer is parked at 200,150 (-OkTime + 12).
At each -Shots time (seconds after launch) the host window is screenshotted (scripts\screenshot.ps1),
the file copied to build\screens\<Tag>_<time>s.png. At -Duration seconds every plasticrecomp process
is killed (the host's own --watchdog is set 15 s later as a second line of defence). The host log is
build\auto_<Tag>.log. Refuses to start if plasticrecomp.exe is already running. Prints the screenshot
paths, the host exit code and the [script]/[hires]/[ddraw] lines of the log.

  -Keys / -Script override the default PLASTIC_KEYS / PLASTIC_SCRIPT strings; -Env sets more
  environment variables (e.g. @{PLASTIC_ZOOM='2'}).
  -Do runs window actions at given times, e.g. -Do '70:maximize','90:restore','100:size 1000 700',
  '110:key OemPlus', '105:syskey Enter', '108:close' (WM_KEYDOWN / WM_KEYUP posted to the game's window: it reaches the window procedure, not DirectInput). They need no extra -Shots entry.
#>
param(
    [string]$Tag = 'run',
    [int]$Duration = 110,
    [string[]]$Shots = @('12','28','40','55','70','90'),
    [string[]]$ClickAt = @('307', '142'),
    [int]$ClickTime = 30,
    [string[]]$OkAt = @('478', '225'),
    [int]$OkTime = 44,
    [string]$Keys,
    [string]$Script,
    [hashtable]$Env = @{},
    [string[]]$Do = @(),
    [string[]]$HostArgs = @()
)
$ShotTimes = @($Shots -split ',' | Where-Object { $_ } | ForEach-Object { [int]$_ } | Sort-Object)   # also fine from pwsh -File
$Do = @($Do -split ';' | Where-Object { $_ })   # from pwsh -File pass one string: -Do '66:key F3;68:key F12'
$Ok = @($OkAt -split ',' | ForEach-Object { [int]$_ })
$Click = @($ClickAt -split ',' | ForEach-Object { [int]$_ })
$root = Split-Path $PSScriptRoot -Parent
$exe  = Join-Path $root 'build\plasticrecomp.exe'
$log  = Join-Path $root "build\auto_$Tag.log"
$shotDir = Join-Path $root 'build\screens'
if (-not (Test-Path $exe)) { Write-Host "auto_bootcamp: $exe not found"; exit 1 }
if (Get-Process plasticrecomp -ErrorAction Ignore) { Write-Host 'auto_bootcamp: plasticrecomp is already running; not starting'; exit 1 }
if (-not (Test-Path 'H:\')) { Write-Host 'auto_bootcamp: warning: H:\ (the mounted disc image) is missing; the CD check will fail' }
if (-not $Keys) { $Keys = "6:ESC;20:ESC;{0}:SPACE" -f ($OkTime + 6) }   # Space closes the Boot Camp sheet
if (-not $Script) {
    # title: hover, then click BOOT CAMP; mission: click OK on "Welcome to Boot Camp!", park the pointer
    $Script = ("{0}:{1},{2};{3}:{1},{2},click;{4}:{5},{6};{7}:{5},{6},click;{8}:200,150" -f ($ClickTime - 2), $Click[0], $Click[1],
        $ClickTime, ($OkTime - 2), $Ok[0], $Ok[1], $OkTime, ($OkTime + 12))
}

$saved = @{}
$vars = @{ PLASTIC_KEYS = $Keys; PLASTIC_SCRIPT = $Script } + $Env
foreach ($k in $vars.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, [string]$vars[$k]) }
$line = (@('--game', (Join-Path $root 'game'), '--watchdog', ($Duration + 15)) + $HostArgs) -join ' '
Write-Host "auto_bootcamp[$Tag]: PLASTIC_KEYS=$Keys PLASTIC_SCRIPT=$Script"
Write-Host "auto_bootcamp[$Tag]: $exe $line"
$p = Start-Process cmd.exe -ArgumentList "/d /s /c `"`"$exe`" $line > `"$log`" 2>&1`"" `
    -WorkingDirectory $root -NoNewWindow -PassThru
$null = $p.Handle
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }   # the child has its copy
$clock = [Diagnostics.Stopwatch]::StartNew()
$taken = @()

if ($Do.Count -and -not ('AbWin' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class AbWin {
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
}
'@
}
# the timeline: screenshots and -Do actions in time order
$timeline = @($ShotTimes | ForEach-Object { [pscustomobject]@{ T = $_; Kind = 'shot'; Arg = '' } })
foreach ($d in $Do) {
    $c = $d.IndexOf(':')
    $timeline += [pscustomobject]@{ T = [int]$d.Substring(0, $c); Kind = 'do'; Arg = $d.Substring($c + 1) }
}
$timeline = @($timeline | Sort-Object T)

foreach ($ev in $timeline) {
    $t = $ev.T
    if ($t -ge $Duration) { break }
    $wait = [math]::Max(0, [int](($t - $clock.Elapsed.TotalSeconds) * 1000))
    if ($p.WaitForExit($wait)) { Write-Host "auto_bootcamp[$Tag]: host exited early at $([int]$clock.Elapsed.TotalSeconds) s"; break }
    if ($ev.Kind -eq 'do') {
        $w = Get-Process plasticrecomp -ErrorAction Ignore | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
        if (-not $w) { Write-Host "auto_bootcamp[$Tag]: t=$t s: '$($ev.Arg)': no window"; continue }
        $h = $w.MainWindowHandle
        $a = $ev.Arg -split ' '
        switch ($a[0]) {
            'maximize' { [AbWin]::ShowWindow($h, 3) | Out-Null }
            'restore'  { [AbWin]::ShowWindow($h, 9) | Out-Null }
            'size'     { [AbWin]::SetWindowPos($h, [IntPtr]::Zero, 0, 0, [int]$a[1], [int]$a[2], 0x16) | Out-Null }   # outer size; NOMOVE|NOZORDER|NOACTIVATE
            'key'      {   # posted to the game's window (WM_KEYDOWN, WM_KEYUP), whatever has the focus: nothing is typed elsewhere
                foreach ($k in $a[1..($a.Count - 1)]) {
                    $vk = [int][Enum]::Parse([System.ConsoleKey], $k, $true)
                    [AbWin]::PostMessage($h, 0x100, [IntPtr]$vk, [IntPtr]1) | Out-Null
                    Start-Sleep -Milliseconds 80
                    [AbWin]::PostMessage($h, 0x101, [IntPtr]$vk, [IntPtr]0xC0000001) | Out-Null
                }
            }
            'syskey'   {   # as 'key' but WM_SYSKEYDOWN / WM_SYSKEYUP with the Alt bit (Alt+Enter: 'syskey Enter'; F10 also arrives so)
                foreach ($k in $a[1..($a.Count - 1)]) {
                    $vk = [int][Enum]::Parse([System.ConsoleKey], $k, $true)
                    [AbWin]::PostMessage($h, 0x104, [IntPtr]$vk, [IntPtr]0x20000001) | Out-Null
                    Start-Sleep -Milliseconds 80
                    [AbWin]::PostMessage($h, 0x105, [IntPtr]$vk, [IntPtr]0xE0000001) | Out-Null
                }
            }
            'close'    { [AbWin]::PostMessage($h, 0x10, [IntPtr]0, [IntPtr]0) | Out-Null }   # WM_CLOSE: the game exits through ExitProcess (the ini is written)
            default    { Write-Host "auto_bootcamp[$Tag]: unknown -Do action '$($ev.Arg)'" }
        }
        Write-Host "auto_bootcamp[$Tag]: t=$t s: $($ev.Arg)"
        continue
    }
    $r = @(& (Join-Path $PSScriptRoot 'screenshot.ps1') -PassThru 6>$null | Where-Object { $_.Path })
    if ($r.Count) {
        $dest = Join-Path $shotDir ("{0}_{1:D3}s.png" -f $Tag, $t)
        Copy-Item $r[0].Path $dest -Force
        $taken += $dest
        Write-Host "auto_bootcamp[$Tag]: t=$t s -> $dest"
    } else { Write-Host "auto_bootcamp[$Tag]: t=$t s: no window to capture" }
}

$killed = $false
if (-not $p.WaitForExit([math]::Max(0, [int](($Duration - $clock.Elapsed.TotalSeconds) * 1000)))) {
    Get-Process plasticrecomp -ErrorAction Ignore | Stop-Process -Force
    $killed = $true
    $null = $p.WaitForExit(5000)
}
Get-Process plasticrecomp -ErrorAction Ignore | Stop-Process -Force
$code = $p.ExitCode
Write-Host ("auto_bootcamp[{0}]: exit code {1}{2}, after {3:N1} s, {4} screenshot(s)" -f $Tag, $code, $(if ($killed) { ' (killed at the deadline)' }), $clock.Elapsed.TotalSeconds, $taken.Count)
Select-String -Path $log -Pattern '^\[(script|hires|ddraw)\]|error|trap|fault' | ForEach-Object { $_.Line } | Select-Object -First 40
exit 0
