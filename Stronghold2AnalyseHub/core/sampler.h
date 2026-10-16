#pragma once
#include "../pch.h"

// Sampling profiler for one thread (the render thread, where the frame is built).
// Read-only: every few ms the thread is suspended for a few microseconds, its
// instruction pointer is read, and the thread is resumed. Nothing in the game
// changes (SH2-Perf docs/design/SAMPLING_PROFILER.md, D005).
//
// Modes (env SH2HUB_SAMPLE, or `sample=` in SH2Proxy.ini):
//   off       default, no thread, no cost
//   module    counts samples per (marker, module); rva = 0
//   function  counts samples per (marker, module, rva)
// SH2HUB_SAMPLE_MS = sampling interval in ms (default 10, range 1..50).
//
// Output at recording stop: <stem>_samples.csv with columns marker,module,rva,count.
namespace Sampler
{
    enum class Mode { Off, Module, Function };

    // Reads the configuration and starts the sampler thread if the mode is not Off.
    void Init();
    // Stops the sampler thread; true = it has ended. false = still running after `timeoutMs`: its handle stays open and the
    // DLL must not be unloaded; call again later.
    bool Shutdown(DWORD timeoutMs = 2000);
    bool ThreadRunning();
    Mode GetMode();

    // Thread to sample (cheap; called once per frame by the render thread).
    void SetTarget(uint32_t tid);

    // Recording control. `markerFn` returns the current marker number.
    // End() writes the CSV and returns true if a file was written.
    void Begin(const std::wstring& csvPath, uint32_t (*markerFn)());
    bool End();

    // Statistics of the last finished recording (also logged).
    struct Stats { uint64_t samples = 0, failed = 0, dropped = 0; double meanSuspendUs = 0, maxSuspendUs = 0; };
    Stats LastStats();

    // Test hooks (tests/lifecycle_test.cpp).
    void Test_BlockThread(bool block);
    bool Test_ThreadAlive();
}
