# Offline Xenos ucode -> native SPIR-V converter (notes/native/design.md, M2). Built on its own, so
# it never touches the ng2 build. Compiles the SDK translator sources directly: the rexgpu-xenos
# plugin does not export them.
cmake_minimum_required(VERSION 3.25)
project(native_shader_tool C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NG2_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(SDK ${NG2_ROOT}/tools/rexglue-sdk)
list(PREPEND CMAKE_PREFIX_PATH ${SDK}/out/install/win-amd64)
find_package(rexglue 0.10.0 REQUIRED CONFIG)

set(G ${SDK}/src/graphics)
add_executable(native_shader_tool
    main.cpp
    ${NG2_ROOT}/project/ng2/src/native/native_shaders.cpp
    ${G}/flags.cpp
    ${G}/format/ucode.cpp
    ${G}/util/draw.cpp
    ${G}/registers.cpp
    ${G}/pipeline/shader/shader.cpp
    ${G}/pipeline/shader/translator.cpp
    ${G}/pipeline/shader/translator_disasm.cpp
    ${G}/pipeline/shader/spirv.cpp
    ${G}/pipeline/shader/spirv_builder.cpp
    ${G}/pipeline/shader/spirv_translator.cpp
    ${G}/pipeline/shader/spirv_translator_alu.cpp
    ${G}/pipeline/shader/spirv_translator_fetch.cpp
    ${G}/pipeline/shader/spirv_translator_memexport.cpp
    ${G}/pipeline/shader/spirv_translator_rb.cpp
)
target_compile_definitions(native_shader_tool PRIVATE NG2_NATIVE_WITH_TRANSLATOR=1)
target_include_directories(native_shader_tool BEFORE PRIVATE
    ${SDK}/include
    ${NG2_ROOT}/project/ng2/src
    ${SDK}/thirdparty/glslang
    ${SDK}/thirdparty/spirv-headers/include
    ${SDK}/thirdparty/renderdoc
    ${SDK}/thirdparty/vulkan-headers/include)
target_link_libraries(native_shader_tool PRIVATE rex::runtime rex::SPIRV rex::glslang)
