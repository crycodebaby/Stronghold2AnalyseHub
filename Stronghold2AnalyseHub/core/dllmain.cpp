#include "../pch.h"
#include "logger.h"
#include "metrics.h"
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

        const bool installed = D3D9Hooks::Install();

        // Tell SH2Launcher (if it started us) that it may resume the game.
        // Signalled on failure too, so the launcher never waits for nothing.
        wchar_t eventName[64];
        swprintf_s(eventName, L"Local\\SH2AnalyseHub.Ready.%lu", GetCurrentProcessId());
        if (HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName)) {
            SetEvent(ready);
            CloseHandle(ready);
        }

        if (!installed) {
            Logger::Error("Hook installation failed; exiting.");
            Logger::Shutdown();
            FreeLibraryAndExitThread(g_module, 0);
            return 0;
        }

        // Simple control loop.
        // END unload, F9 recording on/off, F8 marker, F10 reset histogram.
        while (g_running.load()) {
            if (GetAsyncKeyState(VK_END) & 1) {
                Logger::Info("VK_END pressed: requesting unload.");
                break;
            }
            if (GetAsyncKeyState(VK_F9) & 1) Metrics::ToggleRecording();
            if (GetAsyncKeyState(VK_F8) & 1) Metrics::AddMarker();
            if (GetAsyncKeyState(VK_F10) & 1) Metrics::ResetHistogram();
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
