@echo off
REM ==================================================================
REM Emscripten build driver for the in-browser spfy demo (Windows).
REM
REM Handles three Windows-specific quirks:
REM
REM   1. Auto-activates emsdk if EMSDK is not already in the environment
REM      (checks %EMSDK%, %USERPROFILE%\emsdk, C:\emsdk,
REM      %USERPROFILE%\Documents\emsdk).
REM
REM   2. Locates a **native Windows** CMake (not the MSYS2 / Cygwin one
REM      that Git Bash injects). MSYS cmake can't execute emcc.bat
REM      because it expects POSIX semantics, so we explicitly prepend
REM      a native cmake.exe directory to PATH.
REM
REM   3. Locates a native Ninja the same way, and falls back to
REM      "MinGW Makefiles" (mingw32-make) if no Ninja is found.
REM
REM Outputs (in dist/):
REM   spfy_wasm.js, spfy_wasm.wasm, spfy_wasm.data
REM
REM Usage:
REM   build.bat           Configure + build (Release by default).
REM   build.bat clean     Remove build\ and dist\ and exit.
REM   set BUILD_TYPE=Debug && build.bat   For a debug build.
REM ==================================================================

setlocal enableextensions enabledelayedexpansion

pushd "%~dp0"

REM ---- clean mode --------------------------------------------------
if /i "%~1"=="clean" (
    if exist build rmdir /s /q build
    if exist dist  rmdir /s /q dist
    echo cleaned build\ and dist\
    popd
    exit /b 0
)

REM ---- locate native Windows cmake --------------------------------
REM Check the common install locations explicitly; even if `cmake` is
REM already on PATH, it might be MSYS2's, which the Emscripten toolchain
REM CMake module can't drive (it tries to spawn "emcc -v" without the
REM .bat extension and fails with "no such file or directory").
set "WIN_CMAKE_DIR="
if exist "%ProgramFiles%\CMake\bin\cmake.exe" (
    set "WIN_CMAKE_DIR=%ProgramFiles%\CMake\bin"
)
if not defined WIN_CMAKE_DIR if exist "%ProgramFiles(x86)%\CMake\bin\cmake.exe" (
    set "WIN_CMAKE_DIR=%ProgramFiles(x86)%\CMake\bin"
)
REM Visual Studio bundled CMake (Build Tools / Community / Pro / Ent,
REM 2019 + 2022). The version subdir is enumerated.
if not defined WIN_CMAKE_DIR (
    for %%E in (Community Professional Enterprise BuildTools) do (
        for %%Y in (2022 2019) do (
            if not defined WIN_CMAKE_DIR (
                set "VS_CMK=%ProgramFiles%\Microsoft Visual Studio\%%Y\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
                if exist "!VS_CMK!\cmake.exe" set "WIN_CMAKE_DIR=!VS_CMK!"
            )
            if not defined WIN_CMAKE_DIR (
                set "VS_CMK=%ProgramFiles(x86)%\Microsoft Visual Studio\%%Y\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
                if exist "!VS_CMK!\cmake.exe" set "WIN_CMAKE_DIR=!VS_CMK!"
            )
        )
    )
)

if not defined WIN_CMAKE_DIR (
    echo error: native Windows CMake not found.
    echo.
    echo The cmake already on PATH may be MSYS2 / Cygwin / Git Bash's,
    echo which can't drive the Emscripten toolchain ^(it tries to run
    echo emcc without the .bat extension and fails^).
    echo.
    echo Install one of:
    echo   - https://cmake.org/download/  ^(MSI installer, tick "Add to PATH"^)
    echo   - choco install cmake --installargs 'ADD_CMAKE_TO_PATH=System'
    echo   - winget install Kitware.CMake
    echo   - Visual Studio 2022 with "C++ CMake tools" workload
    echo.
    popd
    exit /b 1
)

REM Prepend the native cmake dir so emcmake / our subsequent `cmake`
REM invocations pick it ahead of any MSYS2 cmake on PATH.
set "PATH=%WIN_CMAKE_DIR%;%PATH%"
echo ==^> using native CMake: %WIN_CMAKE_DIR%

REM ---- locate native Ninja (optional, prefer if present) -----------
set "WIN_NINJA_DIR="
if exist "%WIN_CMAKE_DIR%\ninja.exe" set "WIN_NINJA_DIR=%WIN_CMAKE_DIR%"
if not defined WIN_NINJA_DIR if exist "%ProgramFiles%\CMake\bin\ninja.exe" (
    set "WIN_NINJA_DIR=%ProgramFiles%\CMake\bin"
)
if not defined WIN_NINJA_DIR (
    for %%E in (Community Professional Enterprise BuildTools) do (
        for %%Y in (2022 2019) do (
            if not defined WIN_NINJA_DIR (
                set "VS_NJ=%ProgramFiles%\Microsoft Visual Studio\%%Y\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
                if exist "!VS_NJ!\ninja.exe" set "WIN_NINJA_DIR=!VS_NJ!"
            )
            if not defined WIN_NINJA_DIR (
                set "VS_NJ=%ProgramFiles(x86)%\Microsoft Visual Studio\%%Y\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
                if exist "!VS_NJ!\ninja.exe" set "WIN_NINJA_DIR=!VS_NJ!"
            )
        )
    )
)

if defined WIN_NINJA_DIR (
    set "PATH=%WIN_NINJA_DIR%;%PATH%"
    set "GENERATOR=Ninja"
    echo ==^> using Ninja:       %WIN_NINJA_DIR%
) else (
    set "GENERATOR=MinGW Makefiles"
    echo ==^> Ninja not found, falling back to MinGW Makefiles
)

REM ---- locate + activate emsdk ------------------------------------
where emcmake >nul 2>&1
if errorlevel 1 (
    set "EMSDK_GUESS="
    if defined EMSDK if exist "%EMSDK%\emsdk_env.bat"                   set "EMSDK_GUESS=%EMSDK%"
    if not defined EMSDK_GUESS if exist "%USERPROFILE%\emsdk\emsdk_env.bat"           set "EMSDK_GUESS=%USERPROFILE%\emsdk"
    if not defined EMSDK_GUESS if exist "%USERPROFILE%\Documents\emsdk\emsdk_env.bat" set "EMSDK_GUESS=%USERPROFILE%\Documents\emsdk"
    if not defined EMSDK_GUESS if exist "C:\emsdk\emsdk_env.bat"                     set "EMSDK_GUESS=C:\emsdk"
    if not defined EMSDK_GUESS (
        echo error: emcmake not in PATH and emsdk_env.bat not found.
        echo   - install:  git clone https://github.com/emscripten-core/emsdk.git
        echo   - activate: emsdk install latest ^&^& emsdk activate latest
        echo   - or set EMSDK=^<path^> and re-run.
        popd
        exit /b 1
    )
    echo ==^> activating emsdk:  !EMSDK_GUESS!
    call "!EMSDK_GUESS!\emsdk_env.bat" >nul
    where emcmake >nul 2>&1
    if errorlevel 1 (
        echo error: activated emsdk but emcmake still not on PATH.
        popd
        exit /b 1
    )
)

if not defined BUILD_TYPE set "BUILD_TYPE=Release"

if not exist build mkdir build
if not exist dist  mkdir dist

echo.
echo ==^> emcmake configure ^(%BUILD_TYPE%, %GENERATOR%^)
call emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -G "%GENERATOR%"
if errorlevel 1 (
    echo configure failed.
    popd
    exit /b 1
)

echo.
echo ==^> emmake build
call emmake cmake --build build -j
if errorlevel 1 (
    echo build failed.
    popd
    exit /b 1
)

echo.
echo Built artifacts:
for %%f in (dist\spfy_wasm.js dist\spfy_wasm.wasm dist\spfy_wasm.data) do (
    if exist "%%f" (
        for %%s in ("%%f") do echo   %%~zs bytes  %%f
    )
)
echo.
echo Run the demo:
echo   npm install     ^(one-time^)
echo   npm run dev     ^(http://localhost:6660^)

popd
endlocal & exit /b 0
