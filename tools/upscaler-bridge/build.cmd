@echo off
rem Builds ng2_upscalers.dll and collects it with the vendors' DLLs in project\ng2\upscalers, which
rem ng2's build copies next to ng2.exe and make_release.py ships. Not committed.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
cd /d "%~dp0"
if not exist "build\CMakeCache.txt" (
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ || exit /b 1
)
cmake --build build -j 8 || exit /b 1
set OUT=..\..\project\ng2\upscalers
if not exist %OUT% mkdir %OUT%
copy /y build\ng2_upscalers.dll %OUT% >nul
copy /y ..\upscalers\DLSS\lib\Windows_x86_64\rel\nvngx_dlss.dll %OUT% >nul
copy /y ..\upscalers\FidelityFX\Kits\FidelityFX\signedbin\amd_fidelityfx_loader_vk.dll %OUT% >nul
copy /y ..\upscalers\FidelityFX\Kits\FidelityFX\signedbin\amd_fidelityfx_upscaler_vk.dll %OUT% >nul
