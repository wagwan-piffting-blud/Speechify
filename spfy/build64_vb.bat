@echo off
REM ============================================================
REM  spfy build64_vb.bat -- x64 CONTAINER TOOLS only.
REM
REM  ASCII ONLY IN THIS FILE. cmd reads it in the OEM codepage; a UTF-8
REM  star or warning glyph in a REM line breaks parsing of the lines that
REM  follow, and the caret-continued cmake call silently never runs. The
REM  first version of this script printed "Built:" and produced no exe.
REM
REM  WHY THIS EXISTS. spfy_vb_build is 32-bit, and 32-bit gets 2 GB of user
REM  address space (4 GB with --large-address-aware, which src/cli/CMakeLists
REM  already sets). Tom's real+EL corpus -- 7,057 recordings, 2,766,428 units,
REM  987 MB of u-law -- dies in S1 CORPUS with no error printed. The same
REM  corpus thinned to 6,429 recordings / 1,751,404 units builds fine, so the
REM  ceiling is real and it is close. x64 takes it to physical RAM.
REM
REM  THIS IS NOT A PORT. The container tools carry no front end:
REM  spfy_vb_build.c references no spfy_fe_* symbol, and the spfy_feat_table_*
REM  calls in src/vb are the VIN's feat CHUNK, not the FE. The .fe sidecars are
REM  read as text, produced earlier by spfy_synth. So SPFY_FE_HOSTED is
REM  irrelevant here, and this toolchain is already proven byte-exact with the
REM  oracle (build_emu.bat, verified 2026-07-22 across four platforms).
REM
REM  SCOPE. Only spfy_vb_build and spfy_vb_verify come from this tree.
REM  spfy_synth STAYS 32-bit: it is what the parity gate compares against
REM  vendor Speechify and it must not move. Same reasoning that scopes
REM  --large-address-aware to targets rather than setting it globally.
REM
REM  Release + SPFY_STRICT_FP=ON, MATCHING build32.bat exactly. Two reasons:
REM  build.bat's Debug/-O0 would crawl through S4 JOIN (316 M candidate joins
REM  on Mara), and STRICT_FP's -ffloat-store is what pins down when x87 80-bit
REM  intermediates spill to memory. The whole point of this tree is a container
REM  that is byte-identical to the 32-bit one, so the FP configuration has to
REM  match or the tree-derived chunks will differ for reasons that have nothing
REM  to do with the word size.
REM
REM  INVOKE FROM POWERSHELL BY FULL PATH. msys2 cmake exits 57 when its -D
REM  arguments come through PowerShell argument parsing; running them from a
REM  .bat sidesteps that. cmd does not search the CWD, so bare
REM  "cmd /c build64_vb" fails.
REM
REM  AFTER BUILDING, PROVE IT CHANGED NOTHING: build one corpus with both the
REM  32-bit and 64-bit tools and md5 the containers. Byte-identical
REM  .vin/.vdb/.vcf means no container byte moved.
REM ============================================================
setlocal

if "%MSYS_ROOT%"=="" set "MSYS_ROOT=C:\msys64"

REM mingw64\bin must be on PATH, not merely called by absolute path. These
REM binaries load their own DLLs from there; without it cmake.exe exits with
REM NO OUTPUT AT ALL and errorlevel 0, so the script sails past every check and
REM the failure only surfaces as a missing artifact later.
set "PATH=%MSYS_ROOT%\mingw64\bin;%PATH%"

REM PROBE THE CMAKE, DO NOT ASSUME IT, and probe with || rather than
REM "if errorlevel". The msys2 mingw64 cmake.exe has been unable to start since
REM an msys2 update (exit 57 / STATUS_ENTRYPOINT_NOT_FOUND, no output, no
REM diagnostic). A binary that cannot START does not set errorlevel the way a
REM binary that runs and fails does, so "if errorlevel 1" sails straight past
REM it -- which is how build32.bat once shipped a stale spfy_synth for a day
REM and passed a 221/221 parity run against it. Same trap caught this script.
set "CMAKE=%MSYS_ROOT%\mingw64\bin\cmake.exe"
("%CMAKE%" --version >nul 2>&1) || (
    echo build64_vb: msys2 cmake cannot run -- using the system CMake
    set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
)
set "NINJA=%MSYS_ROOT%\mingw64\bin\ninja.exe"
set "GCC=%MSYS_ROOT%\mingw64\bin\gcc.exe"
set "SCRIPT_DIR=%~dp0"
if "%SPFY_BUILD64_DIR%"=="" set "SPFY_BUILD64_DIR=C:\tmp\spfy_build64"
set "OUT_BUILD=%SPFY_BUILD64_DIR%\src\cli\spfy_vb_build.exe"
set "OUT_VERIFY=%SPFY_BUILD64_DIR%\src\cli\spfy_vb_verify.exe"

for %%T in ("%CMAKE%" "%NINJA%" "%GCC%") do (
    if not exist %%T (
        echo ERROR: missing %%T
        exit /b 1
    )
)

REM Probe the toolchain rather than trusting the path.
"%GCC%" -dumpmachine | findstr /b "x86_64" >nul
if errorlevel 1 (
    echo ERROR: "%GCC%" is not an x86_64 compiler.
    "%GCC%" -dumpmachine
    exit /b 1
)

REM Stale outputs would make a failed build look successful below.
if exist "%OUT_BUILD%" del /q "%OUT_BUILD%"
if exist "%OUT_VERIFY%" del /q "%OUT_VERIFY%"

echo [configure] %SPFY_BUILD64_DIR%
"%CMAKE%" -S "%SCRIPT_DIR%." -B "%SPFY_BUILD64_DIR%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_C_COMPILER="%GCC%" -DCMAKE_BUILD_TYPE=Release -DSPFY_STRICT_FP=ON -DSPFY_BUILD_TESTS=OFF -DSPFY_FE_HOSTED=OFF
if errorlevel 1 (
    echo ERROR: cmake configure failed.
    exit /b 1
)

echo [build] spfy_vb_build spfy_vb_verify
"%CMAKE%" --build "%SPFY_BUILD64_DIR%" --target spfy_vb_build spfy_vb_verify
if errorlevel 1 (
    echo ERROR: cmake --build failed.
    exit /b 1
)

REM Do not trust errorlevel alone -- check the artifacts exist.
if not exist "%OUT_BUILD%" (
    echo ERROR: %OUT_BUILD% was not produced.
    exit /b 1
)
if not exist "%OUT_VERIFY%" (
    echo ERROR: %OUT_VERIFY% was not produced.
    exit /b 1
)

echo.
echo Built:
echo   %OUT_BUILD%
echo   %OUT_VERIFY%
echo.
echo spfy_synth is deliberately NOT built here; it stays 32-bit for the
echo parity gate. Use build32.bat for it.
exit /b 0
