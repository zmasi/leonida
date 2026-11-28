//! Leonida's pages, grouped as classic single-player trainers group them,
//! and the backend behind them: the runtime's own settings for what the
//! runtime can do itself (DLSS's sharpness, the numbers panel, screenshots,
//! recording, the frame-rate cap, the title's clock rate), [`GameActions`]
//! for what needs the title's scripts.

use std::sync::atomic::Ordering;

use crate::actions::GameActions;
use crate::knobs::{self, Cap};
use crate::model::{Backend, Id, Item, Kind, Page, Value};

const MAIN: usize = 0;
const PLAYER: usize = 1;
const VEHICLE: usize = 2;
const SPAWN: usize = 3;
const WEAPONS: usize = 4;
const WORLD: usize = 5;
const TIME: usize = 6;
const WEATHER: usize = 7;
const TELEPORT: usize = 8;
const MISC: usize = 9;
const SETTINGS: usize = 10;
const DISPLAY: usize = 11;
const MENU: usize = 12;

/// The Lua mods' page, when the mods run (`crate::mods`): the first page
/// after Leonida's own, and the mods' pages follow it.
pub const MODS: usize = MENU + 1;

/// Frame-rate caps offered: the title's own pace, frames a second, none.
const CAPS: [&str; 8] = ["Game", "30", "40", "60", "90", "120", "144", "None"];
const WEATHERS: [&str; 9] = [
    "Extra sunny",
    "Clear",
    "Clouds",
    "Overcast",
    "Foggy",
    "Rain",
    "Thunder",
    "Smog",
    "Windy",
];

const fn toggle(id: Id, label: &'static str, help: &'static str) -> Item {
    Item::new(id, label, help, Kind::Toggle)
}
const fn action(id: Id, label: &'static str, help: &'static str) -> Item {
    Item::new(id, label, help, Kind::Action)
}
const fn page(id: Id, label: &'static str, help: &'static str, to: usize) -> Item {
    Item::new(id, label, help, Kind::Submenu(to))
}

/// The pages; the first is the main menu.
pub fn pages() -> Vec<Page> {
    let mut pages = vec![
        Page {
            title: "",
            items: Vec::new()
        };
        MENU + 1
    ];
    pages[MAIN] = Page {
        title: "Main menu",
        items: vec![
            page(
                "go.player",
                "Player",
                "Health, wanted level and abilities.",
                PLAYER,
            ),
            page(
                "go.vehicle",
                "Vehicle",
                "Spawn, repair and tune vehicles.",
                VEHICLE,
            ),
            page("go.weapons", "Weapons", "Weapons and ammunition.", WEAPONS),
            page(
                "go.world",
                "World",
                "The game's speed, gravity and traffic.",
                WORLD,
            ),
            page("go.time", "Time", "The time of day.", TIME),
            page("go.weather", "Weather", "The weather.", WEATHER),
            page("go.teleport", "Teleport", "Move the player.", TELEPORT),
            page(
                "go.misc",
                "Misc",
                "Screenshots, frame recording and the frame-rate cap.",
                MISC,
            ),
            page(
                "go.settings",
                "Settings",
                "Display settings and the menu itself.",
                SETTINGS,
            ),
        ],
    };
    pages[PLAYER] = Page {
        title: "Player",
        items: vec![
            toggle("player.god", "God mode", "The player takes no damage."),
            toggle(
                "player.never_wanted",
                "Never wanted",
                "The police never come after the player.",
            ),
            Item::new(
                "player.wanted",
                "Wanted level",
                "Set the wanted level.",
                Kind::Number {
                    min: 0.0,
                    max: 5.0,
                    step: 1.0,
                    decimals: 0,
                    suffix: "",
                },
            ),
            action(
                "player.clear_wanted",
                "Clear wanted level",
                "Lose the police at once.",
            ),
            action("player.heal", "Heal and armour", "Full health and armour."),
            toggle(
                "player.stamina",
                "Unlimited stamina",
                "Run and swim without tiring.",
            ),
            toggle("player.fast_run", "Fast run", "Run faster."),
            toggle("player.fast_swim", "Fast swim", "Swim faster."),
            toggle("player.super_jump", "Super jump", "Jump far higher."),
            toggle(
                "player.invisible",
                "Invisible",
                "Others cannot see the player.",
            ),
            toggle(
                "player.noiseless",
                "Noiseless",
                "The player makes no noise.",
            ),
            toggle(
                "player.no_ragdoll",
                "No ragdoll",
                "The player does not fall over.",
            ),
        ],
    };
    pages[VEHICLE] = Page {
        title: "Vehicle",
        items: vec![
            page(
                "go.spawn",
                "Spawn vehicle",
                "Pick a kind of vehicle to spawn.",
                SPAWN,
            ),
            action("vehicle.repair", "Repair", "Fix the current vehicle."),
            action("vehicle.clean", "Clean", "Wash the current vehicle."),
            action(
                "vehicle.flip",
                "Flip upright",
                "Put the current vehicle back on its wheels.",
            ),
            toggle(
                "vehicle.god",
                "Indestructible",
                "The current vehicle takes no damage.",
            ),
            toggle("vehicle.boost", "Speed boost", "Faster acceleration."),
            toggle(
                "vehicle.seatbelt",
                "Seatbelt",
                "Never thrown through the windscreen.",
            ),
            action(
                "vehicle.paint",
                "Random paint",
                "A new colour for the current vehicle.",
            ),
            toggle(
                "vehicle.spawn_inside",
                "Spawn inside",
                "Spawned vehicles are entered at once.",
            ),
        ],
    };
    pages[SPAWN] = Page {
        title: "Spawn vehicle",
        items: vec![
            action(
                "spawn.sports",
                "Sports car",
                "A sports car beside the player.",
            ),
            action("spawn.super", "Super car", "A super car beside the player."),
            action(
                "spawn.muscle",
                "Muscle car",
                "A muscle car beside the player.",
            ),
            action("spawn.suv", "SUV", "An SUV beside the player."),
            action(
                "spawn.offroad",
                "Off-road",
                "An off-road vehicle beside the player.",
            ),
            action(
                "spawn.bike",
                "Motorcycle",
                "A motorcycle beside the player.",
            ),
            action("spawn.boat", "Boat", "A boat, on water."),
            action(
                "spawn.heli",
                "Helicopter",
                "A helicopter beside the player.",
            ),
            action("spawn.plane", "Plane", "A small plane beside the player."),
        ],
    };
    pages[WEAPONS] = Page {
        title: "Weapons",
        items: vec![
            action(
                "weapons.all",
                "Get all weapons",
                "Every weapon, with ammunition.",
            ),
            toggle(
                "weapons.infinite_ammo",
                "Infinite ammo",
                "Ammunition never runs out.",
            ),
            toggle("weapons.no_reload", "No reload", "The clip never empties."),
            toggle(
                "weapons.fire",
                "Fire ammo",
                "Bullets set what they hit alight.",
            ),
            toggle("weapons.explosive", "Explosive ammo", "Bullets explode."),
            toggle(
                "weapons.explosive_melee",
                "Explosive melee",
                "Punches explode.",
            ),
            action("weapons.remove", "Remove all weapons", "Empty hands."),
        ],
    };
    pages[WORLD] = Page {
        title: "World",
        items: vec![
            Item::new(
                "world.speed",
                "Game speed",
                "Rate of the title's clock (QueryPerformanceCounter). Experimental: what the title times with other clocks keeps its pace.",
                Kind::Number {
                    min: 0.1,
                    max: 2.0,
                    step: 0.1,
                    decimals: 1,
                    suffix: "x",
                },
            ),
            toggle(
                "world.slow_motion",
                "Slow motion",
                "The title's clock at a third of its rate (experimental, as Game speed).",
            ),
            toggle("world.moon_gravity", "Moon gravity", "Low gravity."),
            toggle("world.no_traffic", "No traffic", "Empty roads."),
            toggle("world.no_peds", "No pedestrians", "Empty pavements."),
            toggle("world.no_police", "No random police", "No police patrols."),
            toggle("world.blackout", "Blackout", "City lights off."),
        ],
    };
    pages[TIME] = Page {
        title: "Time",
        items: vec![
            Item::new(
                "time.hour",
                "Hour",
                "Set the time of day.",
                Kind::Number {
                    min: 0.0,
                    max: 23.0,
                    step: 1.0,
                    decimals: 0,
                    suffix: ":00",
                },
            ),
            action("time.forward", "Hour forward", "One hour on."),
            action("time.back", "Hour back", "One hour back."),
            toggle(
                "time.paused",
                "Clock paused",
                "The time of day stands still.",
            ),
            toggle(
                "time.system",
                "Sync with system",
                "The time of day follows the PC's clock.",
            ),
        ],
    };
    pages[WEATHER] = Page {
        title: "Weather",
        items: vec![
            Item::new(
                "weather.type",
                "Weather",
                "Pick the weather.",
                Kind::Choice(&WEATHERS),
            ),
            toggle(
                "weather.freeze",
                "Freeze weather",
                "The weather does not change.",
            ),
            toggle("weather.wind", "Strong wind", "Wind at its strongest."),
        ],
    };
    pages[TELEPORT] = Page {
        title: "Teleport",
        items: vec![
            action(
                "teleport.waypoint",
                "To waypoint",
                "To the waypoint set on the map.",
            ),
            action(
                "teleport.save",
                "Save position",
                "Remember where the player is.",
            ),
            action(
                "teleport.load",
                "Saved position",
                "Back to the remembered place.",
            ),
            action("teleport.forward", "Forward 10 m", "A short hop forward."),
            action(
                "teleport.vice",
                "To Vice City",
                "To the position in LEONIDA_VICE_CITY (x,y,z in metres), held in place for a few seconds while the ground loads.",
            ),
        ],
    };
    pages[MISC] = Page {
        title: "Misc",
        items: vec![
            action(
                "misc.screenshot",
                "Save screenshot",
                "Saves the next frame, without the menu, as a PNG (LEONIDA_SCREENSHOTS, else screenshots beside the runtime).",
            ),
            toggle(
                "misc.record",
                "Frame recording",
                "Saves every presented frame into LEONIDA_CAPTURE while on (as a file named rec there does).",
            ),
            Item::new(
                "misc.fps_cap",
                "Frame-rate cap",
                "Frames a second at most (LEONIDA_FPS_CAP; the F1 menu's FRAME RATE CAP is the same setting). Game: the title's own pace. None: as fast as it renders.",
                Kind::Choice(&CAPS),
            ),
            toggle("misc.hide_hud", "Hide HUD", "No HUD or radar."),
            toggle(
                "misc.radio_off",
                "Mobile radio off",
                "No radio outside vehicles.",
            ),
        ],
    };
    pages[SETTINGS] = Page {
        title: "Settings",
        items: vec![
            page(
                "go.display",
                "Display",
                "Sharpening and the FPS panel.",
                DISPLAY,
            ),
            page("go.menu", "Menu", "The menu's size.", MENU),
            action("settings.about", "About", "What this menu is."),
        ],
    };
    pages[DISPLAY] = Page {
        title: "Display",
        items: vec![
            Item::new(
                "display.sharpen",
                "Sharpen strength",
                "DLSS's sharpening of the game's picture, 0 to 1 (LEONIDA_DLSS_SHARPNESS; the F1 menu's DLSS SHARPNESS is the same setting).",
                Kind::Number {
                    min: 0.0,
                    max: 1.0,
                    step: 0.05,
                    decimals: 2,
                    suffix: "",
                },
            ),
            toggle(
                "display.panel",
                "FPS panel",
                "The frame-rate and memory panel in the corner (F3 too).",
            ),
        ],
    };
    pages[MENU] = Page {
        title: "Menu",
        items: vec![Item::new(
            "menu.scale",
            "Menu size",
            "The menu's size against the picture's height.",
            Kind::Number {
                min: 0.6,
                max: 1.6,
                step: 0.05,
                decimals: 2,
                suffix: "x",
            },
        )],
    };
    pages
}

/// Leonida's pages and, while the Lua mods run, the Mods page and theirs
/// (a "Mods" item last on the main page).
pub fn pages_with_mods() -> Vec<Page> {
    with_mods(pages(), crate::mods::enabled())
}

fn with_mods(mut all: Vec<Page>, on: bool) -> Vec<Page> {
    if on {
        all[MAIN].items.push(page(
            "go.mods",
            "Mods",
            "Pages the Lua mods add, and a reload of them.",
            MODS,
        ));
        all.extend(crate::mods::pages(MODS));
    }
    all
}

/// Why a runtime feature is missing.
const NO_PANEL: &str =
    "Not in this build: the FPS panel is part of diagnostics builds (--features diagnostics).";
const NO_RECORD: &str =
    "Not available: frame recording needs a diagnostics build and LEONIDA_CAPTURE set to a folder.";
const NO_CLOCK: &str = "Not available: the title's clock is not the runtime's in this run (LEONIDA_MENU=0, or no title loaded).";
const NO_SHARPNESS: &str = "Not available: the title's upscale is not DLSS in this run (LEONIDA_UPSCALE=dlss and a build with the dlss feature).";
const NO_CAP: &str = "Not available: nothing is presented by this runtime yet.";
const NO_SHOT: &str = "Not in this build: screenshots are read back by the diagnostics builds' frame capture (--features diagnostics).";

/// The backend: runtime settings, else the game's scripts.
pub struct Leonida {
    pub game: Box<dyn GameActions>,
    /// The rate before slow motion went on.
    before_slow: f64,
}

impl Leonida {
    pub fn new(game: Box<dyn GameActions>) -> Leonida {
        Leonida {
            game,
            before_slow: 1.0,
        }
    }
}

/// The cap of `CAPS[index]`.
fn cap_of(index: usize) -> Cap {
    match CAPS.get(index) {
        Some(&"None") => Cap::Unlimited,
        Some(c) => c.parse().map_or(Cap::Title, Cap::Fps),
        None => Cap::Title,
    }
}

/// Where `cap` is among `CAPS`; a rate that is not offered (set with
/// `LEONIDA_FPS_CAP` or in the F1 menu) shows as the nearest that is.
fn cap_index(cap: Cap) -> usize {
    match cap {
        Cap::Title => 0,
        Cap::Unlimited => CAPS.len() - 1,
        Cap::Fps(n) => CAPS
            .iter()
            .enumerate()
            .filter_map(|(i, c)| Some((i, c.parse::<u32>().ok()?)))
            .min_by_key(|(_, c)| c.abs_diff(n))
            .map_or(0, |(i, _)| i),
    }
}

impl Backend for Leonida {
    fn value(&self, id: Id) -> Value {
        match id {
            "display.sharpen" => {
                // Rounded to the item's two decimals: the owner keeps it as
                // a text read as an f32.
                let v = f64::from(knobs::SHARPNESS.get().unwrap_or(0.0));
                Value::Number((v.clamp(0.0, 1.0) * 100.0).round() / 100.0)
            }
            "display.panel" => Value::Bool(knobs::PANEL.get().unwrap_or(false)),
            "misc.record" => Value::Bool(knobs::RECORD.get().unwrap_or(false)),
            "misc.fps_cap" => Value::Index(cap_index(knobs::FPS_CAP.get().unwrap_or(Cap::Title))),
            "menu.scale" => Value::Number(f64::from(knobs::MENU_SCALE.get())),
            id if id.starts_with("mod.") => crate::mods::value(id),
            "world.speed" => Value::Number(knobs::clock::rate()),
            "world.slow_motion" => Value::Bool((knobs::clock::rate() - 1.0 / 3.0).abs() < 1e-6),
            id if id.starts_with("go.") || id == "misc.screenshot" || id == "settings.about" => {
                Value::None
            }
            id => self.game.value(id),
        }
    }

    fn set(&mut self, id: Id, value: Value) -> Result<Option<String>, String> {
        match (id, value) {
            ("display.sharpen", Value::Number(v)) => {
                knobs::SHARPNESS.set((v as f32).clamp(0.0, 1.0))?
            }
            ("display.panel", Value::Bool(on)) => knobs::PANEL.set(on)?,
            ("misc.record", Value::Bool(on)) => {
                knobs::RECORD.set(on)?;
                return Ok(Some(
                    if on {
                        "Recording every frame."
                    } else {
                        "Recording stopped."
                    }
                    .into(),
                ));
            }
            ("misc.fps_cap", Value::Index(i)) => knobs::FPS_CAP.set(cap_of(i))?,
            ("menu.scale", Value::Number(v)) => knobs::MENU_SCALE.set(v as f32),
            ("world.speed", Value::Number(v)) => {
                knobs::clock::set_rate(v, knobs::clock::host_now())
            }
            ("world.slow_motion", Value::Bool(on)) => {
                let rate = if on {
                    self.before_slow = knobs::clock::rate();
                    1.0 / 3.0
                } else {
                    self.before_slow
                };
                knobs::clock::set_rate(rate, knobs::clock::host_now());
            }
            ("misc.screenshot", _) => {
                let folder = knobs::screenshot_folder();
                let path = folder.join(format!("leonida-{}.png", stamp()));
                knobs::ask_screenshot(path.clone());
                return Ok(Some(format!("Saving {}", path.display())));
            }
            ("settings.about", _) => {
                return Ok(Some(format!(
                    "Leonida {} trainer menu. F4 opens and closes it; arrows or the number pad move, Enter or 5 selects, Backspace or 0 goes back. Single player only.",
                    env!("CARGO_PKG_VERSION")
                )));
            }
            (id, value) if id.starts_with("mod.") => return crate::mods::apply(id, value),
            (id, value) if !id.starts_with("go.") => return self.game.apply(id, value),
            _ => {}
        }
        Ok(None)
    }

    fn available(&self, id: Id) -> Result<(), String> {
        match id {
            id if id.starts_with("go.") => Ok(()),
            "menu.scale" | "settings.about" => Ok(()),
            id if id.starts_with("mod.") => Ok(()),
            "display.sharpen" if !knobs::SHARPNESS.there() => Err(NO_SHARPNESS.into()),
            "misc.fps_cap" if !knobs::FPS_CAP.there() => Err(NO_CAP.into()),
            "misc.screenshot" if !knobs::screenshots_there() => Err(NO_SHOT.into()),
            "display.panel" if !knobs::PANEL.there() => Err(NO_PANEL.into()),
            "misc.record" if !knobs::RECORD.there() => Err(NO_RECORD.into()),
            "display.sharpen" | "misc.fps_cap" | "misc.screenshot" | "display.panel"
            | "misc.record" => Ok(()),
            "world.speed" | "world.slow_motion"
                if !knobs::clock::HOOKED.load(Ordering::Relaxed) =>
            {
                Err(NO_CLOCK.into())
            }
            "world.speed" | "world.slow_motion" => Ok(()),
            id => self.game.available(id),
        }
    }
}

/// The local time as `YYYYMMDD-HHMMSS-mmm`, from the system clock (UTC: the
/// standard library has no time zones; good enough to order files).
fn stamp() -> String {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default();
    let secs = now.as_secs();
    let (days, rest) = (secs / 86_400, secs % 86_400);
    // Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
    let z = days as i64 + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + i64::from(m <= 2);
    format!(
        "{y:04}{m:02}{d:02}-{:02}{:02}{:02}-{:03}",
        rest / 3600,
        rest % 3600 / 60,
        rest % 60,
        now.subsec_millis()
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::actions::NotYet;
    use crate::model::{Input, Menu, Right};

    #[test]
    fn every_submenu_points_at_a_page_and_ids_are_unique() {
        let pages = pages();
        let mut ids = std::collections::HashSet::new();
        for p in &pages {
            assert!(!p.title.is_empty() && !p.items.is_empty(), "{}", p.title);
            for item in &p.items {
                assert!(ids.insert(item.id), "{} twice", item.id);
                if let Kind::Submenu(to) = item.kind {
                    assert!(to < pages.len() && to != 0);
                }
            }
        }
        let main: Vec<_> = pages[MAIN].items.iter().map(|i| i.label).collect();
        assert_eq!(
            main,
            [
                "Player", "Vehicle", "Weapons", "World", "Time", "Weather", "Teleport", "Misc",
                "Settings"
            ]
        );
    }

    #[test]
    fn the_mods_page_is_last_and_reachable_from_the_main_menu() {
        let _one = knobs::TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        crate::mods::set_pages(vec![crate::mods::ModPage {
            title: "Mine".into(),
            items: vec![crate::mods::ModItem {
                label: "Go".into(),
                help: String::new(),
                kind: crate::mods::ModKind::Action,
            }],
        }]);
        assert_eq!(with_mods(pages(), false).len(), MODS);
        let all = with_mods(pages(), true);
        assert_eq!(all.len(), MODS + 2);
        assert_eq!(all[MAIN].items.last().unwrap().kind, Kind::Submenu(MODS));
        assert_eq!(all[MODS].title, "Mods");
        assert_eq!(all[MODS + 1].title, "Mine");
        // Through the menu: Mods > Mine > Go reaches the mods' queue.
        let mut b = Leonida::new(Box::new(NotYet));
        let mut m = Menu::new(all, 11);
        m.handle(Input::Toggle, &mut b);
        m.handle(Input::Up, &mut b); // Mods, the last item
        m.handle(Input::Select, &mut b);
        assert_eq!(m.view(&b).caption, "MODS");
        m.handle(Input::Down, &mut b);
        m.handle(Input::Select, &mut b);
        assert_eq!(m.view(&b).caption, "MINE");
        crate::mods::take_events();
        m.handle(Input::Select, &mut b);
        let events = crate::mods::take_events();
        assert_eq!(events.len(), 1);
        assert_eq!((events[0].page, events[0].item), (0, 0));
        crate::mods::set_pages(vec![]);
    }

    #[test]
    fn script_features_say_why_they_are_not_there() {
        let mut b = Leonida::new(Box::new(NotYet));
        let mut m = Menu::new(pages(), 11);
        m.handle(Input::Toggle, &mut b);
        m.handle(Input::Select, &mut b); // Player
        let v = m.view(&b);
        assert_eq!(v.caption, "PLAYER");
        assert!(!v.rows[0].enabled);
        assert!(v.footer_warns && v.footer.contains("natives"));
        m.handle(Input::Select, &mut b);
        assert!(m.view(&b).footer.starts_with("Not available yet"));
    }

    /// Stand-ins for the renderer's settings: the owned settings are
    /// provided once a process, so every test that needs them asks here.
    mod owner {
        use super::*;
        use std::sync::Mutex;

        pub static SHARPNESS: knobs::F32 = knobs::F32::new(0.0);
        pub static CAP: Mutex<Cap> = Mutex::new(Cap::Title);

        pub fn provide() {
            knobs::SHARPNESS.provide(
                || SHARPNESS.get(),
                |v| {
                    SHARPNESS.set(v);
                    Ok(())
                },
            );
            knobs::FPS_CAP.provide(
                || *CAP.lock().unwrap_or_else(|e| e.into_inner()),
                |cap| {
                    *CAP.lock().unwrap_or_else(|e| e.into_inner()) = cap;
                    Ok(())
                },
            );
        }
    }

    #[test]
    fn runtime_settings_change_for_real() {
        let _one = knobs::TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        owner::provide();
        let mut b = Leonida::new(Box::new(NotYet));
        let mut m = Menu::new(pages(), 11);
        m.handle(Input::Toggle, &mut b);
        // Settings > Display > Sharpen.
        for _ in 0..8 {
            m.handle(Input::Down, &mut b);
        }
        m.handle(Input::Select, &mut b);
        m.handle(Input::Select, &mut b);
        owner::SHARPNESS.set(0.4);
        m.handle(Input::Right, &mut b);
        assert!((owner::SHARPNESS.get() - 0.45).abs() < 1e-6);
        m.handle(Input::Left, &mut b);
        m.handle(Input::Left, &mut b);
        assert!((owner::SHARPNESS.get() - 0.35).abs() < 1e-6);
        assert_eq!(m.view(&b).rows[0].right, Right::Value("0.35".into()));
        // The panel is not there in a test: dimmed.
        assert!(!m.view(&b).rows[1].enabled);
    }

    #[test]
    fn the_frame_rate_cap_is_the_renderers() {
        let _one = knobs::TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        owner::provide();
        let cap = || *owner::CAP.lock().unwrap_or_else(|e| e.into_inner());
        let mut b = Leonida::new(Box::new(NotYet));
        assert!(b.available("misc.fps_cap").is_ok());
        b.set("misc.fps_cap", Value::Index(3)).unwrap();
        assert_eq!(cap(), Cap::Fps(60));
        assert_eq!(b.value("misc.fps_cap"), Value::Index(3));
        b.set("misc.fps_cap", Value::Index(CAPS.len() - 1)).unwrap();
        assert_eq!(cap(), Cap::Unlimited);
        assert_eq!(b.value("misc.fps_cap"), Value::Index(CAPS.len() - 1));
        b.set("misc.fps_cap", Value::Index(0)).unwrap();
        assert_eq!(cap(), Cap::Title);
        // A rate set elsewhere that is not offered shows as the nearest.
        *owner::CAP.lock().unwrap_or_else(|e| e.into_inner()) = Cap::Fps(80);
        assert_eq!(b.value("misc.fps_cap"), Value::Index(4));
        *owner::CAP.lock().unwrap_or_else(|e| e.into_inner()) = Cap::Title;
    }

    #[test]
    fn caps_map_both_ways() {
        for (i, name) in CAPS.iter().enumerate() {
            assert_eq!(cap_index(cap_of(i)), i, "{name}");
        }
        assert_eq!(cap_of(0), Cap::Title);
        assert_eq!(cap_of(1), Cap::Fps(30));
        assert_eq!(cap_of(7), Cap::Unlimited);
        assert_eq!(cap_index(Cap::Fps(1000)), 6);
    }

    #[test]
    fn what_no_one_provides_says_why() {
        let _one = knobs::TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        // Nothing provides the panel, the recorder or screenshots in a test.
        let mut b = Leonida::new(Box::new(NotYet));
        for id in ["display.panel", "misc.record", "misc.screenshot"] {
            assert!(b.available(id).is_err(), "{id}");
        }
        let mut m = Menu::new(pages(), 11);
        m.handle(Input::Toggle, &mut b);
        for _ in 0..7 {
            m.handle(Input::Down, &mut b);
        }
        m.handle(Input::Select, &mut b); // Misc
        assert_eq!(m.view(&b).caption, "MISC");
        m.handle(Input::Select, &mut b); // Save screenshot
        assert!(m.view(&b).footer_warns);
        assert_eq!(knobs::work() & knobs::SHOT, 0, "nothing was asked for");
    }

    #[test]
    fn stamps_look_like_dates() {
        let s = stamp();
        assert_eq!(s.len(), 19);
        assert!(s.starts_with("20"));
    }
}
