#pragma once
#include "../pch.h"
#include <cwctype>

// Independent measurement switches (SH2-Perf docs/DECISIONS.md D011). Every switch decides
// whether code exists at all in the game's render path, not only whether its output is shown:
//
//   frametime  Present hook: frame time + time inside Present, one record per frame into the
//              capture ring (wait-free). Master switch of recording (F9).
//   counters   hooks on BeginScene / SetTexture / Draw* / Set*ShaderConstantF. Off = not hooked.
//   poll       once per second, only while recording, on the capture thread: per-thread CPU
//              (threads CSV) and process address space (va_used_mb).
//   overlay    ImGui window: EndScene + Reset + WndProc hooks, draws inside the game's frame.
//              Off = those three are not hooked; endscene_ms / between_endscene_ms / overlay_ms
//              are then 0 in the CSV.
//   beep       short tone on recording start/stop (feedback when the overlay is off).
//
// Source, strongest first, per key: environment SH2HUB_<KEY>, then SH2Proxy.ini ([proxy], next to the
// hub DLL), then the profile (SH2HUB_PROFILE / profile=): "full" (default, behaviour of earlier
// hub builds) or "min" (frametime only). Values: 1/0, on/off, true/false.
// The sampling profiler (SH2HUB_SAMPLE, sample=) stays a separate switch (default off).
namespace Config
{
    struct Flags
    {
        bool frameTime = true;
        bool counters = true;
        bool poll = true;
        bool overlay = true;
        bool beep = false;
        bool sample = false;     // SH2HUB_SAMPLE is module or function (read only to decide about hooks)
        const char* sampleMode = "off";   // off | module | function
    };

    // Where each effective value came from (manifest "switches"): env | ini | profile | default | derived | forced.
    struct Sources
    {
        const char* profile = "default";
        const char* frameTime = "default";
        const char* counters = "default";
        const char* poll = "default";
        const char* overlay = "default";
        const char* beep = "default";
        const char* sample = "default";
        const char* profileName = "full";     // full | min
    };

    // Reads the configuration once; logs the result. Call after Logger::Init.
    void Init();
    const Flags& Get();
    const Sources& GetSources();

    // True if any Present hook consumer is on (frametime forced on when others need frame boundaries).
    bool NeedsHooks();

    // Parsing helpers (exposed for tests).
    bool ParseBool(const wchar_t* text, bool fallback);
}
