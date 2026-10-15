#include "../pch.h"
#include "logger.h"
#include "metrics.h"
#include "capture.h"
#include "config.h"
#include "provenance.h"
#include "sampler.h"
#include "../hooks/d3d9_hooks.h"

#include <exception>

// Keep DllMain minimal. Spin up a worker thread for initialization and lifetime.
// This avoids loader lock pitfalls and makes orderly shutdown possible.

namespace
{
    HMODULE           g_module = nullptr;
    HANDLE            g_thread = nullptr;
    std::atomic<bool> g_running{ true };

    // Game builds the hub was checked against (SH2-Perf docs/FINDINGS.md F001, F058).
    struct KnownBuild { const char* sha256; const char* name; };
    constexpr KnownBuild kKnownBuilds[] = {
        { "f3ccdf9bb41158202dffc000e5f2036a82f4c1ce8c5c89462f6140000f32d34b", "Steam 1.5 (no LAA)" },
    };

    // Log only: an unknown build may still work, but addresses in FINDINGS/FUNCTIONS may not match.
    void LogExeBuild()
    {
        // The hash comes from Provenance (hashed once, in the background, at hub start); the same value
        // goes into every recording's manifest.
        std::string sha;
        if (Provenance::Wait(30000))
            for (const auto& f : Provenance::Get().files)
                if (f.role == "game_exe") sha = f.sha256;
        const char* known = nullptr;
        for (const auto& b : kKnownBuilds)
            if (sha == b.sha256) known = b.name;
        if (sha.empty())
            Logger::Warn("exe sha256: could not hash the game exe (%lu).", GetLastError());
        else if (known)
            Logger::Info("exe sha256=%s -> known build: yes (%s)", sha.c_str(), known);
        else
            Logger::Warn("exe sha256=%s -> known build: NO (addresses may differ)", sha.c_str());
    }

    // Opened by name: created by SH2Launcher or by the d3d9.dll proxy, whoever started us.
    void SignalReady()
    {
        wchar_t eventName[64];
        swprintf_s(eventName, L"Local\\SH2AnalyseHub.Ready.%lu", GetCurrentProcessId());
        if (HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName)) {
            SetEvent(ready);
            CloseHandle(ready);
        }
    }

    DWORD RunHub()
    {
        // Visible in DebugView even if the log file cannot be written.
        OutputDebugStringW(L"SH2Hub: init thread started, Logger::Init next\n");
        Logger::Init(); // %SH2HUB_OUT% or next to the game exe
        OutputDebugStringW(L"SH2Hub: Logger::Init returned\n");
        Logger::Info("DLL attached (hub %s).", HUB_GIT_COMMIT);

        Config::Init();                                   // measurement switches (config.h)
        Provenance::Start();                              // background SHA-256 of exe/DLLs for the manifests (provenance.h)
        Capture::Init([] { return Metrics::Markers(); }); // capture thread: files, polling (capture.h)
        const bool installed = D3D9Hooks::Install();
        Sampler::Init();

        // Tell the starter (launcher or proxy) that the game may continue.
        // Signalled on failure too, so nobody waits for nothing.
        SignalReady();
        LogExeBuild();

        if (!installed) {
            Logger::Error("Hook installation failed; exiting.");
            Logger::Shutdown();
            return 0;
        }

        // Optional scripted recording without key presses (tests, automated runs):
        // SH2HUB_AUTORECORD=<frames> starts after the first 30 frames and stops after
        // about <frames> frames (the loop polls every 50 ms, so the count is approximate).
        wchar_t autoValue[16]{};
        uint64_t autoFrames = GetEnvironmentVariableW(L"SH2HUB_AUTORECORD", autoValue, 16)
            ? _wcstoui64(autoValue, nullptr, 10) : 0;
        uint64_t autoStart = 0;
        if (autoFrames) Logger::Info("SH2HUB_AUTORECORD=%llu frames", autoFrames);

        // Watchdog: hooks are in, the device exists, but no frame completed. Without the overlay there
        // is no EndScene fallback for a game that never calls Present.
        const DWORD loopStart = GetTickCount();
        bool warnedNoFrames = false;

        // Simple control loop.
        // END unload, F9 recording on/off, F8 marker, F10 reset histogram.
        while (g_running.load()) {
            if (autoFrames) {
                const uint64_t frame = Metrics::FrameIndex();
                if (!autoStart && frame >= 30 && !Metrics::Recording()) {
                    Metrics::ToggleRecording();
                    autoStart = frame;
                }
                else if (autoStart && frame >= autoStart + autoFrames) {
                    if (Metrics::Recording()) Metrics::ToggleRecording();
                    autoFrames = 0;
                }
            }
            if (GetAsyncKeyState(VK_END) & 1) {
                Logger::Info("VK_END pressed: requesting unload.");
                break;
            }
            if (GetAsyncKeyState(VK_F9) & 1) Metrics::ToggleRecording();
            if (GetAsyncKeyState(VK_F8) & 1) Metrics::AddMarker();
            if (GetAsyncKeyState(VK_F10) & 1) Metrics::ResetHistogram();
            if (!warnedNoFrames && GetTickCount() - loopStart > 30000 && D3D9Hooks::GetDevice() &&
                Metrics::FrameIndex() == 0) {
                warnedNoFrames = true;
                Logger::Warn("No frame completed 30 s after device creation: Present not seen (try overlay=1 for the EndScene fallback).");
            }
            Sleep(50);
        }

        Metrics::Shutdown();
        Sampler::Shutdown();
        D3D9Hooks::Uninstall();
        Logger::Info("DLL detaching.");
        Logger::Shutdown();
        return 0;
    }

    DWORD WINAPI InitThread(LPVOID)
    {
        // An exception escaping a thread calls abort() and takes the game down
        // ("Runtime Error! abnormal program termination"); never let it escape.
        try {
            RunHub();
        }
        catch (const std::exception& e) {
            OutputDebugStringA("SH2Hub: exception in hub thread: ");
            OutputDebugStringA(e.what());
            OutputDebugStringA("\n");
            Logger::Error("Exception in hub thread: %s", e.what());
            SignalReady();
        }
        catch (...) {
            OutputDebugStringW(L"SH2Hub: unknown exception in hub thread\n");
            Logger::Error("Unknown exception in hub thread.");
            SignalReady();
        }
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
