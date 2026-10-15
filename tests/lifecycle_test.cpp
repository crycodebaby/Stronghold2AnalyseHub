// Background-thread lifecycle without the game (SH2-Perf D012, work order "Schutzluecken schliessen", point 5).
// A DLL may only be unloaded when none of its threads can still run its code. Checked here:
//   A. capture writer blocked inside a file write: Shutdown times out, reports "still running", releases nothing (wake event
//      stays valid), a second attempt still reports it; after release the thread ends, the recording is finalized and
//      the module can be started again
//   B. sampler thread that does not come back: same contract
//   C. hashing that ignores the stop request: reported as running, ends after release
//   D. hashing that honours the stop request: Shutdown returns quickly, identity marked cancelled
//   E. the unload order (Lifecycle::StopBackground) with hashing still running: capture is finalized, the result names
//      the hashing thread, and the whole set reports "all ended" only after it ends
// Exit code 0 = all checks passed. Temp output dir (SH2HUB_OUT).
#include "../Stronghold2AnalyseHub/core/capture.h"
#include "../Stronghold2AnalyseHub/core/config.h"
#include "../Stronghold2AnalyseHub/core/lifecycle.h"
#include "../Stronghold2AnalyseHub/core/logger.h"
#include "../Stronghold2AnalyseHub/core/provenance.h"
#include "../Stronghold2AnalyseHub/core/sampler.h"

#include <string>

namespace
{
    int g_failures = 0;
    std::wstring g_dir;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_failures; printf("FAIL line %d: %s  ", __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

    uint32_t Marker() { return 0; }
    bool WaitFor(bool (*pred)(), DWORD timeoutMs = 5000)
    {
        const DWORD t0 = GetTickCount();
        while (!pred()) { if (GetTickCount() - t0 > timeoutMs) return false; Sleep(5); }
        return true;
    }
    bool IsRecording() { return Capture::Recording(); }
    bool NotRecording() { return !Capture::Recording(); }

    std::string ReadFileText(const std::wstring& path)
    {
        std::string out;
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"rb");
        if (!f) return out;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        fclose(f);
        return out;
    }

    std::wstring NewestManifest()
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((g_dir + L"SH2Perf_*_manifest.json").c_str(), &fd);
        std::wstring best;
        FILETIME bt{};
        if (h != INVALID_HANDLE_VALUE) {
            do { if (CompareFileTime(&fd.ftLastWriteTime, &bt) >= 0) { bt = fd.ftLastWriteTime; best = fd.cFileName; } } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        return best.empty() ? best : g_dir + best;
    }

    void PushFrames(int n)
    {
        for (int i = 0; i < n; ++i) {
            Capture::FrameRecord r;
            r.frame = i;
            r.gen = Capture::ActiveGen();
            r.frameMs = 16.6f;
            if (r.gen) Capture::Push(r);
        }
    }
}

int main()
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_dir = std::wstring(tmp) + L"sh2_lifecycle_test_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
    CreateDirectoryW(g_dir.c_str(), nullptr);
    SetEnvironmentVariableW(L"SH2HUB_OUT", g_dir.c_str());
    SetEnvironmentVariableW(L"SH2HUB_SAMPLE", L"module");
    Logger::Init(L"lifecycle_test.log");
    Config::Init();
    Provenance::Start();
    CHECK(Provenance::Wait(30000), "identity not hashed");
    Sampler::Init();
    Capture::Init(Marker);

    // A. blocked capture writer --------------------------------------------------------------------
    {
        Capture::RequestToggle();
        CHECK(WaitFor(IsRecording), "recording did not start");
        Capture::Test_BlockWrites(true);
        PushFrames(20);
        Sleep(200);
        const DWORD t0 = GetTickCount();
        CHECK(!Capture::Shutdown(300), "A: Shutdown must report a still running thread");
        CHECK(GetTickCount() - t0 < 3000, "A: Shutdown must honour its timeout");
        CHECK(Capture::Test_ThreadAlive() && Capture::ThreadRunning(), "A: the thread is still inside our code");
        CHECK(Capture::Test_WakeHandleValid(), "A: the wake event must stay open while the thread runs");
        CHECK(!Capture::Shutdown(200), "A: a second attempt reports it again");
        CHECK(NewestManifest().empty() || ReadFileText(NewestManifest()).find("\"status\": \"recording\"") != std::string::npos,
            "A: no 'finalized' manifest while the writer is blocked");
        Capture::Test_BlockWrites(false);
        CHECK(Capture::Shutdown(10000), "A: after the release an orderly end is possible");
        CHECK(!Capture::Test_ThreadAlive() && !Capture::ThreadRunning(), "A: thread ended");
        const std::string m = ReadFileText(NewestManifest());
        CHECK(m.find("\"status\": \"finalized\"") != std::string::npos && m.find("\"stop_reason\": \"hub_shutdown\"") != std::string::npos,
            "A: the interrupted unload still finalized the recording");
        Capture::Init(Marker);                                   // the module is reusable after a completed shutdown
        Capture::RequestToggle();
        CHECK(WaitFor(IsRecording), "A: second life: recording did not start");
        Capture::RequestToggle();
        CHECK(WaitFor(NotRecording), "A: second life: stop");
        CHECK(Capture::Shutdown(5000), "A: second shutdown");
        printf("A. blocked capture writer: reported, nothing released, ended after release, recording finalized\n");
    }

    // B. sampler thread that does not come back ----------------------------------------------------
    {
        Sampler::Test_BlockThread(true);
        Sleep(100);
        CHECK(!Sampler::Shutdown(300), "B: Shutdown must report a still running thread");
        CHECK(Sampler::Test_ThreadAlive() && Sampler::ThreadRunning(), "B: thread still alive");
        Sampler::Test_BlockThread(false);
        CHECK(Sampler::Shutdown(5000), "B: ends after release");
        CHECK(!Sampler::Test_ThreadAlive(), "B: thread ended");
        printf("B. blocked sampler: reported, ended after release\n");
    }

    // C. hashing that ignores the stop request ------------------------------------------------------
    {
        CHECK(Provenance::Shutdown(5000), "C: the finished identity thread from the start of the test is joined");
        Provenance::Test_Reset();
        Provenance::Test_StallHash(1);
        Provenance::Start();
        Sleep(100);
        CHECK(Provenance::ThreadRunning(), "C: hashing is running");
        CHECK(!Provenance::Shutdown(300), "C: Shutdown must report the running hashing thread");
        CHECK(Provenance::ThreadRunning(), "C: still running");
        Provenance::Test_StallHash(0);
        CHECK(Provenance::Shutdown(10000), "C: ends once it is released");
        CHECK(!Provenance::ThreadRunning(), "C: ended");
        printf("C. stuck hashing: reported as running, ended after release\n");
    }

    // D. hashing that honours the stop request ------------------------------------------------------
    {
        CHECK(Provenance::Shutdown(5000), "D: previous thread joined");
        Provenance::Test_Reset();
        Provenance::Test_StallHash(2);
        Provenance::Start();
        Sleep(100);
        const DWORD t0 = GetTickCount();
        CHECK(Provenance::Shutdown(5000), "D: Shutdown returns once the thread saw the stop request");
        CHECK(GetTickCount() - t0 < 2000, "D: and promptly (%lu ms)", GetTickCount() - t0);
        CHECK(Provenance::Cancelled() && Provenance::Get().files.empty(), "D: a cancelled identity is empty/incomplete, never half-measured");
        printf("D. hashing with stop request: ended in %lu ms, identity marked incomplete\n", GetTickCount() - t0);
        Provenance::Test_StallHash(0);
    }

    // E. unload order with hashing still running ----------------------------------------------------
    {
        Provenance::Test_Reset();
        Provenance::Test_StallHash(1);
        Provenance::Start();
        Sleep(50);
        Capture::Init(Marker);
        Capture::RequestToggle();
        CHECK(WaitFor(IsRecording, 15000), "E: recording started (the start waits for the identity for at most 10 s)");
        PushFrames(30);
        Lifecycle::Result r = Lifecycle::StopBackground(300);
        CHECK(!r.All() && !r.provenance, "E: the result must name the running hashing thread");
        CHECK(r.capture && r.sampler, "E: the capture writer ended on its own (recording finalized) and the idle sampler counts as ended");
        CHECK(Lifecycle::Describe(r) == "provenance hashing", "E: described: %s", Lifecycle::Describe(r).c_str());
        const std::string m = ReadFileText(NewestManifest());
        CHECK(m.find("\"status\": \"finalized\"") != std::string::npos && m.find("\"pending\": true") != std::string::npos,
            "E: the recording is finalized and says the software identity was still pending");
        Provenance::Test_StallHash(0);
        r = Lifecycle::StopBackground(10000);
        CHECK(r.All(), "E: everything ended after the release: %s", Lifecycle::Describe(r).c_str());
        printf("E. unload order with running hashing: hashing named, recording finalized with identity pending, all ended after release\n");
    }

    Logger::Shutdown();
    printf(g_failures ? "lifecycle_test: %d FAILED\n" : "lifecycle_test: all checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
