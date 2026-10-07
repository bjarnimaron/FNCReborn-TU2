# How it works

## Static recompilation

The Xbox 360 runs PowerPC code. Instead of emulating that CPU at runtime, the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) translates every function
in the game's executable (`default.xex`) to C++ ahead of time. The result is
compiled with Clang into a normal x86-64 Windows program. The SDK also provides
the rest of the console: the kernel and system libraries, the Xenos GPU
(translated to Direct3D 12), XMA audio and controller input.

Fight Night Champion needs some game-specific help on top of that. This
document lists what was changed and why.

## Recompiler configuration

`config/fnc_manifest.toml` lists 715 functions the recompiler's static
analysis does not find on its own: targets of unresolved branches, and small
functions (C++ thunks, virtual-call forwarders, setters) that are only reached
through vtables or function tables. Twelve of them are only reached through
code pointers the game stores at run time; they were found by scanning the
executable for `lis`/`addi` pairs that build the address of a function start.
One of them is called when a fighter is created. Without these entries, a call
to such a function stops the game.

## Recompiler fixes

In `patches/rexglue-sdk.patch`, `src/codegen/builders/vector.cpp`:

- **Fused multiply-add.** The console's vector `vmaddfp` / `vnmsubfp` round
  once. Translated as a separate multiply and add, they round twice, and some of
  the game's animation blending then produced exact zeros where the console
  keeps a tiny value, and from those 0/0 NaNs that made fighters vanish.
- **`vexptefp` range.** 2^x is built from the exponent bits; very small or very
  large x produced garbage instead of 0 or infinity, so decay terms blew up late
  in a fight.
- **`vpkuhus` / `vpkuwus`.** The old code wrote the destination register while
  still reading the sources, which broke when the destination is also a source
  (as it always is in this game): green blocks in videos.

## Kernel and system

- **64-bit arguments.** Kernel functions received only the low 32 bits of
  64-bit arguments. The profile ID passed to the save enumeration lost its top
  half, no saves were found and the game made a new empty profile on every
  start.
- **Console signatures.** `XeKeysConsolePrivateKeySign` and
  `XeKeysConsoleSignatureVerification` return success, as on a console; the
  game checks both when it reads and writes save data.
- **Sign-in notifications** are sent to every system listener, not only the
  first. The game's save system has its own listener and only loads the profile
  after it sees the sign-in.
- **System-heap pool.** Every kernel object costs two guest pages from the host
  operating system. The game's job system creates an event per job, so the game
  thread spent about a third of its time in those calls. Freed pages are now
  kept and reused (`fnc_sysheap_pool`).
- **Waiting without spinning.** Several wait loops (thread delays, GPU waits)
  yielded in a tight loop and kept CPU cores busy; they now back off to short,
  high-resolution sleeps. The timer queue blocks instead of spinning.
- **USB camera module.** The Xbox Live Vision functions the game imports are
  built in.

## GPU

- **Command buffers.** Nested command buffers were read through a ring buffer
  exactly as large as their data, which made a full buffer look empty: none of
  its commands ran. Unknown command packets are now skipped instead of
  discarding the rest of the buffer, and the GPU thread is woken up when work
  arrives instead of waiting for its next poll.
- **EDRAM ranges.** Knockdowns, replays and celebrations went black: a
  depth-only pass's estimated EDRAM range wrapped past the end of the EDRAM and
  took ownership of the final image. Ranges are now clamped to the end
  (`fnc_rt_no_edram_wrap`).
- **Readback.** The game builds some textures on the GPU and reads them on the
  CPU: fighter clothing and the skin of created fighters, layer by layer through
  one scratch buffer. The new `smart` readback mode copies such results back
  immediately, so every layer gets its own result. Buffers the game resolves the
  same number of times every frame are only read by the GPU, so they are not
  copied at all (`readback_resolve_smart_skip_steady`); that saves about a
  quarter of the GPU thread's time in a fight.

## Input

- **Controller drop-outs.** A pad that disconnects for a moment and comes back
  is hidden from the game for up to 3 seconds. The game used to pause the fight
  behind a reconnect prompt and could not recover.
- **Back button.** Presses are counted for the camera switch.

## Game code

`project/src`:

- **Frame rate** (`fnc_hooks.cpp`). The game builds a 30 Hz frame timer; the
  hook gives it the rate from `fnc_frame_rate` instead.
- **Camera behind your boxer** (`player_camera.cpp`). After the game's camera
  director finishes the camera for an update, the hook writes a camera behind
  your boxer's head, looking at the opponent. Before the next update the game's
  own values are put back, so the game's camera logic is not affected. The boxer
  positions come from the head bones the game's camera system tracks.
- **DISPLAY tab** (`fnc_settings_menu.cpp`, `tools/display_tab`). Setup adds a
  category with five rows to the Settings screen in your copy of `fe.big`; the
  game's settings manager asks the exe for their values, which come from and
  are saved to `FNCReborn.toml`.
- **Overlays** (`fnc_fps_overlay.cpp`). F1 frame rate counter and F11
  fullscreen. The SDK's developer overlays are off unless `dev_overlays` is set.
- **Crash reports** (`fnc_sampler.cpp`). A crash writes `FNCReborn_crash.txt`
  with the game function it happened in. A file named `profile_on` next to the
  exe also starts a sampling profiler that logs where each game thread spends
  its time.
