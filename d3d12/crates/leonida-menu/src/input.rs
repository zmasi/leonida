//! Keys and pad buttons to [`Input`]s: presses, with Up, Down, Left and
//! Right repeating while held, and which keys the title must not see.
//! Platform-free: the poller (`crate::poll`) feeds it what is held.

use std::time::{Duration, Instant};

use crate::model::Input;

/// Virtual keys.
pub const VK_BACK: u8 = 0x08;
pub const VK_RETURN: u8 = 0x0D;
pub const VK_LEFT: u8 = 0x25;
pub const VK_UP: u8 = 0x26;
pub const VK_RIGHT: u8 = 0x27;
pub const VK_DOWN: u8 = 0x28;
pub const VK_NUMPAD0: u8 = 0x60;
pub const VK_NUMPAD2: u8 = 0x62;
pub const VK_NUMPAD4: u8 = 0x64;
pub const VK_NUMPAD5: u8 = 0x65;
pub const VK_NUMPAD6: u8 = 0x66;
pub const VK_NUMPAD8: u8 = 0x68;
pub const VK_F4: u8 = 0x73;

/// The keys the menu reads, and what each asks: F4 opens and closes; the
/// arrows or the number pad's 8, 2, 4, 6 move; Enter or 5 selects;
/// Backspace or 0 goes back.
pub const KEYS: [(u8, Input); 13] = [
    (VK_F4, Input::Toggle),
    (VK_UP, Input::Up),
    (VK_NUMPAD8, Input::Up),
    (VK_DOWN, Input::Down),
    (VK_NUMPAD2, Input::Down),
    (VK_LEFT, Input::Left),
    (VK_NUMPAD4, Input::Left),
    (VK_RIGHT, Input::Right),
    (VK_NUMPAD6, Input::Right),
    (VK_RETURN, Input::Select),
    (VK_NUMPAD5, Input::Select),
    (VK_BACK, Input::Back),
    (VK_NUMPAD0, Input::Back),
];

/// XInput pad buttons.
pub const PAD_UP: u16 = 0x0001;
pub const PAD_DOWN: u16 = 0x0002;
pub const PAD_LEFT: u16 = 0x0004;
pub const PAD_RIGHT: u16 = 0x0008;
pub const PAD_RB: u16 = 0x0200;
pub const PAD_A: u16 = 0x1000;
pub const PAD_B: u16 = 0x2000;

/// The pad buttons the menu reads while open (and keeps from the title).
pub const PAD_MENU: u16 = PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_A | PAD_B;

/// Held this long, a key starts repeating, then at this rate, and faster
/// still once held a while.
const DELAY: Duration = Duration::from_millis(320);
const RATE: Duration = Duration::from_millis(75);
const FAST_AFTER: Duration = Duration::from_millis(1600);
const FAST_RATE: Duration = Duration::from_millis(35);
/// A second open or close this soon after one is a bounce.
const TOGGLE_GAP: Duration = Duration::from_millis(200);

/// What is held: keyboard keys and pad buttons, as [`Input`]s.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Held(u8);

impl Held {
    fn bit(input: Input) -> u8 {
        1 << match input {
            Input::Toggle => 0,
            Input::Up => 1,
            Input::Down => 2,
            Input::Left => 3,
            Input::Right => 4,
            Input::Select => 5,
            Input::Back => 6,
        }
    }

    pub fn with(mut self, input: Input) -> Held {
        self.0 |= Held::bit(input);
        self
    }

    pub fn has(self, input: Input) -> bool {
        self.0 & Held::bit(input) != 0
    }

    /// The inputs of the keys in `down` (virtual keys held).
    pub fn of_keys(down: impl IntoIterator<Item = u8>) -> Held {
        down.into_iter().fold(Held::default(), |held, vk| {
            match KEYS.iter().find(|(k, _)| *k == vk) {
                Some(&(_, input)) => held.with(input),
                None => held,
            }
        })
    }

    /// The inputs of pad `buttons`: RB with Left opens and closes; the
    /// d-pad moves, A selects and B goes back.
    pub fn of_pad(buttons: u16) -> Held {
        let mut held = Held::default();
        if buttons & PAD_RB != 0 && buttons & PAD_LEFT != 0 {
            return held.with(Input::Toggle);
        }
        for (bit, input) in [
            (PAD_UP, Input::Up),
            (PAD_DOWN, Input::Down),
            (PAD_LEFT, Input::Left),
            (PAD_RIGHT, Input::Right),
            (PAD_A, Input::Select),
            (PAD_B, Input::Back),
        ] {
            if buttons & bit != 0 {
                held = held.with(input);
            }
        }
        held
    }

    pub fn union(self, other: Held) -> Held {
        Held(self.0 | other.0)
    }
}

const ALL: [Input; 7] = [
    Input::Toggle,
    Input::Up,
    Input::Down,
    Input::Left,
    Input::Right,
    Input::Select,
    Input::Back,
];

fn repeats(input: Input) -> bool {
    matches!(input, Input::Up | Input::Down | Input::Left | Input::Right)
}

/// Turns what is held, sampled over time, into presses.
#[derive(Debug, Default)]
pub struct Keys {
    held: Held,
    /// When each input went down and when it last fired.
    since: [Option<(Instant, Instant)>; 7],
    last_toggle: Option<Instant>,
}

impl Keys {
    /// What `held` (sampled at `now`) asks for since the last sample.
    pub fn step(&mut self, held: Held, now: Instant) -> Vec<Input> {
        let mut out = Vec::new();
        for (i, &input) in ALL.iter().enumerate() {
            let (was, is) = (self.held.has(input), held.has(input));
            match (was, is) {
                (false, true) => {
                    self.since[i] = Some((now, now));
                    if input == Input::Toggle {
                        if self.last_toggle.is_some_and(|t| now - t < TOGGLE_GAP) {
                            continue;
                        }
                        self.last_toggle = Some(now);
                    }
                    out.push(input);
                }
                (true, true) if repeats(input) => {
                    let Some((down, fired)) = self.since[i] else {
                        continue;
                    };
                    let held_for = now - down;
                    let rate = if held_for >= FAST_AFTER {
                        FAST_RATE
                    } else {
                        RATE
                    };
                    let due = if fired == down {
                        down + DELAY
                    } else {
                        fired + rate
                    };
                    if now >= due {
                        self.since[i] = Some((down, now));
                        out.push(input);
                    }
                }
                (_, false) => self.since[i] = None,
                _ => {}
            }
        }
        self.held = held;
        out
    }
}

/// Which keys the title must not see: all of the menu's while it is open,
/// and after it closes those still held from then until they are let go
/// (the Enter or Backspace that closed it does not reach the title).
#[derive(Debug, Default)]
pub struct Withheld {
    mask: u16,
}

impl Withheld {
    /// The mask (bit `i`: `KEYS[i]`) for keys `down` with the menu `open`.
    pub fn update(&mut self, open: bool, down: impl Fn(u8) -> bool) -> u16 {
        let held = KEYS
            .iter()
            .enumerate()
            .filter(|(_, (vk, _))| down(*vk))
            .fold(0u16, |m, (i, _)| m | 1 << i);
        self.mask = if open {
            (1u16 << KEYS.len()) - 1
        } else {
            self.mask & held
        };
        self.mask
    }
}

/// Whether `vk` is withheld under `mask`.
pub fn withheld(mask: u16, vk: u8) -> bool {
    KEYS.iter()
        .enumerate()
        .any(|(i, (k, _))| *k == vk && mask & (1 << i) != 0)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ms(t: Instant, n: u64) -> Instant {
        t + Duration::from_millis(n)
    }

    #[test]
    fn a_press_fires_once_and_a_hold_repeats() {
        let (mut k, t) = (Keys::default(), Instant::now());
        let down = Held::default().with(Input::Down);
        assert_eq!(k.step(down, t), [Input::Down]);
        assert!(k.step(down, ms(t, 100)).is_empty());
        assert!(k.step(down, ms(t, 300)).is_empty());
        assert_eq!(k.step(down, ms(t, 330)), [Input::Down]);
        assert!(k.step(down, ms(t, 380)).is_empty());
        assert_eq!(k.step(down, ms(t, 410)), [Input::Down]);
        // Faster after a while.
        assert_eq!(k.step(down, ms(t, 1700)), [Input::Down]);
        assert_eq!(k.step(down, ms(t, 1740)), [Input::Down]);
        assert!(k.step(Held::default(), ms(t, 1750)).is_empty());
        assert_eq!(k.step(down, ms(t, 1760)), [Input::Down]);
    }

    #[test]
    fn select_back_and_toggle_do_not_repeat_and_toggle_debounces() {
        let (mut k, t) = (Keys::default(), Instant::now());
        let sel = Held::default().with(Input::Select);
        assert_eq!(k.step(sel, t), [Input::Select]);
        assert!(k.step(sel, ms(t, 2000)).is_empty());
        let tog = Held::default().with(Input::Toggle);
        let none = Held::default();
        assert_eq!(k.step(tog, ms(t, 3000)), [Input::Toggle]);
        k.step(none, ms(t, 3020));
        // A bounce.
        assert!(k.step(tog, ms(t, 3040)).is_empty());
        k.step(none, ms(t, 3100));
        assert_eq!(k.step(tog, ms(t, 3300)), [Input::Toggle]);
    }

    #[test]
    fn keys_and_pad_map_to_inputs() {
        let h = Held::of_keys([VK_NUMPAD8, VK_RETURN, b'W']);
        assert!(h.has(Input::Up) && h.has(Input::Select) && !h.has(Input::Down));
        assert!(Held::of_keys([VK_F4]).has(Input::Toggle));
        assert!(Held::of_keys([VK_NUMPAD0]).has(Input::Back));
        let p = Held::of_pad(PAD_RB | PAD_LEFT);
        assert!(p.has(Input::Toggle) && !p.has(Input::Left));
        let p = Held::of_pad(PAD_LEFT | PAD_A);
        assert!(p.has(Input::Left) && p.has(Input::Select));
    }

    #[test]
    fn keys_stay_withheld_until_let_go_after_closing() {
        let mut w = Withheld::default();
        let enter = |vk: u8| vk == VK_RETURN;
        let m = w.update(true, enter);
        assert!(withheld(m, VK_UP) && withheld(m, VK_RETURN) && withheld(m, VK_F4));
        assert!(!withheld(m, b'W'));
        // Closed with Enter still down: only Enter stays withheld.
        let m = w.update(false, enter);
        assert!(withheld(m, VK_RETURN) && !withheld(m, VK_UP));
        let m = w.update(false, |_| false);
        assert_eq!(m, 0);
        // Pressed again with the menu closed: the title sees it.
        assert_eq!(w.update(false, enter), 0);
    }
}
