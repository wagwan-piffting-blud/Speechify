@echo off
REM ============================================================
REM  spfy/src/host_emu/build.bat -- standalone 64-bit native build
REM  of the x86 emulator + Phase-1 smoke-test harness.
REM
REM  Build is intentionally 64-bit: the emulator IS the 32-bit
REM  layer; the host can be 64-bit. This is what unlocks Android
REM  arm64 / Apple Silicon / amd64 Linux later.
REM
REM  Run from PowerShell. Toolchain: MSYS2 mingw-w64 (matches the
REM  parent spfy build.bat -- see that file for MSYS_ROOT notes).
REM
REM  Usage:
REM    build.bat                 configure + build
REM    build.bat clean           remove build dir
REM    build.bat rebuild         clean + configure + build
REM    build.bat run             configure + build + run smoke test
REM ============================================================

setlocal EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

if "%MSYS_ROOT%"=="" set "MSYS_ROOT=C:\msys64"
set "CMAKE=%MSYS_ROOT%\mingw64\bin\cmake.exe"
set "NINJA=%MSYS_ROOT%\mingw64\bin\ninja.exe"
set "GCC=%MSYS_ROOT%\mingw64\bin\gcc.exe"

if not exist "%CMAKE%" ( echo error: cmake not found at "%CMAKE%" & exit /b 1 )
if not exist "%NINJA%" ( echo error: ninja not found at "%NINJA%" & exit /b 1 )
if not exist "%GCC%"   ( echo error: gcc not found at "%GCC%"     & exit /b 1 )

if "%SPFY_EMU_BUILD_DIR%"=="" set "SPFY_EMU_BUILD_DIR=C:\tmp\spfy_host_emu_build"
set "BUILD_DIR=%SPFY_EMU_BUILD_DIR%"

set "DLL_PATH=Speechify/bin/SWIttsFe-en-US.dll"

set "ACTION=%~1"
if "%ACTION%"=="" set "ACTION=all"

set "PATH=%MSYS_ROOT%\mingw64\bin;%PATH%"

pushd "%SCRIPT_DIR%" >nul

if /I "%ACTION%"=="clean"   goto :clean
if /I "%ACTION%"=="rebuild" goto :rebuild
if /I "%ACTION%"=="run"     goto :run
if /I "%ACTION%"=="all"     goto :all
echo unknown action: %ACTION%
echo usage: build.bat [clean^|rebuild^|run]
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
call :do_configure
if errorlevel 1 ( popd >nul & exit /b 1 )
call :do_build
if errorlevel 1 ( popd >nul & exit /b 1 )
echo [done] build OK. Test exe: "%BUILD_DIR%\test_emu_boot.exe"
popd >nul
exit /b 0

:run
call :do_configure
if errorlevel 1 ( popd >nul & exit /b 1 )
call :do_build
if errorlevel 1 ( popd >nul & exit /b 1 )
echo.
echo [run] EMU_IATDUMP=1 "%BUILD_DIR%\test_emu_boot.exe" "%DLL_PATH%"
set "EMU_IATDUMP=1"
set "EMU_VERBOSE=1"
"%BUILD_DIR%\test_emu_boot.exe" "%DLL_PATH%"
set "RC=%ERRORLEVEL%"
popd >nul
exit /b %RC%

:do_configure
echo [configure] cmake -^> "%BUILD_DIR%"
"%CMAKE%" -S "%SCRIPT_DIR%" -B "%BUILD_DIR%" ^
    -G Ninja ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
    -DCMAKE_C_COMPILER="%GCC%" ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DSPFY_HOST_EMU_BUILD_TESTS=ON
exit /b %ERRORLEVEL%

:do_build
echo [build] ninja
"%CMAKE%" --build "%BUILD_DIR%"
exit /b %ERRORLEVEL%
