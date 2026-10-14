#include "d3d9_hooks.h"
#include "../core/logger.h"
#include "../core/metrics.h"

// imgui_impl_win32.h keeps this declaration under #if 0 (to avoid pulling in
// windows.h); the backend expects users to forward-declare it.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// -----------------------------------------------------------------------------
// Notes for contributors:
// - Keep hook installation narrow and explicit.
// - Avoid clever macro magic; we value auditability.
// - Handle device reset paths carefully; DX9 likes to surprise.
// -----------------------------------------------------------------------------

namespace
{
    // State captured during CreateDevice
    HWND                 g_hwnd = nullptr;
    IDirect3DDevice9* g_device = nullptr;

    // WndProc trampoline
    WNDPROC              g_prevWndProc = nullptr;

    // ImGui lifecycle flag
    bool                 g_imguiReady = false;

    // Function typedefs
    using Direct3DCreate9_t = IDirect3D9 * (WINAPI*)(UINT);
    using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
    using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    using EndScene_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
    using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    using BeginScene_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
    using SetTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
    using DrawPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
    using DrawIndexedPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    using DrawPrimitiveUP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
    using DrawIndexedPrimitiveUP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
    using SetShaderConstantF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);

    // IDirect3DDevice9 vtable indices (d3d9.h declaration order).
    enum DeviceVtbl : int
    {
        kReset = 16, kPresent = 17, kBeginScene = 41, kEndScene = 42, kSetTexture = 65,
        kDrawPrimitive = 81, kDrawIndexedPrimitive = 82, kDrawPrimitiveUP = 83,
        kDrawIndexedPrimitiveUP = 84, kSetVertexShaderConstantF = 94, kSetPixelShaderConstantF = 109,
    };

    // Original pointers
    Direct3DCreate9_t   s_Direct3DCreate9_Orig = nullptr;
    CreateDevice_t      s_CreateDevice_Orig = nullptr;
    Reset_t             s_Reset_Orig = nullptr;
    EndScene_t          s_EndScene_Orig = nullptr;
    Present_t                s_Present_Orig = nullptr;
    BeginScene_t             s_BeginScene_Orig = nullptr;
    SetTexture_t             s_SetTexture_Orig = nullptr;
    DrawPrimitive_t          s_DrawPrimitive_Orig = nullptr;
    DrawIndexedPrimitive_t   s_DrawIndexedPrimitive_Orig = nullptr;
    DrawPrimitiveUP_t        s_DrawPrimitiveUP_Orig = nullptr;
    DrawIndexedPrimitiveUP_t s_DrawIndexedPrimitiveUP_Orig = nullptr;
    SetShaderConstantF_t     s_SetVSConstF_Orig = nullptr;
    SetShaderConstantF_t     s_SetPSConstF_Orig = nullptr;

    double ElapsedMs(const LARGE_INTEGER& start)
    {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        return 1000.0 * static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
    }

    // Writes every loaded module path to the log once (answers which CRT
    // copy the loader actually picked, see FINDINGS F032).
    void LogLoadedModules()
    {
        HMODULE mods[1024];
        DWORD needed = 0;
        if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
        const DWORD n = needed / sizeof(HMODULE);
        for (DWORD i = 0; i < n && i < 1024; ++i) {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(mods[i], path, MAX_PATH);
            char utf8[MAX_PATH * 3]{};
            WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
            Logger::Info("module %p %s", mods[i], utf8);
        }
    }

    // Forward decls
    LRESULT CALLBACK WndProc_Hook(HWND, UINT, WPARAM, LPARAM);

    // --- ImGui helpers --------------------------------------------------------

    void ImGui_CreateIfReady()
    {
        if (g_imguiReady || !g_hwnd || !g_device) return;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr; // do not write imgui.ini into the game folder

        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX9_Init(g_device);

        g_imguiReady = true;
        Logger::Info("ImGui initialized (DX9 + Win32).");
    }

    void ImGui_Destroy()
    {
        if (!g_imguiReady) return;

        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        g_imguiReady = false;
        Logger::Info("ImGui shutdown.");
    }

    // --- Hooked functions -----------------------------------------------------

    // Route Win32 messages to ImGui first. If ImGui wants the input,
    // we avoid feeding it back to the game.
    LRESULT CALLBACK WndProc_Hook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (g_imguiReady) {
            if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
                return TRUE;

            const ImGuiIO& io = ImGui::GetIO();
            if (io.WantCaptureMouse || io.WantCaptureKeyboard)
                return DefWindowProcW(hWnd, msg, wParam, lParam);
        }
        return CallWindowProcW(g_prevWndProc, hWnd, msg, wParam, lParam);
    }

    HRESULT STDMETHODCALLTYPE Reset_Hook(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp)
    {
        if (g_imguiReady) {
            ImGui_ImplDX9_InvalidateDeviceObjects();
        }

        const HRESULT hr = s_Reset_Orig(device, pp);

        if (SUCCEEDED(hr) && g_imguiReady) {
            ImGui_ImplDX9_CreateDeviceObjects();
            Logger::Info("Device reset handled.");
        }
        return hr;
    }

    // Metrics panel: last frame, histogram, frame-time history.
    void DrawMetricsWindow()
    {
        const Metrics::Snapshot& s = Metrics::Last();
        const auto& c = s.counters;
        const auto& t = s.times;

        ImGui::Text("Device: %p  HWND: %p  render TID: %u", g_device, g_hwnd, s.renderTid);
        ImGui::Text("Frame %.2f ms (%.1f FPS)  Present %.2f  EndScene %.2f  between EndScene %.2f  overlay %.2f",
            t.frameMs, t.frameMs > 0 ? 1000.0 / t.frameMs : 0.0, t.presentMs, t.endSceneMs,
            t.betweenEndSceneMs, t.overlayMs);
        ImGui::Text("Scenes %u  DIP %u  DP %u  DPUP %u  DIPUP %u  prims %llu",
            c.scenes, c.drawIndexed, c.draw, c.drawUP, c.drawIndexedUP, c.primitives);
        ImGui::Text("SetTexture %u  VSConstF %u (%u vec4)  PSConstF %u (%u vec4)",
            c.setTexture, c.vsConstCalls, c.vsConstVec4, c.psConstCalls, c.psConstVec4);
        ImGui::Text("VA used %.0f MB", s.vaUsedMb);

        int count = 0, offset = 0;
        const float* hist = Metrics::FrameHistory(count, offset);
        ImGui::PlotLines("frame ms", hist, count, offset, nullptr, 0.0f, 100.0f, ImVec2(360, 60));

        const auto& h = Metrics::Histogram();
        uint64_t total = 0;
        for (uint64_t v : h) total += v;
        float bars[Metrics::kBucketCount];
        for (int i = 0; i < Metrics::kBucketCount; ++i)
            bars[i] = total ? static_cast<float>(100.0 * static_cast<double>(h[i]) / static_cast<double>(total)) : 0.0f;
        ImGui::PlotHistogram("% frames", bars, Metrics::kBucketCount, 0, nullptr, 0.0f, 100.0f, ImVec2(360, 60));
        ImGui::TextUnformatted("buckets: <8.3 <16.7 <33.3 <50 <100 <250 <500 <1000 >=1000 ms");
        ImGui::Text("%llu %llu %llu %llu %llu %llu %llu %llu %llu",
            h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8]);

        ImGui::Separator();
        ImGui::Text("Recording: %s  markers: %u", Metrics::Recording() ? "ON" : "off", Metrics::Markers());
        ImGui::TextUnformatted("F9 record on/off   F8 marker   F10 reset histogram   END unload");
    }

    HRESULT STDMETHODCALLTYPE EndScene_Hook(IDirect3DDevice9* device)
    {
        if (!g_device) g_device = device;
        Metrics::OnEndSceneEnter();
        ImGui_CreateIfReady();

        LARGE_INTEGER overlayStart;
        QueryPerformanceCounter(&overlayStart);
        if (g_imguiReady) {
            // Exclude our own D3D calls from the counters.
            Metrics::SetInOverlay(true);
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            ImGui::Begin("Stronghold2 Analyse Hub", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            DrawMetricsWindow();
            ImGui::End();

            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            Metrics::SetInOverlay(false);
        }
        const double overlayMs = ElapsedMs(overlayStart);

        LARGE_INTEGER origStart;
        QueryPerformanceCounter(&origStart);
        const HRESULT hr = s_EndScene_Orig(device);
        Metrics::OnEndSceneExit(ElapsedMs(origStart), overlayMs);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Present_Hook(IDirect3DDevice9* device, const RECT* src, const RECT* dst,
        HWND wnd, const RGNDATA* dirty)
    {
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);
        const HRESULT hr = s_Present_Orig(device, src, dst, wnd, dirty);
        Metrics::OnPresent(ElapsedMs(start));
        return hr;
    }

    HRESULT STDMETHODCALLTYPE BeginScene_Hook(IDirect3DDevice9* device)
    {
        if (!Metrics::InOverlay()) ++Metrics::Current().scenes;
        return s_BeginScene_Orig(device);
    }

    HRESULT STDMETHODCALLTYPE SetTexture_Hook(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* tex)
    {
        if (!Metrics::InOverlay()) ++Metrics::Current().setTexture;
        return s_SetTexture_Orig(device, stage, tex);
    }

    HRESULT STDMETHODCALLTYPE DrawPrimitive_Hook(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
        UINT startVertex, UINT primCount)
    {
        if (!Metrics::InOverlay()) {
            auto& c = Metrics::Current();
            ++c.draw;
            c.primitives += primCount;
        }
        return s_DrawPrimitive_Orig(device, type, startVertex, primCount);
    }

    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive_Hook(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
        INT baseVertex, UINT minIndex, UINT numVertices, UINT startIndex, UINT primCount)
    {
        if (!Metrics::InOverlay()) {
            auto& c = Metrics::Current();
            ++c.drawIndexed;
            c.primitives += primCount;
        }
        return s_DrawIndexedPrimitive_Orig(device, type, baseVertex, minIndex, numVertices, startIndex, primCount);
    }

    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP_Hook(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
        UINT primCount, const void* data, UINT stride)
    {
        if (!Metrics::InOverlay()) {
            auto& c = Metrics::Current();
            ++c.drawUP;
            c.primitives += primCount;
        }
        return s_DrawPrimitiveUP_Orig(device, type, primCount, data, stride);
    }

    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP_Hook(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
        UINT minIndex, UINT numVertices, UINT primCount, const void* indices, D3DFORMAT indexFormat,
        const void* vertices, UINT stride)
    {
        if (!Metrics::InOverlay()) {
            auto& c = Metrics::Current();
            ++c.drawIndexedUP;
            c.primitives += primCount;
        }
        return s_DrawIndexedPrimitiveUP_Orig(device, type, minIndex, numVertices, primCount, indices,
            indexFormat, vertices, stride);
    }

    HRESULT STDMETHODCALLTYPE SetVSConstF_Hook(IDirect3DDevice9* device, UINT reg, const float* data, UINT count)
    {
        if (!Metrics::InOverlay()) {
            auto& c = Metrics::Current();
            ++c.vsConstCalls;
            c.vsConstVec4 += count;
        }
        return s_SetVSConstF_Orig(device, reg, data, count);
    }

    HRESULT STDMETHODCALLTYPE SetPSConstF_Hook(IDirect3DDevice9* device, UINT reg, const float* data, UINT count)
    {
        if (!Metrics::InOverlay()) {
            auto& c = Metrics::Current();
            ++c.psConstCalls;
            c.psConstVec4 += count;
        }
        return s_SetPSConstF_Orig(device, reg, data, count);
    }

    // Hooks one device vtable slot; logs failures instead of aborting.
    template <typename T>
    void HookSlot(void** vtbl, int index, LPVOID detour, T* original, const char* name)
    {
        LPVOID target = vtbl[index];
        const MH_STATUS created = MH_CreateHook(target, detour, reinterpret_cast<LPVOID*>(original));
        const MH_STATUS enabled = created == MH_OK ? MH_EnableHook(target) : created;
        if (enabled != MH_OK)
            Logger::Error("Hook %s (slot %d at %p) failed: %s", name, index, target, MH_StatusToString(enabled));
    }

    HRESULT STDMETHODCALLTYPE CreateDevice_Hook(
        IDirect3D9* self,
        UINT adapter, D3DDEVTYPE devType, HWND hFocusWindow,
        DWORD behaviorFlags, D3DPRESENT_PARAMETERS* params,
        IDirect3DDevice9** outDevice)
    {
        Logger::Info("IDirect3D9::CreateDevice called (HWND=%p)", hFocusWindow);

        const HRESULT hr = s_CreateDevice_Orig(self, adapter, devType, hFocusWindow,
            behaviorFlags, params, outDevice);
        if (SUCCEEDED(hr) && outDevice && *outDevice) {
            g_device = *outDevice;
            g_hwnd = hFocusWindow;

            // Hook WndProc once we know the correct HWND.
            if (g_hwnd && !g_prevWndProc) {
                g_prevWndProc = reinterpret_cast<WNDPROC>(
                    SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc_Hook)));
                Logger::Info("WndProc hooked (old=%p new=%p)", g_prevWndProc, WndProc_Hook);
            }

            // Hook device methods. All hooks only count/time and then call the original.
            // Guard against a second CreateDevice: the slots are shared code in d3d9.dll.
            if (!s_EndScene_Orig) {
                void** vtbl = *reinterpret_cast<void***>(g_device);
                HookSlot(vtbl, kReset, reinterpret_cast<LPVOID>(Reset_Hook), &s_Reset_Orig, "Reset");
                HookSlot(vtbl, kEndScene, reinterpret_cast<LPVOID>(EndScene_Hook), &s_EndScene_Orig, "EndScene");
                HookSlot(vtbl, kPresent, reinterpret_cast<LPVOID>(Present_Hook), &s_Present_Orig, "Present");
                HookSlot(vtbl, kBeginScene, reinterpret_cast<LPVOID>(BeginScene_Hook), &s_BeginScene_Orig, "BeginScene");
                HookSlot(vtbl, kSetTexture, reinterpret_cast<LPVOID>(SetTexture_Hook), &s_SetTexture_Orig, "SetTexture");
                HookSlot(vtbl, kDrawPrimitive, reinterpret_cast<LPVOID>(DrawPrimitive_Hook), &s_DrawPrimitive_Orig, "DrawPrimitive");
                HookSlot(vtbl, kDrawIndexedPrimitive, reinterpret_cast<LPVOID>(DrawIndexedPrimitive_Hook), &s_DrawIndexedPrimitive_Orig, "DrawIndexedPrimitive");
                HookSlot(vtbl, kDrawPrimitiveUP, reinterpret_cast<LPVOID>(DrawPrimitiveUP_Hook), &s_DrawPrimitiveUP_Orig, "DrawPrimitiveUP");
                HookSlot(vtbl, kDrawIndexedPrimitiveUP, reinterpret_cast<LPVOID>(DrawIndexedPrimitiveUP_Hook), &s_DrawIndexedPrimitiveUP_Orig, "DrawIndexedPrimitiveUP");
                HookSlot(vtbl, kSetVertexShaderConstantF, reinterpret_cast<LPVOID>(SetVSConstF_Hook), &s_SetVSConstF_Orig, "SetVertexShaderConstantF");
                HookSlot(vtbl, kSetPixelShaderConstantF, reinterpret_cast<LPVOID>(SetPSConstF_Hook), &s_SetPSConstF_Orig, "SetPixelShaderConstantF");
                Logger::Info("Device hooks installed (behaviorFlags=0x%08lx).", behaviorFlags);
                LogLoadedModules();
            }
        }
        return hr;
    }

    IDirect3D9* WINAPI Direct3DCreate9_Hook(UINT sdk)
    {
        Logger::Info("Direct3DCreate9 called (SDK=%u)", sdk);

        IDirect3D9* api = s_Direct3DCreate9_Orig(sdk);
        if (api) {
            // Hook CreateDevice from the vtable (index 16).
            void** vtbl = *reinterpret_cast<void***>(api);
            auto pCreateDevice = reinterpret_cast<CreateDevice_t>(vtbl[16]);

            MH_CreateHook(reinterpret_cast<LPVOID>(pCreateDevice), reinterpret_cast<LPVOID>(CreateDevice_Hook),
                reinterpret_cast<LPVOID*>(&s_CreateDevice_Orig));
            MH_EnableHook(reinterpret_cast<LPVOID>(pCreateDevice));

            Logger::Info("CreateDevice hooked at %p", pCreateDevice);
        }
        return api;
    }
} // namespace

// Public API ------------------------------------------------------------------

bool D3D9Hooks::Install()
{
    if (MH_Initialize() != MH_OK) {
        Logger::Error("MinHook initialization failed.");
        return false;
    }

    // The launcher injects before the game has initialized, so d3d9.dll may not
    // be mapped yet. Loading it here resolves to the same file the game would
    // load (game folder first, e.g. DXVK, then system).
    if (!LoadLibraryW(L"d3d9.dll")) {
        Logger::Error("LoadLibraryW(d3d9.dll) failed (%lu).", GetLastError());
        return false;
    }

    // Hook the exported entry point in d3d9.dll.
    if (MH_CreateHookApi(L"d3d9.dll", "Direct3DCreate9",
        reinterpret_cast<LPVOID>(Direct3DCreate9_Hook),
        reinterpret_cast<LPVOID*>(&s_Direct3DCreate9_Orig)) != MH_OK)
    {
        Logger::Error("CreateHookApi(Direct3DCreate9) failed.");
        return false;
    }

    MH_EnableHook(MH_ALL_HOOKS);
    Logger::Info("D3D9 export hook installed (Direct3DCreate9).");
    return true;
}

void D3D9Hooks::Uninstall()
{
    if (g_hwnd && g_prevWndProc) {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_prevWndProc));
        g_prevWndProc = nullptr;
    }

    ImGui_Destroy();

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    Metrics::Shutdown();

    Logger::Info("All hooks removed.");
}

HWND D3D9Hooks::GetGameHwnd() { return g_hwnd; }
IDirect3DDevice9* D3D9Hooks::GetDevice() { return g_device; }
