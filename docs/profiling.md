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
(`--no-native`): every frame must hash the same. `COSSACKS_NATIVE_STATS=1` prints at exit how often
each path of the blitters ran (a check only covers the paths that ran); `COSSACKS_NATIVE_OFF=0x47bdce,…`
leaves the listed functions to the emulator, to measure one replacement or to find which one a
difference comes from.

| Function | Status | Check | Gain |
|---|---|---|---|
| 0x470794 copy to the screen | done 2026-11-14 | 5075 frames identical (deterministic run, 120 s with a battle against three AIs) | user CPU time of that run 39.2 s → 29.3 s (−25 %) |
| 0x47b491 sprite blitter (copies the frame's pixels) | done 2026-11-14 | 5956 frames identical (the 200 s run below); paths: whole 397 491, left-clipped 5 067, right-clipped 25 328, top-clipped 36 110, outside 167 339 | see below |
| 0x47bdce remap blitter (pixel under the frame = table[pixel]: shadows and the like) | done 2026-11-14 | 5956 frames identical; whole 295 311, left 1 789, right 17 594, top 18 139, outside 130 535 | about 0.5 s of 23.5 s (2 %) |
| 0x47da93 blend blitter (pixel = table[frame pixel × 256 + pixel]) | done 2026-11-14 | 5956 frames identical; whole 679 899, left 2 624, right 13 404, outside 38 764; **the top-clipped path did not run** (it is the code the two other blitters share and that ran there) | about 0.5 s (2 %) |

The three blitters are one function in the engine's source, compiled three times with a different
inner loop (the listings differ only there, and in the bias at 0x5e06fc that only 0x47b491 adds to
the pixel pointer); `native.cpp` has them as one template (`blit_frame`) with three loops.

Timing (2026-11-14, M4 Pro): the deterministic run used for the checks, 200 s of the program's
clock with a battle against three Hard AIs, scrolling and building; user CPU time, two runs each:

| Configuration | User CPU time |
|---|---|
| everything emulated (`--no-native`) | 29.3 s, 29.4 s |
| all replacements | 23.0 s, 23.3 s (−21 %) |
| all but 0x47bdce | 23.7 s, 23.6 s |
| all but 0x47da93 | 24.0 s, 23.3 s |

## Where Unicorn puts the translated code

Before these timings could be trusted, the same deterministic run took either about 23 s or about
31 s (all replacements), or 40 s instead of 29 s (none), from one launch to the next, with nothing
in between. `/usr/bin/time -l` showed the same instructions (580 G) and the same clock speed both
ways, but 95 G cycles against 128 G. `vmmap` showed why: Unicorn's 1 GB buffer of translated code
landed either next to the libraries (fast) or at 0x400000000, 12 GB from Unicorn's own code (slow, 7
launches out of 10), when the gap after the libraries was too small for a whole gigabyte. The buffer
is now 128 MB (`uc_ctl_set_tcg_buffer_size` in `cpu.cpp`): 10 launches out of 10 near and fast. A
120 s battle translates about 8 MB of code.

This is worth more than any single replacement so far. zeus-poseidon-arm64 opens Unicorn the same
way without setting the size, so it may see the same; not measured there.

## Next

In order of share and of how well they are understood: the ground drawing (0x4c6f80, 0x4c9f90),
the fog (0x478217, 0x47924f, 0x47920f), and the simulation functions (0x541cdb, 0x541c65,
0x541264), which need to be understood first. A state log (as in zeus-poseidon-arm64's
`--state-log`) would make the checks of the simulation functions stronger than the frame log alone.
A new profile with the replacements in place will reorder the list.
