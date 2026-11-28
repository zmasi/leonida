//! The running menu: a thread of its own reads the keys and the pads,
//! moves the menu and paints it when it changes; a present asks what there
//! is to do ([`knobs::work`], one atomic load, all a present pays while the
//! menu is closed) and takes the newest picture when it is open
//! ([`frame`]). Closed, the thread looks at F4 and the pads sixty times a
//! second. `LEONIDA_MENU_OPEN=1` starts it open, for a test with no
//! keyboard.

use std::sync::atomic::{AtomicU16, AtomicU32, Ordering};
use std::sync::{Arc, Mutex, Once, OnceLock};
use std::time::{Duration, Instant};

use crate::actions::Player;
use crate::font::FontSource;
use crate::input::{Held, Keys, Withheld};
use crate::knobs;
use crate::model::{Input, Menu, View};
use crate::paint::{Fonts, Layers, paint};
use crate::tree::{Leonida, MODS, pages_with_mods};

/// Rows shown at once.
pub const VISIBLE: usize = 11;

/// Whether the menu is on: unless `LEONIDA_MENU=0`.
pub fn enabled() -> bool {
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_MENU").is_none_or(|v| v != "0"))
}

/// Whether the menu starts open on its first page: `LEONIDA_MENU_OPEN=1`
/// (a test that cannot press F4 sees it, and what it keeps from the title,
/// from the first present).
fn opens_at_start(value: Option<&std::ffi::OsStr>) -> bool {
    value.is_some_and(|v| v == "1")
}

/// Keys (bits of `input::KEYS`) and pad buttons (XInput's) kept from the title.
static WITHHELD_KEYS: AtomicU16 = AtomicU16::new(0);
static WITHHELD_PAD: AtomicU16 = AtomicU16::new(0);
/// The height of the picture presented, for the menu's scale.
static HEIGHT: AtomicU32 = AtomicU32::new(1080);

/// The newest picture of the menu and its number.
static PAINTED: Mutex<(u64, Option<Arc<Layers>>)> = Mutex::new((0, None));

/// Whether the menu is open.
pub fn is_open() -> bool {
    knobs::work() & knobs::DRAW != 0
}

/// Whether the title must not see virtual key `vk` now.
pub fn withheld_key(vk: u8) -> bool {
    let mask = WITHHELD_KEYS.load(Ordering::Relaxed);
    mask != 0 && crate::input::withheld(mask, vk)
}

/// The XInput buttons the title must not see now.
pub fn withheld_pad() -> u16 {
    WITHHELD_PAD.load(Ordering::Relaxed)
}

/// What a present draws.
pub struct Frame {
    /// Changes whenever the picture does.
    pub version: u64,
    pub layers: Arc<Layers>,
    /// Where its top-left corner goes on the picture, for an overlay that
    /// has its own place (the Lua mods'); `None`: the menu's corner.
    pub origin: Option<(i32, i32)>,
}

/// The height of the picture presented: the menu is painted for it. Told
/// when the presenter is made or its size changes, and by [`frame`].
pub fn set_height(height: u32) {
    if HEIGHT.load(Ordering::Relaxed) != height {
        HEIGHT.store(height, Ordering::Relaxed);
    }
}

/// The menu's picture while it is open, for a present of a picture
/// `height` lines high. A present asks [`knobs::work`] first and comes here
/// only when that says [`knobs::DRAW`].
pub fn frame(height: u32) -> Option<Frame> {
    set_height(height);
    if !is_open() {
        return None;
    }
    let painted = PAINTED.lock().unwrap_or_else(|e| e.into_inner());
    let layers = painted.1.clone()?;
    Some(Frame {
        version: painted.0,
        layers,
        origin: None,
    })
}

/// The height of the picture presented last told to [`set_height`].
pub(crate) fn height() -> u32 {
    HEIGHT.load(Ordering::Relaxed)
}

/// The scale for a picture `height` lines high.
pub fn scale_for(height: u32) -> f32 {
    (height.max(1) as f32 / 1080.0 * knobs::MENU_SCALE.get()).max(0.25)
}

/// Starts the menu's thread (once; nothing when disabled or off Windows).
/// The presenter calls it when it is made.
pub fn start() {
    static STARTED: Once = Once::new();
    if !enabled() {
        return;
    }
    STARTED.call_once(|| {
        #[cfg(windows)]
        {
            let _ = std::thread::Builder::new()
                .name("leonida-menu".into())
                .spawn(run);
        }
    });
}

/// The menu, its backend and the fonts at the scale painted last.
pub struct App {
    pub menu: Menu,
    pub backend: Leonida,
    fonts: Option<Fonts>,
    source: Box<dyn FontSource + Send>,
    last: Option<(View, f32)>,
    painted_at: Option<Instant>,
    /// The generation of the Lua mods' pages the menu was made from.
    mods_seen: u64,
}

impl App {
    pub fn new(source: Box<dyn FontSource + Send>) -> App {
        App {
            mods_seen: crate::mods::generation(),
            menu: Menu::new(pages_with_mods(), VISIBLE),
            backend: Leonida::new(Box::new(Player::default())),
            fonts: None,
            source,
            last: None,
            painted_at: None,
        }
    }

    /// Takes in the Lua mods' pages if they changed; whether they did.
    pub fn refresh_mods(&mut self) -> bool {
        let now = crate::mods::generation();
        if now == self.mods_seen {
            return false;
        }
        self.mods_seen = now;
        self.menu.replace_pages(pages_with_mods(), MODS + 1);
        true
    }

    /// Applies `input`; whether something changed.
    pub fn handle(&mut self, input: Input) -> bool {
        self.menu.handle(input, &mut self.backend)
    }

    /// A new picture if the view or the scale changed since the last.
    pub fn repaint(&mut self, scale: f32) -> Option<Layers> {
        let view = self.menu.view(&self.backend);
        if self
            .last
            .as_ref()
            .is_some_and(|(v, s)| *v == view && *s == scale)
        {
            return None;
        }
        if self.fonts.as_ref().is_none_or(|f| f.scale != scale) {
            self.fonts = Some(Fonts::new(&*self.source, scale));
        }
        let layers = paint(&view, self.fonts.as_ref()?);
        self.last = Some((view, scale));
        self.painted_at = Some(Instant::now());
        Some(layers)
    }
}

/// The system's fonts where there are, else the dot font.
pub fn system_fonts() -> Box<dyn FontSource + Send> {
    #[cfg(windows)]
    return Box::new(crate::gdi::Gdi);
    #[cfg(not(windows))]
    Box::new(crate::font::Dots)
}

/// A picture of the menu after `inputs` (it is opened first), at `scale`:
/// for previews.
pub fn preview(inputs: &[Input], scale: f32) -> Layers {
    let mut app = App::new(system_fonts());
    app.handle(Input::Toggle);
    for &i in inputs {
        app.handle(i);
    }
    app.repaint(scale).expect("a first picture")
}

/// The thread: samples, moves, paints.
#[cfg(windows)]
fn run() {
    let mut app = App::new(system_fonts());
    if opens_at_start(std::env::var_os("LEONIDA_MENU_OPEN").as_deref()) {
        app.handle(Input::Toggle);
    }
    let mut keys = Keys::default();
    let mut withheld = Withheld::default();
    let mut pads = crate::poll::Pads::default();
    let mut version = 0u64;
    loop {
        let now = Instant::now();
        // The keyboard is read with one of our windows in front, and not
        // while something else of ours has it (the F1 settings menu).
        let focused = crate::poll::focused() && !knobs::keyboard_elsewhere();
        let pad = pads.buttons();
        let held = if focused {
            Held::of_keys(crate::poll::held_keys())
        } else {
            Held::default()
        };
        let held = held.union(Held::of_pad(pad));
        let mut changed = false;
        for input in keys.step(held, now) {
            changed |= app.handle(input);
        }
        changed |= app.refresh_mods();
        let open = app.menu.is_open();
        // Closed: presents stop drawing at once. Opened: once its picture
        // is painted (below), so no present shows the one from last time.
        if !open && is_open() {
            knobs::set_work(knobs::DRAW, false);
        }
        let mask = withheld.update(open, |vk| focused && crate::poll::down(vk));
        WITHHELD_KEYS.store(mask, Ordering::Relaxed);
        WITHHELD_PAD.store(
            if open { crate::input::PAD_MENU } else { 0 },
            Ordering::Relaxed,
        );
        if open {
            // Things change behind the menu too (F3, a screenshot saved):
            // looked at twice a second.
            let stale = app
                .painted_at
                .is_none_or(|t| t.elapsed() >= Duration::from_millis(500));
            if changed
                || stale
                || app
                    .last
                    .as_ref()
                    .is_some_and(|(_, s)| *s != scale_for(HEIGHT.load(Ordering::Relaxed)))
            {
                if let Some(layers) = app.repaint(scale_for(HEIGHT.load(Ordering::Relaxed))) {
                    version += 1;
                    *PAINTED.lock().unwrap_or_else(|e| e.into_inner()) =
                        (version, Some(Arc::new(layers)));
                } else {
                    app.painted_at = Some(Instant::now());
                }
            }
            if !is_open() {
                knobs::set_work(knobs::DRAW, true);
            }
        }
        std::thread::sleep(Duration::from_millis(if open { 8 } else { 16 }));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_preview_is_painted_and_repaints_only_on_change() {
        let mut app = App::new(Box::new(crate::font::Dots));
        app.handle(Input::Toggle);
        assert!(app.repaint(1.0).is_some());
        assert!(app.repaint(1.0).is_none());
        app.handle(Input::Down);
        let l = app.repaint(1.0).unwrap();
        assert_eq!((l.highlight, l.rows), (1, 9));
        assert!(app.repaint(1.5).is_some(), "a new scale paints again");
    }

    #[test]
    fn only_one_asks_for_the_menu_open_at_start() {
        use std::ffi::OsStr;
        assert!(opens_at_start(Some(OsStr::new("1"))));
        assert!(!opens_at_start(Some(OsStr::new("0"))));
        assert!(!opens_at_start(Some(OsStr::new(""))));
        assert!(!opens_at_start(None));
        // What the thread does then: the first page, open, painted.
        let mut app = App::new(Box::new(crate::font::Dots));
        app.handle(Input::Toggle);
        assert!(app.menu.is_open() && app.menu.depth() == 0);
        assert_eq!(app.repaint(1.0).map(|l| l.page), Some(0));
    }

    #[test]
    fn scale_follows_the_picture() {
        let _one = knobs::TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        knobs::MENU_SCALE.set(1.0);
        assert_eq!(scale_for(2160), 2.0);
        assert_eq!(scale_for(1080), 1.0);
    }
}
