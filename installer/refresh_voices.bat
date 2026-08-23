@echo off
REM Refresh the SAPI voice list. Run after dropping new voice folders
REM into %USERPROFILE%\Documents\Speechify\en-US\.
REM
REM Self-elevates via UAC because regsvr32 writes to HKLM. The 32-bit
REM regsvr32 is required for our 32-bit COM DLL; where it LIVES depends
REM on the Windows, so it is probed below rather than assumed.
REM
REM The DLL likewise does NOT have to sit beside this file. Copy this .bat to
REM the desktop, a USB stick, anywhere -- it finds an installed Speechify by
REM asking the registry which DLL is actually registered. Pass an explicit
REM path (file OR folder) as the first argument to override every probe:
REM
REM     refresh_voices.bat "D:\builds\spfy_sapi.dll"
REM     refresh_voices.bat "D:\builds"

setlocal

REM Self-elevate. If we're not admin, relaunch ourselves elevated.
REM ⚠ The argument has to survive the relaunch, or an explicit path given by
REM the user is silently dropped and the elevated copy probes instead.
net session >nul 2>&1
if %errorlevel% neq 0 (
    if "%~1"=="" (
        powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    ) else (
        powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '\"%~1\"' -Verb RunAs"
    )
    exit /b 0
)

REM ------------------------------------------------------------------
REM Find spfy_sapi.dll. First hit wins; every candidate is checked for
REM existence, so a registry entry left behind by a deleted install is
REM skipped rather than handed to regsvr32.
REM ------------------------------------------------------------------
set "DLL="
set "HOW="

REM 1. Explicit argument -- a file, or a folder containing the DLL.
if not "%~1"=="" (
    if exist "%~1\spfy_sapi.dll" (
        set "DLL=%~1\spfy_sapi.dll"
        set "HOW=argument (folder)"
    ) else if exist "%~1" (
        set "DLL=%~1"
        set "HOW=argument"
    ) else (
        echo WARNING: %~1 does not exist; falling back to the usual locations.
        echo.
    )
)

REM 2. Beside this script -- the installed layout, and the common case.
if not defined DLL if exist "%~dp0spfy_sapi.dll" (
    set "DLL=%~dp0spfy_sapi.dll"
    set "HOW=beside this script"
)

REM 3. Whatever COM currently has registered. This is the authoritative
REM    answer when the .bat has been moved: the CLSID is baked into the DLL
REM    and written by its own DllRegisterServer. 32-bit COM on 64-bit Windows
REM    lands under WOW6432Node; on 32-bit Windows it is the plain hive.
if not defined DLL call :fromkey "HKLM\SOFTWARE\Classes\WOW6432Node\CLSID\{9C3A7D1E-4F5A-4B6C-8EA2-5C71D08F1234}\InprocServer32" "registered COM server (32-bit view)"
if not defined DLL call :fromkey "HKLM\SOFTWARE\Classes\CLSID\{9C3A7D1E-4F5A-4B6C-8EA2-5C71D08F1234}\InprocServer32" "registered COM server"

REM 4. The installer's own record of where it put things. Survives the COM
REM    registration being lost, which is the exact situation this script is
REM    usually run to repair.
if not defined DLL call :fromloc "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{B7EC3D11-1A22-4F2C-9F18-3C7E5E5E3D71}_is1" "installer record"
if not defined DLL call :fromloc "HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\{B7EC3D11-1A22-4F2C-9F18-3C7E5E5E3D71}_is1" "installer record (32-bit view)"

REM 5. The default install directory, in case both registry trails are gone.
REM    ⚠ ProgramW6432 and ProgramFiles(x86) DO NOT EXIST on 32-bit Windows.
REM    Unguarded, "%ProgramW6432%\Speechify\spfy_sapi.dll" collapses to
REM    "\Speechify\spfy_sapi.dll" -- a RELATIVE path on whatever drive is
REM    current, which probes somewhere nobody meant and could match a stray
REM    C:\Speechify. ProgramFiles alone is correct there and is always defined.
if defined ProgramW6432 if not defined DLL call :try "%ProgramW6432%\Speechify\spfy_sapi.dll" "default install dir"
if not defined DLL call :try "%ProgramFiles%\Speechify\spfy_sapi.dll" "default install dir"
if defined ProgramFiles(x86) if not defined DLL call :try "%ProgramFiles(x86)%\Speechify\spfy_sapi.dll" "default install dir (x86)"

if not defined DLL (
    echo ERROR: spfy_sapi.dll not found.
    echo.
    echo Looked beside this script, in the registered COM server entry, in the
    echo installer's record, and in the default install directory.
    echo.
    echo If Speechify is installed somewhere unusual, pass the path:
    echo     "%~nx0" "C:\path\to\spfy_sapi.dll"
    echo.
    pause
    exit /b 1
)

echo Refreshing Speechify SAPI voice list...
echo   DLL: %DLL%
echo   found via: %HOW%
echo.

REM ------------------------------------------------------------------
REM Report what the DLL's own scan will and will not find, BEFORE running
REM it. spfy_sapi.c's scan_and_register_voices() `continue`s past every
REM mismatch without a word, so a voice that does not appear gives the user
REM nothing to go on. This mirrors its rules exactly:
REM
REM   <Documents>\Speechify\en-US\<dir>\<dir>.vin + <dir>8.vdb + <dir>.vcf
REM
REM ⚠ The FOLDER NAME must equal the file basenames. "CRS Mara\crsmara.vin"
REM registers nothing, and neither does a folder named after a build arm.
REM ------------------------------------------------------------------
call :docsdir
echo   Scanning %DOCS%\Speechify\en-US\
if not exist "%DOCS%\Speechify\en-US\" (
    echo     ^(that folder does not exist -- create it and put voice folders in it^)
    echo.
    goto :afterscan
)
set /a VOK=0
set /a VBAD=0
for /d %%D in ("%DOCS%\Speechify\en-US\*") do call :checkvoice "%%~nxD"
echo     %VOK% registerable, %VBAD% incomplete
if %VOK%==0 (
    echo.
    echo     No voice will register. The folder name must MATCH the files
    echo     inside it, and both must sit under the path shown above:
    echo         ...\Speechify\en-US\crsmara\crsmara.vin
    echo                                    \crsmara8.vdb
    echo                                    \crsmara.vcf
    echo     A repository checked out elsewhere is NOT scanned, and a folder
    echo     renamed to anything but the voice name is skipped in silence.
)
echo.
:afterscan

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
exit /b 0

REM ------------------------------------------------------------------
REM Helpers. None of these use setlocal, so the DLL/HOW they set are
REM visible to the caller.
REM ------------------------------------------------------------------

REM :fromkey <key> <label>  -- read the key's DEFAULT value as a DLL path.
:fromkey
for /f "tokens=2,*" %%A in ('reg query "%~1" /ve 2^>nul ^| findstr /i "REG_SZ REG_EXPAND_SZ"') do (
    if exist "%%B" (
        set "DLL=%%B"
        set "HOW=%~2"
    )
)
goto :eof

REM :fromloc <key> <label>  -- read InstallLocation and look inside it.
:fromloc
for /f "tokens=2,*" %%A in ('reg query "%~1" /v InstallLocation 2^>nul ^| findstr /i "REG_SZ REG_EXPAND_SZ"') do (
    if exist "%%B\spfy_sapi.dll" (
        set "DLL=%%B\spfy_sapi.dll"
        set "HOW=%~2"
    )
)
goto :eof

REM :try <path> <label>
:try
if exist "%~1" (
    set "DLL=%~1"
    set "HOW=%~2"
)
goto :eof

REM :docsdir -- the user's real Documents folder.
REM ⚠ NOT %USERPROFILE%\Documents. The DLL calls SHGetFolderPath(CSIDL_PERSONAL),
REM which follows a redirected Documents -- OneDrive moves it constantly. Reading
REM the same shell-folder registry value is the batch equivalent; guessing the
REM literal path sends the report to a directory the DLL never looks at.
:docsdir
set "DOCS="
for /f "tokens=2,*" %%A in ('reg query "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders" /v Personal 2^>nul ^| findstr /i "REG_SZ REG_EXPAND_SZ"') do call set "DOCS=%%B"
if not defined DOCS set "DOCS=%USERPROFILE%\Documents"
if not exist "%DOCS%\" set "DOCS=%USERPROFILE%\Documents"
goto :eof

REM :checkvoice <folder name> -- does this folder satisfy the DLL's rule?
:checkvoice
set "VD=%DOCS%\Speechify\en-US\%~1"
set "VMISS="
if not exist "%VD%\%~1.vin"  set "VMISS=%VMISS% %~1.vin"
if not exist "%VD%\%~18.vdb" set "VMISS=%VMISS% %~18.vdb"
if not exist "%VD%\%~1.vcf"  set "VMISS=%VMISS% %~1.vcf"
if not defined VMISS (
    echo     [ok]      %~1
    set /a VOK+=1
) else (
    echo     [SKIPPED] %~1  -- missing:%VMISS%
    set /a VBAD+=1
)
goto :eof
