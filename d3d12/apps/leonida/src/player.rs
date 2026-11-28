//! Moving the local player by writing its position into the title's memory.
//!
//! The title's script commands cannot be called yet (`docs/mod-menu.md`), so a
//! teleport is done the way a memory trainer does it: the player's transform
//! (a 4x4 or 3x4 float matrix whose position row is the player's place in the
//! world) is found in memory, and a new position is written into it, and into
//! the physics copy of it that moves with it. Where those matrices are is
//! found by the diagnostic `LEONIDA_FIND_PLAYER` (`player_find.rs`), which
//! writes a file of pointer paths from the title's image to them
//! (`%TEMP%\leonida-player\paths.txt`, or `LEONIDA_PLAYER_PATHS=<file>`); this
//! reads that file and follows the paths at run time. A run that has just
//! found the matrices itself uses their addresses directly ([`set_known`]).
//!
//! Reads and writes go through `ReadProcessMemory` / `WriteProcessMemory` on
//! the own process: a page that is gone or read-only is a failed call and not
//! a fault. Nothing leaves the machine; nothing of the title is stored here.
//!
//! - `LEONIDA_TELEPORT=<delay s>,<x>,<y>,<z>[,<hold ms>]` teleports once, that
//!   many seconds after start (the menu's Teleport page and `leonida.teleport`
//!   do the same on demand).
//! - `LEONIDA_TELEPORT_LIFT=<metres>` is added to the target's z (a few metres
//!   above the ground, so a position slightly under it does not trap the player).
//! - `LEONIDA_VICE_CITY=<x>,<y>,<z>` (or the file `vice.txt` beside the paths)
//!   is the place the menu's "To Vice City" item goes to.

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Mutex, OnceLock};
use std::time::Duration;

/// Reads and writes of the own process that fail instead of faulting.
pub(crate) mod mem {
    unsafe extern "system" {
        fn GetCurrentProcess() -> isize;
        fn ReadProcessMemory(
            process: isize,
            address: usize,
            buffer: *mut u8,
            size: usize,
            read: *mut usize,
        ) -> i32;
        fn WriteProcessMemory(
            process: isize,
            address: usize,
            buffer: *const u8,
            size: usize,
            written: *mut usize,
        ) -> i32;
    }

    /// Fills `out` from `address`; false when any part is not readable.
    pub fn read(address: usize, out: &mut [u8]) -> bool {
        let mut got = 0usize;
        // SAFETY: a read of the own process into a buffer of `out.len()` bytes.
        let ok = unsafe {
            ReadProcessMemory(GetCurrentProcess(), address, out.as_mut_ptr(), out.len(), &mut got)
        };
        ok != 0 && got == out.len()
    }

    pub fn read_vec(address: usize, len: usize) -> Option<Vec<u8>> {
        let mut data = vec![0u8; len];
        read(address, &mut data).then_some(data)
    }

    pub fn read_u64(address: usize) -> Option<u64> {
        let mut b = [0u8; 8];
        read(address, &mut b).then(|| u64::from_le_bytes(b))
    }

    /// Writes `data` at `address`; false when any part is not writable.
    pub fn write(address: usize, data: &[u8]) -> bool {
        let mut put = 0usize;
        // SAFETY: a write into the own process from a buffer of `data.len()` bytes.
        let ok = unsafe {
            WriteProcessMemory(GetCurrentProcess(), address, data.as_ptr(), data.len(), &mut put)
        };
        ok != 0 && put == data.len()
    }
}

/// How a transform is laid out in memory (floats, little endian).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Layout {
    /// Four rows of four floats: three axes (the fourth float of each is
    /// padding) and the position row, as in a 4x4 row-major matrix.
    R4,
    /// Three rows of four floats: the axis in the first three, the position's
    /// component in the fourth.
    T34,
    /// Four rows of three floats, packed: three axes and the position.
    R43,
    /// Three f32 (x, y, z) at the start of a 16-byte vector whose fourth float is
    /// not a position (a flag word, 0 or 1): no axes.
    V4,
    /// Three f64 (x, y, z): no axes.
    D3,
}

impl Layout {
    pub fn name(self) -> &'static str {
        match self {
            Layout::R4 => "R4",
            Layout::T34 => "T34",
            Layout::R43 => "R43",
            Layout::V4 => "V4",
            Layout::D3 => "D3",
        }
    }
    /// Whether the layout has axes (a transform) and not just a position.
    pub fn has_axes(self) -> bool {
        !matches!(self, Layout::V4 | Layout::D3)
    }
    pub fn parse(s: &str) -> Option<Layout> {
        Some(match s {
            "R4" => Layout::R4,
            "T34" => Layout::T34,
            "R43" => Layout::R43,
            "V4" => Layout::V4,
            "D3" => Layout::D3,
            _ => return None,
        })
    }
    /// Bytes read to decode one.
    pub fn size(self) -> usize {
        match self {
            Layout::R4 => 64,
            Layout::V4 => 16,
            Layout::D3 => 24,
            _ => 48,
        }
    }
    /// Float index of component `c` of axis `row`.
    fn axis_at(self, row: usize, c: usize) -> usize {
        match self {
            Layout::R43 => row * 3 + c,
            _ => row * 4 + c,
        }
    }
    /// Float indices of the position's x, y, z.
    pub fn pos_at(self) -> [usize; 3] {
        match self {
            Layout::R4 => [12, 13, 14],
            Layout::T34 => [3, 7, 11],
            Layout::R43 => [9, 10, 11],
            Layout::V4 => [0, 1, 2],
            // Doubles: see `decode` and `write_pos`.
            Layout::D3 => [0, 2, 4],
        }
    }
}

/// A decoded transform.
#[derive(Clone, Copy, Debug)]
pub struct Mat {
    pub axes: [[f32; 3]; 3],
    pub pos: [f32; 3],
}

fn float(b: &[u8], i: usize) -> f32 {
    f32::from_le_bytes([b[i * 4], b[i * 4 + 1], b[i * 4 + 2], b[i * 4 + 3]])
}

/// Decodes `b` (at least `layout.size()` bytes).
pub fn decode(b: &[u8], layout: Layout) -> Mat {
    if !layout.has_axes() {
        let identity = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]];
        let pos = if layout == Layout::D3 {
            let d = |i: usize| f64::from_le_bytes(b[i * 8..i * 8 + 8].try_into().unwrap()) as f32;
            [d(0), d(1), d(2)]
        } else {
            [float(b, 0), float(b, 1), float(b, 2)]
        };
        return Mat { axes: identity, pos };
    }
    let mut axes = [[0.0; 3]; 3];
    for (r, axis) in axes.iter_mut().enumerate() {
        for (c, v) in axis.iter_mut().enumerate() {
            *v = float(b, layout.axis_at(r, c));
        }
    }
    let [x, y, z] = layout.pos_at();
    Mat {
        axes,
        pos: [float(b, x), float(b, y), float(b, z)],
    }
}

impl Mat {
    /// Unit axes at right angles, within `tol` (on the lengths and the cosines).
    pub fn orthonormal(&self, tol: f32) -> bool {
        let dot = |a: [f32; 3], b: [f32; 3]| a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        let a = &self.axes;
        (0..3).all(|i| (dot(a[i], a[i]) - 1.0).abs() < 2.0 * tol)
            && dot(a[0], a[1]).abs() < tol
            && dot(a[0], a[2]).abs() < tol
            && dot(a[1], a[2]).abs() < tol
    }
}

/// The transform at `address`, when it is readable and still looks like one.
pub fn read_mat(address: usize, layout: Layout) -> Option<Mat> {
    let b = mem::read_vec(address, layout.size())?;
    let m = decode(&b, layout);
    let sane = m.pos.iter().all(|v| v.is_finite() && v.abs() < 1.0e5);
    (sane && m.orthonormal(0.02)).then_some(m)
}

pub fn write_pos(address: usize, layout: Layout, p: [f32; 3]) -> bool {
    if layout == Layout::D3 {
        let mut b = [0u8; 24];
        for (i, v) in p.iter().enumerate() {
            b[i * 8..i * 8 + 8].copy_from_slice(&f64::from(*v).to_le_bytes());
        }
        return mem::write(address, &b);
    }
    let at = layout.pos_at();
    if at[1] == at[0] + 1 && at[2] == at[0] + 2 {
        let mut b = [0u8; 12];
        for (i, v) in p.iter().enumerate() {
            b[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
        }
        mem::write(address + at[0] * 4, &b)
    } else {
        at.iter()
            .zip(p)
            .all(|(&i, v)| mem::write(address + i * 4, &v.to_le_bytes()))
    }
}

/// One way to reach a transform from the title's image: the location of a
/// pointer in a writable section, then for each further link the offset to add
/// to what was read to get the next pointer's location, and finally the
/// offset from the pointed-to address to the transform.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Path {
    pub rva: usize,
    pub deltas: Vec<usize>,
    pub adjust: usize,
}

/// A transform that moves with the player (its entity's, its physics copy's).
#[derive(Clone, Debug)]
pub struct Group {
    pub layout: Layout,
    /// The one the player's position is read from.
    pub primary: bool,
    pub paths: Vec<Path>,
    /// The address, for a group found in this run.
    pub fixed: Option<usize>,
}

/// What is known about where the player is.
#[derive(Clone, Debug, Default)]
pub struct Known {
    pub groups: Vec<Group>,
    /// The primary transform's axis the player walks along, and the sign.
    pub forward: Option<(usize, f32)>,
}

const HEADER: &str = "leonida-player-paths 1";

impl Known {
    #[cfg_attr(not(feature = "diagnostics"), allow(dead_code))]
    /// The file `paths.txt` is (the image's size first: paths mean nothing in
    /// another build).
    pub fn to_text(&self, image_size: usize) -> String {
        use std::fmt::Write as _;
        let mut s = format!("{HEADER}\nimage-size {image_size:#x}\n");
        if let Some((axis, sign)) = self.forward {
            let _ = writeln!(s, "forward {axis} {sign:+}");
        }
        for g in &self.groups {
            let _ = writeln!(s, "group {} {}", g.layout.name(), if g.primary { "primary" } else { "copy" });
            for p in &g.paths {
                let d: Vec<String> = p.deltas.iter().map(|d| format!("{d:#x}")).collect();
                let _ = writeln!(s, "path {:#x} [{}] {:#x}", p.rva, d.join(","), p.adjust);
            }
        }
        s
    }

    pub fn parse(text: &str, image_size: usize) -> Result<Known, String> {
        let hex = |s: &str| usize::from_str_radix(s.trim().trim_start_matches("0x"), 16).map_err(|e| format!("{s}: {e}"));
        let mut lines = text.lines().map(str::trim).filter(|l| !l.is_empty() && !l.starts_with('#'));
        if lines.next() != Some(HEADER) {
            return Err("not a leonida-player-paths 1 file".into());
        }
        let mut known = Known::default();
        for line in lines {
            let mut w = line.split_whitespace();
            match w.next() {
                Some("image-size") => {
                    let size = hex(w.next().unwrap_or(""))?;
                    if size != image_size {
                        return Err(format!("paths are for an image of {size:#x} bytes, this one has {image_size:#x}"));
                    }
                }
                Some("forward") => {
                    let axis = w.next().and_then(|a| a.parse().ok()).filter(|&a| a < 3).ok_or("forward: axis")?;
                    let sign = w.next().and_then(|a| a.parse::<f32>().ok()).ok_or("forward: sign")?;
                    known.forward = Some((axis, if sign < 0.0 { -1.0 } else { 1.0 }));
                }
                Some("group") => {
                    let layout = w.next().and_then(Layout::parse).ok_or("group: layout")?;
                    known.groups.push(Group {
                        layout,
                        primary: w.next() == Some("primary"),
                        paths: Vec::new(),
                        fixed: None,
                    });
                }
                Some("path") => {
                    let rva = hex(w.next().ok_or("path: rva")?)?;
                    let list = w.next().ok_or("path: deltas")?.trim_matches(['[', ']']);
                    let deltas = list.split(',').filter(|d| !d.is_empty()).map(hex).collect::<Result<_, _>>()?;
                    let adjust = hex(w.next().ok_or("path: adjust")?)?;
                    known
                        .groups
                        .last_mut()
                        .ok_or("path before group")?
                        .paths
                        .push(Path { rva, deltas, adjust });
                }
                _ => return Err(format!("unknown line: {line}")),
            }
        }
        if known.groups.is_empty() {
            return Err("no groups".into());
        }
        if !known.groups.iter().any(|g| g.primary) {
            known.groups[0].primary = true;
        }
        Ok(known)
    }
}

/// The title's image: base and size.
static IMAGE: OnceLock<(usize, usize)> = OnceLock::new();
static KNOWN: Mutex<Option<Known>> = Mutex::new(None);
/// A `LEONIDA_FIND_PLAYER` run is under way and has not got to the point where it
/// knows what to write to (or has given up): a `LEONIDA_TELEPORT` waits for it.
static FINDING: AtomicBool = AtomicBool::new(false);

/// The finder starts (`player_find.rs`).
#[cfg_attr(not(feature = "diagnostics"), allow(dead_code))]
pub fn find_started() {
    FINDING.store(true, Ordering::Release);
}

/// The finder has set what it found (or found nothing).
#[cfg_attr(not(feature = "diagnostics"), allow(dead_code))]
pub fn find_finished() {
    FINDING.store(false, Ordering::Release);
}

/// Bumped by every teleport: an older hold stops when it sees a newer number.
static TELEPORTS: AtomicU64 = AtomicU64::new(0);

pub fn out_dir() -> PathBuf {
    std::env::temp_dir().join("leonida-player")
}

/// Tells the runtime where the player is (from the finder, in this run).
pub fn set_known(known: Known) {
    *KNOWN.lock().unwrap_or_else(|e| e.into_inner()) = Some(known);
}

fn known() -> Option<Known> {
    KNOWN.lock().unwrap_or_else(|e| e.into_inner()).clone()
}

/// Where a group's transform is now: its fixed address, else the address most
/// of its paths lead to (a path that leads somewhere that is not a transform
/// is ignored).
fn resolve(group: &Group) -> Option<usize> {
    if let Some(a) = group.fixed {
        return read_mat(a, group.layout).map(|_| a);
    }
    let (base, _) = *IMAGE.get()?;
    let mut votes: Vec<(usize, usize)> = Vec::new();
    for p in &group.paths {
        let Some(a) = follow(base, p) else { continue };
        if read_mat(a, group.layout).is_none() {
            continue;
        }
        match votes.iter_mut().find(|(v, _)| *v == a) {
            Some((_, n)) => *n += 1,
            None => votes.push((a, 1)),
        }
    }
    votes.into_iter().max_by_key(|&(_, n)| n).map(|(a, _)| a)
}

fn follow(base: usize, path: &Path) -> Option<usize> {
    let mut at = base.checked_add(path.rva)?;
    for d in &path.deltas {
        at = (mem::read_u64(at)? as usize).checked_add(*d)?;
    }
    (mem::read_u64(at)? as usize).checked_add(path.adjust)
}

/// The resolved groups: (address, layout, primary).
fn resolved(known: &Known) -> Vec<(usize, Layout, bool)> {
    known
        .groups
        .iter()
        .filter_map(|g| Some((resolve(g)?, g.layout, g.primary)))
        .collect()
}

fn ready() -> Result<(), String> {
    // The title's own ped, when the game actions found it (`LEONIDA_GAME_ACTIONS=1`).
    if crate::game_actions::position().is_some() {
        return Ok(());
    }
    let Some(k) = known() else {
        return Err(format!(
            "No player position known: run once with LEONIDA_FIND_PLAYER (docs/mod-menu.md), which writes {}.",
            out_dir().join("paths.txt").display()
        ));
    };
    if resolved(&k).is_empty() {
        return Err("The player's position was not found at the known places (a new build, or the player is not loaded yet).".into());
    }
    Ok(())
}

/// The primary transform now.
fn primary() -> Result<(usize, Layout, Mat), String> {
    let k = known().ok_or("No player position known.")?;
    let all = resolved(&k);
    let &(address, layout, _) = all
        .iter()
        .find(|g| g.2)
        .or(all.first())
        .ok_or("The player's position was not found at the known places.")?;
    let m = read_mat(address, layout).ok_or("The player's transform is not readable.")?;
    Ok((address, layout, m))
}

fn position() -> Result<[f32; 3], String> {
    // The ped's own position, which the title's own position function reads, before any found copy of it.
    if let Some(at) = crate::game_actions::position() {
        return Ok(at);
    }
    primary().map(|(_, _, m)| m.pos)
}

fn heading() -> Option<[f32; 2]> {
    if let Some(facing) = crate::game_actions::heading() {
        return Some(facing);
    }
    // The forward axis was measured on the first group that has axes.
    let k = known()?;
    let (address, layout, _) = resolved(&k).into_iter().find(|g| g.1.has_axes())?;
    let m = read_mat(address, layout)?;
    let (axis, sign) = k.forward.unwrap_or((1, 1.0));
    let v = m.axes[axis];
    let len = (v[0] * v[0] + v[1] * v[1]).sqrt();
    (len > 1.0e-3).then(|| [sign * v[0] / len, sign * v[1] / len])
}

/// Moves the player to `to` (its primary transform's position), every other
/// transform by the same amount, and writes them again every 5 ms for
/// `hold_ms` so that the physics does not put the player back and it does not
/// fall through ground that is not loaded yet.
pub fn teleport(to: [f32; 3], hold_ms: u32) -> Result<String, String> {
    // With the game actions on, the write is the title's thread's (an experiment, `LEONIDA_GAME_ACTIONS_WARP=1`).
    if let Some(asked) = crate::game_actions::warp(to, hold_ms) {
        return asked;
    }
    if crate::game_actions::on() && known().is_none() {
        return Err("Moving the player is not done yet: the title's own function for it was not found                     (docs/mod-menu.md). LEONIDA_GAME_ACTIONS_WARP=1 tries a write of the ped's position."
            .into());
    }
    let k = known().ok_or("No player position known.")?;
    let all = resolved(&k);
    let lift = std::env::var("LEONIDA_TELEPORT_LIFT")
        .ok()
        .and_then(|v| v.trim().parse::<f32>().ok())
        .unwrap_or(0.0);
    let to = [to[0], to[1], to[2] + lift];
    let &(main, main_layout, _) = all
        .iter()
        .find(|g| g.2)
        .or(all.first())
        .ok_or("The player's position was not found at the known places.")?;
    let from = read_mat(main, main_layout).ok_or("The player's transform is not readable.")?.pos;
    let shift = [to[0] - from[0], to[1] - from[1], to[2] - from[2]];
    let mut targets: Vec<(usize, Layout, [f32; 3])> = Vec::new();
    let mut line = format!("from ({:.1}, {:.1}, {:.1}) to ({:.1}, {:.1}, {:.1})", from[0], from[1], from[2], to[0], to[1], to[2]);
    if from == [0.0; 3] {
        return Err("The primary position reads all zeros: not a player position.".into());
    }
    for &(address, layout, _) in &all {
        let Some(m) = read_mat(address, layout) else { continue };
        // Never a member that is all zeros or more than 5 m from the primary.
        let far = (0..3).map(|i| (m.pos[i] - from[i]).powi(2)).sum::<f32>().sqrt() > 5.0;
        if m.pos == [0.0; 3] || far {
            eprintln!("Leonida: teleport: {address:#x} {} skipped: it reads ({:.2}, {:.2}, {:.2}), not near the primary", layout.name(), m.pos[0], m.pos[1], m.pos[2]);
            continue;
        }
        let p = [m.pos[0] + shift[0], m.pos[1] + shift[1], m.pos[2] + shift[2]];
        let ok = write_pos(address, layout, p);
        eprintln!(
            "Leonida: teleport: {address:#x} {} ({:.2}, {:.2}, {:.2}) -> ({:.2}, {:.2}, {:.2}): {}",
            layout.name(),
            m.pos[0],
            m.pos[1],
            m.pos[2],
            p[0],
            p[1],
            p[2],
            if ok { "written" } else { "write failed" }
        );
        if ok {
            targets.push((address, layout, p));
        }
    }
    if targets.is_empty() {
        return Err("The position could not be written (the memory is not writable).".into());
    }
    line.push_str(&format!("; wrote {} transform(s)", targets.len()));
    let me = TELEPORTS.fetch_add(1, Ordering::AcqRel) + 1;
    if hold_ms > 0 {
        std::thread::Builder::new()
            .name("leonida-teleport-hold".into())
            .spawn(move || {
                let end = std::time::Instant::now() + Duration::from_millis(u64::from(hold_ms));
                while std::time::Instant::now() < end && TELEPORTS.load(Ordering::Acquire) == me {
                    for &(address, layout, p) in &targets {
                        write_pos(address, layout, p);
                    }
                    std::thread::sleep(Duration::from_millis(5));
                }
            })
            .ok();
        line.push_str(&format!(", holding {hold_ms} ms"));
    }
    Ok(line)
}

fn parse_numbers(text: &str) -> Vec<f32> {
    text.split(|c: char| c.is_whitespace() || c == ',' || c == ';')
        .filter_map(|v| v.parse().ok())
        .collect()
}

fn place(name: &str) -> Option<[f32; 3]> {
    let (var, file) = match name {
        "vice" => ("LEONIDA_VICE_CITY", "vice.txt"),
        _ => return None,
    };
    let text = std::env::var(var)
        .ok()
        .or_else(|| std::fs::read_to_string(out_dir().join(file)).ok())?;
    match parse_numbers(&text)[..] {
        [x, y, z, ..] if [x, y, z].iter().all(|v| v.is_finite()) => Some([x, y, z]),
        _ => None,
    }
}

/// Loads the known paths and hands the menu and the Lua API their hook.
pub fn install(base: usize, size: usize) {
    let _ = IMAGE.set((base, size));
    let file = std::env::var_os("LEONIDA_PLAYER_PATHS")
        .map(PathBuf::from)
        .unwrap_or_else(|| out_dir().join("paths.txt"));
    match std::fs::read_to_string(&file) {
        Ok(text) => match Known::parse(&text, size) {
            Ok(k) => {
                eprintln!("Leonida: player paths: {} group(s) from {}", k.groups.len(), file.display());
                set_known(k);
            }
            Err(e) => eprintln!("Leonida: player paths in {} not used: {e}", file.display()),
        },
        Err(_) => {}
    }
    leonida_menu::knobs::provide_player(leonida_menu::knobs::PlayerHook {
        ready,
        position,
        heading,
        teleport,
        place,
    });
    if let Ok(spec) = std::env::var("LEONIDA_TELEPORT") {
        let n = parse_numbers(&spec);
        if n.len() < 4 {
            eprintln!("Leonida: LEONIDA_TELEPORT={spec}: expected <delay s>,<x>,<y>,<z>[,<hold ms>]");
            return;
        }
        let hold = n.get(4).map_or(3000, |&h| h.max(0.0) as u32);
        let (delay, to) = (n[0].max(0.0), [n[1], n[2], n[3]]);
        std::thread::Builder::new()
            .name("leonida-teleport".into())
            .spawn(move || {
                std::thread::sleep(Duration::from_secs_f32(delay));
                // A finder in this run decides what is written to: wait until it says it is done.
                if FINDING.load(Ordering::Acquire) {
                    eprintln!("Leonida: teleport: waiting for the player finder (LEONIDA_FIND_PLAYER) to finish");
                    while FINDING.load(Ordering::Acquire) {
                        std::thread::sleep(Duration::from_millis(500));
                    }
                }
                match ready().and_then(|()| teleport(to, hold)) {
                    Ok(line) => eprintln!("Leonida: teleport: {line}"),
                    Err(e) => eprintln!("Leonida: teleport failed: {e}"),
                }
            })
            .ok();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn paths_round_trip() {
        let k = Known {
            groups: vec![
                Group {
                    layout: Layout::T34,
                    primary: true,
                    paths: vec![
                        Path { rva: 0x1234, deltas: vec![], adjust: 0x30 },
                        Path { rva: 0x99, deltas: vec![0x10, 0x8], adjust: 0 },
                    ],
                    fixed: None,
                },
                Group { layout: Layout::R4, primary: false, paths: vec![], fixed: None },
            ],
            forward: Some((1, -1.0)),
        };
        let back = Known::parse(&k.to_text(0x5000), 0x5000).unwrap();
        assert_eq!(back.forward, k.forward);
        assert_eq!(back.groups.len(), 2);
        assert_eq!(back.groups[0].paths, k.groups[0].paths);
        assert_eq!(back.groups[1].layout, Layout::R4);
        assert!(Known::parse(&k.to_text(0x5000), 0x6000).is_err());
    }

    #[test]
    fn layouts_decode_the_position() {
        let mut b = [0u8; 64];
        let set = |b: &mut [u8], i: usize, v: f32| b[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
        for (layout, axes) in [(Layout::R4, [0, 5, 10]), (Layout::T34, [0, 5, 10]), (Layout::R43, [0, 4, 8])] {
            b.fill(0);
            for i in axes {
                set(&mut b, i, 1.0);
            }
            let [x, y, z] = layout.pos_at();
            set(&mut b, x, 3.0);
            set(&mut b, y, 4.0);
            set(&mut b, z, 5.0);
            let m = decode(&b, layout);
            assert!(m.orthonormal(1e-3), "{layout:?}");
            assert_eq!(m.pos, [3.0, 4.0, 5.0], "{layout:?}");
        }
    }
}
