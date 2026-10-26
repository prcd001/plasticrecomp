<#
boot_check.ps1 - boot smoke test for build\plasticrecomp.exe.

  pwsh scripts\boot_check.ps1 [host args...]   (default: --watchdog 40)

Starts the host in the background from C:\plasticrecomp, stdout+stderr in build\run.log (one
handle, via cmd, as in run.ps1). At 5, 12, 20 and 30 s it lists the windows of every process
named plasticrecomp and screenshots them (scripts\screenshot.ps1). It waits for the host to
exit; at the watchdog time + 10 s (40 + 10 when no --watchdog is given) it kills every
plasticrecomp process instead. Finally prints the exit code, the screenshots taken and the last
60 lines of run.log, and exits with the host's exit code (1 if the exe is missing).
#>
$root = Split-Path $PSScriptRoot -Parent
$exe  = Join-Path $root 'build\plasticrecomp.exe'
$log  = Join-Path $root 'build\run.log'
$hostArgs = if ($args.Count) { @($args | ForEach-Object { "$_" }) } else { '--watchdog', '40' }
$w = [array]::IndexOf($hostArgs, '--watchdog')
$deadline = 10 + $(if ($w -ge 0 -and $w + 1 -lt $hostArgs.Count) { [double]$hostArgs[$w + 1] } else { 40 })
if (-not (Test-Path $exe)) { Write-Host "boot_check: $exe not found"; exit 1 }
$stale = Get-Process plasticrecomp -ErrorAction Ignore
if ($stale) { Write-Host "boot_check: warning: plasticrecomp already running (PID $($stale.Id -join ', ')); it is captured and killed too" }

$line = ($hostArgs | ForEach-Object { if ($_ -match '[\s"&|<>^()]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
Write-Host "boot_check: $exe $line  (kill deadline $deadline s)"
$p = Start-Process cmd.exe -ArgumentList "/d /s /c `"`"$exe`" $line > `"$log`" 2>&1`"" `
    -WorkingDirectory $root -NoNewWindow -PassThru
$null = $p.Handle   # hold the process handle so ExitCode is still readable after it exits
$clock = [Diagnostics.Stopwatch]::StartNew()
$shots = @()

foreach ($t in 5, 12, 20, 30) {
    if ($t -ge $deadline) { break }
    $wait = [math]::Max(0, [int](($t - $clock.Elapsed.TotalSeconds) * 1000))
    if ($p.WaitForExit($wait)) { break }
    Write-Host "`n=== t=$t s ==="
    $shots += @(& (Join-Path $PSScriptRoot 'screenshot.ps1') -PassThru)
}

$killed = $false
if (-not $p.WaitForExit([math]::Max(0, [int](($deadline - $clock.Elapsed.TotalSeconds) * 1000)))) {
    Write-Host "`nboot_check: still running after $deadline s; killing every plasticrecomp process"
    Get-Process plasticrecomp -ErrorAction Ignore | Stop-Process -Force
    $killed = $true
    $null = $p.WaitForExit(5000)
}
$left = Get-Process plasticrecomp -ErrorAction Ignore
if ($left) { Write-Host "boot_check: plasticrecomp still alive after the host exited (PID $($left.Id -join ', ')); killing"; $left | Stop-Process -Force }

$code = $p.ExitCode
Write-Host ("`nboot_check: exit code {0} (0x{0:X8}){1}, after {2:N1} s" -f $code, $(if ($killed) { ', killed by boot_check' }), $clock.Elapsed.TotalSeconds)
Write-Host "boot_check: $($shots.Count) screenshot(s)"
foreach ($s in $shots) { Write-Host "  $($s.Path)  pid=$($s.Pid)  uniform=$($s.Uniform)  $($s.Method)  `"$($s.Title)`"" }
Write-Host "boot_check: last 60 lines of $log"
Get-Content $log -Tail 60
exit $code
