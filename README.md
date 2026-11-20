# Homeworld Remastered Collection — native Apple Silicon port

**Early, and playable into the campaign.** This is the Steam *Mac* edition of Homeworld Remastered (Aspyr, 2.0.1)
running natively on Apple Silicon. That edition is a 32-bit Intel app; macOS dropped 32-bit apps in 10.15, and
Rosetta 2 does not run them, so the Steam copy no longer starts on a current Mac.

The port statically recompiles the game's own i386 Mach-O executable (`HW2HD_Exe`) into C, compiles it for arm64 and
runs it on a native runtime: the game's OpenGL rendering goes to **Metal**, its SDL2 platform layer to native code,
audio to CoreAudio. No Wine, no Rosetta, no emulator at run time. Nothing from the game is in this repository: the
build regenerates everything from your own Steam files.

## Install

```sh
./install.sh --nightly        # builds from your Steam copy, installs ~/Applications/Homeworld Remastered Nightly.app
```

Needs the Steam Mac edition of Homeworld Remastered Collection downloaded (Steam keeps it in
`~/Library/Application Support/Steam/steamapps/common/Homeworld`), the Xcode command line tools, python3 and
`brew install sdl2-compat glslang spirv-cross`. The game's files are imported into `~/Games/HomeworldNative/game-files` (an APFS clone);
profiles, saves, settings and the logs (the game's `HwRM.log`, the port's `hw2hd.log` and `hw2hd.old.log`) go to `~/Games/HomeworldNative/userdata`.

The app opens a small launcher: Homeworld Remastered (the HW1 campaign) or Homeworld 2 Remastered (the HW2
campaign), full screen or a window, an option to skip all movies, and Graphics Options: Notch (Below It / Around It),
Resolution (Standard / Retina), VSync (On / Off) and Renderer (Metal / Apple GL). It also has Saves and Updates tiles.

## Status

Playable into the campaign. Rendering goes through the port's own Metal renderer (the game's OpenGL 4.1 calls and
GLSL shaders translated to Metal); in the first mission it holds 120 fps (vsync) on an M1 Pro with about 1.3 ms of GPU
time per frame and no CPU-GPU stalls. Apple's OpenGL stays available in the launcher as a fallback. Known issues: the second intro movie shows a gray screen while it plays (press Escape to skip it);
multiplayer (Steam lobbies) is not connected yet; achievements are offline.

Status and plan: [docs/PLAN.md](docs/PLAN.md).
