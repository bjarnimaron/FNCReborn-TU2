//! The setup window.

use crate::config;
use crate::pipeline::{self, GameSource, Options};
use crate::platform;
use crate::report::{Event, Reporter};
use eframe::egui::{self, Color32, RichText};
use std::collections::VecDeque;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{Receiver, channel};

#[derive(PartialEq, Clone, Copy)]
enum Mode {
    Install,
    Update,
}

#[derive(PartialEq, Clone, Copy)]
enum SourceKind {
    Iso,
    Folder,
}

pub struct SetupApp {
    mode: Mode,
    source_kind: SourceKind,
    source: String,
    dir: String,
    desktop: bool,
    accept: bool,
    running: bool,
    cancel: Arc<AtomicBool>,
    rx: Option<Receiver<Event>>,
    status: String,
    progress: Option<f32>,
    log: VecDeque<String>,
    result: Option<Result<String, String>>,
    show_log: bool,
    error: Option<String>,
}

impl SetupApp {
    pub fn new(update: bool, dir: Option<PathBuf>) -> Self {
        let dir = dir
            .or_else(here_install_dir)
            .unwrap_or_else(platform::default_install_dir);
        let existing = pipeline::is_install_folder(&dir);
        let licensed = crate::tools::root_for(&dir).read_dir().map(|rd| {
            rd.flatten()
                .any(|e| e.file_name().to_string_lossy().starts_with("winsysroot-"))
        });
        Self {
            mode: if update || existing {
                Mode::Update
            } else {
                Mode::Install
            },
            source_kind: SourceKind::Iso,
            source: String::new(),
            dir: dir.to_string_lossy().into_owned(),
            desktop: true,
            accept: licensed.unwrap_or(false),
            running: false,
            cancel: Arc::new(AtomicBool::new(false)),
            rx: None,
            status: String::new(),
            progress: None,
            log: VecDeque::new(),
            result: None,
            show_log: false,
            error: None,
        }
    }

    fn options(&self) -> Options {
        let src = self.source.trim().trim_matches('"').to_string();
        let game = if src.is_empty() {
            GameSource::None
        } else {
            match self.source_kind {
                SourceKind::Iso => GameSource::Iso(PathBuf::from(src)),
                SourceKind::Folder => GameSource::Folder(PathBuf::from(src)),
            }
        };
        Options {
            dir: PathBuf::from(self.dir.trim().trim_matches('"')),
            game,
            update: self.mode == Mode::Update,
            desktop_shortcut: self.desktop,
            integrate: true,
            accept_license: self.accept,
            force: false,
            fetch_source: true,
        }
    }

    fn start(&mut self, ctx: &egui::Context) {
        let mut o = self.options();
        // "Install" into a folder that already has FNC Reborn = update it.
        if o.update || pipeline::is_install_folder(&o.dir) {
            o.update = pipeline::is_install_folder(&o.dir);
        }
        if let Err(e) = pipeline::validate(&o) {
            self.error = Some(e.to_string());
            return;
        }
        self.error = None;
        self.result = None;
        self.log.clear();
        self.cancel.store(false, Ordering::Relaxed);
        let (tx, rx) = channel();
        self.rx = Some(rx);
        self.running = true;
        self.status = "Starting".into();
        let reporter = Reporter::new(Some(tx), self.cancel.clone(), false);
        let ctx = ctx.clone();
        std::thread::spawn(move || {
            let res = pipeline::run_all(&reporter, &o).map_err(|e| {
                let msg = format!("{e:#}");
                reporter.log(format!("ERROR: {msg}"));
                msg
            });
            reporter.close_log();
            reporter.finish(res);
            ctx.request_repaint();
        });
    }

    fn pump(&mut self) {
        let Some(rx) = &self.rx else { return };
        while let Ok(e) = rx.try_recv() {
            match e {
                Event::Log(l) => {
                    self.log.push_back(l);
                    while self.log.len() > 4000 {
                        self.log.pop_front();
                    }
                }
                Event::Status(s) => self.status = s,
                Event::Progress(p) => self.progress = p,
                Event::Finished(r) => {
                    self.running = false;
                    match &r {
                        Ok(s) => self.status = s.clone(),
                        Err(e) if e.contains("Cancelled") => self.status = "Cancelled.".into(),
                        Err(_) => {
                            self.status = "Setup failed.".into();
                            self.show_log = true;
                        }
                    }
                    self.result = Some(r);
                }
            }
        }
    }
}

/// When this program runs from inside an install folder, that folder.
fn here_install_dir() -> Option<PathBuf> {
    let me = std::env::current_exe().ok()?;
    let d = me.parent()?.to_path_buf();
    if pipeline::is_install_folder(&d) {
        return Some(d);
    }
    let p = d.parent()?.to_path_buf();
    pipeline::is_install_folder(&p).then_some(p)
}

impl eframe::App for SetupApp {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        self.pump();
        let ctx = ui.ctx().clone();
        if self.running {
            ctx.request_repaint_after(std::time::Duration::from_millis(100));
        }
        egui::CentralPanel::default().show_inside(ui, |ui| {
            ui.add_space(6.0);
            ui.heading(RichText::new("FNC Reborn").size(26.0).strong());
            ui.label(
                RichText::new(
                    "Builds the game on this PC from your own copy of Fight Night Champion. Nothing from the game is \
                     downloaded. Updates only rebuild what changed.",
                )
                .weak(),
            );
            ui.add_space(10.0);
            ui.add_enabled_ui(!self.running, |ui| {
                ui.horizontal(|ui| {
                    ui.radio_value(&mut self.mode, Mode::Install, "Install");
                    ui.radio_value(&mut self.mode, Mode::Update, "Update an existing install");
                });
                ui.add_space(8.0);

                let folder_label = if self.mode == Mode::Update {
                    "Your FNC Reborn folder (the one with the dist folder):"
                } else {
                    "Install folder:"
                };
                ui.label(folder_label);
                ui.horizontal(|ui| {
                    let w = ui.available_width() - 90.0;
                    ui.add(egui::TextEdit::singleline(&mut self.dir).desired_width(w));
                    if ui.button("Browse...").clicked()
                        && let Some(d) = rfd::FileDialog::new().set_title("Choose the FNC Reborn folder").pick_folder()
                    {
                        let d = if self.mode == Mode::Install && !pipeline::is_install_folder(&d) && !is_empty_dir(&d) {
                            d.join("FNCReborn")
                        } else {
                            d
                        };
                        if pipeline::is_install_folder(&d) {
                            self.mode = Mode::Update;
                        }
                        self.dir = d.to_string_lossy().into_owned();
                    }
                });
                ui.add_space(8.0);

                let game_there = PathBuf::from(self.dir.trim()).join("dist").join("game").join("default.xex").is_file();
                ui.label(if game_there {
                    "Your game files (already there; only needed to replace them):"
                } else {
                    "Your Fight Night Champion (Xbox 360) game:"
                });
                ui.horizontal(|ui| {
                    ui.radio_value(&mut self.source_kind, SourceKind::Iso, "Disc image (.iso)");
                    ui.radio_value(&mut self.source_kind, SourceKind::Folder, "Folder with the game files");
                });
                ui.horizontal(|ui| {
                    let w = ui.available_width() - 90.0;
                    let hint = match self.source_kind {
                        SourceKind::Iso => "Fight Night Champion .iso file",
                        SourceKind::Folder => "Folder that contains default.xex and databigs",
                    };
                    ui.add(egui::TextEdit::singleline(&mut self.source).hint_text(hint).desired_width(w));
                    if ui.button("Browse...").clicked() {
                        let picked = match self.source_kind {
                            SourceKind::Iso => rfd::FileDialog::new()
                                .set_title("Select your Fight Night Champion disc image")
                                .add_filter("Disc image", &["iso", "ISO"])
                                .pick_file(),
                            SourceKind::Folder => rfd::FileDialog::new()
                                .set_title("Select the folder with default.xex")
                                .pick_folder(),
                        };
                        if let Some(p) = picked {
                            self.source = p.to_string_lossy().into_owned();
                        }
                    }
                });
                ui.add_space(8.0);
                ui.checkbox(
                    &mut self.desktop,
                    if cfg!(windows) { "Desktop shortcut" } else { "Add to the application menu" },
                );
                ui.horizontal_wrapped(|ui| {
                    ui.checkbox(&mut self.accept, "");
                    ui.label("I accept the");
                    ui.hyperlink_to("Microsoft Visual Studio license terms", config::MS_LICENSE_URL);
                    ui.label(
                        "for the C++ headers and libraries Setup downloads (about 600 MB, no Visual Studio is installed).",
                    );
                });
            });

            ui.add_space(10.0);
            if let Some(e) = &self.error {
                ui.colored_label(Color32::from_rgb(230, 110, 90), e);
                ui.add_space(6.0);
            }
            ui.horizontal(|ui| {
                if self.running {
                    if ui.button("Cancel").clicked() {
                        self.cancel.store(true, Ordering::Relaxed);
                        self.status = "Cancelling...".into();
                    }
                } else {
                    let label = if self.mode == Mode::Update { "Update" } else { "Install" };
                    if ui.add(egui::Button::new(RichText::new(label).strong()).min_size([110.0, 30.0].into())).clicked() {
                        self.start(&ctx);
                    }
                    if matches!(self.result, Some(Ok(_))) && ui.add(egui::Button::new("Play").min_size([90.0, 30.0].into())).clicked() {
                        match platform::launch_game(&PathBuf::from(self.dir.trim())) {
                            Ok(()) => ctx.send_viewport_cmd(egui::ViewportCommand::Close),
                            Err(e) => self.error = Some(e.to_string()),
                        }
                    }
                }
                ui.toggle_value(&mut self.show_log, "Details");
                if ui.small_button("Project page").clicked() {
                    platform::open(config::REPO_PAGE);
                }
            });
            ui.add_space(6.0);
            if self.running || self.result.is_some() {
                ui.label(RichText::new(&self.status).strong());
                let bar = match self.progress {
                    Some(p) => egui::ProgressBar::new(p).show_percentage(),
                    None if self.running => egui::ProgressBar::new(0.0).animate(true),
                    None => egui::ProgressBar::new(if matches!(self.result, Some(Ok(_))) { 1.0 } else { 0.0 }),
                };
                ui.add(bar);
                if let Some(Err(e)) = &self.result
                    && !e.contains("Cancelled")
                {
                    ui.colored_label(Color32::from_rgb(230, 110, 90), e);
                    ui.label(
                        RichText::new(
                            "Fix the problem and press the button again; finished steps are skipped. \
                             The full log is setup-installer.log in the install folder.",
                        )
                        .weak(),
                    );
                }
            }
            if self.show_log {
                ui.add_space(6.0);
                egui::ScrollArea::vertical().auto_shrink(false).stick_to_bottom(true).show(ui, |ui| {
                    for l in &self.log {
                        ui.label(RichText::new(l).monospace().size(11.0));
                    }
                });
            }
        });
    }
}

fn is_empty_dir(d: &std::path::Path) -> bool {
    std::fs::read_dir(d)
        .map(|mut rd| rd.next().is_none())
        .unwrap_or(true)
}
