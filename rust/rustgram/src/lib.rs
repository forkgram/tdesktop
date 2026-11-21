pub mod downloads;

use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt, PermissionsExt};
use std::{
    env,
    error::Error,
    fs, io,
    path::{Path, PathBuf},
};

pub type Result<T> = std::result::Result<T, Box<dyn Error + Send + Sync>>;

pub fn data_dir() -> Result<PathBuf> {
    let base = match env::var_os("XDG_DATA_HOME") {
        Some(value) if Path::new(&value).is_absolute() => PathBuf::from(value),
        _ => PathBuf::from(env::var_os("HOME").ok_or("HOME is not set")?).join(".local/share"),
    };
    Ok(base.join("rustgram"))
}

pub fn private_session(dir: &Path) -> Result<PathBuf> {
    fs::create_dir_all(dir.parent().ok_or("invalid data directory")?)?;
    match fs::DirBuilder::new().mode(0o700).create(dir) {
        Ok(()) => (),
        Err(e) if e.kind() == io::ErrorKind::AlreadyExists => (),
        Err(e) => return Err(e.into()),
    }
    let meta = fs::symlink_metadata(dir)?;
    if !meta.is_dir() || meta.permissions().mode() & 0o077 != 0 {
        return Err("rustgram data directory must be a private directory (mode 0700)".into());
    }
    let path = dir.join("session.sqlite3");
    match fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&path)
    {
        Ok(_) => (),
        Err(e) if e.kind() == io::ErrorKind::AlreadyExists => {
            let meta = fs::symlink_metadata(&path)?;
            if !meta.is_file() || meta.permissions().mode() & 0o077 != 0 {
                return Err("session must be a private regular file (mode 0600)".into());
            }
        }
        Err(e) => return Err(e.into()),
    }
    Ok(path)
}
