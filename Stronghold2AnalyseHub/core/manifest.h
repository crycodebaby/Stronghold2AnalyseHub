#pragma once
#include "../pch.h"

// Per-recording manifest (SH2-Perf docs/FORMATS/CAPTURE_MANIFEST.md, D012).
// `<stem>_manifest.json` is the identity of one recording; latest.json is only a pointer to it.
// Written atomically (temp file + rename) by the capture thread, never by the render thread:
//   at start    status "recording": identity, switches, software, plan; file hashes still empty
//   after stop  status "finalized": written only after every capture file is closed and hashed
// A manifest that stays at "recording" is an interrupted recording (crash, kill, power loss).
namespace Manifest
{
    constexpr int kVersion = 1;

    struct Plan                                   // what was planned in SH2Proxy.ini [plan]; never read from the game
    {
        std::string experiment, protocol, scenario;     // empty = unknown
        bool        hasSpeed = false;
        double      speed = 0;                          // numeric, no range or meaning assumed
        std::string speedText;                          // raw text, kept even if it was not numeric
        bool        speedRejected = false;              // text present but not a finite number
        bool        hasPaused = false;
        bool        paused = false;
        std::string iniSha256;                          // SH2Proxy.ini as it was when the recording started
    };

    struct FileEntry
    {
        std::string role;                         // frames | threads | samples | prepared
        std::string name;
        uint64_t    size = 0;
        std::string sha256;                       // empty until finalized
        bool        present = false;
    };

    struct Input
    {
        std::string captureId, stem;
        std::string startUtc, stopUtc, startLocal, stopLocal, stopReason;
        bool        finalized = false;
        uint64_t    frames = 0;
        uint32_t    markers = 0;
        std::vector<FileEntry> files;
        bool        captureOk = true;
        uint64_t    framesDropped = 0, framesLostWrite = 0;
        uint32_t    writeErrors = 0;
        bool        filesClosed = false;
        bool        hashesOk = false;
        bool        softwarePending = false;      // hashing was not finished when the recording started
        Plan        plan;
    };

    // Reads [plan] from SH2Proxy.ini next to the hub DLL (and hashes the ini). Capture thread only.
    Plan ReadPlan();
    // Parsing helpers (exposed for tests).
    bool ParseSpeed(const std::string& text, double* out);
    bool ParsePaused(const std::string& text, bool* out);

    // Writes `<dir><stem>_manifest.json` atomically. On success `sha256Out` holds the hash of the written file.
    bool Write(const std::wstring& dir, const Input& in, std::string* sha256Out);

    // Test hook: make the next Write fail (disk full / access denied).
    void Test_FailWrites(bool fail);
}
