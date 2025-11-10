#include "../pch.h"
#include "logger.h"
#include "../hooks/d3d9_hooks.h"

// Keep DllMain minimal. Spin up a worker thread for initialization and lifetime.
// This avoids loader lock pitfalls and makes orderly shutdown possible.

namespace
{
    HMODULE           g_module = nullptr;
    HANDLE            g_thread = nullptr;
    std::atomic<bool> g_running{ true };

    DWORD WINAPI InitThread(LPVOID)
    {
        Logger::Init(); // log next to game exe
        Logger::Info("DLL attached.");

        if (!D3D9Hooks::Install()) {
            Logger::Error("Hook installation failed; exiting.");
            Logger::Shutdown();
            FreeLibraryAndExitThread(g_module, 0);
            return 0;
        }

        // Simple control loop.
        // Press END to request unload.
        while (g_running.load()) {
            if (GetAsyncKeyState(VK_END) & 1) {
                Logger::Info("VK_END pressed: requesting unload.");
                break;
            }
            Sleep(50);
        }

        D3D9Hooks::Uninstall();
        Logger::Info("DLL detaching.");
        Logger::Shutdown();

        FreeLibraryAndExitThread(g_module, 0);
        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = hModule;
        DisableThreadLibraryCalls(hModule);
        g_thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        g_running.store(false);
    }
    return TRUE;
}
