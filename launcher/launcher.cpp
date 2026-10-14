// SH2Launcher: start Stronghold 2 suspended, inject Stronghold2AnalyseHub.dll,
// wait until its hooks are installed, then let the game run.
//
// Usage: SH2Launcher.exe <path\to\Stronghold2.exe> [path\to\Stronghold2AnalyseHub.dll]
// Default DLL: next to SH2Launcher.exe.
//
// Starting suspended guarantees the Direct3DCreate9 hook is in place before
// the game creates its device (see SH2-Perf docs/DECISIONS.md D004).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <string>

namespace
{
    int Fail(const char* what)
    {
        fprintf(stderr, "SH2Launcher: %s failed (error %lu)\n", what, GetLastError());
        return 1;
    }

    std::wstring DirOf(const std::wstring& path)
    {
        const size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? L"." : path.substr(0, slash);
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: SH2Launcher.exe <Stronghold2.exe> [Stronghold2AnalyseHub.dll]\n");
        return 2;
    }

    wchar_t exe[MAX_PATH], dll[MAX_PATH];
    GetFullPathNameW(argv[1], MAX_PATH, exe, nullptr);
    if (argc >= 3) {
        GetFullPathNameW(argv[2], MAX_PATH, dll, nullptr);
    } else {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        swprintf_s(dll, L"%s\\Stronghold2AnalyseHub.dll", DirOf(self).c_str());
    }
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) return Fail("finding the DLL");

    const std::wstring workDir = DirOf(exe);
    std::wstring cmdLine = L"\"" + std::wstring(exe) + L"\"";

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmdLine.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
            workDir.c_str(), &si, &pi))
        return Fail("CreateProcessW");

    // Event the DLL signals once its hooks are installed.
    wchar_t eventName[64];
    swprintf_s(eventName, L"Local\\SH2AnalyseHub.Ready.%lu", pi.dwProcessId);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName);

    const SIZE_T bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(pi.hProcess, remote, dll, bytes, nullptr)) {
        TerminateProcess(pi.hProcess, 1);
        return Fail("writing the DLL path");
    }

    // Launcher and game are both 32-bit, so kernel32 sits at the same address.
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(pi.hProcess, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (!thread) {
        TerminateProcess(pi.hProcess, 1);
        return Fail("CreateRemoteThread");
    }
    WaitForSingleObject(thread, INFINITE);
    DWORD module = 0;
    GetExitCodeThread(thread, &module);
    CloseHandle(thread);
    VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
    if (!module) {
        TerminateProcess(pi.hProcess, 1);
        fprintf(stderr, "SH2Launcher: LoadLibraryW returned NULL in the game process\n");
        return 1;
    }

    if (WaitForSingleObject(ready, 10000) != WAIT_OBJECT_0)
        fprintf(stderr, "SH2Launcher: no ready signal within 10 s; resuming anyway\n");
    CloseHandle(ready);

    ResumeThread(pi.hThread);
    printf("SH2Launcher: game PID %lu started with %ls\n", pi.dwProcessId, dll);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
