@echo off
REM Refresh the SAPI voice list. Run after dropping new voice folders
REM into %USERPROFILE%\Documents\Speechify\en-US\.
REM
REM Self-elevates via UAC because regsvr32 writes to HKLM. The 32-bit
REM regsvr32 is required for our 32-bit COM DLL; where it LIVES depends
REM on the Windows, so it is probed below rather than assumed.

setlocal

REM Self-elevate. If we're not admin, relaunch ourselves elevated.
net session >nul 2>&1
if %errorlevel% neq 0 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

set "DLL=%~dp0spfy_sapi.dll"
if not exist "%DLL%" (
    echo ERROR: spfy_sapi.dll not found at %DLL%
    echo The installer may be broken — reinstall.
    pause
    exit /b 1
)

echo Refreshing Speechify SAPI voice list...
echo   DLL: %DLL%
echo   Scanning %USERPROFILE%\Documents\Speechify\en-US\
echo.

REM Locate the 32-bit regsvr32. On 64-bit Windows it is the one in
REM SysWOW64 (the naming is historical); on 32-bit Windows SysWOW64 does
REM not exist at all and System32 IS the 32-bit one. Probe rather than
REM branch on %PROCESSOR_ARCHITECTURE%, which describes the CPU and not
REM the Windows running on it -- 32-bit Windows on an x64 chip reports
REM AMD64 there and would send us to a SysWOW64 that is not present.
set "RSVR=%SystemRoot%\SysWOW64\regsvr32.exe"
if not exist "%RSVR%" set "RSVR=%SystemRoot%\System32\regsvr32.exe"
if not exist "%RSVR%" (
    echo ERROR: regsvr32.exe not found under %SystemRoot%.
    pause
    exit /b 1
)

REM /u first to clear any stale tokens, then re-register
"%RSVR%" /s /u "%DLL%"
"%RSVR%" /s "%DLL%"
if %errorlevel% neq 0 (
    echo ERROR: regsvr32 returned %errorlevel%
    echo Voices may not be available in SAPI clients.
    pause
    exit /b 1
)

echo Done. Restart your SAPI client (Balabolka, Narrator, etc.) to see new voices.
echo.
pause
