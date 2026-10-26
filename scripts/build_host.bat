@echo off
rem Build build\plasticrecomp.exe from the lifted C (src\recomp\gen), pcrecomp's native32
rem runtime and src\host. Run from the project root; the log is build\build.log.
rem GEN_OPT picks the optimisation for the lifted C (default /O1; /Od builds in seconds).
setlocal
set VSLANG=1033
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
if not exist build\obj mkdir build\obj
if "%GEN_OPT%"=="" set GEN_OPT=/O1
set RT=third_party\pcrecomp\runtime
set INC=/I %RT%\recomp32 /I %RT%\native32 /I src\recomp\gen
set DEFS=/D_CRT_SECURE_NO_WARNINGS

rem The default pipeline's recomp_*.c files do not include recomp_funcs.h: /FI puts the
rem prototypes in every file, or calls across files become implicit int declarations.
echo [lifted C %GEN_OPT%] > build\build.log
cl /nologo /c %GEN_OPT% /bigobj /MP /w /MT %DEFS% /FIrecomp_funcs.h %INC% /Fobuild\obj\ ^
   src\recomp\gen\recomp_*.c >> build\build.log 2>&1 || goto fail
echo [runtime + host] >> build\build.log
cl /nologo /c /O2 /W3 /MT %DEFS% %INC% /Fobuild\obj\ ^
   %RT%\native32\native32.c %RT%\recomp32\image_loader.c %RT%\recomp32\recomp_trace.c ^
   src\host\main.c src\host\mouse.c >> build\build.log 2>&1 || goto fail
rem /BASE and a small /STACK keep 0x00400000 free for the guest image (src\host\main.c).
echo [link] >> build\build.log
link /nologo /OUT:build\plasticrecomp.exe /SUBSYSTEM:CONSOLE /BASE:0x60000000 /FIXED:NO ^
     /DYNAMICBASE:NO /NXCOMPAT /LARGEADDRESSAWARE /STACK:0x40000 ^
     build\obj\*.obj user32.lib gdi32.lib winmm.lib ole32.lib advapi32.lib dxguid.lib >> build\build.log 2>&1 || goto fail
echo EXIT=0
exit /b 0
:fail
echo EXIT=1 (see build\build.log)
exit /b 1
