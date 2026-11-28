//! What the host's keyboard and pads say, for `leonida.key_down` and
//! `leonida.pad`. Off Windows there is nothing to read yet (Linux is planned):
//! no key is down and no pad is connected.

/// A pad's state: buttons by name, triggers 0 to 1, sticks -1 to 1.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Pad {
    pub connected: bool,
    /// XInput's button bits.
    pub buttons: u16,
    pub lt: f32,
    pub rt: f32,
    pub lx: f32,
    pub ly: f32,
    pub rx: f32,
    pub ry: f32,
}

/// XInput's button bits and the names a mod sees them by.
pub const BUTTONS: [(&str, u16); 14] = [
    ("up", 0x0001),
    ("down", 0x0002),
    ("left", 0x0004),
    ("right", 0x0008),
    ("start", 0x0010),
    ("back", 0x0020),
    ("ls", 0x0040),
    ("rs", 0x0080),
    ("lb", 0x0100),
    ("rb", 0x0200),
    ("a", 0x1000),
    ("b", 0x2000),
    ("x", 0x4000),
    ("y", 0x8000),
];

#[cfg(windows)]
mod host {
    use super::Pad;
    use std::time::{Duration, Instant};
    use windows_sys::Win32::System::Threading::GetCurrentProcessId;
    use windows_sys::Win32::UI::Input::KeyboardAndMouse::GetAsyncKeyState;
    use windows_sys::Win32::UI::Input::XboxController::{XINPUT_STATE, XInputGetState};
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        GetForegroundWindow, GetWindowThreadProcessId,
    };

    /// Whether the window in front is one of this process's: the keys are
    /// the mods' only then, as the menu's are.
    fn focused() -> bool {
        let mut process = 0;
        // SAFETY: plain queries; a null window gives process 0.
        unsafe {
            GetWindowThreadProcessId(GetForegroundWindow(), &mut process);
            process == GetCurrentProcessId()
        }
    }

    /// Key state with the focus asked once for a batch.
    pub struct Keys {
        focus: bool,
    }

    impl Keys {
        pub fn new() -> Keys {
            Keys { focus: focused() }
        }
        pub fn down(&self, vk: u8) -> bool {
            // SAFETY: a plain query.
            self.focus && unsafe { GetAsyncKeyState(i32::from(vk)) < 0 }
        }
    }

    /// Asking XInput about an empty slot is slow: looked at again this often.
    const EMPTY_RECHECK: Duration = Duration::from_secs(1);

    pub struct Pads {
        empty_until: [Option<Instant>; 4],
    }

    impl Pads {
        pub fn new() -> Pads {
            Pads {
                empty_until: [None; 4],
            }
        }

        /// The first connected pad.
        pub fn first(&mut self) -> Pad {
            fn stick(v: i16) -> f32 {
                (f32::from(v) / 32767.0).clamp(-1.0, 1.0)
            }
            for slot in 0..4 {
                if self.empty_until[slot].is_some_and(|t| Instant::now() < t) {
                    continue;
                }
                let mut state = XINPUT_STATE::default();
                // SAFETY: room for the state.
                if unsafe { XInputGetState(slot as u32, &mut state) } != 0 {
                    self.empty_until[slot] = Some(Instant::now() + EMPTY_RECHECK);
                    continue;
                }
                let g = state.Gamepad;
                return Pad {
                    connected: true,
                    buttons: g.wButtons,
                    lt: f32::from(g.bLeftTrigger) / 255.0,
                    rt: f32::from(g.bRightTrigger) / 255.0,
                    lx: stick(g.sThumbLX),
                    ly: stick(g.sThumbLY),
                    rx: stick(g.sThumbRX),
                    ry: stick(g.sThumbRY),
                };
            }
            Pad::default()
        }
    }
}

#[cfg(not(windows))]
mod host {
    use super::Pad;

    pub struct Keys;
    impl Keys {
        pub fn new() -> Keys {
            Keys
        }
        pub fn down(&self, _vk: u8) -> bool {
            false
        }
    }

    pub struct Pads;
    impl Pads {
        pub fn new() -> Pads {
            Pads
        }
        pub fn first(&mut self) -> Pad {
            Pad::default()
        }
    }
}

pub use host::{Keys, Pads};
