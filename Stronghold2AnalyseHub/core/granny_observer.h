#pragma once
#include "../pch.h"

// Read-only observer at the Stronghold2.exe -> granny2.dll import boundary (SH2-Perf EXP-008, data contract
// docs/FORMATS/GRANNY_OBSERVER.md). Switch `granny` (env SH2HUB_GRANNY, `granny=` in SH2Proxy.ini), default off:
// off = no import slot is touched.
//
// On: six import address table slots of the game exe are pointed at stdcall thunks with the verified signatures
// (SH2-Perf F137). Every thunk calls the original exactly once with the unchanged arguments and returns its result.
// Only while a recording runs, and only on the render thread, the thunk updates preallocated tables (no file, no heap,
// no lock, no wait): per (object, marker) aggregates and one row per frame. Calls on other threads are only counted.
// Calls inside granny2.dll and calls through other modules are not seen. Pointers are object ids within one session.
namespace GrannyObserver
{
    // Signatures as verified in granny2.dll (ret N) and at the exe call sites; argument names follow the Granny API.
    // The clock is a float32 on the stack; it is taken and passed on as its raw 32 bits (bit-exact, no FPU load/store).
    using SetModelClock_t = void(__stdcall*)(void* modelInstance, uint32_t clockBits);
    using SampleAccel_t = void(__stdcall*)(void* modelInstance, int32_t boneCount, const float* offset4x4,
                                           void* localPose, void* worldPose);
    using DeformVertices_t = void(__stdcall*)(void* deformer, const int32_t* matrixIndices, const float* matrixBuffer4x4,
                                              int32_t vertexCount, const void* sourceVertices, void* destVertices);
    using Composite_t = void*(__stdcall*)(void* worldPose);
    using Free_t = void(__stdcall*)(void* object);

    enum Fn : int { kClock = 0, kSample, kDeform, kComposite, kFreeInstance, kFreeDeformer, kFnCount };

    // Reads the switch (Config must be initialised). True = the observer is wanted.
    bool Enabled();
    // `frameFn` = index of the frame being built (same numbering as frames.csv), `markerFn` = current F8 marker.
    // Patches the import slots of `exe` (default: the process exe) if the switch is on. Logs every slot.
    bool Install(uint64_t (*frameFn)(), uint32_t (*markerFn)(), HMODULE exe = nullptr);
    // Restores every slot that still points at our thunk. The thunks stay in the (resident) DLL.
    void Uninstall();
    bool Installed();
    int SlotsPatched();

    // Render thread: the only thread whose calls are aggregated; called when it changes.
    void SetOwnerTid(uint32_t tid);
    // Render thread, at the end of frame `frame` (before the frame index advances).
    void OnFrameEnd(uint64_t frame, uint32_t marker);

    // Capture thread: start/stop of a recording. Begin allocates the tables (VirtualAlloc), End stops aggregation, waits
    // until no thunk is inside its bookkeeping, writes <base>_granny.json, _granny_frames.csv, _granny_objects.csv and
    // frees the tables. End returns true if all three files were written.
    bool Begin(const std::wstring& base);
    bool End();
    bool WroteFiles();                                  // last End() wrote the files

    struct Totals
    {
        uint64_t calls[kFnCount]{};                     // aggregated (owner thread, while recording)
        uint64_t foreign[kFnCount]{};                   // other threads (counted only)
        uint64_t tableDrops = 0, framesDropped = 0, frames = 0;
        uint32_t firstForeignTid = 0;
    };
    Totals LastTotals();

    // Capacities (fixed; overflow is counted and reported, never waited for).
    // Historical pointer identities, not live instances. Keep freed rows as evidence rather than silently evicting them.
    constexpr uint32_t kInstanceSlots = 8192;
    constexpr uint32_t kDeformerSlots = 8192;
    constexpr uint32_t kPoseSlots = 4096;
    constexpr uint32_t kFrameRows = 8192;
    constexpr uint32_t kMarkerSlots = 5;                // markers 0..3, 4 = marker >= 4

    // Test hooks (tests/granny_observer_test.cpp).
    void Test_ForceEnabled(bool on);
    // Address of the import slot of `fn` in `exe` (resolved without patching; call before Install). nullptr = not found.
    void** Test_FindSlot(HMODULE exe, int fn);
}
