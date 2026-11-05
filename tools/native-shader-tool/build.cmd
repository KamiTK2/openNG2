@echo off
rem Builds native_shader_tool in tools\native-shader-tool\build, separate from the ng2 build.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul || exit /b 1
cd /d "%~dp0"
if not exist "build\CMakeCache.txt" (
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ || exit /b 1
)
cmake --build build -j 8
