#pragma once

// Keep global headers minimal. This file is precompiled to speed up builds.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <d3d9.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

// ImGui core + backends (DX9 + Win32)
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx9.h"

// MinHook (lightweight API hooking library)
#include "MinHook.h"
