#include "sampler.h"
#include "logger.h"

#include <mmsystem.h>   // timeBeginPeriod (winmm)
#include <algorithm>
#include <unordered_map>

// -----------------------------------------------------------------------------
// Implementation notes:
// - Between SuspendThread and ResumeThread the sampler touches no heap, no lock
//   and no logger: the suspended thread may hold any of them (deadlock). Only a
//   stack-local CONTEXT is filled. Everything else happens after ResumeThread.
// - EIP is mapped to a module through a table refreshed about once a second (and
//   when a sample misses it); module names are recorded as they are, grouping
//   (exe / dragonfly / driver / ...) is done by the tools.
// - A thread blocked in a system call reports the user-mode stub (ntdll/win32u):
//   waiting shows up as ntdll, not as the module that called the wait.
// -----------------------------------------------------------------------------

namespace
{
    struct ModuleRange { uintptr_t base, end; std::string name; };

    Sampler::Mode g_mode = Sampler::Mode::Off;
    DWORD         g_intervalMs = 10;     // ~100 Hz; see docs/design/SAMPLING_PROFILER.md for the overhead
    HANDLE        g_thread = nullptr;
    std::atomic<bool>     g_running{ false };
    std::atomic<bool>     g_active{ false };
    std::atomic<uint32_t> g_target{ 0 };

    // Guards the state shared between Begin/End (recording thread) and the sampler thread.
    std::mutex g_mutex;
    std::wstring g_csvPath;
    uint32_t (*g_markerFn)() = nullptr;
    std::unordered_map<uint64_t, uint64_t> g_counts;   // key = marker<<48 | name<<32 | rva
    std::vector<ModuleRange> g_modules;
    std::vector<std::string> g_names;                  // interned module names
    Sampler::Stats g_stats;
    Sampler::Stats g_lastStats;
    double g_sumSuspendUs = 0;
    LARGE_INTEGER g_freq{};

    constexpr size_t   kMaxKeys = 500000;
    constexpr uint16_t kUnknownModule = 0xFFFF;

    // Caller holds g_mutex.
    void RefreshModules()
    {
        HMODULE mods[1024];
        DWORD need = 0;
        if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
        std::vector<ModuleRange> fresh;
        const size_t n = std::min<size_t>(need / sizeof(HMODULE), 1024);
        for (size_t i = 0; i < n; ++i) {
            MODULEINFO mi{};
            char name[MAX_PATH]{};
            if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
            if (!GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, MAX_PATH)) continue;
            const uintptr_t b = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
            fresh.push_back({ b, b + mi.SizeOfImage, name });
        }
        g_modules = std::move(fresh);
    }

    int FindModule(uintptr_t eip)
    {
        for (size_t i = 0; i < g_modules.size(); ++i)
            if (eip >= g_modules[i].base && eip < g_modules[i].end) return static_cast<int>(i);
        return -1;
    }

    uint16_t InternName(const std::string& n)
    {
        for (size_t i = 0; i < g_names.size(); ++i)
            if (g_names[i] == n) return static_cast<uint16_t>(i);
        g_names.push_back(n);
        return static_cast<uint16_t>(g_names.size() - 1);
    }

    void Record(uintptr_t eip, double suspendUs, bool ok)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active.load()) return;
        if (!ok) { ++g_stats.failed; return; }
        ++g_stats.samples;
        g_sumSuspendUs += suspendUs;
        g_stats.maxSuspendUs = std::max(g_stats.maxSuspendUs, suspendUs);

        int mi = FindModule(eip);
        if (mi < 0) {                       // DLL loaded since the last refresh?
            RefreshModules();
            mi = FindModule(eip);
        }
        uint16_t nameId = kUnknownModule;
        uint32_t rva = static_cast<uint32_t>(eip);
        if (mi >= 0) {
            nameId = InternName(g_modules[mi].name);
            rva = static_cast<uint32_t>(eip - g_modules[mi].base);
        }
        if (g_mode == Sampler::Mode::Module) rva = 0;
        const uint32_t marker = g_markerFn ? g_markerFn() : 0;
        const uint64_t key = (static_cast<uint64_t>(marker & 0xFFFF) << 48) |
                             (static_cast<uint64_t>(nameId) << 32) | rva;
        auto it = g_counts.find(key);
        if (it != g_counts.end()) ++it->second;
        else if (g_counts.size() < kMaxKeys) g_counts.emplace(key, 1);
        else ++g_stats.dropped;
    }

    DWORD WINAPI SamplerThread(LPVOID)
    {
        QueryPerformanceFrequency(&g_freq);
        timeBeginPeriod(1);
        HANDLE h = nullptr;
        DWORD openTid = 0;
        DWORD lastRefresh = 0;
        while (g_running.load()) {
            if (!g_active.load()) { Sleep(10); continue; }
            const DWORD tid = g_target.load();
            if (tid == 0) { Sleep(10); continue; }
            if (tid != openTid) {
                if (h) CloseHandle(h);
                h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
                openTid = tid;
                if (!h) Logger::Warn("Sampler: OpenThread(%lu) failed (%lu).", tid, GetLastError());
            }
            Sleep(g_intervalMs);
            if (!h || tid == GetCurrentThreadId()) continue;
            if (GetTickCount() - lastRefresh > 1000) {
                std::lock_guard<std::mutex> lock(g_mutex);
                RefreshModules();
                lastRefresh = GetTickCount();
            }

            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            bool ok = false;
            if (SuspendThread(h) != static_cast<DWORD>(-1)) {
                ok = GetThreadContext(h, &ctx) != FALSE;
                ResumeThread(h);
            }
            QueryPerformanceCounter(&t1);
            // ---- thread resumed: heap, locks and logger are safe again ----
            const double us = 1e6 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(g_freq.QuadPart);
            Record(ok ? static_cast<uintptr_t>(ctx.Eip) : 0, us, ok);
        }
        if (h) CloseHandle(h);
        timeEndPeriod(1);
        return 0;
    }
}

void Sampler::Init()
{
    wchar_t v[32]{};
    if (GetEnvironmentVariableW(L"SH2HUB_SAMPLE", v, 32)) {
        if (_wcsicmp(v, L"module") == 0) g_mode = Mode::Module;
        else if (_wcsicmp(v, L"function") == 0) g_mode = Mode::Function;
        else if (_wcsicmp(v, L"off") != 0 && v[0]) Logger::Warn("Sampler: unknown SH2HUB_SAMPLE value, sampler stays off.");
    }
    if (GetEnvironmentVariableW(L"SH2HUB_SAMPLE_MS", v, 32)) {
        const long ms = wcstol(v, nullptr, 10);
        g_intervalMs = static_cast<DWORD>(std::min(50L, std::max(1L, ms)));
    }
    if (g_mode == Mode::Off) return;
    g_running.store(true);
    g_thread = CreateThread(nullptr, 0, SamplerThread, nullptr, 0, nullptr);
    Logger::Info("Sampler: mode=%s interval=%lu ms (render thread, read-only).",
        g_mode == Mode::Module ? "module" : "function", g_intervalMs);
}

void Sampler::Shutdown()
{
    g_active.store(false);
    g_running.store(false);
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

Sampler::Mode Sampler::GetMode() { return g_mode; }
void Sampler::SetTarget(uint32_t tid) { g_target.store(tid); }

void Sampler::Begin(const std::wstring& csvPath, uint32_t (*markerFn)())
{
    if (g_mode == Mode::Off || !g_thread) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_csvPath = csvPath;
        g_markerFn = markerFn;
        g_counts.clear();
        g_stats = {};
        g_sumSuspendUs = 0;
        RefreshModules();
    }
    g_active.store(true);
}

bool Sampler::End()
{
    if (!g_active.exchange(false)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);     // waits for a sample in flight
    g_lastStats = g_stats;
    if (g_stats.samples) g_lastStats.meanSuspendUs = g_sumSuspendUs / static_cast<double>(g_stats.samples);

    struct Row { uint32_t marker; uint16_t name; uint32_t rva; uint64_t count; };
    std::vector<Row> rows;
    rows.reserve(g_counts.size());
    for (const auto& [k, c] : g_counts)
        rows.push_back({ static_cast<uint32_t>(k >> 48), static_cast<uint16_t>((k >> 32) & 0xFFFF),
                         static_cast<uint32_t>(k & 0xFFFFFFFF), c });
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.marker != b.marker ? a.marker < b.marker : a.count > b.count; });

    FILE* f = nullptr;
    _wfopen_s(&f, g_csvPath.c_str(), L"wb");
    if (!f) { Logger::Error("Sampler: could not open samples CSV."); return false; }
    fputs("marker,module,rva,count\n", f);
    for (const Row& r : rows) {
        const char* name = r.name == kUnknownModule ? "?" : g_names[r.name].c_str();
        fprintf(f, "%u,%s,0x%08X,%llu\n", r.marker, name, r.rva, r.count);
    }
    fclose(f);
    Logger::Info("Sampler: %llu samples (%llu failed, %llu dropped), %zu rows, suspend window mean %.1f us max %.1f us.",
        g_stats.samples, g_stats.failed, g_stats.dropped, rows.size(), g_lastStats.meanSuspendUs, g_stats.maxSuspendUs);
    return true;
}

Sampler::Stats Sampler::LastStats() { return g_lastStats; }
