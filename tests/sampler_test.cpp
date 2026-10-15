// Validation of the sampling profiler without the game (SH2-Perf docs/design/SAMPLING_PROFILER.md,
// "Validation before trusting it", step 1): a busy loop in this exe must be attributed to this exe,
// a thread blocked in a wait must show up in the system DLLs, markers must be separated, and
// the loop must not slow down noticeably (overhead).
#include "../Stronghold2AnalyseHub/core/sampler.h"
#include "../Stronghold2AnalyseHub/core/logger.h"

#include <map>
#include <string>

namespace
{
    std::atomic<bool>     g_stop{ false };
    std::atomic<uint64_t> g_iterations{ 0 };
    std::atomic<uint32_t> g_marker{ 1 };

    DWORD WINAPI BusyThread(LPVOID)
    {
        volatile uint64_t x = 0;
        uint64_t local = 0;
        while (!g_stop.load(std::memory_order_relaxed)) {
            for (int i = 0; i < 10000; ++i) x = x * 3 + i;
            ++local;
            g_iterations.store(local, std::memory_order_relaxed);
        }
        return 0;
    }

    DWORD WINAPI WaitingThread(LPVOID)
    {
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        while (!g_stop.load()) WaitForSingleObject(ev, 50);
        CloseHandle(ev);
        return 0;
    }

    // Iterations per second of the busy loop over `seconds`.
    double Rate(double seconds)
    {
        const uint64_t a = g_iterations.load();
        Sleep(static_cast<DWORD>(seconds * 1000));
        return static_cast<double>(g_iterations.load() - a) / seconds;
    }

    // share[marker][module] = samples
    std::map<int, std::map<std::string, uint64_t>> ReadCsv(const std::wstring& path)
    {
        std::map<int, std::map<std::string, uint64_t>> out;
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"rb");
        if (!f) return out;
        char line[512];
        fgets(line, sizeof(line), f);                       // header
        while (fgets(line, sizeof(line), f)) {
            int marker = 0; char mod[260]; char rva[32]; unsigned long long count = 0;
            if (sscanf_s(line, "%d,%259[^,],%31[^,],%llu", &marker, mod, 260, rva, 32, &count) == 4)
                out[marker][mod] += count;
        }
        fclose(f);
        return out;
    }

    int g_failures = 0;
    void Check(bool ok, const char* what)
    {
        printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++g_failures;
    }
}

int main()
{
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    wcscat_s(dir, L"sh2sampler_test");
    CreateDirectoryW(dir, nullptr);
    SetEnvironmentVariableW(L"SH2HUB_OUT", dir);
    SetEnvironmentVariableW(L"SH2HUB_SAMPLE", L"function");
    wchar_t ms[8];
    if (!GetEnvironmentVariableW(L"SH2HUB_SAMPLE_MS", ms, 8)) SetEnvironmentVariableW(L"SH2HUB_SAMPLE_MS", L"10");
    Logger::Init(L"sampler_test.log");
    Sampler::Init();
    Check(Sampler::GetMode() == Sampler::Mode::Function, "mode function from environment");

    DWORD busyTid = 0, waitTid = 0;
    HANDLE busy = CreateThread(nullptr, 0, BusyThread, nullptr, 0, &busyTid);
    HANDLE wait = CreateThread(nullptr, 0, WaitingThread, nullptr, 0, &waitTid);
    Sleep(500);
    const double base = Rate(2.0);

    const std::wstring path = std::wstring(dir) + L"\\samples_busy.csv";
    Sampler::SetTarget(busyTid);
    Sampler::Begin(path, [] { return g_marker.load(); });
    const double loaded1 = Rate(1.5);
    g_marker = 2;
    const double loaded2 = Rate(1.5);
    Check(Sampler::End(), "samples file written");
    auto busyRows = ReadCsv(path);
    const Sampler::Stats st = Sampler::LastStats();
    printf("     samples %llu failed %llu, suspend window mean %.1f us max %.1f us\n",
        st.samples, st.failed, st.meanSuspendUs, st.maxSuspendUs);
    Check(st.samples > 100, "more than 100 samples in 3 s");

    std::string self;
    {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        self = strrchr(exe, '\\') + 1;
    }
    for (int m : { 1, 2 }) {
        uint64_t total = 0;
        for (auto& [mod, c] : busyRows[m]) total += c;
        const double share = total ? 100.0 * busyRows[m][self] / total : 0;
        printf("     marker %d: %llu samples, %.1f %% in %s\n", m, total, share, self.c_str());
        Check(total > 50 && share > 90.0, "busy loop attributed to this exe (> 90 %)");
    }

    const double after = Rate(2.0);                         // control: sampler off again (drift of the machine)
    const double ref = (base + after) / 2;
    const double slowdown = 100.0 * (1.0 - (loaded1 + loaded2) / 2 / ref);
    printf("     busy loop: %.0f it/s before, %.0f after (sampler off), %.0f / %.0f with -> slowdown %.1f %%\n",
        base, after, loaded1, loaded2, slowdown);
    Check(slowdown < 5.0, "overhead below 5 % (synthetic worst case: busy loop on a free core; 10 ms default)");

    const std::wstring path2 = std::wstring(dir) + L"\\samples_wait.csv";
    g_marker = 3;
    Sampler::SetTarget(waitTid);
    Sampler::Begin(path2, [] { return g_marker.load(); });
    Sleep(1500);
    Sampler::End();
    auto waitRows = ReadCsv(path2);
    uint64_t total = 0;
    for (auto& [mod, c] : waitRows[3]) total += c;
    const double selfShare = total ? 100.0 * waitRows[3][self] / total : 100.0;
    printf("     waiting thread: %llu samples, %.1f %% in %s, rest:", total, selfShare, self.c_str());
    for (auto& [mod, c] : waitRows[3]) if (mod != self) printf(" %s=%llu", mod.c_str(), c);
    printf("\n");
    Check(total > 50 && selfShare < 10.0, "waiting thread is not attributed to this exe");

    g_stop = true;
    WaitForSingleObject(busy, 2000);
    WaitForSingleObject(wait, 2000);
    Sampler::Shutdown();
    Logger::Shutdown();
    printf(g_failures ? "FAILED (%d)\n" : "ALL OK\n", g_failures);
    return g_failures ? 1 : 0;
}
