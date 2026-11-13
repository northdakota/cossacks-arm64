# Cossacks on Mac

Work in progress: running the original *Cossacks: Back to War* engine (`dmcr.exe`, GOG edition)
natively on a Mac with Apple Silicon. The unmodified game code runs in a small x86 emulator
(Unicorn); every Windows function it calls is rewritten for macOS (SDL2, FFmpeg).

Stage 1 (feasibility) is in [assessment.md](assessment.md) and [imports.md](imports.md). Known issues, fixes and workarounds: [docs/known-issues.md](docs/known-issues.md).

## Origin and licence

The host (`src/win32/`) is derived from
[zeus-poseidon-arm64](https://github.com/nic0lasm/zeus-poseidon-arm64) by its authors and
contributors, licensed GPL-2.0-or-later, copied from `origin/main` at commit `9b676e5`
on 2026-11-13. Since then it has been modified for Cossacks: Zeus-specific code removed, DLL
loading with relocations, 8-bit DirectDraw with palettes, DirectSound, MCI, and more; the git
history records each change.

This project is distributed under the same licence, GPL-2.0-or-later (see [LICENSE](LICENSE)),
for its own code and documentation only, never the game. It relies on the
[Unicorn engine](https://www.unicorn-engine.org/) (GPL-2.0), SDL2 (zlib) and FFmpeg.

## How it is made

The port is written with [Claude Code](https://claude.com/claude-code), Anthropic's AI coding
assistant, working with the project's author: analysis of the game's executable, the host code
and the documentation. The commits do not carry it as a co-author; this note is the credit.

## Legal

An independent, non-commercial fan project, not affiliated with or endorsed by GSC Game World
or GOG. The repository contains no executable, artwork, sound, text or data from the game, and
circumvents no copy protection: it only runs the DRM-free GOG edition, from files you provide.
Game files (`original/`, the installer) are kept out of git.

## Building

```bash
cmake -S . -B build -G Ninja && cmake --build build
```

```bash
build/cossacks-run original/back_to_war/app/dmcr.exe
```
