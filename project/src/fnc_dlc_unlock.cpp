// [FNC-DLC] Unlock all downloadable content (TU2 build).
//
// Fight Night Champion only treats DLC as owned after its DLC system (DIME) has confirmed the
// purchase with EA's servers, which are closed. The game's own lock checks are answered here:
//
//   * Boxers. Every boxer that was sold or unlocked through the store: the Legends Pack boxers,
//     the Champion Mode boxers ("All Champion Mode Boxers" and the single-boxer packs) and the
//     George Foreman pre-order bonus. In practice this is every locked boxer.
//       - sub_82D0F410(field, boxer_id), the boxer database getter: field 24 = LOCKED
//         (Boxer Gallery and other boxer lists).
//       - sub_82DA0C38(boxer_id) = IsBoxerUnlocked, behind the UI's IsCharacterLocked
//         (Fight Now boxer select).
//   * The Legends Pack venues 200 and 201: sub_82D98E08(venue_id) = IsVenueUnlocked, behind the
//     UI's IsVenueLocked.
//   * Bare Knuckles (the "Bare Knuckles Game Mode" item):
//       - sub_82DAC548(this, feature) = DLCAdapter::IsFeatureAvailable (ION_DLC), which the main
//         menu asks (feature 0) before listing Bare Knuckles Mode;
//       - sub_82D66AB0(game_mode_id) = an unlocked dlc_license_map row for that mode (mode 2).
//
//   * Athleticism boosts (Strength/Speed/... Bronze/Silver/Gold): sub_82D669D0(boost_id) asks the
//     dlc_license_map table for an Unlocked row with that BoostID. It is answered with "has a
//     row", so every boost sold as DLC is owned; boosts earned in Legacy Mode stay as they are.
//   * XP packs (Boxer Experience Point Pack 1000/5000/10000/25000 XP): these were consumables
//     whose remaining count came from EA's servers, and using one asked the servers to take it
//     away before the XP was added. Here every pack is always in stock, and ApplyConsumable adds
//     the XP to the boxer directly (the same fields the game updates after a successful
//     purchase: ExperiencePoints and ExperiencePointsPurchased).
//
// The venues and Bare Knuckles use files from the Legends Pack (dlc_main.big), so they are only
// unlocked while that pack is installed; the Legends boxers only exist when it is installed.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_BOOL(fnc_dlc_unlock, true, "Game",
                    "Unlock all DLC: boxers, Legends venues, Bare Knuckles (EA's servers are closed)");

extern "C" {
void __imp__sub_82D0F410(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_82DA0C38(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_82D98E08(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_82D66AB0(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_82DAC548(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_82D669D0(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_82D2D3B0(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_82D01F60(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_82DABA70(PPCContext& __restrict ctx, uint8_t* base);
// Database query helpers (as used by the game's own lookups).
void sub_82E80840(PPCContext& __restrict ctx, uint8_t* base);  // column(this, field)
void sub_82E80B08(PPCContext& __restrict ctx, uint8_t* base);  // column == value
void sub_82E80260(PPCContext& __restrict ctx, uint8_t* base);  // database handle
void sub_82E81A58(PPCContext& __restrict ctx, uint8_t* base);  // select from table
void sub_82E7E350(PPCContext& __restrict ctx, uint8_t* base);  // where
void sub_82E89640(PPCContext& __restrict ctx, uint8_t* base);  // run query -> result
void sub_82438E68(PPCContext& __restrict ctx, uint8_t* base);  // release handle
void sub_82E7D478(PPCContext& __restrict ctx, uint8_t* base);  // ~column
void sub_82E7DD28(PPCContext& __restrict ctx, uint8_t* base);  // result row count
void sub_82E895D8(PPCContext& __restrict ctx, uint8_t* base);  // ~result
void sub_8260E950(PPCContext& __restrict ctx, uint8_t* base);  // row(out, result, index)
void sub_82E81A20(PPCContext& __restrict ctx, uint8_t* base);  // row value(field) -> int*
void sub_82E7DBD8(PPCContext& __restrict ctx, uint8_t* base);  // ~row
// Licenses.
void __imp__sub_82D405B0(PPCContext& __restrict ctx, uint8_t* base);
void sub_82D6BD70(PPCContext& __restrict ctx, uint8_t* base);  // apply one license
// Consumables and boxer records.
void sub_82D963E0(PPCContext& __restrict ctx, uint8_t* base);  // consumable row ctor
void sub_82D3BB90(PPCContext& __restrict ctx, uint8_t* base);  // load consumable row by id
void sub_82D7BC68(PPCContext& __restrict ctx, uint8_t* base);  // consumable row dtor
void sub_82DA2080(PPCContext& __restrict ctx, uint8_t* base);  // boxer record by boxer id
void sub_82D6AE20(PPCContext& __restrict ctx, uint8_t* base);  // release boxer record
void sub_82DB6D50(PPCContext& __restrict ctx, uint8_t* base);  // UI event sender
void sub_82DAC6A0(PPCContext& __restrict ctx, uint8_t* base);  // send consume result (1, result)
}

namespace {

constexpr uint32_t kFieldLocked = 24;
constexpr uint32_t kDlcVenues[] = {200, 201};  // dlc_venue_200.xml / dlc_venue_201.xml
constexpr uint32_t kBareKnucklesFeature = 0;
constexpr uint32_t kBareKnucklesGameMode = 2;
// The Fight Night Legends Pack package (its folder name under ...\45410915\00000002).
constexpr const char* kLegendsPackage = "E40F674A98DA244CD018C51ACA9815966328CED345";

bool Enabled() { return REXCVAR_GET(fnc_dlc_unlock); }

bool LegendsPackInstalled() {
  static const bool installed = [] {
    auto* runtime = rex::Runtime::instance();
    std::error_code ec;
    bool found = runtime && std::filesystem::is_directory(
                                runtime->user_data_root() / "0000000000000000" / "45410915" /
                                    "00000002" / kLegendsPackage,
                                ec);
    REXLOG_INFO("[FNC-DLC] all DLC unlocked; Legends Pack {}", found ? "installed" : "not installed");
    return found;
  }();
  return installed;
}

// Logs the first time each item is reported as unlocked.
void LogOnce(const char* what, uint32_t id) {
  static std::mutex mutex;
  static std::unordered_set<uint64_t> seen;
  uint64_t key = (uint64_t(what[0]) << 32) | id;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!seen.insert(key).second) return;
  }
  REXLOG_INFO("[FNC-DLC] {} {} reported as unlocked", what, id);
}

inline uint32_t Ld(uint8_t* base, uint32_t a) {
  return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(base + a));
}

void Call(PPCContext& ctx, uint8_t* base, uint32_t fn) {
  rex::runtime::ResolveIndirectFunction(fn)(ctx, base);
}
// obj->vtable[off](obj, a, b)
uint32_t VCall(PPCContext& ctx, uint8_t* base, uint32_t obj, uint32_t off, uint32_t a = 0,
               uint32_t b = 0) {
  ctx.r3.u64 = obj;
  ctx.r4.u64 = a;
  ctx.r5.u64 = b;
  Call(ctx, base, Ld(base, Ld(base, obj) + off));
  return ctx.r3.u32;
}

constexpr uint32_t kLicenseMapTable = 0x82FA6208;  // dlc_license_map columns
constexpr uint32_t kLicenseBoostId = 28;           // BoostID column offset in that list
constexpr uint32_t kDbSchema = 0x83061C44;
constexpr uint32_t kLicenseMapName = 0x82FA6238;

// True if dlc_license_map has a row for this BoostID, whether or not it is unlocked. This is
// sub_82D669D0 without its "Unlocked" condition.
bool BoostHasLicenseRow(PPCContext& ctx, uint8_t* base, uint32_t boost_id) {
  constexpr uint32_t kSize = 256;
  uint32_t saved_sp = ctx.r1.u32;
  uint32_t sp = saved_sp - kSize;
  *reinterpret_cast<uint32_t*>(base + sp) = __builtin_bswap32(saved_sp);
  ctx.r1.u64 = sp;

  ctx.r3.u64 = sp + 96;
  ctx.r4.u64 = Ld(base, kLicenseMapTable + kLicenseBoostId);
  sub_82E80840(ctx, base);
  ctx.r4.u64 = boost_id;
  sub_82E80B08(ctx, base);
  uint32_t condition = ctx.r3.u32;
  ctx.r3.u64 = sp + 84;
  ctx.r4.u64 = Ld(base, kDbSchema);
  sub_82E80260(ctx, base);
  ctx.r4.u64 = ctx.r3.u64;
  ctx.r3.u64 = sp + 80;
  ctx.r5.u64 = Ld(base, kLicenseMapName);
  sub_82E81A58(ctx, base);
  ctx.r4.u64 = condition;
  ctx.r5.u64 = 1;
  sub_82E7E350(ctx, base);
  ctx.r4.u64 = ctx.r3.u64;
  ctx.r3.u64 = sp + 160;
  sub_82E89640(ctx, base);
  ctx.r3.u64 = sp + 80;
  sub_82438E68(ctx, base);
  ctx.r3.u64 = sp + 84;
  sub_82438E68(ctx, base);
  ctx.r3.u64 = sp + 96;
  sub_82E7D478(ctx, base);
  ctx.r3.u64 = sp + 160;
  sub_82E7DD28(ctx, base);
  bool found = (ctx.r3.u32 & 0xFFFF) != 0;
  ctx.r3.u64 = sp + 160;
  sub_82E895D8(ctx, base);

  ctx.r1.u64 = saved_sp;
  return found;
}

// dlc_license_map columns (offsets in the column list at kLicenseMapTable).
constexpr uint32_t kLicenseLicenseId = 4, kLicenseVenueId = 12, kLicenseGameModeId = 24;

// Every license in dlc_license_map: {LicenseID, VenueId, GameModeID}.
struct LicenseRow {
  int32_t license, venue, game_mode;
};
std::vector<LicenseRow> AllLicenses(PPCContext& ctx, uint8_t* base) {
  constexpr uint32_t kSize = 256;
  uint32_t saved_sp = ctx.r1.u32;
  uint32_t sp = saved_sp - kSize;
  *reinterpret_cast<uint32_t*>(base + sp) = __builtin_bswap32(saved_sp);
  ctx.r1.u64 = sp;

  // SELECT * FROM dlc_license_map
  ctx.r3.u64 = sp + 84;
  ctx.r4.u64 = Ld(base, kDbSchema);
  sub_82E80260(ctx, base);
  ctx.r4.u64 = ctx.r3.u64;
  ctx.r3.u64 = sp + 80;
  ctx.r5.u64 = Ld(base, kLicenseMapName);
  sub_82E81A58(ctx, base);
  ctx.r4.u64 = ctx.r3.u64;
  ctx.r3.u64 = sp + 160;
  sub_82E89640(ctx, base);
  ctx.r3.u64 = sp + 80;
  sub_82438E68(ctx, base);
  ctx.r3.u64 = sp + 84;
  sub_82438E68(ctx, base);

  ctx.r3.u64 = sp + 160;
  sub_82E7DD28(ctx, base);
  uint32_t rows = ctx.r3.u32 & 0xFFFF;
  auto value = [&](uint32_t column) {
    ctx.r3.u64 = sp + 88;
    ctx.r4.u64 = Ld(base, kLicenseMapTable + column);
    sub_82E81A20(ctx, base);
    return int32_t(Ld(base, ctx.r3.u32));
  };
  std::vector<LicenseRow> result;
  for (uint32_t i = 0; i < rows; ++i) {
    ctx.r3.u64 = sp + 88;
    ctx.r4.u64 = sp + 160;
    ctx.r5.u64 = i;
    sub_8260E950(ctx, base);
    result.push_back({value(kLicenseLicenseId), value(kLicenseVenueId), value(kLicenseGameModeId)});
    ctx.r3.u64 = sp + 88;
    sub_82E7DBD8(ctx, base);
  }
  ctx.r3.u64 = sp + 160;
  sub_82E895D8(ctx, base);

  ctx.r1.u64 = saved_sp;
  return result;
}

// Applies every license the way the game does for one EA has confirmed (sub_82D6BD70: marks its
// dlc_license_map rows Unlocked and unlocks the boxer, venue or gear each row names). Licenses
// for the Legends venues or Bare Knuckles are skipped without the Legends Pack (their files are
// in it).
void UnlockAllLicenses(PPCContext& ctx, uint8_t* base) {
  auto rows = AllLicenses(ctx, base);
  std::unordered_set<int32_t> skip, done;
  if (!LegendsPackInstalled()) {
    for (const auto& r : rows) {
      bool dlc_venue = r.venue == int32_t(kDlcVenues[0]) || r.venue == int32_t(kDlcVenues[1]);
      if (dlc_venue || r.game_mode == int32_t(kBareKnucklesGameMode)) skip.insert(r.license);
    }
  }
  for (const auto& r : rows) {
    if (r.license <= 0 || skip.count(r.license) || !done.insert(r.license).second) continue;
    ctx.r3.u64 = uint32_t(r.license);
    sub_82D6BD70(ctx, base);
  }
  REXLOG_INFO("[FNC-DLC] applied {} licenses ({} rows, {} skipped without the Legends Pack)", done.size(),
              rows.size(), skip.size());
}

// Boxer record fields (columns of the boxer table).
constexpr uint32_t kFieldExperiencePoints = 199;
constexpr uint32_t kFieldExperiencePointsPurchased = 204;
// Record vtable: +28 get(field), +40 set(field, value), +68 save.
constexpr uint32_t kRecGet = 28, kRecSet = 40, kRecSave = 68;
constexpr uint32_t kCachedRecordHolder = 0x82FE7FA4;  // holder; record at +12 (sub_82B327C8)
constexpr uint32_t kConsumableTypeXp = 1;
constexpr uint32_t kXpPackStock = 99;

// Looks up the consumable; returns its XP amount, or 0 if it isn't an XP pack.
uint32_t XpPackAmount(PPCContext& ctx, uint8_t* base, uint32_t consumable_id) {
  constexpr uint32_t kSize = 224;
  uint32_t saved_sp = ctx.r1.u32;
  uint32_t sp = saved_sp - kSize;
  *reinterpret_cast<uint32_t*>(base + sp) = __builtin_bswap32(saved_sp);
  ctx.r1.u64 = sp;
  uint32_t row_obj = sp + 80;

  ctx.r3.u64 = row_obj;
  sub_82D963E0(ctx, base);
  ctx.r3.u64 = consumable_id;
  ctx.r4.u64 = row_obj;
  sub_82D3BB90(ctx, base);
  uint32_t row = Ld(base, row_obj + 36);
  uint32_t type = row ? Ld(base, row + 4) : 0;
  uint32_t amount = row ? Ld(base, row + 8) : 0;
  ctx.r3.u64 = row_obj;
  sub_82D7BC68(ctx, base);

  ctx.r1.u64 = saved_sp;
  return type == kConsumableTypeXp ? amount : 0;
}

void AddXp(PPCContext& ctx, uint8_t* base, uint32_t record, uint32_t amount, uint32_t old_purchased) {
  uint32_t xp = VCall(ctx, base, record, kRecGet, kFieldExperiencePoints);
  VCall(ctx, base, record, kRecSet, kFieldExperiencePoints, xp + amount);
  VCall(ctx, base, record, kRecSet, kFieldExperiencePointsPurchased, old_purchased + amount);
  VCall(ctx, base, record, kRecSave);
}

// Adds an XP pack to a boxer; returns false if the consumable isn't an XP pack.
bool ApplyXpPack(PPCContext& ctx, uint8_t* base, uint32_t boxer_id, uint32_t consumable_id) {
  uint32_t amount = XpPackAmount(ctx, base, consumable_id);
  if (!amount) return false;

  constexpr uint32_t kSize = 112;
  uint32_t saved_sp = ctx.r1.u32;
  uint32_t sp = saved_sp - kSize;
  *reinterpret_cast<uint32_t*>(base + sp) = __builtin_bswap32(saved_sp);
  ctx.r1.u64 = sp;

  ctx.r3.u64 = boxer_id;
  sub_82DA2080(ctx, base);
  uint32_t record = ctx.r3.u32;
  *reinterpret_cast<uint32_t*>(base + sp + 80) = __builtin_bswap32(record);
  uint32_t purchased = VCall(ctx, base, record, kRecGet, kFieldExperiencePointsPurchased);
  uint32_t xp_before = VCall(ctx, base, record, kRecGet, kFieldExperiencePoints);
  AddXp(ctx, base, record, amount, purchased);
  ctx.r3.u64 = sp + 80;
  sub_82D6AE20(ctx, base);

  // The game also updates its in-memory copy of the boxer being edited (if it is this one).
  uint32_t holder = Ld(base, kCachedRecordHolder);
  uint32_t cached = holder ? Ld(base, holder + 12) : 0;
  if (cached && VCall(ctx, base, cached, kRecGet, kFieldExperiencePointsPurchased) == purchased &&
      VCall(ctx, base, cached, kRecGet, kFieldExperiencePoints) == xp_before) {
    AddXp(ctx, base, cached, amount, purchased);
  }

  // Tell the boosts screen (it waits for XP_PACK_CONSUMED): result 0 = success, "XP applied".
  sub_82DB6D50(ctx, base);
  ctx.r4.u64 = 1;
  ctx.r5.u64 = 0;
  sub_82DAC6A0(ctx, base);

  ctx.r1.u64 = saved_sp;
  REXLOG_INFO("[FNC-DLC] XP pack {} used on boxer {}: +{} XP ({} -> {})", consumable_id, boxer_id, amount,
              xp_before, xp_before + amount);
  return true;
}

}  // namespace

// Applies the licenses EA's servers confirmed (none, offline); then every license.
extern "C" void sub_82D405B0(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_82D405B0(ctx, base);
  if (Enabled()) {
    uint64_t r3 = ctx.r3.u64;
    UnlockAllLicenses(ctx, base);
    ctx.r3.u64 = r3;
  }
}

extern "C" void sub_82D669D0(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t boost_id = ctx.r3.u32;
  __imp__sub_82D669D0(ctx, base);
  if ((ctx.r3.u32 & 0xFF) == 0 && Enabled() && BoostHasLicenseRow(ctx, base, boost_id)) {
    LogOnce("boost", boost_id);
    ctx.r3.u64 = 1;
  }
}

// Consumable inventory (only used by the Legacy Mode consumables list): every pack in stock.
extern "C" void sub_82D2D3B0(PPCContext& __restrict ctx, uint8_t* base) {
  if (!Enabled()) return __imp__sub_82D2D3B0(ctx, base);
  ctx.r3.u64 = 1;
}
extern "C" void sub_82D01F60(PPCContext& __restrict ctx, uint8_t* base) {
  if (!Enabled()) return __imp__sub_82D01F60(ctx, base);
  ctx.r3.u64 = kXpPackStock;
}

// ApplyConsumable(this, boxer_id, consumable_id).
extern "C" void sub_82DABA70(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t boxer_id = ctx.r4.u32, consumable_id = ctx.r5.u32;
  if (Enabled() && ApplyXpPack(ctx, base, boxer_id, consumable_id)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_82DABA70(ctx, base);
}

extern "C" void sub_82D0F410(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t field = ctx.r3.u32, boxer_id = ctx.r4.u32;
  __imp__sub_82D0F410(ctx, base);
  if (field == kFieldLocked && ctx.r3.u32 == 1 && Enabled()) {
    LogOnce("boxer", boxer_id);
    ctx.r3.u64 = 0;
  }
}

extern "C" void sub_82DA0C38(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t boxer_id = ctx.r3.u32;
  __imp__sub_82DA0C38(ctx, base);
  if ((ctx.r3.u32 & 0xFF) == 0 && Enabled()) {
    LogOnce("boxer", boxer_id);
    ctx.r3.u64 = 1;
  }
}

extern "C" void sub_82D98E08(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t venue_id = ctx.r3.u32;
  __imp__sub_82D98E08(ctx, base);
  if ((ctx.r3.u32 & 0xFF) != 0 || !Enabled()) return;
  for (uint32_t v : kDlcVenues) {
    if (venue_id == v && LegendsPackInstalled()) {
      LogOnce("venue", venue_id);
      ctx.r3.u64 = 1;
    }
  }
}

extern "C" void sub_82D66AB0(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t mode = ctx.r3.u32;
  __imp__sub_82D66AB0(ctx, base);
  if ((ctx.r3.u32 & 0xFF) == 0 && mode == kBareKnucklesGameMode && Enabled() &&
      LegendsPackInstalled()) {
    LogOnce("game mode", mode);
    ctx.r3.u64 = 1;
  }
}

extern "C" void sub_82DAC548(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t feature = ctx.r4.u32;
  __imp__sub_82DAC548(ctx, base);
  if ((ctx.r3.u32 & 0xFF) == 0 && feature == kBareKnucklesFeature && Enabled() &&
      LegendsPackInstalled()) {
    LogOnce("feature", feature);
    ctx.r3.u64 = 1;
  }
}
