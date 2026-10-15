#include "manifest.h"
#include "config.h"
#include "logger.h"
#include "provenance.h"

#include <cmath>
#include <cstdlib>

#ifndef HUB_GIT_COMMIT
#define HUB_GIT_COMMIT "unknown"
#endif

namespace
{
    std::atomic<bool> g_failWrites{ false };

    using Provenance::JsonEscape;

    std::string Q(const std::string& s) { return "\"" + JsonEscape(s) + "\""; }
    std::string QOrNull(const std::string& s) { return s.empty() ? "null" : Q(s); }
    const char* B(bool v) { return v ? "true" : "false"; }

    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return std::wstring();
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    std::string IniValue(const std::wstring& ini, const wchar_t* key)
    {
        wchar_t v[256]{};
        GetPrivateProfileStringW(L"plan", key, L"", v, 256, ini.c_str());
        std::wstring w(v);
        while (!w.empty() && iswspace(w.back())) w.pop_back();
        size_t s = 0;
        while (s < w.size() && iswspace(w[s])) ++s;
        return Provenance::Utf8(w.substr(s));
    }

    std::string Num(double v)
    {
        char b[40];
        snprintf(b, sizeof(b), "%.15g", v);
        return b;
    }

    std::string SpeedJson(const Manifest::Plan& p)
    {
        // Origin "planned" means: written down before the run in SH2Proxy.ini [plan]. The hub never reads
        // the game's own speed value (no verified address), so "observed" is never produced here.
        if (p.hasSpeed)
            return "{\"value\": " + Num(p.speed) + ", \"origin\": \"planned\", \"meaning\": \"unspecified\"}";
        if (p.speedRejected)
            return "{\"value\": null, \"origin\": \"unknown\", \"meaning\": \"unspecified\", \"rejected_text\": " + Q(p.speedText) + "}";
        return "{\"value\": null, \"origin\": \"unknown\", \"meaning\": \"unspecified\"}";
    }

    std::string PausedJson(const Manifest::Plan& p)
    {
        if (p.hasPaused) return std::string("{\"value\": ") + B(p.paused) + ", \"origin\": \"planned\"}";
        return "{\"value\": null, \"origin\": \"unknown\"}";
    }

    std::string Switch(bool v, const char* source)
    {
        return std::string("{\"value\": ") + B(v) + ", \"source\": " + Q(source) + "}";
    }

    std::string Build(const Manifest::Input& in)
    {
        const Config::Flags& f = Config::Get();
        const Config::Sources& s = Config::GetSources();
        const Provenance::Software& sw = Provenance::Get();
        std::string j;
        j += "{\n";
        j += "  \"schema\": \"sh2perf.capture-manifest\",\n";
        j += "  \"manifest_version\": " + std::to_string(Manifest::kVersion) + ",\n";
        j += "  \"capture_id\": " + Q(in.captureId) + ",\n";
        j += std::string("  \"status\": ") + (in.finalized ? "\"finalized\"" : "\"recording\"") + ",\n";
        j += "  \"session\": {\"session_id\": " + Q(Provenance::SessionId()) + ", \"pid\": " + std::to_string(GetCurrentProcessId()) + "},\n";
        j += "  \"recording\": {\"stem\": " + Q(in.stem) + ", \"start_utc\": " + Q(in.startUtc) + ", \"stop_utc\": " + QOrNull(in.stopUtc) +
            ", \"start_local\": " + Q(in.startLocal) + ", \"stop_local\": " + QOrNull(in.stopLocal) +
            ", \"stop_reason\": " + QOrNull(in.stopReason) + ", \"frames_written\": " + std::to_string(in.frames) +
            ", \"markers\": " + std::to_string(in.markers) + "},\n";
        j += "  \"plan\": {\"origin\": \"planned\", \"experiment\": " + QOrNull(in.plan.experiment) + ", \"protocol\": " + QOrNull(in.plan.protocol) +
            ", \"scenario\": " + QOrNull(in.plan.scenario) + ", \"source_ini_sha256\": " + QOrNull(in.plan.iniSha256) + "},\n";
        j += "  \"game\": {\"game_speed\": " + SpeedJson(in.plan) + ", \"paused\": " + PausedJson(in.plan) +
            ", \"observation\": \"none: the hub reads no game memory for speed or pause\"},\n";

        j += "  \"software\": {\n";
        j += "    \"basis\": \"hub_session_start\",\n";
        j += "    \"hash_algorithm\": \"sha256\",\n";
        j += std::string("    \"pending\": ") + B(in.softwarePending) + ",\n";
        j += "    \"taken_utc\": " + QOrNull(sw.takenUtc) + ",\n";
        j += "    \"hub\": {\"commit\": " + Q(HUB_GIT_COMMIT) + ", \"dirty\": " + B(sw.hubDirty) + "},\n";
        j += "    \"files\": [";
        for (size_t i = 0; i < sw.files.size(); ++i) {
            const auto& e = sw.files[i];
            j += std::string(i ? ",\n      " : "\n      ") + "{\"role\": " + Q(e.role) + ", \"name\": " + Q(e.name) +
                ", \"size\": " + std::to_string(e.size) + ", \"sha256\": " + QOrNull(e.sha256) + "}";
        }
        j += sw.files.empty() ? "]\n" : "\n    ]\n";
        j += "  },\n";

        j += "  \"switches\": {\n";
        j += std::string("    \"profile\": {\"value\": ") + Q(s.profileName) + ", \"source\": " + Q(s.profile) + "},\n";
        j += "    \"frametime\": " + Switch(f.frameTime, s.frameTime) + ",\n";
        j += "    \"counters\": " + Switch(f.counters, s.counters) + ",\n";
        j += "    \"poll\": " + Switch(f.poll, s.poll) + ",\n";
        j += "    \"overlay\": " + Switch(f.overlay, s.overlay) + ",\n";
        j += "    \"beep\": " + Switch(f.beep, s.beep) + ",\n";
        j += std::string("    \"sample\": {\"value\": ") + Q(f.sampleMode) + ", \"source\": " + Q(s.sample) + "}\n";
        j += "  },\n";

        j += "  \"files\": [";
        for (size_t i = 0; i < in.files.size(); ++i) {
            const auto& e = in.files[i];
            j += std::string(i ? ",\n    " : "\n    ") + "{\"role\": " + Q(e.role) + ", \"name\": " + Q(e.name) + ", \"size\": " +
                (e.present ? std::to_string(e.size) : std::string("null")) + ", \"sha256\": " + QOrNull(e.sha256) + "}";
        }
        j += in.files.empty() ? "],\n" : "\n  ],\n";
        j += "  \"session_log\": {\"name\": \"Stronghold2AnalyseHub.log\", \"integrity\": \"none: shared by all recordings of a session and still growing\"},\n";

        // Technical quality is a fact about the files; suitability as a benchmark is a separate judgement
        // that the hub does not make (the importer/user records it later, outside the raw data).
        j += "  \"technical_quality\": {\"capture_ok\": ";
        j += in.finalized ? B(in.captureOk) : "null";
        j += ", \"frames_dropped\": " + std::to_string(in.framesDropped) + ", \"frames_lost_write\": " + std::to_string(in.framesLostWrite) +
            ", \"write_errors\": " + std::to_string(in.writeErrors) + ", \"files_closed\": " + B(in.filesClosed) +
            ", \"hashes_complete\": " + B(in.hashesOk) + "},\n";
        j += "  \"benchmark_suitability\": {\"verdict\": \"unknown\", \"assessed_by\": null}\n";
        j += "}\n";
        return j;
    }

    bool WriteAll(const std::wstring& path, const std::string& data)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
        ok = ok && FlushFileBuffers(h);
        ok = CloseHandle(h) && ok;
        if (!ok) DeleteFileW(path.c_str());
        return ok;
    }
}

bool Manifest::ParseSpeed(const std::string& text, double* out)
{
    if (text.empty()) return false;
    char* end = nullptr;
    const double v = strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(v)) return false;   // no range check: meaning is not guessed
    if (out) *out = v;
    return true;
}

bool Manifest::ParsePaused(const std::string& text, bool* out)
{
    const std::wstring w = Widen(text);
    if (w.empty()) return false;
    if (!_wcsicmp(w.c_str(), L"1") || !_wcsicmp(w.c_str(), L"true") || !_wcsicmp(w.c_str(), L"yes") || !_wcsicmp(w.c_str(), L"on")) { if (out) *out = true; return true; }
    if (!_wcsicmp(w.c_str(), L"0") || !_wcsicmp(w.c_str(), L"false") || !_wcsicmp(w.c_str(), L"no") || !_wcsicmp(w.c_str(), L"off")) { if (out) *out = false; return true; }
    return false;
}

Manifest::Plan Manifest::ReadPlan()
{
    Plan p;
    const std::wstring ini = Provenance::HubDir() + L"SH2Proxy.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) return p;
    p.iniSha256 = Provenance::Sha256File(ini.c_str());
    p.experiment = IniValue(ini, L"experiment");
    p.protocol = IniValue(ini, L"protocol");
    p.scenario = IniValue(ini, L"scenario");
    const std::string speed = IniValue(ini, L"game_speed");
    if (!speed.empty()) {
        p.speedText = speed;
        p.hasSpeed = ParseSpeed(speed, &p.speed);
        p.speedRejected = !p.hasSpeed;
    }
    const std::string paused = IniValue(ini, L"paused");
    if (!paused.empty()) p.hasPaused = ParsePaused(paused, &p.paused);
    return p;
}

bool Manifest::Write(const std::wstring& dir, const Input& in, std::string* sha256Out)
{
    const std::wstring stem = Widen(in.stem);
    const std::wstring dst = dir + stem + L"_manifest.json";
    const std::wstring tmp = dst + L".tmp";
    const std::string data = Build(in);
    if (g_failWrites.load() || !WriteAll(tmp, data)) {
        Logger::Error("Manifest: could not write %s (%lu).", Provenance::Utf8(dst).c_str(), GetLastError());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Logger::Error("Manifest: could not publish %s (%lu).", Provenance::Utf8(dst).c_str(), GetLastError());
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (sha256Out) *sha256Out = Provenance::Sha256File(dst.c_str());
    return true;
}

void Manifest::Test_FailWrites(bool fail) { g_failWrites.store(fail); }
