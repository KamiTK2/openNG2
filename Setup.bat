@echo off
rem Double-click to install the NG2 recomp toolchain (REA, ReXGlue, Ghidra, compilers).
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
echo.
pause
