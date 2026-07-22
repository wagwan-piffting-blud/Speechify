@echo off
REM ============================================================
REM  build_hosted.bat -- build the VIZ's engine-exact trace binary.
REM
REM  Same as build.bat but with SPFY_FE_HOSTED=ON, so the front-end
REM  is the real SWIttsFe-en-US.dll (driven through the host_emu
REM  software x86 CPU on this 64-bit host -- "EMULATED DLL", 100%%
REM  engine UID match). Produces spfy_synth_trace.exe whose
REM  phonemization matches Speechify exactly (e.g. Monday -> ...d ey,
REM  not the in-house FE's CMUdict-primary ...d iy), then copies it
REM  into bin/ for the viz (viz/app.py::_trace_exe finds it there).
REM
REM  Use build.bat (SPFY_FE_HOSTED=OFF) for the in-house / ARM / WASM
REM  binaries; use THIS for the viz's engine-faithful tracer.
REM ============================================================
setlocal
if "%MSYS_ROOT%"=="" set "MSYS_ROOT=C:\msys64"
set "CMAKE=%MSYS_ROOT%\mingw64\bin\cmake.exe"
set "NINJA=%MSYS_ROOT%\mingw64\bin\ninja.exe"
set "GCC=%MSYS_ROOT%\mingw64\bin\gcc.exe"
set "PATH=%MSYS_ROOT%\mingw64\bin;%PATH%"

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "BUILD=C:\tmp\spfy_build_hosted"
set "BIN=%SCRIPT_DIR%\..\bin"

REM Match the CI / Linux configuration (Release + strict x87 FP) so this
REM tracer is byte-for-byte comparable with them -- and faster for the viz.
REM Override for a debugging session:  set SPFY_BUILD_TYPE=Debug
if "%SPFY_BUILD_TYPE%"=="" set "SPFY_BUILD_TYPE=Release"
if "%SPFY_STRICT_FP%"=="" set "SPFY_STRICT_FP=ON"

if not exist "%CMAKE%" ( echo error: cmake not found at "%CMAKE%" & exit /b 1 )

echo [configure] SPFY_FE_HOSTED=ON -^> "%BUILD%"
"%CMAKE%" -S "%SCRIPT_DIR%" -B "%BUILD%" -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_C_COMPILER="%GCC%" ^
  -DCMAKE_BUILD_TYPE=%SPFY_BUILD_TYPE% -DSPFY_STRICT_FP=%SPFY_STRICT_FP% -DSPFY_BUILD_TESTS=OFF ^
  -DSPFY_FE_HOSTED=ON
if errorlevel 1 ( echo CONFIGURE FAILED & exit /b 1 )

echo [build] spfy_synth_trace
"%CMAKE%" --build "%BUILD%" --target spfy_synth_trace
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )

echo [install] -^> "%BIN%\spfy_synth_trace.exe"
copy /Y "%BUILD%\src\cli\spfy_synth_trace.exe" "%BIN%\spfy_synth_trace.exe" >nul
if errorlevel 1 ( echo COPY FAILED & exit /b 1 )
echo HOSTED BUILD OK -- viz now uses the engine-exact FE.
