#pragma once
#include "../pch.h"
#include "metrics.h"

// Recording pipeline. The render thread only copies one fixed-size record per frame into a
// bounded single-producer/single-consumer ring (wait-free: no lock, no allocation, no file or
// system call). A separate low-priority capture thread formats CSV rows, writes the files,
// polls thread CPU / address space (poll switch) and runs the start/stop work (opening files,
// sampler CSV, latest.json). If the ring is full the record is dropped and counted; the render
// thread never waits. Overflows and write errors are logged, shown in the overlay and written
// to latest.json (frames_dropped, write_errors, capture_ok).
// CSV columns are unchanged from earlier hub builds (SH2-Perf docs/FORMATS).
// Provenance (D012): every recording has its own manifest `<stem>_manifest.json` (manifest.h). It is
// written at start with status "recording" and published as "finalized" only after all capture files
// are closed and hashed; latest.json is just a pointer to it.
namespace Capture
{
    constexpr uint32_t kRingSize = 8192;          // power of two; ~0.9 MB static, > 2 min at 60 FPS

    struct FrameRecord
    {
        uint64_t frame = 0;
        int64_t  tick = 0;                         // QueryPerformanceCounter at the end of the frame
        uint32_t gen = 0;                          // recording this record belongs to
        uint32_t marker = 0;
        float    frameMs = 0, presentMs = 0, endSceneMs = 0, betweenEndSceneMs = 0, overlayMs = 0;
        uint32_t vaUsedMb = 0;
        Metrics::FrameCounters counters;
    };

    struct Status
    {
        bool     recording = false;
        bool     openFailed = false;               // last start could not open its files
        bool     writeFailed = false;              // a write failed: rest of this recording is lost
        uint64_t pushed = 0;                       // records handed over in this recording
        uint64_t written = 0;
        uint64_t dropped = 0;                      // ring full (render thread did not wait)
        uint32_t writeErrors = 0;
        bool     manifestFailed = false;           // the manifest could not be written (start refused / final not published)
    };

    // `markerFn` returns the current F8 marker number. Starts the capture thread.
    void Init(uint32_t (*markerFn)());
    // Asks the capture thread to finish (an open recording is finalized first) and waits up to `timeoutMs` for it to END.
    // true = the thread has ended. false = it is still running (blocked in a file operation, for example): nothing it
    // uses is released, the caller must not unload the DLL and may call this again later.
    bool Shutdown(DWORD timeoutMs = 5000);
    bool ThreadRunning();

    // --- render thread ---
    uint32_t ActiveGen();                          // 0 = not recording
    void Push(const FrameRecord& record);          // wait-free; the caller sets record.gen = ActiveGen()
    void SetRenderTid(uint32_t tid);

    // --- any thread ---
    void RequestToggle();                          // F9: start or stop, executed by the capture thread
    bool Recording();
    Status GetStatus();
    uint32_t VaUsedMb();                           // last polled value (0 = not polled)

    // Test hooks (tests/capture_test.cpp).
    void Test_StallWriter(bool stall);
    void Test_BlockWrites(bool block);             // the writer sticks inside its next file write
    bool Test_ThreadAlive();                       // the capture thread function has not returned
    bool Test_WakeHandleValid();                   // the wake event handle is still open
    void Test_FailNextWrites(bool fail);
    void Test_FailManifest(bool fail);             // next manifest writes fail
    void Test_AbandonRecording();                  // behave like a killed process: no final manifest
}
