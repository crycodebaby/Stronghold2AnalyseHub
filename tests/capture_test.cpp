// Validation of the capture pipeline without the game (SH2-Perf docs/DECISIONS.md D011):
//   1. normal recording: every pushed frame arrives once, in order; Push cost is printed
//   2. ring overflow (writer stalled): Push never waits, drops are counted, reported in
//      Status and latest.json (capture_ok=false, frames_dropped)
//   3. files cannot be opened: no recording, openFailed set, logged, no crash
//   4. write error mid-recording: flagged, rest counted as lost, recording ends cleanly
//   6-11 (D012): manifest finalized + hashes, interrupted, manifest write failures, game speed/pause, prepared record
//   5. lifecycle: several recordings in a row (distinct files), a late record from the previous
//      recording is not written into the next one
// Exit code 0 = all checks passed. Uses a temp output dir (SH2HUB_OUT).
#include "../Stronghold2AnalyseHub/core/capture.h"
#include "../Stronghold2AnalyseHub/core/config.h"
#include "../Stronghold2AnalyseHub/core/logger.h"
#include "../Stronghold2AnalyseHub/core/manifest.h"
#include "../Stronghold2AnalyseHub/core/provenance.h"

#include <string>

namespace
{
    int g_failures = 0;
    std::wstring g_dir;

#define CHECK(cond, ...) do { if (!(cond)) { ++g_failures; printf("FAIL line %d: %s  ", __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

    bool WaitFor(bool (*pred)(), DWORD timeoutMs = 5000)
    {
        const DWORD t0 = GetTickCount();
        while (!pred()) {
            if (GetTickCount() - t0 > timeoutMs) return false;
            Sleep(5);
        }
        return true;
    }
    bool IsRecording() { return Capture::Recording(); }
    bool NotRecording() { return !Capture::Recording(); }

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

    // Value of a numeric/boolean key in latest.json (-1 if missing; true=1, false=0).
    long long JsonValue(const std::string& json, const char* key)
    {
        const std::string k = std::string("\"") + key + "\": ";
        const size_t p = json.find(k);
        if (p == std::string::npos) return -1;
        const char* v = json.c_str() + p + k.size();
        if (!strncmp(v, "true", 4)) return 1;
        if (!strncmp(v, "false", 5)) return 0;
        return atoll(v);
    }

    std::string JsonString(const std::string& json, const char* key)
    {
        const std::string k = std::string("\"") + key + "\": \"";
        const size_t p = json.find(k);
        if (p == std::string::npos) return "";
        const size_t s = p + k.size();
        return json.substr(s, json.find('"', s) - s);
    }

    size_t Lines(const std::string& s)
    {
        size_t n = 0;
        for (char c : s) n += c == '\n';
        return n;
    }

    uint32_t Marker() { return 3; }

    // Pushes `count` records like the render thread does. Returns the longest single Push in us.
    double PushFrames(uint64_t firstFrame, uint32_t count, double* meanNs = nullptr, uint32_t forceGen = 0)
    {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        double worst = 0, sum = 0;
        for (uint32_t i = 0; i < count; ++i) {
            Capture::FrameRecord r;
            r.frame = firstFrame + i;
            QueryPerformanceCounter(&a);
            r.tick = a.QuadPart;
            r.gen = forceGen ? forceGen : Capture::ActiveGen();
            r.marker = Marker();
            r.frameMs = 16.667f;
            r.presentMs = 0.25f;
            r.counters.drawIndexed = 1000 + i % 7;
            if (r.gen) Capture::Push(r);
            QueryPerformanceCounter(&b);
            const double us = 1e6 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart);
            worst = us > worst ? us : worst;
            sum += us;
        }
        if (meanNs) *meanNs = 1000.0 * sum / count;
        return worst;
    }

    // Finds the single *_frames.csv created after `since` names; returns its path (empty if none).
    std::wstring NewestFrames()
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((g_dir + L"SH2Perf_*_frames.csv").c_str(), &fd);
        std::wstring best;
        FILETIME bestTime{};
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (CompareFileTime(&fd.ftLastWriteTime, &bestTime) >= 0) { bestTime = fd.ftLastWriteTime; best = fd.cFileName; }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        return best.empty() ? best : g_dir + best;
    }

    void Record(uint32_t frames, bool pace, std::wstring* csv, double* meanNs = nullptr)
    {
        Capture::RequestToggle();
        CHECK(WaitFor(IsRecording), "recording did not start");
        if (pace) {
            for (uint32_t i = 0; i < frames; i += 100) { PushFrames(i, 100, meanNs); Sleep(10); }
        }
        else {
            PushFrames(0, frames, meanNs);
        }
        Capture::RequestToggle();
        CHECK(WaitFor(NotRecording), "recording did not stop");
        if (csv) *csv = NewestFrames();
    }
}

int main()
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_dir = std::wstring(tmp) + L"sh2_capture_test_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
    CreateDirectoryW(g_dir.c_str(), nullptr);
    SetEnvironmentVariableW(L"SH2HUB_OUT", g_dir.c_str());

    Logger::Init(L"capture_test.log");
    Config::Init();
    Provenance::Start();
    CHECK(Provenance::Wait(30000), "software identity not hashed");
    Capture::Init(Marker);

    // 1. normal recording -------------------------------------------------------------------
    {
        std::wstring csv;
        double meanNs = 0;
        Record(3000, true, &csv, &meanNs);
        const std::string data = ReadFile(csv);
        CHECK(Lines(data) == 3001, "rows %zu", Lines(data) - 1);
        CHECK(data.rfind("frame,t_s,frame_ms,present_ms,endscene_ms,between_endscene_ms,overlay_ms,scenes,dip,", 0) == 0, "header");
        CHECK(data.find("\n2999,") != std::string::npos && data.find("\n0,") != std::string::npos, "first/last frame");
        const std::string json = ReadFile(g_dir + L"latest.json");
        CHECK(JsonValue(json, "capture_ok") == 1, "capture_ok");
        CHECK(JsonValue(json, "frames") == 3000, "frames %lld", JsonValue(json, "frames"));
        CHECK(JsonValue(json, "frames_dropped") == 0, "dropped");
        CHECK(JsonValue(json, "capture_version") == 3, "version");
        CHECK(JsonString(json, "threads_csv").find("_threads.csv") != std::string::npos, "threads_csv listed");
        printf("1. normal: 3000 frames written, mean Push cost %.0f ns\n", meanNs);
        CHECK(meanNs < 5000, "Push mean %.0f ns is too slow", meanNs);
    }

    // 2. ring overflow ----------------------------------------------------------------------
    {
        Capture::Test_StallWriter(true);
        Capture::RequestToggle();
        CHECK(WaitFor(IsRecording), "recording did not start");
        const uint32_t total = Capture::kRingSize * 3;
        const double worstUs = PushFrames(0, total);
        const Capture::Status s = Capture::GetStatus();
        printf("2. overflow: pushed %u into a ring of %u, dropped %llu, longest Push %.1f us\n",
            total, Capture::kRingSize, s.dropped, worstUs);
        CHECK(s.dropped == total - Capture::kRingSize, "dropped %llu", s.dropped);
        CHECK(worstUs < 2000, "a Push took %.1f us - the render thread must never wait", worstUs);
        Capture::Test_StallWriter(false);
        Capture::RequestToggle();
        CHECK(WaitFor(NotRecording), "recording did not stop");
        const std::string json = ReadFile(g_dir + L"latest.json");
        CHECK(JsonValue(json, "capture_ok") == 0, "capture_ok must be false after drops");
        CHECK(JsonValue(json, "frames_dropped") == total - Capture::kRingSize, "frames_dropped %lld", JsonValue(json, "frames_dropped"));
        CHECK(JsonValue(json, "frames") == Capture::kRingSize, "frames %lld", JsonValue(json, "frames"));
    }

    // 3. files cannot be opened -------------------------------------------------------------
    {
        const std::wstring bad = g_dir + L"missing\\deeper\\";
        SetEnvironmentVariableW(L"SH2HUB_OUT", bad.c_str());
        Capture::RequestToggle();
        CHECK(WaitFor([] { return Capture::GetStatus().openFailed; }), "openFailed not set");
        CHECK(!Capture::Recording(), "must not be recording");
        CHECK(Capture::ActiveGen() == 0, "render thread must see no active recording");
        printf("3. open failure: reported, not recording\n");
        SetEnvironmentVariableW(L"SH2HUB_OUT", g_dir.c_str());
    }

    // 4. write error mid-recording ----------------------------------------------------------
    {
        Capture::RequestToggle();
        CHECK(WaitFor(IsRecording), "recording did not start");
        PushFrames(0, 100);
        Sleep(100);
        Capture::Test_FailNextWrites(true);
        PushFrames(100, 200);
        CHECK(WaitFor([] { return Capture::GetStatus().writeFailed; }), "writeFailed not set");
        Capture::Test_FailNextWrites(false);
        Capture::RequestToggle();
        CHECK(WaitFor(NotRecording), "recording did not stop after a write error");
        const std::string json = ReadFile(g_dir + L"latest.json");
        CHECK(JsonValue(json, "capture_ok") == 0, "capture_ok must be false");
        CHECK(JsonValue(json, "write_errors") >= 1, "write_errors %lld", JsonValue(json, "write_errors"));
        CHECK(JsonValue(json, "frames_lost_write") > 0, "frames_lost_write %lld", JsonValue(json, "frames_lost_write"));
        printf("4. write error: flagged (write_errors=%lld, lost=%lld), recording closed\n",
            JsonValue(json, "write_errors"), JsonValue(json, "frames_lost_write"));
    }

    // 5. lifecycle --------------------------------------------------------------------------
    {
        std::wstring first, second, third;
        Record(300, false, &first);
        const uint32_t oldGen = 1;                              // any generation that is no longer active
        Capture::RequestToggle();                               // start #2 ...
        CHECK(WaitFor(IsRecording), "start #2");
        PushFrames(0, 5, nullptr, oldGen + 1000);               // ... with a stale record in the ring
        PushFrames(0, 250);
        Capture::RequestToggle();
        CHECK(WaitFor(NotRecording), "stop #2");
        second = NewestFrames();
        Record(100, false, &third);
        CHECK(!first.empty() && first != second && second != third && first != third, "recordings must not share a file");
        CHECK(Lines(ReadFile(first)) == 301, "first rows %zu", Lines(ReadFile(first)) - 1);
        CHECK(Lines(ReadFile(second)) == 251, "second rows %zu (stale record leaked?)", Lines(ReadFile(second)) - 1);
        CHECK(Lines(ReadFile(third)) == 101, "third rows %zu", Lines(ReadFile(third)) - 1);
        const std::string json = ReadFile(g_dir + L"latest.json");
        CHECK(JsonValue(json, "capture_ok") == 1, "capture_ok after clean recording following errors");
        printf("5. lifecycle: 3 recordings, 3 files, no leak between them\n");
    }

    // ---- Provenance (D012) ------------------------------------------------------------------
    const std::wstring ini = Provenance::HubDir() + L"SH2Proxy.ini";
    auto writeIni = [&](const std::string& plan) {
        FILE* f = nullptr;
        _wfopen_s(&f, ini.c_str(), L"wb");
        if (f) { fwrite(plan.data(), 1, plan.size(), f); fclose(f); }
    };
    auto manifestOf = [&](const std::wstring& framesCsv) {
        std::wstring m = framesCsv;
        m.replace(m.size() - 11, 11, L"_manifest.json");   // "_frames.csv"
        return m;
    };
    auto hasText = [](const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; };

    // 6. manifest of a normal recording: finalized after the files are closed, hashes match the files
    {
        std::wstring csv;
        Record(500, false, &csv);
        const std::string m = ReadFile(manifestOf(csv));
        CHECK(hasText(m, "\"schema\": \"sh2perf.capture-manifest\""), "schema");
        CHECK(hasText(m, "\"manifest_version\": 1"), "manifest_version");
        CHECK(hasText(m, "\"status\": \"finalized\""), "status finalized");
        CHECK(hasText(m, "\"stop_reason\": \"user_toggle\""), "stop reason");
        CHECK(hasText(m, "\"capture_ok\": true"), "capture_ok");
        CHECK(hasText(m, "\"files_closed\": true") && hasText(m, "\"hashes_complete\": true"), "closed + hashed");
        const std::string id = JsonString(m, "capture_id");
        CHECK(id.rfind("rec-", 0) == 0 && id.size() == 4 + 16 + 1 + 8, "capture id format %s", id.c_str());
        const std::string latest = ReadFile(g_dir + L"latest.json");
        CHECK(JsonString(latest, "capture_id") == id, "latest.json names the same recording");
        CHECK(JsonString(latest, "manifest_sha256") == Provenance::Sha256File(manifestOf(csv).c_str()), "latest.json manifest hash");
        const std::string framesHash = Provenance::Sha256File(csv.c_str());
        CHECK(framesHash.size() == 64 && hasText(m, framesHash.c_str()), "frames hash in manifest equals the file");
        CHECK(hasText(m, "\"role\": \"game_exe\"") && hasText(m, "\"role\": \"hub_dll\""), "software identity listed");
        CHECK(hasText(m, "\"basis\": \"hub_session_start\""), "software basis");
        CHECK(hasText(m, "\"game_speed\": {\"value\": null, \"origin\": \"unknown\""), "missing speed stays unknown");
        CHECK(hasText(m, "\"paused\": {\"value\": null, \"origin\": \"unknown\"}"), "missing pause stays unknown");
        CHECK(hasText(m, "\"benchmark_suitability\": {\"verdict\": \"unknown\""), "suitability not judged by the hub");
        CHECK(hasText(m, "\"counters\": {\"value\": true, \"source\": \"default\"}"), "switch with source");
        CHECK(GetFileAttributesW((manifestOf(csv) + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "no temp manifest left");
        printf("6. manifest: finalized, id %s, hashes match the files\n", id.c_str());
    }

    // 7. interrupted recording: the manifest says "recording" while it runs and stays so after a crash
    {
        Capture::RequestToggle();
        CHECK(WaitFor(IsRecording), "recording did not start");
        PushFrames(0, 120);
        Sleep(150);
        const std::wstring csv = NewestFrames();
        const std::string running = ReadFile(manifestOf(csv));
        CHECK(hasText(running, "\"status\": \"recording\""), "status while recording");
        CHECK(hasText(running, "\"capture_ok\": null"), "quality unknown while recording");
        CHECK(hasText(running, "\"files_closed\": false"), "files not closed yet");
        Capture::Test_AbandonRecording();
        CHECK(WaitFor(NotRecording), "abandon");
        const std::string after = ReadFile(manifestOf(csv));
        CHECK(hasText(after, "\"status\": \"recording\""), "interrupted recording keeps status recording");
        CHECK(!hasText(after, "\"stop_utc\": \""), "no stop time published");
        CHECK(GetFileAttributesW(csv.c_str()) != INVALID_FILE_ATTRIBUTES, "interrupted CSV is kept");
        printf("7. interrupted: status stays recording, files kept\n");
        Sleep(1100);
    }

    // 8. final manifest cannot be written: not published as complete, latest.json carries no manifest
    {
        Capture::RequestToggle();
        CHECK(WaitFor(IsRecording), "recording did not start");
        PushFrames(0, 50);
        Sleep(100);
        const std::wstring csv = NewestFrames();
        Capture::Test_FailManifest(true);
        Capture::RequestToggle();
        CHECK(WaitFor(NotRecording), "stop");
        Capture::Test_FailManifest(false);
        CHECK(hasText(ReadFile(manifestOf(csv)), "\"status\": \"recording\""), "failed final write must not publish finalized");
        const std::string latest = ReadFile(g_dir + L"latest.json");
        CHECK(JsonValue(latest, "capture_ok") == 0 && hasText(latest, "\"manifest\": null"), "latest.json flags it");
        CHECK(Capture::GetStatus().manifestFailed, "status.manifestFailed");
        printf("8. manifest write failure: not published as finalized\n");
        Sleep(1100);
    }

    // 9. manifest cannot be written at start: recording refused, nothing left behind
    {
        auto countFiles = [&]() {
            size_t n = 0;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((g_dir + L"SH2Perf_*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) { do { ++n; } while (FindNextFileW(h, &fd)); FindClose(h); }
            return n;
        };
        const size_t before = countFiles();
        Capture::Test_FailManifest(true);
        Capture::RequestToggle();
        CHECK(WaitFor([] { return Capture::GetStatus().openFailed; }), "start must be refused");
        Capture::Test_FailManifest(false);
        CHECK(!Capture::Recording(), "not recording");
        const size_t after = countFiles();
        CHECK(before == after, "files before %zu after %zu: a refused start must not leave a CSV without a manifest", before, after);
        printf("9. manifest write failure at start: refused, %zu files unchanged\n", after);
        Sleep(1100);                                              // next recording gets a fresh stem
    }

    // 10. game speed 30 vs 90 vs missing vs invalid; pause is independent of speed
    {
        struct Case { const char* plan; const char* speed; const char* paused; };
        const Case cases[] = {
            { "[plan]\r\ngame_speed=30\r\nexperiment=EXP-X\r\nprotocol=Real save\r\n", "\"value\": 30, \"origin\": \"planned\"", "\"paused\": {\"value\": null" },
            { "[plan]\r\ngame_speed=90\r\npaused=1\r\n", "\"value\": 90, \"origin\": \"planned\"", "\"paused\": {\"value\": true, \"origin\": \"planned\"}" },
            { "[plan]\r\npaused=0\r\n", "\"game_speed\": {\"value\": null, \"origin\": \"unknown\"", "\"paused\": {\"value\": false, \"origin\": \"planned\"}" },
            { "[plan]\r\ngame_speed=fast\r\n", "\"rejected_text\": \"fast\"", "\"paused\": {\"value\": null" },
            { "[plan]\r\ngame_speed=1.5\r\n", "\"value\": 1.5, \"origin\": \"planned\"", "\"paused\": {\"value\": null" },
        };
        std::string ids[5];
        int i = 0;
        for (const Case& c : cases) {
            writeIni(c.plan);
            Sleep(1100);
            std::wstring csv;
            Record(60, false, &csv);
            const std::string m = ReadFile(manifestOf(csv));
            CHECK(hasText(m, c.speed), "case %d speed: %s", i, c.speed);
            CHECK(hasText(m, c.paused), "case %d paused: %s", i, c.paused);
            CHECK(hasText(m, "\"meaning\": \"unspecified\""), "case %d meaning not guessed", i);
            ids[i] = JsonString(m, "capture_id");
            if (i == 0) CHECK(hasText(m, "\"experiment\": \"EXP-X\"") && hasText(m, "\"protocol\": \"Real save\""), "plan experiment/protocol");
            if (i == 0) CHECK(hasText(m, "\"source_ini_sha256\": \""), "plan names the ini hash");
            ++i;
        }
        CHECK(ids[0] != ids[1] && ids[1] != ids[2], "capture ids differ");
        DeleteFileW(ini.c_str());
        printf("10. game speed: 30 / 90 / missing / invalid / 1.5 recorded as written, pause separate\n");
    }

    // 11. prepared-start record: copied next to the CSVs and listed with a hash when present
    {
        const std::wstring prepared = Provenance::HubDir() + L"SH2Prepared.json";
        FILE* f = nullptr;
        _wfopen_s(&f, prepared.c_str(), L"wb");
        const char body[] = "{\"prepared_utc\": \"2026-01-01T00:00:00Z\"}\n";
        if (f) { fwrite(body, 1, sizeof(body) - 1, f); fclose(f); }
        Sleep(1100);
        std::wstring csv;
        Record(40, false, &csv);
        const std::string m = ReadFile(manifestOf(csv));
        CHECK(hasText(m, "\"role\": \"prepared\""), "prepared record listed");
        CHECK(hasText(m, Provenance::Sha256File(prepared.c_str()).c_str()), "prepared hash");
        DeleteFileW(prepared.c_str());
        printf("11. prepared record copied and hashed\n");
    }

    Capture::Shutdown();
    Logger::Shutdown();
    printf(g_failures ? "capture_test: %d FAILED\n" : "capture_test: all checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
