#pragma once
#include "../pch.h"

// Read-only frame telemetry.
// Collects per-frame D3D9 call counters and timings, keeps a frame-time
// histogram, and optionally records CSV files next to the game executable.
// Nothing here changes what the game renders or simulates.
//
// Frame boundary = IDirect3DDevice9::Present. If the game never calls
// Present (e.g. uses a swap chain directly), EndScene is used instead.
namespace Metrics
{
    // Histogram bucket upper bounds in ms (last bucket is open-ended).
    constexpr int   kBucketCount = 9;
    extern const double kBucketUpperMs[kBucketCount - 1];

    struct FrameCounters
    {
        uint32_t scenes = 0;          // BeginScene calls
        uint32_t drawIndexed = 0;     // DrawIndexedPrimitive
        uint32_t draw = 0;            // DrawPrimitive
        uint32_t drawUP = 0;          // DrawPrimitiveUP
        uint32_t drawIndexedUP = 0;   // DrawIndexedPrimitiveUP
        uint64_t primitives = 0;      // sum of primitive counts of all draws
        uint32_t setTexture = 0;
        uint32_t vsConstCalls = 0;    // SetVertexShaderConstantF
        uint32_t vsConstVec4 = 0;     // sum of Vector4fCount
        uint32_t psConstCalls = 0;    // SetPixelShaderConstantF
        uint32_t psConstVec4 = 0;
    };

    struct FrameTimes
    {
        double frameMs = 0;           // Present-to-Present (or EndScene-to-EndScene in fallback)
        double presentMs = 0;         // time inside original Present
        double endSceneMs = 0;        // time inside original EndScene (overlay excluded)
        double betweenEndSceneMs = 0; // EndScene entry to previous EndScene exit
        double overlayMs = 0;         // time spent drawing our own overlay
    };

    struct Snapshot
    {
        uint64_t      frameIndex = 0;
        FrameCounters counters;
        FrameTimes    times;
        double        vaUsedMb = 0;   // process virtual address space in use
        uint32_t      renderTid = 0;
    };

    // Counters (called from D3D9 hooks; ignored while the overlay draws).
    FrameCounters& Current();
    bool InOverlay();
    void SetInOverlay(bool v);

    // Timing hooks.
    void OnEndSceneEnter();
    void OnEndSceneExit(double origMs, double overlayMs);
    void OnPresent(double origMs);

    // Read side for the overlay (render thread only).
    const Snapshot& Last();
    const std::array<uint64_t, kBucketCount>& Histogram();
    const float* FrameHistory(int& count, int& offset); // ring buffer of frame ms
    bool Recording();
    uint32_t Markers();
    uint64_t FrameIndex();                      // any thread: frames completed so far

    // Control (thread-safe). Recording start/stop is executed by the capture thread (capture.h),
    // the histogram reset is applied at the next frame boundary.
    void ToggleRecording();
    void ResetHistogram();
    void AddMarker();

    // Finish an open recording and stop the capture thread.
    void Shutdown();
}
