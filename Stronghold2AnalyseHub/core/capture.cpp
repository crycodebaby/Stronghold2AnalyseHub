#include "capture.h"
#include "config.h"
#include "logger.h"
#include "manifest.h"
#include "provenance.h"
#include "sampler.h"

#include <cerrno>
#include <exception>

#ifndef HUB_GIT_COMMIT
#define HUB_GIT_COMMIT "unknown"
#endif

// -----------------------------------------------------------------------------
// Implementation notes:
// - Ring: one producer (render thread), one consumer (capture thread). head/tail are 32-bit
//   free-running counters (wrap-safe); head is published with release after the slot is
//   written, tail with release after the slot was read. The producer only reads tail.
// - Records carry the recording generation. A record that arrives after Stop (render thread was
//   mid-push) has a stale generation and is discarded by the next drain.
// - Everything that can block (file open/write/flush, Toolhelp snapshot, GetThreadTimes,
//   GlobalMemoryStatusEx, sampler CSV, latest.json) runs on the capture thread.
// -----------------------------------------------------------------------------

namespace
{
    using Capture::FrameRecord;
    constexpr uint32_t kMask = Capture::kRingSize - 1;
    static_assert((Capture::kRingSize & kMask) == 0, "ring size must be a power of two");

    FrameRecord g_ring[Capture::kRingSize];
    std::atomic<uint32_t> g_head{ 0 };
    std::atomic<uint32_t> g_tail{ 0 };
    std::atomic<uint32_t> g_pushed{ 0 };          // producer-only writer
    std::atomic<uint32_t> g_dropped{ 0 };         // producer-only writer
    std::atomic<uint32_t> g_activeGen{ 0 };
    std::atomic<uint32_t> g_va{ 0 };
    std::atomic<uint32_t> g_renderTid{ 0 };

    std::atomic<bool> g_toggle{ false };
    std::atomic<bool> g_running{ false };
    HANDLE g_wake = nullptr;                      // never closed while the process lives: the capture thread and RequestToggle may still use it
    HANDLE g_thread = nullptr;
    uint32_t (*g_markerFn)() = nullptr;

    // Status for readers on other threads.
    std::atomic<bool>     g_recording{ false };
    std::atomic<bool>     g_openFailed{ false };
    std::atomic<bool>     g_writeFailed{ false };
    std::atomic<uint64_t> g_written{ 0 };
    std::atomic<uint32_t> g_writeErrors{ 0 };
    std::atomic<uint32_t> g_droppedAtStart{ 0 };
    std::atomic<uint32_t> g_pushedAtStart{ 0 };

    std::atomic<bool> g_manifestFailed{ false };
    std::atomic<bool> g_abandon{ false };
    std::atomic<bool> g_testStall{ false };
    std::atomic<bool> g_testBlockWrite{ false };
    std::atomic<bool> g_threadAlive{ false };
    std::atomic<bool> g_testFail{ false };

    // Capture-thread state of the current recording.
    struct Rec
    {
        bool     active = false;
        uint32_t gen = 0;
        FILE*    frames = nullptr;
        FILE*    threads = nullptr;
        bool     failed = false;                  // a write failed; later rows are counted as lost
        int64_t  startTick = 0;
        int64_t  lastHousekeeping = 0;
        uint64_t startFrame = 0;
        uint64_t lastFrame = 0;
        bool     haveFrame = false;
        uint64_t lost = 0;                        // rows not written because of write errors
        uint32_t droppedReported = 0;
        std::string stem, startIso;
        std::string captureId, startUtc;
        bool hasPrepared = false;                 // SH2Prepared.json was copied next to the CSVs
        Manifest::Plan plan;
        bool softwarePending = false;
        bool samples = false;
        std::vector<std::pair<DWORD, uint64_t>> threadPrev;
        int64_t lastThreadTick = 0;
    } g_rec;

    uint32_t g_nextGen = 0;
    LARGE_INTEGER g_freq{};

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

    std::string LocalIso()
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char buf[32];
        snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u", st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);
        return buf;
    }

    // One write with error accounting. After the first failure the recording is marked failed.
    bool WriteBytes(FILE* f, const char* data, size_t n, const char* what)
    {
        while (g_testBlockWrite.load()) Sleep(5);        // test hook: a writer stuck inside a file operation
        if (g_rec.failed) return false;
        const bool injected = g_testFail.load(std::memory_order_relaxed);
        if (injected || fwrite(data, 1, n, f) != n) {
            g_rec.failed = true;
            g_writeFailed.store(true);
            g_writeErrors.fetch_add(1);
            Logger::Error("Capture: write to %s failed (errno %d%s); the rest of this recording is lost.",
                what, errno, injected ? ", injected" : "");
            return false;
        }
        return true;
    }

    void Tone(unsigned freq)
    {
        if (Config::Get().beep) ::Beep(freq, 40);
    }

    double VaUsedMbNow()
    {
        MEMORYSTATUSEX ms{ sizeof(ms) };
        if (!GlobalMemoryStatusEx(&ms)) return 0;
        return static_cast<double>(ms.ullTotalVirtual - ms.ullAvailVirtual) / (1024.0 * 1024.0);
    }

    // Per-thread CPU since the previous call (100 ns units vs wall clock); writes rows if `write`.
    void SampleThreads(int64_t now, bool write)
    {
        const double intervalMs = ToMs(now - g_rec.lastThreadTick);
        g_rec.lastThreadTick = now;

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

        if (write && g_rec.threads && intervalMs > 0) {
            const double tS = ToMs(now - g_rec.startTick) / 1000.0;
            const uint32_t renderTid = g_renderTid.load();
            std::string out;
            for (const auto& [tid, total] : current) {
                for (const auto& [ptid, ptotal] : g_rec.threadPrev) {
                    if (ptid != tid) continue;
                    const double pct = 100.0 * (static_cast<double>(total - ptotal) / 10000.0) / intervalMs;
                    if (pct >= 0.5) {
                        char row[96];
                        const int n = snprintf(row, sizeof(row), "%.3f,%lu,%.1f,%d\n", tS, tid, pct,
                            tid == renderTid ? 1 : 0);
                        out.append(row, static_cast<size_t>(n));
                    }
                    break;
                }
            }
            if (!out.empty()) WriteBytes(g_rec.threads, out.data(), out.size(), "threads CSV");
        }
        g_rec.threadPrev = std::move(current);
    }

    void WriteRow(const FrameRecord& r)
    {
        if (g_rec.failed) { ++g_rec.lost; return; }
        char row[512];
        const auto& c = r.counters;
        const double tS = r.tick > g_rec.startTick ? ToMs(r.tick - g_rec.startTick) / 1000.0 : 0.0;
        const int n = snprintf(row, sizeof(row),
            "%llu,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%u,%llu,%u,%u,%u,%u,%u,%u,%u\n",
            r.frame, tS, r.frameMs, r.presentMs, r.endSceneMs, r.betweenEndSceneMs, r.overlayMs,
            c.scenes, c.drawIndexed, c.draw, c.drawUP, c.drawIndexedUP, c.primitives, c.setTexture,
            c.vsConstCalls, c.vsConstVec4, c.psConstCalls, c.psConstVec4, r.vaUsedMb, r.marker);
        if (WriteBytes(g_rec.frames, row, static_cast<size_t>(n), "frames CSV")) {
            g_written.fetch_add(1);
            g_rec.lastFrame = r.frame;
            g_rec.haveFrame = true;
        }
        else {
            ++g_rec.lost;
        }
    }

    void Drain()
    {
        uint32_t tail = g_tail.load(std::memory_order_relaxed);
        const uint32_t head = g_head.load(std::memory_order_acquire);
        while (tail != head) {
            const FrameRecord& r = g_ring[tail & kMask];
            if (g_rec.active && r.gen == g_rec.gen) WriteRow(r);
            ++tail;
        }
        g_tail.store(tail, std::memory_order_release);
    }

    // latest.json is only a pointer to the newest finished recording. Identity, switches and software
    // live in the manifest (D012); the legacy keys stay so older readers keep working.
    void WriteLatestJson(bool ok, uint64_t frames, uint64_t dropped, const std::string& manifestSha)
    {
        const std::wstring dir = Logger::OutputDir();
        const std::wstring tmp = dir + L"latest.json.tmp", dst = dir + L"latest.json";
        FILE* f = nullptr;
        _wfopen_s(&f, tmp.c_str(), L"wb");
        if (!f) { Logger::Error("Capture: could not write latest.json."); return; }
        const Config::Flags& m = Config::Get();
        const std::string manifestName = manifestSha.empty() ? std::string("null") : "\"" + g_rec.stem + "_manifest.json\"";
        const std::string manifestHash = manifestSha.empty() ? std::string("null") : "\"" + manifestSha + "\"";
        const std::string threadsKey = m.poll ? "  \"threads_csv\": \"" + g_rec.stem + "_threads.csv\",\n" : std::string();
        const std::string samplesKey = g_rec.samples ? "  \"samples_csv\": \"" + g_rec.stem + "_samples.csv\",\n" : std::string();
        fprintf(f,
            "{\n"
            "  \"frames_csv\": \"%s_frames.csv\",\n"
            "%s"
            "  \"log\": \"Stronghold2AnalyseHub.log\",\n"
            "  \"start\": \"%s\",\n"
            "  \"stop\": \"%s\",\n"
            "  \"frames\": %llu,\n"
            "  \"markers\": %u,\n"
            "%s"
            "  \"capture_version\": 3,\n"
            "  \"capture_id\": \"%s\",\n"
            "  \"manifest\": %s,\n"
            "  \"manifest_sha256\": %s,\n"
            "  \"capture_ok\": %s,\n"
            "  \"frames_dropped\": %llu,\n"
            "  \"frames_lost_write\": %llu,\n"
            "  \"write_errors\": %u,\n"
            "  \"modes\": {\"frametime\": %s, \"counters\": %s, \"poll\": %s, \"overlay\": %s, \"sample\": %s},\n"
            "  \"hub_commit\": \"%s\"\n"
            "}\n",
            g_rec.stem.c_str(), threadsKey.c_str(),
            g_rec.startIso.c_str(), LocalIso().c_str(), frames, g_markerFn ? g_markerFn() : 0,
            samplesKey.c_str(), g_rec.captureId.c_str(), manifestName.c_str(), manifestHash.c_str(),
            ok ? "true" : "false", dropped, g_rec.lost, g_writeErrors.load(),
            m.frameTime ? "true" : "false", m.counters ? "true" : "false", m.poll ? "true" : "false",
            m.overlay ? "true" : "false", m.sample ? "true" : "false", HUB_GIT_COMMIT);
        const bool wrote = ferror(f) == 0;
        const bool closed = fclose(f) == 0;
        if (!wrote || !closed || !MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING))
            Logger::Error("Capture: latest.json not written completely (%lu).", GetLastError());
    }

    void CloseFiles()
    {
        if (g_rec.frames) { if (fclose(g_rec.frames) != 0) g_writeErrors.fetch_add(1); }
        if (g_rec.threads) { if (fclose(g_rec.threads) != 0) g_writeErrors.fetch_add(1); }
        g_rec.frames = g_rec.threads = nullptr;
    }

    std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }   // stems and names are ASCII

    // The capture files of the current recording, with sizes and SHA-256 when `hash` is set.
    // Hashing reads the closed files on the capture thread; it can take a moment for large recordings.
    std::vector<Manifest::FileEntry> CaptureFiles(const std::wstring& dir, bool hash, bool* allHashed)
    {
        std::vector<Manifest::FileEntry> files;
        auto add = [&](const char* role, const std::string& name) {
            Manifest::FileEntry e;
            e.role = role;
            e.name = name;
            e.present = true;
            if (hash) {
                e.sha256 = Provenance::Sha256File((dir + Wide(name)).c_str(), &e.size);
                if (e.sha256.empty()) { e.present = false; if (allHashed) *allHashed = false; }
            }
            files.push_back(std::move(e));
        };
        add("frames", g_rec.stem + "_frames.csv");
        if (Config::Get().poll) add("threads", g_rec.stem + "_threads.csv");
        if (g_rec.samples) add("samples", g_rec.stem + "_samples.csv");
        if (g_rec.hasPrepared) add("prepared", g_rec.stem + "_prepared.json");
        return files;
    }

    Manifest::Input BaseManifest(bool finalized)
    {
        Manifest::Input in;
        in.captureId = g_rec.captureId;
        in.stem = g_rec.stem;
        in.startUtc = g_rec.startUtc;
        in.startLocal = g_rec.startIso;
        in.plan = g_rec.plan;
        in.finalized = finalized;
        in.softwarePending = g_rec.softwarePending;
        return in;
    }

    void Stop(const char* reason)
    {
        if (!g_rec.active) return;
        g_activeGen.store(0, std::memory_order_release);   // render thread stops pushing
        Drain();                                   // frames pushed before the switch
        if (g_rec.frames && !g_rec.failed && fflush(g_rec.frames) != 0) {
            g_rec.failed = true;
            g_writeFailed.store(true);
            g_writeErrors.fetch_add(1);
            Logger::Error("Capture: final flush of frames CSV failed (errno %d).", errno);
        }
        g_rec.samples = Sampler::End();
        CloseFiles();
        g_rec.active = false;

        const std::wstring dir = Logger::OutputDir();
        const uint32_t dropped = g_dropped.load() - g_droppedAtStart.load();
        const uint64_t written = g_written.load();

        // Finalize: the manifest is published only now, after every capture file is closed and hashed.
        // If it cannot be written, the "recording" manifest from the start stays: the recording then
        // looks interrupted to the importer, never complete.
        bool hashed = true;
        Manifest::Input in = BaseManifest(true);
        in.files = CaptureFiles(dir, true, &hashed);
        in.stopUtc = Provenance::NowUtcIso();
        in.stopLocal = LocalIso();
        in.stopReason = reason;
        in.frames = written;
        in.markers = g_markerFn ? g_markerFn() : 0;
        in.framesDropped = dropped;
        in.framesLostWrite = g_rec.lost;
        in.writeErrors = g_writeErrors.load();
        in.filesClosed = true;
        in.hashesOk = hashed;
        in.softwarePending = !Provenance::Wait(0);
        in.captureOk = !g_rec.failed && dropped == 0 && g_writeErrors.load() == 0 && hashed;
        std::string manifestSha;
        const bool manifestOk = Manifest::Write(dir, in, &manifestSha) && !manifestSha.empty();
        if (!manifestOk) {
            g_manifestFailed.store(true);
            g_writeErrors.fetch_add(1);
            manifestSha.clear();
        }

        const bool ok = in.captureOk && manifestOk && g_writeErrors.load() == 0;
        WriteLatestJson(ok, written, dropped, manifestSha);
        Logger::Info("Recording stopped (%s): %llu frames written, %u dropped (ring full), %llu lost to write errors, %u write errors, manifest %s -> %s.",
            reason, written, dropped, g_rec.lost, g_writeErrors.load(), manifestOk ? "published" : "NOT published",
            ok ? "capture OK" : "CAPTURE INCOMPLETE");
        g_recording.store(false);                  // files, manifest and latest.json are complete from here on
        Tone(440);
    }

    // A start that fails leaves nothing behind: the files it just created are removed again so no
    // CSV without a manifest sits in the output directory.
    void DiscardStartFiles(const std::wstring& base)
    {
        CloseFiles();
        DeleteFileW((base + L"_frames.csv").c_str());
        DeleteFileW((base + L"_threads.csv").c_str());
        DeleteFileW((base + L"_prepared.json").c_str());
        DeleteFileW((base + L"_manifest.json").c_str());
        DeleteFileW((base + L"_manifest.json.tmp").c_str());
    }

    void Start()
    {
        g_manifestFailed.store(false);
        g_openFailed.store(false);
        g_writeFailed.store(false);
        g_written.store(0);
        g_writeErrors.store(0);

        const std::wstring dir = Logger::OutputDir();
        // Name = local start time to the second (tools/ingest.py parses it). A second recording
        // within the same second must not overwrite the first: move on to the next free second.
        SYSTEMTIME st;
        GetLocalTime(&st);
        FILETIME ft;
        SystemTimeToFileTime(&st, &ft);
        char stem[64];
        for (int attempt = 0; attempt < 60; ++attempt) {
            ULARGE_INTEGER u;
            u.LowPart = ft.dwLowDateTime;
            u.HighPart = ft.dwHighDateTime;
            u.QuadPart += static_cast<ULONGLONG>(attempt) * 10000000ULL;
            FILETIME t;
            t.dwLowDateTime = u.LowPart;
            t.dwHighDateTime = u.HighPart;
            SYSTEMTIME s2;
            FileTimeToSystemTime(&t, &s2);
            snprintf(stem, sizeof(stem), "SH2Perf_%04u%02u%02u_%02u%02u%02u", s2.wYear, s2.wMonth, s2.wDay,
                s2.wHour, s2.wMinute, s2.wSecond);
            const std::string probe(stem);
            const std::wstring frames = dir + std::wstring(probe.begin(), probe.end()) + L"_frames.csv";
            if (GetFileAttributesW(frames.c_str()) == INVALID_FILE_ATTRIBUTES) break;
        }
        g_rec = Rec{};
        g_rec.stem = stem;
        g_rec.startIso = LocalIso();
        g_rec.startUtc = Provenance::NowUtcIso();
        g_rec.captureId = "rec-" + Provenance::NowUtcCompact() + "-" + Provenance::RandomHex(4);
        const std::wstring base = dir + std::wstring(g_rec.stem.begin(), g_rec.stem.end());

        const errno_t e1 = _wfopen_s(&g_rec.frames, (base + L"_frames.csv").c_str(), L"wb");
        errno_t e2 = 0;
        if (Config::Get().poll) e2 = _wfopen_s(&g_rec.threads, (base + L"_threads.csv").c_str(), L"wb");
        if (e1 || e2 || !g_rec.frames || (Config::Get().poll && !g_rec.threads)) {
            Logger::Error("Capture: could not open capture files (errno %d/%d); recording NOT started.", e1, e2);
            g_openFailed.store(true);
            DiscardStartFiles(base);
            Tone(220);
            return;
        }
        setvbuf(g_rec.frames, nullptr, _IOFBF, 1 << 16);
        static const char kHeader[] =
            "frame,t_s,frame_ms,present_ms,endscene_ms,between_endscene_ms,overlay_ms,"
            "scenes,dip,dp,dpup,dipup,prims,settex,vsconst_calls,vsconst_vec4,"
            "psconst_calls,psconst_vec4,va_used_mb,marker\n";
        bool ok = WriteBytes(g_rec.frames, kHeader, sizeof(kHeader) - 1, "frames CSV");
        if (ok && g_rec.threads) {
            static const char kThreads[] = "t_s,tid,cpu_pct,is_render_thread\n";
            ok = WriteBytes(g_rec.threads, kThreads, sizeof(kThreads) - 1, "threads CSV");
        }
        if (!ok) {
            g_openFailed.store(true);
            DiscardStartFiles(base);
            Tone(220);
            return;
        }

        // Identity first: the "recording" manifest is on disk before the first frame is written, so an
        // interrupted recording is recognisable. Without it the recording does not start.
        g_rec.softwarePending = !Provenance::Wait(10000);
        if (g_rec.softwarePending) Logger::Warn("Capture: software identity not hashed yet; the manifest marks it pending.");
        g_rec.plan = Manifest::ReadPlan();
        const std::wstring preparedSrc = Provenance::HubDir() + L"SH2Prepared.json";
        g_rec.hasPrepared =
            CopyFileW(preparedSrc.c_str(), (base + L"_prepared.json").c_str(), FALSE) != 0;
        {
            Manifest::Input in = BaseManifest(false);
            in.files = CaptureFiles(dir, false, nullptr);
            if (!Manifest::Write(dir, in, nullptr)) {
                Logger::Error("Capture: could not write the manifest; recording NOT started.");
                g_openFailed.store(true);
                g_manifestFailed.store(true);
                DiscardStartFiles(base);
                Tone(220);
                return;
            }
        }

        g_rec.gen = ++g_nextGen;
        g_rec.startTick = Now();
        g_rec.lastHousekeeping = g_rec.startTick;
        g_rec.lastThreadTick = g_rec.startTick;
        g_droppedAtStart.store(g_dropped.load());
        g_pushedAtStart.store(g_pushed.load());
        if (Config::Get().poll) {                  // baseline for the first CPU interval and first VA value
            SampleThreads(g_rec.startTick, false);
            g_va.store(static_cast<uint32_t>(VaUsedMbNow() + 0.5));
        }
        Sampler::Begin(base + L"_samples.csv", g_markerFn);
        g_rec.active = true;
        g_recording.store(true);
        g_activeGen.store(g_rec.gen, std::memory_order_release);
        Logger::Info("Recording started (%s, %s).", g_rec.stem.c_str(), g_rec.captureId.c_str());
        Tone(880);
    }

    // About once per second while recording.
    void Housekeeping()
    {
        const int64_t now = Now();
        if (ToMs(now - g_rec.lastHousekeeping) < 1000.0) return;
        g_rec.lastHousekeeping = now;

        if (Config::Get().poll) {
            SampleThreads(now, true);
            g_va.store(static_cast<uint32_t>(VaUsedMbNow() + 0.5));
        }
        if (g_rec.frames && !g_rec.failed && fflush(g_rec.frames) != 0) {
            g_rec.failed = true;
            g_writeFailed.store(true);
            g_writeErrors.fetch_add(1);
            Logger::Error("Capture: flush of frames CSV failed (errno %d); the rest of this recording is lost.", errno);
        }
        const uint32_t dropped = g_dropped.load() - g_droppedAtStart.load();
        if (dropped != g_rec.droppedReported) {
            Logger::Warn("Capture: %u frames dropped so far (ring full, the render thread was not delayed).", dropped);
            g_rec.droppedReported = dropped;
        }
    }

    DWORD WINAPI WriterMain(LPVOID)
    {
        g_threadAlive.store(true);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        while (g_running.load()) {
            WaitForSingleObject(g_wake, 50);
            if (g_abandon.exchange(false)) {          // test only: behave like a killed process
                if (g_rec.frames) { fflush(g_rec.frames); }
                CloseFiles();
                g_activeGen.store(0);
                g_rec.active = false;
                g_recording.store(false);
                continue;
            }
            try {
                if (g_toggle.exchange(false)) {
                    if (g_rec.active) Stop("user_toggle"); else Start();
                }
                if (!g_testStall.load()) Drain();
                if (g_rec.active) Housekeeping();
            }
            catch (const std::exception& e) {
                Logger::Error("Capture: exception on capture thread: %s", e.what());
            }
            catch (...) {
                Logger::Error("Capture: unknown exception on capture thread.");
            }
        }
        try { Stop("hub_shutdown"); } catch (...) {}
        g_threadAlive.store(false);
        return 0;
    }
}

void Capture::Init(uint32_t (*markerFn)())
{
    if (g_thread) { Logger::Warn("Capture: Init called while the capture thread still exists; ignored."); return; }
    QueryPerformanceFrequency(&g_freq);
    g_markerFn = markerFn;
    if (!g_wake) g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_running.store(true);
    g_thread = CreateThread(nullptr, 0, WriterMain, nullptr, 0, nullptr);
    if (!g_thread) {
        g_running.store(false);
        Logger::Error("Capture: could not start the capture thread (%lu); recording unavailable.", GetLastError());
    }
}

bool Capture::Shutdown(DWORD timeoutMs)
{
    if (!g_thread) return true;
    g_running.store(false);
    g_activeGen.store(0, std::memory_order_release);   // the render thread stops handing over frames
    if (g_wake) SetEvent(g_wake);
    if (WaitForSingleObject(g_thread, timeoutMs) != WAIT_OBJECT_0) {
        // A blocked writer. Closing its handles or letting the DLL go would pull the code from under it, so nothing is
        // released; the caller keeps the hub loaded and asks again (SH2-Perf D012).
        Logger::Error("Capture: the capture thread is still running after %lu ms (blocked in a file operation?); nothing is released and the hub stays loaded.", timeoutMs);
        return false;
    }
    CloseHandle(g_thread);
    g_thread = nullptr;
    return true;
}

bool Capture::ThreadRunning() { return g_thread != nullptr && g_threadAlive.load(); }

uint32_t Capture::ActiveGen() { return g_activeGen.load(std::memory_order_acquire); }

void Capture::Push(const FrameRecord& record)
{
    const uint32_t head = g_head.load(std::memory_order_relaxed);
    const uint32_t tail = g_tail.load(std::memory_order_acquire);
    if (head - tail >= kRingSize) {                // full: never wait, count the loss
        g_dropped.store(g_dropped.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        return;
    }
    g_ring[head & kMask] = record;
    g_pushed.store(g_pushed.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    g_head.store(head + 1, std::memory_order_release);
}

void Capture::SetRenderTid(uint32_t tid) { g_renderTid.store(tid, std::memory_order_relaxed); }

void Capture::RequestToggle()
{
    g_toggle.store(true);
    if (g_wake) SetEvent(g_wake);
}

bool Capture::Recording() { return g_recording.load(); }
uint32_t Capture::VaUsedMb() { return g_va.load(std::memory_order_relaxed); }

Capture::Status Capture::GetStatus()
{
    Status s;
    s.recording = g_recording.load();
    s.openFailed = g_openFailed.load();
    s.writeFailed = g_writeFailed.load();
    s.pushed = g_pushed.load() - g_pushedAtStart.load();
    s.written = g_written.load();
    s.dropped = g_dropped.load() - g_droppedAtStart.load();
    s.writeErrors = g_writeErrors.load();
    s.manifestFailed = g_manifestFailed.load();
    return s;
}

void Capture::Test_StallWriter(bool stall) { g_testStall.store(stall); }
void Capture::Test_BlockWrites(bool block) { g_testBlockWrite.store(block); }
bool Capture::Test_ThreadAlive() { return g_threadAlive.load(); }
bool Capture::Test_WakeHandleValid() { DWORD f = 0; return g_wake && GetHandleInformation(g_wake, &f); }
void Capture::Test_FailNextWrites(bool fail) { g_testFail.store(fail); }
void Capture::Test_FailManifest(bool fail) { Manifest::Test_FailWrites(fail); }
void Capture::Test_AbandonRecording()
{
    // Simulates a crash: the capture thread is gone, files stay as they are, no final manifest.
    g_abandon.store(true);
    if (g_wake) SetEvent(g_wake);
}
