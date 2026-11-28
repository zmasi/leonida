//! What is held on the host: the menu's keys (with one of this process's
//! windows in front) and the XInput pads.

use std::time::{Duration, Instant};

use windows_sys::Win32::System::Threading::GetCurrentProcessId;
use windows_sys::Win32::UI::Input::KeyboardAndMouse::GetAsyncKeyState;
use windows_sys::Win32::UI::Input::XboxController::{XINPUT_STATE, XInputGetState};
use windows_sys::Win32::UI::WindowsAndMessaging::{GetForegroundWindow, GetWindowThreadProcessId};

use crate::input::KEYS;

/// Whether the window in front is one of this process's.
pub fn focused() -> bool {
    let mut process = 0;
    // SAFETY: plain queries; a null window gives process 0.
    unsafe {
        GetWindowThreadProcessId(GetForegroundWindow(), &mut process);
        process == GetCurrentProcessId()
    }
}

/// Whether virtual key `vk` is down.
pub fn down(vk: u8) -> bool {
    // SAFETY: a plain query.
    unsafe { GetAsyncKeyState(i32::from(vk)) < 0 }
}

/// The menu's keys that are down.
pub fn held_keys() -> impl Iterator<Item = u8> {
    KEYS.iter().map(|(vk, _)| *vk).filter(|&vk| down(vk))
}

/// Asking XInput about an empty slot is slow: looked at again this often.
const EMPTY_RECHECK: Duration = Duration::from_secs(1);

/// The pads' buttons, all slots together.
#[derive(Default)]
pub struct Pads {
    empty_until: [Option<Instant>; 4],
}

impl Pads {
    pub fn buttons(&mut self) -> u16 {
        let mut all = 0;
        for slot in 0..4 {
            if self.empty_until[slot].is_some_and(|t| Instant::now() < t) {
                continue;
            }
            let mut state: XINPUT_STATE = XINPUT_STATE::default();
            // SAFETY: room for the state.
            if unsafe { XInputGetState(slot as u32, &mut state) } != 0 {
                self.empty_until[slot] = Some(Instant::now() + EMPTY_RECHECK);
                continue;
            }
            all |= state.Gamepad.wButtons;
        }
        all
    }
}
