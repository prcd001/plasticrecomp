<#
run.ps1 - run build\plasticrecomp.exe from C:\plasticrecomp and keep its output.

  pwsh scripts\run.ps1 [host args...]        (default: --watchdog 30)

stdout and stderr go to build\run.log (overwritten). cmd does the redirection, so both streams
share one file handle and keep the order the host wrote them in; the re-launched child inherits
the same handles, so its output lands there too. Then prints the exit code and the last 40 log
lines, and exits with the host's exit code (1 if the exe is missing).
#>
$root = Split-Path $PSScriptRoot -Parent
$exe  = Join-Path $root 'build\plasticrecomp.exe'
$log  = Join-Path $root 'build\run.log'
$hostArgs = if ($args.Count) { @($args | ForEach-Object { "$_" }) } else { '--watchdog', '30' }
if (-not (Test-Path $exe)) { Write-Host "run: $exe not found"; exit 1 }

$line = ($hostArgs | ForEach-Object { if ($_ -match '[\s"&|<>^()]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
Write-Host "run: $exe $line"
$p = Start-Process cmd.exe -ArgumentList "/d /s /c `"`"$exe`" $line > `"$log`" 2>&1`"" `
    -WorkingDirectory $root -NoNewWindow -PassThru
$null = $p.Handle   # hold the process handle so ExitCode is still readable after it exits
$p.WaitForExit()
$code = $p.ExitCode

Write-Host ("run: exit code {0} (0x{0:X8})" -f $code)
Write-Host "run: last 40 lines of $log"
Get-Content $log -Tail 40
exit $code
