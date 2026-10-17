# Contributing to Stronghold2AnalyseHub

This repository is the in-game side of a measurement project: proxy, hub DLL, overlay and native tests. Research
questions, findings and experiment protocols are kept in
[Stronghold2PerformanceResearch](https://github.com/crycodebaby/Stronghold2PerformanceResearch); read its README and
`docs/STATE.md` before proposing hub changes, so a change answers a concrete measurement question.

## Set up and test

Follow "Build" and "Tests without the game" in the [README](README.md). Before opening a pull request:

```
cmake -S . -B build -A Win32          # again after each commit (the commit id is baked in)
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure -LE timing
ctest --test-dir build -C Release -L timing     # sampler overhead only, load-dependent; report the numbers
```

If you have the research repository next to this one (`../Stronghold2PerformanceResearch` or the local name
`../SH2-Perf`), also run its end-to-end stand-in check, which uses the real proxy and hub DLL from your build:
`python -I tools/e2e_standin.py` (Windows with an interactive desktop).

## Rules for changes

- **Read-only by default.** Hooks observe and forward; they do not change rendering, simulation, timing or game data.
  A change that alters game behaviour (cache, skip, patch) is a separate, switchable experiment that needs a written
  measurement plan in the research repository first.
- **Nothing blocking on the render thread.** No file, heap, lock or wait in a hook; work goes to the capture thread.
- **Every new switch defaults to off** and is recorded with its source in the manifest. A switch that is off installs no
  hook.
- **Losses are reported, never hidden.** Bounded tables and rings count their drops; tests cover the overflow path.
- **A game-free test for every change** in `tests/`, registered in `CMakeLists.txt` with `add_test`. Tests write only
  under `%TEMP%` and exit 0 on success. Timing-sensitive checks get the label `timing`.
- Keep commits small and focused; do not mix runtime changes with documentation or build changes. Explain *why* in the
  commit message.
- Vendored code in `external/` keeps its license; when you update it, update `THIRD_PARTY_NOTICES.md` too.

## Never commit

Game executables or DLLs (including the real `granny2.dll`), game data or assets, Ghidra databases, decompiled game code,
raw recordings, or logs with personal paths. The `.gitignore` excludes build output; check `git status` anyway.

## Discord notices

`.github/workflows/discord-notify.yml` posts a short notice to the community Discord when a pull request is opened or
merged (#changelog) and when a release is published (#announcements). It needs two repository secrets with Discord
webhook URLs: `DISCORD_WEBHOOK_CHANGELOG` and `DISCORD_WEBHOOK_ANNOUNCEMENTS`. Without them the workflow skips and
nothing fails. Titles and the first lines of the description are posted as plain text, so write them to be read there.

## Reporting a problem from the game

Attach the relevant log lines and the `*_manifest.json` of the recording, with absolute paths and your user name
replaced (for example `C:\Users\<user>\...`). State the hub commit (`git describe --always --dirty --abbrev=12`), the
game exe SHA-256 shown in the log and which switches were on. Say what you observed separately from what you think the
cause is.
