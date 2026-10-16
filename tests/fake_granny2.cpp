// Stand-in granny2.dll for tests/granny_observer_test.cpp: the six exports the observer patches, with the same
// stdcall decoration as the real granny2.dll (_Name@bytes). Records the raw arguments of every call so the test can
// check that the observer forwards each call exactly once and unchanged. Not the Granny runtime; no behaviour.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

struct FakeGrannyLog
{
    uint32_t calls[6];
    void* a[6][6];                  // last raw arguments per function (as 32-bit values)
    uint32_t clockBits;
};

static FakeGrannyLog g_log;

extern "C" __declspec(dllexport) FakeGrannyLog* __cdecl FakeGranny_Log() { return &g_log; }

extern "C" __declspec(dllexport) void __stdcall GrannySetModelClock(void* inst, uint32_t clockBits)
{
    ++g_log.calls[0]; g_log.a[0][0] = inst; g_log.clockBits = clockBits;
}
extern "C" __declspec(dllexport) void __stdcall GrannySampleModelAnimationsAccelerated(void* inst, int32_t bones, const float* offset,
                                                                                         void* local, void* world)
{
    ++g_log.calls[1]; g_log.a[1][0] = inst; g_log.a[1][1] = reinterpret_cast<void*>(static_cast<intptr_t>(bones));
    g_log.a[1][2] = const_cast<float*>(offset); g_log.a[1][3] = local; g_log.a[1][4] = world;
}
extern "C" __declspec(dllexport) void __stdcall GrannyDeformVertices(void* deformer, const int32_t* indices, const float* matrix,
                                                                     int32_t count, const void* src, void* dst)
{
    ++g_log.calls[2]; g_log.a[2][0] = deformer; g_log.a[2][1] = const_cast<int32_t*>(indices); g_log.a[2][2] = const_cast<float*>(matrix);
    g_log.a[2][3] = reinterpret_cast<void*>(static_cast<intptr_t>(count)); g_log.a[2][4] = const_cast<void*>(src); g_log.a[2][5] = dst;
}
// Like the real getter (returns the field at +8 of the world pose); here a fixed offset so the test needs no struct.
extern "C" __declspec(dllexport) void* __stdcall GrannyGetWorldPoseComposite4x4Array(void* world)
{
    ++g_log.calls[3]; g_log.a[3][0] = world;
    return static_cast<char*>(world) + 0x100;
}
extern "C" __declspec(dllexport) void __stdcall GrannyFreeModelInstance(void* inst) { ++g_log.calls[4]; g_log.a[4][0] = inst; }
extern "C" __declspec(dllexport) void __stdcall GrannyFreeMeshDeformer(void* d) { ++g_log.calls[5]; g_log.a[5][0] = d; }

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
