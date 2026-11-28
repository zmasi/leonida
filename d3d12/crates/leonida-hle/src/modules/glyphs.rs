//! Leonida: PlayStation button prompts. The title has one set of button glyphs, an Xbox one: a Scaleform movie of
//! 38 vector sprites (`platform_xbsx.input_icons.Main_Button_N`, see `docs/prompt_glyphs.md`) that sits in
//! `data.rpf` as one zlib stream. While the player holds a Sony pad (`gameinput::active_family`), the title's
//! read of that stream is answered with a movie of our own, built on the player's PC by
//! `tools/ps_prompt_glyphs.py`.
//!
//! How the swap finds the stream without the archive's table of contents (encrypted, and not used by this port):
//! the title asks DirectStorage for a zlib request on the archive's file at the stream's offset, with the inflated
//! size it knows; both are measured on the stream (`docs/prompt_glyphs.md`), so a request for that file at that
//! offset and that size is the movie. The stored size does not matter (the replacement is served inflated), and
//! the archive is not touched.
//!
//! The movie is read when the title asks for it, so the family at that moment counts. If the title loads the
//! movie once, at start, the glyphs follow the pad held then, or `LEONIDA_PROMPTS`; a later switch shows on the
//! next load.
//!
//! `LEONIDA_PS_GLYPHS=<file>` names the replacement movie (`off` turns the swap off); without it,
//! `ps_prompt_glyphs.gfx` beside the runtime, or in one of the six folders above it, is used.
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use super::gameinput::Family;

/// The archive the movie is in.
pub const ARCHIVE: &str = "data.rpf";
/// Where its zlib stream starts in the archive, and what it inflates to.
pub const MOVIE_OFFSET: u64 = 0x117d_7000;
pub const MOVIE_SIZE: u32 = 28_148;
/// The name of the built movie.
pub const FILE_NAME: &str = "ps_prompt_glyphs.gfx";

/// Whether a read of `file` at `offset` inflating to `size` bytes is the title's glyph movie.
pub fn is_movie(file: &str, offset: u64, size: u32) -> bool {
    offset == MOVIE_OFFSET && size == MOVIE_SIZE && {
        let name = file.rsplit(['\\', '/']).next().unwrap_or(file);
        name.eq_ignore_ascii_case(ARCHIVE)
    }
}

/// A movie that can stand in for the title's: Scaleform's or Flash's header (`GFX`/`FWS`, uncompressed) whose
/// length field is the whole file, as long as the one it replaces.
fn valid(movie: &[u8]) -> Result<(), String> {
    if movie.len() != MOVIE_SIZE as usize {
        return Err(format!("{} bytes, not {MOVIE_SIZE} (build it with tools/ps_prompt_glyphs.py)", movie.len()));
    }
    if !(movie.starts_with(b"GFX") || movie.starts_with(b"FWS")) {
        return Err("not an uncompressed GFX movie".into());
    }
    let length = u32::from_le_bytes([movie[4], movie[5], movie[6], movie[7]]);
    if length != MOVIE_SIZE {
        return Err(format!("its header says {length} bytes"));
    }
    Ok(())
}

/// Where the movie is looked for: the variable, else beside the runtime and up to six folders above it.
fn candidates() -> Vec<PathBuf> {
    let mut found = Vec::new();
    if let Ok(exe) = std::env::current_exe() {
        let mut at = exe.parent().map(Path::to_path_buf);
        for _ in 0..7 {
            let Some(dir) = at else { break };
            found.push(dir.join(FILE_NAME));
            at = dir.parent().map(Path::to_path_buf);
        }
    }
    found.push(PathBuf::from(FILE_NAME));
    found
}

fn load() -> Option<Vec<u8>> {
    let path = match std::env::var_os("LEONIDA_PS_GLYPHS") {
        Some(v) if v == "off" || v == "0" => return None,
        Some(v) => PathBuf::from(v),
        None => candidates().into_iter().find(|p| p.is_file())?,
    };
    let movie = match std::fs::read(&path) {
        Ok(movie) => movie,
        Err(error) => {
            tracing::warn!(path = %path.display(), %error, "prompt glyphs: the PlayStation movie cannot be read");
            return None;
        }
    };
    if let Err(why) = valid(&movie) {
        tracing::warn!(path = %path.display(), "prompt glyphs: the PlayStation movie is not usable: {why}");
        return None;
    }
    tracing::info!(path = %path.display(), "prompt glyphs: PlayStation movie loaded");
    Some(movie)
}

/// The built movie, read once.
fn movie() -> Option<&'static [u8]> {
    static MOVIE: OnceLock<Option<Vec<u8>>> = OnceLock::new();
    MOVIE.get_or_init(load).as_deref()
}

/// The movie to serve for a read of `file` at `offset` (inflating to `size` bytes) while `family` is in use:
/// `None` leaves the read to the archive.
pub fn replacement(file: &str, offset: u64, size: u32, family: impl FnOnce() -> Family) -> Option<&'static [u8]> {
    if !is_movie(file, offset, size) {
        if offset == MOVIE_OFFSET {
            static ONCE: std::sync::Once = std::sync::Once::new();
            ONCE.call_once(|| {
                tracing::warn!(file, size, "prompt glyphs: a read at the glyph movie's offset is not the movie (another archive or size)")
            });
        }
        return None;
    }
    let movie = movie()?;
    let family = family();
    static LOGGED: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(u32::MAX);
    if LOGGED.swap(family as u32, std::sync::atomic::Ordering::Relaxed) != family as u32 {
        tracing::info!(family = family.name(), "prompt glyphs: the title reads its glyph movie");
    }
    (family == Family::PlayStation).then_some(movie)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn movie(len: usize, header: &[u8; 4], length: u32) -> Vec<u8> {
        let mut m = vec![0u8; len];
        m[..4].copy_from_slice(header);
        m[4..8].copy_from_slice(&length.to_le_bytes());
        m
    }

    #[test]
    fn the_title_s_read_is_recognised_by_file_offset_and_size() {
        assert!(is_movie(r"G:\data.rpf", MOVIE_OFFSET, MOVIE_SIZE));
        assert!(is_movie("C:/Games/GTA6/DATA.RPF", MOVIE_OFFSET, MOVIE_SIZE));
        assert!(is_movie("data.rpf", MOVIE_OFFSET, MOVIE_SIZE));
        assert!(!is_movie(r"G:\data.rpf", MOVIE_OFFSET + 0x1000, MOVIE_SIZE));
        assert!(!is_movie(r"G:\data.rpf", MOVIE_OFFSET, MOVIE_SIZE + 1));
        assert!(!is_movie(r"G:\script.rpf", MOVIE_OFFSET, MOVIE_SIZE));
        assert!(!is_movie(r"G:\old_data.rpf", MOVIE_OFFSET, MOVIE_SIZE));
        assert!(!is_movie("", MOVIE_OFFSET, MOVIE_SIZE));
    }

    #[test]
    fn a_replacement_must_be_the_size_of_the_movie_it_replaces() {
        assert!(valid(&movie(MOVIE_SIZE as usize, b"GFX*", MOVIE_SIZE)).is_ok());
        assert!(valid(&movie(MOVIE_SIZE as usize, b"FWS\x09", MOVIE_SIZE)).is_ok());
        assert!(valid(&movie(MOVIE_SIZE as usize - 1, b"GFX*", MOVIE_SIZE)).is_err(), "short");
        assert!(valid(&movie(MOVIE_SIZE as usize, b"GFX*", 100)).is_err(), "the header disagrees");
        assert!(valid(&movie(MOVIE_SIZE as usize, b"CWS\x09", MOVIE_SIZE)).is_err(), "compressed");
        assert!(valid(&[]).is_err());
    }

    #[test]
    fn a_read_of_something_else_never_asks_for_the_family() {
        let asked = std::cell::Cell::new(false);
        let family = || {
            asked.set(true);
            Family::PlayStation
        };
        assert!(replacement(r"G:\data.rpf", 0x1000, 28, family).is_none());
        assert!(!asked.get());
    }
}
