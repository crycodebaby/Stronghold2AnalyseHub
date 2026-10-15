# Build (32-bit only, the game is 32-bit):
#   cmake -S . -B build -A Win32
#   cmake --build build --config Release
cmake_minimum_required(VERSION 3.21)
project(Stronghold2AnalyseHub LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Stronghold 2 is 32-bit: configure with -A Win32")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL must not depend on whichever msvcr*/vcruntime the game folder has.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE)

add_library(minhook STATIC
    external/minhook/src/buffer.c
    external/minhook/src/hook.c
    external/minhook/src/trampoline.c
    external/minhook/src/hde/hde32.c)
target_include_directories(minhook PUBLIC external/minhook/include)

add_library(imgui STATIC
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_dx9.cpp
    external/imgui/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC external/imgui)

add_library(Stronghold2AnalyseHub SHARED
    Stronghold2AnalyseHub/core/dllmain.cpp
    Stronghold2AnalyseHub/core/logger.cpp
    Stronghold2AnalyseHub/core/metrics.cpp
    Stronghold2AnalyseHub/hooks/d3d9_hooks.cpp)
target_precompile_headers(Stronghold2AnalyseHub PRIVATE Stronghold2AnalyseHub/pch.h)

# Commit baked into latest.json so captures name the exact hub build.
# Evaluated at configure time: re-run cmake after committing.
execute_process(COMMAND git describe --always --dirty --abbrev=12
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE HUB_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT HUB_GIT_COMMIT)
    set(HUB_GIT_COMMIT "unknown")
endif()
target_compile_definitions(Stronghold2AnalyseHub PRIVATE HUB_GIT_COMMIT="${HUB_GIT_COMMIT}")
target_link_libraries(Stronghold2AnalyseHub PRIVATE minhook imgui d3d9 psapi)

add_executable(SH2Launcher launcher/launcher.cpp)
