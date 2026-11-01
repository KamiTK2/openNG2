@echo off
rem Build the ng2 recomp. Usage: build.cmd [preset] [jobs]
rem   preset: win-amd64-relwithdebinfo (default) | win-amd64-release | win-amd64-debug
rem Runs inside a VS 2022 Build Tools environment so CMake finds VS's bundled clang.
setlocal
set PRESET=%~1
if "%PRESET%"=="" set PRESET=win-amd64-relwithdebinfo
set JOBS=%~2
if "%JOBS%"=="" set JOBS=12

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul || exit /b 1
cd /d "%~dp0"

if not exist "out\build\%PRESET%\CMakeCache.txt" (
  cmake --preset %PRESET% || exit /b 1
)
cmake --build --preset %PRESET% -j %JOBS%
