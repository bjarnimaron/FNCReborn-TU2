// FNC Reborn - application class (window, paths, overlays).
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>

namespace fnc_prof {
void Start(const void* func_mappings);  // src/fnc_sampler.cpp
}
namespace fnc_cache {
void Mount();  // src/fnc_hooks.cpp (cache: drive)
}
namespace rex::ui { class ImGuiDrawer; class Window; }
namespace fnc_fps {
void Install(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window);  // src/fnc_fps_overlay.cpp (F1 FPS, F11 windowed)
}

class FncApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    // The name sets FNCReborn.toml (next to the exe) and the save folder Documents\FNCReborn.
    return std::unique_ptr<FncApp>(new FncApp(ctx, "FNCReborn",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  void OnPostSetup() override {
    // Crash reporter; the sampling profiler only runs when a "profile_on" file sits next to
    // FNCReborn.exe.
    fnc_prof::Start(PPCImageConfig.func_mappings);
  }
  void OnPreLaunchModule() override { fnc_cache::Mount(); }
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    if (window()) window()->SetTitle("FNC Reborn");
    fnc_fps::Install(drawer, window());
  }
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  void OnConfigurePaths(rex::PathConfig& paths) override {
    auto exe_dir = rex::filesystem::GetExecutableFolder();
    {
      // A relative game_data_root is relative to the exe, not to wherever the
      // game was started from (shortcuts and Explorer use other folders).
      std::error_code ec;
      auto has_xex = [&ec](const std::filesystem::path& p) {
        return std::filesystem::exists(p / "default.xex", ec);
      };
      if (!paths.game_data_root.empty() && paths.game_data_root.is_relative()) {
        paths.game_data_root = exe_dir / paths.game_data_root;
      }
      if (paths.game_data_root.empty() || !has_xex(paths.game_data_root)) {
        // dist\game next to the exe (normal install). The others cover starting
        // the copy in build\game, or a development tree with ..\extracted_game.
        const std::filesystem::path candidates[] = {
            exe_dir / "game",
            exe_dir.parent_path() / "dist" / "game",
            exe_dir.parent_path().parent_path() / "dist" / "game",
            exe_dir.parent_path() / "extracted_game",
        };
        auto found = exe_dir / "game";  // shown in the error if nothing is found
        for (const auto& c : candidates) {
          if (has_xex(c)) {
            found = c;
            break;
          }
        }
        paths.game_data_root = found;
      }
    }
    // The TU2 build keeps its saves and DLC in Documents\FNCReborn-TU2, apart from the disc
    // build's Documents\FNCReborn. (FNCReborn.toml is read after the paths are chosen, so a
    // user_data_root set there has no effect.)
    if (paths.user_data_root.filename() == "FNCReborn") {
      auto tu2_dir = paths.user_data_root.parent_path() / "FNCReborn-TU2";
      if (paths.cache_root == paths.user_data_root / "cache") paths.cache_root = tu2_dir / "cache";
      paths.user_data_root = tu2_dir;
    }
  }
};
