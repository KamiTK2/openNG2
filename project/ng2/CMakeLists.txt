# ng2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(ng2 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(NG2_SOURCES
    src/main.cpp
    src/crash_handler.cpp
    src/ng2_autopilot.cpp
    src/ng2_hooks.cpp
    src/native/ng2_native_d3d.cpp
    src/native/native_renderer.cpp
    src/native/native_shaders.cpp
    src/native/native_textures.cpp
    src/ps_prompts.cpp
    src/recomp_settings.cpp
    src/shader_precompile.cpp
    src/update_check.cpp
    src/ng2.rc
)

if(WIN32)
    add_executable(ng2 WIN32 ${NG2_SOURCES})
else()
    add_executable(ng2 ${NG2_SOURCES})
endif()

rexglue_setup_target(ng2 GPU_PLUGINS xenos)

# openNG2 version (update check). The launcher passes it; a source checkout reads ../../VERSION.
if(NOT OPENNG2_VERSION)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION")
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" OPENNG2_VERSION LIMIT_COUNT 1)
    else()
        set(OPENNG2_VERSION "dev")
    endif()
endif()
set_source_files_properties(src/update_check.cpp src/recomp_settings.cpp PROPERTIES
    COMPILE_DEFINITIONS "OPENNG2_VERSION=\"${OPENNG2_VERSION}\"")
target_link_libraries(ng2 PRIVATE winhttp windowscodecs ole32)

# The native renderer uses the SDK Vulkan headers, which include RenderDoc's from their own folder.
target_include_directories(ng2 SYSTEM PRIVATE "${REXGLUE_SHARE_DIR}/../../include/renderdoc")

# Bind-time translation for vertex shaders the game patches at runtime, which the offline pack can't
# hold in full. The plugin doesn't export the translator, so its sources are compiled in here, as
# in tools/native-shader-tool.
set(NG2_SDK ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/rexglue-sdk)
if(NOT EXISTS ${NG2_SDK}/src/graphics)
    set(NG2_SDK ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/translator)  # launcher install (make_release.py)
endif()
set(NG2_G ${NG2_SDK}/src/graphics)
target_sources(ng2 PRIVATE
    ${NG2_G}/flags.cpp
    ${NG2_G}/format/ucode.cpp
    ${NG2_G}/util/draw.cpp
    ${NG2_G}/registers.cpp
    ${NG2_G}/pipeline/shader/shader.cpp
    ${NG2_G}/pipeline/shader/translator.cpp
    ${NG2_G}/pipeline/shader/translator_disasm.cpp
    ${NG2_G}/pipeline/shader/spirv.cpp
    ${NG2_G}/pipeline/shader/spirv_builder.cpp
    ${NG2_G}/pipeline/shader/spirv_translator.cpp
    ${NG2_G}/pipeline/shader/spirv_translator_alu.cpp
    ${NG2_G}/pipeline/shader/spirv_translator_fetch.cpp
    ${NG2_G}/pipeline/shader/spirv_translator_memexport.cpp
    ${NG2_G}/pipeline/shader/spirv_translator_rb.cpp)
set_source_files_properties(src/native/native_shaders.cpp src/native/native_renderer.cpp PROPERTIES
    COMPILE_DEFINITIONS NG2_NATIVE_WITH_TRANSLATOR=1)
target_include_directories(ng2 BEFORE PRIVATE ${NG2_SDK}/include)
target_include_directories(ng2 SYSTEM PRIVATE
    ${NG2_SDK}/thirdparty/glslang
    ${NG2_SDK}/thirdparty/spirv-headers/include)
target_link_libraries(ng2 PRIVATE rex::SPIRV rex::glslang)

# NG2 DIAG: whole-program guest function counter (tools/gen_trace_all.py). Off by default.
option(NG2_TRACE_ALL "Count guest function entries (diagnostic)" OFF)
if(NG2_TRACE_ALL)
    target_sources(ng2 PRIVATE src/ng2_trace_all.cpp src/ng2_trace_ctl.cpp)
    # Per-file define: a target-wide one would rebuild all generated sources.
    set_source_files_properties(src/main.cpp PROPERTIES COMPILE_DEFINITIONS NG2_TRACE_ALL=1)
endif()

# Ship the per-game runtime config (cvar defaults) next to the executable.
add_custom_command(TARGET ng2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/ng2.toml $<TARGET_FILE_DIR:ng2>/ng2.toml
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:ng2>/shaders
    COMMAND ${CMAKE_COMMAND}
        -DPACK=${CMAKE_CURRENT_SOURCE_DIR}/../../tools/native-shader-tool/out/ng2_native_shaders.pack
        -DDEST=$<TARGET_FILE_DIR:ng2>/ng2_native_shaders.pack
        -P ${CMAKE_CURRENT_SOURCE_DIR}/src/native/deploy_pack.cmake
    VERBATIM)
# DLSS and FSR: ng2_upscalers.dll and the vendors' DLLs, built by tools/upscaler-bridge. Without
# them the game builds and runs, and the upscaler rows offer FSR 1.0 only.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/upscalers)
    add_custom_command(TARGET ng2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/upscalers $<TARGET_FILE_DIR:ng2>
        VERBATIM)
endif()
