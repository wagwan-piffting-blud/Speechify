@echo off
setlocal
set "MSYS_ROOT=C:\msys64"
set "PATH=%MSYS_ROOT%\mingw32\bin;%PATH%"
set "CMAKE=%MSYS_ROOT%\mingw64\bin\cmake.exe"
set "NINJA=%MSYS_ROOT%\mingw64\bin\ninja.exe"
set "GCC=%MSYS_ROOT%\mingw32\bin\gcc.exe"
set "GXX=%MSYS_ROOT%\mingw32\bin\g++.exe"
set "BUILD_DIR=C:\tmp\spfy_build32"
set "SRC_DIR=%USERPROFILE%\Documents\Speechify\spfy"

REM Match the CI / Linux configuration (Release + strict x87 FP) so local
REM Windows builds are byte-for-byte comparable. This matters on i686:
REM -O3 changes when x87 80-bit intermediates spill to memory, and
REM SPFY_STRICT_FP's -ffloat-store is what pins that down.
REM Override for a debugging session:  set SPFY_BUILD_TYPE=Debug
if "%SPFY_BUILD_TYPE%"=="" set "SPFY_BUILD_TYPE=Release"
if "%SPFY_STRICT_FP%"=="" set "SPFY_STRICT_FP=ON"

if "%~1"=="configure" goto :configure
if "%~1"=="sapi64"    goto :sapi64
if "%~1"=="" goto :all

:all
call :configure
if errorlevel 1 exit /b 1
"%CMAKE%" --build "%BUILD_DIR%"
if errorlevel 1 exit /b 1
REM src/sapi/CMakeLists.txt gates spfy_sapi64 on CMAKE_SIZEOF_VOID_P EQUAL 8,
REM so this 32-bit configure can never produce it -- but
REM installer/spfy_setup.iss requires it with no skipifsourcedoesntexist, so
REM ISCC fails outright on a tree that has only ever seen build32.bat. Building
REM it here is what makes "delete BUILD_DIR, rebuild, package" actually work.
call :sapi64
exit /b %ERRORLEVEL%

REM ------------------------------------------------------------------
REM  spfy_sapi64.dll -- the 64-bit SAPI shim.
REM
REM  A standalone gcc call rather than a CMake target: it does NOT link the
REM  synth core (it subprocess-spawns the 32-bit spfy_synth.exe and needs only
REM  spfy_dsp's pitch_shift / time_stretch for post-processing), which is why a
REM  64-bit DLL can land in an otherwise 32-bit build tree. Mirrors the
REM  invocation in .github/workflows/build-installer.yml so local installer
REM  tests package the same binary CI does.
REM
REM  Missing mingw64 WARNS rather than fails: the 32-bit CLI and the parity
REM  gate are perfectly usable without it, and failing the whole build would
REM  punish everyone who is not packaging an installer. Set
REM  SPFY_SKIP_SAPI64=1 to skip it deliberately.
REM ------------------------------------------------------------------
:sapi64
setlocal
if not "%SPFY_SKIP_SAPI64%"=="" (
    echo -- sapi64: skipped, SPFY_SKIP_SAPI64 is set
    endlocal & exit /b 0
)
set "GCC64=%MSYS_ROOT%\mingw64\bin\gcc.exe"
if not exist "%GCC64%" (
    echo.
    echo WARNING: mingw64 gcc not found at %GCC64%
    echo          spfy_sapi64.dll will NOT be built, and ISCC will fail on
    echo          installer\spfy_setup.iss until it exists.
    echo.
    endlocal & exit /b 0
)
REM mingw64\bin must be on PATH, not merely called by absolute path: gcc loads
REM its own runtime DLLs from beside itself and fails with NO diagnostic output
REM at all when it cannot.
set "PATH=%MSYS_ROOT%\mingw64\bin;%PATH%"
set "SAPI_SRC=%~dp0src\sapi"
set "DSP_SRC=%~dp0src\dsp"
set "OUT_DIR=%BUILD_DIR%\src\sapi"
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
REM -static -static-libgcc: no runtime dep on libgcc_s_seh-1.dll /
REM libwinpthread-1.dll on a stock Windows box.
REM --kill-at: strip the stdcall '@N' decoration so regsvr32 resolves the
REM Dll* entry points by their plain names.
"%GCC64%" -shared -O2 ^
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DCOBJMACROS -DCINTERFACE ^
    -I "%SAPI_SRC%" -I "%DSP_SRC%" ^
    -o "%OUT_DIR%\spfy_sapi64.dll" ^
    "%SAPI_SRC%\spfy_sapi64.c" ^
    "%DSP_SRC%\pitch_shift.c" ^
    "%DSP_SRC%\time_stretch.c" ^
    "%SAPI_SRC%\spfy_sapi64.def" ^
    -lole32 -loleaut32 -ladvapi32 -lshell32 -luuid ^
    -static -static-libgcc ^
    -Wl,--kill-at ^
    -Wl,--out-implib,"%OUT_DIR%\libspfy_sapi64.dll.a"
if errorlevel 1 (
    echo ERROR: 64-bit SAPI shim build failed.
    endlocal & exit /b 1
)
echo Built %OUT_DIR%\spfy_sapi64.dll
endlocal & exit /b 0

:configure
"%CMAKE%" -S "%SRC_DIR%" -B "%BUILD_DIR%" -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
  -DCMAKE_C_COMPILER="%GCC%" ^
  -DCMAKE_BUILD_TYPE=%SPFY_BUILD_TYPE% ^
  -DSPFY_STRICT_FP=%SPFY_STRICT_FP% ^
  -DSPFY_BUILD_TESTS=OFF ^
  -DSPFY_FE_HOSTED=ON
exit /b %ERRORLEVEL%
