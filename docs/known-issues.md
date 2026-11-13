# Known issues, fixes and workarounds

A running log, kept up to date with every change: what we found and fixed, and every shortcut
("workaround") that still has to be done properly. Before a release, the **Open** and
**Workarounds** sections become the release's list of known limitations.

Each entry: what the game does, what the host does about it, and the evidence (an address in
`dmcr.exe`, GOG build `b4eea674…`, or a file). Statuses: **fixed** (done properly), **workaround**
(works, but not the real behaviour; revisit), **open** (not done).

## Open

| Issue | Notes |
|---|---|
| No intro or briefing videos | MCI `AVIVideo` is refused (`winmm.cpp`). The videos are Cinepak 640×360/480 with PCM or MS-ADPCM sound; FFmpeg decodes both. The engine opens them at 0x4c68fc (`MCI_OPEN` with `MCI_OPEN_TYPE|MCI_OPEN_ELEMENT`), then `MCI_WINDOW` 0x841, `MCI_PUT` 0x842, `MCI_PLAY` with notify. |
| No multiplayer | Winsock 2 refuses to start (`WSAStartup` → `WSASYSNOTREADY`), `CoCreateInstance(CLSID_DirectPlay)` → `REGDB_E_CLASSNOTREG`, `DirectPlayLobbyCreateA` → `E_FAIL`, the 10 functions of GSC's `iChat.dll` do nothing (`system.cpp`). |
| No missions or campaigns | The 125 mission DLLs (`Missions`, `History_battl`, `UserMissions`) are not placed in memory yet (`cossacks_run/main.cpp` preloads only `AI\*.dll` and `unrar.dll`). They import 240 functions from `dmcr.exe`; the loader supports that, untested. |
| In-game help does not open | The engine starts `HView.exe` with `WinExec` (0x4a08a6): refused, the host starts no other program. |
| Only one build of the game tested | GOG *Back to War* 2.1.0.18, `dmcr.exe` SHA-256 `b4eea674…`. *European Wars* and *The Art of War* installers not analysed. |

## Workarounds (revisit before calling them done)

| Area | What the host does | Why it is a workaround | File |
|---|---|---|---|
| Screen refresh | The host presents the primary surface itself about every 16 ms, from `PeekMessageA`, `GetMessageA` and `GetTickCount`. | The engine locks the primary surface twice in a whole session and then writes into it directly (`ScreenPtr`); on Windows that memory is the screen, and GSC's `mdraw.dll` presents it from a thread of its own. Presenting from the game's own calls is not tied to its frames: a frame can show half drawn, and an unchanged screen is converted again. A hook at the end of the engine's frame (`FlipPages`, export) would be exact. | `ddraw.cpp`, `process.h` |
| Busy clock | After 64 `GetTickCount` calls with no other system call in between, each further call sleeps 1 ms. | The engine waits for its next frame by reading the clock in a loop (up to 6 million reads in 20 s at the menus). The sleep is not in the original; it may round frame times up by up to 1 ms. | `kernel32.cpp` |
| Idle message loop | An empty `PeekMessageA` returns at most every 4 ms. | Chosen without measuring the engine's pacing; marked `TODO(cossacks)`. | `user32.cpp` |
| Missing CD tracks | A CD track with no `Tracks\Track_NN.ogg` (Back to War's new nations ask for 20 and later; GOG ships 2–19) plays an existing track instead. | GOG's own `minmm.dll` plays nothing then. An improvement on GOG, but not the original behaviour; the choice of replacement is arbitrary (`(N − 2) mod count`). | `winmm.cpp` |
| MCI notifications | Only `MCI_NOTIFY_SUCCESSFUL` at the natural end of a track; a stop or a new `PLAY` drops the pending notification silently. | Windows sends `MCI_NOTIFY_ABORTED` / `SUPERSEDED`. The engine's handler (0x574720) only acts on `SUCCESSFUL`, so this is enough for it. `MCI_STATUS` is not written. | `winmm.cpp` |
| DirectSound buffers | A buffer is turned into a sound at `Play`, cached until the next `Unlock`; `GetCurrentPosition` is estimated from the mixer; no 3D sound (`QueryInterface` → `E_NOINTERFACE`). | A buffer written while it plays (streaming) would not be heard changing. The engine's effects are static buffers, so it works for them. | `dsound.cpp` |
| W A S D | W A S D are the arrows while the engine polls the arrows to scroll the map (`GetKeyState(VK_RIGHT)` at 0x4ed81b, run every frame on the battle map and skipped while a text is typed); with Ctrl or Alt held they stay letters. The code bytes there are checked first. | A sign taken from the engine's timing (polled less than 250 ms ago) rather than a state variable; the flags it tests before polling (0x5e4df0, 0x8fbb55, 0x1026234) are not named yet. W A S D shortcuts of the engine, if any, are lost on the map. `COSSACKS_KEYLOG=1` logs every key. | `desktop.cpp`, `cossacks_run/main.cpp` |
| Wide screen | `EnumDisplayModes` offers, besides the usual 4:3 modes, modes in the proportions of the Mac's screen (heights 720–1080, widths a multiple of 32). | Not checked yet that every screen of the engine (menus, minimap, briefing) lays out well at those sizes. | `ddraw.cpp` |
| Keyboard to text | `ToAscii` knows a US layout only. | Other layouts give wrong characters for signs. | `user32.cpp` |
| File times | Local time is UTC (`FileTimeToLocalFileTime` and the reverse are the identity). | Saved-game times may show off by the time zone. | `system.cpp` |
| Registry | Kept in `registry.txt` in the write folder. | Only the five functions the engine uses; no enumeration, no deletion. | `system.cpp` |
| Files opened for writing | A file of the installation opened with `GENERIC_WRITE` is first copied into the write folder (VFS rule), even if the engine never writes it. | The engine opens every clan picture (`Clans\*\*.bmp`) read-write six times: about 200 copies on the first run. Harmless, but a lazy copy (at the first write) would be cleaner. | `kernel32_files.cpp`, `vfs.cpp` |
| File mappings | `MapViewOfFile` places the file itself in the emulated memory (`mmap`, copy-on-write) from 0x40000000 up. | Views never reuse their addresses (2 GB in all); only `PAGE_READONLY` and `PAGE_WRITECOPY`; a mapping larger than its file is refused. | `kernel32_files.cpp`, `cpu.cpp` |
| DLL loading | Every DLL the engine may load is placed in memory before the start; `LoadLibraryA` restores its bytes as loaded and calls `DllMain`. | Memory the DLL's C runtime allocated is not reclaimed at `FreeLibrary` if the DLL does not free it. The DLL area is 256 MB (0x20000000–0x30000000). | `modules.cpp`, `process.cpp` |
| C runtime of `AI\Denmark.dll` | `msvcrt.dll`: only `malloc`, `free`, `sprintf`, `_initterm`, `_adjust_fdiv` (0). | Enough for this one module. | `system.cpp` |
| Mixer | One device: the speakers (destination) with two sources, CD (the music's volume, 0x1005) and wave (0x1008), each with one volume control. Only the CD volume acts (on the music's voice); the speakers' and the wave's are kept and returned, without effect. | The engine's Options screen sets the music volume through the CD line about every frame (0x576101). Whether it also uses the wave line for "Sound Volume" is not checked. | `winmm.cpp` |
| Exceptions | `RaiseException` stops the program with its code; `RtlUnwind` is not written. | C++ exceptions and SEH of the engine are not supported; none seen so far. | `system.cpp` |
| Locale | `GetLocaleInfoA` answers only the code pages; `GetLocaleInfoW` nothing. | Enough for the C runtime's start. | `system.cpp` |
| Heap | One 256 MB heap (0x10000000), bump allocation with exact-size free lists; `HeapCreate` heaps are the same heap. | Fragmentation over a long session is not measured. Peak use so far: 171 MB resident for the whole process. | `process.cpp`, `kernel32_memory.cpp` |

## Fixed

| Date | Problem | Fix |
|---|---|---|
| 2026-11-13 | The engine imports `DirectDrawCreate` from `MDRAW.DLL` (GSC's DirectDraw wrapper) and uses an 8-bit palettized mode (`SetDisplayMode(w, h, 8)` at 0x460074). | `ddraw.cpp`: `MDRAW.DLL` is an alias of DDRAW; surfaces of 8, 16 or 32 bits; palettes (`CreatePalette`, `SetEntries`, `GetEntries`, `SetPalette`); `EnumDisplayModes`. `Desktop` presents 8-bit (through the palette) and 32-bit pictures. |
| 2026-11-13 | AI players are DLLs (`AI\*.dll`) loaded with `LoadLibraryA`, all preferring base 0x10000000 (the host's heap); `unrar.dll` prefers 0x00400000 (the executable). | `pe_image.cpp`: base relocations and export tables; `modules.cpp`: DLLs placed in their own area, `LoadLibraryA`, `FreeLibrary`, `GetProcAddress` (also for system DLLs: host functions), `GetModuleHandleA`, `GetModuleFileNameA`, `DllMain`. Imports from `DMCR.EXE` bind to its exports; imported variables (`def_data`). |
| 2026-11-13 | The engine maps its archives (`ALL.GSC`, 243 MB; `PATCH01.GS1`) whole with `CreateFileMappingA` + `MapViewOfFile` (`CGSCarch::Open`, 0x402804). | `Cpu::map_file`: the file is `mmap`ed copy-on-write straight into the emulated memory: no copy, read on demand. |
| 2026-11-13 | The random-map screen stayed frozen with empty player rows. | Same cause as the screen refresh workaround above: the engine draws straight into the primary surface. |
| 2026-11-13 | Up to 6 million `GetTickCount` calls in 20 s at the menus. | Busy-clock workaround above: down to about 40 000. |
| 2026-11-13 | No sound effects. | `dsound.cpp`: `IDirectSound`, `IDirectSoundBuffer` (with `DuplicateSoundBuffer`) over the host's mixer. |
| 2026-11-13 | No music. | `winmm.cpp`: MCI `CDAudio` plays `Tracks\Track_NN.ogg` through FFmpeg and posts `MM_MCINOTIFY` at the end of a track, as GOG's `minmm.dll` does (it plays only with `MCI_TO`, like it). |
| 2026-11-13 | The windmill's dome (and every white highlight of the sprites) showed as black blobs: sprite pixels of index 255 were drawn with the colour the engine itself writes at 255, a dark one. | `ddraw.cpp`: the engine creates its palette without `DDPCAPS_ALLOW256` (flags 0x4, at 0x460cfd); in exclusive mode Windows then keeps entries 0 and 255 black and white whatever the program writes, and the sprites rely on it. Found with `--watch-pixel` (writes to one pixel of the drawing buffer), the LZ decoder at 0x485070 checked byte for byte against a decode of `ALL.GSC` in Python, and the frame rendered from the archive. |
| 2026-11-13 | W A S D did not scroll on a real keyboard: the first sign of the battle map, `?GameInProgress@@3_NA`, stays 0 in a battle against the computer (the key log showed it for every key). | The engine's own arrow polling at 0x4ed81b is the sign now; checked in deterministic runs (W moves the view up, D right). |
| 2026-11-13 | The music volume slider had no effect. | `winmm.cpp`: an emulated mixer; the engine finds the CD line among the sources of the speakers (0x575c55: it needs `cConnections` and `MIXER_GETLINEINFOF_SOURCE`) and sets its volume, applied to the music's voice. |
| 2026-11-13 | No W A S D scrolling; no wide modes; the window could be larger than the screen (1280 × 1024 on a laptop). | W A S D on the battle map; wide modes in the options; the window is fitted to the screen and resizable. |
| 2026-11-13 | Missing system functions at start: `GetKeyState`, `GetKeyboardState`, `ToAscii`, `SetTimer`, `wsprintfA`, `CreateFileMappingA`, registry, TLS, critical sections, `Global*`, file-time conversions, … | Written (`user32.cpp`, `system.cpp`, `kernel32_files.cpp`). |
