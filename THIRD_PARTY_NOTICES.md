# Third-party notices

The project's own code is under the MIT license in [LICENSE.txt](LICENSE.txt). Scope of that license (until
2026-10-17 appended to `LICENSE.txt`, moved here unchanged so that the file contains only the MIT text): "This license
covers the project's own files only. Files under external/ keep their own licenses; see THIRD_PARTY_NOTICES.md."

The following vendored sources keep their original licenses and copyright notices. They are compiled into the hub DLL and the test programs, so binary
redistributions must carry these notices as well.

| Path | Component | Version in tree | License | Full text |
|---|---|---|---|---|
| `external/imgui/` | Dear ImGui (core + DX9/Win32 backends), Omar Cornut | 1.92.5 WIP (`IMGUI_VERSION` in `imgui.h`) | MIT | [external/imgui/LICENSE.txt](external/imgui/LICENSE.txt) |
| `external/minhook/` | MinHook, Tsuda Kageyu | headers say 2009-2017; exact upstream commit not recorded | BSD 2-Clause | [external/minhook/LICENSE.txt](external/minhook/LICENSE.txt) |
| `external/minhook/src/hde/` | Hacker Disassembler Engine 32 C, Vyacheslav Patkov (part of MinHook) | as shipped with MinHook | BSD 2-Clause | same file, second section |
| `external/minhook/src/hde/pstdint.h` | portable `stdint.h`, Paul Hsieh (shipped with MinHook) | as shipped with MinHook | BSD-style, full text in the file header | in the file |

The two `LICENSE.txt` files were added on 2026-10-16 as verbatim copies of the upstream files
(`ocornut/imgui` `LICENSE.txt`, tag `v1.92.5`; `TsudaKageyu/minhook` `LICENSE.txt`, branch `master`); the vendored
sources themselves are unchanged. The copyright lines match the headers of the vendored files.

## Not part of this repository

- Stronghold 2, its executable, `granny2.dll` (RAD Game Tools Granny runtime), game data and assets are proprietary and
  are neither included nor licensed here. `tests/fake_granny2.cpp` is our own stand-in that only reproduces the names
  and calling convention of six imports so the observer can be tested without the real library.
- Direct3D 9 / Windows SDK headers and libraries come from the local Windows SDK at build time.
