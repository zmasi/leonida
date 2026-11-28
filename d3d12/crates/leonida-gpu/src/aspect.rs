//! Leonida: a window of another shape than the title's picture (ultrawide monitors).
//!
//! The title outputs 16:9 (3840x2160 on Series X) whatever its window is, and the swap chain was stretched over
//! the window. Two settings, the ones `docs/ultrawide.md` gives the Vulkan runtime:
//!
//! - `LEONIDA_ASPECT=21:9|32:9|16:9|W:H|<decimal>|auto`: the shape the title's window is made in (`auto`: the
//!   primary monitor's). Read by leonida-hle's `lifecycle.rs`, which widens the window the title asks for.
//! - `LEONIDA_PRESENT_SCALE=fit|stretch`: `fit` keeps the picture's shape in a window of another one, centred,
//!   with black bars beside it (or above and below); `stretch` fills the window as before. Unset: `fit` with
//!   `LEONIDA_ASPECT`, else `stretch`. The F1 menu changes it while running.
//!
//! With `fit` the swap chain takes the window's shape at the picture's height (or width), the picture is copied
//! into its middle unscaled and the rest is cleared ([`frame`], used by the presenter in `host.rs`); the window
//! then scales it evenly. Neither makes the title draw a wider view: that is the title's output size
//! (`LEONIDA_OUTPUT`, leonida-loader's `hooks.rs`, and `docs/ultrawide.md` for what is known of it).
#![cfg_attr(not(windows), allow(dead_code))]

use std::sync::atomic::{AtomicU8, Ordering};

/// What `LEONIDA_ASPECT` can say.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Asked {
    /// Width over height.
    Ratio(f64),
    /// The primary monitor's shape.
    Auto,
}

/// `LEONIDA_ASPECT`'s text: `W:H` (or `WxH`, `W/H`), a decimal, or `auto`; a shape from 1 to 4 wide per high.
/// `off`, `none` and nothing are no shape.
pub fn parse(text: &str) -> Option<Asked> {
    let text = text.trim();
    if text.eq_ignore_ascii_case("auto") {
        return Some(Asked::Auto);
    }
    let ratio = match text.split_once([':', 'x', 'X', '/']) {
        Some((w, h)) => {
            let (w, h): (f64, f64) = (w.trim().parse().ok()?, h.trim().parse().ok()?);
            if h <= 0.0 {
                return None;
            }
            w / h
        }
        None => text.parse().ok()?,
    };
    (ratio.is_finite() && (1.0..=4.0).contains(&ratio)).then_some(Asked::Ratio(ratio))
}

/// The shape `LEONIDA_ASPECT` asks for as width over height, `auto` taken from `screen` (the primary monitor's
/// size, if known).
pub fn resolve(asked: Option<Asked>, screen: Option<(u32, u32)>) -> Option<f64> {
    match asked? {
        Asked::Ratio(r) => Some(r),
        Asked::Auto => screen.filter(|&(w, h)| w > 0 && h > 0).map(|(w, h)| f64::from(w) / f64::from(h)),
    }
}

/// The window shape asked for in the environment, if any.
pub fn asked() -> Option<f64> {
    static ASKED: std::sync::OnceLock<Option<f64>> = std::sync::OnceLock::new();
    *ASKED.get_or_init(|| {
        let text = std::env::var("LEONIDA_ASPECT").ok()?;
        let asked = parse(&text);
        if asked.is_none() && !matches!(text.trim().to_ascii_lowercase().as_str(), "" | "off" | "none") {
            tracing::warn!(text, "LEONIDA_ASPECT wants 21:9, 32:9, W:H, a number from 1 to 4, or auto; ignored");
        }
        resolve(asked, screen())
    })
}

/// The primary monitor's size in pixels.
#[cfg(windows)]
fn screen() -> Option<(u32, u32)> {
    use windows_sys::Win32::UI::WindowsAndMessaging::{GetSystemMetrics, SM_CXSCREEN, SM_CYSCREEN};
    // SAFETY: plain queries.
    let (w, h) = unsafe { (GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)) };
    (w > 0 && h > 0).then_some((w as u32, h as u32))
}

#[cfg(not(windows))]
fn screen() -> Option<(u32, u32)> {
    None
}

/// A window the title asks for as `width` x `height`, in the shape `aspect` at the same height. Unchanged when it
/// has that shape already (within a pixel) or no size.
pub fn widened(width: i32, height: i32, aspect: f64) -> (i32, i32) {
    if width <= 0 || height <= 0 {
        return (width, height);
    }
    let wide = (f64::from(height) * aspect).round() as i32;
    if (wide - width).abs() <= 1 { (width, height) } else { (wide, height) }
}

const UNSET: u8 = 0;
const FIT: u8 = 1;
const STRETCH: u8 = 2;
/// `LEONIDA_PRESENT_SCALE` as last read or set; [`UNSET`] before the first look.
static SCALE: AtomicU8 = AtomicU8::new(UNSET);

/// Whether `LEONIDA_PRESENT_SCALE`'s text means fit, with `shaped` saying whether a window shape was asked for
/// (what an unset or unknown text goes by).
pub fn fit_from(text: Option<&str>, shaped: bool) -> bool {
    match text.map(|t| t.trim().to_ascii_lowercase()).as_deref() {
        Some("fit") => true,
        Some("stretch") => false,
        _ => shaped,
    }
}

/// Whether the picture keeps its shape in a window of another one.
pub fn fit() -> bool {
    match SCALE.load(Ordering::Relaxed) {
        FIT => true,
        STRETCH => false,
        _ => {
            let text = std::env::var("LEONIDA_PRESENT_SCALE").ok();
            let fit = fit_from(text.as_deref(), asked().is_some());
            SCALE.store(if fit { FIT } else { STRETCH }, Ordering::Relaxed);
            fit
        }
    }
}

/// The F1 menu changed `LEONIDA_PRESENT_SCALE` (the variable is set already).
pub fn changed() {
    SCALE.store(UNSET, Ordering::Relaxed);
}

/// Where a picture goes in a frame of a window's shape: the frame's size, and the picture's top left corner in it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Frame {
    pub width: u32,
    pub height: u32,
    pub left: u32,
    pub top: u32,
}

impl Frame {
    /// A frame that is the picture.
    pub const fn whole(width: u32, height: u32) -> Self {
        Frame { width, height, left: 0, top: 0 }
    }

    /// Whether the picture fills it.
    pub const fn is_whole(&self, picture: (u32, u32)) -> bool {
        self.width == picture.0 && self.height == picture.1
    }
}

/// The largest side of a frame: Direct3D 12's limit for a texture.
const MOST: u32 = 16384;

/// The frame for a `picture` shown in a window whose client area is `window`: the window's shape at the
/// picture's height (a wider window: bars at the sides) or at its width (a narrower one: bars above and below),
/// the picture in the middle. A window of the picture's shape within one part in a hundred, or of no size (a
/// minimised one), gives the picture itself. Sides are even, so the picture sits on whole pixels.
pub fn frame(picture: (u32, u32), window: (u32, u32)) -> Frame {
    let (w, h) = picture;
    if w == 0 || h == 0 || window.0 == 0 || window.1 == 0 {
        return Frame::whole(w, h);
    }
    let shape = f64::from(w) / f64::from(h);
    let wanted = f64::from(window.0) / f64::from(window.1);
    if (wanted / shape - 1.0).abs() < 0.01 {
        return Frame::whole(w, h);
    }
    // To the even number at or above, so that the bars are equal.
    let even = |v: f64| ((v.round().min(f64::from(MOST)) as u32) + 1) & !1;
    if wanted > shape {
        let width = even(f64::from(h) * wanted).max(w);
        Frame { width, height: h, left: (width - w) / 2, top: 0 }
    } else {
        let height = even(f64::from(w) / wanted).max(h);
        Frame { width: w, height, left: 0, top: (height - h) / 2 }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shapes_read_as_ratios_decimals_and_auto() {
        assert_eq!(parse("21:9"), Some(Asked::Ratio(21.0 / 9.0)));
        assert_eq!(parse(" 32:9 "), Some(Asked::Ratio(32.0 / 9.0)));
        assert_eq!(parse("16:9"), Some(Asked::Ratio(16.0 / 9.0)));
        assert_eq!(parse("3440x1440"), Some(Asked::Ratio(3440.0 / 1440.0)));
        assert_eq!(parse("43/18"), Some(Asked::Ratio(43.0 / 18.0)));
        assert_eq!(parse("2.37"), Some(Asked::Ratio(2.37)));
        assert_eq!(parse("AUTO"), Some(Asked::Auto));
        for wrong in ["", "off", "none", "9:21", "0.5", "5", "21:0", "wide", "21:", ":9", "nan", "inf"] {
            assert_eq!(parse(wrong), None, "{wrong:?}");
        }
    }

    #[test]
    fn auto_is_the_screens_shape() {
        assert_eq!(resolve(Some(Asked::Auto), Some((3440, 1440))), Some(3440.0 / 1440.0));
        assert_eq!(resolve(Some(Asked::Auto), None), None);
        assert_eq!(resolve(Some(Asked::Auto), Some((0, 1440))), None);
        assert_eq!(resolve(Some(Asked::Ratio(2.0)), None), Some(2.0));
        assert_eq!(resolve(None, Some((3440, 1440))), None);
    }

    #[test]
    fn a_window_is_widened_at_its_height() {
        // The title's 3840x2160 window, 21:9 and 32:9.
        assert_eq!(widened(3840, 2160, 21.0 / 9.0), (5040, 2160));
        assert_eq!(widened(3840, 2160, 32.0 / 9.0), (7680, 2160));
        assert_eq!(widened(3840, 2160, 16.0 / 9.0), (3840, 2160));
        assert_eq!(widened(1920, 1080, 3440.0 / 1440.0), (2580, 1080));
        // CW_USEDEFAULT and the like stay.
        assert_eq!(widened(i32::MIN, i32::MIN, 2.0), (i32::MIN, i32::MIN));
        assert_eq!(widened(0, 0, 2.0), (0, 0));
    }

    #[test]
    fn fit_is_the_default_only_with_a_shape() {
        assert!(fit_from(Some("fit"), false));
        assert!(fit_from(Some(" FIT "), false));
        assert!(!fit_from(Some("stretch"), true));
        assert!(fit_from(None, true));
        assert!(!fit_from(None, false));
        assert!(fit_from(Some(""), true));
        assert!(!fit_from(Some("other"), false));
    }

    #[test]
    fn a_window_of_the_pictures_shape_gets_the_picture() {
        let picture = (3840, 2160);
        for window in [(3840, 2160), (2560, 1440), (1920, 1080), (1280, 720), (1920, 1090), (0, 0), (0, 1080), (1920, 0)] {
            assert_eq!(frame(picture, window), Frame::whole(3840, 2160), "{window:?}");
            assert!(frame(picture, window).is_whole(picture));
        }
        assert_eq!(frame((0, 0), (1920, 1080)), Frame::whole(0, 0));
    }

    #[test]
    fn a_wider_window_gets_bars_at_the_sides() {
        // 21:9 exactly, a 3440x1440 monitor, 32:9.
        assert_eq!(frame((3840, 2160), (2520, 1080)), Frame { width: 5040, height: 2160, left: 600, top: 0 });
        assert_eq!(frame((3840, 2160), (3440, 1440)), Frame { width: 5160, height: 2160, left: 660, top: 0 });
        assert_eq!(frame((3840, 2160), (5120, 1440)), Frame { width: 7680, height: 2160, left: 1920, top: 0 });
        // Series S' 1920x1080 picture.
        assert_eq!(frame((1920, 1080), (3440, 1440)), Frame { width: 2580, height: 1080, left: 330, top: 0 });
    }

    #[test]
    fn a_narrower_window_gets_bars_above_and_below() {
        // 16:10 and 4:3.
        assert_eq!(frame((3840, 2160), (1920, 1200)), Frame { width: 3840, height: 2400, left: 0, top: 120 });
        assert_eq!(frame((3840, 2160), (1600, 1200)), Frame { width: 3840, height: 2880, left: 0, top: 360 });
    }

    #[test]
    fn frames_have_even_sides_hold_the_picture_and_stay_within_the_limit() {
        for picture in [(3840u32, 2160u32), (2560, 1440), (1920, 1080)] {
            for window in [(1001u32, 333u32), (333, 1001), (3439, 1441), (10000, 10), (10, 10000), (1717, 911)] {
                let f = frame(picture, window);
                assert!(f.width % 2 == 0 && f.height % 2 == 0, "{picture:?} in {window:?}: {f:?}");
                assert!(f.width <= MOST && f.height <= MOST, "{f:?}");
                assert!(f.left + picture.0 <= f.width && f.top + picture.1 <= f.height, "{f:?}");
                // One side is the picture's, and the bars are equal.
                assert!(f.width == picture.0 || f.height == picture.1, "{f:?}");
                assert_eq!((f.width - picture.0, f.height - picture.1), (2 * f.left, 2 * f.top), "{f:?}");
            }
        }
    }
}
