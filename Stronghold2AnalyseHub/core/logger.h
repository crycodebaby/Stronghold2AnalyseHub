#pragma once
#include "../pch.h"

// Simple thread-safe logger.
// Writes UTF-8 messages with timestamp + PID/TID next to the game executable.
// Keep the interface small; clarity beats feature creep.
namespace Logger
{
    // Creates/opens the log file and writes a header line.
    // Default file name: "Stronghold2AnalyseHub.log".
    void Init(const wchar_t* fileName = L"Stronghold2AnalyseHub.log");

    // Directory for log and capture files, with trailing backslash:
    // %SH2HUB_OUT% if set (SH2Launcher --out), else the game exe folder.
    std::wstring OutputDir();

    void Info(const char* fmt, ...);
    void Warn(const char* fmt, ...);
    void Error(const char* fmt, ...);

    // Ensure data hits disk (usually not needed if Init/Shutdown are used).
    void Flush();

    // Close the log file. Safe to call more than once.
    void Shutdown();
}
