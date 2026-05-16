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
  -DCMAKE_BUILD_TYPE=Debug ^
  -DSPFY_STRICT_FP=OFF ^
  -DSPFY_BUILD_TESTS=OFF ^
  -DSPFY_FE_HOSTED=ON
exit /b %ERRORLEVEL%
