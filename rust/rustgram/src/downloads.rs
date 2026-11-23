//! Private, bounded streaming downloads. Existing files are never overwritten.
use std::{
    fs, io,
    os::unix::fs::{DirBuilderExt, OpenOptionsExt, PermissionsExt},
    path::{Path, PathBuf},
};
use tokio::io::AsyncWriteExt;

pub const MAX_BYTES: u64 = 512 * 1024 * 1024;

/// Convert an untrusted remote filename into a single, bounded path component.
pub fn safe_filename(name: &str) -> String {
    let mut result = String::new();
    for ch in name.chars() {
        let safe = ch.is_alphanumeric() || matches!(ch, '.' | '-' | '_' | ' ');
        let ch = if safe { ch } else { '_' };
        // Linux filesystems usually limit one component to 255 bytes. Leave
        // space for message IDs, a collision suffix, and the temporary marker.
        if result.len() + ch.len_utf8() > 120 {
            break;
        }
        result.push(ch);
    }
    let trimmed = result.trim_matches(|c: char| c == '.' || c.is_whitespace());
    if trimmed.is_empty() {
        "attachment.bin".into()
    } else {
        trimmed.to_owned()
    }
}

pub fn private_dir(parent: &Path) -> io::Result<PathBuf> {
    let dir = parent.join("downloads");
    match fs::DirBuilder::new().mode(0o700).create(&dir) {
        Ok(()) => (),
        Err(e) if e.kind() == io::ErrorKind::AlreadyExists => (),
        Err(e) => return Err(e),
    }
    let meta = fs::symlink_metadata(&dir)?;
    if !meta.is_dir() || meta.permissions().mode() & 0o077 != 0 {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "download directory must be a private directory",
        ));
    }
    Ok(dir)
}

struct Partial(PathBuf);
impl Drop for Partial {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.0);
    }
}

/// A temporary file in a private directory, linked into place on completion.
pub struct DownloadFile {
    partial: Partial,
    destination: PathBuf,
    file: tokio::fs::File,
    written: u64,
    max_bytes: u64,
}
impl DownloadFile {
    pub fn create(
        dir: &Path,
        chat_id: i64,
        message_id: i32,
        filename: &str,
        max_bytes: u64,
    ) -> io::Result<Self> {
        let filename = safe_filename(filename);
        for suffix in 0..10_000u32 {
            let stem = format!("{chat_id}_{message_id}_{suffix}_{filename}");
            let destination = dir.join(&stem);
            // The hard link in `finish` performs the authoritative non-overwrite check.
            if fs::symlink_metadata(&destination).is_ok() {
                continue;
            }
            let partial = dir.join(format!(".{stem}.part"));
            match fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .mode(0o600)
                .open(&partial)
            {
                Ok(file) => {
                    return Ok(Self {
                        partial: Partial(partial),
                        destination,
                        file: tokio::fs::File::from_std(file),
                        written: 0,
                        max_bytes,
                    })
                }
                Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(e) => return Err(e),
            }
        }
        Err(io::Error::new(
            io::ErrorKind::AlreadyExists,
            "no free download filename",
        ))
    }
    pub async fn write_chunk(&mut self, chunk: &[u8]) -> io::Result<()> {
        let next = self
            .written
            .checked_add(chunk.len() as u64)
            .ok_or_else(|| io::Error::other("download size overflow"))?;
        if next > self.max_bytes {
            return Err(io::Error::other("attachment exceeds size limit"));
        }
        self.file.write_all(chunk).await?;
        self.written = next;
        Ok(())
    }
    pub async fn finish(self, expected_bytes: Option<u64>) -> io::Result<PathBuf> {
        if expected_bytes.is_some_and(|expected| expected != self.written) {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "download size does not match attachment metadata",
            ));
        }
        self.file.sync_all().await?;
        // hard_link fails if the destination already exists, including a symlink.
        fs::hard_link(&self.partial.0, &self.destination)?;
        Ok(self.destination.clone())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp() -> PathBuf {
        let path = std::env::temp_dir().join(format!(
            "rustgram-download-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir(&path).unwrap();
        path
    }
    #[test]
    fn filename_is_a_safe_single_component() {
        assert_eq!(safe_filename("../../important"), "_.._important");
        assert_eq!(safe_filename("..."), "attachment.bin");
        assert!(!safe_filename("a\\b/c\n.txt").contains('/'));
        assert!(safe_filename(&"ж".repeat(500)).len() <= 120);
    }
    #[tokio::test(flavor = "current_thread")]
    async fn existing_files_are_preserved_and_new_download_is_private() {
        let root = temp();
        let dir = private_dir(&root).unwrap();
        let existing = dir.join("1_2_0_photo.jpg");
        fs::write(&existing, b"old").unwrap();
        let mut download = DownloadFile::create(&dir, 1, 2, "photo.jpg", 10).unwrap();
        download.write_chunk(b"new").await.unwrap();
        let output = download.finish(Some(3)).await.unwrap();
        assert_eq!(fs::read(existing).unwrap(), b"old");
        assert_eq!(fs::read(&output).unwrap(), b"new");
        assert_eq!(
            fs::metadata(output).unwrap().permissions().mode() & 0o077,
            0
        );
        fs::remove_dir_all(root).unwrap();
    }
    #[tokio::test(flavor = "current_thread")]
    async fn size_limit_removes_own_partial_file() {
        let root = temp();
        let dir = private_dir(&root).unwrap();
        let mut download = DownloadFile::create(&dir, 1, 2, "x", 2).unwrap();
        assert!(download.write_chunk(b"abc").await.is_err());
        drop(download);
        assert_eq!(fs::read_dir(&dir).unwrap().count(), 0);
        fs::remove_dir_all(root).unwrap();
    }
    #[tokio::test(flavor = "current_thread")]
    async fn competing_destination_is_not_overwritten() {
        let root = temp();
        let dir = private_dir(&root).unwrap();
        let mut download = DownloadFile::create(&dir, 3, 4, "file.txt", 10).unwrap();
        download.write_chunk(b"new").await.unwrap();
        let existing = dir.join("3_4_0_file.txt");
        fs::write(&existing, b"other").unwrap();
        assert!(download.finish(Some(3)).await.is_err());
        assert_eq!(fs::read(&existing).unwrap(), b"other");
        assert_eq!(fs::read_dir(&dir).unwrap().count(), 1);
        fs::remove_dir_all(root).unwrap();
    }
    #[tokio::test(flavor = "current_thread")]
    async fn truncated_download_is_not_published() {
        let root = temp();
        let dir = private_dir(&root).unwrap();
        let mut download = DownloadFile::create(&dir, 1, 3, "file.bin", 10).unwrap();
        download.write_chunk(b"abc").await.unwrap();
        assert_eq!(
            download.finish(Some(4)).await.unwrap_err().kind(),
            io::ErrorKind::UnexpectedEof
        );
        assert_eq!(fs::read_dir(&dir).unwrap().count(), 0);
        fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn rejects_a_public_download_directory() {
        let root = temp();
        let dir = root.join("downloads");
        fs::create_dir(&dir).unwrap();
        fs::set_permissions(&dir, fs::Permissions::from_mode(0o755)).unwrap();
        assert!(private_dir(&root).is_err());
        fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn rejects_symlink_download_directory() {
        let root = temp();
        let target = root.join("other");
        fs::create_dir(&target).unwrap();
        std::os::unix::fs::symlink(&target, root.join("downloads")).unwrap();
        assert!(private_dir(&root).is_err());
        fs::remove_dir_all(root).unwrap();
    }
}
