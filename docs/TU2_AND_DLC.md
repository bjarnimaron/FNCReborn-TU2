# Title update 2 and DLC

How this fork builds Fight Night Champion with title update 2 and makes the DLC
work without EA's servers. [HOW_IT_WORKS.md](HOW_IT_WORKS.md) covers the rest
of FNC Reborn.

## Title update 2

ReXGlue recompiles the PowerPC executable to C++ ahead of time. The SDK applies
a `default.xexp` (an XEX delta patch) placed next to `default.xex`, both when
generating code and at run time, so the TU2 build is the disc executable plus
the title update. `scripts/setup.ps1` takes `default.xexp` from the player's
own title update package with `tools/fnc_content` and checks its SHA-1
(`F0B4EE711583F7D34C00F246EB71838A4BFC2783`).

The Legends Pack needs the title update: the disc version crashes when it
loads the pack.

Code generation needs every function the analysis misses to be listed in
`config/fnc_manifest.toml`. FNC Reborn's list for the disc executable was
carried over to TU2 by matching instructions (branch offsets and addresses
masked), then completed with pointer-table and constant scans and a few
functions found only at run time. A function that is still missing shows up as
`Call to invalid or unregistered function at guest address 0x…` in
`dist/FNCReborn.log`; `dist/FNCReborn_crash.txt` then has the call stack (the
fork also reports such aborts, not only access violations). The fix is to add
the address to `[entrypoint.functions]` and run setup again.

FNC Reborn's hooks (frame timer, DISPLAY tab, player camera) were moved to
their TU2 addresses.

## Installing DLC

The runtime's content manager looks for DLC in
`<user data>\0000000000000000\45410915\00000002\<package name>\` (the package's
files) with a header in
`<user data>\0000000000000000\45410915\Headers\00000002\<package name>.header`:
`XCONTENT_AGGREGATE_DATA` (0x148 bytes, big-endian) followed by a 4-byte
license mask. `tools/fnc_content install` writes both with the SDK's own STFS
reader, the same code the runtime uses to mount packages, and sets the license
mask to `FFFFFFFF`. Setup also accepts folders Xenia installed packages to,
copying their header or building one.

The user data folder is `Documents\FNCReborn-TU2` (`project/src/fnc_app.h`),
apart from the disc version's `Documents\FNCReborn`.

## DLC checks (`project/src/fnc_dlc_unlock.cpp`)

The game only treats DLC as owned after its DLC manager (EA's "DIME") has
confirmed the purchase with EA's servers, which never happens offline. Instead
of faking the servers, the game's own checks are answered. Each hook calls the
original function and only changes a "locked" answer; `fnc_dlc_unlock = false`
turns all of them off.

| Game function | What it is | Hook |
|---|---|---|
| `sub_82D405B0()` | Applies the licenses EA's servers confirmed (none, offline) | Afterwards applies **every** license in `dlc_license_map` with the game's own `sub_82D6BD70`: marks its rows Unlocked and unlocks the boxer, venue or gear each row names (this covers gear such as the Everlast 100th Anniversary set). Licenses for the Legends venues and Bare Knuckles are skipped without the Legends Pack |
| `sub_82D0F410(field, boxer)` | Boxer database getter; field 24 = LOCKED | Locked → 0 (Boxer Gallery and lists) |
| `sub_82DA0C38(boxer)` | IsBoxerUnlocked | → unlocked (Fight Now boxer select) |
| `sub_82D98E08(venue)` | IsVenueUnlocked | Venues 200 and 201 → unlocked |
| `sub_82DAC548(this, feature)` | DLCAdapter::IsFeatureAvailable | Feature 0 (Bare Knuckles) → available |
| `sub_82D66AB0(mode)` | Game mode licensed (`dlc_license_map`) | Mode 2 (Bare Knuckles) → licensed |
| `sub_82D669D0(boost)` | Boost has an *Unlocked* `dlc_license_map` row | Reimplemented without the *Unlocked* condition: every boost sold as DLC is owned; boosts earned in Legacy Mode are unchanged |
| `sub_82D2D3B0`, `sub_82D01F60` | Consumable inventory: has / count | Every XP pack in stock (99) |
| `sub_82DABA70(this, boxer, consumable)` | ApplyConsumable | Reads the pack's amount from `consumable_info`, adds it to the boxer's `ExperiencePoints` (field 199) and `ExperiencePointsPurchased` (204), saves, and sends the `XP_PACK_CONSUMED` success event the boosts screen waits for |

The Legends venues and Bare Knuckles Mode use files from the Legends Pack, so
they are only unlocked while package `E40F674A98DA244CD018C51ACA9815966328CED345`
is installed.

XP packs were consumables: their count came from EA's servers, and using one
asked the servers to take it away before the XP was added. Offline, both steps
fail, so the hook does what the game does after a successful purchase.

## Skill level cap (`project/src/fnc_skill_cap.cpp`)

*LEVELS AVAILABLE x/220* is the tuning value `XpMaxSlots`: how many skill
levels a boxer may buy in total (17 skills × 20 levels = 340 exist). It is read
by exactly two functions: `sub_82E01590` (the cap) and `sub_82E033A0` (cap
minus levels bought, used by the screen and when buying a level). Both return
`fnc_skill_level_cap` instead, never less than the game's 220.

## Legends Pack commentary

The pack's commentary bank (`speech_eng_dlc_samples.sbr` / `.sbs`) is stored
in its `dlc_main.big`, compressed. The game streams commentary from a loose,
decompressed copy at `D:\datadlc\dlc_pack1\dlc\data\xenon\loc\eng\speech\`
(`dist/game/datadlc/...`); without it, the audio thread crashes in the middle
of a fight with a Legends boxer whose lines need it (for example Young George
Foreman). Setup extracts and decompresses both files with `fnc_content`.

EA's `chunklzx` format (big-endian): `"chunklzx"`, u32 version, u32 total size,
u32 chunk size (0x40000), u32 chunk count (at 0x14), u32 alignment (at 0x18),
padding to 0x28. Then per chunk: u32 compressed size, u32 type (3 = LZX), data.
Each chunk's data starts on an alignment boundary, with its 8-byte header just
before it. The data is xcompress-framed LZX (`[0xFF, u16 output size]`
optional, `u16 input size`, payload), which the SDK's `lzx_decompress`
decodes.

## Settings tabs

`tools/display_tab/aptpatch_display.py` adds the DISPLAY (category 200) and
EXTRAS (210) tabs to the Settings screen's Apt movie, `gamesettingdata.big`;
`project/src/fnc_settings_menu.cpp` serves their values from the runtime
settings and saves changes to `FNCReborn.toml`. The row lists in both files
must match.

The Legends Pack replaces the Settings screen with its own copy inside its
`dlc_main.big`, so with the pack installed that copy is the one the game shows,
and setup patches it as well. The pack's `dlc.toc` records the archive's size,
which the patcher updates (`--toc`). Originals are kept as `.orig`.

## Instant replays

On a console the game mounts the hard drive's cache partition as `cache:` (a
"utility drive") and keeps working files there, including its replay data. The
runtime has no such drive (its `Cache0` / `Cache1` partitions are placeholders
without storage), so the game found its cache unavailable and skipped knockdown
replays and round highlights. `fnc_cache::Mount` (`project/src/fnc_hooks.cpp`,
called from `FncApp::OnPreLaunchModule`) mounts `Documents\FNCReborn-TU2\cache_drive`
as `cache:` before the game starts, like Xenia's `mount_cache`.
`fnc_cache_drive = false` turns it off.

## Not covered

- The Fight Night Store screen: it needs EA's servers, and its catalog came from
  Xbox Live.
- Online modes.
- The *"not connected to EA Servers"* warning when entering Legacy Mode.
