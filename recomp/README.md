# Recompiler: Homeworld Remastered as native code

Static recompilation of your own Steam Mac edition (Aspyr 2.0.1): the 32-bit Intel Mach-O `HW2HD_Exe` becomes C, compiled
for arm64. Nothing derived from the game is committed; `build_hw.sh` regenerates everything locally. Installing and playing:
[README](../README.md). Status and plan: [docs/PLAN.md](../docs/PLAN.md).

```sh
recomp/build_hw.sh "/path/to/HomeworldRemastered.app"      # -> recomp/work/hw2hd
```

## The pipeline

`build_hw.sh` runs analysis (`tools/analyze.py`), lifting to C (`tools/lift.py`), compilation (`cc1.sh`), then
`tools/build_hw_runtime.sh`, which links the runtime.

| Where | What |
|---|---|
| `runtime/rt.c` | the execution core shared with the lifted code |
| `runtime/mac/` | the native runtime: Mach-O loader, libSystem, CoreFoundation, SDL, Steam stubs, Miles audio |
| `runtime/mac/mgl/` | the game's OpenGL 4.1 calls and GLSL shaders on Metal |
| `host/hw_launcher.m` | the launcher window (game, display, Graphics Options, saves, updates) |

## Switches

Set in the environment of the `hw2hd` binary.

| Variable | Effect |
|---|---|
| `HW_DATA` | the user data folder (the launcher passes `<data>/userdata`) |
| `HW_FULLSCREEN`, `HW_NOTCH`, `HW_RETINA`, `HW_VSYNC`, `HW_GL` | the launcher's display choices; `HW_GL` selects Apple GL instead of Metal |
| `HW_TEST` | test mode |
| `HW_SHOT`, `HW_SCRIPT`, `HW_QUIT_MS` | take a screenshot, drive input from a script, quit after N ms |
| `HW_TRACE`, `HW_STUB` | trace calls; report stubbed calls |
| `HW_LOG_FPS`, `HW_LOG_GL`, `HW_LOG_FILES` and other `HW_LOG_*` | subsystem logs |
| `HW_MGL_DUMP=<dir>` | write the Metal source of pipelines that fail to build |

## Not part of this build

`host/launcher.m`, `host/r6_launcher.m`, `host/linux.c`, `host/linux_launcher/` and `runtime/win32/` came with the sister ports
this repository was started from. Nothing here compiles them (`build_hw.sh`, `tools/build_hw_runtime.sh` and `install.sh` use
`host/hw_launcher.m`, `runtime/rt.c` and `runtime/mac/**`). They are kept for reference only.
