//! D3D12.X on host D3D12.
//!
//! D3D12.X records commands from code inlined into the title, which writes
//! packets into the command list while its put pointer is below a limit and
//! calls the list's method otherwise. Our lists keep the limits at zero, so
//! every command arrives here as a call, to be run on host D3D12. The
//! title's GPU addresses are CPU addresses (console memory is unified).
//!
//! [`device`] is the device object titles create, [`dxgi`] its adapter side,
//! [`resource`] heaps and resources, [`fence`] fences, [`queue`] command
//! queues, [`list`] command allocators and lists, [`descriptor`]
//! descriptor heaps, [`root`] root signatures, [`query`] query heaps,
//! [`signature`] command signatures, [`pipeline`] pipeline states,
//! [`frame`] frame pacing, [`object`] what all console graphics objects
//! share, [`console`] which console model the title sees. Planned:
//! `present` (swap chain).

// With `mimalloc` the process allocates through mimalloc. On Windows with
// diagnostics, copystat's counting allocator wraps the same backend instead.
#[cfg(all(feature = "mimalloc", not(all(windows, feature = "diagnostics"))))]
#[global_allocator]
static HEAP: mimalloc::MiMalloc = mimalloc::MiMalloc;

#[cfg(windows)]
mod ambient;
#[cfg(all(windows, feature = "diagnostics"))]
mod capture;
#[cfg(windows)]
mod compose;
pub mod console;
#[cfg(all(windows, feature = "diagnostics"))]
mod copystat;
#[cfg(all(windows, feature = "diagnostics"))]
mod faultbook;
#[cfg(windows)]
mod cores;
pub mod descriptor;
pub mod device;
#[cfg(windows)]
mod draw;
#[cfg(any(windows, test))]
mod drops;
#[cfg(windows)]
mod rootcache;
pub mod dxgi;
pub mod fence;
pub mod frame;
#[cfg(all(windows, feature = "diagnostics"))]
mod book;
#[cfg(windows)]
mod host;

/// LOCAL (11/26): the F1 settings menu is shown (host/menu.rs): the title gets no keyboard.
pub static SETTINGS_MENU_OPEN: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
/// Leonida: the game window holds the mouse for mouse look (leonida-hle `gameinput`): its window procedures hide the
/// cursor over the client area while this is set.
pub static MOUSE_CAPTURED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
pub mod aspect;
#[cfg(windows)]
pub mod streamline;
#[cfg(windows)]
mod fg;
#[cfg(windows)]
mod indirect;
#[cfg(windows)]
mod inplace;
#[cfg(windows)]
mod port_journal;
#[cfg(windows)]
mod port;
#[cfg(windows)]
mod port_cbv;
#[cfg(windows)]
pub mod port_descriptors;
#[cfg(windows)]
mod land;
pub mod list;
pub mod object;
pub mod pipeline;
#[cfg(windows)]
mod picked;
#[cfg(windows)]
mod pool;
#[cfg(all(windows, feature = "diagnostics"))]
mod popwatch;
#[cfg(windows)]
mod primitive;
pub mod query;
pub mod queue;
#[cfg(all(windows, feature = "diagnostics"))]
pub mod replay;
#[cfg(windows)]
mod residency;
pub mod resource;
pub mod root;
#[cfg(windows)]
mod stretch;
#[cfg(all(windows, feature = "dlss"))]
mod dlss;
#[cfg(windows)]
mod safecopy;
#[cfg(windows)]
mod sampler;
#[cfg(windows)]
pub mod shader_cache;
#[cfg(windows)]
mod memory;
#[cfg(windows)]
mod shadow;
#[cfg(windows)]
mod tiles;
pub mod signature;
mod switch;
#[cfg(all(windows, feature = "diagnostics"))]
mod ab;
#[cfg(windows)]
mod tdr;
#[cfg(windows)]
mod texture;
pub mod timeline;
#[cfg(windows)]
pub mod titlewrite;
#[cfg(windows)]
mod upload;
#[cfg(windows)]
mod watch;
#[cfg(windows)]
mod texback;

#[cfg(windows)]
pub use host::on_failure;

/// Whether `switch` is on now, if it is one of those `LEONIDA_AB` turns on and
/// off while the title runs (`ab`).
#[cfg(all(windows, feature = "diagnostics"))]
pub fn switched(switch: &str) -> Option<bool> {
    ab::pick(switch)
}

/// What the frame is at (`copystat::phase`), for the sampling profiler.
#[cfg(all(windows, feature = "diagnostics"))]
pub fn frame_phase() -> u32 {
    copystat::phase()
}
#[cfg(windows)]
pub use inplace::{title_memory_freeing, title_memory_section, title_memory_section_gone, title_memory_view};
#[cfg(windows)]
pub use shadow::title_memory_write_combined;
#[cfg(windows)]
pub use watch::{
    kernel_writing, remap_boost, title_memory_allocated, title_memory_freed, title_memory_reading, title_memory_remapped,
    title_memory_remapping, title_memory_splitting, title_memory_unmapping, title_memory_written,
    writing as kernel_will_write,
};
