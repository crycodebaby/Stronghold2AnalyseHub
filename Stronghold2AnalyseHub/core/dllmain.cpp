#include "../pch.h"
#include "logger.h"
#include "metrics.h"
#include "../hooks/d3d9_hooks.h"

#include <bcrypt.h>
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

    // SHA-256 of a file as lowercase hex; empty string on error.
    std::string Sha256File(const wchar_t* path)
    {
        std::string hex;
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) return hex;

        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
            BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            std::vector<UCHAR> buf(1 << 20);
            DWORD got = 0;
            bool ok = true;
            while (ok && ReadFile(file, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got)
                ok = BCryptHashData(hash, buf.data(), got, 0) == 0;
            UCHAR digest[32];
            if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                char two[3];
                for (UCHAR b : digest) { sprintf_s(two, "%02x", b); hex += two; }
            }
        }
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        CloseHandle(file);
        return hex;
    }

    // Log only: an unknown build may still work, but addresses in FINDINGS/FUNCTIONS may not match.
    void LogExeBuild()
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::string sha = Sha256File(exe);
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

        const bool installed = D3D9Hooks::Install();

        // Tell the starter (launcher or proxy) that the game may continue.
        // Signalled on failure too, so nobody waits for nothing.
        SignalReady();
        LogExeBuild();

        if (!installed) {
            Logger::Error("Hook installation failed; exiting.");
            Logger::Shutdown();
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
