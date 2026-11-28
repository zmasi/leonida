//! What the mods' runtime asks of the host. [`Real`] is the running title's;
//! the tests have a fake.

use std::cell::{Cell, RefCell};

use leonida_menu::knobs;
use leonida_menu::model::Value;
use leonida_menu::mods::{self, Event, ModPage};
use leonida_menu::overlay::{self, Cmd};

use crate::input::{Keys, Pad, Pads};
use crate::memory;

pub trait Platform {
    /// Called once at the start of each pass: the focus is looked at once for all keys.
    fn refresh_input(&self) {}
    fn key_down(&self, vk: u8) -> bool;
    fn pad(&self) -> Pad;
    fn menu_open(&self) -> bool;
    /// What the overlay shows from the next present on (empty: nothing).
    fn publish_overlay(&self, cmds: &[Cmd]);
    /// Holds exactly these keys down for the title.
    fn inject_keys(&self, keys: &[u8]);
    fn set_menu_pages(&self, pages: Vec<ModPage>);
    /// A mod changes the value an item of its page shows.
    fn set_menu_value(&self, page: usize, item: usize, value: Value);
    fn menu_events(&self) -> Vec<Event>;
    /// The menu's "Reload Lua mods" was chosen.
    fn take_reload(&self) -> bool;
    /// Asks for a screenshot of the next frame; the path it goes to.
    fn screenshot(&self) -> Result<String, String>;
    fn log(&self, mod_name: &str, line: &str);
    /// The title's memory, checked: false where a page is not mapped (or not writable).
    fn read(&self, address: usize, out: &mut [u8]) -> bool;
    fn write(&self, address: usize, data: &[u8]) -> bool;
    /// Where the title's executable image is mapped: base and size.
    fn image(&self) -> Option<(usize, usize)>;
}

/// The running title's host.
pub struct Real {
    keys: RefCell<Keys>,
    pads: RefCell<Pads>,
    shots: Cell<u32>,
}

impl Real {
    pub fn new() -> Real {
        Real {
            keys: RefCell::new(Keys::new()),
            pads: RefCell::new(Pads::new()),
            shots: Cell::new(0),
        }
    }
}

impl Default for Real {
    fn default() -> Self {
        Real::new()
    }
}

impl Platform for Real {
    fn refresh_input(&self) {
        *self.keys.borrow_mut() = Keys::new();
    }

    fn key_down(&self, vk: u8) -> bool {
        self.keys.borrow().down(vk)
    }

    fn pad(&self) -> Pad {
        self.pads.borrow_mut().first()
    }

    fn menu_open(&self) -> bool {
        leonida_menu::is_open()
    }

    fn publish_overlay(&self, cmds: &[Cmd]) {
        overlay::publish(cmds);
    }

    fn inject_keys(&self, keys: &[u8]) {
        knobs::set_injected_keys(keys);
    }

    fn set_menu_pages(&self, pages: Vec<ModPage>) {
        mods::set_pages(pages);
    }

    fn set_menu_value(&self, page: usize, item: usize, value: Value) {
        mods::set_value(page, item, value);
    }

    fn menu_events(&self) -> Vec<Event> {
        mods::take_events()
    }

    fn take_reload(&self) -> bool {
        mods::take_reload()
    }

    fn screenshot(&self) -> Result<String, String> {
        if !knobs::screenshots_there() {
            return Err(
                "screenshots are not in this build (diagnostics builds read frames back)".into(),
            );
        }
        let n = self.shots.get() + 1;
        self.shots.set(n);
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |d| d.as_millis());
        let path = knobs::screenshot_folder().join(format!("leonida-lua-{stamp}-{n}.png"));
        knobs::ask_screenshot(path.clone());
        Ok(path.display().to_string())
    }

    fn log(&self, mod_name: &str, line: &str) {
        tracing::info!(target: "lua", mod_name, "{line}");
    }

    fn read(&self, address: usize, out: &mut [u8]) -> bool {
        memory::host().is_some_and(|h| (h.read)(address, out))
    }

    fn write(&self, address: usize, data: &[u8]) -> bool {
        memory::host().is_some_and(|h| (h.write)(address, data))
    }

    fn image(&self) -> Option<(usize, usize)> {
        memory::image()
    }
}
