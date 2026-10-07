//! Things that differ between Windows and Linux.

#[cfg(windows)]
use crate::report::{Env, run_code};
use crate::report::{Reporter, capture};
use anyhow::{Result, bail};
use std::fs;
use std::path::{Path, PathBuf};

pub const EXE: &str = if cfg!(windows) { ".exe" } else { "" };

/// Where the toolchain is kept, shared by every install of this user.
pub fn data_dir() -> PathBuf {
    #[cfg(windows)]
    {
        let base = std::env::var_os("LOCALAPPDATA")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("C:\\"));
        base.join("FNCReborn")
    }
    #[cfg(not(windows))]
    {
        if let Some(x) = std::env::var_os("XDG_DATA_HOME") {
            return PathBuf::from(x).join("FNCReborn");
        }
        home().join(".local/share/FNCReborn")
    }
}

#[cfg(not(windows))]
pub fn home() -> PathBuf {
    std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp"))
}

pub fn default_install_dir() -> PathBuf {
    if let Some(d) = remembered_install_dir() {
        return d;
    }
    #[cfg(windows)]
    {
        let drive = std::env::var("SystemDrive").unwrap_or_else(|_| "C:".into());
        PathBuf::from(format!("{drive}\\Games\\FNCReborn"))
    }
    #[cfg(not(windows))]
    {
        home().join("Games/FNCReborn")
    }
}

pub fn remembered_install_dir() -> Option<PathBuf> {
    #[cfg(windows)]
    {
        let out = capture(std::process::Command::new("reg").args([
            "query",
            "HKCU\\Software\\FNCReborn",
            "/v",
            "InstallDir",
        ]))?;
        let line = out.lines().find(|l| l.contains("InstallDir"))?;
        let value = line.split("REG_SZ").nth(1)?.trim();
        let p = PathBuf::from(value);
        crate::pipeline::is_install_folder(&p).then_some(p)
    }
    #[cfg(not(windows))]
    {
        let p = PathBuf::from(crate::fsx::read_text(&data_dir().join("install_dir.txt"))?);
        crate::pipeline::is_install_folder(&p).then_some(p)
    }
}

pub fn remember_install_dir(dir: &Path) {
    #[cfg(windows)]
    {
        let _ = capture(std::process::Command::new("reg").args([
            "add",
            "HKCU\\Software\\FNCReborn",
            "/v",
            "InstallDir",
            "/t",
            "REG_SZ",
            "/d",
            &dir.to_string_lossy(),
            "/f",
        ]));
    }
    #[cfg(not(windows))]
    {
        let _ = crate::fsx::write_text(&data_dir().join("install_dir.txt"), &dir.to_string_lossy());
    }
}

/// x86-64 level the recompiled code is built for: what this PC supports.
pub fn cpu_level() -> &'static str {
    #[cfg(target_arch = "x86_64")]
    {
        let v3 = std::arch::is_x86_feature_detected!("avx2")
            && std::arch::is_x86_feature_detected!("bmi1")
            && std::arch::is_x86_feature_detected!("bmi2")
            && std::arch::is_x86_feature_detected!("lzcnt")
            && std::arch::is_x86_feature_detected!("movbe")
            && std::arch::is_x86_feature_detected!("fma")
            && std::arch::is_x86_feature_detected!("f16c");
        if v3 {
            return "v3";
        }
        if std::arch::is_x86_feature_detected!("sse4.2")
            && std::arch::is_x86_feature_detected!("popcnt")
        {
            return "v2";
        }
        "sse41"
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        "v2"
    }
}

pub fn ram_gb() -> u64 {
    #[cfg(windows)]
    unsafe {
        use windows_sys::Win32::System::SystemInformation::{GlobalMemoryStatusEx, MEMORYSTATUSEX};
        let mut m: MEMORYSTATUSEX = std::mem::zeroed();
        m.dwLength = std::mem::size_of::<MEMORYSTATUSEX>() as u32;
        if GlobalMemoryStatusEx(&mut m) != 0 {
            return m.ullTotalPhys / (1 << 30);
        }
        8
    }
    #[cfg(not(windows))]
    {
        fs::read_to_string("/proc/meminfo")
            .ok()
            .and_then(|s| {
                s.lines()
                    .find(|l| l.starts_with("MemTotal:"))
                    .and_then(|l| l.split_whitespace().nth(1)?.parse::<u64>().ok())
            })
            .map(|kb| kb / (1 << 20))
            .unwrap_or(8)
    }
}

/// Free space in GB on the drive holding `dir` (or its nearest existing parent).
pub fn free_gb(dir: &Path) -> Option<u64> {
    let mut d = dir.to_path_buf();
    while !d.exists() {
        d = d.parent()?.to_path_buf();
    }
    #[cfg(windows)]
    unsafe {
        use std::os::windows::ffi::OsStrExt;
        use windows_sys::Win32::Storage::FileSystem::GetDiskFreeSpaceExW;
        let w: Vec<u16> = d.as_os_str().encode_wide().chain([0]).collect();
        let mut free = 0u64;
        if GetDiskFreeSpaceExW(
            w.as_ptr(),
            &mut free,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        ) != 0
        {
            return Some(free / (1 << 30));
        }
        None
    }
    #[cfg(not(windows))]
    {
        let out = capture(std::process::Command::new("df").arg("-Pk").arg(&d))?;
        let kb: u64 = out
            .lines()
            .nth(1)?
            .split_whitespace()
            .nth(3)?
            .parse()
            .ok()?;
        Some(kb / (1 << 20))
    }
}

/// Lets console output work when started from a command prompt (the program
/// is a window program, which has no console of its own).
pub fn attach_console() {
    #[cfg(windows)]
    unsafe {
        use windows_sys::Win32::System::Console::{ATTACH_PARENT_PROCESS, AttachConsole};
        AttachConsole(ATTACH_PARENT_PROCESS);
    }
}

#[cfg(windows)]
fn ps_quote(p: &Path) -> String {
    format!("'{}'", p.to_string_lossy().replace('\'', "''"))
}

#[cfg(windows)]
fn powershell(r: &Reporter, script: &str) -> Result<i32> {
    let mut cmd = Env::default().command("powershell.exe");
    cmd.args([
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-Command",
        script,
    ]);
    run_code(r, &mut cmd, &mut |_: &str| {})
}

/// Start menu / desktop shortcuts on Windows; an application-menu entry on Linux.
pub fn create_shortcuts(r: &Reporter, dir: &Path, desktop: bool, setup_copy: Option<&Path>) {
    let dist = dir.join("dist");
    let target = dist.join("FNCReborn.exe");
    #[cfg(windows)]
    {
        let mut s = String::from(
            "$ErrorActionPreference='Stop'; $sh = New-Object -ComObject WScript.Shell;\n\
             function L($lnk,$t,$w,$d,$a){ $s=$sh.CreateShortcut($lnk); $s.TargetPath=$t; if($a){$s.Arguments=$a}; \
             $s.WorkingDirectory=$w; $s.Description=$d; $s.IconLocation=\"$t,0\"; $s.Save() }\n\
             $menu = Join-Path ([Environment]::GetFolderPath('Programs')) 'FNC Reborn'\n\
             New-Item -ItemType Directory -Force $menu | Out-Null\n",
        );
        s += &format!(
            "L (Join-Path $menu 'FNC Reborn.lnk') {} {} 'Play FNC Reborn' ''\n",
            ps_quote(&target),
            ps_quote(&dist)
        );
        if let Some(setup) = setup_copy {
            s += &format!(
                "L (Join-Path $menu 'Update FNC Reborn.lnk') {} {} 'Get the latest FNC Reborn update' '--update'\n",
                ps_quote(setup),
                ps_quote(dir)
            );
        }
        if desktop {
            s += &format!(
                "L (Join-Path ([Environment]::GetFolderPath('DesktopDirectory')) 'FNC Reborn.lnk') {} {} 'Play FNC Reborn' ''\n",
                ps_quote(&target),
                ps_quote(&dist)
            );
        }
        if !matches!(powershell(r, &s), Ok(0)) {
            r.log("Could not create the shortcuts (the game itself is fine).");
        }
    }
    #[cfg(not(windows))]
    {
        let _ = desktop;
        let runner = ["umu-run", "wine"].into_iter().find(|t| which(t).is_some());
        let apps = home().join(".local/share/applications");
        let exec = match runner {
            Some(t) => format!(
                "sh -c 'cd \"{}\" && {} \"{}\"'",
                dist.display(),
                t,
                target.display()
            ),
            None => {
                r.log(
                    "Neither umu-run nor wine was found, so no menu entry was made. In Steam: Add a Game > \
                     Add a Non-Steam Game > choose dist/FNCReborn.exe, then Properties > Compatibility > \
                     Force Proton.",
                );
                return;
            }
        };
        let mut text = format!(
            "[Desktop Entry]\nType=Application\nName=FNC Reborn\nComment=Play FNC Reborn\nExec={exec}\n\
             Path={}\nCategories=Game;\nTerminal=false\n",
            dist.display()
        );
        let icon = dir.join("project/res/FNCReborn.png");
        if icon.exists() {
            text += &format!("Icon={}\n", icon.display());
        }
        let _ = fs::create_dir_all(&apps);
        if fs::write(apps.join("fncreborn.desktop"), text).is_err() {
            r.log("Could not create the application menu entry (the game itself is fine).");
        }
        if let Some(setup) = setup_copy {
            let t = format!(
                "[Desktop Entry]\nType=Application\nName=Update FNC Reborn\nExec=\"{}\" --update\nPath={}\nCategories=Game;\nTerminal=false\n",
                setup.display(),
                dir.display()
            );
            let _ = fs::write(apps.join("fncreborn-update.desktop"), t);
        }
    }
}

#[cfg(not(windows))]
pub fn which(tool: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    std::env::split_paths(&path)
        .map(|d| d.join(tool))
        .find(|p| p.is_file())
}

/// Visual C++ runtime (vcruntime140.dll etc.), which the game needs on Windows.
#[cfg(windows)]
pub fn ensure_vc_runtime(r: &Reporter, downloads: &Path) -> Result<()> {
    r.status("Checking the Visual C++ runtime");
    let out = capture(std::process::Command::new("reg").args([
        "query",
        "HKLM\\SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64",
        "/reg:64",
    ]))
    .unwrap_or_default();
    let val = |name: &str| -> Option<u32> {
        let l = out
            .lines()
            .find(|l| l.split_whitespace().next() == Some(name))?;
        let v = l.split_whitespace().last()?;
        u32::from_str_radix(v.trim_start_matches("0x"), 16).ok()
    };
    if val("Installed") == Some(1)
        && val("Major").unwrap_or(0) >= 14
        && val("Minor").unwrap_or(0) >= 40
    {
        r.log("Visual C++ runtime: installed");
        return Ok(());
    }
    let exe = downloads.join("vc_redist.x64.exe");
    r.status("Downloading the Visual C++ runtime");
    crate::report::download(r, crate::config::VC_REDIST_URL, &exe, None)?;
    r.status("Installing the Visual C++ runtime (Windows asks for permission)");
    let code = powershell(
        r,
        &format!(
            "try {{ $p = Start-Process -FilePath {} -ArgumentList '/install','/passive','/norestart' -Verb RunAs -Wait -PassThru; exit $p.ExitCode }} catch {{ exit 1223 }}",
            ps_quote(&exe)
        ),
    )?;
    let _ = fs::remove_file(&exe);
    match code {
        0 | 3010 | 1638 | 1641 => Ok(()),
        1223 => bail!(
            "Installing the Visual C++ runtime needs administrator permission. Press Install again and allow it."
        ),
        c => bail!("Installing the Visual C++ runtime failed (exit code {c})."),
    }
}

/// Opens a folder, file or web page with the system's default program.
pub fn open(target: &str) {
    #[cfg(windows)]
    {
        let _ = std::process::Command::new("explorer").arg(target).spawn();
    }
    #[cfg(not(windows))]
    {
        let _ = std::process::Command::new("xdg-open").arg(target).spawn();
    }
}

/// Starts the game (through umu-run / wine on Linux).
pub fn launch_game(dir: &Path) -> Result<()> {
    let dist = dir.join("dist");
    let exe = dist.join("FNCReborn.exe");
    #[cfg(windows)]
    {
        std::process::Command::new(&exe)
            .current_dir(&dist)
            .spawn()?;
    }
    #[cfg(not(windows))]
    {
        let Some(t) = ["umu-run", "wine"].into_iter().find(|t| which(t).is_some()) else {
            bail!(
                "Start the game from Steam with Proton (see the log), or install umu-launcher or wine."
            );
        };
        std::process::Command::new(t)
            .arg(&exe)
            .current_dir(&dist)
            .spawn()?;
    }
    Ok(())
}
