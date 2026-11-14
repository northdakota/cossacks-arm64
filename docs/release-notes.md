# Release notes (draft)

What every release's notes must tell players, collected as we go. The known limitations come from
`known-issues.md` (its Open and Workarounds sections).

## Recommended settings

- **Game Mode: Slow, Game Speed near the maximum** (Options screen). The engine's Game Speed slider
  is a frame limiter, not a speed control of its own: a slower setting means fewer frames a second,
  and choppier scrolling. Game Mode sets how far units move in one frame: Fast mode moves them twice
  as far, so the same pace of play comes with half the frames. It was made for the slow computers of
  2001, which could not draw many frames. On a Mac, Slow mode with the slider near the maximum plays
  at a comfortable pace and scrolls smoothly.
- The settings are kept in the write folder: `mode.dat` (resolution, sound volume, game speed and
  mode, written by the game) and `mixer.txt` (music volume, written by the launcher, since the game
  reads it from the system's mixer).

## When the game stops

If the game stops on something the launcher cannot do yet, a window says so, and a report is saved
in the folder `Crash reports` of the write folder (Copy report puts it on the clipboard). Players
should send it with a few words on what they were doing; it holds no paths or user names.
