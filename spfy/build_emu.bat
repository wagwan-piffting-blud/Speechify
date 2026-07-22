@echo off
REM ============================================================
REM  spfy build_emu.bat -- Windows x64 build of the engine-faithful spfy
REM  (the real SWIttsFe-<lang>.dll driven through the src/host_emu x86
REM  interpreter).
REM
REM  Same host toolchain as build.bat (MSYS2 mingw-w64 64-bit gcc), but
REM  sets SPFY_FE_HOSTED=ON so the real DLL front-end is used instead of
REM  the in-house pure-C one. The emulator is the only FE backend now
REM  (the native src/host/ PE loader was retired 2026-07-22), so there is
REM  no longer an SPFY_FE_EMU switch to pass.
REM
REM  Output lives in a separate build dir so it doesn't clash with
REM  build.bat's C:\tmp\spfy_build.
REM
REM  Usage:
REM    build_emu.bat              configure + build
REM    build_emu.bat clean        remove build dir
REM    build_emu.bat rebuild      clean then configure+build
REM ============================================================

setlocal EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

if "%MSYS_ROOT%"=="" set "MSYS_ROOT=C:\msys64"
set "CMAKE=%MSYS_ROOT%\mingw64\bin\cmake.exe"
set "NINJA=%MSYS_ROOT%\mingw64\bin\ninja.exe"
set "GCC=%MSYS_ROOT%\mingw64\bin\gcc.exe"
set "GXX=%MSYS_ROOT%\mingw64\bin\g++.exe"

if not exist "%CMAKE%" ( echo error: cmake not found at "%CMAKE%" & exit /b 1 )
if not exist "%NINJA%" ( echo error: ninja not found at "%NINJA%" & exit /b 1 )
if not exist "%GCC%"   ( echo error: gcc not found at "%GCC%"     & exit /b 1 )

REM Match the CI / Linux configuration (Release + strict x87 FP) so local
REM Windows builds are byte-for-byte comparable with them.
REM Override for a debugging session:  set SPFY_BUILD_TYPE=Debug
if "%SPFY_BUILD_TYPE%"=="" set "SPFY_BUILD_TYPE=Release"
if "%SPFY_STRICT_FP%"=="" set "SPFY_STRICT_FP=ON"

if "%SPFY_EMU_FE_BUILD_DIR%"=="" set "SPFY_EMU_FE_BUILD_DIR=C:\tmp\spfy_build_emu"
set "BUILD_DIR=%SPFY_EMU_FE_BUILD_DIR%"
set "ACTION=%~1"
if "%ACTION%"=="" set "ACTION=all"

set "PATH=%MSYS_ROOT%\mingw64\bin;%PATH%"

pushd "%SCRIPT_DIR%" >nul

if /I "%ACTION%"=="clean"   goto :clean
if /I "%ACTION%"=="rebuild" goto :rebuild
if /I "%ACTION%"=="all"     goto :all
echo unknown action: %ACTION%
echo usage: build_emu.bat [clean^|rebuild]
popd >nul
exit /b 2

:clean
echo [clean] removing "%BUILD_DIR%"
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
popd >nul
exit /b 0

:rebuild
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
goto :all

:all
REM  No -DCMAKE_CXX_COMPILER here: spfy is project(... LANGUAGES C), so
REM  passing one only earns a configure-time "Manually-specified variables
REM  were not used by the project" warning. %GXX% stays defined above for
REM  anyone extending this script.
echo [configure] cmake -^> "%BUILD_DIR%"
"%CMAKE%" -S "%SCRIPT_DIR%" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
    -DCMAKE_C_COMPILER="%GCC%" ^
    -DCMAKE_BUILD_TYPE=%SPFY_BUILD_TYPE% ^
    -DSPFY_STRICT_FP=%SPFY_STRICT_FP% ^
    -DSPFY_BUILD_TESTS=ON ^
    -DSPFY_FE_HOSTED=ON
if errorlevel 1 ( popd >nul & exit /b 1 )

echo [build] ninja
"%CMAKE%" --build "%BUILD_DIR%"
if errorlevel 1 ( popd >nul & exit /b 1 )

echo [done] emulator-backed spfy built into "%BUILD_DIR%"
popd >nul
exit /b 0
