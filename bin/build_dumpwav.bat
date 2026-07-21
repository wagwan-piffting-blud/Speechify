@echo off
REM ============================================================
REM  build_dumpwav.bat -- rebuild bin/spfy_dumpwav.exe (MSVC, x86).
REM
REM  MUST be 32-bit: the client LoadLibrary's the 32-bit SWItts.dll
REM  that ships beside it, so an x64 build fails at load time.
REM
REM  Run from PowerShell:   .\bin\build_dumpwav.bat
REM
REM  Override the toolchain location:
REM    set VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community
REM ============================================================

setlocal EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

if "%VSDIR%"=="" set "VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community"
set "VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvars32.bat"

if not exist "%VCVARS%" (
    echo error: vcvars32.bat not found at "%VCVARS%"
    echo        set VSDIR to your Visual Studio install root and retry.
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 ( echo error: vcvars32 failed & exit /b 1 )

pushd "%SCRIPT_DIR%" >nul

REM /O2 for the sample-copy loops; /W3 keeps the noise down on this
REM 2003-era API surface without hiding real problems.
REM _CRT_SECURE_NO_WARNINGS is defined in the source, not here -- defining
REM it on the command line too triggers C4005 macro redefinition.
cl /nologo /O2 /W3 ^
   spfy_dumpwav.c ws2_32.lib /Fe:spfy_dumpwav.exe /Fo:spfy_dumpwav.obj
set "RC=%ERRORLEVEL%"

popd >nul

if "%RC%"=="0" (
    echo [done] bin\spfy_dumpwav.exe rebuilt.
) else (
    echo [fail] cl returned %RC%
)
exit /b %RC%
