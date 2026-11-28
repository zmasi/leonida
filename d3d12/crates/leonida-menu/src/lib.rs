//! Leonida's in-game trainer menu (`LEONIDA_MENU=0` turns it off; F4, or
//! RB with d-pad Left, opens and closes it; `LEONIDA_MENU_OPEN=1` starts it
//! open). It stands beside the renderer's own F1 settings menu and F3
//! numbers panel (`leonida-gpu`, `host/menu.rs` and `host/panel.rs`).
//!
//! - [`model`]: pages, items, the cursor and its back stack.
//! - [`tree`]: Leonida's pages and the backend behind them.
//! - [`actions`]: the features that need the title's scripts (none can be
//!   done yet; `docs/mod-menu.md`).
//! - [`knobs`]: the run-time settings the rest of the runtime reads, those
//!   the renderer owns and lends, and what a present has to do for the menu
//!   in one atomic load.
//! - [`input`]: keys and pad buttons to presses, and what is kept from the
//!   title while the menu is open.
//! - [`font`], `gdi`, [`paint`]: glyphs from the system's fonts and the
//!   menu painted on the CPU; the GPU side lays it over the frame at
//!   present (`leonida-gpu`, `host/trainer.rs`).
//! - [`overlay`], [`mods`]: what the Lua mods (`leonida-lua`) draw over the
//!   picture, and the "Mods" page they add to the menu.
//! - [`png`]: an encoder for the previews.
//!
//! Single player only: nothing here reaches other players or anything
//! online.

pub mod actions;
mod app;
pub mod font;
#[cfg(windows)]
pub mod gdi;
pub mod input;
pub mod knobs;
pub mod model;
pub mod mods;
pub mod overlay;
pub mod paint;
pub mod png;
#[cfg(windows)]
mod poll;
pub mod tree;

pub use app::{
    App, Frame, VISIBLE, enabled, frame, is_open, preview, scale_for, set_height, start,
    system_fonts, withheld_key, withheld_pad,
};
pub use knobs::{DRAW, LUA_DRAW, SHOT, work};
