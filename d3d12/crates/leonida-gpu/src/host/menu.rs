//! LOCAL (11/26): the F1 settings menu. F1 shows and hides it while our
//! window has the focus; Up/Down pick a setting, Left/Right change it, Enter
//! runs the line picked (save, save and restart, reset, close), Esc closes.
//! While it is shown the title sees no keyboard (`crate::SETTINGS_MENU_OPEN`,
//! read by the input module); a pad still works.
//!
//! Most settings are read once at start, so a change is saved to the file
//! `LEONIDA_MENU_FILE` names (play.ps1 sets it to the play folder's
//! settings.txt and applies it at every start) and takes effect at the next
//! start; "SAVE AND RESTART" starts play.ps1 again (`LEONIDA_MENU_PLAY`,
//! with `LEONIDA_MENU_ARGS`) and ends this run. Lines marked LIVE change at
//! once.
//!
//! Drawn as the stats panel is (`super::panel`): white text on a black box,
//! copied onto the swap chain's buffer, in the middle of the picture.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};
use std::time::Duration;

use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::*;
use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
    GetAsyncKeyState, VK_DOWN, VK_ESCAPE, VK_F1, VK_LEFT, VK_RETURN, VK_RIGHT, VK_UP,
};
use windows_sys::Win32::UI::WindowsAndMessaging::{GetForegroundWindow, GetWindowThreadProcessId};

use super::font::{colours, glyph};
use super::{Host, HostList, transition};

const GLYPH: (u32, u32) = (5, 7);
const CELL: (u32, u32) = (6, 9);
const COLUMNS: u32 = 60;
/// Lines of the box: a title, a help line, a blank, the settings shown, a
/// blank and a note.
const ROWS: usize = 20;
const LINES: u32 = ROWS as u32 + 5;
const RIM: u32 = 4;
const UPLOADS: usize = 4;

/// A setting: the variable, its name in the menu, the values it takes
/// (`""`: not set, the build's own default), and whether a change applies at once.
struct Item {
    var: &'static str,
    label: &'static str,
    values: &'static [(&'static str, &'static str)],
    live: bool,
}

const OFF_ON: &[(&str, &str)] = &[("0", "OFF"), ("1", "ON")];
const UNSET_ON: &[(&str, &str)] = &[("", "OFF"), ("1", "ON")];

#[rustfmt::skip]
const ITEMS: &[Item] = &[
    // Series S: the game's own is FSR, slow here. Series X: its own temporal upscaler; with DLSS (or the stretch) the
    // launcher makes the title upscale through FidelityFX there (LEONIDA_FSR_UPSCALER), where DLSS takes over.
    Item { var: "LEONIDA_UPSCALE", label: "UPSCALER", values: &[("dlss", "DLSS"), ("stretch", "STRETCH"), ("", "GAME OWN (S: FSR SLOW)")], live: false },
    // DLSS's mode is its render height: of the output's lines 2/3 (quality), 0.6 (balanced), 1/2 (performance), 1/3
    // (ultra performance). Unset: Series S renders its own 2/3; the launcher gives Series X 2/3 with DLSS.
    Item { var: "LEONIDA_RENDER_SCALE", label: "RENDER SCALE (DLSS MODE)", values: &[("", "GAME"), ("0.6667", "0.667 QUALITY"), ("0.6", "0.6 BALANCED"), ("0.5", "0.5 PERFORMANCE"), ("0.3333", "0.333 ULTRA PERF."), ("0.75", "0.75")], live: false },
    // On Series X the launcher turns DLSS on with it: the generator's depth and motion vectors come from that upscale.
    Item { var: "LEONIDA_FRAME_GEN", label: "FRAME GEN (NOT WITH RAY TR.)", values: &[("", "OFF"), ("2", "2X"), ("3", "3X"), ("4", "4X"), ("5", "5X"), ("6", "6X (MOST)")], live: false },
    // The window's shape, and the game's 16:9 picture in a window of another shape (`crate::aspect`).
    Item { var: "LEONIDA_ASPECT", label: "WINDOW SHAPE", values: &[("", "16:9 (GAME)"), ("21:9", "21:9"), ("32:9", "32:9"), ("auto", "AS THE SCREEN")], live: false },
    Item { var: "LEONIDA_PRESENT_SCALE", label: "PICTURE IN ANOTHER SHAPE", values: &[("", "BY WINDOW SHAPE"), ("fit", "BARS"), ("stretch", "STRETCH")], live: true },
    Item { var: "LEONIDA_FPS_LINE", label: "FPS AT THE TOP", values: &[("1", "ON"), ("0", "OFF")], live: true },
    // The title's own pace is 60 with the loader's frame-rate patch (`leonida-loader`'s `vipatch.rs`), which is always on.
    Item { var: "LEONIDA_FPS_CAP", label: "FRAME RATE CAP", values: &[("", "GAME (60)"), ("30", "30"), ("60", "60"), ("120", "120"), ("0", "NONE")], live: true },
    Item { var: "LEONIDA_BVH_BUILDER", label: "RAY TRACING (REAL LIGHTING)", values: &[("", "OFF"), ("1", "ON")], live: false },
    Item { var: "LEONIDA_DLSS_PRESET", label: "DLSS MODEL", values: &[("", "DEFAULT (K)"), ("K", "K TRANSFORMER"), ("J", "J TRANSFORMER"), ("M", "M TRANSFORMER"), ("L", "L TRANSFORMER")], live: false },
    Item { var: "LEONIDA_DLSS_SHARPNESS", label: "DLSS SHARPNESS", values: &[("0", "0"), ("0.25", "0.25"), ("0.5", "0.5"), ("0.75", "0.75"), ("1", "1")], live: true },
    Item { var: "LEONIDA_MENU_SMOOTH_MOTION", label: "SMOOTH MOTION", values: &[("1", "ON"), ("0", "OFF")], live: false },
    Item { var: "LEONIDA_MENU_CONSOLE", label: "CONSOLE", values: &[("series-s", "SERIES S"), ("series-x", "SERIES X")], live: false },
    Item { var: "LEONIDA_ADAPTER", label: "GRAPHICS CARD", values: &[("", "WINDOWS DEFAULT"), ("#0", "CARD 0"), ("#1", "CARD 1"), ("#2", "CARD 2")], live: false },
    Item { var: "LEONIDA_PANEL", label: "STATS PANEL (F3)", values: UNSET_ON, live: true },
    Item { var: "LEONIDA_RELAY", label: "RELAY (SPEED, TRIANGLES)", values: &[("2", "2"), ("1", "1"), ("0", "0 OFF")], live: false },
    Item { var: "LEONIDA_RELAY_COPY", label: "RELAY COPY QUEUE", values: OFF_ON, live: false },
    Item { var: "LEONIDA_FETCH_CHECK", label: "FETCH CHECK", values: OFF_ON, live: false },
    Item { var: "LEONIDA_WIDE_BACK", label: "WIDE COPY BACK", values: OFF_ON, live: false },
    Item { var: "LEONIDA_CARD_STEPS", label: "CARD STEPS", values: &[("2", "2"), ("1", "1"), ("0", "0 OFF")], live: false },
    Item { var: "LEONIDA_TEXTURE_NO_WAIT", label: "TEXTURE NO WAIT", values: OFF_ON, live: false },
    Item { var: "LEONIDA_HEAP_RECHECK_EVERY", label: "TEXTURE RECHECK (OIL TEST)", values: &[("", "OFF"), ("60", "EVERY 60 FRAMES"), ("15", "EVERY 15 FRAMES")], live: false },
    Item { var: "LEONIDA_NULL_SAMPLER_LINEAR", label: "LINEAR STAND-IN SAMPLER", values: UNSET_ON, live: false },
    Item { var: "LEONIDA_AMBIENT", label: "AMBIENT STAND-IN", values: OFF_ON, live: false },
    Item { var: "LEONIDA_AMBIENT_AIR", label: "FOG CAP", values: &[("1.5", "1.5"), ("1", "1"), ("2", "2"), ("", "NONE")], live: false },
    Item { var: "LEONIDA_NAN_SCRUB", label: "NAN SCRUB (SHADERS REBUILD)", values: OFF_ON, live: false },
    Item { var: "LEONIDA_THIN", label: "THIN", values: OFF_ON, live: false },
    Item { var: "LEONIDA_JOIN_PARTS", label: "JOIN PARTS", values: &[("8", "8"), ("4", "4"), ("16", "16"), ("1", "1")], live: false },
    Item { var: "LEONIDA_JOIN_COPIES", label: "JOIN COPIES", values: OFF_ON, live: false },
    Item { var: "LEONIDA_UPLOAD_AHEAD", label: "UPLOAD AHEAD", values: OFF_ON, live: false },
    Item { var: "LEONIDA_INDIRECT_WINDOW", label: "INDIRECT WINDOW", values: OFF_ON, live: false },
    Item { var: "LEONIDA_INDIRECT_BATCH", label: "INDIRECT BATCH", values: &[("2", "2"), ("1", "1"), ("0", "0 OFF")], live: false },
    Item { var: "LEONIDA_CARD_REHASH_RUNS", label: "CARD REHASH RUNS", values: OFF_ON, live: false },
    Item { var: "LEONIDA_MAP_THREADS", label: "MAP THREADS", values: OFF_ON, live: false },
    Item { var: "LEONIDA_TITLE_PRIORITIES", label: "TITLE THREAD PRIORITIES", values: OFF_ON, live: false },
    Item { var: "LEONIDA_CARD_AWAKE", label: "KEEP CARD AWAKE", values: OFF_ON, live: false },
    Item { var: "LEONIDA_SHOW_AFTER", label: "SHOW AFTER", values: OFF_ON, live: false },
    Item { var: "LEONIDA_PIPELINE_WAIT_MS", label: "SHADER WAIT MS", values: &[("10000", "10000"), ("30000", "30000"), ("1000", "1000"), ("0", "0 SKIP")], live: false },
    Item { var: "LEONIDA_DYNAMIC_SLOTS", label: "DESCRIPTOR SLOTS", values: &[("524288", "524288"), ("262144", "262144"), ("1000000", "1000000")], live: false },
    Item { var: "LEONIDA_CARD_BUFFERS_MB", label: "CARD BUFFERS MB", values: &[("1536", "1536"), ("1024", "1024"), ("2048", "2048"), ("512", "512"), ("0", "0 OFF")], live: false },
    Item { var: "LEONIDA_OWN_UPSCALER", label: "OWN UPSCALER", values: UNSET_ON, live: false },
    // The game patches after VI-Patch that have a switch (docs/vi_patch.md); the loader reads them when it maps the title.
    Item { var: "LEONIDA_VIPATCH_NO_VIGNETTE", label: "NO VIGNETTE (VI-PATCH)", values: UNSET_ON, live: false },
    Item { var: "LEONIDA_D3D12_DRED", label: "CRASH BREADCRUMBS", values: &[("light", "LIGHT"), ("", "OFF")], live: false },
];

/// The lines after the settings.
const ACTIONS: [&str; 4] = ["SAVE AND RESTART NOW", "SAVE FOR NEXT START", "RESET ALL TO PLAY DEFAULTS", "CLOSE"];

fn get(var: &str) -> String {
    std::env::var(var).unwrap_or_default()
}

/// Whether the frame-rate line is shown at the top while the menu is closed
/// (`LEONIDA_FPS_LINE=0`: not; the menu changes it).
fn fps_line_on() -> &'static AtomicBool {
    static ON: OnceLock<AtomicBool> = OnceLock::new();
    ON.get_or_init(|| AtomicBool::new(std::env::var_os("LEONIDA_FPS_LINE").is_none_or(|v| v != "0")))
}

/// That line: the frames rendered a second and, while frame generation shows more, those shown.
fn fps_line() -> Option<String> {
    if !fps_line_on().load(Ordering::Relaxed) {
        return None;
    }
    // The stats panel has that corner (diagnostics builds, F3), and so has the trainer menu.
    #[cfg(feature = "diagnostics")]
    if super::panel::shown().load(Ordering::Relaxed) {
        return None;
    }
    if leonida_menu::work() & leonida_menu::DRAW != 0 {
        return None;
    }
    let fps = f64::from(super::METER_FPS_TENTHS.load(Ordering::Relaxed)) / 10.0;
    if fps <= 0.0 {
        return None;
    }
    let line = match crate::fg::shown_per_frame() {
        1 => format!("FPS: {fps:.1}"),
        per => format!("FPS: {fps:.1}  FG: {:.1}", fps * f64::from(per)),
    };
    // While host pipelines are still being made (minutes after a change of the translator or the driver):
    // frames wait for them and things drawn with them are missing, so the player is told how many are left.
    Some(match crate::pipeline::making() {
        0 => line,
        left => format!("{line}  SHADERS COMPILING: {left} LEFT"),
    })
}

/// Whether a change of `item` shows at once.
fn live(item: &Item) -> bool {
    item.live || (item.var == "LEONIDA_FRAME_GEN" && crate::fg::active())
}

/// The index of `value` among the item's values (an unknown value: shown as
/// itself, at the end).
fn index_of(item: &Item, value: &str) -> Option<usize> {
    item.values.iter().position(|(v, _)| *v == value)
}

/// Keys pressed, gathered on a thread of their own every 10 ms: a frame
/// lasts 50 ms and more, and a short press would fall between two frames.
fn keys() -> &'static Mutex<Vec<u16>> {
    static KEYS: OnceLock<Mutex<Vec<u16>>> = OnceLock::new();
    KEYS.get_or_init(|| {
        let _ = std::thread::Builder::new().name("settings menu keys".into()).spawn(|| {
            const WATCHED: [u16; 7] = [VK_F1, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_ESCAPE];
            let mut was = [false; WATCHED.len()];
            loop {
                std::thread::sleep(Duration::from_millis(10));
                let mut process = 0;
                // SAFETY: plain queries; room for the answer.
                unsafe { GetWindowThreadProcessId(GetForegroundWindow(), &mut process) };
                let ours = process == std::process::id();
                for (i, &vk) in WATCHED.iter().enumerate() {
                    // SAFETY: a plain query.
                    let down = ours && unsafe { GetAsyncKeyState(i32::from(vk)) } < 0;
                    if down && !was[i] {
                        keys().lock().unwrap_or_else(|e| e.into_inner()).push(vk);
                    }
                    was[i] = down;
                }
            }
        });
        Mutex::new(Vec::new())
    })
}

/// The box's size in pixels.
const fn size(scale: u32) -> (u32, u32) {
    ((COLUMNS * CELL.0 + 2 * RIM) * scale, (LINES * CELL.1 + 2 * RIM) * scale)
}

fn lit(lines: &[String], x: u32, y: u32) -> bool {
    let (Some(x), Some(y)) = (x.checked_sub(RIM), y.checked_sub(RIM)) else {
        return false;
    };
    if x % CELL.0 >= GLYPH.0 || y % CELL.1 >= GLYPH.1 || x / CELL.0 >= COLUMNS {
        return false;
    }
    let Some(&c) = lines.get((y / CELL.1) as usize).and_then(|l| l.as_bytes().get((x / CELL.0) as usize)) else {
        return false;
    };
    (glyph(c.to_ascii_uppercase())[(y % CELL.1) as usize] >> (GLYPH.0 - 1 - x % CELL.0)) & 1 != 0
}

fn painted(lines: &[String], scale: u32, colours: [&[u8]; 2], pitch: usize) -> Vec<u8> {
    let (width, height) = size(scale);
    let bytes = colours[0].len();
    let mut out = vec![0; pitch * height as usize];
    for y in 0..height / scale {
        let top = pitch * (y * scale) as usize;
        for x in 0..width / scale {
            let colour = colours[usize::from(lit(lines, x, y))];
            for i in 0..scale {
                let at = top + (x * scale + i) as usize * bytes;
                out[at..at + bytes].copy_from_slice(colour);
            }
        }
        for i in 1..scale as usize {
            out.copy_within(top..top + pitch, top + i * pitch);
        }
    }
    out
}

struct Buffers {
    format: DXGI_FORMAT,
    scale: u32,
    pitch: u32,
    uploads: Vec<(ID3D12Resource, *mut u8)>,
    card: ID3D12Resource,
    turn: usize,
    fresh: bool,
    /// The card's buffer was copied into before (it is in the copy-source state;
    /// at first it is in the common state, promoted by the first copy).
    used: bool,
}

// SAFETY: free-threaded resources, and memory only written under the presenter's lock.
unsafe impl Send for Buffers {}

impl Buffers {
    fn new(host: &Host, format: DXGI_FORMAT, scale: u32) -> Option<Buffers> {
        let (width, height) = size(scale);
        let pitch = (width * colours(format)?[0].len() as u32).next_multiple_of(256);
        let bytes = u64::from(pitch) * u64::from(height);
        let uploads = (0..UPLOADS).map(|_| host.upload_buffer(bytes)).collect::<Option<Vec<_>>>()?;
        let heap = D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_DEFAULT, ..Default::default() };
        let desc = D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
            Width: bytes,
            Height: 1,
            DepthOrArraySize: 1,
            MipLevels: 1,
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            ..Default::default()
        };
        let mut card: Option<ID3D12Resource> = None;
        // SAFETY: plain creation of a buffer.
        unsafe {
            host.device
                .CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, None, &mut card)
                .ok()?;
        }
        Some(Buffers { format, scale, pitch, uploads, card: card?, turn: 0, fresh: false, used: false })
    }

    fn write(&mut self, lines: &[String]) {
        let Some(colours) = colours(self.format) else { return };
        let picture = painted(lines, self.scale, colours, self.pitch as usize);
        self.turn = (self.turn + 1) % UPLOADS;
        // SAFETY: a mapped upload buffer of the picture's size, which the card read frames ago.
        unsafe { std::ptr::copy_nonoverlapping(picture.as_ptr(), self.uploads[self.turn].1, picture.len()) };
        self.fresh = true;
    }
}

/// The menu of a presenter.
pub(super) struct Menu {
    /// The values chosen, by item (as at start until changed).
    values: Vec<String>,
    /// The values at start.
    started: Vec<String>,
    /// The line picked: an item, or an action after them.
    at: usize,
    note: String,
    changed: bool,
    /// What the buffer holds: the frame-rate line, or (none) the menu's lines.
    painted: Option<String>,
    buffers: Option<Buffers>,
    failed: bool,
}

impl Menu {
    pub(super) fn new() -> Menu {
        // Start the key thread now: F1 must be seen while the menu is closed.
        let _ = keys();
        let values: Vec<String> = ITEMS.iter().map(|i| get(i.var)).collect();
        Menu { started: values.clone(), values, at: 0, note: String::new(), changed: true, painted: None, buffers: None, failed: false }
    }

    fn open() -> bool {
        crate::SETTINGS_MENU_OPEN.load(Ordering::Relaxed)
    }

    fn lines(&self) -> Vec<String> {
        let total = ITEMS.len() + ACTIONS.len();
        let first = self.at.saturating_sub(ROWS / 2).min(total.saturating_sub(ROWS));
        let mut out = vec![
            "SETTINGS                       F1 OR ESC CLOSES".to_owned(),
            "UP/DOWN PICK  LEFT/RIGHT CHANGE  ENTER DOES IT".to_owned(),
            String::new(),
        ];
        for row in first..(first + ROWS).min(total) {
            let cursor = if row == self.at { '>' } else { ' ' };
            let line = if let Some(item) = ITEMS.get(row) {
                let value = &self.values[row];
                let shown = index_of(item, value).map_or(value.to_ascii_uppercase(), |i| item.values[i].1.to_owned());
                let mark = if *value != self.started[row] { if live(item) { " *LIVE" } else { " *" } } else if live(item) { " LIVE" } else { "" };
                format!("{cursor} {:<27} < {shown} >{mark}", item.label)
            } else {
                format!("{cursor} {}", ACTIONS[row - ITEMS.len()])
            };
            out.push(line.chars().take(COLUMNS as usize).collect());
        }
        while out.len() < 3 + ROWS {
            out.push(String::new());
        }
        out.push(String::new());
        out.push(if self.note.is_empty() { "* = CHANGED: APPLIES AT THE NEXT START UNLESS LIVE".to_owned() } else { self.note.clone() });
        out
    }

    /// Changes item `i` by `step` (+1 / -1) through its values.
    fn change(&mut self, i: usize, step: isize) {
        let item = &ITEMS[i];
        let n = item.values.len() as isize;
        let now = index_of(item, &self.values[i]).map_or(-1, |x| x as isize);
        let next = if now < 0 { if step > 0 { 0 } else { n - 1 } } else { (now + step).rem_euclid(n) };
        self.values[i] = item.values[next as usize].0.to_owned();
        if live(item) {
            self.apply_live(i);
        }
        self.note.clear();
    }

    /// With a start that had frame generation on, its amount (and off) changes while running;
    /// from a start without it, turning it on needs the restart (Streamline's swap chain).
    fn apply_live(&self, i: usize) {
        let (var, value) = (ITEMS[i].var, &self.values[i]);
        // SAFETY: the variables changed here are read by this build with
        // std::env::var at each use; Windows' environment block is locked by
        // the process heap's routines, and std holds its own lock.
        unsafe {
            if value.is_empty() {
                std::env::remove_var(var);
            } else {
                std::env::set_var(var, value);
            }
        }
        if var == "LEONIDA_FPS_CAP" {
            crate::frame::set_cap(value);
        }
        if var == "LEONIDA_FPS_LINE" {
            fps_line_on().store(value != "0", Ordering::Relaxed);
        }
        if var == "LEONIDA_PRESENT_SCALE" {
            crate::aspect::changed();
        }
        if var == "LEONIDA_PANEL" {
            #[cfg(feature = "diagnostics")]
            super::panel::shown().store(!value.is_empty(), Ordering::Relaxed);
        }
    }

    /// Leonida: the settings that change while running are shown as they are
    /// now when the menu opens: F3 and the trainer menu (`super::trainer`)
    /// change the panel, the frame-rate cap and DLSS's sharpness too.
    fn refresh_live(&mut self) {
        for (i, item) in ITEMS.iter().enumerate() {
            if !item.live {
                continue;
            }
            self.values[i] = match item.var {
                #[cfg(feature = "diagnostics")]
                "LEONIDA_PANEL" if super::panel::shown().load(Ordering::Relaxed) => "1".to_owned(),
                "LEONIDA_PANEL" => String::new(),
                var => get(var),
            };
        }
    }

    /// Writes the changed values over those in the settings file.
    fn save(&mut self) -> bool {
        let Some(path) = std::env::var_os("LEONIDA_MENU_FILE") else {
            self.note = "NO SETTINGS FILE: START THE GAME WITH PLAY.PS1".to_owned();
            return false;
        };
        let mut kept: Vec<(String, String)> = std::fs::read_to_string(&path)
            .unwrap_or_default()
            .lines()
            .filter_map(|l| l.split_once('=').map(|(k, v)| (k.trim().to_owned(), v.trim().to_owned())))
            .filter(|(k, _)| k.starts_with("LEONIDA_"))
            .collect();
        for (i, item) in ITEMS.iter().enumerate() {
            if self.values[i] == self.started[i] {
                continue;
            }
            kept.retain(|(k, _)| k != item.var);
            kept.push((item.var.to_owned(), self.values[i].clone()));
        }
        let mut text = String::from("# Saved by the F1 menu; play.ps1 applies these after its own settings (an empty value: unset).\r\n");
        for (k, v) in &kept {
            text.push_str(&format!("{k}={v}\r\n"));
        }
        match std::fs::write(&path, text) {
            Ok(()) => {
                tracing::info!(path = %std::path::Path::new(&path).display(), "settings menu: saved");
                self.started = self.values.clone();
                self.note = "SAVED. APPLIES AT THE NEXT START".to_owned();
                true
            }
            Err(e) => {
                tracing::warn!(error = %e, "settings menu: not saved");
                self.note = "COULD NOT SAVE THE SETTINGS FILE".to_owned();
                false
            }
        }
    }

    /// Starts play.ps1 again in a new console, a few seconds on, and ends this run.
    fn restart(&mut self) {
        use std::os::windows::process::CommandExt;
        let Some(play) = std::env::var_os("LEONIDA_MENU_PLAY") else {
            self.note = "SAVED. RESTART PLAY.PS1 YOURSELF".to_owned();
            return;
        };
        let play = play.to_string_lossy().replace('\'', "''");
        let args = get("LEONIDA_MENU_ARGS");
        let command = format!("Start-Sleep 3; & '{play}' {args}");
        const CREATE_NEW_CONSOLE: u32 = 0x10;
        match std::process::Command::new("powershell.exe")
            .args(["-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", &command])
            .creation_flags(CREATE_NEW_CONSOLE)
            .spawn()
        {
            Ok(_) => {
                tracing::info!(command, "settings menu: restarting");
                // Ended at once: std::process::exit runs the DLLs' unloading, and with Streamline
                // loaded that never came back (11/26: a windowless leftover with one thread
                // and 27 GB, which also kept the new build from being put in place).
                // SAFETY: this process, with nothing of its own left to save.
                unsafe {
                    use windows_sys::Win32::System::Threading::{GetCurrentProcess, TerminateProcess};
                    TerminateProcess(GetCurrentProcess(), 0);
                }
                std::process::exit(0);
            }
            Err(e) => {
                tracing::warn!(error = %e, "settings menu: restart failed");
                self.note = "SAVED, BUT THE RESTART FAILED".to_owned();
            }
        }
    }

    /// Empties the settings file: play.ps1's own settings at the next start.
    fn reset(&mut self) {
        if let Some(path) = std::env::var_os("LEONIDA_MENU_FILE") {
            let _ = std::fs::remove_file(path);
        }
        self.note = "RESET. PLAY DEFAULTS AT THE NEXT START".to_owned();
    }

    /// A frame was presented: the keys pressed since the last one.
    pub(super) fn frame(&mut self) {
        let pressed = std::mem::take(&mut *keys().lock().unwrap_or_else(|e| e.into_inner()));
        if pressed.is_empty() {
            return;
        }
        let total = ITEMS.len() + ACTIONS.len();
        for vk in pressed {
            let open = Self::open();
            if vk == VK_F1 || (open && vk == VK_ESCAPE) {
                crate::SETTINGS_MENU_OPEN.store(!open && vk == VK_F1, Ordering::Relaxed);
                if !open && vk == VK_F1 {
                    self.refresh_live();
                }
                self.note.clear();
                self.changed = true;
                continue;
            }
            if !open {
                continue;
            }
            match vk {
                VK_UP => self.at = (self.at + total - 1) % total,
                VK_DOWN => self.at = (self.at + 1) % total,
                VK_LEFT if self.at < ITEMS.len() => self.change(self.at, -1),
                VK_RIGHT if self.at < ITEMS.len() => self.change(self.at, 1),
                VK_RETURN if self.at < ITEMS.len() => self.change(self.at, 1),
                VK_RETURN => match self.at - ITEMS.len() {
                    0 => {
                        if self.save() {
                            self.restart();
                        }
                    }
                    1 => {
                        self.save();
                    }
                    2 => self.reset(),
                    _ => crate::SETTINGS_MENU_OPEN.store(false, Ordering::Relaxed),
                },
                _ => {}
            }
            self.changed = true;
        }
    }

    /// Records the copy of the box onto `back` (in the copy-destination state).
    pub(super) fn draw(&mut self, host: &Host, list: &HostList, back: &ID3D12Resource, (width, height): (u32, u32), format: DXGI_FORMAT) {
        if self.failed {
            return;
        }
        // Closed: the frame-rate line in the top left corner, if it is wanted and there is a rate.
        let open = Self::open();
        let line = if open { None } else { fps_line() };
        if !open && line.is_none() {
            return;
        }
        // Our own copies are not part of a recorded frame (the recorder is in diagnostics builds).
        #[cfg(feature = "diagnostics")]
        let _ours = crate::replay::record::unrecorded();
        let scale = (height / 540).max(1);
        if self.buffers.as_ref().is_none_or(|b| (b.format, b.scale) != (format, scale)) {
            std::mem::forget(self.buffers.take());
            self.buffers = Buffers::new(host, format, scale);
            self.failed = self.buffers.is_none();
            self.changed = true;
        }
        if self.painted != line {
            self.painted = line.clone();
            self.changed = true;
        }
        let lines = if self.changed { Some(line.as_ref().map_or_else(|| self.lines(), |l| vec![l.clone()])) } else { None };
        let Some(buffers) = &mut self.buffers else { return };
        if let Some(lines) = lines {
            self.changed = false;
            buffers.write(&lines);
        }
        // The whole box in the middle, or of it the first line's part in the corner.
        let (w, h) = match &line {
            Some(l) => ((l.len() as u32 * CELL.0 + 2 * RIM) * scale, (CELL.1 + 2 * RIM) * scale),
            None => size(scale),
        };
        let (w, h) = (w.min(width), h.min(height));
        let (x, y) = if line.is_some() { (0, 0) } else { ((width - w) / 2, (height - h) / 2) };
        let list = &list.list;
        // SAFETY: an open list and live resources; the box lies within both.
        unsafe {
            if std::mem::take(&mut buffers.fresh) {
                if std::mem::replace(&mut buffers.used, true) {
                    list.ResourceBarrier(&[transition(&buffers.card, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST)]);
                }
                list.CopyResource(&buffers.card, &buffers.uploads[buffers.turn].0);
                list.ResourceBarrier(&[transition(&buffers.card, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE)]);
            }
            let from = D3D12_TEXTURE_COPY_LOCATION {
                pResource: std::mem::transmute_copy(&buffers.card),
                Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
                    PlacedFootprint: D3D12_PLACED_SUBRESOURCE_FOOTPRINT {
                        Offset: 0,
                        Footprint: D3D12_SUBRESOURCE_FOOTPRINT { Format: format, Width: size(scale).0, Height: size(scale).1, Depth: 1, RowPitch: buffers.pitch },
                    },
                },
            };
            let to = D3D12_TEXTURE_COPY_LOCATION {
                pResource: std::mem::transmute_copy(back),
                Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: 0 },
            };
            let part = D3D12_BOX { left: 0, top: 0, front: 0, right: w, bottom: h, back: 1 };
            list.CopyTextureRegion(&to, x, y, 0, &from, Some(&part));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_line_fits_and_every_character_has_a_picture() {
        let mut menu = Menu { values: ITEMS.iter().map(|_| String::new()).collect(), started: Vec::new(), at: 0, note: String::new(), changed: false, painted: None, buffers: None, failed: false };
        menu.started = menu.values.clone();
        for at in 0..ITEMS.len() + ACTIONS.len() {
            menu.at = at;
            for line in menu.lines() {
                assert!(line.len() <= COLUMNS as usize, "{line}");
                for c in line.bytes() {
                    assert_ne!(glyph(c.to_ascii_uppercase()), glyph(b'#'), "{line}: {}", c as char);
                }
            }
        }
    }

    #[test]
    fn values_cycle_both_ways() {
        let mut menu = Menu { values: ITEMS.iter().map(|i| i.values[0].0.to_owned()).collect(), started: Vec::new(), at: 0, note: String::new(), changed: false, painted: None, buffers: None, failed: false };
        menu.started = menu.values.clone();
        let n = ITEMS[0].values.len();
        for _ in 0..n {
            menu.change(0, 1);
        }
        assert_eq!(menu.values[0], ITEMS[0].values[0].0);
        menu.change(0, -1);
        assert_eq!(menu.values[0], ITEMS[0].values[n - 1].0);
    }
}
