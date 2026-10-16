# AGENTS.md

Instructions for automated coding agents (any tool). Humans: see [CONTRIBUTING.md](CONTRIBUTING.md).

## Read first

1. [README.md](README.md): parts, build, switches, known limits.
2. [CONTRIBUTING.md](CONTRIBUTING.md): rules for changes and what never to commit.
3. The research repository's `AGENTS.md` and `docs/STATE.md`
   ([Stronghold2PerformanceResearch](https://github.com/crycodebaby/Stronghold2PerformanceResearch)) for the current
   question. Do not start hub work that no open research task asks for.

## Boundaries

- Hooks only read and forward. No behaviour-changing patch, cache or skip without an explicit task and a measurement plan.
- Never touch an original game installation; experiments run on a separate copy and only when the task says so.
- No game files, real `granny2.dll`, Ghidra databases, decompiled code, raw recordings or personal paths in the repo.
- Do not push, publish or open pull requests unless asked.
- Keep runtime changes, build changes and documentation in separate commits.

## Done when

- `cmake --build build --config Release` succeeds for `-A Win32` and
  `ctest --test-dir build -C Release --output-on-failure -LE timing` passes; `-L timing` results are reported as numbers.
- New behaviour has a game-free test registered with `add_test`.
- README "Known limits" and the research repository's format docs (`docs/FORMATS/`) match the change.
- The final report separates what was tested without the game from anything that would need the game.
