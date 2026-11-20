# Plan and status

Source: Steam app 244160, Mac depot (Aspyr 2.0.1). Executables in `HomeworldRemastered.app/Contents/MacOS`:

| File | What | Port order |
|---|---|---|
| `HW2HD_Exe` | Remastered engine: HW1 + HW2 Remastered campaigns, skirmish, multiplayer | **first** |
| `HW1C_Exe` | Homeworld 1 Classic (uses Bink) | later, same runtime |
| `HW2C_Exe` | Homeworld 2 Classic | later, same runtime |
| `AppBundleExe` | Aspyr's launcher | replaced by our launcher, not ported |

All are i386 Mach-O, not position independent (image at 0x1000), with symbols (26,028 named functions in
`HW2HD_Exe`). Platform code is a statically linked SDL2 (all Objective-C/AppKit/IOKit/Carbon use lives there) plus
Aspyr's "ASL" Win32 layer (`HWND_Mac`, `IDirectSound_Mac` on Miles, `CreateFile_core`, WinHttp on CFNetwork). The
engine itself calls OpenGL directly (172 call sites).

## Model

- `tools/macho.py`: Mach-O reader with the PE reader's interface. Imports = the symbol-pointer slots (823 after
  weak definitions the executable wins are resolved locally; their 3,754 stubs become direct jumps); data binds
  (1,657: C++ typeinfo vtables, CFString class) and fixups (5,342) go to `work/<tag>/macho_tab.c` for the loader.
- `tools/pic.py`: Apple i386 position-independent patterns: `call next; pop reg` (a program-counter read, lifted as a
  push) and relative jump tables (`mov r,[base+i*4+d]; add r,base; jmp r`).
- Runtime (to do): Mach-O loader (segments, fixups, binds, TLV, static constructors before `main`), then import
  implementations in guest ABI (cdecl, 16-byte stack alignment, structs returned through a hidden pointer the callee
  pops):
  - libSystem (262): C library, pthreads, files, time — on the host.
  - libc++ (151 + 1,555 binds): 32-bit libc++ layouts (std::string, streams, locale, mutex/future), Itanium C++
    exceptions (61 throw sites): written for the guest ABI.
  - SDL2 (247 functions in the image, not imports): replaced at the function level by host SDL/native windowing, so
    none of the Cocoa/IOKit code behind them runs.
  - Aspyr Win32 layer: file, timing, sound (Miles imports, 25) mapped to the host.
  - OpenGL (52): **Metal backend** (GL state tracker → Metal), Apple's OpenGL only as a comparison reference.
  - CoreFoundation/CoreServices (151), Steam API (17: lobby, P2P, achievements): CF on the host; Steam stubbed
    first (offline), real Steam later.

## Done so far

- 2026-11-03: Mach-O front end, PIC patterns; `analyze.py` 33,133 functions (93.8% of the code span: the rest is
  jump tables and padding inside `__text`), 782 jump tables (93 unresolved), `lift.py` lifts everything with no
  internal errors. Unsupported instructions remaining: SSSE3/SSE4/AES/SHA (CPU-dispatched, the runtime's CPUID
  reports SSE3), plus SSE3/MMX ones the game can reach (fisttp ×220, movshdup/movsldup, haddpd, pshufw, paddsb):
  next.
- 2026-11-04 (night): the runtime (`recomp/runtime/mac`) runs the game to its main window: static constructors,
  `main`, offline Steam, SDL on the host SDL2, Apple's OpenGL 4.1 as the reference backend, the Miles mixer on
  CoreAudio, Aspyr's file layer (File Manager forks, POSIX calls fetched with `dlsym`), the data path and fonts.
  Build: `recomp/build_hw.sh <HomeworldRemastered.app>`; run from `recomp/work/hw2hd`:
  `HW_TEST=1 ./hw2hd <app> -dlccampaign HW1Campaign.big -campaign HomeworldClassic -moviepath DataHW1Campaign`
  (HW2: `HW2Campaign.big` / `Ascension` / `DataHW2Campaign`; these are the arguments Aspyr's launcher passes).
  C++ exceptions: `runtime/mac/unwind.c` (two-phase unwinder over the compact unwind table and the LSDAs; the
  lifter arms each function's landing pads at entry).

## Metal backend (done, default)

`runtime/mac/mgl` (mgl.m + shader.cpp) implements the game's OpenGL 4.1 core subset on Metal; `HW_GL=apple`
(launcher checkbox) falls back to Apple's OpenGL. Design:
- Shaders: GLSL → SPIR-V (glslang, relaxed rules; loose uniforms become one block at set 0 binding 0) → MSL
  (SPIRV-Cross 2.3, Y flipped, clip-space depth fixed up). At link time the fragment stage is translated again with
  its inputs placed at the vertex outputs' locations by name (GL matches varyings by name). All 284 shaders work.
- GL row order kept everywhere; the default framebuffer is an offscreen BGRA8 + Depth32F/S8 pair, presented with a
  flipped quad. GL viewport/scissor numbers apply to Metal unchanged; winding inverted for the flip.
- Buffers: up to 6 MTLBuffer versions per GL buffer (orphaning without stalls), map/unmap on shared memory.
- Textures: BC1-3 (+sRGB), RGB expanded to RGBA, swizzle via texture views, samplers cached by state, PBO uploads.
- Render passes open lazily per framebuffer; glClear becomes the pass load action. 3 frames in flight.
- Pipelines cached by program + attachment formats + blend + vertex layout; disabled attributes read a constant.
- Limits/extensions/renderer ID from a hidden Apple CGL context (Aspyr's renderer check).
- `HW_VSYNC=0` uncaps; `HW_LOG_FPS` also reports time per frame waiting for the display; `HW_MGL_DUMP=<dir>` writes
  the MSL of pipelines that fail.

Measured (M1 Pro, 1728x1080, HW1 mission 1, `HW_LOG_FPS`): Metal 120 fps vsync-locked, GPU ~1.3 ms per frame
(command buffer GPU start/end), 0 CPU waits for the GPU in play (waits only while loading), ~5 ms of each 8.3 ms
frame blocked on the next drawable. Uncapped wall-clock throughput not measured yet (see below); Apple GL runs
220 fps uncapped. Mission frame at 150 s matches Apple GL by eye (scratch comparison).

## Known issues / next
- Display (launcher): Window / Full Screen (macOS full screen, a Space: Game Mode verified on via gamepolicyd);
  Notch: Below It / Around It (HW_NOTCH); Resolution: Standard (points) / Retina (HW_RETINA: SDL ALLOW_HIGHDPI; the
  game renders the full pixel drawable and takes the mouse in points; its HUD is pixel-sized, so smaller). Retina
  HW1 mission 1 windowed (3456x2160): ~3.8 ms GPU per frame, 120 fps.
- Uncapped Metal still shows 120 fps: `displaySyncEnabled = NO` is set on the layer, but frame pacing appears to
  stay display-bound (check `maximumDrawableCount` / presentation in full screen).
- Not implemented in mgl (not seen in play so far): renderbuffers, depth blits, partial (scissored/masked) clears,
  client-side vertex arrays, MRT beyond attachment 0. Missing entry points log "mgl: X is not implemented".
- Loading is slowish; multiplayer/Steam lobbies are offline stubs; the Classic exes (HW1C needs Bink, HW2C) untouched.
