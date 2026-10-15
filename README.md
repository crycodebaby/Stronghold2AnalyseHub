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
Recording writes `SH2Perf_<timestamp>_frames.csv` (and `_threads.csv` with `poll`, `_samples.csv` with the sampler)
plus `latest.json` into the output dir (`SH2HUB_OUT`, `out=` in `SH2Proxy.ini`, default `SH2Perf_out`).

### Measurement switches and capture pipeline

What the hub does inside the game's frame is limited on purpose. Four independent switches decide which hooks
exist at all (env `SH2HUB_<KEY>` > `SH2Proxy.ini` `[proxy]` > profile; values `1/0`, `on/off`):

| Switch | Effect |
|---|---|
| `frametime` | `Present` hook: frame time + time inside `Present`; master switch of recording |
| `counters` | hooks on BeginScene, SetTexture, DrawPrimitive*, DrawIndexedPrimitive*, Set{Vertex,Pixel}ShaderConstantF; off = not hooked |
| `poll` | per-thread CPU and address space once per second, only while recording, on the capture thread |
| `overlay` | ImGui window (EndScene, Reset, WndProc hooks); off = not hooked, EndScene timing columns are 0 |
| `beep` | tone on recording start/stop (default on when the overlay is off) |

`profile=full` (default, all four on) or `profile=min` (`frametime` only). All switches off = no hook installed.
The sampling profiler stays separate (`SH2HUB_SAMPLE`, default off).

The render thread only copies one fixed-size record per frame into a bounded lock-free ring (8,192 frames) and never
waits. A low-priority capture thread formats the CSV, writes the files and does start/stop work. If the ring is full
the frame is dropped and counted; drops, open failures and write errors are logged, shown in the overlay and written to
`latest.json` (`capture_ok`, `frames_dropped`, `write_errors`, `modes`; `capture_version: 2`). The CSV columns are
unchanged. Details and the compatibility rule: SH2-Perf `docs/FORMATS/HUB_CAPTURE.md`, decision D011.

Tests without the game: `build/Release/capture_test.exe` (ring, overflow, open/write errors, lifecycle, push cost),
`sampler_test.exe`, `standin_d3d9.exe`.

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
`SH2HUB_AUTORECORD=<frames>`: record without key presses (starts after 30 frames, stops after about
`<frames>` frames); used for automated tests.
DXVK: rename DXVK's x32 `d3d9.dll` to `d3d9_dxvk.dll` and set `target=d3d9_dxvk.dll`.
Back to vanilla: delete `d3d9.dll`, `Stronghold2AnalyseHub.dll`, `SH2Proxy.ini`.

Output folder:
- `SH2Proxy.log`: loader steps (config, real d3d9 path, hub ready / timeout / disabled, calling module of
  every `Direct3DCreate9`), rewritten per start.
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

## Recording provenance (perf-metrics branch, SH2-Perf D012)

Every recording writes its own `<stem>_manifest.json` next to the CSVs: capture id, session, planned conditions from
`SH2Proxy.ini [plan]` (`experiment`, `protocol`, `scenario`, `game_speed`, `paused`), the SHA-256 of the game exe, the hub
DLL, the d3d9 files and the ini **as hashed when the hub was loaded in that session**, the effective switches with their
source, the file list with sizes and hashes, and the capture health. The manifest is first written with `status: recording`
and published as `finalized` only after the capture files are closed and hashed; `latest.json` is just a pointer.
All of this runs on the capture thread or a background hashing thread. The hub reads no game memory: game speed and pause
are only what was planned in the ini. Format and import rules: SH2-Perf `docs/FORMATS/CAPTURE_MANIFEST.md`.
