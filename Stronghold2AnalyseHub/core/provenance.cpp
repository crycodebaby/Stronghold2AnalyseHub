#include "provenance.h"
#include "logger.h"

#include <bcrypt.h>
#include <exception>

#ifndef HUB_GIT_COMMIT
#define HUB_GIT_COMMIT "unknown"
#endif

namespace
{
    Provenance::Software g_software;
    std::atomic<bool> g_done{ false };
    std::atomic<bool> g_started{ false };
    HANDLE g_thread = nullptr;
    std::string g_sessionId;
    std::mutex g_sessionMutex;

    std::wstring ModuleDir(HMODULE module)
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(module, path, MAX_PATH);
        if (wchar_t* slash = wcsrchr(path, L'\\')) *(slash + 1) = L'\0';
        return path;
    }

    std::string BaseName(const std::wstring& path)
    {
        const size_t p = path.find_last_of(L'\\');
        return Provenance::Utf8(p == std::wstring::npos ? path : path.substr(p + 1));
    }

    void AddFile(const char* role, const std::wstring& path)
    {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        Provenance::FileId id;
        id.role = role;
        id.name = BaseName(path);
        id.sha256 = Provenance::Sha256File(path.c_str(), &id.size);
        if (id.sha256.empty())
            Logger::Warn("Provenance: could not hash %s (%lu); identity of this file stays unknown.", id.name.c_str(), GetLastError());
        g_software.files.push_back(std::move(id));
    }

    DWORD WINAPI HashMain(LPVOID)
    {
        try {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&Provenance::Start), &self);
            wchar_t exe[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            wchar_t hub[MAX_PATH]{};
            GetModuleFileNameW(self, hub, MAX_PATH);

            const std::wstring gameDir = ModuleDir(nullptr);
            const std::wstring hubDir = ModuleDir(self);

            g_software.hubCommit = HUB_GIT_COMMIT;
            const std::string& c = g_software.hubCommit;
            g_software.hubDirty = c.size() >= 6 && c.compare(c.size() - 6, 6, "-dirty") == 0;

            AddFile("game_exe", exe);
            AddFile("hub_dll", hub);
            AddFile("d3d9_in_game_dir", gameDir + L"d3d9.dll");       // proxy, or DXVK in the layout before D009
            AddFile("d3d9_dxvk", gameDir + L"d3d9_dxvk.dll");
            AddFile("config_ini", hubDir + L"SH2Proxy.ini");
            g_software.takenUtc = Provenance::NowUtcIso();
            Logger::Info("Provenance: session identity hashed (%u files, hub %s).",
                static_cast<unsigned>(g_software.files.size()), g_software.hubCommit.c_str());
        }
        catch (const std::exception& e) {
            Logger::Error("Provenance: exception while hashing: %s", e.what());
        }
        catch (...) {
            Logger::Error("Provenance: unknown exception while hashing.");
        }
        g_done.store(true, std::memory_order_release);
        return 0;
    }
}

void Provenance::Start()
{
    if (g_started.exchange(true)) return;
    g_thread = CreateThread(nullptr, 0, HashMain, nullptr, 0, nullptr);
    if (!g_thread) {
        Logger::Error("Provenance: could not start the hashing thread (%lu); software identity unavailable.", GetLastError());
        g_done.store(true);
    }
}

bool Provenance::Wait(DWORD timeoutMs)
{
    if (!g_started.load()) return false;           // never started: nothing to wait for, identity unavailable
    const DWORD t0 = GetTickCount();
    while (!g_done.load(std::memory_order_acquire)) {
        if (GetTickCount() - t0 >= timeoutMs) return false;
        Sleep(10);
    }
    return true;
}

const Provenance::Software& Provenance::Get()
{
    static const Software kEmpty;                  // while hashing runs the vector is still being filled
    return g_done.load(std::memory_order_acquire) ? g_software : kEmpty;
}

const std::string& Provenance::SessionId()
{
    std::lock_guard<std::mutex> lock(g_sessionMutex);
    if (g_sessionId.empty()) g_sessionId = "ses-" + NowUtcCompact() + "-" + RandomHex(4);
    return g_sessionId;
}

std::wstring Provenance::HubDir()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Provenance::HubDir), &self))
        return L"";
    return ModuleDir(self);
}

std::string Provenance::Sha256File(const wchar_t* path, uint64_t* sizeOut)
{
    if (sizeOut) *sizeOut = 0;
    std::string hex;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return hex;

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<UCHAR> buf(1 << 20);
        DWORD got = 0;
        bool ok = true;
        uint64_t total = 0;
        for (;;) {
            if (!ReadFile(file, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) { ok = false; break; }
            if (!got) break;
            if (BCryptHashData(hash, buf.data(), got, 0) != 0) { ok = false; break; }
            total += got;
        }
        UCHAR digest[32];
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            char two[3];
            for (UCHAR b : digest) { sprintf_s(two, "%02x", b); hex += two; }
            if (sizeOut) *sizeOut = total;
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    return hex;
}

std::string Provenance::Utf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string Provenance::JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
            else out += static_cast<char>(c);
        }
    }
    return out;
}

std::string Provenance::RandomHex(size_t bytes)
{
    std::vector<UCHAR> buf(bytes);
    if (BCryptGenRandom(nullptr, buf.data(), static_cast<ULONG>(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        uint64_t x = static_cast<uint64_t>(t.QuadPart) ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^ GetTickCount64();
        for (size_t i = 0; i < bytes; ++i) { x = x * 6364136223846793005ULL + 1442695040888963407ULL; buf[i] = static_cast<UCHAR>(x >> 56); }
    }
    std::string hex;
    char two[3];
    for (UCHAR b : buf) { sprintf_s(two, "%02x", b); hex += two; }
    return hex;
}

std::string Provenance::NowUtcIso()
{
    SYSTEMTIME st;
    GetSystemTime(&st);
    char b[32];
    snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

std::string Provenance::NowUtcCompact()
{
    SYSTEMTIME st;
    GetSystemTime(&st);
    char b[32];
    snprintf(b, sizeof(b), "%04u%02u%02uT%02u%02u%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}
