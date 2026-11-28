//! Leonida: which kind of input the player is using, so the runtime can show the matching button prompts.
//!
//! The title is told every pad is an Xbox pad, so it cannot know what is in the player's hands; this keeps that
//! record for the runtime's own use (the glyph movie served by `files`, a HUD): [`super::active_family`]. The rules are
//! those of the Vulkan runtime's `docs/gameinput.md` ("Active input"):
//!
//! - only deliberate input changes the answer: a button going down, a trigger rising past 0.3, a stick leaving a
//!   radius of 0.35, a key going down. Held buttons, drift inside the radius and noise never count;
//! - a device's first reading is its baseline, so connecting a pad with a button held changes nothing;
//! - after a change the next waits `LEONIDA_PROMPTS_SWITCH_MS` (300 ms): input from another family inside that time
//!   is ignored, not queued, so two devices used at once cannot make the answer flicker;
//! - `LEONIDA_PROMPTS` forces a family (`xbox`, `playstation` or `ps5`/`ps4`/`ps3`, `keyboard`); `auto` follows
//!   the devices. Tracking goes on underneath.
//!
//! Before the first deliberate input the answer is Xbox, or PlayStation when a Sony pad is present.
use std::sync::atomic::{AtomicU8, AtomicU32, Ordering};
use std::time::Duration;

use super::{KeyState, PadState};

/// What the player holds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Family {
    Xbox = 0,
    PlayStation = 1,
    KeyboardMouse = 2,
}

impl Family {
    pub fn name(self) -> &'static str {
        match self {
            Family::Xbox => "Xbox",
            Family::PlayStation => "PlayStation",
            Family::KeyboardMouse => "keyboard and mouse",
        }
    }

    fn from_u8(v: u8) -> Family {
        match v {
            1 => Family::PlayStation,
            2 => Family::KeyboardMouse,
            _ => Family::Xbox,
        }
    }

    /// `LEONIDA_PROMPTS` text: `Some(None)` for `auto` (or nothing), `None` for text that names no family.
    pub fn parse(text: &str) -> Option<Option<Family>> {
        match text.trim().to_ascii_lowercase().as_str() {
            "" | "auto" => Some(None),
            "xbox" => Some(Some(Family::Xbox)),
            "playstation" | "ps" | "ps5" | "ps4" | "ps3" | "dualsense" | "dualshock" => Some(Some(Family::PlayStation)),
            "keyboard" | "kbm" | "pc" => Some(Some(Family::KeyboardMouse)),
            _ => None,
        }
    }
}

static ACTIVE: AtomicU8 = AtomicU8::new(Family::Xbox as u8);
static GENERATION: AtomicU32 = AtomicU32::new(0);
static FORCED: AtomicU8 = AtomicU8::new(0);

/// The family of the device used last (or the one `LEONIDA_PROMPTS` forces). Lock-free, readable from any thread
/// every frame.
pub fn current() -> Family {
    Family::from_u8(ACTIVE.load(Ordering::Relaxed))
}

/// Goes up by one with each change of [`super::active_family`] (the first answer counts as generation 0).
pub fn generation() -> u32 {
    GENERATION.load(Ordering::Relaxed)
}

/// Whether `LEONIDA_PROMPTS` forces the answer.
pub fn forced() -> bool {
    FORCED.load(Ordering::Relaxed) != 0
}

/// Sticks and triggers: what counts as pushed.
const TRIGGER_ON: f32 = 0.3;
const STICK_ON: f32 = 0.35;

/// Spots deliberate input in a stream of pad readings.
#[derive(Clone, Debug, Default)]
pub(super) struct Detector {
    previous: Option<PadState>,
}

impl Detector {
    /// The next reading; whether it holds an edge. The first one after `reset` is a baseline and holds none.
    pub(super) fn deliberate(&mut self, pad: &PadState) -> bool {
        let Some(was) = self.previous.replace(*pad) else {
            return false;
        };
        let rose = |before: f32, now: f32, on: f32| before < on && now >= on;
        let stick = |bx: f32, by: f32, x: f32, y: f32| rose(bx.hypot(by), x.hypot(y), STICK_ON);
        pad.buttons & !was.buttons != 0
            || rose(was.left_trigger, pad.left_trigger, TRIGGER_ON)
            || rose(was.right_trigger, pad.right_trigger, TRIGGER_ON)
            || stick(was.left_x, was.left_y, pad.left_x, pad.left_y)
            || stick(was.right_x, was.right_y, pad.right_x, pad.right_y)
    }

    /// The device went away (or is not read this poll): its next reading is a baseline again.
    pub(super) fn reset(&mut self) {
        self.previous = None;
    }
}

/// Keyboard and mouse: a key (or mouse button) going down, and the mouse look stick leaving its radius.
#[derive(Clone, Debug, Default)]
pub(super) struct KeyDetector {
    down: Option<Vec<u8>>,
    look: Detector,
}

impl KeyDetector {
    pub(super) fn deliberate(&mut self, keys: &[KeyState], look: (f32, f32)) -> bool {
        let now: Vec<u8> = keys.iter().map(|k| k.virtual_key).collect();
        let key = match self.down.replace(now.clone()) {
            Some(was) => now.iter().any(|vk| !was.contains(vk)),
            None => false,
        };
        let look = self.look.deliberate(&PadState { right_x: look.0, right_y: look.1, ..Default::default() });
        key || look
    }
}

/// When an XInput pad and a Sony pad are one controller: Steam Input or DS4Windows turn the Sony pad into an XInput
/// one and leave the real device readable. Input on both within `WINDOW` of each other makes them one (for `HOLD`
/// afterwards), so the Sony pad's own reading is left out of the pad the title sees, and the XInput pad's input
/// counts as PlayStation.
#[derive(Clone, Debug, Default)]
pub(super) struct Mirror {
    xinput: Option<Duration>,
    sony: Option<Duration>,
    until: Option<Duration>,
}

impl Mirror {
    const WINDOW: Duration = Duration::from_millis(150);
    const HOLD: Duration = Duration::from_secs(10);

    pub(super) fn xinput_input(&mut self, now: Duration) {
        self.xinput = Some(now);
        self.check(now, self.sony);
    }

    pub(super) fn sony_input(&mut self, now: Duration) {
        self.sony = Some(now);
        self.check(now, self.xinput);
    }

    fn check(&mut self, now: Duration, other: Option<Duration>) {
        if other.is_some_and(|t| now.saturating_sub(t) <= Self::WINDOW) {
            self.until = Some(now + Self::HOLD);
        }
    }

    pub(super) fn active(&self, now: Duration) -> bool {
        self.until.is_some_and(|until| now < until)
    }
}

/// The rules above, on a clock the caller supplies.
#[derive(Clone, Debug)]
pub(super) struct Tracker {
    current: Family,
    forced: Option<Family>,
    changed: Option<Duration>,
    guard: Duration,
}

impl Tracker {
    pub(super) fn new(forced: Option<Family>, guard: Duration) -> Tracker {
        Tracker { current: forced.unwrap_or(Family::Xbox), forced, changed: None, guard }
    }

    /// `LEONIDA_PROMPTS`, `LEONIDA_PROMPTS_SWITCH_MS`.
    pub(super) fn from_env() -> Tracker {
        let forced = match std::env::var("LEONIDA_PROMPTS") {
            Ok(text) => Family::parse(&text).unwrap_or_else(|| {
                tracing::warn!(value = %text, "LEONIDA_PROMPTS: unknown family (xbox, playstation, keyboard or auto)");
                None
            }),
            Err(_) => None,
        };
        let ms = std::env::var("LEONIDA_PROMPTS_SWITCH_MS")
            .ok()
            .and_then(|v| v.trim().parse::<u64>().ok())
            .map_or(300, |ms| ms.min(2000));
        Tracker::new(forced, Duration::from_millis(ms))
    }

    /// The family in force.
    pub(super) fn active(&self) -> Family {
        self.forced.unwrap_or(self.current)
    }

    /// Before the first change: PlayStation when a Sony pad is present. Not a change that starts the guard.
    pub(super) fn start_with(&mut self, family: Family) {
        if self.changed.is_none() {
            self.current = family;
        }
    }

    /// Deliberate input of `family` at `now`; whether the tracked family changed.
    pub(super) fn note(&mut self, family: Family, now: Duration) -> bool {
        if family == self.current {
            return false;
        }
        if self.changed.is_some_and(|at| now.saturating_sub(at) < self.guard) {
            return false;
        }
        self.current = family;
        self.changed = Some(now);
        true
    }
}

/// The tracker the runtime shares, with the answer published for [`super::active_family`].
pub(super) struct Shared {
    tracker: Tracker,
    published: Option<Family>,
}

impl Shared {
    pub(super) fn new(tracker: Tracker) -> Shared {
        Shared { tracker, published: None }
    }

    pub(super) fn tracker(&mut self) -> &mut Tracker {
        &mut self.tracker
    }

    /// Deliberate input of `family`: publishes a change.
    pub(super) fn input(&mut self, family: Family, now: Duration, why: &str) {
        if self.tracker.note(family, now) {
            self.publish(why);
        }
    }

    /// Makes the tracker's answer the global one and logs a change.
    pub(super) fn publish(&mut self, why: &str) {
        let family = self.tracker.active();
        if self.published == Some(family) {
            return;
        }
        if self.published.is_some() {
            GENERATION.fetch_add(1, Ordering::Relaxed);
        }
        FORCED.store(u8::from(self.tracker.forced.is_some()), Ordering::Relaxed);
        ACTIVE.store(family as u8, Ordering::Relaxed);
        self.published = Some(family);
        tracing::info!(
            family = family.name(),
            forced = self.tracker.forced.is_some(),
            "input in use: {} ({why})",
            family.name()
        );
    }
}

/// The runtime's one tracker (the input hub's host source is the only writer).
pub(super) static SHARED: std::sync::LazyLock<std::sync::Mutex<Shared>> =
    std::sync::LazyLock::new(|| std::sync::Mutex::new(Shared::new(Tracker::from_env())));

#[cfg(test)]
mod tests {
    use super::*;
    use crate::modules::gameinput::{A, UP};

    fn ms(n: u64) -> Duration {
        Duration::from_millis(n)
    }

    fn pad() -> PadState {
        PadState::default()
    }

    #[test]
    fn first_reading_is_a_baseline() {
        let mut d = Detector::default();
        // Connected with a button held, a trigger down and a stick pushed: nothing yet.
        let held = PadState { buttons: A, left_trigger: 1.0, right_x: 1.0, ..pad() };
        assert!(!d.deliberate(&held));
        assert!(!d.deliberate(&held));
        // Letting go is not input either.
        assert!(!d.deliberate(&pad()));
        // A reset makes the next reading a baseline again.
        d.reset();
        assert!(!d.deliberate(&held));
    }

    #[test]
    fn edges_buttons_triggers_and_sticks() {
        let mut d = Detector::default();
        d.deliberate(&pad());
        assert!(d.deliberate(&PadState { buttons: UP, ..pad() }));
        assert!(!d.deliberate(&PadState { buttons: UP, ..pad() }), "held");
        assert!(d.deliberate(&PadState { buttons: UP | A, ..pad() }), "a second button");
        d.deliberate(&pad());
        assert!(!d.deliberate(&PadState { right_trigger: 0.29, ..pad() }));
        assert!(d.deliberate(&PadState { right_trigger: 0.31, ..pad() }));
        assert!(!d.deliberate(&PadState { right_trigger: 1.0, ..pad() }), "already past the threshold");
        d.deliberate(&pad());
        // Drift and noise inside the radius.
        assert!(!d.deliberate(&PadState { left_x: 0.2, left_y: 0.2, ..pad() }));
        assert!(!d.deliberate(&PadState { left_x: -0.25, left_y: 0.24, ..pad() }));
        assert!(d.deliberate(&PadState { left_x: 0.3, left_y: 0.3, ..pad() }), "0.42 from the centre");
        // A stick resting past the radius from the start does not count again.
        assert!(!d.deliberate(&PadState { left_x: 0.5, left_y: 0.3, ..pad() }));
    }

    #[test]
    fn keys_and_mouse_look() {
        let key = |vk: u8| KeyState { virtual_key: vk, ..Default::default() };
        let mut d = KeyDetector::default();
        assert!(!d.deliberate(&[key(b'W')], (0.0, 0.0)), "held when first seen");
        assert!(!d.deliberate(&[key(b'W')], (0.0, 0.0)));
        assert!(d.deliberate(&[key(b'W'), key(b'A')], (0.0, 0.0)));
        assert!(!d.deliberate(&[], (0.0, 0.0)), "letting go");
        assert!(!d.deliberate(&[], (0.1, 0.1)), "mouse jitter");
        assert!(d.deliberate(&[], (0.6, 0.0)), "a real mouse movement");
        assert!(!d.deliberate(&[], (0.7, 0.0)));
    }

    #[test]
    fn family_changes_wait_out_the_guard() {
        let mut t = Tracker::new(None, ms(300));
        assert_eq!(t.active(), Family::Xbox);
        assert!(t.note(Family::PlayStation, ms(1000)), "the first change is not delayed");
        assert_eq!(t.active(), Family::PlayStation);
        assert!(!t.note(Family::KeyboardMouse, ms(1200)), "inside the guard: ignored, not queued");
        assert_eq!(t.active(), Family::PlayStation);
        assert!(!t.note(Family::PlayStation, ms(1300)), "a second device of the family in use changes nothing");
        assert!(t.note(Family::Xbox, ms(1300)), "300 ms after the change");
        assert_eq!(t.active(), Family::Xbox);
    }

    #[test]
    fn the_start_is_not_a_change() {
        let mut t = Tracker::new(None, ms(300));
        assert!(!t.note(Family::Xbox, ms(10)));
        t.start_with(Family::PlayStation);
        assert_eq!(t.active(), Family::PlayStation);
        // The start did not begin the guard: the first deliberate input is not held back.
        assert!(t.note(Family::Xbox, ms(20)));
        // A start after a change is ignored.
        t.start_with(Family::PlayStation);
        assert_eq!(t.active(), Family::Xbox);
    }

    #[test]
    fn forcing_wins_and_tracking_goes_on() {
        let mut t = Tracker::new(Some(Family::PlayStation), ms(300));
        assert_eq!(t.active(), Family::PlayStation);
        t.note(Family::KeyboardMouse, ms(500));
        assert_eq!(t.active(), Family::PlayStation);
        t.forced = None;
        assert_eq!(t.active(), Family::KeyboardMouse, "clearing the force returns to the device in use");
    }

    #[test]
    fn prompt_names() {
        assert_eq!(Family::parse("PlayStation"), Some(Some(Family::PlayStation)));
        assert_eq!(Family::parse(" ps5 "), Some(Some(Family::PlayStation)));
        assert_eq!(Family::parse("xbox"), Some(Some(Family::Xbox)));
        assert_eq!(Family::parse("keyboard"), Some(Some(Family::KeyboardMouse)));
        assert_eq!(Family::parse("auto"), Some(None));
        assert_eq!(Family::parse(""), Some(None));
        assert_eq!(Family::parse("switch"), None);
    }

    #[test]
    fn a_remapper_makes_both_pads_one() {
        let mut m = Mirror::default();
        assert!(!m.active(ms(0)));
        m.xinput_input(ms(1000));
        assert!(!m.active(ms(1001)), "input on one side only");
        m.sony_input(ms(1080));
        assert!(m.active(ms(1100)), "both within 150 ms");
        assert!(m.active(ms(10_000)));
        assert!(!m.active(ms(11_100)), "held for 10 s");
        // Two pads used at different times are two pads.
        let mut m = Mirror::default();
        m.sony_input(ms(1000));
        m.xinput_input(ms(1500));
        assert!(!m.active(ms(1600)));
    }

    #[test]
    fn publishing_sets_the_global_answer() {
        // The globals are shared by every test in the process: this is the one that writes them.
        let mut s = Shared::new(Tracker::new(None, ms(0)));
        s.publish("start");
        assert_eq!(current(), Family::Xbox);
        let before = generation();
        s.tracker().note(Family::PlayStation, ms(5));
        s.publish("test");
        assert_eq!(current(), Family::PlayStation);
        assert_eq!(generation(), before + 1);
        assert!(!forced());
        s.publish("again");
        assert_eq!(generation(), before + 1, "no change, no new generation");
    }
}
