# Notices and credits

**Homeworld Remastered Collection** © Gearbox Software; the Mac edition was published by Aspyr Media. This project
contains no part of the game. The build recompiles the Mac executables of a copy the user owns (bought on Steam) and
the app reads the game's data from the user's copy. The recompiled program is for that user only.

This repository started from the recompiler of the Dungeon Siege / Fallout: New Vegas / The Sims / Delta Force /
Rainbow Six ports. No part of those games is included either.

Included:
- **Vulkan-Headers** (Apache-2.0 / MIT, The Khronos Group), **MojoShader** (zlib), **minimp3** (CC0):
  `recomp/third_party`, inherited from the earlier ports.

Used at build time (installed into `recomp/.venv` with pip, not redistributed here):
- **Capstone** (BSD) — disassembly for the recompiler. https://www.capstone-engine.org/
- **Unicorn** (GPL-2.0) — x86 emulation, used only by the tests to compare the recompiled code with the original.
