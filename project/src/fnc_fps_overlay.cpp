// F1 FPS counter: just the number in the top-left corner, nothing else.
// F11: toggle windowed / fullscreen.
// FPS = the game's own frame swaps per second (counted in rexgpu-xenos.dll),
// averaged over half a second, so it shows what the game is really doing.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>

#include <imgui.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fnc_fps {
namespace {

using SwapCountFn = uint64_t (*)();

SwapCountFn GetSwapCounter() {
#ifdef _WIN32
  static SwapCountFn fn = nullptr;
  if (!fn) {
    if (HMODULE m = GetModuleHandleW(L"rexgpu-xenos.dll")) {
      fn = reinterpret_cast<SwapCountFn>(GetProcAddress(m, "fnc_gpu_swap_count"));
    }
  }
  return fn;
#else
  return nullptr;
#endif
}

class FpsOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit FpsOverlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    auto now = std::chrono::steady_clock::now();
    SwapCountFn swaps = GetSwapCounter();
    uint64_t count = swaps ? swaps() : 0;
    if (!started_) {
      started_ = true;
      last_time_ = now;
      last_count_ = count;
    }
    double secs = std::chrono::duration<double>(now - last_time_).count();
    if (secs >= 0.5) {
      fps_ = swaps ? double(count - last_count_) / secs : double(io.Framerate);
      last_time_ = now;
      last_count_ = count;
    }

    char text[32];
    std::snprintf(text, sizeof(text), "%.0f", fps_);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float size = ImGui::GetFontSize() * 1.4f;
    ImVec2 pos(12.0f, 10.0f);
    dl->AddText(ImGui::GetFont(), size, ImVec2(pos.x + 1.5f, pos.y + 1.5f),
                IM_COL32(0, 0, 0, 200), text);
    dl->AddText(ImGui::GetFont(), size, pos, IM_COL32(255, 255, 255, 255), text);
  }

 private:
  bool started_ = false;
  std::chrono::steady_clock::time_point last_time_{};
  uint64_t last_count_ = 0;
  double fps_ = 0.0;
};

std::unique_ptr<FpsOverlay> g_overlay;

}  // namespace

void Install(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window) {
  rex::ui::RegisterBind("bind_fps_counter", "F1", "Toggle FPS counter", [drawer] {
    if (g_overlay) {
      g_overlay.reset();
    } else {
      g_overlay = std::make_unique<FpsOverlay>(drawer);
    }
  });
  rex::ui::RegisterBind("bind_fullscreen", "F11", "Toggle windowed / fullscreen", [window] {
    if (window) {
      window->SetFullscreen(!window->IsFullscreen());
    }
  });
}

}  // namespace fnc_fps
