// [FNC-CAP] Raise the Legacy Mode skill level cap.
//
// The skills screen's "LEVELS AVAILABLE x/220" is the "XpMaxSlots" tuning value: how many skill
// levels a boxer may buy in total (17 skills x 20 levels = 340 exist). The game shipped with 220
// ("Your boxer has reached his skill level cap! For now...") and EA meant to raise it later.
// Only two functions read XpMaxSlots:
//   * sub_82E01590(growth) = the cap (the screen's MAX_SLOTS);
//   * sub_82E033A0(growth, levels[17]) = cap - levels bought, at least 0 (levels still
//     available; used by the screen and by buying levels).
// Both are answered with fnc_skill_level_cap instead (never lower than the game's own value).

#include <algorithm>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_INT32(fnc_skill_level_cap, 320, "Game",
                     "Legacy Mode skill level cap (game default 220, the most there are is 340)");

extern "C" {
void __imp__sub_82E01590(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_82E033A0(PPCContext& __restrict ctx, uint8_t* base);
}

namespace {

constexpr uint32_t kSkillCount = 17;
constexpr int32_t kMostLevels = 340;

inline uint32_t Ld(uint8_t* base, uint32_t a) {
  return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(base + a));
}

int32_t Cap(int32_t game_cap) {
  int32_t cap = std::clamp<int32_t>(REXCVAR_GET(fnc_skill_level_cap), 0, kMostLevels);
  int32_t result = std::max(cap, game_cap);
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("[FNC-CAP] skill level cap {} (game: {})", result, game_cap);
  }
  return result;
}

}  // namespace

extern "C" void sub_82E01590(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_82E01590(ctx, base);
  ctx.r3.u64 = uint32_t(Cap(int32_t(ctx.r3.u32)));
}

extern "C" void sub_82E033A0(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t growth = ctx.r3.u32, levels = ctx.r4.u32;
  int32_t used = 0;
  for (uint32_t i = 0; i < kSkillCount; ++i) used += int32_t(Ld(base, levels + 4 * i));
  ctx.r3.u64 = growth;
  __imp__sub_82E01590(ctx, base);
  int32_t cap = Cap(int32_t(ctx.r3.u32));
  ctx.r3.u64 = uint32_t(std::max(cap - used, 0));
}
