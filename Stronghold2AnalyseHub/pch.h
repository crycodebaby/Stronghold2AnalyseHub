#pragma once

// Keep global headers minimal. This file is precompiled to speed up builds.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdarg>
#include <fstream>
#include <mutex>
#include <string>

// ImGui core + backends (DX9 + Win32)
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx9.h"

// MinHook (lightweight API hooking library)
#include "MinHook.h"
