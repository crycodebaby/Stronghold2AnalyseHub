#pragma once
#include "../pch.h"

// Orderly end of the hub's own background threads before the DLL may be unloaded (SH2-Perf D012).
// Closing a thread handle is no proof that the thread ended, and unloading a DLL under a running thread crashes the
// game. So every thread is asked to stop, waited for, and only a thread that has really ended counts. Nothing is ever
// terminated by force. If a thread does not end (a blocked file write), the caller keeps the DLL loaded and asks again.
namespace Lifecycle
{
    struct Result
    {
        bool capture = true, sampler = true, provenance = true;       // true = that thread has ended (or never ran)
        bool All() const { return capture && sampler && provenance; }
    };

    // Order matters: the capture thread first (it finalizes the open recording and needs the software identity and the
    // sampler's data), the sampler second, the hashing thread last. Waits up to `perThreadMs` for each.
    Result StopBackground(DWORD perThreadMs);
    // "capture, hashing" - the names of the threads that have not ended, for the log.
    std::string Describe(const Result& r);
}
