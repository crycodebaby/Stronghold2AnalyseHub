#include "d3d9_hooks.h"
#include "../core/logger.h"

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

    // Original pointers
    Direct3DCreate9_t   s_Direct3DCreate9_Orig = nullptr;
    CreateDevice_t      s_CreateDevice_Orig = nullptr;
    Reset_t             s_Reset_Orig = nullptr;
    EndScene_t          s_EndScene_Orig = nullptr;

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

    HRESULT STDMETHODCALLTYPE EndScene_Hook(IDirect3DDevice9* device)
    {
        if (!g_device) g_device = device;
        ImGui_CreateIfReady();

        if (g_imguiReady) {
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            // Minimal overlay; keep it small and useful. Extend as needed.
            ImGui::Begin("Stronghold2 Analyse Hub", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::Text("Device: %p  HWND: %p", g_device, g_hwnd);
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

            static bool show_demo = false;
            ImGui::Checkbox("Show ImGui Demo", &show_demo);
            if (show_demo) ImGui::ShowDemoWindow(&show_demo);

            ImGui::End();

            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        }
        return s_EndScene_Orig(device);
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

            // Hook device methods (Reset / EndScene)
            void** vtbl = *reinterpret_cast<void***>(g_device);
            auto pReset = reinterpret_cast<Reset_t>(vtbl[16]);
            auto pEndScene = reinterpret_cast<EndScene_t>(vtbl[42]);

            MH_CreateHook(reinterpret_cast<LPVOID>(pReset), reinterpret_cast<LPVOID>(Reset_Hook), reinterpret_cast<LPVOID*>(&s_Reset_Orig));
            MH_CreateHook(reinterpret_cast<LPVOID>(pEndScene), reinterpret_cast<LPVOID>(EndScene_Hook), reinterpret_cast<LPVOID*>(&s_EndScene_Orig));

            MH_EnableHook(reinterpret_cast<LPVOID>(pReset));
            MH_EnableHook(reinterpret_cast<LPVOID>(pEndScene));

            Logger::Info("Device hooks installed (Reset=%p, EndScene=%p)", pReset, pEndScene);
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

    Logger::Info("All hooks removed.");
}

HWND D3D9Hooks::GetGameHwnd() { return g_hwnd; }
IDirect3DDevice9* D3D9Hooks::GetDevice() { return g_device; }
