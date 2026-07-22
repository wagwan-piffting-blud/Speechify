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
if "%~1"=="" goto :all

:all
call :configure
if errorlevel 1 exit /b 1
"%CMAKE%" --build "%BUILD_DIR%"
exit /b %ERRORLEVEL%

:configure
"%CMAKE%" -S "%SRC_DIR%" -B "%BUILD_DIR%" -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
  -DCMAKE_C_COMPILER="%GCC%" ^
  -DCMAKE_BUILD_TYPE=%SPFY_BUILD_TYPE% ^
  -DSPFY_STRICT_FP=%SPFY_STRICT_FP% ^
  -DSPFY_BUILD_TESTS=OFF ^
  -DSPFY_FE_HOSTED=ON
exit /b %ERRORLEVEL%
