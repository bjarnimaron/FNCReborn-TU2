// Game-specific function overrides for Fight Night Champion (default.xex v0.0.0.3,
// media 2DC50868). Addresses are only valid for that executable.
//
// Recompiled functions are weak symbols; defining one here replaces it, and
// __imp__<name> is always the original.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_DOUBLE(fnc_frame_rate, 60.0, "Game",
                      "Frame rate cap. The retail game runs at 30. Set 30 to restore it.");

// ---------------------------------------------------------------------------
// 60 FPS.
// sub_827C6380 builds one of the game's fixed-rate frame timers; f1 = rate in Hz,
// which sub_827C5E60 turns into a tick period (timebase / rate). The game makes
// exactly two, each once:
//   sub_822A9570 -> 30 Hz (constant 0x82008430), returns to 0x822A9594  <- changed
//   sub_822AA558 -> 60 Hz (constant 0x82008454), returns to 0x822AA580  <- untouched
// The community "Unlock FPS" patch for TU2 (illusion, xenia-canary/game-patches)
// raises the same 30.0 constant (it sits at 0x82008574 in TU2).
// ---------------------------------------------------------------------------
extern "C" void __imp__sub_827C6380(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_827C6380(PPCContext& __restrict ctx, uint8_t* base) {
  if (ctx.lr == 0x822A9594 && ctx.f1.f64 == 30.0) {
    double rate = REXCVAR_GET(fnc_frame_rate);
    if (rate >= 10.0 && rate <= 240.0 && rate != 30.0) {
      REXLOG_INFO("[FNC-60FPS] frame timer 30 Hz -> {:.0f} Hz (fnc_frame_rate)", rate);
      ctx.f1.f64 = rate;
    } else {
      REXLOG_INFO("[FNC-60FPS] frame timer left at 30 Hz (fnc_frame_rate = {})", rate);
    }
  }
  __imp__sub_827C6380(ctx, base);
}
