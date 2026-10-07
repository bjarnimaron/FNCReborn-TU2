# Contributing

Thanks for helping out. A few ground rules:

- **Never share game files.** Do not attach, upload or link disc images, game
  files, recompiled code (`build/generated`), built executables or memory
  dumps of the game. Issues and pull requests containing them will be removed.
- **Code in this repository must be original.** Hooks may refer to game
  functions by address, but must not contain copied or recompiled game code.

## Reporting bugs

Include where in the game it happens, what you expected, and your CPU, GPU and
driver version. Attach `dist/FNCReborn.log` and, after a crash,
`dist/FNCReborn_crash.txt`. A screenshot you took yourself is fine.

## Making changes

- Game-specific behaviour goes in `project/src` (function overrides) or
  `config/fnc_manifest.toml` (recompiler configuration).
- Changes to the SDK go in `patches/rexglue-sdk.patch`. To update it, edit the
  SDK checkout in `build/rexglue-sdk` and run
  `git -C build/rexglue-sdk diff > patches/rexglue-sdk.patch`, then rebuild with
  `del build\stamps\sdk` followed by `setup.bat`.
- After changing the manifest, run `setup.bat -Clean` so the code is
  regenerated.
- Keep comments short and factual: what a hook does and why it is needed.
