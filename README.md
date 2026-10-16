# Stronghold2AnalyseHub

A d3d9 proxy, a measurement DLL ("hub") with an optional ImGui overlay, and game-free native tests for
**Stronghold 2 (Steam 1.5, 32-bit, Direct3D 9)**.

> Maintainer: **crycodebaby (Germany)**
> Community contributions welcome.

> **Status: research instrumentation, not a performance mod.** The hub observes the game; it does not make it
> faster. No performance improvement has been shown so far. The measurement results, the analysis tools and the
> current research plan live in the companion repository
> **[Stronghold2PerformanceResearch](https://github.com/crycodebaby/Stronghold2PerformanceResearch)** (start with its
> README). This repository holds only the code that runs inside or next to the game.

## What it does

| Part | Path | Role |
|---|---|---|
| d3d9 proxy | `proxy/` | `d3d9.dll` placed next to the game exe; loads the real d3d9 (system or DXVK) and the hub, holds `Direct3DCreate9` until the hub's hook is in place |
| Hub DLL | `Stronghold2AnalyseHub/` | D3D9 hooks (MinHook), per-frame timing and counters, capture pipeline, manifests, optional overlay, sampler, Granny observer |
| Launcher | `launcher/` | fallback: starts the game suspended and injects the hub (do not combine with the proxy) |
| Tests | `tests/` | capture pipeline, deactivation lifecycle, sampler, Granny observer with a stand-in `granny2.dll`, D3D9 stand-in program |
| Vendored | `external/` | Dear ImGui 1.92.5 WIP, MinHook (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)) |

Data path: the render thread only copies one fixed-size record per frame into a bounded lock-free ring (8,192 frames)
and never waits. A low-priority capture thread formats the CSVs, writes the files, polls thread CPU/address space and
publishes a per-recording manifest. Drops, open failures and write errors are counted and reported, never hidden.

```
Stronghold2.exe ── d3d9.dll (proxy) ──► real d3d9 (system or d3d9_dxvk.dll)
                        └──► Stronghold2AnalyseHub.dll ── hooks (Present, EndScene, Draw*, ...) ──► ring
                                                         capture thread ──► SH2Perf_out/ (CSV, manifest, logs)
```

## Build (Windows, 32-bit)

Requirements: Visual Studio 2022 or its Build Tools with the C++ workload (MSVC v143, Windows SDK), CMake ≥ 3.21,
Git. Verified with CMake 4.4.4, MSVC 19.44, Windows SDK 10.0.26100.

```
git clone https://github.com/crycodebaby/Stronghold2AnalyseHub
cd Stronghold2AnalyseHub
cmake -S . -B build -A Win32
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure -LE timing
```

- Clone to a short path (for example `C:\src\`). MSBuild fails with "path exceeds 260 characters" when the build
  folder sits deep inside a long path such as `%TEMP%\...`.
- `git describe` is baked into the DLL at configure time: re-run the configure step after every commit so recordings
  name the exact hub build (`-dirty` marks an uncommitted tree).
- Outputs in `build/Release/`: `Stronghold2AnalyseHub.dll`, `d3d9.dll` (proxy), `SH2Launcher.exe`, the test programs
  and a **test-only** `granny2.dll` stand-in. Never copy that stand-in into a game folder.
- The empty `Stronghold2AnalyseHub.sln` and `CppProperties.json` in the root are leftovers from the original Visual
  Studio layout; CMake is the only supported build.

## Tests without the game

| Test | Checks | Typical time |
|---|---|---|
| `capture_test` | ring, overflow, open/write errors, lifecycle, push cost, manifests and provenance | ~13 s |
| `lifecycle_test` | blocked writer / sampler / hashing thread at deactivation, unload order | ~12 s |
| `granny_observer_test` | observer off = no import slot touched; every call forwarded once with bit-identical arguments; scripted data; table/row limits; restore on END. Uses the stand-in `granny2.dll` | < 1 s |
| `sampler_functional` (`sampler_test --checks functional`) | mode from the environment, samples file written, > 100 samples in 3 s, attribution of a busy and a waiting thread (gating) | ~9 s |
| `sampler_overhead` (`sampler_test --checks overhead`, label `timing`) | the same run, plus busy-loop slowdown below 5 % (reported, not gating) | ~9 s |

All tests write only into a new folder under `%TEMP%` and exit with 0 on success. Only the sampler's 5 % overhead
threshold is load- and hardware-dependent: in repeated runs on 2026-10-16 the development machine measured between
3.8 % and 6.3 %, mostly above 5 %, while other work was running (load is a possible, unproven cause); every functional
check passed in every run. It is
therefore a separate CTest entry that CI reports without gating; the functional checks gate. The threshold is unchanged. The end-to-end run with the real proxy and hub DLL against a
D3D9 stand-in program (`e2e_standin.py`) lives in the research repository because it uses its import tools.

What these tests do **not** show: behaviour inside Stronghold 2, hook latency in the real game, or that the observer is
neutral with the real `granny2.dll`. In-game checks need your own copy of the game (see below).

## Run (d3d9.dll proxy, default)

Use a **copy** of the game folder for experiments. Put next to `Stronghold2.exe`: `d3d9.dll` (proxy),
`Stronghold2AnalyseHub.dll`, optionally `SH2Proxy.ini`. Start the game normally. The hub logs the game exe's SHA-256
and `known build: yes/no` (Steam 1.5 is the known build). Steam must be running; when the exe is started directly, a
`steam_appid.txt` containing `40960` next to it is needed.

`SH2Proxy.ini` (all keys optional, ASCII):

```
[proxy]
target=system                     ; real d3d9: system dir, or e.g. d3d9_dxvk.dll (DXVK renamed)
hub=1                             ; 0 = pass-through only, hub not loaded
hubdll=Stronghold2AnalyseHub.dll  ; relative to the proxy's folder
out=SH2Perf_out                   ; output dir, relative to the proxy's folder
profile=full                      ; full (default) or min; single switches below override it
[plan]                            ; planned conditions, copied into every manifest (never read from the game)
experiment=EXP-000
game_speed=90
```

Environment overrides: `SH2HUB_DISABLE=1`, `SH2HUB_OUT=<dir>`, `SH2HUB_<SWITCH>=0|1`, `SH2HUB_SAMPLE=off|module|function`,
`SH2HUB_GRANNY=0|1`, `SH2HUB_AUTORECORD=<frames>` (record without key presses, for automated tests).
DXVK: rename DXVK's x32 `d3d9.dll` to `d3d9_dxvk.dll` and set `target=d3d9_dxvk.dll`.
Back to vanilla: delete `d3d9.dll`, `Stronghold2AnalyseHub.dll`, `SH2Proxy.ini`.

Launcher (fallback): `SH2Launcher.exe "C:\path\to\Stronghold2.exe" [path\to\Stronghold2AnalyseHub.dll] [--out <dir>]`.

### Hotkeys

`F9` record on/off · `F8` marker · `F10` reset histogram · `END` deactivate the hub (not an unload).
Without the overlay, F9 gives a high (start) / low (stop) tone (`beep` switch).

### Measurement switches

Env `SH2HUB_<KEY>` > `SH2Proxy.ini` `[proxy]` > profile. A switch that is off installs no hook at all.

| Switch | Effect | Profile `full` / `min` |
|---|---|---|
| `frametime` | `Present` hook: frame time and time inside `Present`; master switch of recording | on / on |
| `counters` | BeginScene, SetTexture, Draw*, Set{Vertex,Pixel}ShaderConstantF counts | on / off |
| `poll` | per-thread CPU and address space once per second, on the capture thread | on / off |
| `overlay` | ImGui window (EndScene, Reset, WndProc hooks); off = EndScene timing columns are 0 | on / off |
| `beep` | tone on recording start/stop | default on when the overlay is off |
| `sample` | render-thread sampling profiler (`module` or `function`), separate, default off; biases frame times by ~2-5 % in the game | — |
| `granny` | Granny import-boundary observer, separate, default off; **research only**, see limits below | — |

### Output files (in `SH2Perf_out/`)

Per recording: `SH2Perf_<ts>_frames.csv`, `_threads.csv` (with `poll`), `_samples.csv` (with the sampler),
`_granny*.{json,csv}` (with the observer), `<stem>_manifest.json` (capture id, planned conditions, SHA-256 of exe, hub,
d3d9 files and ini as hashed at load, effective switches with their source, file hashes, capture health; written as
`recording`, published as `finalized` after the files are closed) and `<stem>_prepared.json`. `latest.json` only points
at the newest recording. Logs: `SH2Proxy.log` (loader, rewritten per start) and `Stronghold2AnalyseHub.log` (appended).
Formats: research repo `docs/FORMATS/HUB_CAPTURE.md`, `CAPTURE_MANIFEST.md`, `GRANNY_OBSERVER.md`.

Logs and manifests contain absolute paths of loaded modules, which usually include your user name. Remove them before
sharing a log (see CONTRIBUTING.md).

## Known limits

- **No performance gain.** All hooks only read. The sampler and the Granny observer add measurable work; frame times
  from sampled or observed runs are biased and are not benchmark results.
- **Granny observer (research only).** It redirects six exe→`granny2.dll` imports on the render thread and forwards every
  call unchanged. In the latest real-game run (hub `32ebc38`, recording `rec-20261015T122211Z-dff71642`) it still lost
  126,632 per-instance aggregations to probe failures, so per-instance tables are incomplete; frame-level call counts
  are complete. 609 final calls are in the totals without an emitted frame row, although frame drops read 0. Its
  `all_observed_same` field is not a general sample-to-sample clock comparison. The native test uses a fake
  `granny2.dll` and proves neither neutrality with the real library nor complete coverage. Details and the independent
  audit: research repo `docs/evidence/EXP-008R_G1_PARTIAL.md` and `audits/2026-10-16-independent/`.
- `END` deactivates; it does not unload. The DLL and its MinHook trampolines stay mapped until the game exits, so
  re-activation or another hub build needs a game restart. A permanently blocked background thread defers deactivation.
- A normal game exit does **not** finalize a running recording; stop with `F9` and wait for `manifest published` in the
  log first.
- Win32/Direct3D 9 only. Only the Steam 1.5 exe (SHA-256 `f3ccdf9b…f32d34b`) has been run with the hub.

## Version pair

The verified pair for the first public release is listed in the research repository (`README.md`, "Verified version
pair"). On this branch the runtime sources (`Stronghold2AnalyseHub/`, `proxy/`, `launcher/`) are identical to
`perf-metrics` commit `32ebc38a05ea`, the build used for the latest in-game recordings; later commits change only
documentation, licensing, test registration and the check selection of `tests/sampler_test.cpp`. All earlier commit
ids are kept (no rebase or squash), because recording manifests name them.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md); agents start with [AGENTS.md](AGENTS.md). Findings and experiment plans belong
in the research repository.

## License

MIT for this project's own code ([LICENSE.txt](LICENSE.txt)). Dear ImGui and MinHook keep their licenses
([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). Stronghold 2 and its files are not part of this repository.
