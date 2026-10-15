// d3d9.dll proxy: lets the game load Stronghold2AnalyseHub without an injector.
//
// Placed next to Stronghold2.exe as d3d9.dll. The game imports Direct3DCreate9
// from it; every export is passed on to the real implementation:
//   target=system          -> <system dir>\d3d9.dll (SysWOW64 for this 32-bit process)
//   target=d3d9_dxvk.dll   -> DXVK renamed, next to this proxy
// On load a starter thread loads the real d3d9 and then the hub DLL. The first
// Direct3DCreate9 call waits until the hub reports that its hook is installed,
// so the hub sees device creation without a suspended start.
//
// Configuration: SH2Proxy.ini next to this DLL (all keys optional)
//   [proxy]
//   target=system                     ; or a DLL name/path, e.g. d3d9_dxvk.dll
//   hub=1                             ; 0 = plain pass-through, hub not loaded
//   hubdll=Stronghold2AnalyseHub.dll  ; relative to this DLL's folder
//   out=SH2Perf_out                   ; hub output dir, relative to this DLL's folder
//   sample=off                        ; sampling profiler: off | module | function (hub only)
// Environment overrides: SH2HUB_DISABLE=1 (hub off), SH2HUB_OUT=<dir> (output dir).
// Diagnostics: <out>\SH2Proxy.log (rewritten on every start) and OutputDebugString.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>

// Resolved export addresses of the real d3d9, indexed like kExports.
// File scope (not in the namespace) so the naked stubs can address it from asm.
static FARPROC g_procs[17];

namespace
{
    // Same order as the stubs below; names as exported by the system d3d9.dll and DXVK.
    const char* const kExports[] = {
        "D3DPERF_BeginEvent", "D3DPERF_EndEvent", "D3DPERF_GetStatus", "D3DPERF_QueryRepeatFrame",
        "D3DPERF_SetMarker", "D3DPERF_SetOptions", "D3DPERF_SetRegion", "DebugSetLevel", "DebugSetMute",
        "Direct3D9EnableMaximizedWindowedModeShim", "Direct3DCreate9", "Direct3DCreate9Ex",
        "Direct3DCreate9On12", "Direct3DCreate9On12Ex", "Direct3DShaderValidatorCreate9",
        "PSGPError", "PSGPSampleTexture",
    };
    constexpr int kExportCount = sizeof(kExports) / sizeof(kExports[0]);
    static_assert(kExportCount == sizeof(g_procs) / sizeof(g_procs[0]), "g_procs size");
    constexpr int kDirect3DCreate9 = 10;
    constexpr DWORD kHubWaitMs = 10000;

    HMODULE   g_self = nullptr;
    HMODULE   g_real = nullptr;
    HANDLE    g_hubDone = nullptr;      // set once the hub is ready, failed or disabled
    HANDLE    g_log = INVALID_HANDLE_VALUE;
    INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;
    wchar_t   g_dir[MAX_PATH];          // folder of this DLL, no trailing backslash
    wchar_t   g_ini[MAX_PATH];

    void Log(const wchar_t* fmt, ...)
    {
        wchar_t line[1024];
        int n = swprintf_s(line, L"SH2Proxy: ");
        va_list ap;
        va_start(ap, fmt);
        vswprintf_s(line + n, sizeof(line) / sizeof(line[0]) - n - 1, fmt, ap);
        va_end(ap);
        wcscat_s(line, L"\n");
        OutputDebugStringW(line);
        if (g_log != INVALID_HANDLE_VALUE) {
            char utf8[3072];
            const int len = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
            DWORD written = 0;
            if (len > 1) WriteFile(g_log, utf8, len - 1, &written, nullptr);
        }
    }

    // Relative paths are taken relative to the proxy's folder.
    void Resolve(const wchar_t* value, wchar_t* out)
    {
        wchar_t joined[MAX_PATH];
        const bool absolute = value[0] == L'\\' || (value[0] && value[1] == L':');
        if (absolute) wcscpy_s(joined, value);
        else swprintf_s(joined, L"%s\\%s", g_dir, value);
        GetFullPathNameW(joined, MAX_PATH, out, nullptr);
    }

    void Fatal(const wchar_t* message)
    {
        Log(L"FATAL: %s", message);
        MessageBoxW(nullptr, message, L"SH2 d3d9 proxy", MB_ICONERROR | MB_OK);
        ExitProcess(1);
    }

    // Reads the config, opens the log, loads the real d3d9 and resolves all exports.
    BOOL CALLBACK InitProxy(PINIT_ONCE, PVOID, PVOID*)
    {
        GetModuleFileNameW(g_self, g_dir, MAX_PATH);
        if (wchar_t* slash = wcsrchr(g_dir, L'\\')) *slash = L'\0';
        swprintf_s(g_ini, L"%s\\SH2Proxy.ini", g_dir);

        // Output dir for the hub (Logger::OutputDir reads SH2HUB_OUT) and our log.
        wchar_t out[MAX_PATH];
        if (!GetEnvironmentVariableW(L"SH2HUB_OUT", out, MAX_PATH)) {
            wchar_t value[MAX_PATH];
            GetPrivateProfileStringW(L"proxy", L"out", L"SH2Perf_out", value, MAX_PATH, g_ini);
            Resolve(value, out);
            SetEnvironmentVariableW(L"SH2HUB_OUT", out);
        }
        CreateDirectoryW(out, nullptr);
        // Sampling profiler mode for the hub (off | module | function); env wins over the ini.
        wchar_t sample[16];
        if (!GetEnvironmentVariableW(L"SH2HUB_SAMPLE", sample, 16)) {
            GetPrivateProfileStringW(L"proxy", L"sample", L"off", sample, 16, g_ini);
            SetEnvironmentVariableW(L"SH2HUB_SAMPLE", sample);
        }
        wchar_t logPath[MAX_PATH];
        swprintf_s(logPath, L"%s\\SH2Proxy.log", out);
        g_log = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        Log(L"config %s (%s)", g_ini,
            GetFileAttributesW(g_ini) == INVALID_FILE_ATTRIBUTES ? L"missing, defaults" : L"found");
        Log(L"output dir %s", out);

        wchar_t target[MAX_PATH], realPath[MAX_PATH], selfPath[MAX_PATH];
        GetPrivateProfileStringW(L"proxy", L"target", L"system", target, MAX_PATH, g_ini);
        if (_wcsicmp(target, L"system") == 0) {
            wchar_t sys[MAX_PATH];
            GetSystemDirectoryW(sys, MAX_PATH); // WOW64 redirects System32 -> SysWOW64
            swprintf_s(realPath, L"%s\\d3d9.dll", sys);
        }
        else {
            Resolve(target, realPath);
        }
        GetModuleFileNameW(g_self, selfPath, MAX_PATH);
        if (_wcsicmp(realPath, selfPath) == 0)
            Fatal(L"SH2Proxy.ini target points at the proxy itself. Use target=system or e.g. d3d9_dxvk.dll.");

        g_real = LoadLibraryW(realPath);
        if (!g_real) {
            wchar_t msg[MAX_PATH + 160];
            swprintf_s(msg, L"Cannot load the real d3d9 '%s' (error %lu). Check target= in SH2Proxy.ini.",
                realPath, GetLastError());
            Fatal(msg);
        }
        for (int i = 0; i < kExportCount; ++i)
            g_procs[i] = GetProcAddress(g_real, kExports[i]);
        Log(L"target=%s -> loaded %s", target, realPath);
        return TRUE;
    }

    DWORD WINAPI StarterThread(LPVOID)
    {
        InitOnceExecuteOnce(&g_initOnce, InitProxy, nullptr, nullptr);

        wchar_t disable[8]{};
        const bool envOff = GetEnvironmentVariableW(L"SH2HUB_DISABLE", disable, 8) && disable[0] == L'1';
        const bool iniOn = GetPrivateProfileIntW(L"proxy", L"hub", 1, g_ini) != 0;
        if (envOff || !iniOn) {
            Log(L"hub disabled (%s); pass-through only", envOff ? L"SH2HUB_DISABLE=1" : L"hub=0");
            SetEvent(g_hubDone);
            return 0;
        }

        wchar_t value[MAX_PATH], hubPath[MAX_PATH];
        GetPrivateProfileStringW(L"proxy", L"hubdll", L"Stronghold2AnalyseHub.dll", value, MAX_PATH, g_ini);
        Resolve(value, hubPath);

        // The hub signals this event when its Direct3DCreate9 hook is in place.
        wchar_t eventName[64];
        swprintf_s(eventName, L"Local\\SH2AnalyseHub.Ready.%lu", GetCurrentProcessId());
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName);

        if (!LoadLibraryW(hubPath)) {
            Log(L"hub NOT loaded: LoadLibraryW(%s) failed (error %lu); game runs without hub",
                hubPath, GetLastError());
        }
        else if (WaitForSingleObject(ready, kHubWaitMs) == WAIT_OBJECT_0) {
            Log(L"hub loaded and ready: %s", hubPath);
        }
        else {
            Log(L"hub loaded but no ready signal within %lu ms: %s", kHubWaitMs, hubPath);
        }
        if (ready) CloseHandle(ready);
        SetEvent(g_hubDone);
        return 0;
    }
}

extern "C" void EnsureReal()
{
    InitOnceExecuteOnce(&g_initOnce, InitProxy, nullptr, nullptr);
}

// Stubs: make sure the real DLL is loaded, then jump to its export with the
// caller's stack untouched (works for any signature/calling convention).
#define PROXY_STUB(index, name)                                         \
    extern "C" __declspec(naked) void Stub_##name()                     \
    {                                                                   \
        __asm pushad                                                    \
        __asm call EnsureReal                                           \
        __asm popad                                                     \
        __asm jmp dword ptr [g_procs + (index) * 4]                     \
    }

PROXY_STUB(0, D3DPERF_BeginEvent)
PROXY_STUB(1, D3DPERF_EndEvent)
PROXY_STUB(2, D3DPERF_GetStatus)
PROXY_STUB(3, D3DPERF_QueryRepeatFrame)
PROXY_STUB(4, D3DPERF_SetMarker)
PROXY_STUB(5, D3DPERF_SetOptions)
PROXY_STUB(6, D3DPERF_SetRegion)
PROXY_STUB(7, DebugSetLevel)
PROXY_STUB(8, DebugSetMute)
PROXY_STUB(9, Direct3D9EnableMaximizedWindowedModeShim)
PROXY_STUB(11, Direct3DCreate9Ex)
PROXY_STUB(12, Direct3DCreate9On12)
PROXY_STUB(13, Direct3DCreate9On12Ex)
PROXY_STUB(14, Direct3DShaderValidatorCreate9)
PROXY_STUB(15, PSGPError)
PROXY_STUB(16, PSGPSampleTexture)

// The one export the game uses: wait for the hub before creating the D3D object.
extern "C" void* WINAPI Proxy_Direct3DCreate9(UINT sdkVersion)
{
    EnsureReal();
    // Which module asks (the game exe and dxrenderer.dll both import it)? Windows
    // compatibility shims (apphelp.dll / AcLayers.dll) may sit in between, so log
    // the chain of distinct calling modules, innermost first.
    void* frames[12];
    const USHORT n = RtlCaptureStackBackTrace(0, 12, frames, nullptr);
    wchar_t chain[512] = L"";
    HMODULE last = g_self;
    int listed = 0;
    for (USHORT i = 0; i < n && listed < 4; ++i) {
        HMODULE m = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(frames[i]), &m) || m == last)
            continue;
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        if (listed++) wcscat_s(chain, L" <- ");
        wcscat_s(chain, name ? name + 1 : path);
        last = m;
    }
    Log(L"Direct3DCreate9(SDK=%u) called from %s", sdkVersion, chain[0] ? chain : L"?");
    if (WaitForSingleObject(g_hubDone, kHubWaitMs + 5000) != WAIT_OBJECT_0)
        Log(L"Direct3DCreate9: hub start still pending, continuing without waiting");
    using Create_t = void*(WINAPI*)(UINT);
    auto create = reinterpret_cast<Create_t>(g_procs[kDirect3DCreate9]);
    return create ? create(sdkVersion) : nullptr;
}

// Lets the hub find the real implementation (two modules named d3d9.dll are loaded).
// Exported name doubles as the proxy marker checked by SH2-Perf tools/variant.py.
extern "C" HMODULE WINAPI SH2Proxy_RealModule()
{
    EnsureReal();
    return g_real;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        // Loader lock is held here: no LoadLibrary, just start a thread that does it.
        g_hubDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (HANDLE t = CreateThread(nullptr, 0, StarterThread, nullptr, 0, nullptr)) CloseHandle(t);
        else SetEvent(g_hubDone);
    }
    return TRUE;
}
