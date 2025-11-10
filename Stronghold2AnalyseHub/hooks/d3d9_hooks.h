#pragma once
#include "../pch.h"

// DX9 + Win32 hooks.
// We hook from the exported entry (Direct3DCreate9) so we see every device.
// The goal is to capture HWND + IDirect3DDevice9* and render an ImGui overlay.
namespace D3D9Hooks
{
    // Returns true on success (MinHook initialized, export hook active).
    bool Install();

    // Restores original WndProc, shuts down ImGui, disables MinHook.
    void Uninstall();

    // Accessors for other modules/tools if needed.
    HWND               GetGameHwnd();
    IDirect3DDevice9* GetDevice();
}
