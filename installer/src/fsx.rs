//! File helpers: unpacking archives and copying folders.

use crate::report::Reporter;
use anyhow::{Context, Result, bail};
use std::fs::{self, File};
use std::io;
use std::path::{Component, Path, PathBuf};

/// Unpacks a .zip. With `strip`, the archive's single top folder is dropped.
pub fn unzip(archive: &Path, dest: &Path, strip: bool) -> Result<()> {
    let mut zip = zip::ZipArchive::new(File::open(archive)?)
        .with_context(|| format!("{} is not a valid zip file", archive.display()))?;
    fs::create_dir_all(dest)?;
    for i in 0..zip.len() {
        let mut entry = zip.by_index(i)?;
        let Some(rel) = entry.enclosed_name() else {
            continue;
        };
        let rel: PathBuf = if strip {
            let mut c = rel.components();
            c.next();
            c.as_path().to_path_buf()
        } else {
            rel
        };
        if rel.as_os_str().is_empty() {
            continue;
        }
        let out = dest.join(&rel);
        if entry.is_dir() {
            fs::create_dir_all(&out)?;
            continue;
        }
        if let Some(p) = out.parent() {
            fs::create_dir_all(p)?;
        }
        let mut f = File::create(&out)?;
        io::copy(&mut entry, &mut f)?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = entry.unix_mode().unwrap_or(0o644);
            let exec = mode & 0o111 != 0 || rel.extension().is_none();
            fs::set_permissions(
                &out,
                fs::Permissions::from_mode(if exec { 0o755 } else { 0o644 }),
            )?;
        }
    }
    Ok(())
}

/// Unpacks a .tar.xz / .tar.gz here (no external tar: the one in some
/// Windows versions can't read .xz). With `strip`, the archive's top folder is
/// dropped; `keep` chooses which (stripped) paths are written.
pub fn untar(
    r: &Reporter,
    archive: &Path,
    dest: &Path,
    strip: bool,
    keep: &dyn Fn(&Path) -> bool,
) -> Result<()> {
    struct Counting<R> {
        inner: R,
        read: u64,
        total: u64,
        last: u64,
        r: Reporter,
    }
    impl<R: io::Read> io::Read for Counting<R> {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            let n = self.inner.read(buf)?;
            self.read += n as u64;
            if self.total > 0 && self.read - self.last > (8 << 20) {
                self.last = self.read;
                self.r.progress(Some(self.read as f32 / self.total as f32));
            }
            Ok(n)
        }
    }
    fs::create_dir_all(dest)?;
    let total = fs::metadata(archive)?.len();
    let file = Counting {
        inner: io::BufReader::with_capacity(1 << 20, File::open(archive)?),
        read: 0,
        total,
        last: 0,
        r: r.clone(),
    };
    let name = archive.to_string_lossy().to_ascii_lowercase();
    let reader: Box<dyn io::Read> = if name.ends_with(".xz") {
        Box::new(xz2::read::XzDecoder::new_multi_decoder(file))
    } else if name.ends_with(".gz") || name.ends_with(".tgz") {
        Box::new(flate2::read::GzDecoder::new(file))
    } else {
        Box::new(file)
    };
    let mut tar = tar::Archive::new(reader);
    tar.set_preserve_permissions(true);
    let mut links: Vec<(PathBuf, PathBuf)> = Vec::new();
    for entry in tar
        .entries()
        .with_context(|| format!("{} is not a valid archive", archive.display()))?
    {
        r.check()?;
        let mut entry = entry.with_context(|| format!("{} is damaged", archive.display()))?;
        let path = entry.path()?.into_owned();
        let rel: PathBuf = if strip {
            path.components().skip(1).collect()
        } else {
            path
        };
        if rel.as_os_str().is_empty()
            || safe_relative(&rel.to_string_lossy()).is_none()
            || !keep(&rel)
        {
            continue;
        }
        let out = dest.join(&rel);
        let kind = entry.header().entry_type();
        if kind.is_dir() {
            fs::create_dir_all(&out)?;
            continue;
        }
        if let Some(p) = out.parent() {
            fs::create_dir_all(p)?;
        }
        if kind.is_hard_link() {
            // Hard links name the target by its path inside the archive.
            if let Some(target) = entry.link_name()? {
                let t: PathBuf = if strip {
                    target.components().skip(1).collect()
                } else {
                    target.into_owned()
                };
                links.push((dest.join(t), out));
            }
            continue;
        }
        entry
            .unpack(&out)
            .with_context(|| format!("could not write {} (is the disk full?)", out.display()))?;
    }
    for (target, out) in links {
        if target.exists() {
            fs::copy(&target, &out)?;
        }
    }
    Ok(())
}

/// Deletes a folder, also when files in it are read-only (git objects).
pub fn remove_dir(dir: &Path) -> Result<()> {
    if !dir.exists() {
        return Ok(());
    }
    if fs::remove_dir_all(dir).is_ok() {
        return Ok(());
    }
    clear_readonly(dir);
    fs::remove_dir_all(dir).with_context(|| format!("could not delete {}", dir.display()))
}

fn clear_readonly(dir: &Path) {
    let Ok(rd) = fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        if let Ok(m) = fs::symlink_metadata(&p) {
            if m.is_dir() {
                clear_readonly(&p);
            } else {
                let mut perm = m.permissions();
                #[allow(clippy::permissions_set_readonly_false)]
                perm.set_readonly(false);
                let _ = fs::set_permissions(&p, perm);
            }
        }
    }
}

/// Copies a folder tree. `skip(name)` leaves out files and folders by name.
pub fn copy_tree(
    r: &Reporter,
    from: &Path,
    to: &Path,
    skip: &dyn Fn(&str, bool) -> bool,
    count: &mut u64,
) -> Result<()> {
    fs::create_dir_all(to)?;
    for e in fs::read_dir(from).with_context(|| format!("could not read {}", from.display()))? {
        r.check()?;
        let e = e?;
        let name = e.file_name().to_string_lossy().into_owned();
        let ty = e.file_type()?;
        if skip(&name, ty.is_dir()) {
            continue;
        }
        let dest = to.join(&name);
        if ty.is_dir() {
            copy_tree(r, &e.path(), &dest, skip, count)?;
        } else {
            fs::copy(e.path(), &dest).with_context(|| {
                format!(
                    "could not copy {} to {}",
                    e.path().display(),
                    dest.display()
                )
            })?;
            *count += 1;
        }
    }
    Ok(())
}

/// True when `child` is inside `root` (no "..", no absolute part).
pub fn safe_relative(rel: &str) -> Option<PathBuf> {
    let p = Path::new(rel);
    let mut out = PathBuf::new();
    for c in p.components() {
        match c {
            Component::Normal(s) => out.push(s),
            Component::CurDir => {}
            _ => return None,
        }
    }
    if out.as_os_str().is_empty() {
        None
    } else {
        Some(out)
    }
}

pub fn write_text(path: &Path, text: &str) -> Result<()> {
    if let Some(p) = path.parent() {
        fs::create_dir_all(p)?;
    }
    fs::write(path, text).with_context(|| format!("could not write {}", path.display()))
}

pub fn read_text(path: &Path) -> Option<String> {
    fs::read_to_string(path).ok().map(|s| s.trim().to_string())
}

pub fn require(path: &Path, what: &str) -> Result<()> {
    if !path.exists() {
        bail!("{what} is missing: {}", path.display());
    }
    Ok(())
}
