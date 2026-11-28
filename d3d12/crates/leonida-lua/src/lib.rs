//! Leonida's Lua modding API (`docs/lua-modding.md`): Lua 5.4 mods in
//! `mods\<name>\` (a `mod.json` with `"script": "main.lua"`), run in a
//! sandbox on a thread of their own, with what the runtime itself controls:
//! frame, key and menu events, an overlay, its live settings, pages in the
//! F4 trainer menu, checked reads and writes of the title's memory and a
//! pattern scan. Nothing of the title's own scripts is reachable (the runtime
//! cannot call them: `docs/mod-menu.md`).
//!
//! `LEONIDA_LUA=0` turns it all off: no thread, and a present pays one atomic
//! load in [`tick`]. Otherwise [`start`] runs the thread once; the renderer
//! calls [`tick`] at every present.
//!
//! - [`sandbox`]: the Lua state and what a mod can reach in it.
//! - [`api`]: the `leonida` table.
//! - [`runtime`]: the mods, and the passes that call their handlers.
//! - [`memory`], [`keys`], [`input`], [`settings`], [`manifest`]: what the API stands on.
//! - [`platform`]: what the host provides (a fake in the tests).

pub mod api;
pub mod input;
mod json;
pub mod keys;
pub mod manifest;
pub mod memory;
pub mod platform;
pub mod runtime;
pub mod sandbox;
pub mod settings;

#[cfg(test)]
mod tests;

pub use api::API_VERSION;
pub use memory::{Host as MemoryHost, set_host as set_memory_host, set_image};

use std::rc::Rc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Once, OnceLock};
use std::thread::Thread;
use std::time::{Duration, Instant};

/// Whether the Lua mods are on: unless `LEONIDA_LUA=0`.
pub fn enabled() -> bool {
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_LUA").is_none_or(|v| v != "0"))
}

/// The thread is running.
static RUNNING: AtomicBool = AtomicBool::new(false);
/// Some mod has handlers: a present wakes the thread.
static LISTENING: AtomicBool = AtomicBool::new(false);
/// Presents so far.
static FRAME: AtomicU64 = AtomicU64::new(0);
static THREAD: OnceLock<Thread> = OnceLock::new();

/// A frame was presented. All a present pays: one load while the mods are
/// off, and, while they run, an add and a load (and a wake-up when a mod has
/// handlers). It never waits for the mods.
#[inline]
pub fn tick() {
    if !RUNNING.load(Ordering::Relaxed) {
        return;
    }
    FRAME.fetch_add(1, Ordering::Relaxed);
    if LISTENING.load(Ordering::Relaxed)
        && let Some(thread) = THREAD.get()
    {
        thread.unpark();
    }
}

/// Starts the mods' thread (once; nothing with `LEONIDA_LUA=0`). The
/// renderer calls it when its presenter is made.
pub fn start() {
    static STARTED: Once = Once::new();
    if !enabled() {
        return;
    }
    STARTED.call_once(|| {
        leonida_menu::mods::enable();
        let spawned = std::thread::Builder::new()
            .name("leonida-lua".into())
            .spawn(run);
        match spawned {
            Ok(handle) => {
                let _ = THREAD.set(handle.thread().clone());
                RUNNING.store(true, Ordering::Release);
            }
            Err(e) => tracing::warn!(error = %e, "the Lua mods' thread could not be started"),
        }
    });
}

/// A fresh runtime with the mods of the mods folder loaded.
fn fresh(platform: &Rc<dyn platform::Platform>) -> Option<runtime::Runtime> {
    let mut rt = match runtime::Runtime::new(platform.clone(), runtime::Config::default()) {
        Ok(rt) => rt,
        Err(e) => {
            tracing::warn!(error = %e, "the Lua state could not be made; no Lua mods");
            return None;
        }
    };
    match manifest::mods_dir() {
        Some(dir) => {
            let report = rt.load_dir(&dir);
            let running = report
                .iter()
                .filter(|(_, s)| *s == runtime::Status::Running)
                .count();
            tracing::info!(folder = %dir.display(), mods = report.len(), running, "Lua mods");
            for (name, status) in &report {
                if let runtime::Status::Failed(why) = status {
                    tracing::warn!(%name, %why, "Lua mod not running");
                }
            }
        }
        None => tracing::info!("Lua mods: no mods folder (LEONIDA_MODS names one)"),
    }
    Some(rt)
}

fn run() {
    let platform: Rc<dyn platform::Platform> = Rc::new(platform::Real::new());
    let mut rt = fresh(&platform);
    let mut crashes = 0;
    loop {
        let listening = rt.as_ref().is_some_and(|r| r.listens());
        LISTENING.store(listening, Ordering::Relaxed);
        std::thread::park_timeout(Duration::from_millis(if listening { 16 } else { 250 }));
        let reload = platform.take_reload();
        let step = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            if reload {
                if let Some(old) = rt.as_mut() {
                    old.clear_host();
                }
                // Dropped first: the old state is gone before the new loads.
                drop(rt.take());
                rt = fresh(&platform);
            } else if let Some(r) = rt.as_mut() {
                r.pass(FRAME.load(Ordering::Relaxed), Instant::now());
            }
        }));
        if step.is_err() {
            crashes += 1;
            tracing::error!(
                crashes,
                "the Lua mods' runtime failed; the mods are stopped"
            );
            if let Some(old) = rt.as_mut() {
                old.clear_host();
            }
            rt = None;
            if crashes >= 3 {
                LISTENING.store(false, Ordering::Relaxed);
                RUNNING.store(false, Ordering::Release);
                return;
            }
        }
    }
}

/// Tests that touch process-wide settings take turns.
#[cfg(test)]
pub(crate) static TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());
