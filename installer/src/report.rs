//! Progress reporting, cancelling, running programs and downloading files.

use anyhow::{Context, Result, anyhow, bail};
use sha2::{Digest, Sha256};
use std::ffi::OsString;
use std::fs::{self, File, OpenOptions};
use std::io::{BufRead, BufReader, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::Sender;
use std::sync::{Arc, Mutex};
use std::time::Duration;

pub enum Event {
    Log(String),
    Status(String),
    /// 0.0..=1.0, or None for "busy, unknown length".
    Progress(Option<f32>),
    Finished(Result<String, String>),
}

/// Shared by the worker thread and the window (or the console).
#[derive(Clone)]
pub struct Reporter {
    tx: Option<Sender<Event>>,
    log_file: Arc<Mutex<Option<File>>>,
    cancel: Arc<AtomicBool>,
    console: bool,
}

impl Reporter {
    pub fn new(tx: Option<Sender<Event>>, cancel: Arc<AtomicBool>, console: bool) -> Self {
        Self {
            tx,
            log_file: Arc::new(Mutex::new(None)),
            cancel,
            console,
        }
    }

    pub fn open_log(&self, path: &Path) {
        if let Ok(f) = OpenOptions::new().create(true).append(true).open(path) {
            *self.log_file.lock().unwrap() = Some(f);
        }
    }

    pub fn close_log(&self) {
        *self.log_file.lock().unwrap() = None;
    }

    fn send(&self, e: Event) {
        if let Some(tx) = &self.tx {
            let _ = tx.send(e);
        }
    }

    pub fn log(&self, line: impl Into<String>) {
        let line = line.into();
        if let Some(f) = self.log_file.lock().unwrap().as_mut() {
            let _ = writeln!(f, "{line}");
        }
        if self.console {
            println!("{line}");
        }
        self.send(Event::Log(line));
    }

    pub fn status(&self, text: impl Into<String>) {
        let text = text.into();
        self.log(format!("== {text} =="));
        self.send(Event::Status(text));
        self.progress(None);
    }

    pub fn progress(&self, p: Option<f32>) {
        self.send(Event::Progress(p));
    }

    pub fn finish(&self, r: Result<String, String>) {
        self.send(Event::Finished(r));
    }

    pub fn cancelled(&self) -> bool {
        self.cancel.load(Ordering::Relaxed)
    }

    pub fn check(&self) -> Result<()> {
        if self.cancelled() {
            bail!("Cancelled.")
        } else {
            Ok(())
        }
    }
}

/// How child programs are started: PATH with the portable tools first and
/// nothing that could make them pick up a Visual Studio install.
#[derive(Clone, Default)]
pub struct Env {
    pub path_prefix: Vec<PathBuf>,
    pub vars: Vec<(String, OsString)>,
}

impl Env {
    pub fn command(&self, exe: impl AsRef<std::ffi::OsStr>) -> Command {
        let mut c = Command::new(exe);
        let mut parts: Vec<PathBuf> = self.path_prefix.clone();
        if let Some(p) = std::env::var_os("PATH") {
            for d in std::env::split_paths(&p) {
                if cfg!(windows) && d.to_string_lossy().contains("Microsoft Visual Studio") {
                    continue;
                }
                parts.push(d);
            }
        }
        if let Ok(p) = std::env::join_paths(parts) {
            c.env("PATH", p);
        }
        for v in [
            "INCLUDE",
            "LIB",
            "LIBPATH",
            "VCINSTALLDIR",
            "VCToolsInstallDir",
            "VSINSTALLDIR",
            "WindowsSdkDir",
            "UniversalCRTSdkDir",
            "CC",
            "CXX",
            "CFLAGS",
            "CXXFLAGS",
            "LDFLAGS",
        ] {
            c.env_remove(v);
        }
        c.env("GIT_TERMINAL_PROMPT", "0");
        // Git refuses folders it can't prove belong to this Windows user
        // ("detected dubious ownership"): exFAT / FAT32 / network drives, or an
        // install folder made by another account. Setup only runs git in its
        // own install folder, so every folder is trusted for these commands.
        c.env("GIT_CONFIG_COUNT", "1");
        c.env("GIT_CONFIG_KEY_0", "safe.directory");
        c.env("GIT_CONFIG_VALUE_0", "*");
        for (k, v) in &self.vars {
            c.env(k, v);
        }
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            c.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
        }
        #[cfg(unix)]
        {
            use std::os::unix::process::CommandExt;
            c.process_group(0);
        }
        c
    }
}

fn kill_tree(child: &mut Child) {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        let _ = Command::new("taskkill")
            .args(["/T", "/F", "/PID", &child.id().to_string()])
            .creation_flags(0x0800_0000)
            .status();
    }
    #[cfg(unix)]
    {
        let _ = Command::new("kill")
            .args(["-TERM", &format!("-{}", child.id())])
            .status();
    }
    let _ = child.kill();
}

/// Runs a program, sending every output line to the log. `on_line` sees each
/// line too (used for build progress). Fails on a non-zero exit code.
pub fn run(
    r: &Reporter,
    mut cmd: Command,
    what: &str,
    mut on_line: impl FnMut(&str),
) -> Result<()> {
    let code = run_code(r, &mut cmd, &mut on_line)?;
    if code != 0 {
        bail!("{what} failed (exit code {code}). The log above says why.");
    }
    Ok(())
}

pub fn run_code(r: &Reporter, cmd: &mut Command, on_line: &mut impl FnMut(&str)) -> Result<i32> {
    r.check()?;
    r.log(format!("> {}", describe(cmd)));
    cmd.stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    let mut child = cmd
        .spawn()
        .with_context(|| format!("could not start {}", cmd.get_program().to_string_lossy()))?;
    let (tx, rx) = std::sync::mpsc::channel::<String>();
    let mut readers = Vec::new();
    for pipe in [
        child
            .stdout
            .take()
            .map(|p| Box::new(p) as Box<dyn Read + Send>),
        child
            .stderr
            .take()
            .map(|p| Box::new(p) as Box<dyn Read + Send>),
    ]
    .into_iter()
    .flatten()
    {
        let tx = tx.clone();
        readers.push(std::thread::spawn(move || {
            let mut reader = BufReader::new(pipe);
            let mut buf = Vec::new();
            loop {
                buf.clear();
                match reader.read_until(b'\n', &mut buf) {
                    Ok(0) | Err(_) => break,
                    Ok(_) => {
                        let s = String::from_utf8_lossy(&buf);
                        for part in s.split('\r') {
                            let t = part.trim_end_matches(['\n']);
                            if !t.trim().is_empty() {
                                let _ = tx.send(t.to_string());
                            }
                        }
                    }
                }
            }
        }));
    }
    drop(tx);
    let status = loop {
        while let Ok(line) = rx.try_recv() {
            on_line(&line);
            r.log(line);
        }
        if r.cancelled() {
            kill_tree(&mut child);
            let _ = child.wait();
            bail!("Cancelled.");
        }
        if let Some(s) = child.try_wait()? {
            break s;
        }
        std::thread::sleep(Duration::from_millis(80));
    };
    for t in readers {
        let _ = t.join();
    }
    while let Ok(line) = rx.try_recv() {
        on_line(&line);
        r.log(line);
    }
    Ok(status.code().unwrap_or(-1))
}

/// Runs a program and returns its standard output (no logging), or None.
pub fn capture(cmd: &mut Command) -> Option<String> {
    cmd.stdin(Stdio::null()).stderr(Stdio::null());
    let out = cmd.output().ok()?;
    if !out.status.success() {
        return None;
    }
    Some(String::from_utf8_lossy(&out.stdout).trim().to_string())
}

fn describe(cmd: &Command) -> String {
    let mut s = cmd.get_program().to_string_lossy().into_owned();
    for a in cmd.get_args() {
        let a = a.to_string_lossy();
        if a.contains(' ') {
            s.push_str(&format!(" \"{a}\""));
        } else {
            s.push(' ');
            s.push_str(&a);
        }
    }
    s
}

/// ninja's "[12/345]" progress.
pub fn ninja_progress(line: &str) -> Option<(u32, u32)> {
    let rest = line.strip_prefix('[')?;
    let end = rest.find(']')?;
    let (a, b) = rest[..end].split_once('/')?;
    Some((a.trim().parse().ok()?, b.trim().parse().ok()?))
}

fn agent() -> ureq::Agent {
    ureq::AgentBuilder::new()
        .timeout_connect(Duration::from_secs(30))
        .timeout_read(Duration::from_secs(90))
        .user_agent(&format!("FNCReborn-Setup/{}", crate::config::APP_VERSION))
        .build()
}

pub fn sha256_file(path: &Path) -> Result<String> {
    let mut f = File::open(path)?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(hex(&h.finalize()))
}

pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// Downloads `url` to `dest` with progress and up to 3 attempts. Resumes a
/// partial download (`dest.part`) when the server allows it, which matters on
/// slow connections. With `sha256`, the file must match it.
pub fn download(r: &Reporter, url: &str, dest: &Path, sha256: Option<&str>) -> Result<()> {
    if let Some(want) = sha256
        && dest.exists()
        && sha256_file(dest)? == want
    {
        return Ok(());
    }
    r.log(format!("Downloading {url}"));
    let part = PathBuf::from(format!("{}.part", dest.display()));
    let agent = agent();
    let mut last_err = anyhow!("download failed");
    for attempt in 1..=4 {
        r.check()?;
        let have = fs::metadata(&part).map(|m| m.len()).unwrap_or(0);
        let mut req = agent.get(url);
        if have > 0 {
            req = req.set("Range", &format!("bytes={have}-"));
        }
        let result = (|| -> Result<()> {
            let resp = req.call().map_err(|e| anyhow!("{e}"))?;
            let resumed = resp.status() == 206;
            let len: Option<u64> = resp.header("Content-Length").and_then(|v| v.parse().ok());
            let (mut file, mut done) = if resumed {
                (OpenOptions::new().append(true).open(&part)?, have)
            } else {
                (File::create(&part)?, 0)
            };
            let total = len.map(|l| l + done);
            let mut reader = resp.into_reader();
            let mut buf = vec![0u8; 1 << 16];
            let mut last_pct = -1i32;
            loop {
                if r.cancelled() {
                    bail!("Cancelled.");
                }
                let n = reader.read(&mut buf)?;
                if n == 0 {
                    break;
                }
                file.write_all(&buf[..n])?;
                done += n as u64;
                if let Some(t) = total
                    && t > 0
                {
                    let pct = (done * 1000 / t) as i32;
                    if pct != last_pct {
                        last_pct = pct;
                        r.progress(Some(done as f32 / t as f32));
                    }
                }
            }
            if let Some(t) = total
                && done != t
            {
                bail!("the download was cut short ({done} of {t} bytes)");
            }
            Ok(())
        })();
        match result {
            Ok(()) => {
                if let Some(want) = sha256 {
                    let got = sha256_file(&part)?;
                    if got != want {
                        let _ = fs::remove_file(&part);
                        last_err = anyhow!("the file is damaged (checksum {got}, expected {want})");
                        r.log(format!("Download check failed: {last_err}"));
                        continue;
                    }
                }
                if dest.exists() {
                    fs::remove_file(dest)?;
                }
                fs::rename(&part, dest)?;
                return Ok(());
            }
            Err(e) => {
                if r.cancelled() {
                    bail!("Cancelled.");
                }
                r.log(format!("Download attempt {attempt} failed: {e}"));
                last_err = e;
                std::thread::sleep(Duration::from_secs(3));
            }
        }
    }
    Err(last_err.context(format!(
        "Downloading {url} failed. Check your internet connection and press Install again; \
         the part already downloaded is kept."
    )))
}
