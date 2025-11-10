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
4. Inject the produced DLL into the Stronghold 2 process (any standard loader using `CreateRemoteThread + LoadLibraryW` will do).
5. Look for `Stronghold2AnalyseHub.log` next to the game’s `.exe` to follow the timeline:
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
- Optional DX9 proxy mode (`d3d9.dll`) for zero-setup injection

---

## Notes

- 32-bit only (the game is 32-bit).
- Keep changes small and reviewable. Good comments matter more than clever macros.
- Prefer reading/logging before altering behavior when exploring the engine.

---

## License

MIT for our code. ImGui and MinHook maintain their respective licenses—please see their repositories.
