# Stronghold2AnalyseHub

A lightweight analysis & overlay DLL for **Stronghold 2 (DX9/Win32)**.  
The goal is to understand how the game engine ticks (rendering order, device lifecycle, input, timing) and to prepare a clean path toward **modding**, visual debugging, and eventually research into graphics and AI/pathfinding.

> Maintainer: **crycodebaby (Germany)**  
> Community contributions welcome.

---

## What this project does

- Hooks **Direct3D9** creation and device lifecycle:
  - `Direct3DCreate9` → `IDirect3D9::CreateDevice` → `IDirect3DDevice9::{EndScene, Reset}`
- Sets up a minimal **ImGui** overlay for in-frame tools and telemetry.
- Hooks the game window **WndProc** to route input to ImGui.
- Provides a small, thread-safe **logger** with timestamps, PID/TID.

This is intentionally modest and readable—meant to be a solid base for deeper analysis (render passes, device resets, wireframe toggles, pathfinding visualizations, etc.).

---

## Quick start

1. **Clone** the repo and open the Visual Studio solution.
2. Place **ImGui** and **MinHook** sources in `external/` (see folder layout above).
3. Build the project as **Win32 / Release**, **DLL**.
4. Copy `d3d9.dll` (proxy) and `Stronghold2AnalyseHub.dll` next to the game’s `.exe` and start the game normally
   (see *Run* below; injecting with `CreateRemoteThread + LoadLibraryW` also works).
5. Look for `Stronghold2AnalyseHub.log` in the output folder (default `SH2Perf_out` next to the game’s `.exe`) to follow the timeline:
   - D3D9 creation
   - Device creation
   - Device reset
   - Per-frame overlay rendering

**Hotkeys**
- `END` — request unload (removes hooks cleanly, shuts down ImGui, flushes logs)

---

## Why this approach

- Hooking from the **export** (`Direct3DCreate9`) makes us resilient across builds—no brittle signatures just to get the device.
- We obtain the **correct HWND** via `CreateDevice`, ensuring input routing is reliable.
- Minimal moving parts (DX9, Win32, ImGui, MinHook) that are well-understood and easy to review.

---

## Roadmap (community-driven)

- Render-state experiments (wireframe/overdraw)
- Frame & sim tick correlation
- Safely reading game structures (telemetry first, no patching initially)
- Visualization layers for navigation graphs / pathfinding
- ~~Optional DX9 proxy mode (`d3d9.dll`) for zero-setup injection~~ done, see *Run*

## Perf metrics (branch `perf-metrics`)

Read-only telemetry; nothing changes how the game renders or simulates.

- Per frame (frame = `Present` to `Present`): frame time, time inside `Present`, time inside `EndScene`,
  time between `EndScene` calls, overlay cost.
- Per-frame counters: `BeginScene`, `DrawIndexedPrimitive`, `DrawPrimitive`, `DrawPrimitiveUP`,
  `DrawIndexedPrimitiveUP`, primitives, `SetTexture`, `Set{Vertex,Pixel}ShaderConstantF` (calls + vec4 count).
  Draw calls made by the overlay itself are excluded.
- Frame-time histogram (<8.3, <16.7, <33.3, <50, <100, <250, <500, <1000, >=1000 ms) and a 600-frame plot.
- Process virtual address space in use (relevant for LARGE_ADDRESS_AWARE tests).
- Per-thread CPU usage once per second (which thread is busy, which one renders).
- Loaded module list written to the log once (which DLL copies the loader picked).

Hotkeys: `F9` record on/off, `F8` marker, `F10` reset histogram, `END` unload.
Recording writes `SH2Perf_<timestamp>_frames.csv` and `_threads.csv` next to the game exe.

### Build (CMake, 32-bit)

```
cmake -S . -B build -A Win32
cmake --build build --config Release
```

Produces `build/Release/Stronghold2AnalyseHub.dll` (static CRT) and `build/Release/SH2Launcher.exe`.

### Run (d3d9.dll proxy, default)

Put next to `Stronghold2.exe`: `d3d9.dll` (proxy), `Stronghold2AnalyseHub.dll`, optionally `SH2Proxy.ini`.
Then start the game normally. The game loads the proxy as its `d3d9.dll`; the proxy loads the real
d3d9 and the hub, and its `Direct3DCreate9` waits until the hub's hook is installed. No injector,
no suspended process.

`SH2Proxy.ini` (all keys optional, ASCII):

```
[proxy]
target=system                     ; real d3d9: system dir, or e.g. d3d9_dxvk.dll (DXVK renamed)
hub=1                             ; 0 = pass-through only, hub not loaded (no rebuild needed)
hubdll=Stronghold2AnalyseHub.dll  ; relative to the proxy's folder
out=SH2Perf_out                   ; output dir for logs/CSVs, relative to the proxy's folder
```

Environment overrides: `SH2HUB_DISABLE=1` (hub off), `SH2HUB_OUT=<dir>` (output dir).
DXVK: rename DXVK's x32 `d3d9.dll` to `d3d9_dxvk.dll` and set `target=d3d9_dxvk.dll`.
Back to vanilla: delete `d3d9.dll`, `Stronghold2AnalyseHub.dll`, `SH2Proxy.ini`.

Output folder:
- `SH2Proxy.log`: loader steps (config, real d3d9 path, hub ready / timeout / disabled), rewritten per start.
- `Stronghold2AnalyseHub.log`: hub timeline (appended), including the game exe's sha256 and
  `known build: yes/no` (log only; Steam 1.5 is the known build).
- Debug messages (`SH2Proxy:` / `SH2Hub:`) are also sent to `OutputDebugString` (DebugView).

Steam must be running; when the game is started directly, put a `steam_appid.txt` containing
`40960` next to `Stronghold2.exe`.

### Run (launcher, fallback)

```
SH2Launcher.exe "C:\path\to\Stronghold2.exe" [path\to\Stronghold2AnalyseHub.dll] [--out <dir>]
```

Starts the game suspended, injects the DLL, waits until the `Direct3DCreate9` hook is installed and
then resumes the game. Do not combine with the proxy (the hub would be loaded twice).
Reports if the game process ends before the hub is ready (with exit code).

Note: hub builds up to `d29d935` aborted the host process right after loading ("Runtime Error!
abnormal program termination", empty log) because the logger locked its mutex twice. Fixed since.

---

## Notes

- 32-bit only (the game is 32-bit).
- Keep changes small and reviewable. Good comments matter more than clever macros.
- Prefer reading/logging before altering behavior when exploring the engine.

---

## License

MIT for our code. ImGui and MinHook maintain their respective licenses—please see their repositories.
