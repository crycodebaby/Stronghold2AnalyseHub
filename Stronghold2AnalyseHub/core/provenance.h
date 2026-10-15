#pragma once
#include "../pch.h"

// Provenance of a recording (SH2-Perf docs/FORMATS/CAPTURE_MANIFEST.md, D012).
//
// Software identity is measured by the hub itself, on a background thread started when the hub is
// loaded into the game: full SHA-256 of the game exe, the hub DLL, the d3d9 loader files and the
// config file *of this session*. A hash taken later (at import) is never provenance. Nothing here
// runs on the render thread; Capture only reads the finished result.
namespace Provenance
{
    struct FileId
    {
        std::string role;                       // game_exe | hub_dll | d3d9_in_game_dir | d3d9_dxvk | config_ini
        std::string name;                       // file name only, never a path (D007)
        uint64_t    size = 0;
        std::string sha256;                     // 64 lowercase hex chars, empty if the file could not be read
    };

    struct Software
    {
        std::vector<FileId> files;              // only files that exist
        std::string hubCommit;                  // HUB_GIT_COMMIT baked in at configure time
        bool        hubDirty = false;           // commit id ends in "-dirty"
        std::string takenUtc;                   // when the hashing finished (ISO 8601 UTC)
    };

    // Starts the hashing thread (once). Call from the hub init thread after Logger::Init.
    void Start();
    // Blocks up to `timeoutMs` until the hashing is done; false = not finished (identity pending).
    bool Wait(DWORD timeoutMs);
    // Result of Start(); valid after Wait() returned true.
    const Software& Get();
    // "ses-<utc>-<8 hex>", fixed for the process lifetime.
    const std::string& SessionId();
    // Directory of the hub DLL, with trailing backslash (SH2Proxy.ini, SH2Prepared.json live there).
    std::wstring HubDir();

    // Full SHA-256 of a file, lowercase hex; empty on error. `sizeOut` receives the bytes hashed.
    std::string Sha256File(const wchar_t* path, uint64_t* sizeOut = nullptr);

    // Helpers shared with the manifest writer.
    std::string Utf8(const std::wstring& w);
    std::string JsonEscape(const std::string& s);
    std::string RandomHex(size_t bytes);        // BCryptGenRandom; falls back to tick/pid mixing
    std::string NowUtcIso();                    // 2026-10-15T01:44:11Z
    std::string NowUtcCompact();                // 20261015T014411Z
}
