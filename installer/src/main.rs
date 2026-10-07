// SPDX-License-Identifier: Apache-2.0
//! FNC Reborn Setup: installs and updates FNC Reborn. Downloads a portable
//! build toolchain (no Visual Studio), builds the game from the player's own
//! disc, and keeps it up to date.
#![windows_subsystem = "windows"]

mod config;
mod display_tab;
mod fsx;
mod gui;
mod pipeline;
mod platform;
mod report;
mod tools;

use pipeline::{GameSource, Options};
use report::Reporter;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;

const HELP: &str = "FNC Reborn Setup

Without options the setup window opens. Command line use:
  FNCReborn-Setup --cli --dir <folder> [--iso <file> | --game-dir <folder>]
                  --accept-license [--update] [--force] [--no-shortcut]

  --dir <folder>       install folder (or the existing FNC Reborn folder)
  --iso <file>         your Fight Night Champion (Xbox 360) disc image
  --game-dir <folder>  a folder with your game files (default.xex, databigs)
  --accept-license     accept the Microsoft Visual Studio license terms for the
                       C++ headers and libraries that are downloaded
  --update             update an existing install (default when --dir is one)
  --force              rebuild even when nothing changed
  --no-download        build the source already in --dir instead of
                       downloading it (for testing local changes)
  --no-shortcut        no desktop shortcut
  --no-integration     no shortcuts or menu entries at all, and the folder is
                       not remembered (for test builds)
  --display-tab <file> only add the Settings > DISPLAY tab to this fe.big
  --sdk-only <src>     only build the toolchain, the SDK and the recompiler
                       for a source folder (for CI; no game files needed)";

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let has = |f: &str| {
        args.iter()
            .any(|a| a.eq_ignore_ascii_case(f) || a.eq_ignore_ascii_case(&f.replace("--", "/")))
    };
    let value = |f: &str| {
        args.iter()
            .position(|a| a.eq_ignore_ascii_case(f))
            .and_then(|i| args.get(i + 1))
            .map(PathBuf::from)
    };
    let cli =
        has("--cli") || has("--help") || has("-h") || has("--display-tab") || has("--sdk-only");
    if cli {
        platform::attach_console();
    }
    if has("--help") || has("-h") {
        println!("{HELP}");
        return;
    }
    if let Some(fe) = value("--display-tab") {
        match display_tab::patch_fe_big(&fe) {
            Ok(true) => println!("Added the DISPLAY tab to {}", fe.display()),
            Ok(false) => println!("{} already has the DISPLAY tab.", fe.display()),
            Err(e) => {
                eprintln!("ERROR: {e:#}");
                std::process::exit(1)
            }
        }
        return;
    }
    if let Some(src) = value("--sdk-only") {
        let r = Reporter::new(None, Arc::new(AtomicBool::new(false)), true);
        match pipeline::sdk_only(
            &r,
            &std::path::absolute(&src).unwrap_or(src),
            has("--accept-license"),
        ) {
            Ok(s) => println!("{s}"),
            Err(e) => {
                eprintln!("ERROR: {e:#}");
                std::process::exit(1)
            }
        }
        return;
    }
    if cli {
        let Some(dir) = value("--dir") else {
            eprintln!("--dir is needed.\n\n{HELP}");
            std::process::exit(2)
        };
        let game = match (value("--iso"), value("--game-dir")) {
            (Some(i), _) => GameSource::Iso(i),
            (_, Some(g)) => GameSource::Folder(g),
            _ => GameSource::None,
        };
        let dir = std::path::absolute(&dir).unwrap_or(dir);
        let o = Options {
            update: has("--update") || pipeline::is_install_folder(&dir),
            dir,
            game,
            desktop_shortcut: !has("--no-shortcut") && !has("--no-integration"),
            integrate: !has("--no-integration"),
            accept_license: has("--accept-license"),
            force: has("--force"),
            fetch_source: !has("--no-download"),
        };
        if let Err(e) = pipeline::validate(&o) {
            eprintln!("{e:#}");
            std::process::exit(2)
        }
        let r = Reporter::new(None, Arc::new(AtomicBool::new(false)), true);
        match pipeline::run_all(&r, &o) {
            Ok(s) => {
                r.log(s);
                r.close_log();
            }
            Err(e) => {
                r.log(format!("ERROR: {e:#}"));
                r.close_log();
                std::process::exit(1)
            }
        }
        return;
    }

    let update = has("--update");
    let dir = value("--dir");
    let options = eframe::NativeOptions {
        viewport: eframe::egui::ViewportBuilder::default()
            .with_title("FNC Reborn Setup")
            .with_inner_size([760.0, 560.0])
            .with_min_inner_size([620.0, 460.0]),
        ..Default::default()
    };
    let _ = eframe::run_native(
        "FNC Reborn Setup",
        options,
        Box::new(move |cc| {
            cc.egui_ctx.set_theme(eframe::egui::Theme::Dark);
            Ok(Box::new(gui::SetupApp::new(update, dir)))
        }),
    );
}
