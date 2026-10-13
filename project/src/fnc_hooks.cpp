// Game-specific function overrides for Fight Night Champion (default.xex v0.0.0.3 + title update 2 (v0.0.2.3),
// media 2DC50868). Addresses are only valid for that executable.
//
// Recompiled functions are weak symbols; defining one here replaces it, and
// __imp__<name> is always the original.

#include <cstring>
#include <filesystem>
#include <memory>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_DOUBLE(fnc_frame_rate, 60.0, "Game",
                      "Frame rate cap. The retail game runs at 30. Set 30 to restore it.");

// ---------------------------------------------------------------------------
// 60 FPS.
// sub_827C9378 builds one of the game's fixed-rate frame timers; f1 = rate in Hz,
// which a helper turns into a tick period (timebase / rate). The game makes
// exactly two, each once:
//   sub_822A9540 -> 30 Hz (constant 0x82008574), returns to 0x822A9564  <- changed
//   sub_822AA530 -> 60 Hz, returns to 0x822AA558  <- untouched
// The community "Unlock FPS" patch for TU2 (illusion, xenia-canary/game-patches)
// raises the same 30.0 constant (it sits at 0x82008574 in TU2).
// ---------------------------------------------------------------------------
// Debug menu (community "Debug Menu" patch for TU2, Adrian, xenia-canary/game-patches): the
// UI's native method "IsFinal" at 0x82218884 is renamed "IsDebug", which turns on the
// developer menu (hold left on the left stick and click it). Applied once at startup, from
// the frame timer hook below.
REXCVAR_DEFINE_BOOL(fnc_debug_menu, false, "Game",
                    "Enable the game's developer menu (hold left on the left stick and click it)");

extern "C" void __imp__sub_827C9378(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_827C9378(PPCContext& __restrict ctx, uint8_t* base) {
  if (ctx.lr == 0x822A9564 && REXCVAR_GET(fnc_debug_menu)) {
    char* name = reinterpret_cast<char*>(base + 0x82218884);
    if (std::memcmp(name, "IsFinal", 8) == 0) {
      // The string sits in read-only image data.
      DWORD old_protect;
      if (VirtualProtect(name, 8, PAGE_READWRITE, &old_protect)) {
        std::memcpy(name, "IsDebug", 8);
        VirtualProtect(name, 8, old_protect, &old_protect);
        REXLOG_INFO("[FNC-DEBUG] developer menu enabled (fnc_debug_menu)");
      } else {
        REXLOG_WARN("[FNC-DEBUG] could not make the IsFinal string writable");
      }
    }
  }
  if (ctx.lr == 0x822A9564 && ctx.f1.f64 == 30.0) {
    double rate = REXCVAR_GET(fnc_frame_rate);
    if (rate >= 10.0 && rate <= 240.0 && rate != 30.0) {
      REXLOG_INFO("[FNC-60FPS] frame timer 30 Hz -> {:.0f} Hz (fnc_frame_rate)", rate);
      ctx.f1.f64 = rate;
    } else {
      REXLOG_INFO("[FNC-60FPS] frame timer left at 30 Hz (fnc_frame_rate = {})", rate);
    }
  }
  __imp__sub_827C9378(ctx, base);
}

// ---------------------------------------------------------------------------
// cache: drive.
// On a console the game mounts the hard drive's cache partition as cache: (a "utility drive")
// and keeps working files there, including its replay data. The runtime has no such drive (its
// Cache0/Cache1 partitions are placeholders), so the game's cache stayed unavailable and it
// skipped knockdown replays and round highlights. A folder in the save folder is mounted as
// cache: instead, before the game starts (FncApp::OnPreLaunchModule).
REXCVAR_DEFINE_BOOL(fnc_cache_drive, true, "Game",
                    "Give the game a cache: drive (needed for replays); stored in the save folder");

namespace fnc_cache {
void Mount() {
  if (!REXCVAR_GET(fnc_cache_drive)) return;
  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->file_system()) return;
  auto dir = runtime->user_data_root() / "cache_drive";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  auto device = std::make_unique<rex::filesystem::HostPathDevice>("\\CACHE", dir, false);
  if (!device->Initialize() || !runtime->file_system()->RegisterDevice(std::move(device)) ||
      !runtime->file_system()->RegisterSymbolicLink("cache:", "\\CACHE")) {
    REXLOG_WARN("[FNC-CACHE] could not mount {} as cache:", dir.string());
    return;
  }
  REXLOG_INFO("[FNC-CACHE] cache: is {}", dir.string());
}
}  // namespace fnc_cache
