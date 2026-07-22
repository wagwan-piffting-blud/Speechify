@echo off
REM ============================================================
REM  spfy build.bat -- Windows dev build helper.
REM
REM  IMPORTANT: this script configures SPFY_FE_HOSTED=OFF -- the
REM  in-house pure-C front-end, which is NOT engine-faithful (~91%
REM  audit). Its output therefore does NOT match the Speechify oracle.
REM
REM  That is a FRONT-END choice, not a toolchain limitation. The mingw-w64
REM  builds from build32.bat (x86, native PE loader) and build_emu.bat
REM  (x64, emulator-backed FE) ARE byte-exact with the oracle -- verified
REM  2026-07-22: Windows x86, Windows x64, Linux x86_64 and Linux arm64
REM  all emit an identical WAV for the same text.
REM
REM  Use this script only for fast iteration on loaders, parsers, and
REM  unit tests (it stays on Debug/-O0 for that reason). For
REM  engine-faithful output use instead:
REM    build32.bat        x86, native PE loader
REM    build_emu.bat      x64, emulator-backed FE
REM    build_hosted.bat   x64 viz tracer -> bin\spfy_synth_trace.exe
REM
REM  Usage:
REM    build.bat              configure + build
REM    build.bat clean        remove build dir
REM    build.bat rebuild      clean then configure+build
REM    build.bat configure    configure only
REM    build.bat test         configure + build + ctest (may be blocked by AV)
REM
REM  Why no tests by default: Windows Defender heuristic-flags small mingw
REM  test exes containing crypto-like byte patterns (the 0xCE constant
REM  used by the SWIttsRiffEncryption module). ctest reports them as
REM  BAD_COMMAND because Defender silently kills the launch. The
REM  canonical 1:1 test target is Linux x86_64; on this Windows host,
REM  use 'build.bat test' if you have a Defender exclusion configured,
REM  otherwise rely on the Linux build for test verification.
REM
REM  Override the toolchain location:
REM    set MSYS_ROOT=D:\msys2 && build.bat
REM ============================================================

setlocal EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

if "%MSYS_ROOT%"=="" set "MSYS_ROOT=C:\msys64"
REM Use the MINGW64 cmake, NOT %MSYS_ROOT%\usr\bin\cmake.exe -- the latter is
REM a cygwin-style binary that mis-parses Windows paths with drive letters.
set "CMAKE=%MSYS_ROOT%\mingw64\bin\cmake.exe"
set "CTEST=%MSYS_ROOT%\mingw64\bin\ctest.exe"
set "NINJA=%MSYS_ROOT%\mingw64\bin\ninja.exe"
set "GCC=%MSYS_ROOT%\mingw64\bin\gcc.exe"
set "GXX=%MSYS_ROOT%\mingw64\bin\g++.exe"

if not exist "%CMAKE%" ( echo error: cmake not found at "%CMAKE%" & exit /b 1 )
if not exist "%NINJA%" ( echo error: ninja not found at "%NINJA%" & exit /b 1 )
if not exist "%GCC%"   ( echo error: gcc not found at "%GCC%"     & exit /b 1 )

REM Build outside the user profile by default. Windows Defender applies more
REM aggressive heuristics to small mingw exes under %USERPROFILE%\Documents,
REM blocking ctest from launching them. C:\tmp avoids the false positives.
REM Override with: set SPFY_BUILD_DIR=...
if "%SPFY_BUILD_DIR%"=="" set "SPFY_BUILD_DIR=C:\tmp\spfy_build"
set "BUILD_DIR=%SPFY_BUILD_DIR%"
set "ACTION=%~1"
if "%ACTION%"=="" set "ACTION=all"

set "PATH=%MSYS_ROOT%\mingw64\bin;%PATH%"

pushd "%SCRIPT_DIR%" >nul

if /I "%ACTION%"=="clean"     goto :clean
if /I "%ACTION%"=="rebuild"   goto :rebuild
if /I "%ACTION%"=="configure" goto :configure
if /I "%ACTION%"=="test"      goto :test
if /I "%ACTION%"=="all"       goto :all
echo unknown action: %ACTION%
echo usage: build.bat [clean^|rebuild^|configure^|test]
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

:configure
call :do_configure
set "RC=%ERRORLEVEL%"
popd >nul
exit /b %RC%

:test
call :do_configure
if errorlevel 1 ( popd >nul & exit /b 1 )
call :do_build
if errorlevel 1 ( popd >nul & exit /b 1 )
call :do_test
set "RC=%ERRORLEVEL%"
popd >nul
exit /b %RC%

:all
call :do_configure
if errorlevel 1 ( popd >nul & exit /b 1 )
call :do_build
if errorlevel 1 ( popd >nul & exit /b 1 )
echo [done] build OK. tests skipped on Windows; run 'build.bat test' to attempt.
popd >nul
exit /b 0

:do_configure
echo [configure] cmake -^> "%BUILD_DIR%"
REM Project is LANGUAGES C only — do NOT pass -DCMAKE_CXX_COMPILER (it would
REM emit a "Manually-specified variables were not used" CMake warning).
"%CMAKE%" -S "%SCRIPT_DIR%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_C_COMPILER="%GCC%" -DCMAKE_BUILD_TYPE=Debug -DSPFY_STRICT_FP=OFF -DSPFY_BUILD_TESTS=ON -DSPFY_FE_HOSTED=OFF
exit /b %ERRORLEVEL%

:do_build
echo [build] ninja
"%CMAKE%" --build "%BUILD_DIR%"
exit /b %ERRORLEVEL%

:do_test
echo [test] ctest
"%CTEST%" --test-dir "%BUILD_DIR%" --output-on-failure
exit /b %ERRORLEVEL%
