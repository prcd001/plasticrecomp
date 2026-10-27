<#
setup.ps1 - from "I own Army Men II" to a built plasticrecomp host, in one command.

  pwsh scripts\setup.ps1 [-From <game folder>] [-GameDir <dir>] [-GenDir <dir>] [-NoBuild]
                         [-NoLift] [-NoFetch] [-Force] [-Report <json>]

  0. First run: fetches what the repository does not hold, before anything is copied: pcrecomp
     (git clone at the commit pinned in NOTES.md), the cnc-ddraw v7.1.0.0 zip from its GitHub
     release (SHA-256 checked) and a .venv with capstone and pefile. Needs git, Python 3 and
     the Visual Studio 2022 Build Tools (C++ x86); a missing one is named. -NoFetch downloads
     nothing and stops at the first missing piece instead.
  1. Finds the game: -From (the folder holding ArmyMen2.exe: the retail disc, a copy of its
     files, or an install), else, read only, the GOG install in the registry (game id
     1913968552) and the Steam install (app 549170, via Steam's libraryfolders.vdf and
     appmanifest_549170.acf). If both are found it stops and asks for -From.
  2. Names the build by ArmyMen2.exe's MD5: retail English CD (has the CD check), the 2017
     re-release (GOG, CD check disabled by the publisher; Steam's is likely the same exe),
     Russian Edition, or unknown (a warning, not an error). An exe wrapped in Steam's DRM
     (a PE section named .bind) is refused: this project does not remove copy protection.
  3. Copies the folder to -GameDir (default game\). The game writes Options.cfg and its saves
     into its working folder, so it never runs in the player's install, and the source folder
     is never written. Re-running on the same build only adds missing files (saves and
     Options.cfg stay); a -GameDir holding a different exe is refused unless -Force.
  4. Installs cnc-ddraw v7.1.0.0 from third_party\cnc-ddraw into the copy: windowed=true and
     savesettings=0 under [ddraw], plus a [plasticrecomp] profile equal to its [ArmyMen2] one.
     A GOG or Steam install's own ddraw.dll wrapper is replaced in the copy only.
     -NoLift stops here (no lift, no build).
  5. Lifts the copy's ArmyMen2.exe with scripts\run_lift.py into -GenDir (default
     src\recomp\gen, cleared first; the table-arms report goes to -Report, default
     work\table_arms.json). The host refuses any exe but the one lifted, so every player lifts
     their own: a GOG lift simply has no CD check.
  6. Builds build\plasticrecomp.exe with scripts\build_host.bat, unless -NoBuild.
  7. Prints the command that runs it.
  Exits non-zero on any failure.
#>
param(
    [string]$From,
    [string]$GameDir,
    [string]$GenDir,
    [string]$Report,
    [switch]$NoBuild,
    [switch]$NoLift,
    [switch]$NoFetch,
    [switch]$Force
)
if ($NoLift) { $NoBuild = $true }
$ErrorActionPreference = 'Stop'
trap { Write-Host "setup: $_" -ForegroundColor Red; exit 1 }
function Fail([string]$msg) { Write-Host "setup: $msg" -ForegroundColor Red; exit 1 }
function Full([string]$p) {   # absolute, no trailing '\' (except a drive root such as D:\, the disc)
    $p = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($p.Replace('/', '\'))
    if ($p.Length -gt 3) { $p.TrimEnd('\') } else { $p }
}
function HasExe([string]$dir) { $dir -and (Test-Path -LiteralPath (Join-Path $dir 'ArmyMen2.exe') -PathType Leaf) }
function Inside([string]$a, [string]$b) { "$($a.TrimEnd('\'))\".StartsWith("$($b.TrimEnd('\'))\", [StringComparison]::OrdinalIgnoreCase) }
function Md5([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm MD5).Hash.ToLower() }

$Root = Split-Path -Parent $PSScriptRoot
$DefaultGen = Join-Path $Root 'src\recomp\gen'
$GameDir = Full $(if ($GameDir) { $GameDir } else { Join-Path $Root 'game' })
$GenDir = Full $(if ($GenDir) { $GenDir } else { $DefaultGen })
$Report = Full $(if ($Report) { $Report } else { Join-Path $Root 'work\table_arms.json' })
$CncDir = Join-Path $Root 'third_party\cnc-ddraw\v7.1.0.0'
$Python = Join-Path $Root '.venv\Scripts\python.exe'
$GogKeys = 'HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games\1913968552', 'HKLM:\SOFTWARE\GOG.com\Games\1913968552'
$SteamApp = '549170'
# The Steam exe being GOG's is inferred from Steam's depot size, not measured.
$Builds = @{
    '59180d8113d1d756e838b0dab1790b6b' = 'retail 1999, English CD'
    '78a6f5344559c2724f48862d22120b2f' = '2017 re-release (GOG; Steam likely the same)'
    '0c760f05a945d7276c67a753a838a295' = 'Russian Edition'
}

# The third-party pieces (not in this repository) and the tools this needs, fetched or checked
# before anything is copied. The pins are the ones in NOTES.md.
$PcrecompUrl = 'https://github.com/sp00nznet/pcrecomp'
$PcrecompCommit = '54cb21f7110776af390bd7260a297ffcaa2a0b01'
$CncZipUrl = 'https://github.com/FunkyFr3sh/cnc-ddraw/releases/download/v7.1.0.0/cnc-ddraw.zip'
$CncZipSha = '0b13ab89a64c9918189b1dadd449ef6ed3cb3b7b19cabd96d8adbd95505bb908'
$CncDllSha = '85e0f7d530dfda134793a57cb3e76b0287dcc96892ee57162dd68f47283b03a9'
$Pcrecomp = Join-Path $Root 'third_party\pcrecomp'
$CncZip = Join-Path $Root 'third_party\cnc-ddraw\cnc-ddraw.zip'

function Sha256([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower() }
function Missing([string]$what, [string]$how) {
    if ($NoFetch) { Fail "missing ${what}: $how" }
}
function Exe([string]$cmd, [string[]]$a) {   # run a tool, stop on a non-zero exit
    & $cmd @a
    if ($LASTEXITCODE -ne 0) { Fail "'$cmd $($a -join ' ')' failed (exit $LASTEXITCODE)" }
}

# pcrecomp, the lifter and the 32-bit runtime, at the pinned commit.
if (-not (Test-Path -LiteralPath (Join-Path $Pcrecomp 'tools') -PathType Container)) {
    Missing 'third_party\pcrecomp' "clone $PcrecompUrl and check out $PcrecompCommit"
    if ((Test-Path -LiteralPath $Pcrecomp) -and (Get-ChildItem -LiteralPath $Pcrecomp -Force | Select-Object -First 1)) {
        Fail "'$Pcrecomp' exists, is not empty and is not pcrecomp: remove it or fix it by hand"
    }
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail 'git is needed to fetch pcrecomp: https://git-scm.com/download/win' }
    Write-Host "setup: fetching pcrecomp $PcrecompCommit from $PcrecompUrl"
    Exe git @('clone', '--quiet', $PcrecompUrl, $Pcrecomp)
    Exe git @('-C', $Pcrecomp, 'checkout', '--quiet', $PcrecompCommit)
}

# cnc-ddraw v7.1.0.0: the zip (checked by its SHA-256) unpacked beside it.
if (-not ((Test-Path -LiteralPath "$CncDir\ddraw.dll" -PathType Leaf) -and (Test-Path -LiteralPath "$CncDir\ddraw.ini" -PathType Leaf))) {
    Missing 'cnc-ddraw' "unpack cnc-ddraw v7.1.0.0 ($CncZip) into $CncDir"
    if (-not (Test-Path -LiteralPath $CncZip -PathType Leaf)) {
        Write-Host "setup: downloading $CncZipUrl"
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $CncZip) | Out-Null
        Invoke-WebRequest -UseBasicParsing -Uri $CncZipUrl -OutFile $CncZip
    }
    if ((Sha256 $CncZip) -ne $CncZipSha) { Fail "$CncZip is not cnc-ddraw v7.1.0.0 (SHA-256 differs from the one in NOTES.md); delete it and run again" }
    Expand-Archive -LiteralPath $CncZip -DestinationPath $CncDir -Force
    if ((Sha256 "$CncDir\ddraw.dll") -ne $CncDllSha) { Fail "$CncDir\ddraw.dll differs from the one in NOTES.md" }
}

# The lifter's Python: a .venv with capstone and pefile (Python 3.11 by preference).
if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) {
    Missing '.venv' 'create it with Python 3.11 and pip install capstone pefile'
    $venv = Join-Path $Root '.venv'
    if ((Test-Path -LiteralPath $venv) -and (Get-ChildItem -LiteralPath $venv -Force | Select-Object -First 1)) { Fail "'$venv' exists but has no Scripts\python.exe: delete it and run again" }
    Write-Host 'setup: creating .venv (capstone, pefile)'
    if (Get-Command py -ErrorAction SilentlyContinue) {
        & py -3.11 -m venv $venv 2>$null
        if ($LASTEXITCODE -ne 0) { & py -3 -m venv $venv }
    } elseif (Get-Command python -ErrorAction SilentlyContinue) {
        & python -m venv $venv
    } else {
        Fail 'Python 3 is needed (3.11 is the tested one): https://www.python.org/downloads/ (tick "Add python.exe to PATH") and run again'
    }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $Python -PathType Leaf)) { Fail 'could not create the .venv' }
    Exe $Python @('-m', 'pip', 'install', '--quiet', '--disable-pip-version-check', 'capstone==5.0.9', 'pefile==2024.8.26')
}

# The C compiler build_host.bat uses: Visual Studio 2022 Build Tools with the C++ x86 tools.
if (-not $NoBuild) {
    $vc = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat'
    if (-not (Test-Path -LiteralPath $vc -PathType Leaf) -and -not $env:VCVARS32) {
        $vsw = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $inst = if (Test-Path -LiteralPath $vsw) { & $vsw -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
        $found = if ($inst) { Join-Path @($inst)[0] 'VC\Auxiliary\Build\vcvars32.bat' }
        if ($found -and (Test-Path -LiteralPath $found -PathType Leaf)) { $env:VCVARS32 = $found }
        else { Fail 'no C++ compiler: install "Build Tools for Visual Studio 2022" with the "Desktop development with C++" workload (https://aka.ms/vs/17/release/vs_BuildTools.exe), then run again' }
    }
}
if (-not $NoBuild -and $GenDir -ne $DefaultGen) { Fail "build_host.bat builds $DefaultGen only: drop -GenDir, or add -NoBuild" }
if ((Test-Path -LiteralPath $GenDir) -and (Get-ChildItem -LiteralPath $GenDir -Force | Select-Object -First 1) -and
    -not (Test-Path -LiteralPath (Join-Path $GenDir 'recomp_funcs.h'))) {
    Fail "'$GenDir' is not empty and is not lifter output (no recomp_funcs.h): refusing to clear it"
}

# GOG's installer and GOG Galaxy record the install folder as 'path' (also 'workingDir';
# 'exe' is the game exe's full path). Read only.
function Get-GogPath([string[]]$Keys) {
    foreach ($k in $Keys) {
        $v = Get-ItemProperty -LiteralPath $k -ErrorAction SilentlyContinue
        if (-not $v) { continue }
        $v.path; $v.workingDir
        if ($v.exe) { [IO.Path]::GetDirectoryName($v.exe) }
    }
}

# Steam's folder: HKCU SteamPath, else HKLM InstallPath. Read only.
function Get-SteamRoot {
    $p = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if (-not $p) { $p = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -ErrorAction SilentlyContinue).InstallPath }
    $p
}

# Every value of "key" in a Valve KeyValues text file (.vdf, .acf); \\ and \" are escapes.
function Get-VdfValues([string]$file, [string]$key) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { return }
    foreach ($m in [regex]::Matches([IO.File]::ReadAllText($file), "`"$key`"\s+`"((?:[^`"\\]|\\.)*)`"", 'IgnoreCase')) {
        $m.Groups[1].Value -replace '\\(.)', '$1'
    }
}

# <library>\steamapps\common\<installdir> for every library holding appmanifest_549170.acf.
function Get-SteamGame([string]$SteamRoot) {
    if (-not $SteamRoot) { return }
    foreach ($lib in @($SteamRoot) + @(Get-VdfValues (Join-Path $SteamRoot 'steamapps\libraryfolders.vdf') 'path')) {
        foreach ($dir in Get-VdfValues (Join-Path $lib "steamapps\appmanifest_$SteamApp.acf") 'installdir') {
            Join-Path $lib "steamapps\common\$dir"
        }
    }
}

# Which store a folder came from, for the messages ('' when it is neither).
function Get-Store([string]$dir) {
    if ($dir -match '\\steamapps\\common\\') { 'Steam' }
    elseif (Test-Path -LiteralPath (Join-Path $dir 'goggame-1913968552.info')) { 'GOG' }
    else { '' }
}

function Find-Game([string]$From) {
    if ($From) {
        $dir = Full $From
        if (-not (Test-Path -LiteralPath $dir)) { Fail "-From '$dir' does not exist" }
        if ([IO.Path]::GetFileName($dir) -eq 'ArmyMen2.exe') { $dir = [IO.Path]::GetDirectoryName($dir) }
        # the retail disc keeps the game in ARMYMEN\, so the disc's root (E:\) is accepted too
        if (-not (HasExe $dir) -and (HasExe (Join-Path $dir 'ARMYMEN'))) { $dir = Full (Join-Path $dir 'ARMYMEN') }
        if (-not (HasExe $dir)) {
            Fail "no ArmyMen2.exe in '$dir': -From must be the folder holding it (the disc, a copy of its files, or your install)"
        }
        return [pscustomobject]@{ Dir = $dir; Store = (Get-Store $dir) }
    }
    $found = @(
        foreach ($d in Get-GogPath $GogKeys) { if (HasExe $d) { [pscustomobject]@{ Dir = (Full $d); Store = 'GOG' }; break } }
        foreach ($d in Get-SteamGame (Get-SteamRoot)) { if (HasExe $d) { [pscustomobject]@{ Dir = (Full $d); Store = 'Steam' }; break } }
    )
    if ($found.Count -gt 1) {
        Fail ("found two installs, " + (($found | ForEach-Object { "$($_.Store) in '$($_.Dir)'" }) -join ' and ') +
              ": pick one with -From `"<folder>`"")
    }
    if ($found.Count -eq 1) { Write-Host "found the $($found[0].Store) install"; return $found[0] }
    Fail ("no game folder: no -From was given, and no GOG (id 1913968552) or Steam (app $SteamApp) install of Army Men II was found.`n" +
          "  run: pwsh scripts\setup.ps1 -From `"<the folder holding ArmyMen2.exe>`"")
}

# PE section names, read from the header ($null when the file is not a PE).
function Get-PeSections([string]$exe) {
    $b = [IO.File]::ReadAllBytes($exe)
    if ($b.Length -lt 0x40 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { return }
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    if ($pe -lt 0 -or $pe + 24 -gt $b.Length -or [BitConverter]::ToUInt32($b, $pe) -ne 0x4550) { return }
    $table = $pe + 24 + [BitConverter]::ToUInt16($b, $pe + 20)
    $count = [BitConverter]::ToUInt16($b, $pe + 6)
    for ($i = 0; $i -lt $count -and $table + 40 * $i + 8 -le $b.Length; $i++) {
        [Text.Encoding]::ASCII.GetString($b, $table + 40 * $i, 8).TrimEnd([char]0)
    }
}

# 1. Find the game; 2. name the build.
$Game = Find-Game $From
$Src = $Game.Dir
$Exe = Join-Path $Src 'ArmyMen2.exe'
$Md5 = Md5 $Exe
Write-Host ("game:  $Src" + $(if ($Game.Store) { " ($($Game.Store))" } else { '' }))
$sections = @(Get-PeSections $Exe)
if (-not $sections) { Fail "$Exe is not a Windows (PE) executable" }
if ($sections -contains '.bind') {
    Fail ("$Exe is wrapped in Steam's DRM (SteamStub: a PE section named .bind).`n" +
          "  This project does not remove copy protection, so it cannot lift this exe.")
}
if ($Builds.ContainsKey($Md5)) { Write-Host "build: $($Builds[$Md5]) (ArmyMen2.exe md5 $Md5)" }
elseif ($Game.Store -eq 'Steam') { Write-Host "warning: Steam build not seen before (ArmyMen2.exe md5 $Md5); continuing" -ForegroundColor Yellow }
else { Write-Host "warning: unknown build (ArmyMen2.exe md5 $Md5); continuing" -ForegroundColor Yellow }
switch ($Md5) {
    '59180d8113d1d756e838b0dab1790b6b' { Write-Host "       this exe has the CD check: past the title menu, SINGLE PLAYER and BOOT CAMP want the disc (label ARMYMEN2) in a drive" }
    '78a6f5344559c2724f48862d22120b2f' { Write-Host "       the publisher disabled the CD check: no disc needed" }
    '0c760f05a945d7276c67a753a838a295' { Write-Host "       retail code: expect the CD check past the title menu" }
}

# 3. The working copy.
if ((Inside $GameDir $Src) -or (Inside $Src $GameDir)) { Fail "-GameDir '$GameDir' overlaps the game folder '$Src': the working copy must be a separate folder" }
$overwrite = $true
if (Test-Path -LiteralPath $GameDir) {
    $old = Join-Path $GameDir 'ArmyMen2.exe'
    if (Test-Path -LiteralPath $old -PathType Leaf) {
        $oldMd5 = Md5 $old
        if ($oldMd5 -eq $Md5) { $overwrite = [bool]$Force }
        elseif (-not $Force) { Fail "'$GameDir' already holds a different ArmyMen2.exe (md5 $oldMd5): pass -Force to overwrite it, or pick another -GameDir" }
    } elseif ((Get-ChildItem -LiteralPath $GameDir -Force | Select-Object -First 1) -and -not $Force) {
        Fail "'$GameDir' is not empty and holds no ArmyMen2.exe: pass -Force to copy into it anyway, or pick another -GameDir"
    }
}
$copied = 0; $kept = 0
[void][IO.Directory]::CreateDirectory($GameDir)
foreach ($item in Get-ChildItem -LiteralPath $Src -Recurse -Force) {
    $dst = Join-Path $GameDir $item.FullName.Substring($Src.Length).TrimStart('\')
    if ($item.PSIsContainer) { [void][IO.Directory]::CreateDirectory($dst); continue }
    if ([IO.File]::Exists($dst)) {
        if (-not $overwrite) { $kept++; continue }
        [IO.File]::SetAttributes($dst, 'Normal')
    }
    [IO.File]::Copy($item.FullName, $dst, $true)
    # Files copied off a CD are read-only, and the game rewrites Options.cfg and its saves.
    [IO.File]::SetAttributes($dst, [IO.File]::GetAttributes($dst) -band -bnot [IO.FileAttributes]::ReadOnly)
    $copied++
}
Write-Host ("copy:  $copied file(s) to $GameDir" + $(if ($kept) { ", $kept already there kept (Options.cfg and saves stay)" } else { '' }))

# 4. cnc-ddraw.
$own = Join-Path $Src 'ddraw.dll'
if ((Test-Path -LiteralPath $own) -and (Md5 $own) -ne (Md5 "$CncDir\ddraw.dll")) {
    $whose = if ($Game.Store) { "the $($Game.Store) release's DirectDraw wrapper" } else { 'a DirectDraw wrapper' }
    Write-Host "       the game folder ships its own ddraw.dll ($whose, $((Get-Item -LiteralPath $own).Length) B): replaced by cnc-ddraw in the working copy only"
}
foreach ($f in 'ddraw.dll', 'ddraw.ini') {
    $dst = Join-Path $GameDir $f
    if ([IO.File]::Exists($dst)) { [IO.File]::SetAttributes($dst, 'Normal') }
}
[IO.File]::Copy("$CncDir\ddraw.dll", (Join-Path $GameDir 'ddraw.dll'), $true)
$section = ''; $hits = 0
$ini = foreach ($line in [IO.File]::ReadAllLines("$CncDir\ddraw.ini")) {
    if ($line -match '^\s*\[([^\]]+)\]') { $section = $Matches[1] }
    elseif ($section -eq 'ddraw' -and $line -match '^\s*(windowed|savesettings)\s*=') {
        $line = @{ windowed = 'windowed=true'; savesettings = 'savesettings=0' }[$Matches[1].ToLower()]
        $hits++
    }
    $line
}
if ($hits -ne 2) { Fail "unexpected $CncDir\ddraw.ini: windowed= and savesettings= are not each under [ddraw] once" }
$ini += '', '; plasticrecomp: the recompiled Army Men 2 host (same settings as [ArmyMen2] above)',
        '[plasticrecomp]', 'maxfps=60', 'maxgameticks=120', 'minfps=-1'
[IO.File]::WriteAllLines((Join-Path $GameDir 'ddraw.ini'), [string[]]$ini)
Write-Host "ddraw: cnc-ddraw v7.1.0.0 (windowed=true, savesettings=0, [plasticrecomp] profile)"
if ($NoLift) { Write-Host "`nstopped before the lift (-NoLift): run setup again without it to lift and build"; exit 0 }

# 5. Lift the copy's exe (the same bytes as the source's).
if (Test-Path -LiteralPath $GenDir) { Get-ChildItem -LiteralPath $GenDir -Force | Remove-Item -Recurse -Force }
[void][IO.Directory]::CreateDirectory($GenDir)
[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Report))
Write-Host "lift:  $GenDir"
& $Python (Join-Path $Root 'scripts\run_lift.py') (Join-Path $GameDir 'ArmyMen2.exe') $GenDir --report $Report
if ($LASTEXITCODE) { Fail "run_lift.py failed (exit $LASTEXITCODE)" }
$target = Join-Path $GenDir 'recomp_target.h'
if (-not ((Test-Path -LiteralPath $target) -and (Select-String -LiteralPath $target -SimpleMatch "`"$Md5`"" -Quiet))) {
    Fail "$target is missing or does not name md5 $Md5"
}

# 6. Build.
if (-not $NoBuild) {
    Push-Location -LiteralPath $Root
    try { $out = & cmd /c 'scripts\build_host.bat'; $code = $LASTEXITCODE } finally { Pop-Location }
    $out | Where-Object { $_ -match '^EXIT=' } | ForEach-Object { Write-Host "build: $_" }
    if ($code) { Fail "build failed: see $(Join-Path $Root 'build\build.log')" }
}

# 7. How to run.
Write-Host ""
if ($NoBuild) { Write-Host "not built (-NoBuild): build with scripts\build_host.bat from $Root, then run:" } else { Write-Host "run it:" }
Write-Host "  cd `"$Root`""
Write-Host "  build\plasticrecomp.exe --game `"$GameDir`""
