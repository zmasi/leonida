//! The title's `QueryPerformanceCounter`, at the rate Leonida's trainer
//! menu sets (World, Game speed): the host's count until the rate is first changed,
//! then a continuous count at that rate (`leonida_menu::knobs::clock`).
//! Only the import the title makes from the profile API set is served; the
//! runtime's own timing and the DLLs the title loads keep the host's clock,
//! and so does what the title times with the processor's time-stamp counter
//! (`rdtsc`, which no import reaches). Not served with `LEONIDA_MENU=0`.
#![allow(non_snake_case)]

use std::sync::atomic::Ordering;

use leonida_menu::knobs::clock;

use crate::{HleModule, hle_export};

/// `QueryPerformanceCounter`: the title's count.
pub extern "system" fn QueryPerformanceCounter(out: *mut i64) -> i32 {
    if out.is_null() {
        return 0;
    }
    if !clock::HOOKED.load(Ordering::Relaxed) {
        clock::HOOKED.store(true, Ordering::Relaxed);
    }
    let host = clock::host_now();
    // SAFETY: the caller's out pointer, checked for null like the host's.
    unsafe { out.write(clock::title(host)) };
    1
}

pub(crate) fn module() -> HleModule {
    HleModule {
        name: "clock",
        dlls: &["api-ms-win-core-profile-*"],
        exports: if leonida_menu::enabled() {
            vec![hle_export!(QueryPerformanceCounter)]
        } else {
            Vec::new()
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_title_count_follows_the_host_until_the_rate_changes() {
        let mut a = 0i64;
        assert_eq!(QueryPerformanceCounter(&mut a), 1);
        let host = clock::host_now();
        assert!(host >= a && a > 0);
        assert_eq!(QueryPerformanceCounter(std::ptr::null_mut()), 0);
    }
}
