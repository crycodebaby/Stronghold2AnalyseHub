#include "config.h"
#include "logger.h"

namespace
{
    Config::Flags g_flags;
    Config::Sources g_sources;

    // Reads one key: environment first, then SH2Proxy.ini. Returns false if neither sets it.
    bool Lookup(const wchar_t* key, const wchar_t* iniPath, std::wstring& out, const char** origin = nullptr)
    {
        wchar_t env[64]{}, name[64]{}, value[64]{};
        swprintf_s(name, L"SH2HUB_%s", key);
        for (wchar_t* p = name; *p; ++p) *p = static_cast<wchar_t>(towupper(*p));
        if (GetEnvironmentVariableW(name, env, 64) && env[0]) { out = env; if (origin) *origin = "env"; return true; }
        if (iniPath && GetPrivateProfileStringW(L"proxy", key, L"", value, 64, iniPath) && value[0]) {
            out = value;
            if (origin) *origin = "ini";
            return true;
        }
        return false;
    }

    std::wstring IniPath()
    {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&Config::Get), &self))
            return L"";
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(self, path, MAX_PATH);
        if (wchar_t* slash = wcsrchr(path, L'\\')) *slash = L'\0';
        return std::wstring(path) + L"\\SH2Proxy.ini";
    }

    void Resolve(bool& flag, const char*& source, const wchar_t* key, const wchar_t* ini)
    {
        std::wstring v;
        const char* origin = nullptr;
        if (Lookup(key, ini, v, &origin)) {
            const bool parsed = Config::ParseBool(v.c_str(), flag);
            if (parsed != flag || v == L"1" || v == L"0" || !_wcsicmp(v.c_str(), L"on") || !_wcsicmp(v.c_str(), L"off") ||
                !_wcsicmp(v.c_str(), L"true") || !_wcsicmp(v.c_str(), L"false") || !_wcsicmp(v.c_str(), L"yes") || !_wcsicmp(v.c_str(), L"no"))
                source = origin;                       // an unparsable value keeps the earlier source
            flag = parsed;
        }
    }
}

bool Config::ParseBool(const wchar_t* t, bool fallback)
{
    if (!t || !*t) return fallback;
    if (!_wcsicmp(t, L"1") || !_wcsicmp(t, L"on") || !_wcsicmp(t, L"true") || !_wcsicmp(t, L"yes")) return true;
    if (!_wcsicmp(t, L"0") || !_wcsicmp(t, L"off") || !_wcsicmp(t, L"false") || !_wcsicmp(t, L"no")) return false;
    return fallback;
}

void Config::Init()
{
    const std::wstring iniStr = IniPath();
    const wchar_t* ini = iniStr.empty() ? nullptr : iniStr.c_str();

    Flags f;
    Sources s;
    std::wstring v;
    const char* profileOrigin = nullptr;
    if (Lookup(L"profile", ini, v, &profileOrigin)) {
        if (!_wcsicmp(v.c_str(), L"min")) {
            f.counters = f.poll = f.overlay = false;
            s.profileName = "min";
            s.profile = profileOrigin;
            s.counters = s.poll = s.overlay = "profile";
        }
        else if (_wcsicmp(v.c_str(), L"full") != 0) Logger::Warn("Config: unknown profile, using full.");
        else { s.profile = profileOrigin; }
    }
    f.beep = !f.overlay;                       // default: feedback only when there is no overlay
    s.beep = "derived";
    Resolve(f.frameTime, s.frameTime, L"frametime", ini);
    Resolve(f.counters, s.counters, L"counters", ini);
    Resolve(f.poll, s.poll, L"poll", ini);
    Resolve(f.overlay, s.overlay, L"overlay", ini);
    f.beep = !f.overlay;                       // follows the final overlay value unless set explicitly
    Resolve(f.beep, s.beep, L"beep", ini);

    // sample= in SH2Proxy.ini is read by the sampler itself (environment first); resolve it the same way here.
    std::wstring sampleValue;
    const char* sampleOrigin = nullptr;
    if (Lookup(L"sample", ini, sampleValue, &sampleOrigin)) {
        // The d3d9 proxy copies sample= from the ini into SH2HUB_SAMPLE before the hub starts: same value = ini.
        wchar_t fromIni[16]{};
        if (sampleOrigin && !strcmp(sampleOrigin, "env") && ini &&
            GetPrivateProfileStringW(L"proxy", L"sample", L"", fromIni, 16, ini) && !_wcsicmp(fromIni, sampleValue.c_str()))
            sampleOrigin = "ini";
        // No sample= in the ini and the value is "off": the proxy exports its own default, nobody chose it.
        else if (sampleOrigin && !strcmp(sampleOrigin, "env") && ini && !_wcsicmp(sampleValue.c_str(), L"off") &&
            !GetPrivateProfileStringW(L"proxy", L"sample", L"", fromIni, 16, ini))
            sampleOrigin = "default";
        if (!_wcsicmp(sampleValue.c_str(), L"module")) { f.sample = true; f.sampleMode = "module"; s.sample = sampleOrigin; }
        else if (!_wcsicmp(sampleValue.c_str(), L"function")) { f.sample = true; f.sampleMode = "function"; s.sample = sampleOrigin; }
        else if (!_wcsicmp(sampleValue.c_str(), L"off")) { s.sample = sampleOrigin; }
    }

    // Recording is built on the frame boundary: frametime is the master switch.
    if (!f.frameTime && (f.counters || f.poll || f.overlay || f.sample)) {
        Logger::Warn("Config: frametime=0 but counters/poll/overlay/sample need frame boundaries; frametime forced on.");
        f.frameTime = true;
        s.frameTime = "forced";
    }
    g_flags = f;
    g_sources = s;
    Logger::Info("Config: frametime=%d counters=%d poll=%d overlay=%d beep=%d sample=%d (ini %s)",
        f.frameTime, f.counters, f.poll, f.overlay, f.beep, f.sample,
        ini && GetFileAttributesW(ini) != INVALID_FILE_ATTRIBUTES ? "found" : "absent");
}

const Config::Flags& Config::Get() { return g_flags; }
const Config::Sources& Config::GetSources() { return g_sources; }
bool Config::NeedsHooks() { return g_flags.frameTime || g_flags.counters || g_flags.poll || g_flags.overlay || g_flags.sample; }
