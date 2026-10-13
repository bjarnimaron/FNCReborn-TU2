# Building

This fork is built with `setup.bat` (the original project's one-click installer
in `installer/` has not been updated for title update 2 and DLC). It needs
Visual Studio 2022 and runs `scripts/setup.ps1`, which performs the steps below. Each step
records its completion in `build/stamps`, so running setup again continues
where it stopped, and redoes a step only when its inputs changed (for example
after you pull an update).

| Step | Output |
|---|---|
| 1. Find Visual Studio 2022 and load its x64 build environment | – |
| 2. Build `tools/xiso_extract` and extract your `.iso` | `dist/game` |
| 3. Download the ReXGlue SDK at the pinned commit, apply `patches/rexglue-sdk.patch`, build and install it | `build/rexglue-sdk`, `build/sdk` |
| 3b. Build `tools/fnc_content` (reads Xbox 360 packages with the SDK's own reader) | `build/fnc_content` |
| 3c. Take `default.xexp` from your title update 2 package (or a `.zip` with it) and check it (SHA-1) | `dist/game/default.xexp` |
| 4. Recompile `dist/game/default.xex` with title update 2 applied, using `config/fnc_manifest.toml` | `build/generated` |
| 5. Compile the game and copy it with the SDK's DLLs and the default settings | `build/game`, `dist/FNCReborn.exe`, `dist/FNCReborn.toml` |
| 6. First time only: copy your FNC Reborn saves (`Documents\FNCReborn`) | `Documents\FNCReborn-TU2` |
| 7. Install your DLC: Xbox 360 packages (LIVE/PIRS/CON), also inside `.zip` files (and `.rar` / `.7z` with 7-Zip), or folders Xenia installed; every license is marked as owned | `Documents\FNCReborn-TU2\0000000000000000\45410915` |
| 8. Unpack the Legends Pack's commentary (the game streams it from a decompressed copy) | `dist/game/datadlc/dlc_pack1/...` |
| 9. Add the DISPLAY and EXTRAS tabs to the Settings screen, in `fe.big` and in the Legends Pack's own copy of the screen (needs Python 3; skipped with a warning otherwise) | `fe.big`, the pack's `dlc_main.big` and `dlc.toc` (originals kept as `.orig`) |

A log of the last run is written to `build/setup.log`.

## Options

```
setup.bat -Iso "D:\Games\Fight Night Champion.iso"   # skip the file pickers
setup.bat -TitleUpdate "D:\TU_12K228L_000000C000000.00000000000G3"
setup.bat -Dlc "D:\My DLC"                            # install (more) DLC
setup.bat -NoDlc                                     # don't ask for DLC
setup.bat -Clean                                     # redo steps 4 and 5
```

DLC is only asked for when none is installed yet; use `-Dlc` to add more later.

To start completely from scratch, delete the `build` and `dist` folders. Your
saves and DLC are in `Documents\FNCReborn-TU2` and are not touched.

The recompiled code is built for the CPU of the PC that builds it: x86-64-v3
(AVX2, FMA3; Intel Haswell / AMD Zen and newer) when available, x86-64-v2
otherwise. To build for a different PC, pass `-DFNC_CPU_LEVEL=v2` or `v3` to the
game's CMake configure step.

## Troubleshooting

**"Visual Studio 2022 was not found" / "missing components"**
Open the Visual Studio Installer, choose *Modify*, and make sure *Desktop
development with C++* is selected along with the individual components *C++
Clang Compiler for Windows* and *C++ CMake tools for Windows*.

**"default.xex is not the version this port was made for"**
The port's hooks refer to fixed addresses in one specific build of the game.
Other versions will most likely not work.

**"This is not title update 2"**
Only title update 2 (`TU_12K228L_000000C000000.00000000000G3`, `default.xexp`
SHA-1 `F0B4EE71...`) is supported. Title update 1 (`...0000000000083`) is not.

**The Legends boxers, venues or Bare Knuckles Mode are missing**
They come from the Legends Pack (package `E40F674A98DA244CD018C51ACA9815966328CED345`).
Run setup with `-Dlc` and the folder that contains it.

**The SDK download fails**
Check your internet connection and run setup again. If the problem persists,
delete `build/rexglue-sdk` and retry.

**The build runs out of memory**
Close other programs. Compiling the recompiled code needs a lot of RAM; 16 GB
is the practical minimum.

**The DISPLAY and EXTRAS tabs are missing**
Install Python 3 from python.org (not the Microsoft Store placeholder) and run
setup again. With the Legends Pack installed, the game shows the pack's own
copy of the Settings screen, so setup patches that too. To undo the changes,
copy each `.orig` file over the patched one: `fe.big` in
`dist/game/databigs/xenon`, and `dlc_main.big` and `dlc.toc` in the Legends
Pack's folder.

**The game starts but shows a black screen or crashes**
Make sure your GPU driver is up to date and supports Direct3D 12. If the
problem persists, open an issue and include `build/setup.log`,
`dist/FNCReborn.log` and, after a crash, `dist/FNCReborn_crash.txt`.
