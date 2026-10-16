// Stand-in for the game: creates a D3D9 device like the game does (Direct3DCreate9 import), presents
// frames and burns CPU on the render thread, so the d3d9.dll proxy + hub (+ sampler) can be tested
// without Stronghold 2. Copy standin_d3d9.exe, the proxy d3d9.dll and the hub DLL into one folder
// (tools/test_variant.py does not touch this). Set SH2HUB_OUT, SH2HUB_AUTORECORD, SH2HUB_SAMPLE.
//   standin_d3d9.exe [frames=400] [burn_ms=4]
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdlib>

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv)
{
    const int frames = argc > 1 ? atoi(argv[1]) : 400;
    const int burnMs = argc > 2 ? atoi(argv[2]) : 4;

    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SH2StandIn";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"SH2 stand-in", WS_OVERLAPPEDWINDOW, 50, 50, 320, 240,
        nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { puts("Direct3DCreate9 failed"); return 2; }
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = nullptr;
    if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, &dev))) {
        puts("CreateDevice failed");
        return 3;
    }

    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    for (int i = 0; i < frames; ++i) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(i & 255, 40, 80), 1.0f, 0);
        dev->BeginScene();
        dev->EndScene();
        // CPU work on the render thread (user code of this exe).
        QueryPerformanceCounter(&a);
        volatile unsigned x = 1;
        do { for (int k = 0; k < 1000; ++k) x = x * 1664525u + 1013904223u; QueryPerformanceCounter(&b); }
        while ((b.QuadPart - a.QuadPart) * 1000 < static_cast<LONGLONG>(burnMs) * f.QuadPart);
        dev->Present(nullptr, nullptr, nullptr, nullptr);
    }
    dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    puts("standin done");
    return 0;
}
