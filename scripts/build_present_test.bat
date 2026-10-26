@echo off
rem Build build\present_test\present_test.exe: the D3D11 presenter (src\host\present.c) and its
rem standalone test (scripts\present_test.c). Run from the project root; the log is
rem build\present_test\build.log. Run: build\present_test\present_test.exe 24 --script
setlocal
set VSLANG=1033
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
if not exist build\present_test mkdir build\present_test
cl /nologo /O2 /W3 /MT /D_CRT_SECURE_NO_WARNINGS /Fobuild\present_test\ /Febuild\present_test\present_test.exe ^
   src\host\present.c scripts\present_test.c ^
   /link /SUBSYSTEM:CONSOLE d3d11.lib dxgi.lib d3dcompiler.lib user32.lib winmm.lib ^
   > build\present_test\build.log 2>&1 || goto fail
echo EXIT=0
exit /b 0
:fail
type build\present_test\build.log
echo EXIT=1
exit /b 1
