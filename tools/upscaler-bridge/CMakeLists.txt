# ng2_upscalers.dll: the only code that touches the DLSS and FSR SDKs (see upscale_bridge.h). Those
# SDKs sit in tools/upscalers, which is not committed; fetch steps are in notes/native/design.md.
cmake_minimum_required(VERSION 3.25)
project(ng2_upscalers CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(DLSS ${ROOT}/tools/upscalers/DLSS)
set(FFX ${ROOT}/tools/upscalers/FidelityFX/Kits/FidelityFX)
add_library(ng2_upscalers SHARED bridge.cpp)
target_include_directories(ng2_upscalers PRIVATE
    ${ROOT}/project/ng2/src/native
    ${ROOT}/tools/rexglue-sdk/thirdparty/vulkan-headers/include
    ${DLSS}/include
    ${FFX}/api/include
    ${FFX}/upscalers/include)
target_compile_definitions(ng2_upscalers PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WINDOWS)
target_link_libraries(ng2_upscalers PRIVATE ${DLSS}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib
    $ENV{VULKAN_SDK}/Lib/vulkan-1.lib)
