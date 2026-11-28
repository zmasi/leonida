//! Leonida: button prompts for the device in the player's hands. The title has one set of button glyphs, an Xbox
//! one: a Scaleform movie of 38 vector sprites (`platform_xbsx.input_icons.Main_Button_N`, see
//! `docs/prompt_glyphs.md`) that sits in `data.rpf` as one zlib stream. The title's read of that stream is answered
//! with a movie of our own, built on the player's PC, for the family in use (`gameinput::active_family`):
//!
//! | family | movie | built by |
//! |---|---|---|
//! | Xbox | the title's own (the read goes to the archive) | |
//! | PlayStation | `ps_prompt_glyphs.gfx`, or `LEONIDA_PS_GLYPHS` | `tools/ps_prompt_glyphs.py` |
//! | keyboard and mouse | `pc_prompt_glyphs.gfx`, or `LEONIDA_PC_GLYPHS` | `tools/pc_prompt_glyphs.py` |
//!
//! How the swap finds the stream without the archive's table of contents (encrypted, and not used by this port):
//! the title asks DirectStorage for a zlib request on the archive's file at the stream's offset, with the inflated
//! size it knows; both are measured on the stream (`docs/prompt_glyphs.md`), so a request for that file at that
//! offset and that size is the movie. The stored size does not matter (the replacement is served inflated), and
//! the archive is not touched.
//!
//! The title reads the movie once, about 3 s after it starts (measured: one read in a 15 minute session with the
//! menus opened), so the family at that moment decides and a later switch of device shows on the next start. With
//! no pad connected that is the keyboard and mouse, with a Sony pad PlayStation; `LEONIDA_PROMPTS` forces one.
//!
//! Each variable names its movie file, or `off` (or `0`) to leave the title's own in place; without it the file is
//! looked for beside the runtime and in the six folders above it. A family whose movie is missing or not usable
//! gets the title's Xbox glyphs.
use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use super::gameinput::Family;

/// The archive the movie is in.
pub const ARCHIVE: &str = "data.rpf";
/// Where its zlib stream starts in the archive, and what it inflates to.
pub const MOVIE_OFFSET: u64 = 0x117d_7000;
pub const MOVIE_SIZE: u32 = 28_148;

/// One replacement movie: whose it is and where it comes from.
#[derive(Debug, PartialEq, Eq)]
pub struct Set {
    /// For the log: "PlayStation movie loaded".
    pub label: &'static str,
    /// The name of the built movie.
    pub file: &'static str,
    /// The variable that names another file, or turns the set off.
    pub variable: &'static str,
    /// What builds it.
    pub tool: &'static str,
}

pub static PLAYSTATION: Set = Set {
    label: "PlayStation",
    file: "ps_prompt_glyphs.gfx",
    variable: "LEONIDA_PS_GLYPHS",
    tool: "tools/ps_prompt_glyphs.py",
};

pub static KEYBOARD: Set = Set {
    label: "keyboard",
    file: "pc_prompt_glyphs.gfx",
    variable: "LEONIDA_PC_GLYPHS",
    tool: "tools/pc_prompt_glyphs.py",
};

/// The set a family is shown; none for Xbox, whose glyphs are the title's own.
pub fn set_for(family: Family) -> Option<&'static Set> {
    match family {
        Family::Xbox => None,
        Family::PlayStation => Some(&PLAYSTATION),
        Family::KeyboardMouse => Some(&KEYBOARD),
    }
}

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
        return Err(format!("{} bytes, not {MOVIE_SIZE}", movie.len()));
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

/// What a set's variable asks for.
#[derive(Debug, PartialEq, Eq)]
enum Source {
    /// `off` or `0`: the title's own movie stays.
    Off,
    /// This file.
    File(PathBuf),
    /// Not set: the set's file beside the runtime, or above it.
    Search,
}

fn source(value: Option<OsString>) -> Source {
    match value {
        Some(v) if v == "off" || v == "0" => Source::Off,
        Some(v) if !v.is_empty() => Source::File(PathBuf::from(v)),
        _ => Source::Search,
    }
}

/// Where a movie named `file` is looked for: beside the runtime and up to six folders above it, then the
/// working folder.
fn candidates(file: &str) -> Vec<PathBuf> {
    let mut found = Vec::new();
    if let Ok(exe) = std::env::current_exe() {
        let mut at = exe.parent().map(Path::to_path_buf);
        for _ in 0..7 {
            let Some(dir) = at else { break };
            found.push(dir.join(file));
            at = dir.parent().map(Path::to_path_buf);
        }
    }
    found.push(PathBuf::from(file));
    found
}

/// Reads and checks one movie file.
fn read(path: &Path) -> Result<Vec<u8>, String> {
    let movie = std::fs::read(path).map_err(|error| format!("cannot be read: {error}"))?;
    valid(&movie).map(|()| movie)
}

fn load(set: &Set) -> Option<Vec<u8>> {
    let path = match source(std::env::var_os(set.variable)) {
        Source::Off => return None,
        Source::File(path) => path,
        Source::Search => candidates(set.file).into_iter().find(|p| p.is_file())?,
    };
    match read(&path) {
        Ok(movie) => {
            tracing::info!(path = %path.display(), "prompt glyphs: {} movie loaded", set.label);
            Some(movie)
        }
        Err(why) => {
            tracing::warn!(
                path = %path.display(),
                "prompt glyphs: the {} movie is not usable: {why} (build it with {})",
                set.label,
                set.tool
            );
            None
        }
    }
}

/// A set's built movie, read once.
fn movie(set: &'static Set) -> Option<&'static [u8]> {
    static PLAYSTATION_MOVIE: OnceLock<Option<Vec<u8>>> = OnceLock::new();
    static KEYBOARD_MOVIE: OnceLock<Option<Vec<u8>>> = OnceLock::new();
    let cell = if *set == PLAYSTATION { &PLAYSTATION_MOVIE } else { &KEYBOARD_MOVIE };
    cell.get_or_init(|| load(set)).as_deref()
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
    let family = family();
    let served = set_for(family).and_then(|set| movie(set).map(|movie| (set, movie)));
    static LOGGED: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(u32::MAX);
    if LOGGED.swap(family as u32, std::sync::atomic::Ordering::Relaxed) != family as u32 {
        tracing::info!(
            family = family.name(),
            glyphs = served.map_or("the title's own", |(set, _)| set.label),
            "prompt glyphs: the title reads its glyph movie"
        );
    }
    served.map(|(_, movie)| movie)
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

    #[test]
    fn each_family_has_its_own_movie_and_xbox_keeps_the_title_s() {
        assert_eq!(set_for(Family::Xbox), None);
        let ps = set_for(Family::PlayStation).unwrap();
        assert_eq!((ps.file, ps.variable), ("ps_prompt_glyphs.gfx", "LEONIDA_PS_GLYPHS"));
        let pc = set_for(Family::KeyboardMouse).unwrap();
        assert_eq!((pc.file, pc.variable), ("pc_prompt_glyphs.gfx", "LEONIDA_PC_GLYPHS"));
        assert_ne!(ps, pc, "the two are told apart when their movies are kept");
    }

    #[test]
    fn a_variable_names_a_file_or_turns_its_set_off() {
        assert_eq!(source(None), Source::Search);
        assert_eq!(source(Some("".into())), Source::Search);
        assert_eq!(source(Some("off".into())), Source::Off);
        assert_eq!(source(Some("0".into())), Source::Off);
        assert_eq!(source(Some(r"D:\glyphs\mine.gfx".into())), Source::File(PathBuf::from(r"D:\glyphs\mine.gfx")));
    }

    #[test]
    fn the_movie_is_looked_for_beside_the_runtime_first_under_its_own_name() {
        let found = candidates(KEYBOARD.file);
        assert!(found.len() >= 2, "beside the runtime, and the working folder");
        assert!(found.iter().all(|p| p.file_name().is_some_and(|n| n == "pc_prompt_glyphs.gfx")));
        assert_eq!(found.last(), Some(&PathBuf::from("pc_prompt_glyphs.gfx")));
        let exe = std::env::current_exe().unwrap();
        assert_eq!(found[0].parent(), exe.parent());
    }

    #[test]
    fn a_movie_file_is_read_whole_and_checked() {
        let dir = std::env::temp_dir().join(format!("leonida_glyphs_test_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let good = dir.join("pc_prompt_glyphs.gfx");
        std::fs::write(&good, movie(MOVIE_SIZE as usize, b"GFX*", MOVIE_SIZE)).unwrap();
        assert_eq!(read(&good).unwrap().len(), MOVIE_SIZE as usize);
        let short = dir.join("short.gfx");
        std::fs::write(&short, movie(100, b"GFX*", 100)).unwrap();
        assert!(read(&short).unwrap_err().contains("100 bytes"));
        assert!(read(&dir.join("missing.gfx")).unwrap_err().contains("cannot be read"));
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
