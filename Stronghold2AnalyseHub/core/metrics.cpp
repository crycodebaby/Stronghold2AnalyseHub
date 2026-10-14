#include "metrics.h"
#include "logger.h"

// -----------------------------------------------------------------------------
// Implementation notes:
// - All D3D9 calls of this game are assumed to come from one render thread.
//   Counters are plain integers; control requests from the hotkey thread go
//   through atomics and are applied at the next frame boundary.
// - CSV rows are buffered in memory and written about once per second to keep
//   file I/O out of the measured frame times as far as possible.
// - Thread CPU sampling (once per second) answers "which thread is busy?"
//   without touching game code.
// -----------------------------------------------------------------------------

const double Metrics::kBucketUpperMs[Metrics::kBucketCount - 1] =
    { 8.33, 16.67, 33.33, 50.0, 100.0, 250.0, 500.0, 1000.0 };

namespace
{
    constexpr int kHistory = 600;

    LARGE_INTEGER g_freq{};
    Metrics::FrameCounters g_current;
    Metrics::Snapshot      g_last;
    std::array<uint64_t, Metrics::kBucketCount> g_histogram{};
    float g_history[kHistory]{};
    int   g_historyCount = 0;
    int   g_historyOffset = 0;

    bool    g_inOverlay = false;
    bool    g_presentSeen = false;
    int64_t g_lastFrameTick = 0;
    int64_t g_lastEndSceneExit = 0;
    int     g_endScenesWithoutPresent = 0;
    uint64_t g_frameIndex = 0;

    // Accumulated per frame (EndScene may run more than once per frame).
    double g_frameEndSceneMs = 0;
    double g_frameBetweenMs = 0;
    double g_frameOverlayMs = 0;

    std::atomic<bool>     g_wantToggle{ false };
    std::atomic<bool>     g_wantReset{ false };
    std::atomic<uint32_t> g_markers{ 0 };

    bool        g_recording = false;
    FILE*       g_frameCsv = nullptr;
    FILE*       g_threadCsv = nullptr;
    std::string g_frameBuf;
    int64_t     g_lastFlushTick = 0;
    int64_t     g_recordStartTick = 0;

    // Thread sampling state: tid -> last (kernel+user) 100ns units.
    std::vector<std::pair<DWORD, uint64_t>> g_threadPrev;
    int64_t g_lastThreadSampleTick = 0;

    int64_t Now()
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return t.QuadPart;
    }

    double ToMs(int64_t ticks)
    {
        return 1000.0 * static_cast<double>(ticks) / static_cast<double>(g_freq.QuadPart);
    }

    double VaUsedMb()
    {
        MEMORYSTATUSEX ms{ sizeof(ms) };
        if (!GlobalMemoryStatusEx(&ms)) return 0;
        return static_cast<double>(ms.ullTotalVirtual - ms.ullAvailVirtual) / (1024.0 * 1024.0);
    }

    std::wstring CaptureBasePath()
    {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (wchar_t* p = wcsrchr(exePath, L'\\')) *(p + 1) = L'\0';

        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t stamp[64];
        swprintf_s(stamp, L"SH2Perf_%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);
        return std::wstring(exePath) + stamp;
    }

    void FlushFrameBuffer()
    {
        if (g_frameCsv && !g_frameBuf.empty()) {
            fwrite(g_frameBuf.data(), 1, g_frameBuf.size(), g_frameCsv);
            fflush(g_frameCsv);
        }
        g_frameBuf.clear();
    }

    void StopRecording()
    {
        if (!g_recording) return;
        FlushFrameBuffer();
        if (g_frameCsv) fclose(g_frameCsv);
        if (g_threadCsv) fclose(g_threadCsv);
        g_frameCsv = g_threadCsv = nullptr;
        g_recording = false;
        Logger::Info("Recording stopped at frame %llu.", g_frameIndex);
    }

    void StartRecording()
    {
        const std::wstring base = CaptureBasePath();
        _wfopen_s(&g_frameCsv, (base + L"_frames.csv").c_str(), L"wb");
        _wfopen_s(&g_threadCsv, (base + L"_threads.csv").c_str(), L"wb");
        if (!g_frameCsv || !g_threadCsv) {
            Logger::Error("Could not open capture CSV files.");
            StopRecording();
            return;
        }
        fputs("frame,t_s,frame_ms,present_ms,endscene_ms,between_endscene_ms,overlay_ms,"
              "scenes,dip,dp,dpup,dipup,prims,settex,vsconst_calls,vsconst_vec4,"
              "psconst_calls,psconst_vec4,va_used_mb,marker\n", g_frameCsv);
        fputs("t_s,tid,cpu_pct,is_render_thread\n", g_threadCsv);
        g_recordStartTick = Now();
        g_lastFlushTick = g_recordStartTick;
        g_recording = true;
        Logger::Info("Recording started at frame %llu.", g_frameIndex);
    }

    void SampleThreads(int64_t now)
    {
        const double intervalMs = ToMs(now - g_lastThreadSampleTick);
        g_lastThreadSampleTick = now;

        std::vector<std::pair<DWORD, uint64_t>> current;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return;

        const DWORD pid = GetCurrentProcessId();
        THREADENTRY32 te{ sizeof(te) };
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            FILETIME c, e, k, u;
            if (GetThreadTimes(h, &c, &e, &k, &u)) {
                const uint64_t kt = (static_cast<uint64_t>(k.dwHighDateTime) << 32) | k.dwLowDateTime;
                const uint64_t ut = (static_cast<uint64_t>(u.dwHighDateTime) << 32) | u.dwLowDateTime;
                current.emplace_back(te.th32ThreadID, kt + ut);
            }
            CloseHandle(h);
        }
        CloseHandle(snap);

        if (g_recording && g_threadCsv && intervalMs > 0) {
            const double tS = ToMs(now - g_recordStartTick) / 1000.0;
            for (const auto& [tid, total] : current) {
                for (const auto& [ptid, ptotal] : g_threadPrev) {
                    if (ptid != tid) continue;
                    // 100ns units -> ms, relative to wall-clock interval.
                    const double pct = 100.0 * (static_cast<double>(total - ptotal) / 10000.0) / intervalMs;
                    if (pct >= 0.5) {
                        fprintf(g_threadCsv, "%.3f,%lu,%.1f,%d\n", tS, tid, pct,
                            tid == g_last.renderTid ? 1 : 0);
                    }
                    break;
                }
            }
            fflush(g_threadCsv);
        }
        g_threadPrev = std::move(current);
    }

    // Closes the current frame: snapshot, histogram, CSV row, reset counters.
    void EndFrame(int64_t now, double presentMs)
    {
        if (g_lastFrameTick != 0) {
            Metrics::Snapshot s;
            s.frameIndex = g_frameIndex;
            s.counters = g_current;
            s.times.frameMs = ToMs(now - g_lastFrameTick);
            s.times.presentMs = presentMs;
            s.times.endSceneMs = g_frameEndSceneMs;
            s.times.betweenEndSceneMs = g_frameBetweenMs;
            s.times.overlayMs = g_frameOverlayMs;
            s.renderTid = GetCurrentThreadId();
            s.vaUsedMb = (g_frameIndex % 30 == 0) ? VaUsedMb() : g_last.vaUsedMb;
            g_last = s;

            int bucket = Metrics::kBucketCount - 1;
            for (int i = 0; i < Metrics::kBucketCount - 1; ++i) {
                if (s.times.frameMs < Metrics::kBucketUpperMs[i]) { bucket = i; break; }
            }
            ++g_histogram[bucket];

            g_history[g_historyOffset] = static_cast<float>(s.times.frameMs);
            g_historyOffset = (g_historyOffset + 1) % kHistory;
            if (g_historyCount < kHistory) ++g_historyCount;

            if (g_recording) {
                const auto& c = s.counters;
                char row[512];
                snprintf(row, sizeof(row),
                    "%llu,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%u,%llu,%u,%u,%u,%u,%u,%.0f,%u\n",
                    s.frameIndex, ToMs(now - g_recordStartTick) / 1000.0,
                    s.times.frameMs, s.times.presentMs, s.times.endSceneMs, s.times.betweenEndSceneMs,
                    s.times.overlayMs, c.scenes, c.drawIndexed, c.draw, c.drawUP, c.drawIndexedUP,
                    c.primitives, c.setTexture, c.vsConstCalls, c.vsConstVec4, c.psConstCalls,
                    c.psConstVec4, s.vaUsedMb, g_markers.load());
                g_frameBuf += row;
                if (ToMs(now - g_lastFlushTick) > 1000.0) {
                    FlushFrameBuffer();
                    g_lastFlushTick = now;
                }
            }
        }

        if (ToMs(now - g_lastThreadSampleTick) > 1000.0) SampleThreads(now);

        // Apply control requests at the frame boundary.
        if (g_wantReset.exchange(false)) {
            g_histogram.fill(0);
            Logger::Info("Histogram reset at frame %llu.", g_frameIndex);
        }
        if (g_wantToggle.exchange(false)) {
            if (g_recording) StopRecording(); else StartRecording();
        }

        g_current = {};
        g_frameEndSceneMs = g_frameBetweenMs = g_frameOverlayMs = 0;
        g_lastFrameTick = now;
        ++g_frameIndex;
    }
}

Metrics::FrameCounters& Metrics::Current() { return g_current; }
bool Metrics::InOverlay() { return g_inOverlay; }
void Metrics::SetInOverlay(bool v) { g_inOverlay = v; }

void Metrics::OnEndSceneEnter()
{
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    const int64_t now = Now();
    if (g_lastEndSceneExit != 0) g_frameBetweenMs += ToMs(now - g_lastEndSceneExit);
}

void Metrics::OnEndSceneExit(double origMs, double overlayMs)
{
    g_frameEndSceneMs += origMs;
    g_frameOverlayMs += overlayMs;
    const int64_t now = Now();
    g_lastEndSceneExit = now;

    // Fallback frame boundary if Present is never called on the device.
    if (!g_presentSeen && ++g_endScenesWithoutPresent == 120) {
        Logger::Warn("No IDirect3DDevice9::Present seen after 120 EndScenes; using EndScene as frame boundary.");
    }
    if (!g_presentSeen && g_endScenesWithoutPresent >= 120) EndFrame(now, 0.0);
}

void Metrics::OnPresent(double origMs)
{
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    g_presentSeen = true;
    EndFrame(Now(), origMs);
}

const Metrics::Snapshot& Metrics::Last() { return g_last; }
const std::array<uint64_t, Metrics::kBucketCount>& Metrics::Histogram() { return g_histogram; }

const float* Metrics::FrameHistory(int& count, int& offset)
{
    count = g_historyCount;
    offset = g_historyCount < kHistory ? 0 : g_historyOffset;
    return g_history;
}

bool Metrics::Recording() { return g_recording; }
uint32_t Metrics::Markers() { return g_markers.load(); }

void Metrics::ToggleRecording() { g_wantToggle.store(true); }
void Metrics::ResetHistogram() { g_wantReset.store(true); }

void Metrics::AddMarker()
{
    const uint32_t m = ++g_markers;
    Logger::Info("Marker %u set.", m);
}

void Metrics::Shutdown() { StopRecording(); }
