# Profiling

## 2026-11-14: a random-map battle against three Hard AIs

Scenario: 1024 × 768, random map, three AI players (Hard), our side idle; deterministic and
headless; the profile counts from 120 s to 340 s of the program's clock (`--profile-from 120`),
with some scrolling. Summed per function by `tools/profile_functions.py` (function starts: the
engine's 552 exported names and every target of a direct `call`; measurement cost of 18 ns per
block taken off, as in zeus-poseidon-arm64's `tools/profile_report.py`).

```bash
build-dev/cossacks-run original/back_to_war/app/dmcr.exe --headless --deterministic --write-dir run/prof --seconds 340 --profile run/battle.tsv --profile-from 120 --script "…"
```

```bash
python3 -I tools/profile_functions.py run/battle.tsv dmcr.asm 40
```

Corrected time 23.1 s over 574 functions; the 10 heaviest take 54 %, the 40 heaviest 80 %.

| Rank | Address | Share | What it is (so far) |
|---|---|---|---|
| 1 | host | 13.3 % | the host's functions; mostly presenting frames (inside `GetTickCount`, see the pacing notes) |
| 2 | 0x470794 | 6.6 % | **copy of a rectangle of the drawing buffer to the screen** (`RefreshView` and others), `rep movsd` per row — **replaced natively** |
| 3 | 0x4c6f80 | 5.4 % | ground drawing (writes the terrain pixels; see the windmill investigation) |
| 4 | 0x478217 | 4.9 % | near `LoadFog`: fog of war or shading |
| 5, 6 | 0x541cdb, 0x541c65 | 4.8 %, 4.7 % | simulation (same call counts; a grid walk with constant 0x10812: path finding or a map field) |
| 7 | 0x47bdce | 4.2 % | sprite drawing (GP), a variant of the blitter below |
| 8 | 0x47b491 | 3.9 % | sprite blitter of `ShowGP` / `ShowGPLayers` (RLE segments, `rep movsd` / `rep movsb`) |
| 9 | 0x4c9f90 | 3.6 % | ground drawing |
| 10 | 0x47da93 | 2.9 % | sprite drawing (GP, palette variant) |
| 11 | 0x541264 | 2.6 % | simulation |
| 12–14 | 0x47924f, 0x448d3f, 0x47920f | 2.0 %, 1.5 %, 1.5 % | fog; line drawing (`xLine` area: minimap or selection) |

The copy loops are the heaviest part in the emulator: Unicorn runs every iteration of a `rep`
string instruction as a block of its own (hundreds of millions of blocks in this profile), where
native code copies the same bytes in a fraction of the time.

## Native replacements

Each replacement is in `src/win32/native.cpp`, installed only if the original's bytes are the
expected ones, and checked by comparing the frame log of a deterministic run with and without it
(`--no-native`): every frame must hash the same.

| Function | Status | Check | Gain |
|---|---|---|---|
| 0x470794 copy to the screen | done 2026-11-14 | 5075 frames identical (deterministic run, 120 s with a battle against three AIs) | user CPU time of that run 39.2 s → 29.3 s (−25 %) |

Next, in order of share and of how well they are understood: the sprite blitters (0x47b491 and its
variants 0x47bdce, 0x47da93: format known from the windmill investigation, clipping paths to
reproduce), then the ground drawing (0x4c6f80, 0x4c9f90), the fog (0x478217 …), and the simulation
functions (0x541cdb, 0x541c65, 0x541264), which need to be understood first. A state log (as in
zeus-poseidon-arm64's `--state-log`) would make the checks of the simulation functions stronger than
the frame log alone.
