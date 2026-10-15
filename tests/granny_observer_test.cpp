// Validation of the Granny import-boundary observer without the game (SH2-Perf EXP-008, docs/FORMATS/GRANNY_OBSERVER.md).
// This exe imports the six functions from the stand-in granny2.dll (tests/fake_granny2.cpp) exactly like Stronghold2.exe
// imports them from the real one, so the observer patches this exe's import slots.
//   1. switch off: Install refuses, no import slot changes
//   2. forwarding: every call reaches the original exactly once with bit-identical arguments (incl. a signalling-NaN
//      clock), the composite return value is unchanged; also while not recording and from a foreign thread
//   3. data: a scripted running/paused sequence gives the expected per-object and per-frame numbers and links
//   4. limits: instance table full and frame rows full are counted and reported, a faulting offset read is counted
//   5. ending: End() while the owner thread keeps calling (no crash, files written), Uninstall restores every slot
// Exit code 0 = all checks passed. Uses a temp output dir.
#include "../Stronghold2AnalyseHub/core/granny_observer.h"
#include "../Stronghold2AnalyseHub/core/logger.h"

#include <cmath>
#include <map>
#include <sstream>
#include <string>

struct FakeGrannyLog { uint32_t calls[6]; void* a[6][6]; uint32_t clockBits; };
extern "C" __declspec(dllimport) FakeGrannyLog* __cdecl FakeGranny_Log();
extern "C" __declspec(dllimport) void __stdcall GrannySetModelClock(void* inst, uint32_t clockBits);
extern "C" __declspec(dllimport) void __stdcall GrannySampleModelAnimationsAccelerated(void* inst, int32_t bones, const float* offset, void* local, void* world);
extern "C" __declspec(dllimport) void __stdcall GrannyDeformVertices(void* d, const int32_t* idx, const float* m, int32_t n, const void* s, void* dst);
extern "C" __declspec(dllimport) void* __stdcall GrannyGetWorldPoseComposite4x4Array(void* world);
extern "C" __declspec(dllimport) void __stdcall GrannyFreeModelInstance(void* inst);
extern "C" __declspec(dllimport) void __stdcall GrannyFreeMeshDeformer(void* d);

namespace
{
    int g_failures = 0;
    std::wstring g_dir;
    uint64_t g_frame = 0;
    uint32_t g_marker = 0;
    uint64_t FrameFn() { return g_frame; }
    uint32_t MarkerFn() { return g_marker; }

#define CHECK(cond, ...) do { if (!(cond)) { ++g_failures; printf("FAIL line %d: %s  ", __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

    uint32_t Bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

    std::string ReadFile(const std::wstring& path)
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

    // rows keyed by "<first column>|<marker_slot or frame>"; values by header name
    using Row = std::map<std::string, std::string>;
    std::map<std::string, Row> ReadCsv(const std::wstring& path, const char* second)
    {
        std::map<std::string, Row> out;
        std::istringstream in(ReadFile(path));
        std::string line;
        std::vector<std::string> head;
        while (std::getline(in, line)) {
            std::vector<std::string> cells;
            std::stringstream ss(line);
            std::string c;
            while (std::getline(ss, c, ',')) cells.push_back(c);
            if (head.empty()) { head = cells; continue; }
            Row r;
            for (size_t i = 0; i < head.size() && i < cells.size(); ++i) r[head[i]] = cells[i];
            out[cells[0] + "|" + r[second]] = r;
        }
        return out;
    }
    long long N(const std::map<std::string, Row>& t, const std::string& key, const char* col)
    {
        auto it = t.find(key);
        if (it == t.end()) return -1;
        auto c = it->second.find(col);
        return c == it->second.end() ? -1 : atoll(c->second.c_str());
    }
    std::string Id(const void* p) { char b[16]; snprintf(b, sizeof(b), "0x%08X", static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p))); return b; }
    long long JsonValue(const std::string& json, const char* key)
    {
        const std::string k = std::string("\"") + key + "\": ";
        const size_t p = json.find(k);
        return p == std::string::npos ? -1 : atoll(json.c_str() + p + k.size());
    }

    // Values the import slots hold right now, read through volatile pointers to the slots (the compiler treats the
    // import pointer as constant, so `&GrannySetModelClock` may be read once and cached).
    void* volatile* g_slotClock = nullptr;
    void* volatile* g_slotDeform = nullptr;
    void* SlotClock() { return *g_slotClock; }
    void* SlotDeform() { return *g_slotDeform; }
    void* Export(const char* n) { return reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"granny2.dll"), n)); }

    alignas(16) char g_inst[4][64], g_world[4][0x200], g_local[64], g_def[4][64], g_src[64], g_dst[64];
    int32_t g_idx[8];
    float g_off[4][16];

    void Frame(int f, bool paused, int repeatI3)
    {
        g_frame = static_cast<uint64_t>(f);
        for (int i = 0; i < 3; ++i) {
            const float clock = 0.1f * static_cast<float>(paused ? 19 : f) + static_cast<float>(i);   // paused: frozen at frame 19
            GrannySetModelClock(g_inst[i], Bits(clock));
            if (i == 0 && !paused) g_off[0][12] = static_cast<float>(f);      // instance 0 moves while running
            const int samples = i == 2 ? repeatI3 : 1;
            for (int k = 0; k < samples; ++k) GrannySampleModelAnimationsAccelerated(g_inst[i], 20, g_off[i], g_local, g_world[i]);
            void* c = GrannyGetWorldPoseComposite4x4Array(g_world[i]);
            GrannyDeformVertices(g_def[i], g_idx, static_cast<const float*>(c), 100 + i, g_src, g_dst);
        }
        GrannyObserver::OnFrameEnd(g_frame, g_marker);
    }

    std::atomic<bool> g_stopWorker{ false };
    std::atomic<uint64_t> g_workerCalls{ 0 };
    DWORD WINAPI Foreign(LPVOID)
    {
        for (int i = 0; i < 5; ++i) GrannySetModelClock(g_inst[3], 0x3F800000u);
        return 0;
    }
    DWORD WINAPI OwnerWorker(LPVOID)
    {
        GrannyObserver::SetOwnerTid(GetCurrentThreadId());
        uint64_t n = 0;
        while (!g_stopWorker.load()) {
            GrannySetModelClock(g_inst[0], static_cast<uint32_t>(n));
            GrannySampleModelAnimationsAccelerated(g_inst[0], 20, g_off[0], g_local, g_world[0]);
            void* c = GrannyGetWorldPoseComposite4x4Array(g_world[0]);
            GrannyDeformVertices(g_def[0], g_idx, static_cast<const float*>(c), 7, g_src, g_dst);
            n += 4;
            if (n % 400 == 0) GrannyObserver::OnFrameEnd(n / 400, 0);
        }
        g_workerCalls.store(n);
        return 0;
    }
}

int main()
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_dir = std::wstring(tmp) + L"sh2_granny_observer_test_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
    CreateDirectoryW(g_dir.c_str(), nullptr);
    SetEnvironmentVariableW(L"SH2HUB_OUT", g_dir.c_str());
    Logger::Init();
    FakeGrannyLog& log = *FakeGranny_Log();

    g_slotClock = GrannyObserver::Test_FindSlot(GetModuleHandleW(nullptr), GrannyObserver::kClock);
    g_slotDeform = GrannyObserver::Test_FindSlot(GetModuleHandleW(nullptr), GrannyObserver::kDeform);
    if (!g_slotClock || !g_slotDeform) { printf("granny_observer_test: import slots not found\n"); return 1; }

    // 1. switch off: nothing patched
    GrannyObserver::Test_ForceEnabled(false);
    CHECK(!GrannyObserver::Install(FrameFn, MarkerFn, GetModuleHandleW(nullptr)), "install with switch off");
    CHECK(SlotClock() == Export("_GrannySetModelClock@8"), "slot unchanged when off");
    CHECK(SlotDeform() == Export("_GrannyDeformVertices@24"), "slot unchanged when off");

    // 2. install + forwarding while not recording
    GrannyObserver::Test_ForceEnabled(true);
    CHECK(GrannyObserver::Install(FrameFn, MarkerFn, GetModuleHandleW(nullptr)), "install");
    CHECK(GrannyObserver::SlotsPatched() == 6, "%d slots", GrannyObserver::SlotsPatched());
    CHECK(SlotClock() != Export("_GrannySetModelClock@8"), "slot redirected");
    const uint32_t sNaN = 0x7F800001u;
    GrannySetModelClock(g_inst[0], sNaN);
    CHECK(log.calls[0] == 1 && log.a[0][0] == g_inst[0] && log.clockBits == sNaN, "clock forwarded bit-exact: %u 0x%08X", log.calls[0], log.clockBits);
    GrannySampleModelAnimationsAccelerated(g_inst[1], -3, g_off[1], g_local, g_world[1]);
    CHECK(log.calls[1] == 1 && log.a[1][0] == g_inst[1] && log.a[1][1] == reinterpret_cast<void*>(-3) && log.a[1][2] == g_off[1] &&
          log.a[1][3] == g_local && log.a[1][4] == g_world[1], "sample forwarded");
    void* comp = GrannyGetWorldPoseComposite4x4Array(g_world[2]);
    CHECK(comp == g_world[2] + 0x100 && log.calls[3] == 1, "composite return value unchanged");
    GrannyDeformVertices(g_def[1], g_idx, static_cast<const float*>(comp), 0x7FFFFFFF, g_src, g_dst);
    CHECK(log.calls[2] == 1 && log.a[2][0] == g_def[1] && log.a[2][1] == g_idx && log.a[2][2] == comp &&
          log.a[2][3] == reinterpret_cast<void*>(0x7FFFFFFF) && log.a[2][4] == g_src && log.a[2][5] == g_dst, "deform forwarded");
    GrannyFreeModelInstance(g_inst[3]);
    GrannyFreeMeshDeformer(g_def[3]);
    CHECK(log.calls[4] == 1 && log.calls[5] == 1, "frees forwarded");

    // 3. scripted recording: frames 0..19 marker 1 running, 20..29 marker 3 paused (instance 2 sampled twice per paused frame)
    for (int i = 0; i < 4; ++i) for (int k = 0; k < 16; ++k) g_off[i][k] = static_cast<float>(i * 16 + k);
    GrannyObserver::SetOwnerTid(GetCurrentThreadId());
    const uint32_t before[6] = { log.calls[0], log.calls[1], log.calls[2], log.calls[3], log.calls[4], log.calls[5] };
    CHECK(GrannyObserver::Begin(g_dir + L"obs1"), "begin");
    g_marker = 1;
    for (int f = 0; f < 20; ++f) Frame(f, false, 1);
    g_marker = 3;
    for (int f = 20; f < 30; ++f) Frame(f, true, 2);
    HANDLE th = CreateThread(nullptr, 0, Foreign, nullptr, 0, nullptr);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    // faulting offset read (the stand-in does not read it; the real function would)
    void* noAccess = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    GrannySampleModelAnimationsAccelerated(g_inst[3], 1, static_cast<const float*>(noAccess), g_local, g_world[3]);
    GrannyFreeModelInstance(g_inst[1]);
    g_frame = 30;
    GrannySetModelClock(g_inst[1], Bits(5.0f));            // pointer used again after a free
    GrannyObserver::OnFrameEnd(30, 3);
    CHECK(GrannyObserver::End(), "end writes files");
    VirtualFree(noAccess, 0, MEM_RELEASE);
    CHECK(log.calls[0] - before[0] == 30 * 3 + 5 + 1, "clock calls forwarded once each: %u", log.calls[0] - before[0]);
    CHECK(log.calls[1] - before[1] == 20 * 3 + 10 * 4 + 1, "sample calls: %u", log.calls[1] - before[1]);
    CHECK(log.calls[2] - before[2] == 90 && log.calls[3] - before[3] == 90, "deform/composite calls");

    const auto inst = ReadCsv(g_dir + L"obs1_granny_instances.csv", "marker_slot");
    const auto defs = ReadCsv(g_dir + L"obs1_granny_deformers.csv", "marker_slot");
    const auto frames = ReadCsv(g_dir + L"obs1_granny_frames.csv", "frame");
    const std::string meta = ReadFile(g_dir + L"obs1_granny.json");
    const std::string i0 = Id(g_inst[0]), i1 = Id(g_inst[1]), i2 = Id(g_inst[2]), i3 = Id(g_inst[3]);
    CHECK(N(inst, i0 + "|1", "clock_calls") == 20 && N(inst, i0 + "|1", "clock_noprev") == 1 && N(inst, i0 + "|1", "clock_changed") == 19,
          "running clock i0");
    CHECK(N(inst, i0 + "|1", "sample_changed") == 19 && N(inst, i0 + "|1", "ch_offset") == 19, "running offset i0");
    CHECK(N(inst, i1 + "|1", "sample_same") == 19 && N(inst, i1 + "|1", "all_observed_same") == 0, "running i1: inputs same, clock changes");
    CHECK(N(inst, i0 + "|3", "clock_same") == 10 && N(inst, i0 + "|3", "clock_changed") == 0, "paused i0 clock: %lld/%lld",
          N(inst, i0 + "|3", "clock_same"), N(inst, i0 + "|3", "clock_changed"));
    CHECK(N(inst, i1 + "|3", "all_observed_same") == 10, "paused i1 all same: %lld", N(inst, i1 + "|3", "all_observed_same"));
    CHECK(N(inst, i2 + "|3", "sample_calls") == 20 && N(inst, i2 + "|3", "sample_repeat_in_frame") == 10, "paused i2 repeat");
    CHECK(N(inst, i3 + "|3", "offset_faults") == 1, "offset fault counted");
    CHECK(N(inst, i1 + "|3", "used_after_free") == 1 && N(inst, i1 + "|3", "frees") == 1, "reuse after free flagged");
    CHECK(N(defs, Id(g_def[0]) + "|1", "calls") == 20 && N(defs, Id(g_def[0]) + "|1", "same") == 19 && N(defs, Id(g_def[0]) + "|1", "link_frame") == 20,
          "deformer 0 running");
    CHECK(defs.count(Id(g_def[1]) + "|3") && defs.at(Id(g_def[1]) + "|3").at("linked_instance") == i1, "deformer 1 linked to instance 1");
    CHECK(N(defs, Id(g_def[2]) + "|3", "link_ambiguous") == 0 && N(defs, Id(g_def[2]) + "|3", "vertex_count_max") == 102, "deformer 2");
    CHECK(frames.size() == 31 && N(frames, "0|0", "partial") == 1 && N(frames, "5|5", "partial") == 0, "frame rows %zu", frames.size());
    CHECK(N(frames, "25|25", "inst_clocked") == 3 && N(frames, "25|25", "clock_same") == 3 && N(frames, "25|25", "all_observed_same") == 4 &&
          N(frames, "25|25", "link_frame") == 3 && N(frames, "25|25", "marker") == 3, "frame 25 row");
    CHECK(N(frames, "5|5", "clock_changed") == 3 && N(frames, "5|5", "sample_calls") == 3, "frame 5 row");
    CHECK(meta.find("\"GrannySetModelClock\": 5") != std::string::npos, "foreign calls in meta");
    CHECK(JsonValue(meta, "offset_read_faults") == 1 && JsonValue(meta, "instance_table_full") == 0, "losses in meta");

    // 4. limits
    CHECK(GrannyObserver::Begin(g_dir + L"obs2"), "begin 2");
    g_frame = 0;
    for (uint32_t i = 0; i < 6000; ++i) GrannySetModelClock(reinterpret_cast<void*>(static_cast<uintptr_t>(0x20000000u + i * 64u)), i);
    for (uint32_t f = 0; f < GrannyObserver::kFrameRows + 100; ++f) GrannyObserver::OnFrameEnd(f, 0);
    CHECK(GrannyObserver::End(), "end 2");
    const std::string meta2 = ReadFile(g_dir + L"obs2_granny.json");
    CHECK(JsonValue(meta2, "instance_table_full") >= 6000 - static_cast<long long>(GrannyObserver::kInstanceSlots), "table full counted: %lld",
          JsonValue(meta2, "instance_table_full"));
    CHECK(JsonValue(meta2, "frame_rows_dropped") == 100 && JsonValue(meta2, "frame_rows_written") == GrannyObserver::kFrameRows, "frame rows full counted");

    // 5. End while the owner keeps calling, then Uninstall
    const uint32_t c0 = log.calls[0];
    CHECK(GrannyObserver::Begin(g_dir + L"obs3"), "begin 3");
    HANDLE w = CreateThread(nullptr, 0, OwnerWorker, nullptr, 0, nullptr);
    Sleep(100);
    CHECK(GrannyObserver::End(), "end while calls continue");
    Sleep(50);
    g_stopWorker.store(true);
    WaitForSingleObject(w, INFINITE);
    CloseHandle(w);
    CHECK(log.calls[0] - c0 == g_workerCalls.load() / 4, "every worker call forwarded once: %u vs %llu", log.calls[0] - c0, g_workerCalls.load() / 4);
    GrannyObserver::Uninstall();
    CHECK(SlotClock() == Export("_GrannySetModelClock@8") && SlotDeform() == Export("_GrannyDeformVertices@24"), "slots restored");
    CHECK(GrannyObserver::SlotsPatched() == 0, "nothing patched after uninstall");

    Logger::Shutdown();
    printf("granny_observer_test: %s (%d failure(s)); output %ls\n", g_failures ? "FAILED" : "OK", g_failures, g_dir.c_str());
    return g_failures ? 1 : 0;
}
