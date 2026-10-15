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
    Stronghold2AnalyseHub/core/config.cpp
    Stronghold2AnalyseHub/core/capture.cpp
    Stronghold2AnalyseHub/core/manifest.cpp
    Stronghold2AnalyseHub/core/provenance.cpp
    Stronghold2AnalyseHub/core/lifecycle.cpp
    Stronghold2AnalyseHub/core/logger.cpp
    Stronghold2AnalyseHub/core/metrics.cpp
    Stronghold2AnalyseHub/core/sampler.cpp
    Stronghold2AnalyseHub/core/granny_observer.cpp
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
target_link_libraries(Stronghold2AnalyseHub PRIVATE minhook imgui d3d9 psapi bcrypt winmm)

add_executable(SH2Launcher launcher/launcher.cpp)

# d3d9.dll proxy: loads the hub without an injector (SH2-Perf docs/DECISIONS.md D009).
add_library(d3d9_proxy SHARED proxy/d3d9_proxy.cpp proxy/d3d9.def)
set_target_properties(d3d9_proxy PROPERTIES OUTPUT_NAME d3d9)

# Sampler validation without the game (docs/design/SAMPLING_PROFILER.md): build/Release/sampler_test.exe
add_executable(sampler_test tests/sampler_test.cpp Stronghold2AnalyseHub/core/sampler.cpp Stronghold2AnalyseHub/core/logger.cpp)
target_precompile_headers(sampler_test PRIVATE Stronghold2AnalyseHub/pch.h)
target_link_libraries(sampler_test PRIVATE minhook imgui d3d9 psapi winmm)

# Capture pipeline test without the game (ring, overflow, write/open errors, lifecycle): build/Release/capture_test.exe
add_executable(capture_test tests/capture_test.cpp Stronghold2AnalyseHub/core/capture.cpp Stronghold2AnalyseHub/core/config.cpp
    Stronghold2AnalyseHub/core/manifest.cpp Stronghold2AnalyseHub/core/provenance.cpp
    Stronghold2AnalyseHub/core/sampler.cpp Stronghold2AnalyseHub/core/granny_observer.cpp Stronghold2AnalyseHub/core/logger.cpp)
target_precompile_headers(capture_test PRIVATE Stronghold2AnalyseHub/pch.h)
target_compile_definitions(capture_test PRIVATE HUB_GIT_COMMIT="test")
target_link_libraries(capture_test PRIVATE minhook imgui d3d9 psapi winmm bcrypt)

# Background-thread lifecycle (blocked writer / sampler / hashing at unload): build/Release/lifecycle_test.exe
add_executable(lifecycle_test tests/lifecycle_test.cpp Stronghold2AnalyseHub/core/capture.cpp Stronghold2AnalyseHub/core/config.cpp
    Stronghold2AnalyseHub/core/manifest.cpp Stronghold2AnalyseHub/core/provenance.cpp Stronghold2AnalyseHub/core/sampler.cpp
    Stronghold2AnalyseHub/core/lifecycle.cpp Stronghold2AnalyseHub/core/granny_observer.cpp Stronghold2AnalyseHub/core/logger.cpp)
target_precompile_headers(lifecycle_test PRIVATE Stronghold2AnalyseHub/pch.h)
target_compile_definitions(lifecycle_test PRIVATE HUB_GIT_COMMIT="test")
target_link_libraries(lifecycle_test PRIVATE minhook imgui d3d9 psapi winmm bcrypt)

# Granny import-boundary observer without the game (SH2-Perf EXP-008): stand-in granny2.dll with the same decorated stdcall
# exports + a test exe that imports from it like Stronghold2.exe. build/Release/granny_observer_test.exe (granny2.dll next to it)
add_library(fake_granny2 SHARED tests/fake_granny2.cpp)
set_target_properties(fake_granny2 PROPERTIES OUTPUT_NAME granny2)
add_executable(granny_observer_test tests/granny_observer_test.cpp Stronghold2AnalyseHub/core/granny_observer.cpp
    Stronghold2AnalyseHub/core/config.cpp Stronghold2AnalyseHub/core/logger.cpp)
target_precompile_headers(granny_observer_test PRIVATE Stronghold2AnalyseHub/pch.h)
target_compile_definitions(granny_observer_test PRIVATE HUB_GIT_COMMIT="test")
target_link_libraries(granny_observer_test PRIVATE fake_granny2 minhook imgui d3d9 psapi winmm)

# D3D9 stand-in for the game, to test proxy + hub + sampler without Stronghold 2: build/Release/standin_d3d9.exe
add_executable(standin_d3d9 tests/standin_d3d9.cpp)
target_link_libraries(standin_d3d9 PRIVATE d3d9)
