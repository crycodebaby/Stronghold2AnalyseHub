#include "logger.h"

namespace
{
    std::mutex        g_mutex;
    std::wofstream    g_stream;
    std::atomic<bool> g_ready{ false };

    std::wstring NowTimestamp()
    {
        using namespace std::chrono;
        const auto now = system_clock::now();
        const auto t = system_clock::to_time_t(now);

        std::tm tm{};
        localtime_s(&tm, &t);

        wchar_t buf[64]{};
        wcsftime(buf, 64, L"%Y-%m-%d %H:%M:%S", &tm);
        return buf;
    }

    // Caller holds g_mutex. std::mutex is not recursive: locking it twice on one
    // thread throws std::system_error, which used to abort the game from Init().
    void WriteLineLocked(const wchar_t* level, const char* utf8)
    {
        if (!g_ready) return;

        const auto ts = NowTimestamp();
        const DWORD pid = GetCurrentProcessId();
        const DWORD tid = GetCurrentThreadId();

        // Convert UTF-8 → UTF-16 for reliable wide writing
        int needed = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
        std::wstring msg;
        if (needed > 0) {
            msg.resize(static_cast<size_t>(needed - 1));
            MultiByteToWideChar(CP_UTF8, 0, utf8, -1, msg.data(), needed);
        }

        g_stream << L"[" << ts << L"][" << level << L"][PID:" << pid << L"][TID:" << tid << L"] "
            << msg << L"\n";
        g_stream.flush();
    }

    void WriteLine(const wchar_t* level, const char* utf8)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        WriteLineLocked(level, utf8);
    }

    void VLog(const wchar_t* level, const char* fmt, va_list ap)
    {
        char buf[2048];
        vsnprintf_s(buf, _TRUNCATE, fmt, ap);
        WriteLine(level, buf);
    }
}

std::wstring Logger::OutputDir()
{
    // SH2Launcher passes the directory via the environment (inherited by the game).
    wchar_t dir[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"SH2HUB_OUT", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        if (wchar_t* p = wcsrchr(dir, L'\\')) *p = L'\0';
    }
    std::wstring out(dir);
    CreateDirectoryW(out.c_str(), nullptr); // ok if it exists
    if (!out.empty() && out.back() != L'\\') out += L'\\';
    return out;
}

void Logger::Init(const wchar_t* fileName)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_ready) return;

    const std::wstring fullPath = OutputDir() + fileName;

    g_stream.open(fullPath, std::ios::out | std::ios::app);
    g_ready = g_stream.is_open();

    if (g_ready) WriteLineLocked(L"INFO", "Logger initialized");
}

void Logger::Info(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt); VLog(L"INFO", fmt, ap); va_end(ap);
}

void Logger::Warn(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt); VLog(L"WARN", fmt, ap); va_end(ap);
}

void Logger::Error(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt); VLog(L"ERROR", fmt, ap); va_end(ap);
}

void Logger::Flush()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_ready) g_stream.flush();
}

void Logger::Shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_stream.is_open()) g_stream.close();
    g_ready = false;
}
