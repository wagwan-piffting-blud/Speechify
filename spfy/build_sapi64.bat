@echo off
REM ============================================================
REM  build_sapi64.bat -- build spfy_sapi64.dll (the 64-bit SAPI shim).
REM
REM  ⚠ THIS IS NOW A THIN WRAPPER. build32.bat builds the shim as part of a
REM  normal build, and owns the gcc invocation. Two copies of that command
REM  line drifting apart is exactly the failure this avoids: the flags have to
REM  match .github/workflows/build-installer.yml or a locally packaged
REM  installer ships a different binary from the one CI produces.
REM
REM  Kept because it is referenced by docs and muscle memory, and because
REM  rebuilding only the shim is genuinely useful -- the full build is not
REM  cheap and the shim does not depend on the synth core.
REM
REM  Usage (run from PowerShell, per project convention):
REM      .\spfy\build_sapi64.bat
REM ============================================================
call "%~dp0build32.bat" sapi64
exit /b %ERRORLEVEL%
