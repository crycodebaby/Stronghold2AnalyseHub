#include "granny_observer.h"
#include "config.h"
#include "logger.h"

#include <intrin.h>

// -----------------------------------------------------------------------------
// Implementation notes:
// - Thunk path (game threads): one atomic load when not recording. While recording, only the owner (render) thread
//   touches the tables, under a single "busy" flag: End() clears `active` and then waits on the capture thread until
//   `busy` is 0, so the tables are never freed or read while a thunk is inside its bookkeeping (Dekker order, seq_cst).
//   No heap, no lock, no file, no logger in a thunk. Other threads only bump atomic counters.
// - Bookkeeping happens before the original call (arguments) or after it (return value of the composite getter);
//   the original is always called exactly once with the arguments it was given.
// - The only read of game data is the 64-byte offset matrix of GrannySampleModelAnimationsAccelerated, which the exe
//   fills immediately before the call (F137); it is copied under SEH and a fault is counted, not hidden.
// - Tables: open addressing on the pointer value, linear probe of at most kProbe slots, never deleted. Full = counted.
// -----------------------------------------------------------------------------

namespace
{
    using namespace GrannyObserver;
    constexpr uint32_t kProbe = 32;
    const char* const kFnNames[kFnCount] = {
        "GrannySetModelClock", "GrannySampleModelAnimationsAccelerated", "GrannyDeformVertices",
        "GrannyGetWorldPoseComposite4x4Array", "GrannyFreeModelInstance", "GrannyFreeMeshDeformer" };
    const char* const kImportNames[kFnCount] = {
        "_GrannySetModelClock@8", "_GrannySampleModelAnimationsAccelerated@20", "_GrannyDeformVertices@24",
        "_GrannyGetWorldPoseComposite4x4Array@4", "_GrannyFreeModelInstance@4", "_GrannyFreeMeshDeformer@4" };

    struct InstM
    {
        uint32_t clockFrames, clockCalls, clockSame, clockChanged, clockNoPrev, clockRepeat, clockChangeInFrame;
        uint32_t sampleFrames, sampleCalls, sampleSame, sampleChanged, sampleNoPrev, sampleRepeat;
        uint32_t chBone, chOffset, chLocal, chWorld, allSame, offsetFaults;
        float clockMin, clockMax;
    };
    struct Inst
    {
        uintptr_t key;
        uint32_t firstFrame1, lastFrame1, clockFrame1, sampleFrame1;   // frame index + 1 (0 = never)
        uint32_t clockBits;
        uint8_t haveClock, haveSample, clockSameThisCall, freed, multiSiteClock, multiSiteSample, offsetValid, pad;
        int32_t bone;
        uintptr_t local, world;
        uint32_t offset[16];                                   // raw bits of the last offset matrix
        uint32_t frees, usedAfterFree, siteClock, siteSample;
        InstM m[kMarkerSlots];
    };
    struct DefM
    {
        uint32_t frames, calls, same, changed, noPrev, repeat;
        uint32_t chIndices, chMatrix, chCount, chSrc, chDst;
        uint32_t linkFrame, linkOlder, linkNone, linkAmbiguous, linkInstChanged;
        int32_t countMin, countMax;
    };
    struct Def
    {
        uintptr_t key;
        uint32_t firstFrame1, lastFrame1;
        uintptr_t indices, matrix, src, dst, linkedInst;
        int32_t count;
        uint8_t have, multiSite, freed, pad;
        uint32_t frees, usedAfterFree, site;
        DefM m[kMarkerSlots];
    };
    struct Pose          // key = world pose
    {
        uintptr_t key, composite, sampledInst;
        uint32_t compositeFrame1, sampledFrame1, ambiguousFrame1;
    };
    struct Comp          // key = composite array pointer returned for a world pose
    {
        uintptr_t key, world;
        uint32_t frame1, conflicts;
    };
    struct FrameRow
    {
        uint32_t frame, marker, partial;
        uint32_t calls[kFnCount];
        uint32_t instClocked, clockSame, clockChanged, clockNoPrev;
        uint32_t instSampled, sampleSame, sampleChanged, sampleNoPrev, allSame;
        uint32_t deformers, defSame, defChanged, defNoPrev;
        uint32_t linkFrame, linkOlder, linkNone, linkAmbiguous;
        uint32_t drops;
    };
    struct Tables
    {
        Inst inst[kInstanceSlots];
        Def def[kDeformerSlots];
        Pose pose[kPoseSlots];
        Comp comp[kPoseSlots];
        FrameRow rows[kFrameRows];
        FrameRow cur;
        uint32_t rowCount, framesDropped;
        uint32_t dropsInst, dropsDef, dropsPose, dropsComp;
        uint64_t calls[kFnCount];
        uint32_t offsetFaults, compConflicts;
        bool first;
    };

    struct Slot { void** iat = nullptr; void* orig = nullptr; void* thunk = nullptr; bool patched = false; uint32_t iatRva = 0, origRva = 0; };

    bool g_forceEnabled = false;
    Slot g_slots[kFnCount];
    bool g_installed = false;
    uintptr_t g_exeBase = 0, g_exeEnd = 0;
    uint64_t (*g_frameFn)() = nullptr;
    uint32_t (*g_markerFn)() = nullptr;

    std::atomic<bool> g_active{ false };
    std::atomic<int> g_busy{ 0 };
    std::atomic<uint32_t> g_owner{ 0 };
    std::atomic<uint64_t> g_foreign[kFnCount];
    std::atomic<uint32_t> g_firstForeignTid{ 0 };
    Tables* g_t = nullptr;
    std::wstring g_base;
    GrannyObserver::Totals g_lastTotals;
    bool g_wroteFiles = false;

    SetModelClock_t  g_origClock = nullptr;
    SampleAccel_t    g_origSample = nullptr;
    DeformVertices_t g_origDeform = nullptr;
    Composite_t      g_origComposite = nullptr;
    Free_t           g_origFreeInstance = nullptr;
    Free_t           g_origFreeDeformer = nullptr;

    // ---- thunk-side helpers (no heap, no lock, no wait) ----

    bool Enter(int fn)
    {
        if (!g_active.load(std::memory_order_acquire)) return false;
        const uint32_t tid = GetCurrentThreadId();
        if (tid != g_owner.load(std::memory_order_relaxed)) {
            g_foreign[fn].fetch_add(1, std::memory_order_relaxed);
            uint32_t none = 0;
            g_firstForeignTid.compare_exchange_strong(none, tid, std::memory_order_relaxed);
            return false;
        }
        g_busy.store(1, std::memory_order_seq_cst);
        if (!g_active.load(std::memory_order_seq_cst)) { g_busy.store(0, std::memory_order_release); return false; }
        return true;
    }
    void Leave() { g_busy.store(0, std::memory_order_release); }

    uint32_t Hash(uintptr_t p) { return static_cast<uint32_t>(p >> 3) * 2654435761u; }

    template <typename T>
    T* Find(T* table, uint32_t cap, uintptr_t key, uint32_t& drops)
    {
        uint32_t i = Hash(key) & (cap - 1);
        for (uint32_t n = 0; n < kProbe; ++n, i = (i + 1) & (cap - 1)) {
            if (table[i].key == key) return &table[i];
            if (table[i].key == 0) { table[i].key = key; return &table[i]; }
        }
        ++drops;
        return nullptr;
    }
    template <typename T>
    T* Lookup(T* table, uint32_t cap, uintptr_t key)
    {
        uint32_t i = Hash(key) & (cap - 1);
        for (uint32_t n = 0; n < kProbe; ++n, i = (i + 1) & (cap - 1)) {
            if (table[i].key == key) return &table[i];
            if (table[i].key == 0) return nullptr;
        }
        return nullptr;
    }

    uint32_t Slot5(uint32_t marker) { return marker < kMarkerSlots - 1 ? marker : kMarkerSlots - 1; }
    uint32_t Site(uintptr_t ret) { return ret >= g_exeBase && ret < g_exeEnd ? static_cast<uint32_t>(ret - g_exeBase) : 0xFFFFFFFFu; }

    bool CopyMatrix(const float* src, uint32_t* dst)
    {
        __try {
            const volatile uint32_t* s = reinterpret_cast<const volatile uint32_t*>(src);
            for (int i = 0; i < 16; ++i) dst[i] = s[i];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void Touch(uint32_t& first1, uint32_t& last1, uint32_t f1) { if (!first1) first1 = f1; last1 = f1; }

    void NoteFreedInst(Inst* e) { if (e->freed) { ++e->usedAfterFree; e->freed = 0; } }

    void OnClock(void* inst, uint32_t bits, uintptr_t ret)
    {
        Tables& t = *g_t;
        ++t.calls[kClock]; ++t.cur.calls[kClock];
        Inst* e = Find(t.inst, kInstanceSlots, reinterpret_cast<uintptr_t>(inst), t.dropsInst);
        if (!e) { ++t.cur.drops; return; }
        const uint32_t f1 = static_cast<uint32_t>(g_frameFn()) + 1;
        InstM& m = e->m[Slot5(g_markerFn())];
        NoteFreedInst(e);
        Touch(e->firstFrame1, e->lastFrame1, f1);
        const uint32_t site = Site(ret);
        if (!e->siteClock) e->siteClock = site; else if (e->siteClock != site) e->multiSiteClock = 1;
        float clock;
        memcpy(&clock, &bits, 4);
        const bool newFrame = e->clockFrame1 != f1;
        if (m.clockCalls == 0) m.clockMin = m.clockMax = clock;
        else { if (clock < m.clockMin) m.clockMin = clock; if (clock > m.clockMax) m.clockMax = clock; }
        ++m.clockCalls;
        if (newFrame) { ++m.clockFrames; ++t.cur.instClocked; }
        else { ++m.clockRepeat; if (bits != e->clockBits) ++m.clockChangeInFrame; }
        if (!e->haveClock) { ++m.clockNoPrev; ++t.cur.clockNoPrev; e->clockSameThisCall = 0; }
        else if (bits == e->clockBits) { ++m.clockSame; ++t.cur.clockSame; e->clockSameThisCall = 1; }
        else { ++m.clockChanged; ++t.cur.clockChanged; e->clockSameThisCall = 0; }
        e->clockBits = bits;
        e->haveClock = 1;
        e->clockFrame1 = f1;
    }

    void OnSample(void* inst, int32_t bone, const float* offset, void* local, void* world, uintptr_t ret)
    {
        Tables& t = *g_t;
        ++t.calls[kSample]; ++t.cur.calls[kSample];
        const uint32_t f1 = static_cast<uint32_t>(g_frameFn()) + 1;
        const uintptr_t key = reinterpret_cast<uintptr_t>(inst), w = reinterpret_cast<uintptr_t>(world);
        if (Pose* p = Find(t.pose, kPoseSlots, w, t.dropsPose)) {
            if (p->sampledFrame1 == f1 && p->sampledInst != key) p->ambiguousFrame1 = f1;
            p->sampledInst = key;
            p->sampledFrame1 = f1;
        }
        Inst* e = Find(t.inst, kInstanceSlots, key, t.dropsInst);
        if (!e) { ++t.cur.drops; return; }
        InstM& m = e->m[Slot5(g_markerFn())];
        NoteFreedInst(e);
        Touch(e->firstFrame1, e->lastFrame1, f1);
        const uint32_t site = Site(ret);
        if (!e->siteSample) e->siteSample = site; else if (e->siteSample != site) e->multiSiteSample = 1;

        uint32_t mat[16]{};
        bool matOk = false;
        if (offset) {
            matOk = CopyMatrix(offset, mat);
            if (!matOk) { ++m.offsetFaults; ++t.offsetFaults; }
        }
        ++m.sampleCalls;
        if (e->sampleFrame1 != f1) { ++m.sampleFrames; ++t.cur.instSampled; } else ++m.sampleRepeat;
        if (!e->haveSample) { ++m.sampleNoPrev; ++t.cur.sampleNoPrev; }
        else {
            const bool sameBone = bone == e->bone, sameLocal = reinterpret_cast<uintptr_t>(local) == e->local, sameWorld = w == e->world;
            // null offset == null offset; a faulted copy never counts as unchanged
            const bool sameOffset = offset ? (matOk && e->offsetValid && memcmp(mat, e->offset, sizeof(mat)) == 0) : !e->offsetValid;
            if (!sameBone) ++m.chBone;
            if (!sameLocal) ++m.chLocal;
            if (!sameWorld) ++m.chWorld;
            if (!sameOffset) ++m.chOffset;
            if (sameBone && sameLocal && sameWorld && sameOffset) {
                ++m.sampleSame; ++t.cur.sampleSame;
                if (e->clockFrame1 == f1 && e->clockSameThisCall) { ++m.allSame; ++t.cur.allSame; }
            }
            else { ++m.sampleChanged; ++t.cur.sampleChanged; }
        }
        e->bone = bone;
        e->local = reinterpret_cast<uintptr_t>(local);
        e->world = w;
        e->offsetValid = offset && matOk ? 1 : 0;
        if (e->offsetValid) memcpy(e->offset, mat, sizeof(mat));
        e->haveSample = 1;
        e->sampleFrame1 = f1;
    }

    void OnDeform(void* deformer, const int32_t* indices, const float* matrix, int32_t count, const void* src, void* dst, uintptr_t ret)
    {
        Tables& t = *g_t;
        ++t.calls[kDeform]; ++t.cur.calls[kDeform];
        Def* e = Find(t.def, kDeformerSlots, reinterpret_cast<uintptr_t>(deformer), t.dropsDef);
        if (!e) { ++t.cur.drops; return; }
        const uint32_t f1 = static_cast<uint32_t>(g_frameFn()) + 1;
        DefM& m = e->m[Slot5(g_markerFn())];
        if (e->freed) { ++e->usedAfterFree; e->freed = 0; }
        const bool newFrame = e->lastFrame1 != f1;
        Touch(e->firstFrame1, e->lastFrame1, f1);
        const uint32_t site = Site(ret);
        if (!e->site) e->site = site; else if (e->site != site) e->multiSite = 1;
        if (m.calls == 0) m.countMin = m.countMax = count;
        else { if (count < m.countMin) m.countMin = count; if (count > m.countMax) m.countMax = count; }
        ++m.calls;
        if (newFrame) { ++m.frames; ++t.cur.deformers; } else ++m.repeat;
        const uintptr_t ind = reinterpret_cast<uintptr_t>(indices), mat = reinterpret_cast<uintptr_t>(matrix),
            s = reinterpret_cast<uintptr_t>(src), d = reinterpret_cast<uintptr_t>(dst);
        if (!e->have) { ++m.noPrev; ++t.cur.defNoPrev; }
        else {
            bool same = true;
            if (ind != e->indices) { ++m.chIndices; same = false; }
            if (mat != e->matrix) { ++m.chMatrix; same = false; }
            if (count != e->count) { ++m.chCount; same = false; }
            if (s != e->src) { ++m.chSrc; same = false; }
            if (d != e->dst) { ++m.chDst; same = false; }
            if (same) { ++m.same; ++t.cur.defSame; } else { ++m.changed; ++t.cur.defChanged; }
        }
        e->indices = ind; e->matrix = mat; e->count = count; e->src = s; e->dst = d; e->have = 1;

        // Link: matrix buffer == composite array returned for world pose W, and W was the Result of a sample of instance I.
        uintptr_t inst = 0;
        const Comp* c = Lookup(t.comp, kPoseSlots, mat);
        const Pose* p = c ? Lookup(t.pose, kPoseSlots, c->world) : nullptr;
        if (!p || !p->sampledFrame1) { ++m.linkNone; ++t.cur.linkNone; }
        else if (p->ambiguousFrame1 == p->sampledFrame1) { ++m.linkAmbiguous; ++t.cur.linkAmbiguous; }
        else {
            inst = p->sampledInst;
            if (c->frame1 == f1 && p->sampledFrame1 == f1) { ++m.linkFrame; ++t.cur.linkFrame; }
            else { ++m.linkOlder; ++t.cur.linkOlder; }
        }
        if (inst) {
            if (e->linkedInst && e->linkedInst != inst) ++m.linkInstChanged;
            e->linkedInst = inst;
        }
    }

    void OnComposite(void* world, void* result)
    {
        Tables& t = *g_t;
        ++t.calls[kComposite]; ++t.cur.calls[kComposite];
        const uint32_t f1 = static_cast<uint32_t>(g_frameFn()) + 1;
        const uintptr_t w = reinterpret_cast<uintptr_t>(world), r = reinterpret_cast<uintptr_t>(result);
        if (Pose* p = Find(t.pose, kPoseSlots, w, t.dropsPose)) { p->composite = r; p->compositeFrame1 = f1; }
        if (!r) return;
        if (Comp* c = Find(t.comp, kPoseSlots, r, t.dropsComp)) {
            if (c->world && c->world != w) { ++c->conflicts; ++t.compConflicts; }
            c->world = w;
            c->frame1 = f1;
        }
    }

    void OnFree(int fn, void* obj)
    {
        Tables& t = *g_t;
        ++t.calls[fn]; ++t.cur.calls[fn];
        const uintptr_t key = reinterpret_cast<uintptr_t>(obj);
        if (fn == kFreeInstance) { if (Inst* e = Lookup(t.inst, kInstanceSlots, key)) { ++e->frees; e->freed = 1; } }
        else if (Def* e = Lookup(t.def, kDeformerSlots, key)) { ++e->frees; e->freed = 1; }
    }

    // ---- thunks: same convention and arguments as the import; the original is called exactly once ----

    void __stdcall T_Clock(void* inst, uint32_t clockBits)
    {
        if (Enter(kClock)) { OnClock(inst, clockBits, reinterpret_cast<uintptr_t>(_ReturnAddress())); Leave(); }
        g_origClock(inst, clockBits);
    }
    void __stdcall T_Sample(void* inst, int32_t bone, const float* offset, void* local, void* world)
    {
        if (Enter(kSample)) { OnSample(inst, bone, offset, local, world, reinterpret_cast<uintptr_t>(_ReturnAddress())); Leave(); }
        g_origSample(inst, bone, offset, local, world);
    }
    void __stdcall T_Deform(void* deformer, const int32_t* indices, const float* matrix, int32_t count, const void* src, void* dst)
    {
        if (Enter(kDeform)) { OnDeform(deformer, indices, matrix, count, src, dst, reinterpret_cast<uintptr_t>(_ReturnAddress())); Leave(); }
        g_origDeform(deformer, indices, matrix, count, src, dst);
    }
    void* __stdcall T_Composite(void* world)
    {
        void* r = g_origComposite(world);
        if (Enter(kComposite)) { OnComposite(world, r); Leave(); }
        return r;
    }
    void __stdcall T_FreeInstance(void* obj)
    {
        if (Enter(kFreeInstance)) { OnFree(kFreeInstance, obj); Leave(); }
        g_origFreeInstance(obj);
    }
    void __stdcall T_FreeDeformer(void* obj)
    {
        if (Enter(kFreeDeformer)) { OnFree(kFreeDeformer, obj); Leave(); }
        g_origFreeDeformer(obj);
    }

    // ---- output (capture thread) ----

    std::wstring W(const char* s) { return std::wstring(s, s + strlen(s)); }

    bool WriteAll(const Tables& t, const GrannyObserver::Totals& tot)
    {
        bool ok = true;
        FILE* f = nullptr;
        // meta
        if (_wfopen_s(&f, (g_base + L"_granny.json").c_str(), L"wb") || !f) return false;
        fprintf(f, "{\n  \"schema\": \"sh2perf.granny-observer\",\n  \"version\": 1,\n  \"hub\": \"%s\",\n", HUB_GIT_COMMIT);
        fprintf(f, "  \"owner_tid\": %u,\n  \"slots\": [", g_owner.load());
        for (int i = 0; i < kFnCount; ++i)
            fprintf(f, "%s\n    {\"fn\": \"%s\", \"import\": \"%s\", \"iat_rva\": \"0x%08X\", \"orig_rva_in_granny2\": \"0x%08X\", \"patched\": %s}",
                i ? "," : "", kFnNames[i], kImportNames[i], g_slots[i].iatRva, g_slots[i].origRva, g_slots[i].patched ? "true" : "false");
        fprintf(f, "\n  ],\n  \"calls\": {");
        for (int i = 0; i < kFnCount; ++i) fprintf(f, "%s\"%s\": %llu", i ? ", " : "", kFnNames[i], tot.calls[i]);
        fprintf(f, "},\n  \"foreign_thread_calls\": {");
        for (int i = 0; i < kFnCount; ++i) fprintf(f, "%s\"%s\": %llu", i ? ", " : "", kFnNames[i], tot.foreign[i]);
        fprintf(f, "},\n  \"first_foreign_tid\": %u,\n", tot.firstForeignTid);
        fprintf(f, "  \"capacity\": {\"instances\": %u, \"deformers\": %u, \"poses\": %u, \"frame_rows\": %u, \"probe\": %u},\n",
            kInstanceSlots, kDeformerSlots, kPoseSlots, kFrameRows, kProbe);
        fprintf(f, "  \"losses\": {\"instance_table_full\": %u, \"deformer_table_full\": %u, \"pose_table_full\": %u, \"composite_table_full\": %u, "
            "\"frame_rows_dropped\": %u, \"offset_read_faults\": %u},\n",
            t.dropsInst, t.dropsDef, t.dropsPose, t.dropsComp, t.framesDropped, t.offsetFaults);
        fprintf(f, "  \"composite_pointer_conflicts\": %u,\n  \"frame_rows_written\": %u,\n", t.compConflicts, t.rowCount);
        fprintf(f, "  \"coverage\": \"exe import slots of granny2.dll only; calls inside granny2.dll and calls from other modules are not seen; "
            "only the owner (render) thread is aggregated, other threads are counted in foreign_thread_calls\"\n}\n");
        ok &= ferror(f) == 0;
        ok &= fclose(f) == 0;

        // frames
        if (_wfopen_s(&f, (g_base + L"_granny_frames.csv").c_str(), L"wb") || !f) return false;
        setvbuf(f, nullptr, _IOFBF, 1 << 16);
        fputs("frame,marker,partial,clock_calls,sample_calls,deform_calls,composite_calls,free_instance_calls,free_deformer_calls,"
              "inst_clocked,clock_same,clock_changed,clock_noprev,inst_sampled,sample_same,sample_changed,sample_noprev,all_observed_same,"
              "deformers,deform_same,deform_changed,deform_noprev,link_frame,link_older,link_none,link_ambiguous,drops\n", f);
        for (uint32_t i = 0; i < t.rowCount; ++i) {
            const FrameRow& r = t.rows[i];
            fprintf(f, "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                r.frame, r.marker, r.partial, r.calls[0], r.calls[1], r.calls[2], r.calls[3], r.calls[4], r.calls[5],
                r.instClocked, r.clockSame, r.clockChanged, r.clockNoPrev, r.instSampled, r.sampleSame, r.sampleChanged, r.sampleNoPrev,
                r.allSame, r.deformers, r.defSame, r.defChanged, r.defNoPrev, r.linkFrame, r.linkOlder, r.linkNone, r.linkAmbiguous, r.drops);
        }
        ok &= ferror(f) == 0;
        ok &= fclose(f) == 0;

        // instances
        if (_wfopen_s(&f, (g_base + L"_granny_instances.csv").c_str(), L"wb") || !f) return false;
        setvbuf(f, nullptr, _IOFBF, 1 << 16);
        fputs("instance,marker_slot,first_frame,last_frame,clock_frames,clock_calls,clock_same,clock_changed,clock_noprev,"
              "clock_repeat_in_frame,clock_change_in_frame,clock_min,clock_max,sample_frames,sample_calls,sample_same,sample_changed,"
              "sample_noprev,sample_repeat_in_frame,ch_bone_count,ch_offset,ch_local_pose,ch_world_pose,all_observed_same,offset_faults,"
              "bone_count,world_pose,frees,used_after_free,site_clock,site_sample,multi_site\n", f);
        for (uint32_t i = 0; i < kInstanceSlots; ++i) {
            const Inst& e = t.inst[i];
            if (!e.key) continue;
            for (uint32_t s = 0; s < kMarkerSlots; ++s) {
                const InstM& m = e.m[s];
                if (!m.clockCalls && !m.sampleCalls) continue;
                fprintf(f, "0x%08X,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.9g,%.9g,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,0x%08X,%u,%u,0x%08X,0x%08X,%u\n",
                    static_cast<uint32_t>(e.key), s, e.firstFrame1 - 1, e.lastFrame1 - 1, m.clockFrames, m.clockCalls, m.clockSame,
                    m.clockChanged, m.clockNoPrev, m.clockRepeat, m.clockChangeInFrame,
                    m.clockCalls ? m.clockMin : 0.0f, m.clockCalls ? m.clockMax : 0.0f,
                    m.sampleFrames, m.sampleCalls, m.sampleSame, m.sampleChanged, m.sampleNoPrev, m.sampleRepeat,
                    m.chBone, m.chOffset, m.chLocal, m.chWorld, m.allSame, m.offsetFaults,
                    e.bone, static_cast<uint32_t>(e.world), e.frees, e.usedAfterFree, e.siteClock, e.siteSample,
                    (e.multiSiteClock || e.multiSiteSample) ? 1u : 0u);
            }
        }
        ok &= ferror(f) == 0;
        ok &= fclose(f) == 0;

        // deformers
        if (_wfopen_s(&f, (g_base + L"_granny_deformers.csv").c_str(), L"wb") || !f) return false;
        setvbuf(f, nullptr, _IOFBF, 1 << 16);
        fputs("deformer,marker_slot,first_frame,last_frame,frames,calls,same,changed,noprev,repeat_in_frame,"
              "ch_indices,ch_matrix,ch_count,ch_src,ch_dst,vertex_count_min,vertex_count_max,"
              "link_frame,link_older,link_none,link_ambiguous,link_instance_changes,linked_instance,frees,used_after_free,site,multi_site\n", f);
        for (uint32_t i = 0; i < kDeformerSlots; ++i) {
            const Def& e = t.def[i];
            if (!e.key) continue;
            for (uint32_t s = 0; s < kMarkerSlots; ++s) {
                const DefM& m = e.m[s];
                if (!m.calls) continue;
                fprintf(f, "0x%08X,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%u,%u,%u,%u,%u,0x%08X,%u,%u,0x%08X,%u\n",
                    static_cast<uint32_t>(e.key), s, e.firstFrame1 - 1, e.lastFrame1 - 1, m.frames, m.calls, m.same, m.changed, m.noPrev,
                    m.repeat, m.chIndices, m.chMatrix, m.chCount, m.chSrc, m.chDst, m.countMin, m.countMax,
                    m.linkFrame, m.linkOlder, m.linkNone, m.linkAmbiguous, m.linkInstChanged, static_cast<uint32_t>(e.linkedInst),
                    e.frees, e.usedAfterFree, e.site, e.multiSite);
            }
        }
        ok &= ferror(f) == 0;
        ok &= fclose(f) == 0;
        return ok;
    }

    // Finds the granny2.dll import descriptor of `exe` and resolves our six slots. All-or-nothing.
    bool ResolveSlots(HMODULE exe)
    {
        auto* base = reinterpret_cast<BYTE*>(exe);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        g_exeBase = reinterpret_cast<uintptr_t>(base);
        g_exeEnd = g_exeBase + nt->OptionalHeader.SizeOfImage;
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) { Logger::Error("Granny observer: exe has no import directory."); return false; }
        HMODULE granny = GetModuleHandleW(L"granny2.dll");
        if (!granny) { Logger::Error("Granny observer: granny2.dll is not loaded."); return false; }
        for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
            if (_stricmp(reinterpret_cast<const char*>(base + d->Name), "granny2.dll") != 0) continue;
            if (!d->OriginalFirstThunk) { Logger::Error("Granny observer: no import name table for granny2.dll."); return false; }
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + d->OriginalFirstThunk);
            auto* iat = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + d->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++iat) {
                if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
                const char* name = reinterpret_cast<const char*>(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name);
                for (int i = 0; i < kFnCount; ++i) {
                    if (strcmp(name, kImportNames[i]) != 0) continue;
                    g_slots[i].iat = reinterpret_cast<void**>(&iat->u1.Function);
                    g_slots[i].iatRva = static_cast<uint32_t>(reinterpret_cast<BYTE*>(g_slots[i].iat) - base);
                }
            }
        }
        bool ok = true;
        for (int i = 0; i < kFnCount; ++i) {
            Slot& s = g_slots[i];
            if (!s.iat) { Logger::Error("Granny observer: import %s not found in the exe.", kImportNames[i]); ok = false; continue; }
            void* expected = reinterpret_cast<void*>(GetProcAddress(granny, kImportNames[i]));
            s.orig = *s.iat;
            s.origRva = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s.orig) - reinterpret_cast<uintptr_t>(granny));
            if (!expected || s.orig != expected) {
                Logger::Error("Granny observer: slot %s holds %p, granny2 export is %p (already redirected?); nothing patched.",
                    kImportNames[i], s.orig, expected);
                ok = false;
            }
        }
        return ok;
    }

    bool PatchSlot(Slot& s, void* value)
    {
        DWORD old = 0;
        if (!VirtualProtect(s.iat, sizeof(void*), PAGE_READWRITE, &old)) return false;
        InterlockedExchange(reinterpret_cast<volatile LONG*>(s.iat), static_cast<LONG>(reinterpret_cast<uintptr_t>(value)));
        DWORD dummy = 0;
        VirtualProtect(s.iat, sizeof(void*), old, &dummy);
        return true;
    }
}

bool GrannyObserver::Enabled() { return g_forceEnabled || Config::Get().granny; }
void GrannyObserver::Test_ForceEnabled(bool on) { g_forceEnabled = on; }

void** GrannyObserver::Test_FindSlot(HMODULE exe, int fn)
{
    if (g_installed) return nullptr;
    for (auto& s : g_slots) s = Slot{};
    ResolveSlots(exe);
    void** r = g_slots[fn].iat;
    for (auto& s : g_slots) s = Slot{};
    return r;
}
bool GrannyObserver::Installed() { return g_installed; }

int GrannyObserver::SlotsPatched()
{
    int n = 0;
    for (const Slot& s : g_slots) n += s.patched ? 1 : 0;
    return n;
}

bool GrannyObserver::Install(uint64_t (*frameFn)(), uint32_t (*markerFn)(), HMODULE exe)
{
    if (!Enabled()) return false;                       // off: no slot is touched
    if (g_installed) return true;
    g_frameFn = frameFn;
    g_markerFn = markerFn;
    for (auto& s : g_slots) s = Slot{};
    if (!ResolveSlots(exe ? exe : GetModuleHandleW(nullptr))) {
        Logger::Error("Granny observer: NOT installed (see above); the game runs unobserved.");
        return false;
    }
    g_origClock = reinterpret_cast<SetModelClock_t>(g_slots[kClock].orig);
    g_origSample = reinterpret_cast<SampleAccel_t>(g_slots[kSample].orig);
    g_origDeform = reinterpret_cast<DeformVertices_t>(g_slots[kDeform].orig);
    g_origComposite = reinterpret_cast<Composite_t>(g_slots[kComposite].orig);
    g_origFreeInstance = reinterpret_cast<Free_t>(g_slots[kFreeInstance].orig);
    g_origFreeDeformer = reinterpret_cast<Free_t>(g_slots[kFreeDeformer].orig);
    void* thunks[kFnCount] = { reinterpret_cast<void*>(&T_Clock), reinterpret_cast<void*>(&T_Sample), reinterpret_cast<void*>(&T_Deform),
        reinterpret_cast<void*>(&T_Composite), reinterpret_cast<void*>(&T_FreeInstance), reinterpret_cast<void*>(&T_FreeDeformer) };
    for (int i = 0; i < kFnCount; ++i) {
        g_slots[i].thunk = thunks[i];
        g_slots[i].patched = PatchSlot(g_slots[i], thunks[i]);
        Logger::Info("Granny observer: %s iat rva 0x%08X -> granny2+0x%08X: %s", kImportNames[i], g_slots[i].iatRva,
            g_slots[i].origRva, g_slots[i].patched ? "patched" : "PATCH FAILED");
    }
    g_installed = SlotsPatched() == kFnCount;
    if (!g_installed) { Uninstall(); Logger::Error("Granny observer: not all slots patched; all restored."); return false; }
    Logger::Info("Granny observer: installed (6 exe import slots; aggregates only while recording, render thread only).");
    return true;
}

void GrannyObserver::Uninstall()
{
    for (Slot& s : g_slots) {
        if (!s.patched) continue;
        if (*s.iat == s.thunk) PatchSlot(s, s.orig);
        else Logger::Warn("Granny observer: a slot changed since patching; left as it is.");
        s.patched = false;
    }
    if (g_installed) Logger::Info("Granny observer: import slots restored.");
    g_installed = false;
}

void GrannyObserver::SetOwnerTid(uint32_t tid) { g_owner.store(tid, std::memory_order_relaxed); }

void GrannyObserver::OnFrameEnd(uint64_t frame, uint32_t marker)
{
    if (!g_active.load(std::memory_order_acquire)) return;
    if (GetCurrentThreadId() != g_owner.load(std::memory_order_relaxed)) return;
    g_busy.store(1, std::memory_order_seq_cst);
    if (g_active.load(std::memory_order_seq_cst)) {
        Tables& t = *g_t;
        t.cur.frame = static_cast<uint32_t>(frame);
        t.cur.marker = marker;
        t.cur.partial = t.first ? 1 : 0;
        t.first = false;
        if (t.rowCount < kFrameRows) t.rows[t.rowCount++] = t.cur;
        else ++t.framesDropped;
        t.cur = FrameRow{};
    }
    g_busy.store(0, std::memory_order_release);
}

bool GrannyObserver::Begin(const std::wstring& base)
{
    g_wroteFiles = false;
    if (!g_installed) return false;
    void* mem = VirtualAlloc(nullptr, sizeof(Tables), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);   // zeroed
    if (!mem) { Logger::Error("Granny observer: could not allocate %zu bytes; this recording is not observed.", sizeof(Tables)); return false; }
    g_t = static_cast<Tables*>(mem);
    g_t->first = true;
    g_base = base;
    for (auto& c : g_foreign) c.store(0);
    g_firstForeignTid.store(0);
    g_active.store(true, std::memory_order_seq_cst);
    Logger::Info("Granny observer: recording (%zu KiB tables).", sizeof(Tables) / 1024);
    return true;
}

bool GrannyObserver::End()
{
    if (!g_t) return false;
    g_active.store(false, std::memory_order_seq_cst);
    const DWORD t0 = GetTickCount();
    while (g_busy.load(std::memory_order_seq_cst)) {
        if (GetTickCount() - t0 > 5000) {
            Logger::Error("Granny observer: a thunk stayed inside its bookkeeping for 5 s; tables kept, no files written.");
            g_t = nullptr;                              // leaked on purpose: a thunk may still use them
            return false;
        }
        Sleep(1);
    }
    Tables& t = *g_t;
    Totals tot;
    for (int i = 0; i < kFnCount; ++i) { tot.calls[i] = t.calls[i]; tot.foreign[i] = g_foreign[i].load(); }
    tot.firstForeignTid = g_firstForeignTid.load();
    tot.tableDrops = static_cast<uint64_t>(t.dropsInst) + t.dropsDef + t.dropsPose + t.dropsComp;
    tot.framesDropped = t.framesDropped;
    tot.frames = t.rowCount;
    g_lastTotals = tot;
    g_wroteFiles = WriteAll(t, tot);
    Logger::Info("Granny observer: %llu frames, calls clock %llu sample %llu deform %llu composite %llu free %llu/%llu, foreign-thread calls %llu/%llu/%llu, "
        "table drops %llu, frame rows dropped %llu, offset faults %u -> files %s.",
        tot.frames, tot.calls[kClock], tot.calls[kSample], tot.calls[kDeform], tot.calls[kComposite], tot.calls[kFreeInstance],
        tot.calls[kFreeDeformer], tot.foreign[kClock], tot.foreign[kSample], tot.foreign[kDeform], tot.tableDrops, tot.framesDropped,
        t.offsetFaults, g_wroteFiles ? "written" : "NOT written");
    VirtualFree(g_t, 0, MEM_RELEASE);
    g_t = nullptr;
    return g_wroteFiles;
}

bool GrannyObserver::WroteFiles() { return g_wroteFiles; }
GrannyObserver::Totals GrannyObserver::LastTotals() { return g_lastTotals; }
