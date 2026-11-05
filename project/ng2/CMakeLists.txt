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
    VERBATIM)
