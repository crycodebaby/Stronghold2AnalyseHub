#include "metrics.h"
#include "capture.h"
#include "logger.h"
#include "sampler.h"
#include "granny_observer.h"

// -----------------------------------------------------------------------------
// Implementation notes:
// - All D3D9 calls of this game are assumed to come from one render thread.
//   Counters are plain integers; control requests from the hotkey thread go
//   through atomics and are applied at the next frame boundary.
// - The render thread does no file I/O, no formatting, no system queries and no
//   waiting here: a finished frame is copied into the capture ring (capture.h) and
//   everything else (CSV, thread CPU, address space, files) happens on the capture thread.
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
    uint32_t g_lastRenderTid = 0;
    std::atomic<uint64_t> g_frameIndexShared{ 0 };

    // Accumulated per frame (EndScene may run more than once per frame).
    double g_frameEndSceneMs = 0;
    double g_frameBetweenMs = 0;
    double g_frameOverlayMs = 0;

    std::atomic<bool>     g_wantReset{ false };
    std::atomic<uint32_t> g_markers{ 0 };

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

    // Closes the current frame: snapshot, histogram, hand-over to the capture ring, reset counters.
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
            if (s.renderTid != g_lastRenderTid) {
                g_lastRenderTid = s.renderTid;
                Sampler::SetTarget(s.renderTid);
                Capture::SetRenderTid(s.renderTid);
                GrannyObserver::SetOwnerTid(s.renderTid);
            }
            s.vaUsedMb = Capture::VaUsedMb();          // last value polled by the capture thread
            g_last = s;

            int bucket = Metrics::kBucketCount - 1;
            for (int i = 0; i < Metrics::kBucketCount - 1; ++i) {
                if (s.times.frameMs < Metrics::kBucketUpperMs[i]) { bucket = i; break; }
            }
            ++g_histogram[bucket];

            g_history[g_historyOffset] = static_cast<float>(s.times.frameMs);
            g_historyOffset = (g_historyOffset + 1) % kHistory;
            if (g_historyCount < kHistory) ++g_historyCount;

            if (const uint32_t gen = Capture::ActiveGen()) {
                Capture::FrameRecord r;
                r.frame = s.frameIndex;
                r.tick = now;
                r.gen = gen;
                r.marker = g_markers.load(std::memory_order_relaxed);
                r.frameMs = static_cast<float>(s.times.frameMs);
                r.presentMs = static_cast<float>(s.times.presentMs);
                r.endSceneMs = static_cast<float>(s.times.endSceneMs);
                r.betweenEndSceneMs = static_cast<float>(s.times.betweenEndSceneMs);
                r.overlayMs = static_cast<float>(s.times.overlayMs);
                r.vaUsedMb = Capture::VaUsedMb();
                r.counters = s.counters;
                Capture::Push(r);                      // wait-free; counts a drop if the ring is full
            }
        }

        // Apply control requests at the frame boundary.
        if (g_wantReset.exchange(false)) {
            g_histogram.fill(0);
            Logger::Info("Histogram reset at frame %llu.", g_frameIndex);
        }

        GrannyObserver::OnFrameEnd(g_frameIndex, g_markers.load(std::memory_order_relaxed));   // no-op unless observing
        g_current = {};
        g_frameEndSceneMs = g_frameBetweenMs = g_frameOverlayMs = 0;
        g_lastFrameTick = now;
        ++g_frameIndex;
        g_frameIndexShared.store(g_frameIndex, std::memory_order_relaxed);
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

bool Metrics::Recording() { return Capture::Recording(); }
uint32_t Metrics::Markers() { return g_markers.load(); }
uint64_t Metrics::FrameIndex() { return g_frameIndexShared.load(std::memory_order_relaxed); }

void Metrics::ToggleRecording() { Capture::RequestToggle(); }
void Metrics::ResetHistogram() { g_wantReset.store(true); }

void Metrics::AddMarker()
{
    const uint32_t m = ++g_markers;
    Logger::Info("Marker %u set.", m);
}

void Metrics::Shutdown() { Capture::Shutdown(); }
