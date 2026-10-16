#include "lifecycle.h"
#include "capture.h"
#include "provenance.h"
#include "sampler.h"

Lifecycle::Result Lifecycle::StopBackground(DWORD perThreadMs)
{
    Result r;
    r.capture = Capture::Shutdown(perThreadMs);
    r.sampler = Sampler::Shutdown(perThreadMs);
    r.provenance = Provenance::Shutdown(perThreadMs);
    return r;
}

std::string Lifecycle::Describe(const Result& r)
{
    std::string s;
    auto add = [&](bool ended, const char* name) { if (!ended) s += (s.empty() ? "" : ", ") + std::string(name); };
    add(r.capture, "capture writer");
    add(r.sampler, "sampler");
    add(r.provenance, "provenance hashing");
    return s.empty() ? "none" : s;
}
