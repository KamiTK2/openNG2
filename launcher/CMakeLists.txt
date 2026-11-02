# openNG2 launcher: a small native GUI that checks the player's files, builds the game with a
# downloaded toolchain and installs ng2.exe (see notes/findings.md #67).
cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0091 NEW)
project(openNG2Launcher LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static C/C++ runtime: the launcher runs on a fresh Windows install with nothing else present.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Re-configure when VERSION changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
if(NOT OPENNG2_VERSION)  # -DOPENNG2_VERSION=... overrides (update tests)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" OPENNG2_VERSION LIMIT_COUNT 1)
endif()
set(NG2_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../project/ng2/src")
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools/rexglue-sdk/thirdparty/imgui" CACHE PATH "Dear ImGui")

add_executable(openNG2 WIN32
    src/main.cpp
    src/inputs.cpp
    src/pipeline.cpp
    src/util.cpp
    src/xbox.cpp
    src/launcher.rc
    src/updater.cpp
    ${NG2_SRC}/update_check.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(openNG2 PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends ${NG2_SRC})
target_compile_definitions(openNG2 PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
    "OPENNG2_VERSION=\"${OPENNG2_VERSION}\"")
target_link_libraries(openNG2 PRIVATE d3d11 dxgi dwmapi shell32 ole32 uuid winhttp bcrypt)
