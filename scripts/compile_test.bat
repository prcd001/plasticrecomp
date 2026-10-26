@echo off
rem Compile-only check of the lifted C with MSVC x86. Run from the project root.
rem Output goes to build\ (gitignored); the log is build\compile.log.
rem The default pipeline's recomp_*.c files do not include recomp_funcs.h, so calls across
rem files would be implicit int declarations: /FI puts the prototypes in every file.
set VSLANG=1033
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
if not exist build mkdir build
cl /nologo /c /Od /bigobj /MP /W0 /FIrecomp_funcs.h ^
   /I third_party\pcrecomp\runtime\recomp32 /I src\recomp\gen ^
   /Fobuild\ src\recomp\gen\recomp_*.c > build\compile.log 2>&1
echo EXIT=%ERRORLEVEL%
