//! Run-time settings the menu changes and the rest of the runtime reads.
//! What the menu keeps itself is a plain atomic (the menu's size, the
//! title's clock rate). What something else owns already (the renderer's
//! frame-rate cap, DLSS's sharpness, the numbers panel, the frame recorder)
//! is an [`Owned`] setting: its owner says how it is read and set, and both
//! menus (this one and the renderer's F1 settings) then show and change the
//! same thing. What a present has to do for the menu is one word ([`work`]).

use std::path::PathBuf;
use std::sync::Mutex;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

/// An `f32` in an atomic.
pub struct F32(AtomicU32);

impl F32 {
    pub const fn new(v: f32) -> F32 {
        F32(AtomicU32::new(v.to_bits()))
    }
    pub fn get(&self) -> f32 {
        f32::from_bits(self.0.load(Ordering::Relaxed))
    }
    pub fn set(&self, v: f32) {
        self.0.store(v.to_bits(), Ordering::Relaxed);
    }
}

/// What a present has to do for the menu, as bits ([`DRAW`], [`SHOT`]).
static WORK: AtomicU32 = AtomicU32::new(0);
/// [`work`]: the menu is open and to be drawn.
pub const DRAW: u32 = 1;
/// [`work`]: a screenshot is asked for.
pub const SHOT: u32 = 2;
/// [`work`]: a Lua mod's overlay (`crate::overlay`) has something to draw.
pub const LUA_DRAW: u32 = 4;

/// What a present has to do for the menu: 0 while it is closed and no
/// screenshot is asked for, so that such a present pays this one load.
#[inline]
pub fn work() -> u32 {
    WORK.load(Ordering::Relaxed)
}

pub(crate) fn set_work(bit: u32, on: bool) {
    if on {
        WORK.fetch_or(bit, Ordering::AcqRel);
    } else {
        WORK.fetch_and(!bit, Ordering::AcqRel);
    }
}

/// Keys (virtual-key codes 0 to 255) the Lua modding API holds down for the
/// title, as `LEONIDA_KEYS_FILE` does for a test; the input shim adds them
/// to what it reports ([`injected_keys`]).
static INJECTED: [AtomicU64; 4] = [
    AtomicU64::new(0),
    AtomicU64::new(0),
    AtomicU64::new(0),
    AtomicU64::new(0),
];
static INJECTING: AtomicBool = AtomicBool::new(false);

/// Holds exactly these keys down for the title (virtual-key codes); an empty
/// list lets them all go.
pub fn set_injected_keys(keys: &[u8]) {
    let mut mask = [0u64; 4];
    for &vk in keys {
        mask[usize::from(vk >> 6)] |= 1 << (vk & 63);
    }
    for (slot, bits) in INJECTED.iter().zip(mask) {
        slot.store(bits, Ordering::Release);
    }
    INJECTING.store(mask != [0; 4], Ordering::Release);
}

/// The keys held down for the title now (none unless a mod holds some).
pub fn injected_keys() -> Vec<u8> {
    if !INJECTING.load(Ordering::Acquire) {
        return Vec::new();
    }
    (0..=255u8)
        .filter(|&vk| INJECTED[usize::from(vk >> 6)].load(Ordering::Acquire) >> (vk & 63) & 1 != 0)
        .collect()
}

/// How an [`Owned`] setting is read and set; a setter says why it could not.
type Access<T> = (fn() -> T, fn(T) -> Result<(), String>);

/// A setting something else owns: the owner says how it is read and set
/// ([`Owned::provide`]); until then the menu shows it as unavailable. The
/// functions are called on the menu's thread.
pub struct Owned<T: 'static> {
    access: OnceLock<Access<T>>,
}

impl<T> Owned<T> {
    pub const fn new() -> Owned<T> {
        Owned {
            access: OnceLock::new(),
        }
    }
    /// The owner is there (once; later calls change nothing).
    pub fn provide(&self, get: fn() -> T, set: fn(T) -> Result<(), String>) {
        let _ = self.access.set((get, set));
    }
    pub fn there(&self) -> bool {
        self.access.get().is_some()
    }
    /// The value now; `None` without an owner.
    pub fn get(&self) -> Option<T> {
        self.access.get().map(|(get, _)| get())
    }
    pub fn set(&self, value: T) -> Result<(), String> {
        match self.access.get() {
            Some((_, set)) => set(value),
            None => Err("Not available in this run.".into()),
        }
    }
}

impl<T> Default for Owned<T> {
    fn default() -> Self {
        Owned::new()
    }
}

/// Tests that change these settings take turns.
/// What the runtime can do with the local player by reading and writing the
/// title's memory (`apps/leonida/src/player.rs`), for the Teleport page and
/// `leonida.teleport`. Set once by the runtime; without it every teleport
/// says it is not available.
pub struct PlayerHook {
    /// `Ok` when the player's position can be found now, else why not.
    pub ready: fn() -> Result<(), String>,
    /// Where the player is, in the title's world units (metres).
    pub position: fn() -> Result<[f32; 3], String>,
    /// The horizontal direction the player faces, as a unit vector (x, y).
    pub heading: fn() -> Option<[f32; 2]>,
    /// Moves the player there and keeps it there for `hold_ms` milliseconds
    /// (so that the physics and the unloaded ground do not take it back or
    /// let it fall); a line for the menu.
    pub teleport: fn(to: [f32; 3], hold_ms: u32) -> Result<String, String>,
    /// A named place (`"vice"`) the user has told the runtime about.
    pub place: fn(name: &str) -> Option<[f32; 3]>,
}

static PLAYER: OnceLock<PlayerHook> = OnceLock::new();

/// The runtime can move the player (once; later calls change nothing).
pub fn provide_player(hook: PlayerHook) {
    let _ = PLAYER.set(hook);
}

/// The player hook, when the runtime provided one.
pub fn player() -> Option<&'static PlayerHook> {
    PLAYER.get()
}

/// What the runtime can do in the title itself, through the title's own code on the title's own thread
/// (`apps/leonida/src/game_actions.rs`, `LEONIDA_GAME_ACTIONS=1`): wanted level, time of day, weather, the
/// title's cheats. Each answers `None` for an item that is not its own, and the menu then asks elsewhere.
pub struct GameHook {
    /// `Ok` when the item can work now, else why not.
    pub available: fn(id: &str) -> Option<Result<(), String>>,
    /// The item's value now.
    pub value: fn(id: &str) -> Option<crate::model::Value>,
    /// Does it (asks the title's thread to): a line for the menu.
    pub apply: fn(id: &str, value: crate::model::Value) -> Option<Result<Option<String>, String>>,
}

static GAME: OnceLock<GameHook> = OnceLock::new();

/// The runtime can act in the title (once; later calls change nothing).
pub fn provide_game(hook: GameHook) {
    let _ = GAME.set(hook);
}

/// The game hook, when the runtime provided one.
pub fn game() -> Option<&'static GameHook> {
    GAME.get()
}

/// Why nothing can be done with the player.
pub const NO_PLAYER: &str = "Not available: this run has no way to find the player in the title's memory.";

#[cfg(test)]
pub(crate) static TEST_LOCK: Mutex<()> = Mutex::new(());

/// The frame-rate cap (`LEONIDA_FPS_CAP`, the renderer's frame clock).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Cap {
    /// The title's own pace (the variable unset).
    Title,
    /// None (`0`).
    Unlimited,
    /// This many frames a second at most.
    Fps(u32),
}

/// The FPS and memory panel (diagnostics builds; F3 flips it too).
pub static PANEL: Owned<bool> = Owned::new();
/// The frame recorder (diagnostics builds with `LEONIDA_CAPTURE`): every
/// presented frame saved while on.
pub static RECORD: Owned<bool> = Owned::new();
/// The frame-rate cap: the renderer's own, which its F1 menu changes too.
pub static FPS_CAP: Owned<Cap> = Owned::new();
/// DLSS's sharpness, 0 to 1 (`LEONIDA_DLSS_SHARPNESS`): there when the
/// title's upscale is DLSS.
pub static SHARPNESS: Owned<f32> = Owned::new();

/// Whether something else has the keyboard now (the renderer's F1 settings
/// menu while it is shown): the menu then reads the pads only.
static KEYBOARD_ELSEWHERE: OnceLock<fn() -> bool> = OnceLock::new();

pub fn provide_keyboard_elsewhere(taken: fn() -> bool) {
    let _ = KEYBOARD_ELSEWHERE.set(taken);
}

pub fn keyboard_elsewhere() -> bool {
    KEYBOARD_ELSEWHERE.get().is_some_and(|taken| taken())
}

/// The menu's size against its 1080-line layout.
pub static MENU_SCALE: F32 = F32::new(1.0);

/// A screenshot of the next frame, to be saved at this path.
static SCREENSHOT: Mutex<Option<PathBuf>> = Mutex::new(None);
/// Whether presents take the screenshots asked for (the renderer says so).
static SCREENSHOTS_THERE: AtomicBool = AtomicBool::new(false);

/// Presents take the screenshots asked for from here on.
pub fn provide_screenshots() {
    SCREENSHOTS_THERE.store(true, Ordering::Release);
}

pub fn screenshots_there() -> bool {
    SCREENSHOTS_THERE.load(Ordering::Acquire)
}

/// Asks for a screenshot of the next frame ([`work`] says [`SHOT`] until a
/// present takes it).
pub fn ask_screenshot(path: PathBuf) {
    *SCREENSHOT.lock().unwrap_or_else(|e| e.into_inner()) = Some(path);
    set_work(SHOT, true);
}

/// The path of the screenshot asked for, taken.
pub fn take_screenshot() -> Option<PathBuf> {
    let mut asked = SCREENSHOT.lock().unwrap_or_else(|e| e.into_inner());
    set_work(SHOT, false);
    asked.take()
}

/// The folder screenshots go to: `LEONIDA_SCREENSHOTS`, else `screenshots`
/// beside the runtime.
pub fn screenshot_folder() -> PathBuf {
    if let Some(dir) = std::env::var_os("LEONIDA_SCREENSHOTS") {
        return PathBuf::from(dir);
    }
    std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(|p| p.join("screenshots")))
        .unwrap_or_else(|| PathBuf::from("screenshots"))
}

/// The title's clock: what its `QueryPerformanceCounter` reads, at a rate
/// against the host's. Continuous: a change of rate starts from the value
/// read at that moment, so the title's time never jumps or goes back.
pub mod clock {
    use super::*;

    /// A sequence lock over (host count, title count, rate) at the last
    /// change: odd while being written.
    static SEQ: AtomicU64 = AtomicU64::new(0);
    static HOST_AT: AtomicU64 = AtomicU64::new(0);
    static TITLE_AT: AtomicU64 = AtomicU64::new(0);
    static RATE: AtomicU64 = AtomicU64::new(0x3FF0_0000_0000_0000); // 1.0
    /// Whether the rate was ever changed: until then the host's count is
    /// passed through untouched.
    static CHANGED: AtomicBool = AtomicBool::new(false);
    static WRITER: Mutex<()> = Mutex::new(());

    fn read() -> (i64, i64, f64) {
        loop {
            let s = SEQ.load(Ordering::Acquire);
            if s & 1 == 1 {
                std::hint::spin_loop();
                continue;
            }
            let v = (
                HOST_AT.load(Ordering::Acquire) as i64,
                TITLE_AT.load(Ordering::Acquire) as i64,
                f64::from_bits(RATE.load(Ordering::Acquire)),
            );
            if SEQ.load(Ordering::Acquire) == s {
                return v;
            }
        }
    }

    /// The title's count for host count `host`.
    pub fn title(host: i64) -> i64 {
        if !CHANGED.load(Ordering::Relaxed) {
            return host;
        }
        let (host_at, title_at, rate) = read();
        title_at + ((host - host_at) as f64 * rate) as i64
    }

    /// The rate now.
    pub fn rate() -> f64 {
        read().2
    }

    /// Sets the rate (clamped to 0.05 to 4), at host count `host`.
    pub fn set_rate(rate: f64, host: i64) {
        let _one = WRITER.lock().unwrap_or_else(|e| e.into_inner());
        let rate = if rate.is_finite() {
            rate.clamp(0.05, 4.0)
        } else {
            1.0
        };
        let now = title(host);
        SEQ.fetch_add(1, Ordering::AcqRel);
        HOST_AT.store(host as u64, Ordering::Release);
        TITLE_AT.store(now as u64, Ordering::Release);
        RATE.store(rate.to_bits(), Ordering::Release);
        SEQ.fetch_add(1, Ordering::AcqRel);
        CHANGED.store(true, Ordering::Release);
    }

    /// Whether the title's clock is ours (the runtime put it in).
    pub static HOOKED: AtomicBool = AtomicBool::new(false);

    /// The host's count now.
    #[cfg(windows)]
    pub fn host_now() -> i64 {
        let mut count = 0i64;
        // SAFETY: a valid out pointer.
        unsafe { windows_sys::Win32::System::Performance::QueryPerformanceCounter(&mut count) };
        count
    }

    /// The host's count now: nanoseconds since the first look.
    #[cfg(not(windows))]
    pub fn host_now() -> i64 {
        static START: OnceLock<std::time::Instant> = OnceLock::new();
        START
            .get_or_init(std::time::Instant::now)
            .elapsed()
            .as_nanos() as i64
    }

    #[cfg(test)]
    pub(crate) fn reset() {
        SEQ.store(0, Ordering::SeqCst);
        CHANGED.store(false, Ordering::SeqCst);
        RATE.store(1f64.to_bits(), Ordering::SeqCst);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_clock_passes_through_then_scales_without_jumps() {
        let _one = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        clock::reset();
        assert_eq!(clock::title(1000), 1000);
        clock::set_rate(0.5, 1000);
        assert_eq!(clock::title(1000), 1000);
        assert_eq!(clock::title(3000), 2000);
        clock::set_rate(2.0, 3000);
        assert_eq!(clock::title(3000), 2000);
        assert_eq!(clock::title(3500), 3000);
        clock::set_rate(1.0, 4000);
        assert_eq!(clock::title(4000), 4000);
        assert_eq!(clock::title(5000), 5000);
        assert_eq!(clock::rate(), 1.0);
        clock::set_rate(100.0, 5000);
        assert_eq!(clock::rate(), 4.0);
        clock::reset();
    }

    #[test]
    fn owned_settings_are_unavailable_until_provided_and_keep_their_first_owner() {
        static VALUE: AtomicU32 = AtomicU32::new(7);
        let s: Owned<u32> = Owned::new();
        assert!(!s.there() && s.get().is_none() && s.set(1).is_err());
        s.provide(
            || VALUE.load(Ordering::Relaxed),
            |v| {
                if v > 100 {
                    return Err("too much".into());
                }
                VALUE.store(v, Ordering::Relaxed);
                Ok(())
            },
        );
        s.provide(|| 0, |_| Ok(()));
        assert!(s.there());
        assert_eq!(s.get(), Some(7));
        assert_eq!(s.set(9), Ok(()));
        assert_eq!(s.get(), Some(9));
        assert_eq!(s.set(101), Err("too much".into()));
    }

    #[test]
    fn a_screenshot_asked_for_is_work_until_taken() {
        let _one = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        assert_eq!(work() & SHOT, 0);
        assert!(take_screenshot().is_none());
        ask_screenshot(PathBuf::from("a.png"));
        assert_eq!(work() & SHOT, SHOT);
        assert_eq!(take_screenshot(), Some(PathBuf::from("a.png")));
        assert_eq!(work() & SHOT, 0);
        assert!(take_screenshot().is_none());
    }

    #[test]
    fn injected_keys_are_held_until_released() {
        let _one = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        assert!(injected_keys().is_empty());
        set_injected_keys(&[b'W', 0x20, 0xA0, b'W']);
        assert_eq!(injected_keys(), [0x20, b'W', 0xA0]);
        set_injected_keys(&[b'D']);
        assert_eq!(injected_keys(), [b'D']);
        set_injected_keys(&[]);
        assert!(injected_keys().is_empty());
    }

    #[test]
    fn work_bits_are_set_and_cleared_apart() {
        let _one = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        set_work(DRAW, true);
        ask_screenshot(PathBuf::from("b.png"));
        assert_eq!(work(), DRAW | SHOT);
        set_work(DRAW, false);
        assert_eq!(work(), SHOT);
        take_screenshot();
        assert_eq!(work(), 0);
    }
}
