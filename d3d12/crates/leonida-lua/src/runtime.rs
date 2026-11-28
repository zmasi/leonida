//! The mods' runtime: the Lua state, the mods loaded into it, and the
//! passes that call their handlers. It runs on one thread and knows the host
//! only through [`Platform`], so the tests drive it without a title.

use std::cell::RefCell;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::time::{Duration, Instant};

use leonida_menu::model::Value as MenuValue;
use leonida_menu::mods::{ModItem, ModKind, ModPage};
use leonida_menu::overlay::Cmd;
use mlua::{Function, IntoLuaMulti, Lua, MultiValue};

use crate::api;
use crate::keys;
use crate::manifest::{self, Manifest};
use crate::platform::Platform;
use crate::sandbox::{self, Budget, Guard};

/// Handlers one mod may have in all.
pub const MAX_HANDLERS: usize = 64;
/// Items and pages one mod may add are limited by the menu (`leonida_menu::mods`).

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Event {
    Frame,
    Key,
    MenuOpen,
}

impl Event {
    pub fn parse(name: &str) -> Option<Event> {
        match name {
            "frame" => Some(Event::Frame),
            "key" => Some(Event::Key),
            "menu_open" => Some(Event::MenuOpen),
            _ => None,
        }
    }
}

pub struct Handler {
    pub mod_idx: usize,
    pub event: Event,
    pub function: Function,
}

/// A mod, as loaded.
pub struct ModState {
    pub name: String,
    pub folder: PathBuf,
    /// Why it is off, once it is.
    pub error: Option<String>,
}

pub struct ItemDef {
    pub label: String,
    pub help: String,
    pub kind: ModKind,
    pub callback: Option<Function>,
}

pub struct PageDef {
    /// Never reused: what a page object in a mod holds on to.
    pub uid: u64,
    pub mod_idx: usize,
    pub title: String,
    pub items: Vec<ItemDef>,
}

/// What the API's functions and the passes share.
pub struct Shared {
    pub platform: Rc<dyn Platform>,
    pub mods: Vec<ModState>,
    pub handlers: Vec<Handler>,
    /// What is drawn this pass.
    pub draw: Vec<Cmd>,
    pub pages: Vec<PageDef>,
    pub next_page: u64,
    pub pages_dirty: bool,
    /// The keys each mod holds for the title.
    pub held: Vec<Vec<u8>>,
    pub held_dirty: bool,
    pub frame: u64,
    pub started: Instant,
    pub guard: Guard,
}

impl Shared {
    /// Logs a line for a mod.
    pub fn log(&self, idx: usize, line: &str) {
        let name = self.mods.get(idx).map_or("lua", |m| m.name.as_str());
        self.platform.log(name, line);
    }

    /// Switches mod `idx` off: no handlers, no pages, no held keys.
    pub fn disable(&mut self, idx: usize, why: &str) {
        let Some(m) = self.mods.get_mut(idx) else {
            return;
        };
        if m.error.is_some() {
            return;
        }
        m.error = Some(why.to_owned());
        let name = m.name.clone();
        self.platform
            .log(&name, &format!("disabled (the other mods go on): {why}"));
        self.handlers.retain(|h| h.mod_idx != idx);
        let before = self.pages.len();
        self.pages.retain(|p| p.mod_idx != idx);
        self.pages_dirty |= self.pages.len() != before;
        if let Some(held) = self.held.get_mut(idx)
            && !held.is_empty()
        {
            held.clear();
            self.held_dirty = true;
        }
    }

    pub fn active(&self, idx: usize) -> bool {
        self.mods.get(idx).is_some_and(|m| m.error.is_none())
    }
}

/// How the runtime runs.
#[derive(Clone, Copy, Debug, Default)]
pub struct Config {
    pub budget: Budget,
}

pub struct Runtime {
    lua: Lua,
    guard: Guard,
    shared: Rc<RefCell<Shared>>,
    config: Config,
    platform: Rc<dyn Platform>,
    last_frame: u64,
    last_frame_at: Instant,
    keys_down: [bool; 256],
    menu_was_open: bool,
    drew: bool,
}

/// How a mod stands after loading.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Status {
    Running,
    /// `"enabled": false`.
    Off,
    /// Failed: why.
    Failed(String),
}

impl Runtime {
    pub fn new(platform: Rc<dyn Platform>, config: Config) -> mlua::Result<Runtime> {
        let (lua, guard) = sandbox::new_lua()?;
        let shared = Rc::new(RefCell::new(Shared {
            platform: platform.clone(),
            mods: Vec::new(),
            handlers: Vec::new(),
            draw: Vec::new(),
            pages: Vec::new(),
            next_page: 1,
            pages_dirty: false,
            held: Vec::new(),
            held_dirty: false,
            frame: 0,
            started: Instant::now(),
            guard: guard.clone(),
        }));
        Ok(Runtime {
            lua,
            guard,
            shared,
            config,
            platform,
            last_frame: 0,
            last_frame_at: Instant::now(),
            keys_down: [false; 256],
            menu_was_open: false,
            drew: false,
        })
    }

    /// The mods and how each stands.
    pub fn mods(&self) -> Vec<(String, Status)> {
        let s = self.shared.borrow();
        s.mods
            .iter()
            .map(|m| {
                (
                    m.name.clone(),
                    m.error.clone().map_or(Status::Running, Status::Failed),
                )
            })
            .collect()
    }

    /// Loads every enabled Lua mod of `dir`; what was found, by folder.
    pub fn load_dir(&mut self, dir: &Path) -> Vec<(String, Status)> {
        let mut report = Vec::new();
        for folder in manifest::folders(dir) {
            if let Some(entry) = self.load_mod(&folder) {
                report.push(entry);
            }
        }
        self.after_pass();
        report
    }

    /// Loads one mod folder; `None` for a folder that is not a Lua mod.
    pub fn load_mod(&mut self, folder: &Path) -> Option<(String, Status)> {
        let folder_name = folder.file_name()?.to_string_lossy().into_owned();
        let manifest = match manifest::read(folder) {
            Ok(m) => m,
            Err(e) => {
                self.platform.log(&folder_name, &format!("not loaded: {e}"));
                return Some((folder_name, Status::Failed(e)));
            }
        };
        let Manifest {
            name,
            enabled,
            script,
            ..
        } = manifest;
        let script = script?;
        if !enabled {
            return Some((name, Status::Off));
        }
        let idx = {
            let mut s = self.shared.borrow_mut();
            s.mods.push(ModState {
                name: name.clone(),
                folder: folder.to_path_buf(),
                error: None,
            });
            s.held.push(Vec::new());
            s.mods.len() - 1
        };
        let status = match self.run_script(idx, folder, &script) {
            Ok(()) => Status::Running,
            Err(e) => {
                self.shared.borrow_mut().disable(idx, &e);
                Status::Failed(e)
            }
        };
        Some((name, status))
    }

    fn run_script(&mut self, idx: usize, folder: &Path, script: &str) -> Result<(), String> {
        let rel = manifest::safe_relative(script)?;
        let path = folder.join(rel);
        let len = std::fs::metadata(&path)
            .map_err(|e| format!("cannot read {script}: {e}"))?
            .len();
        if len > manifest::MAX_SCRIPT {
            return Err(format!(
                "{script} is larger than {} bytes",
                manifest::MAX_SCRIPT
            ));
        }
        let code = std::fs::read(&path).map_err(|e| format!("cannot read {script}: {e}"))?;
        let env = api::build_env(&self.lua, &self.guard, &self.shared, idx, folder)
            .map_err(|e| format!("cannot set the mod up: {e}"))?;
        let function = self
            .lua
            .load(code)
            .set_name(format!(
                "={}/{}",
                folder
                    .file_name()
                    .map_or_else(|| "mod".into(), |n| n.to_string_lossy()),
                script
            ))
            .set_mode(mlua::prelude::LuaChunkMode::Text)
            .set_environment(env)
            .into_function()
            .map_err(|e| e.to_string())?;
        self.guard.start(self.config.budget.load);
        function.call::<()>(()).map_err(|e| e.to_string())
    }

    /// Calls `function` for mod `idx` with `args`, within the call budget; an
    /// error switches the mod off.
    fn call(&self, idx: usize, function: &Function, args: impl IntoLuaMulti) {
        if !self.shared.borrow().active(idx) {
            return;
        }
        self.guard.start(self.config.budget.call);
        if let Err(e) = function.call::<MultiValue>(args) {
            self.shared.borrow_mut().disable(idx, &e.to_string());
        }
    }

    fn handlers(&self, event: Event) -> Vec<(usize, Function)> {
        self.shared
            .borrow()
            .handlers
            .iter()
            .filter(|h| h.event == event)
            .map(|h| (h.mod_idx, h.function.clone()))
            .collect()
    }

    /// One pass of the mods' thread: what the menu did, keys, the menu
    /// opening, and, when presents have passed, the frame handlers; then what
    /// they drew and added is handed to the host. `frame` counts presents.
    pub fn pass(&mut self, frame: u64, now: Instant) {
        self.platform.refresh_input();
        {
            let mut s = self.shared.borrow_mut();
            s.draw.clear();
            s.frame = frame;
        }
        let key_handlers = self.handlers(Event::Key);
        if !key_handlers.is_empty() {
            for vk in keys::watched() {
                let down = self.platform.key_down(vk);
                if down != std::mem::replace(&mut self.keys_down[usize::from(vk)], down) {
                    let name = keys::name(vk);
                    for (idx, f) in &key_handlers {
                        self.call(*idx, f, (name.clone(), down));
                    }
                }
            }
        }
        let open_handlers = self.handlers(Event::MenuOpen);
        if !open_handlers.is_empty() {
            let open = self.platform.menu_open();
            if open && !self.menu_was_open {
                for (idx, f) in &open_handlers {
                    self.call(*idx, f, ());
                }
            }
            self.menu_was_open = open;
        }
        // Named by page before any of them runs: a mod that fails on the
        // first leaves the pages (and their numbers) of the others behind.
        let events: Vec<_> = {
            let s = self.shared.borrow();
            self.platform
                .menu_events()
                .into_iter()
                .filter_map(|e| Some((s.pages.get(e.page)?.uid, e.item, e.value)))
                .collect()
        };
        for (uid, item, value) in events {
            self.menu_event(uid, item, value);
        }
        if frame != self.last_frame {
            let dt = now
                .saturating_duration_since(self.last_frame_at)
                .as_secs_f64();
            self.last_frame = frame;
            self.last_frame_at = now;
            for (idx, f) in self.handlers(Event::Frame) {
                self.call(idx, &f, dt);
            }
        }
        self.after_pass();
    }

    fn menu_event(&mut self, uid: u64, item: usize, value: MenuValue) {
        let (callback, owner) = {
            let mut s = self.shared.borrow_mut();
            let Some(page) = s.pages.iter_mut().find(|p| p.uid == uid) else {
                return;
            };
            let owner = page.mod_idx;
            let Some(item) = page.items.get_mut(item) else {
                return;
            };
            match (&mut item.kind, value) {
                (ModKind::Toggle(on), MenuValue::Bool(v)) => *on = v,
                (ModKind::Number { value, .. }, MenuValue::Number(v)) => *value = v,
                _ => {}
            }
            (item.callback.clone(), owner)
        };
        let Some(function) = callback else { return };
        match value {
            MenuValue::Bool(v) => self.call(owner, &function, v),
            MenuValue::Number(v) => self.call(owner, &function, v),
            _ => self.call(owner, &function, ()),
        }
    }

    /// Hands the host what a pass produced.
    fn after_pass(&mut self) {
        let (draw, pages, held) = {
            let mut s = self.shared.borrow_mut();
            let draw = if s.draw.is_empty() && !self.drew {
                None
            } else {
                Some(std::mem::take(&mut s.draw))
            };
            let pages = std::mem::take(&mut s.pages_dirty).then(|| {
                s.pages
                    .iter()
                    .map(|p| ModPage {
                        title: p.title.clone(),
                        items: p
                            .items
                            .iter()
                            .map(|i| ModItem {
                                label: i.label.clone(),
                                help: i.help.clone(),
                                kind: i.kind.clone(),
                            })
                            .collect(),
                    })
                    .collect::<Vec<_>>()
            });
            let held = std::mem::take(&mut s.held_dirty).then(|| {
                let mut all: Vec<u8> = s.held.iter().flatten().copied().collect();
                all.sort_unstable();
                all.dedup();
                all
            });
            (draw, pages, held)
        };
        if let Some(draw) = draw {
            self.drew = !draw.is_empty();
            self.platform.publish_overlay(&draw);
        }
        if let Some(pages) = pages {
            self.platform.set_menu_pages(pages);
        }
        if let Some(held) = held {
            self.platform.inject_keys(&held);
        }
    }

    /// Whether any mod listens for `event` (the thread polls what is listened to).
    pub fn listens(&self) -> bool {
        !self.shared.borrow().handlers.is_empty()
    }

    /// Drops everything a mod put out: drawings, pages, held keys.
    pub fn clear_host(&mut self) {
        {
            let mut s = self.shared.borrow_mut();
            s.draw.clear();
            s.pages.clear();
            s.pages_dirty = true;
            s.held.iter_mut().for_each(Vec::clear);
            s.held_dirty = true;
        }
        self.drew = true;
        self.after_pass();
    }

    /// The time `leonida.time()` counts from.
    pub fn uptime(&self) -> Duration {
        self.shared.borrow().started.elapsed()
    }

    #[cfg(test)]
    pub(crate) fn shared(&self) -> &Rc<RefCell<Shared>> {
        &self.shared
    }
}
