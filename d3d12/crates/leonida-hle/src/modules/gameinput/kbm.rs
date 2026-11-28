//! Leonida: the keyboard and mouse as the title's pad, in Grand Theft Auto V's PC layout.
//!
//! The title reads an Xbox pad only, and a pad button means different things on foot and in a vehicle: the right
//! trigger shoots on foot and accelerates in a car, the left trigger aims and brakes. One table of keys cannot be
//! a PC layout then (W would have to be the left stick and the right trigger at once), so there are two, one on
//! foot and one in a vehicle (`SHARED` with `FOOT_ONLY` or with `VEHICLE_ONLY`), and which one counts follows what
//! the player is in ([`PlayerContext`]): told from outside with [`set_player_context`] (the runtime reads it from
//! the title), and while nobody tells, the on-foot table with Caps Lock as the switch to the driving one by hand
//! (only then: Caps Lock is GTA V's special ability, and a press out of habit must not change the keys).
//!
//! The keys are Grand Theft Auto V's defaults (its Settings > Key Bindings, as listed in BisectHosting's and
//! StrategyWiki's control guides); the pad button of each action is the title's own default pad layout as far as
//! it is known (`docs/gameinput.md`). A GTA V key whose action has no known pad button here is left out.
//!
//! The hand-over rule, when the table changes: a key whose pad input differs between the tables and that is down
//! at that moment gives nothing until it has been let go and pressed again, and no such key gives anything for
//! a short pause (`Config::settle`) after the change. So the left mouse button, held on foot to shoot, never
//! becomes the accelerator, and W, held to accelerate, never becomes a shot. A key that is the same pad input in
//! both tables (A and D, Space, F) carries over, as the button of a real pad would.
//!
//! `LEONIDA_KBM=classic` brings back the single table of before ([`super::fallback`]);
//! `LEONIDA_KBM_DRIVE=on|off|auto` forces the driving table on or off (a test), `auto` being the default;
//! `LEONIDA_KBM_SETTLE_MS` (400; 0..3000) is the pause after a change.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

use super::{
    A, B, DOWN, KeyState, LEFT, LEFT_SHOULDER, LEFT_STICK, MENU, PadState, RIGHT, RIGHT_SHOULDER, RIGHT_STICK, UP,
    VIEW, VK_MOUSE_LEFT, VK_MOUSE_RIGHT, X, Y,
};

const VK_BACK: u8 = 0x08;
const VK_TAB: u8 = 0x09;
const VK_RETURN: u8 = 0x0D;
const VK_CAPITAL: u8 = 0x14;
const VK_ESCAPE: u8 = 0x1B;
const VK_SPACE: u8 = 0x20;
const VK_LEFT: u8 = 0x25;
const VK_UP: u8 = 0x26;
const VK_RIGHT: u8 = 0x27;
const VK_DOWN: u8 = 0x28;
const VK_NUMPAD4: u8 = 0x64;
const VK_NUMPAD5: u8 = 0x65;
const VK_NUMPAD6: u8 = 0x66;
const VK_NUMPAD8: u8 = 0x68;
const VK_LSHIFT: u8 = 0xA0;
const VK_LCONTROL: u8 = 0xA2;
const VK_LMENU: u8 = 0xA4;
const VK_RMENU: u8 = 0xA5;

/// The key that enters and leaves a vehicle (Y on the pad, in both tables).
const VK_ENTER_EXIT: u8 = b'F';
/// Keys that give nothing while an Alt key is down: with it they are shortcuts of Windows.
const WITH_ALT: [u8; 4] = [VK_TAB, VK_RETURN, VK_SPACE, VK_ESCAPE];

/// What the player is in, as far as the runtime can tell.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PlayerContext {
    /// Nobody has told, or not for [`STALE`]: the on-foot table, and Caps Lock switches by hand.
    Unknown = 0,
    OnFoot = 1,
    InVehicle = 2,
}

/// A context that was not set again for this long is unknown: 500 ms, in the clock of [`super::now`].
const STALE: u64 = 500_000;
/// And the stand-in pad only lets go of a known context once it has seen it unknown for this long (100 ms): after
/// a stall of the whole process the poll may run before whoever sets the context does.
const UNKNOWN_AFTER: u64 = 100_000;

/// The context and when it was set, in one word: no lock between the thread that sets it and the poll.
pub(super) struct Shared(AtomicU64);

impl Shared {
    pub(super) const fn new() -> Shared {
        Shared(AtomicU64::new(0))
    }

    /// `now`: microseconds ([`super::now`]).
    pub(super) fn set(&self, context: PlayerContext, now: u64) {
        self.0.store((now << 2) | context as u64, Ordering::Relaxed);
    }

    pub(super) fn get(&self, now: u64) -> PlayerContext {
        let word = self.0.load(Ordering::Relaxed);
        if now.saturating_sub(word >> 2) > STALE {
            return PlayerContext::Unknown;
        }
        match word & 3 {
            1 => PlayerContext::OnFoot,
            2 => PlayerContext::InVehicle,
            _ => PlayerContext::Unknown,
        }
    }
}

static CONTEXT: Shared = Shared::new();
/// Whether the driving table is the one in use now (what the stand-in pad last decided).
static DRIVING: AtomicBool = AtomicBool::new(false);

/// Tells the stand-in pad what the player is in. It holds for half a second, so whoever knows says it again and
/// again (once a frame); a context nobody renews becomes [`PlayerContext::Unknown`]. Lock-free; any thread.
pub fn set_player_context(context: PlayerContext) {
    CONTEXT.set(context, super::now());
}

/// The context last set, [`PlayerContext::Unknown`] once it is stale.
pub fn player_context() -> PlayerContext {
    CONTEXT.get(super::now())
}

/// Whether the keyboard and mouse are on the driving table now (W accelerates, the left mouse button shoots from
/// the vehicle): the measured context, or Caps Lock, or `LEONIDA_KBM_DRIVE`. For whoever shows key prompts.
pub fn driving_controls() -> bool {
    DRIVING.load(Ordering::Relaxed)
}

/// Whether the stand-in pad follows the player's context at all: not with `LEONIDA_KBM=classic` nor with
/// `LEONIDA_KBM_DRIVE=on|off`. Whoever would measure the context need not when this is false.
pub fn player_context_wanted() -> bool {
    let config = Config::from_env();
    config.layout == Layout::GtaV && config.drive == Drive::Auto
}

/// What a key does to the pad.
#[derive(Clone, Copy, Debug, PartialEq)]
enum To {
    Button(u32),
    LeftTrigger,
    RightTrigger,
    /// A push of the left stick: -1 or 1 (x right, y up).
    LeftX(f32),
    LeftY(f32),
    RightX(f32),
    RightY(f32),
}

type Bind = (u8, To);

/// The keys that mean the same in both tables. GTA V's key, the pad input, and what it does in the title.
const SHARED: &[Bind] = &[
    // Move, steer (the left stick's x in a vehicle).
    (b'A', To::LeftX(-1.0)),
    (b'D', To::LeftX(1.0)),
    // Jump and climb on foot, the handbrake in a vehicle: the title's A.
    (VK_SPACE, To::Button(A)),
    // Enter a vehicle, leave it: the title's Y.
    (VK_ENTER_EXIT, To::Button(Y)),
    // Reload (GTA V's R; melee too): the title's X. In a vehicle GTA V has the cinematic camera on R, which has no
    // known button here, so R stays the same pad button there.
    (b'R', To::Button(X)),
    // The weapon wheel: the title's loadout wheel (hold), holster (tap).
    (VK_TAB, To::Button(LEFT_SHOULDER)),
    // Look behind; zoom while aiming.
    (b'C', To::Button(RIGHT_STICK)),
    // Pause: Esc and P.
    (VK_ESCAPE, To::Button(MENU)),
    (b'P', To::Button(MENU)),
    // The map (the title's View button).
    (b'M', To::Button(VIEW)),
    // The phone (up) and its menus; down is the title's HUD and objective, left its radio menu, right its partner
    // menu.
    (VK_UP, To::Button(UP)),
    (VK_DOWN, To::Button(DOWN)),
    (VK_LEFT, To::Button(LEFT)),
    (VK_RIGHT, To::Button(RIGHT)),
    // Phone and menus: accept, back.
    (VK_RETURN, To::Button(A)),
    (VK_BACK, To::Button(B)),
    // GTA V's radar zoom and info: the same d-pad press as on its pad.
    (b'Z', To::Button(DOWN)),
    // (GTA V's character switch, Left Alt, would be the title's partner menu, d-pad right. It is left out: every
    // Alt+Tab would press it. The right arrow is that button.)
    // Not GTA V's: the pad-named keys of the layout before, where GTA V has nothing on them.
    (b'Y', To::Button(Y)),
    (b'I', To::RightY(1.0)),
    (b'K', To::RightY(-1.0)),
    (b'J', To::RightX(-1.0)),
    (b'L', To::RightX(1.0)),
    // (The mouse itself is the right stick: `MouseLook`.)
];

/// On foot.
const FOOT_ONLY: &[Bind] = &[
    (b'W', To::LeftY(1.0)),
    (b'S', To::LeftY(-1.0)),
    // Shoot, aim.
    (VK_MOUSE_LEFT, To::RightTrigger),
    (VK_MOUSE_RIGHT, To::LeftTrigger),
    // Sprint: the title runs on a click of the left stick.
    (VK_LSHIFT, To::Button(LEFT_STICK)),
    // GTA V's stealth: the title's crouch.
    (VK_LCONTROL, To::Button(B)),
    // Cover.
    (b'Q', To::Button(RIGHT_SHOULDER)),
    // GTA V's context action: the title interacts with Y.
    (b'E', To::Button(Y)),
];

/// In a vehicle.
const VEHICLE_ONLY: &[Bind] = &[
    // Accelerate; brake and reverse.
    (b'W', To::RightTrigger),
    (b'S', To::LeftTrigger),
    // Lean forward and back on a bike, the nose of an aircraft: the left stick's y. Num 4 and 6 roll.
    (VK_LSHIFT, To::LeftY(1.0)),
    (VK_NUMPAD8, To::LeftY(1.0)),
    (VK_LCONTROL, To::LeftY(-1.0)),
    (VK_NUMPAD5, To::LeftY(-1.0)),
    (VK_NUMPAD4, To::LeftX(-1.0)),
    (VK_NUMPAD6, To::LeftX(1.0)),
    // The horn.
    (b'E', To::Button(LEFT_STICK)),
    // Shoot from the vehicle. (Aiming from it, GTA V's right mouse button, has no known button here.)
    (VK_MOUSE_LEFT, To::Button(RIGHT_SHOULDER)),
    // Duck; the headlights.
    (b'X', To::Button(B)),
    (b'H', To::Button(B)),
    // GTA V's radio wheel: the title's radio menu.
    (b'Q', To::Button(LEFT)),
];

/// The two tables: what is shared, then what is the context's own.
fn table(driving: bool) -> impl Iterator<Item = &'static Bind> {
    SHARED.iter().chain(if driving { VEHICLE_ONLY } else { FOOT_ONLY })
}

/// What `key` does in a table, in the table's order.
fn outputs(driving: bool, key: u8) -> impl Iterator<Item = To> {
    table(driving).filter(move |(k, _)| *k == key).map(|(_, to)| *to)
}

/// Whether `key` is another pad input in a vehicle than on foot: the keys the hand-over rule is about.
fn changes(key: u8) -> bool {
    !outputs(false, key).eq(outputs(true, key))
}

/// The pad of a table, with the keys `on` says are down.
fn pad_of(driving: bool, on: impl Fn(u8) -> bool) -> PadState {
    let mut pad = PadState::default();
    for &(key, to) in table(driving) {
        if !on(key) {
            continue;
        }
        match to {
            To::Button(bit) => pad.buttons |= bit,
            To::LeftTrigger => pad.left_trigger = 1.0,
            To::RightTrigger => pad.right_trigger = 1.0,
            To::LeftX(push) => pad.left_x += push,
            To::LeftY(push) => pad.left_y += push,
            To::RightX(push) => pad.right_x += push,
            To::RightY(push) => pad.right_y += push,
        }
    }
    for axis in [&mut pad.left_x, &mut pad.left_y, &mut pad.right_x, &mut pad.right_y] {
        *axis = axis.clamp(-1.0, 1.0);
    }
    pad
}

/// The button a scripted key (`LEONIDA_KEYS`) presses on a real pad: the on-foot table's, or with
/// `LEONIDA_KBM=classic` the old table's.
#[cfg_attr(not(windows), allow(dead_code))]
pub(super) fn scripted_button(key: u8) -> Option<u32> {
    if Config::from_env().layout == Layout::Classic {
        return super::FALLBACK_BUTTONS.iter().find(|(k, _)| *k == key).map(|(_, bit)| *bit);
    }
    outputs(false, key).find_map(|to| match to {
        To::Button(bit) => Some(bit),
        _ => None,
    })
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Layout {
    GtaV,
    Classic,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Drive {
    Auto,
    On,
    Off,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct Config {
    pub(super) layout: Layout,
    pub(super) drive: Drive,
    /// Microseconds after a change of table in which no key that changes with it counts.
    pub(super) settle: u64,
}

impl Default for Config {
    fn default() -> Config {
        Config { layout: Layout::GtaV, drive: Drive::Auto, settle: 400_000 }
    }
}

impl Config {
    /// From the values of `LEONIDA_KBM`, `LEONIDA_KBM_DRIVE` and `LEONIDA_KBM_SETTLE_MS`; what does not read is
    /// the default.
    pub(super) fn parse(kbm: Option<&str>, drive: Option<&str>, settle_ms: Option<&str>) -> Config {
        let default = Config::default();
        let word = |v: Option<&str>| v.map(|v| v.trim().to_ascii_lowercase());
        Config {
            layout: match word(kbm).as_deref() {
                Some("classic") => Layout::Classic,
                _ => Layout::GtaV,
            },
            drive: match word(drive).as_deref() {
                Some("on" | "1") => Drive::On,
                Some("off" | "0") => Drive::Off,
                _ => Drive::Auto,
            },
            settle: settle_ms
                .and_then(|v| v.trim().parse::<u64>().ok())
                .map_or(default.settle, |ms| ms.min(3000) * 1000),
        }
    }

    pub(super) fn from_env() -> Config {
        static CONFIG: std::sync::LazyLock<Config> = std::sync::LazyLock::new(|| {
            let var = |name: &str| std::env::var(name).ok();
            Config::parse(
                var("LEONIDA_KBM").as_deref(),
                var("LEONIDA_KBM_DRIVE").as_deref(),
                var("LEONIDA_KBM_SETTLE_MS").as_deref(),
            )
        });
        *CONFIG
    }
}

/// A set of virtual keys.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct Keys([u64; 4]);

impl Keys {
    fn of(keys: &[KeyState]) -> Keys {
        let mut set = Keys::default();
        for key in keys {
            set.0[usize::from(key.virtual_key >> 6)] |= 1 << (key.virtual_key & 63);
        }
        set
    }

    fn has(&self, key: u8) -> bool {
        self.0[usize::from(key >> 6)] & (1 << (key & 63)) != 0
    }

    fn both(&self, other: &Keys) -> Keys {
        Keys(std::array::from_fn(|i| self.0[i] & other.0[i]))
    }

    fn either(&self, other: &Keys) -> Keys {
        Keys(std::array::from_fn(|i| self.0[i] | other.0[i]))
    }

    /// Those of the set `keep` says yes to.
    fn only(&self, keep: impl Fn(u8) -> bool) -> Keys {
        let mut set = Keys::default();
        for key in 0..=u8::MAX {
            if self.has(key) && keep(key) {
                set.0[usize::from(key >> 6)] |= 1 << (key & 63);
            }
        }
        set
    }
}

/// The stand-in pad's keyboard and mouse half: which table counts, and the hand-over between them.
#[derive(Debug)]
pub(super) struct Kbm {
    config: Config,
    /// The table in use.
    driving: bool,
    /// Caps Lock's say while no context is measured: the driving table, until Caps Lock again or F (which leaves
    /// the vehicle).
    by_hand: bool,
    /// The measured context as it counts (see [`UNKNOWN_AFTER`]).
    measured: PlayerContext,
    unknown_since: Option<u64>,
    /// The keys down at the last poll.
    down: Keys,
    /// Keys that were down when the table changed: nothing from them until they are let go.
    latched: Keys,
    /// Until then (microseconds) no key that changes with the table counts.
    settle_until: u64,
}

impl Kbm {
    pub(super) fn new(config: Config) -> Kbm {
        Kbm {
            config,
            driving: false,
            by_hand: false,
            measured: PlayerContext::Unknown,
            unknown_since: None,
            down: Keys::default(),
            latched: Keys::default(),
            settle_until: 0,
        }
    }

    /// The pad the keys held make, with the context measured now (`now`: microseconds).
    pub(super) fn pad(&mut self, keys: &[KeyState], measured: PlayerContext, now: u64) -> PadState {
        if self.config.layout == Layout::Classic {
            return super::fallback(keys);
        }
        let (mut down, was) = (Keys::of(keys), self.down);
        // With Alt down these are Windows' own (Alt+Tab, Alt+Enter, Alt+Space, Alt+Esc), not the game's.
        if down.has(VK_LMENU) || down.has(VK_RMENU) {
            down = down.only(|key| !WITH_ALT.contains(&key));
        }
        let pressed = |key: u8| down.has(key) && !was.has(key);
        let (wanted, why) = match self.config.drive {
            Drive::On => (true, "LEONIDA_KBM_DRIVE=on"),
            Drive::Off => (false, "LEONIDA_KBM_DRIVE=off"),
            Drive::Auto => {
                let measured = self.settled(measured, now);
                if measured != self.measured {
                    // What is measured counts; driving by hand is for when nothing is, and starts anew then.
                    self.measured = measured;
                    self.by_hand = false;
                }
                match self.measured {
                    PlayerContext::InVehicle => (true, "the player is in a vehicle"),
                    PlayerContext::OnFoot => (false, "the player is on foot"),
                    PlayerContext::Unknown if pressed(VK_CAPITAL) => {
                        self.by_hand = !self.by_hand;
                        (self.by_hand, "Caps Lock")
                    }
                    PlayerContext::Unknown if pressed(VK_ENTER_EXIT) && self.by_hand => {
                        // Driving by hand, and the player leaves the vehicle: W must not stay the right trigger.
                        self.by_hand = false;
                        (false, "F leaves the vehicle; Caps Lock is the driving keys")
                    }
                    PlayerContext::Unknown => (self.by_hand, "what the player is in is not known"),
                }
            }
        };
        if wanted != self.driving {
            self.driving = wanted;
            DRIVING.store(wanted, Ordering::Relaxed);
            self.latched = self.latched.either(&down.only(changes));
            self.settle_until = now + self.config.settle;
            tracing::info!(
                "keyboard and mouse: {} controls ({why})",
                if wanted { "driving" } else { "on-foot" }
            );
        }
        self.latched = self.latched.both(&down);
        self.down = down;
        let settling = now < self.settle_until;
        let latched = self.latched;
        pad_of(self.driving, |key| down.has(key) && !latched.has(key) && !(settling && changes(key)))
    }

    /// The measured context, an unknown one only once it has been unknown for [`UNKNOWN_AFTER`].
    fn settled(&mut self, measured: PlayerContext, now: u64) -> PlayerContext {
        if measured != PlayerContext::Unknown || self.measured == PlayerContext::Unknown {
            self.unknown_since = None;
            return measured;
        }
        let since = *self.unknown_since.get_or_insert(now);
        if now.saturating_sub(since) < UNKNOWN_AFTER { self.measured } else { measured }
    }
}

/// The context for the poll at `now`.
pub(super) fn measured(now: u64) -> PlayerContext {
    CONTEXT.get(now)
}

#[cfg(test)]
mod tests {
    use super::*;

    const MS: u64 = 1000;
    const FOOT: PlayerContext = PlayerContext::OnFoot;
    const CAR: PlayerContext = PlayerContext::InVehicle;
    const UNKNOWN: PlayerContext = PlayerContext::Unknown;

    fn keys(held: &[u8]) -> Vec<KeyState> {
        held.iter().map(|&vk| KeyState { virtual_key: vk, ..Default::default() }).collect()
    }

    fn foot(held: &[u8]) -> PadState {
        pad_of(false, |key| held.contains(&key))
    }

    fn car(held: &[u8]) -> PadState {
        pad_of(true, |key| held.contains(&key))
    }

    fn button(bit: u32) -> PadState {
        PadState { buttons: bit, ..Default::default() }
    }

    fn kbm() -> Kbm {
        Kbm::new(Config::default())
    }

    /// Whether the pad pulls a trigger or presses the button that shoots from a vehicle.
    fn fires_or_drives(pad: &PadState) -> bool {
        pad.left_trigger != 0.0 || pad.right_trigger != 0.0 || pad.buttons & RIGHT_SHOULDER != 0
    }

    #[test]
    fn on_foot_is_gta_vs_layout() {
        // Movement is W A S D, and nothing else of them.
        assert_eq!(foot(b"W"), PadState { left_y: 1.0, ..Default::default() });
        assert_eq!(foot(b"S"), PadState { left_y: -1.0, ..Default::default() });
        assert_eq!(foot(b"A"), PadState { left_x: -1.0, ..Default::default() });
        assert_eq!(foot(b"D"), PadState { left_x: 1.0, ..Default::default() });
        assert_eq!(foot(b"WS"), PadState::default());
        // Shooting and aiming are the mouse, and nothing else of it.
        assert_eq!(foot(&[VK_MOUSE_LEFT]), PadState { right_trigger: 1.0, ..Default::default() });
        assert_eq!(foot(&[VK_MOUSE_RIGHT]), PadState { left_trigger: 1.0, ..Default::default() });
        for (key, bit) in [
            (VK_SPACE, A),
            (VK_LSHIFT, LEFT_STICK),
            (VK_LCONTROL, B),
            (b'Q', RIGHT_SHOULDER),
            (b'R', X),
            (b'F', Y),
            (b'E', Y),
            (VK_TAB, LEFT_SHOULDER),
            (b'C', RIGHT_STICK),
            (b'M', VIEW),
            (VK_ESCAPE, MENU),
            (b'P', MENU),
            (VK_UP, UP),
            (VK_DOWN, DOWN),
            (VK_LEFT, LEFT),
            (VK_RIGHT, RIGHT),
            (VK_RETURN, A),
            (VK_BACK, B),
            (b'Z', DOWN),
            (b'Y', Y),
        ] {
            assert_eq!(foot(&[key]), button(bit), "key {key:#x}");
        }
        assert_eq!(foot(b"IL"), PadState { right_x: 1.0, right_y: 1.0, ..Default::default() });
        // GTA V keys whose action has no known pad button: nothing. So are the old layout's trigger keys.
        for key in *b"VGXHOTBN1234567890" {
            assert_eq!(foot(&[key]), PadState::default(), "key {}", key as char);
        }
        assert_eq!(foot(&[VK_CAPITAL, VK_NUMPAD8, VK_NUMPAD5, VK_LMENU, VK_RMENU]), PadState::default());
    }

    #[test]
    fn alt_tab_presses_nothing() {
        let mut k = kbm();
        // Alt, then Tab (or Enter, Space, Esc) with it: Windows' shortcuts, no button of the pad.
        assert_eq!(k.pad(&keys(&[VK_LMENU]), FOOT, 0), PadState::default());
        for key in [VK_TAB, VK_RETURN, VK_SPACE, VK_ESCAPE] {
            assert_eq!(k.pad(&keys(&[VK_LMENU, key]), FOOT, 8 * MS), PadState::default(), "key {key:#x}");
            assert_eq!(k.pad(&keys(&[VK_RMENU, key]), FOOT, 16 * MS), PadState::default(), "key {key:#x}");
        }
        // Other keys are not held back, and without Alt Tab is the weapon wheel.
        assert_eq!(k.pad(&keys(&[VK_LMENU, b'W']), FOOT, 24 * MS), PadState { left_y: 1.0, ..Default::default() });
        assert_eq!(k.pad(&keys(&[VK_TAB]), FOOT, 32 * MS), button(LEFT_SHOULDER));
    }

    #[test]
    fn in_a_vehicle_w_and_s_are_the_triggers_and_the_mouse_shoots() {
        // W accelerates, S brakes: the triggers, fully, and not the stick.
        assert_eq!(car(b"W"), PadState { right_trigger: 1.0, ..Default::default() });
        assert_eq!(car(b"S"), PadState { left_trigger: 1.0, ..Default::default() });
        assert_eq!(car(b"WS"), PadState { left_trigger: 1.0, right_trigger: 1.0, ..Default::default() });
        // A and D steer.
        assert_eq!(car(b"A"), PadState { left_x: -1.0, ..Default::default() });
        assert_eq!(car(b"D"), PadState { left_x: 1.0, ..Default::default() });
        // Bikes and aircraft: Shift and Ctrl (and the number pad) are the stick's other axis.
        assert_eq!(car(&[VK_LSHIFT]), PadState { left_y: 1.0, ..Default::default() });
        assert_eq!(car(&[VK_LCONTROL]), PadState { left_y: -1.0, ..Default::default() });
        assert_eq!(car(&[VK_LSHIFT, VK_NUMPAD8]), PadState { left_y: 1.0, ..Default::default() });
        assert_eq!(car(&[VK_NUMPAD5, VK_NUMPAD6]), PadState { left_x: 1.0, left_y: -1.0, ..Default::default() });
        assert_eq!(car(&[VK_NUMPAD4]), PadState { left_x: -1.0, ..Default::default() });
        // The left mouse button shoots from the vehicle, which is a button and no trigger. The right one has no
        // known button.
        assert_eq!(car(&[VK_MOUSE_LEFT]), button(RIGHT_SHOULDER));
        assert_eq!(car(&[VK_MOUSE_RIGHT]), PadState::default());
        for (key, bit) in [
            (VK_SPACE, A),
            (b'F', Y),
            (b'E', LEFT_STICK),
            (b'X', B),
            (b'H', B),
            (b'Q', LEFT),
            (b'R', X),
            (b'C', RIGHT_STICK),
            (VK_TAB, LEFT_SHOULDER),
            (b'M', VIEW),
            (VK_ESCAPE, MENU),
            (VK_UP, UP),
            (VK_RETURN, A),
            (VK_BACK, B),
        ] {
            assert_eq!(car(&[key]), button(bit), "key {key:#x}");
        }
        for key in *b"VGOTBN1234567890" {
            assert_eq!(car(&[key]), PadState::default(), "key {}", key as char);
        }
    }

    /// The two rules the layout stands on, read off the tables: no table makes a mouse button the accelerator or
    /// the brake, and no table makes W or S a shot.
    #[test]
    fn the_mouse_never_drives_and_w_never_shoots() {
        for mouse in [VK_MOUSE_LEFT, VK_MOUSE_RIGHT] {
            assert!(outputs(true, mouse).all(|to| !matches!(to, To::LeftTrigger | To::RightTrigger)));
            assert!(outputs(false, mouse).all(|to| matches!(to, To::LeftTrigger | To::RightTrigger)));
        }
        for key in *b"WASD" {
            assert!(outputs(false, key).all(|to| matches!(to, To::LeftX(_) | To::LeftY(_))), "{}", key as char);
            assert!(outputs(true, key).all(|to| to != To::Button(RIGHT_SHOULDER)), "{}", key as char);
        }
        // What changes with the table, and what does not.
        for key in [b'W', b'S', VK_MOUSE_LEFT, VK_MOUSE_RIGHT, VK_LSHIFT, VK_LCONTROL, b'Q', b'E', b'X', b'H'] {
            assert!(changes(key), "key {key:#x}");
        }
        for key in [b'A', b'D', VK_SPACE, b'F', b'R', b'C', VK_TAB, VK_ESCAPE, VK_UP, b'M', b'V'] {
            assert!(!changes(key), "key {key:#x}");
        }
        // No key is bound twice to the same thing, and the tables are not empty.
        for driving in [false, true] {
            let all: Vec<&Bind> = table(driving).collect();
            assert!(all.len() > 25);
            for (i, a) in all.iter().enumerate() {
                assert!(all[i + 1..].iter().all(|b| a != b), "{a:?} twice");
            }
        }
    }

    #[test]
    fn entering_a_vehicle_with_keys_held_neither_shoots_nor_accelerates() {
        let mut k = kbm();
        // On foot: walking forward and shooting.
        let held = keys(&[b'W', VK_MOUSE_LEFT, b'A']);
        let pad = k.pad(&held, FOOT, 0);
        assert_eq!(pad, PadState { left_x: -1.0, left_y: 1.0, right_trigger: 1.0, ..Default::default() });
        // The player is in a vehicle now, the keys still down: neither W nor the mouse button gives anything,
        // however long they stay down. A (the same stick push in both) carries over.
        for ms in [8, 100, 399, 400, 1000, 60_000] {
            let pad = k.pad(&held, CAR, ms * MS);
            assert_eq!(pad, PadState { left_x: -1.0, ..Default::default() }, "{ms} ms");
        }
        // The mouse button let go and pressed again: it shoots from the vehicle, and is still no accelerator.
        k.pad(&keys(&[b'W', b'A']), CAR, 60_008 * MS);
        let pad = k.pad(&held, CAR, 60_016 * MS);
        assert_eq!(pad, PadState { buttons: RIGHT_SHOULDER, left_x: -1.0, ..Default::default() });
        // W let go and pressed again: it accelerates.
        k.pad(&keys(&[]), CAR, 60_024 * MS);
        let pad = k.pad(&keys(b"W"), CAR, 60_032 * MS);
        assert_eq!(pad, PadState { right_trigger: 1.0, ..Default::default() });
    }

    #[test]
    fn leaving_a_vehicle_with_keys_held_neither_shoots_nor_walks() {
        let mut k = kbm();
        k.pad(&keys(&[]), CAR, 0);
        // Accelerating, steering, the handbrake on.
        let held = keys(&[b'W', b'D', VK_SPACE]);
        let pad = k.pad(&held, CAR, 1000 * MS);
        assert_eq!(pad, PadState { buttons: A, left_x: 1.0, right_trigger: 1.0, ..Default::default() });
        // On foot now with W still down: no trigger (it would be a shot), and no walking until W is pressed
        // again. D and Space are the same pad input in both.
        for ms in [1008, 1200, 1400, 5000] {
            let pad = k.pad(&held, FOOT, ms * MS);
            assert_eq!(pad, PadState { buttons: A, left_x: 1.0, ..Default::default() }, "{ms} ms");
        }
        k.pad(&keys(&[]), FOOT, 5008 * MS);
        assert_eq!(k.pad(&keys(b"W"), FOOT, 5016 * MS), PadState { left_y: 1.0, ..Default::default() });
    }

    #[test]
    fn a_key_pressed_right_after_the_change_waits_for_the_pause() {
        let mut k = kbm();
        k.pad(&keys(&[]), FOOT, 0);
        k.pad(&keys(&[]), CAR, 1000 * MS);
        // Pressed 50 ms after the change: nothing until 400 ms after it, then what the new table says.
        for ms in [1050, 1200, 1399] {
            assert_eq!(k.pad(&keys(&[b'W', VK_MOUSE_LEFT]), CAR, ms * MS), PadState::default(), "{ms} ms");
        }
        let pad = k.pad(&keys(&[b'W', VK_MOUSE_LEFT]), CAR, 1400 * MS);
        assert_eq!(pad, PadState { buttons: RIGHT_SHOULDER, right_trigger: 1.0, ..Default::default() });
        // A key that is the same in both tables does not wait.
        let mut k = kbm();
        k.pad(&keys(&[]), CAR, 0);
        k.pad(&keys(&[]), FOOT, 1000 * MS);
        assert_eq!(k.pad(&keys(b"F"), FOOT, 1008 * MS), button(Y));
        assert_eq!(k.pad(&keys(&[b'F', VK_MOUSE_LEFT]), FOOT, 1100 * MS), button(Y));
        // With no pause set, a fresh press counts at once; a held key still does not.
        let mut k = Kbm::new(Config { settle: 0, ..Config::default() });
        k.pad(&keys(b"S"), FOOT, 0);
        assert_eq!(k.pad(&keys(b"S"), CAR, 8 * MS), PadState::default());
        assert_eq!(k.pad(&keys(b"SW"), CAR, 16 * MS), PadState { right_trigger: 1.0, ..Default::default() });
    }

    /// Whatever is held when the table changes, in either direction: until a key is pressed anew, nothing pulls
    /// a trigger and nothing shoots from the vehicle.
    #[test]
    fn no_set_of_held_keys_fires_or_drives_across_a_change() {
        let changing = [b'W', b'S', VK_MOUSE_LEFT, VK_MOUSE_RIGHT, VK_LSHIFT, VK_LCONTROL, b'Q', b'E', b'X', b'H'];
        for (from, to) in [(FOOT, CAR), (CAR, FOOT), (UNKNOWN, CAR), (CAR, UNKNOWN)] {
            for mask in 0u32..1 << changing.len() {
                let held: Vec<u8> =
                    changing.iter().enumerate().filter(|(i, _)| mask & (1 << i) != 0).map(|(_, k)| *k).collect();
                let held = keys(&held);
                let mut k = kbm();
                k.pad(&keys(&[]), from, 0);
                k.pad(&held, from, 1000 * MS);
                for ms in [1008, 1100, 1399, 1400, 1500, 9000] {
                    let pad = k.pad(&held, to, ms * MS);
                    // (An unknown context counts 100 ms late; until then the pad is the old table's.)
                    if to == UNKNOWN && ms < 1108 {
                        continue;
                    }
                    assert_eq!(pad, PadState::default(), "{from:?} to {to:?}, keys {mask:#b}, {ms} ms");
                    assert!(!fires_or_drives(&pad));
                }
            }
        }
    }

    #[test]
    fn caps_lock_switches_by_hand_while_the_context_is_unknown() {
        let mut k = kbm();
        // Unknown: on foot.
        assert_eq!(k.pad(&keys(b"W"), UNKNOWN, 0), PadState { left_y: 1.0, ..Default::default() });
        k.pad(&keys(&[]), UNKNOWN, 8 * MS);
        // Caps Lock: driving, once per press however long it is held.
        for ms in [16, 24, 500, 2000] {
            k.pad(&keys(&[VK_CAPITAL]), UNKNOWN, ms * MS);
            assert!(k.driving, "{ms} ms");
        }
        k.pad(&keys(&[]), UNKNOWN, 2008 * MS);
        assert_eq!(k.pad(&keys(b"W"), UNKNOWN, 2016 * MS), PadState { right_trigger: 1.0, ..Default::default() });
        // Caps Lock again, W still down: on foot, and W gives nothing until pressed again.
        assert_eq!(k.pad(&keys(&[b'W', VK_CAPITAL]), UNKNOWN, 3000 * MS), PadState::default());
        assert!(!k.driving);
        k.pad(&keys(&[]), UNKNOWN, 3008 * MS);
        // Driving by hand ends when F leaves the vehicle: W must not stay the trigger on foot.
        k.pad(&keys(&[VK_CAPITAL]), UNKNOWN, 4000 * MS);
        k.pad(&keys(&[]), UNKNOWN, 4008 * MS);
        assert!(k.driving);
        assert_eq!(k.pad(&keys(b"WF"), UNKNOWN, 5000 * MS), button(Y));
        assert!(!k.driving);
        // On foot F is just F.
        k.pad(&keys(&[]), UNKNOWN, 5008 * MS);
        k.pad(&keys(b"F"), UNKNOWN, 6000 * MS);
        assert!(!k.driving);
    }

    #[test]
    fn a_measured_context_wins_and_caps_lock_is_no_switch_then() {
        let mut k = kbm();
        // Driving by hand, then the runtime starts to tell: its word counts.
        k.pad(&keys(&[VK_CAPITAL]), UNKNOWN, 0);
        k.pad(&keys(&[]), UNKNOWN, 8 * MS);
        assert!(k.driving);
        k.pad(&keys(&[]), FOOT, 16 * MS);
        assert!(!k.driving);
        // Caps Lock (GTA V's special ability, pressed out of habit) does nothing on foot: W stays the stick.
        k.pad(&keys(&[VK_CAPITAL]), FOOT, 500 * MS);
        assert!(!k.driving);
        assert_eq!(k.pad(&keys(&[VK_CAPITAL, b'W']), FOOT, 600 * MS), PadState { left_y: 1.0, ..Default::default() });
        k.pad(&keys(&[]), FOOT, 700 * MS);
        // Nor in a vehicle, and F is just the Y button there.
        k.pad(&keys(&[]), CAR, 1000 * MS);
        assert!(k.driving);
        k.pad(&keys(&[VK_CAPITAL]), CAR, 2000 * MS);
        k.pad(&keys(b"F"), CAR, 2100 * MS);
        assert!(k.driving);
        k.pad(&keys(&[]), CAR, 2200 * MS);
        // The context is lost (nobody tells any more): on foot, not the driving by hand of before.
        k.pad(&keys(&[]), UNKNOWN, 3000 * MS);
        k.pad(&keys(&[]), UNKNOWN, 3100 * MS);
        assert!(!k.driving);
        // And Caps Lock is the switch again.
        k.pad(&keys(&[VK_CAPITAL]), UNKNOWN, 4000 * MS);
        assert!(k.driving);
    }

    #[test]
    fn a_context_nobody_renews_is_unknown() {
        let shared = Shared::new();
        assert_eq!(shared.get(0), UNKNOWN);
        assert_eq!(shared.get(10_000 * MS), UNKNOWN);
        shared.set(CAR, 1000 * MS);
        assert_eq!(shared.get(1000 * MS), CAR);
        assert_eq!(shared.get(1500 * MS), CAR);
        assert_eq!(shared.get(1501 * MS), UNKNOWN);
        shared.set(FOOT, 1600 * MS);
        assert_eq!(shared.get(1700 * MS), FOOT);
        shared.set(UNKNOWN, 1800 * MS);
        assert_eq!(shared.get(1800 * MS), UNKNOWN);
        // A reader whose clock is behind the writer's does not go wrong.
        shared.set(CAR, 5000 * MS);
        assert_eq!(shared.get(4000 * MS), CAR);
        // The stand-in pad lets go of a known context only after 100 ms of unknown, and then as on any change.
        let mut k = kbm();
        k.pad(&keys(&[]), CAR, 0);
        assert_eq!(k.pad(&keys(b"W"), CAR, 1000 * MS), PadState { right_trigger: 1.0, ..Default::default() });
        assert_eq!(k.pad(&keys(b"W"), UNKNOWN, 1010 * MS), PadState { right_trigger: 1.0, ..Default::default() });
        assert_eq!(k.pad(&keys(b"W"), UNKNOWN, 1100 * MS), PadState { right_trigger: 1.0, ..Default::default() });
        // Known again in time: nothing happened.
        assert_eq!(k.pad(&keys(b"W"), CAR, 1105 * MS), PadState { right_trigger: 1.0, ..Default::default() });
        assert_eq!(k.pad(&keys(b"W"), UNKNOWN, 2000 * MS), PadState { right_trigger: 1.0, ..Default::default() });
        assert_eq!(k.pad(&keys(b"W"), UNKNOWN, 2100 * MS), PadState::default());
        assert!(!k.driving);
    }

    #[test]
    fn the_switches_read() {
        assert_eq!(Config::parse(None, None, None), Config::default());
        assert_eq!(Config::default().layout, Layout::GtaV);
        assert_eq!(Config::default().drive, Drive::Auto);
        assert_eq!(Config::parse(Some("classic"), None, None).layout, Layout::Classic);
        assert_eq!(Config::parse(Some(" Classic "), None, None).layout, Layout::Classic);
        assert_eq!(Config::parse(Some("gta5"), None, None).layout, Layout::GtaV);
        assert_eq!(Config::parse(None, Some("on"), None).drive, Drive::On);
        assert_eq!(Config::parse(None, Some("OFF"), None).drive, Drive::Off);
        assert_eq!(Config::parse(None, Some("auto"), None).drive, Drive::Auto);
        assert_eq!(Config::parse(None, Some("what"), None).drive, Drive::Auto);
        assert_eq!(Config::parse(None, None, Some("0")).settle, 0);
        assert_eq!(Config::parse(None, None, Some("250")).settle, 250 * MS);
        assert_eq!(Config::parse(None, None, Some("99999")).settle, 3000 * MS);
        assert_eq!(Config::parse(None, None, Some("soon")).settle, 400 * MS);
    }

    #[test]
    fn the_forced_tables_and_the_classic_one() {
        // Forced on: driving whatever is measured, and Caps Lock is no switch.
        let mut k = Kbm::new(Config { drive: Drive::On, ..Config::default() });
        k.pad(&keys(&[]), FOOT, 0);
        assert_eq!(k.pad(&keys(b"W"), FOOT, 1000 * MS), PadState { right_trigger: 1.0, ..Default::default() });
        k.pad(&keys(&[VK_CAPITAL]), UNKNOWN, 2000 * MS);
        assert!(k.driving);
        // Forced off.
        let mut k = Kbm::new(Config { drive: Drive::Off, ..Config::default() });
        assert_eq!(k.pad(&keys(b"W"), CAR, 0), PadState { left_y: 1.0, ..Default::default() });
        k.pad(&keys(&[b'W', VK_CAPITAL]), CAR, 1000 * MS);
        assert!(!k.driving);
        // Classic: the table of before, whatever the context.
        let mut k = Kbm::new(Config { layout: Layout::Classic, ..Config::default() });
        for context in [UNKNOWN, FOOT, CAR] {
            let held = keys(&[b'W', b'V', b'Q', VK_MOUSE_RIGHT, VK_CAPITAL]);
            assert_eq!(k.pad(&held, context, 0), super::super::fallback(&held));
            assert_eq!(
                k.pad(&held, context, 0),
                PadState {
                    buttons: LEFT_SHOULDER,
                    left_y: 1.0,
                    left_trigger: 1.0,
                    right_trigger: 1.0,
                    ..Default::default()
                }
            );
        }
    }

    #[test]
    fn scripted_keys_press_the_on_foot_buttons() {
        assert_eq!(scripted_button(VK_RETURN), Some(A));
        assert_eq!(scripted_button(VK_ESCAPE), Some(MENU));
        assert_eq!(scripted_button(VK_BACK), Some(B));
        assert_eq!(scripted_button(VK_UP), Some(UP));
        assert_eq!(scripted_button(b'F'), Some(Y));
        // A stick or a trigger is no button; an unbound key is none.
        assert_eq!(scripted_button(b'W'), None);
        assert_eq!(scripted_button(VK_MOUSE_LEFT), None);
        assert_eq!(scripted_button(b'V'), None);
    }
}
