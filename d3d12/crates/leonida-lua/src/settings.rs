//! The live settings `leonida.get` and `leonida.set` reach: the ones the F1
//! and F4 menus change, through the same run-time knobs
//! (`leonida_menu::knobs`). Nothing else is reachable by name.

use leonida_menu::knobs::{self, Cap};

/// The names, in the order the API lists them.
pub const NAMES: [&str; 5] = [
    "fps_cap",
    "dlss_sharpness",
    "panel",
    "menu_scale",
    "game_speed",
];

#[derive(Clone, Debug, PartialEq)]
pub enum Setting {
    Bool(bool),
    Number(f64),
    Text(&'static str),
}

/// The value now; `None` for an unknown name or a setting this run does not have.
pub fn get(name: &str) -> Option<Setting> {
    match name {
        "fps_cap" => Some(match knobs::FPS_CAP.get()? {
            Cap::Title => Setting::Text("game"),
            Cap::Unlimited => Setting::Number(0.0),
            Cap::Fps(n) => Setting::Number(f64::from(n)),
        }),
        "dlss_sharpness" => knobs::SHARPNESS
            .get()
            .map(|v| Setting::Number(f64::from(v))),
        "panel" => knobs::PANEL.get().map(Setting::Bool),
        "menu_scale" => Some(Setting::Number(f64::from(knobs::MENU_SCALE.get()))),
        "game_speed" => knobs::clock::HOOKED
            .load(std::sync::atomic::Ordering::Relaxed)
            .then(|| Setting::Number(knobs::clock::rate())),
        _ => None,
    }
}

/// Sets one; the reason when it cannot be.
pub fn set(name: &str, value: &Setting) -> Result<(), String> {
    let number = |what: &str| match value {
        Setting::Number(n) if n.is_finite() => Ok(*n),
        _ => Err(format!("{name} takes a number ({what})")),
    };
    match name {
        "fps_cap" => {
            let cap = match value {
                Setting::Text("game" | "title") => Cap::Title,
                Setting::Number(n) if n.is_finite() && *n == 0.0 => Cap::Unlimited,
                Setting::Number(n) if n.is_finite() && (1.0..=1000.0).contains(n) => {
                    Cap::Fps(n.round() as u32)
                }
                _ => {
                    return Err(
                        "fps_cap takes frames a second (1 to 1000), 0 for none, or \"game\"".into(),
                    );
                }
            };
            knobs::FPS_CAP.set(cap)
        }
        "dlss_sharpness" => {
            let v = number("0 to 1")?;
            knobs::SHARPNESS.set(v.clamp(0.0, 1.0) as f32)
        }
        "panel" => match value {
            Setting::Bool(on) => knobs::PANEL.set(*on),
            _ => Err("panel takes true or false".into()),
        },
        "menu_scale" => {
            let v = number("0.6 to 1.6")?;
            knobs::MENU_SCALE.set(v.clamp(0.6, 1.6) as f32);
            Ok(())
        }
        "game_speed" => {
            let v = number("0.1 to 2")?;
            if !knobs::clock::HOOKED.load(std::sync::atomic::Ordering::Relaxed) {
                return Err("game_speed is not available: the title's clock is not the runtime's in this run".into());
            }
            knobs::clock::set_rate(v.clamp(0.1, 2.0), knobs::clock::host_now());
            Ok(())
        }
        _ => Err(format!(
            "`{name}` is not a setting a mod can change (they are: {})",
            NAMES.join(", ")
        )),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_the_named_settings_are_reachable() {
        assert!(get("anything_else").is_none());
        assert!(set("LEONIDA_FPS_CAP", &Setting::Number(30.0)).is_err());
        assert!(
            set("frame_gen", &Setting::Bool(true))
                .unwrap_err()
                .contains("fps_cap")
        );
    }

    #[test]
    fn the_menus_size_is_read_clamped_and_set() {
        assert!(set("menu_scale", &Setting::Text("big")).is_err());
        assert!(set("menu_scale", &Setting::Number(f64::NAN)).is_err());
        set("menu_scale", &Setting::Number(9.0)).unwrap();
        assert_eq!(get("menu_scale"), Some(Setting::Number(1.600000023841858)));
        set("menu_scale", &Setting::Number(1.0)).unwrap();
    }

    #[test]
    fn what_this_run_does_not_have_is_refused() {
        // No renderer here: the cap, sharpness and panel have no owner.
        assert!(get("panel").is_none());
        assert!(set("panel", &Setting::Bool(true)).is_err());
        assert!(set("fps_cap", &Setting::Number(0.5)).is_err());
        assert!(set("fps_cap", &Setting::Number(60.0)).is_err());
        assert!(
            set("game_speed", &Setting::Number(0.5))
                .unwrap_err()
                .contains("not available")
        );
    }
}
