// Player camera mod for Fight Night Champion (default.xex v0.0.0.3 + title update 2 only).
//
// Back on the controller (or F2) toggles the normal TV camera <-> a camera that follows YOUR boxer:
// third person behind your boxer, looking at the opponent (or first person with
// fnc_view_first_person = true). "Your boxer" = the boxer camera-target object at the lower address
// (the game creates the player's first; set fnc_view_boxer = 1 if a fight picks the opponent).
//
// How it works:
//  * Live game camera object (the one sub_823180A0 is asked for while sub_8231D2B0 runs):
//      +0x04 fov (radians)  +0x08 aspect  +0x0C near  +0x10 far
//      +0x20 forward xyz    +0x30 up xyz  +0x40 position xyz (feet)  +0x50 focus distance
//  * sub_8231D2B0 (a CameraDirector method, ~60 times a second) finishes the camera each update.
//    We put the game's own values back, let it run, save its result, then write our camera,
//    so the game's camera logic never sees our values.
//  * Boxer camera-target objects (r3 of sub_82317E58): +0x08 boxer height, then bone records of
//    { quat xyzw, pos xyz, pad } every 0x20 bytes from +0x10; record 0 = head (pos at +0x20).
//  * The Back-button press count comes from rexruntime.dll (fnc_back_press_count, SDK patch).
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(fnc_fp_enable, true, "Game", "Player camera mod: Back / F2 toggles it");
REXCVAR_DEFINE_BOOL(fnc_view_first_person, false, "Game", "Player camera is first person instead of third person");
REXCVAR_DEFINE_INT32(fnc_view_boxer, 0, "Game", "Which boxer is yours: 0 = first created (default), 1 = the other");
REXCVAR_DEFINE_DOUBLE(fnc_tp_back, 8.0, "Game", "Third person: distance behind your head (feet)");
REXCVAR_DEFINE_DOUBLE(fnc_tp_height, 1.8, "Game", "Third person: height above your head (feet)");
REXCVAR_DEFINE_DOUBLE(fnc_tp_side, 1.5, "Game", "Third person: sideways offset, + = over the right shoulder (feet)");
REXCVAR_DEFINE_DOUBLE(fnc_tp_fov, 0.8, "Game", "Third person: field of view (TV camera ~0.5)");
REXCVAR_DEFINE_DOUBLE(fnc_tp_look_between, 0.65, "Game", "Third person: aim point between your head (0) and the opponent's (1)");
REXCVAR_DEFINE_DOUBLE(fnc_tp_smooth, 0.85, "Game", "Third person: smoothing 0 (none) .. 0.95 (heavy)");
REXCVAR_DEFINE_DOUBLE(fnc_fp_fov, 1.0, "Game", "First-person field of view (game units, retail ~0.5)");
REXCVAR_DEFINE_DOUBLE(fnc_fp_forward, 0.35, "Game", "First-person eye offset towards the opponent (feet)");
REXCVAR_DEFINE_DOUBLE(fnc_fp_up, 0.05, "Game", "First-person eye height offset (feet)");
REXCVAR_DEFINE_DOUBLE(fnc_fp_aim_drop, 0.4, "Game", "Aim this far below the opponent's head (feet)");
REXCVAR_DEFINE_DOUBLE(fnc_fp_smooth, 0.5, "Game", "Eye smoothing 0 (none) .. 0.95 (heavy)");
REXCVAR_DEFINE_BOOL(fnc_fp_restore, true, "Game", "Restore the game camera before each camera update");

namespace {

struct V3 { float x, y, z; };
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Len(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Norm(V3 a) { float l = Len(a); return l > 1e-6f ? a * (1.0f / l) : V3{0, 0, 1}; }

float RdF(uint8_t* base, uint32_t a) {
  uint32_t v; std::memcpy(&v, base + a, 4); v = __builtin_bswap32(v);
  float f; std::memcpy(&f, &v, 4); return f;
}
void WrF(uint8_t* base, uint32_t a, float f) {
  uint32_t v; std::memcpy(&v, &f, 4); v = __builtin_bswap32(v); std::memcpy(base + a, &v, 4);
}
V3 RdV(uint8_t* base, uint32_t a) { return {RdF(base, a), RdF(base, a + 4), RdF(base, a + 8)}; }
void WrV(uint8_t* base, uint32_t a, V3 v) { WrF(base, a, v.x); WrF(base, a + 4, v.y); WrF(base, a + 8, v.z); }
bool Sane(float f, float lim) { return std::isfinite(f) && std::fabs(f) < lim; }

constexpr uint32_t kCamSaveSize = 0x60;
thread_local bool t_in_director = false;
uint32_t g_cam_inner = 0;       // camera object asked for during the current director update
uint32_t g_cam = 0;             // camera object we last wrote
uint8_t g_saved[kCamSaveSize];  // the game's own camera values from the last update
bool g_written = false;
uint64_t g_update = 0;
int g_mode = 0;  // 0 normal, 1 player camera
V3 g_eye_smoothed{}, g_aim_smoothed{};
bool g_eye_valid = false;

struct BoxerSlot { std::atomic<uint32_t> obj{0}; std::atomic<uint64_t> seen{0}; };
BoxerSlot g_boxer[2];

bool BoxerValid(uint8_t* base, uint32_t obj) {
  if (obj < 0x30000000u || obj >= 0xC0000000u) return false;
  float h = RdF(base, obj + 0x08);
  V3 head = RdV(base, obj + 0x20);
  return Sane(h, 100) && h > 3.5f && h < 8.5f && Sane(head.x, 500) && Sane(head.z, 500) &&
         head.y > 0.5f && head.y < 9.0f;
}

void NoteBoxer(uint8_t* base, uint32_t obj) {
  if (!BoxerValid(base, obj)) return;
  uint64_t now = g_update;
  for (auto& b : g_boxer) {
    if (b.obj.load(std::memory_order_relaxed) == obj) { b.seen.store(now, std::memory_order_relaxed); return; }
  }
  // Replace the stalest slot.
  BoxerSlot& s = g_boxer[0].seen.load() <= g_boxer[1].seen.load() ? g_boxer[0] : g_boxer[1];
  s.obj.store(obj, std::memory_order_relaxed);
  s.seen.store(now, std::memory_order_relaxed);
}

uint32_t ReadBackPresses() {
#ifdef _WIN32
  using Fn = uint32_t (*)();
  static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"rexruntime.dll"), "fnc_back_press_count"));
  return fn ? fn() : 0;
#else
  return 0;
#endif
}

bool ToggleRequested() {
  static uint32_t last_back = ReadBackPresses();
  uint32_t back = ReadBackPresses();
  bool pressed = back != last_back;
  last_back = back;
#ifdef _WIN32
  static bool f2_prev = false;
  DWORD fg_pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
  bool f2 = fg_pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
  if (f2 && !f2_prev) pressed = true;
  f2_prev = f2;
#endif
  return pressed;
}

}  // namespace

// Accessor: returns camera+0x20. Remember which camera object the director works on.
extern "C" void __imp__sub_823180A0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_823180A0(PPCContext& __restrict ctx, uint8_t* base) {
  if (t_in_director) g_cam_inner = ctx.r3.u32;
  __imp__sub_823180A0(ctx, base);
}

// Boxer camera target (head/neck/chest/... bone records).
extern "C" void __imp__sub_82317E58(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82317E58(PPCContext& __restrict ctx, uint8_t* base) {
  NoteBoxer(base, ctx.r3.u32);
  __imp__sub_82317E58(ctx, base);
}

// CameraDirector update: finishes the live camera once per game camera update.
extern "C" void __imp__sub_8231D2B0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8231D2B0(PPCContext& __restrict ctx, uint8_t* base) {
  ++g_update;
  if (!REXCVAR_GET(fnc_fp_enable)) {
    __imp__sub_8231D2B0(ctx, base);
    return;
  }
  if (ToggleRequested()) {
    g_mode = g_mode ? 0 : 1;
    g_eye_valid = false;
    REXLOG_INFO("[FNC-FP] view {} (0 normal, 1 player camera); boxers {:08X} {:08X}, camera {:08X}",
                g_mode, g_boxer[0].obj.load(), g_boxer[1].obj.load(), g_cam);
  }

  // Give the game its own camera back before it updates (keeps its smoothing/logic consistent).
  if (g_written && g_cam && REXCVAR_GET(fnc_fp_restore)) std::memcpy(base + g_cam, g_saved, kCamSaveSize);
  g_written = false;

  g_cam_inner = 0;
  t_in_director = true;
  __imp__sub_8231D2B0(ctx, base);
  t_in_director = false;

  uint32_t cam = g_cam_inner;
  if (cam < 0x30000000u || cam >= 0xC0000000u) return;
  g_cam = cam;
  if (g_mode == 0) return;

  // Your boxer = lower address (fnc_view_boxer = 0) or the higher one; both must be seen recently.
  uint32_t o0 = g_boxer[0].obj.load(), o1 = g_boxer[1].obj.load();
  uint64_t s0 = g_boxer[0].seen.load(), s1 = g_boxer[1].seen.load();
  if (!o0 || !o1 || o0 == o1 || g_update - s0 > 30 || g_update - s1 > 30) return;
  if (!BoxerValid(base, o0) || !BoxerValid(base, o1)) return;
  uint32_t lo = o0 < o1 ? o0 : o1, hi = o0 < o1 ? o1 : o0;
  uint32_t me = REXCVAR_GET(fnc_view_boxer) == 1 ? hi : lo;
  uint32_t opp = me == o0 ? o1 : o0;

  V3 my_head = RdV(base, me + 0x20);
  V3 opp_head = RdV(base, opp + 0x20);
  V3 to_opp = opp_head - my_head;
  // Facing direction on the floor; hold the last good one when the heads are nearly stacked
  // (clinch, knockdown) so the camera doesn't whip around.
  static V3 last_flat{0, 0, 1};
  V3 flat_raw{to_opp.x, 0.0f, to_opp.z};
  V3 flat = Len(flat_raw) > 0.75f ? Norm(flat_raw) : last_flat;
  last_flat = flat;
  V3 world_up{0, 1, 0};
  V3 eye, aim;
  float fov, k;
  if (REXCVAR_GET(fnc_view_first_person)) {
    eye = my_head + flat * float(REXCVAR_GET(fnc_fp_forward)) + V3{0, float(REXCVAR_GET(fnc_fp_up)), 0};
    aim = opp_head - V3{0, float(REXCVAR_GET(fnc_fp_aim_drop)), 0};
    fov = float(REXCVAR_GET(fnc_fp_fov));
    k = float(REXCVAR_GET(fnc_fp_smooth));
  } else {
    V3 right = Norm(V3{-flat.z, 0.0f, flat.x});  // flat x up, horizontal
    eye = my_head - flat * float(REXCVAR_GET(fnc_tp_back)) +
          V3{0, float(REXCVAR_GET(fnc_tp_height)), 0} + right * float(REXCVAR_GET(fnc_tp_side));
    float t = float(REXCVAR_GET(fnc_tp_look_between));
    aim = my_head + to_opp * t - V3{0, float(REXCVAR_GET(fnc_fp_aim_drop)) * t, 0};
    fov = float(REXCVAR_GET(fnc_tp_fov));
    k = float(REXCVAR_GET(fnc_tp_smooth));
  }
  k = std::fmin(std::fmax(k, 0.0f), 0.95f);
  if (g_eye_valid) {
    eye = eye + (g_eye_smoothed - eye) * k;
    aim = aim + (g_aim_smoothed - aim) * k;
  }
  g_eye_smoothed = eye;
  g_aim_smoothed = aim;
  g_eye_valid = true;
  V3 fwd = Norm(aim - eye);
  V3 up = Norm(world_up - fwd * Dot(fwd, world_up));
  if (!Sane(eye.x, 500) || !Sane(fwd.x, 2) || !Sane(up.y, 2)) return;

  std::memcpy(g_saved, base + cam, kCamSaveSize);
  WrF(base, cam + 0x04, fov);
  WrV(base, cam + 0x20, fwd);
  WrV(base, cam + 0x30, up);
  WrV(base, cam + 0x40, eye);
  WrF(base, cam + 0x50, Len(aim - eye));
  g_written = true;

  static uint64_t last_log = 0;
  if (g_update - last_log > 600) {
    last_log = g_update;
    REXLOG_INFO("[FNC-FP] view {} eye ({:.2f} {:.2f} {:.2f}) fwd ({:.2f} {:.2f} {:.2f}) cam {:08X}", g_mode,
                eye.x, eye.y, eye.z, fwd.x, fwd.y, fwd.z, cam);
  }
}
