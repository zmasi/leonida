//! The `leonida` table a mod sees, and the environment around it. Every
//! function here is a closure over one mod (its index, its folder), so what a
//! mod does is its own: its handlers, its drawing, its pages, its held keys.

use std::cell::RefCell;
use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::time::Instant;

use leonida_menu::model::Value as MenuValue;
use leonida_menu::mods::{MAX_ITEMS, MAX_PAGES, ModKind};
use leonida_menu::overlay::{Cmd, DEFAULT_SIZE, MAX_COMMANDS};
use mlua::prelude::LuaChunkMode as ChunkMode;
use mlua::{Function, IntoLua, Lua, LuaString, MultiValue, Result, Table, Value, Variadic};

use crate::input::BUTTONS;
use crate::keys;
use crate::manifest::safe_relative;
use crate::memory::{self, Pattern};
use crate::platform::Platform;
use crate::runtime::{Event, Handler, ItemDef, MAX_HANDLERS, PageDef, Shared};
use crate::sandbox::display;
use crate::sandbox::{self, Guard};
use crate::settings::{self, Setting};

/// The API's version: it changes only when something a mod may rely on
/// does. `leonida.api_version`.
pub const API_VERSION: i64 = 1;
/// The most a mod's file read or write moves.
const MAX_READ: u64 = 4 << 20;
const MAX_WRITE: usize = 1 << 20;

type Rc_<T> = Rc<RefCell<T>>;

fn err(message: impl std::fmt::Display) -> mlua::Error {
    mlua::Error::runtime(message)
}

/// The environment of mod `idx`: the sandbox's, plus `leonida` and `require`.
pub fn build_env(
    lua: &Lua,
    guard: &Guard,
    shared: &Rc_<Shared>,
    idx: usize,
    folder: &Path,
) -> Result<Table> {
    let log_to = shared.clone();
    let env = sandbox::mod_env(lua, guard, move |line| log_to.borrow().log(idx, line))?.table;
    env.set("leonida", leonida(lua, shared, idx, folder)?)?;
    env.set("require", require(lua, &env, folder)?)?;
    Ok(env)
}

/// `require(name)`: a Lua file of the mod's own folder (`name.lua` or
/// `name/init.lua`, dots as folders), run once in the mod's environment. There
/// is no `package` and no C module.
fn require(lua: &Lua, env: &Table, folder: &Path) -> Result<Function> {
    let env = env.clone();
    let folder = folder.to_path_buf();
    let loaded = lua.create_table()?;
    let busy: Rc_<HashSet<String>> = Rc::default();
    lua.create_function(move |lua, name: String| {
        if name.is_empty()
            || name.len() > 100
            || !name
                .chars()
                .all(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '-' | '.'))
            || name.contains("..")
            || name.starts_with('.')
            || name.ends_with('.')
        {
            return Err(err(format!("bad module name `{name}`")));
        }
        if let Value::Nil = loaded.raw_get::<Value>(name.as_str())? {
        } else {
            return loaded.raw_get::<Value>(name.as_str());
        }
        if !busy.borrow_mut().insert(name.clone()) {
            return Err(err(format!("loop while requiring `{name}`")));
        }
        let path = name.replace('.', "/");
        let found = [format!("{path}.lua"), format!("{path}/init.lua")]
            .into_iter()
            .filter_map(|rel| safe_relative(&rel).ok())
            .map(|rel| folder.join(rel))
            .find(|p| p.is_file());
        let result = (|| {
            let file = found
                .ok_or_else(|| err(format!("module `{name}` not found in the mod's folder")))?;
            let size = std::fs::metadata(&file).map_err(err)?.len();
            if size > crate::manifest::MAX_SCRIPT {
                return Err(err(format!("module `{name}` is too large")));
            }
            let code = std::fs::read(&file).map_err(err)?;
            let value: Value = lua
                .load(code)
                .set_name(format!("={name}.lua"))
                .set_mode(ChunkMode::Text)
                .set_environment(env.clone())
                .call(name.as_str())?;
            let value = if let Value::Nil = value {
                Value::Boolean(true)
            } else {
                value
            };
            loaded.raw_set(name.as_str(), value.clone())?;
            Ok(value)
        })();
        busy.borrow_mut().remove(&name);
        result
    })
}

/// A colour from a table (`{255, 128, 0}`, `{r=, g=, b=, a=}`), a string
/// (`"#RRGGBB"`, `"#RRGGBBAA"`) or an integer (`0xRRGGBB`, or `0xRRGGBBAA`
/// when it is above 24 bits).
fn colour(value: &Value, default: [u8; 4]) -> Result<[u8; 4]> {
    let byte = |v: Value| -> Result<u8> {
        match v {
            Value::Integer(i) => Ok(i.clamp(0, 255) as u8),
            Value::Number(n) if n.is_finite() => Ok(n.clamp(0.0, 255.0) as u8),
            _ => Err(err("a colour channel is a number from 0 to 255")),
        }
    };
    match value {
        Value::Nil => Ok(default),
        Value::Integer(i) => {
            let i =
                u32::try_from(*i).map_err(|_| err("a colour integer is 0xRRGGBB or 0xRRGGBBAA"))?;
            Ok(if i <= 0xFF_FFFF {
                [(i >> 16) as u8, (i >> 8) as u8, i as u8, 255]
            } else {
                [(i >> 24) as u8, (i >> 16) as u8, (i >> 8) as u8, i as u8]
            })
        }
        Value::String(s) => {
            let s = s.to_str()?;
            let hex = s.strip_prefix('#').unwrap_or(&s);
            let digit = |i: usize| u8::from_str_radix(hex.get(i..i + 2)?, 16).ok();
            match hex.len() {
                6 | 8 if hex.is_ascii() => Ok([
                    digit(0).ok_or_else(|| err("bad colour string"))?,
                    digit(2).ok_or_else(|| err("bad colour string"))?,
                    digit(4).ok_or_else(|| err("bad colour string"))?,
                    if hex.len() == 8 {
                        digit(6).ok_or_else(|| err("bad colour string"))?
                    } else {
                        255
                    },
                ]),
                _ => Err(err("a colour string is \"#RRGGBB\" or \"#RRGGBBAA\"")),
            }
        }
        Value::Table(t) => {
            let channel = |named: &str, at: i64, default: Option<u8>| -> Result<u8> {
                let v: Value = match t.get::<Value>(named)? {
                    Value::Nil => t.get::<Value>(at)?,
                    v => v,
                };
                match (v, default) {
                    (Value::Nil, Some(d)) => Ok(d),
                    (Value::Nil, None) => {
                        Err(err("a colour table has r, g and b (or three numbers)"))
                    }
                    (v, _) => byte(v),
                }
            };
            Ok([
                channel("r", 1, None)?,
                channel("g", 2, None)?,
                channel("b", 3, None)?,
                channel("a", 4, Some(255))?,
            ])
        }
        _ => Err(err(
            "a colour is a table, a \"#RRGGBB\" string or an integer",
        )),
    }
}

/// One value: what a call that worked returns (no trailing nil).
fn one(lua: &Lua, value: impl IntoLua) -> Result<MultiValue> {
    Ok(MultiValue::from_iter([value.into_lua(lua)?]))
}

/// What a call that did not work returns: `first` (nil or false) and why.
fn refused(lua: &Lua, first: Value, why: impl Into<String>) -> Result<MultiValue> {
    Ok(MultiValue::from_iter([first, why.into().into_lua(lua)?]))
}

fn setting_to_lua(lua: &Lua, s: Setting) -> Result<Value> {
    match s {
        Setting::Bool(b) => Ok(Value::Boolean(b)),
        Setting::Number(n) => n.into_lua(lua),
        Setting::Text(t) => t.into_lua(lua),
    }
}

fn leonida(lua: &Lua, shared: &Rc_<Shared>, idx: usize, folder: &Path) -> Result<Table> {
    let t = lua.create_table()?;
    t.set("api_version", API_VERSION)?;
    t.set("version", env!("CARGO_PKG_VERSION"))?;
    let platform: Rc<dyn Platform> = shared.borrow().platform.clone();

    // Events.
    let s = shared.clone();
    t.set(
        "on",
        lua.create_function(move |_, (event, function): (String, Function)| {
            let Some(parsed) = Event::parse(&event) else {
                return Err(err(format!(
                    "unknown event `{event}` (frame, key, menu_open)"
                )));
            };
            let mut s = s.borrow_mut();
            if s.handlers.iter().filter(|h| h.mod_idx == idx).count() >= MAX_HANDLERS {
                return Err(err("too many handlers"));
            }
            s.handlers.push(Handler {
                mod_idx: idx,
                event: parsed,
                function,
            });
            Ok(())
        })?,
    )?;

    // Input.
    let p = platform.clone();
    t.set(
        "key_down",
        lua.create_function(move |_, name: String| {
            let vk = keys::code(&name).ok_or_else(|| err(format!("unknown key `{name}`")))?;
            Ok(p.key_down(vk))
        })?,
    )?;
    let p = platform.clone();
    t.set(
        "pad",
        lua.create_function(move |lua, ()| {
            let pad = p.pad();
            let t = lua.create_table()?;
            t.set("connected", pad.connected)?;
            for (name, bit) in BUTTONS {
                t.set(name, pad.buttons & bit != 0)?;
            }
            for (name, v) in [
                ("lt", pad.lt),
                ("rt", pad.rt),
                ("lx", pad.lx),
                ("ly", pad.ly),
                ("rx", pad.rx),
                ("ry", pad.ry),
            ] {
                t.set(name, f64::from(v))?;
            }
            Ok(t)
        })?,
    )?;
    let s = shared.clone();
    t.set(
        "hold_keys",
        lua.create_function(move |_, list: Table| {
            let mut codes = Vec::new();
            for name in list.sequence_values::<String>() {
                let name = name?;
                codes.push(keys::code(&name).ok_or_else(|| err(format!("unknown key `{name}`")))?);
            }
            codes.sort_unstable();
            codes.dedup();
            let mut s = s.borrow_mut();
            if let Some(held) = s.held.get_mut(idx)
                && *held != codes
            {
                *held = codes;
                s.held_dirty = true;
            }
            Ok(())
        })?,
    )?;
    let s = shared.clone();
    t.set(
        "release_keys",
        lua.create_function(move |_, ()| {
            let mut s = s.borrow_mut();
            if let Some(held) = s.held.get_mut(idx)
                && !held.is_empty()
            {
                held.clear();
                s.held_dirty = true;
            }
            Ok(())
        })?,
    )?;

    // Overlay.
    let s = shared.clone();
    t.set(
        "text",
        lua.create_function(
            move |_, (x, y, text, opts): (f64, f64, LuaString, Option<Table>)| {
                let (mut size, mut colour_v) = (DEFAULT_SIZE as f64, Value::Nil);
                if let Some(o) = opts {
                    if let Some(v) = o.get::<Option<f64>>("size")? {
                        size = v;
                    }
                    colour_v = o.get::<Value>("color")?;
                    if let Value::Nil = colour_v {
                        colour_v = o.get::<Value>("colour")?;
                    }
                }
                let colour = colour(&colour_v, [255, 255, 255, 255])?;
                let mut s = s.borrow_mut();
                if s.draw.len() < MAX_COMMANDS {
                    s.draw.push(Cmd::Text {
                        x: x as f32,
                        y: y as f32,
                        text: text.to_string_lossy().chars().take(300).collect(),
                        size: size as f32,
                        colour,
                    });
                }
                Ok(())
            },
        )?,
    )?;
    let s = shared.clone();
    t.set(
        "rect",
        lua.create_function(move |_, (x, y, w, h, color): (f64, f64, f64, f64, Value)| {
            let colour = colour(&color, [0, 0, 0, 160])?;
            let mut s = s.borrow_mut();
            if s.draw.len() < MAX_COMMANDS {
                s.draw.push(Cmd::Rect {
                    x: x as f32,
                    y: y as f32,
                    w: w as f32,
                    h: h as f32,
                    colour,
                });
            }
            Ok(())
        })?,
    )?;

    // Settings.
    t.set(
        "get",
        lua.create_function(|lua, name: String| match settings::get(&name) {
            Some(s) => setting_to_lua(lua, s),
            None => Ok(Value::Nil),
        })?,
    )?;
    t.set(
        "set",
        lua.create_function(|lua, (name, value): (String, Value)| {
            let setting = match &value {
                Value::Boolean(b) => Setting::Bool(*b),
                Value::Integer(i) => Setting::Number(*i as f64),
                Value::Number(n) => Setting::Number(*n),
                Value::String(s) => match s.to_str()?.as_ref() {
                    "game" => Setting::Text("game"),
                    "title" => Setting::Text("title"),
                    other => {
                        return refused(
                            lua,
                            Value::Boolean(false),
                            format!("`{other}` is not a value of a setting"),
                        );
                    }
                },
                _ => {
                    return refused(
                        lua,
                        Value::Boolean(false),
                        "a setting takes a boolean, a number or \"game\"",
                    );
                }
            };
            match settings::set(&name, &setting) {
                Ok(()) => one(lua, true),
                Err(why) => refused(lua, Value::Boolean(false), why),
            }
        })?,
    )?;
    t.set("settings", lua.create_sequence_from(settings::NAMES)?)?;

    // The trainer menu.
    t.set("menu", menu(lua, shared, idx)?)?;

    // Memory.
    t.set("memory", memory_table(lua, shared, &platform)?)?;

    // Misc.
    let s = shared.clone();
    t.set(
        "log",
        lua.create_function(move |_, args: Variadic<Value>| {
            let line: Vec<String> = args.iter().map(display).collect();
            s.borrow().log(idx, &line.join(" "));
            Ok(())
        })?,
    )?;
    let p = platform.clone();
    t.set(
        "screenshot",
        lua.create_function(move |lua, ()| match p.screenshot() {
            Ok(path) => one(lua, path),
            Err(why) => refused(lua, Value::Nil, why),
        })?,
    )?;
    // The player, by memory (`apps/leonida/src/player.rs`): not the title's own scripts.
    t.set(
        "teleport",
        lua.create_function(
            move |lua, (x, y, z, hold): (f64, f64, f64, Option<f64>)| {
                let Some(hook) = leonida_menu::knobs::player() else {
                    return refused(lua, Value::Nil, leonida_menu::knobs::NO_PLAYER);
                };
                if ![x, y, z].iter().all(|v| v.is_finite() && v.abs() < 1.0e6) {
                    return refused(lua, Value::Nil, "teleport(x, y, z): finite numbers expected");
                }
                let hold_ms = (hold.unwrap_or(3.0).clamp(0.0, 30.0) * 1000.0) as u32;
                let result = (hook.ready)().and_then(|()| (hook.teleport)([x as f32, y as f32, z as f32], hold_ms));
                match result {
                    Ok(line) => one(lua, line),
                    Err(why) => refused(lua, Value::Nil, why),
                }
            },
        )?,
    )?;
    t.set(
        "player_position",
        lua.create_function(move |lua, ()| {
            let Some(hook) = leonida_menu::knobs::player() else {
                return refused(lua, Value::Nil, leonida_menu::knobs::NO_PLAYER);
            };
            match (hook.ready)().and_then(|()| (hook.position)()) {
                Ok([x, y, z]) => Ok(MultiValue::from_iter([
                    Value::Number(f64::from(x)),
                    Value::Number(f64::from(y)),
                    Value::Number(f64::from(z)),
                ])),
                Err(why) => refused(lua, Value::Nil, why),
            }
        })?,
    )?;
    let s = shared.clone();
    t.set(
        "frame",
        lua.create_function(move |_, ()| Ok(s.borrow().frame as i64))?,
    )?;
    let start: Instant = shared.borrow().started;
    t.set(
        "time",
        lua.create_function(move |_, ()| Ok(start.elapsed().as_secs_f64()))?,
    )?;

    // Files, in the mod's own folder.
    let root = folder.to_path_buf();
    t.set(
        "read_file",
        lua.create_function(move |lua, name: String| match read_file(&root, &name) {
            Ok(bytes) => one(lua, lua.create_string(bytes)?),
            Err(why) => refused(lua, Value::Nil, why),
        })?,
    )?;
    let root = folder.to_path_buf();
    t.set(
        "write_file",
        lua.create_function(move |lua, (name, text): (String, LuaString)| {
            match write_file(&root, &name, &text.as_bytes()) {
                Ok(()) => one(lua, true),
                Err(why) => refused(lua, Value::Boolean(false), why),
            }
        })?,
    )?;
    Ok(t)
}

/// `root` joined with a plain relative `name`, checked not to leave it.
fn inside(root: &Path, name: &str) -> std::result::Result<PathBuf, String> {
    let rel = safe_relative(name)?;
    let path = root.join(rel);
    // A link or junction inside the folder must not lead out of it.
    let canon_root = std::fs::canonicalize(root).map_err(|e| format!("the mod's folder: {e}"))?;
    let mut probe = path.as_path();
    while !probe.exists() {
        probe = probe.parent().ok_or("no such folder")?;
    }
    let canon = std::fs::canonicalize(probe).map_err(|e| e.to_string())?;
    if !canon.starts_with(&canon_root) {
        return Err(format!("`{name}` leads out of the mod's folder"));
    }
    Ok(path)
}

fn read_file(root: &Path, name: &str) -> std::result::Result<Vec<u8>, String> {
    let path = inside(root, name)?;
    let meta = std::fs::metadata(&path).map_err(|e| format!("{name}: {e}"))?;
    if !meta.is_file() || meta.len() > MAX_READ {
        return Err(format!("{name} is not a file of at most {MAX_READ} bytes"));
    }
    std::fs::read(&path).map_err(|e| format!("{name}: {e}"))
}

fn write_file(root: &Path, name: &str, data: &[u8]) -> std::result::Result<(), String> {
    if data.len() > MAX_WRITE {
        return Err(format!("a write is at most {MAX_WRITE} bytes"));
    }
    let lower = name.to_ascii_lowercase();
    let extension = lower.rsplit_once('.').map_or("", |(_, e)| e);
    if lower == "mod.json"
        || [
            "exe", "dll", "bat", "cmd", "com", "ps1", "vbs", "msi", "scr", "lnk", "sys", "reg",
            "hta", "jar",
        ]
        .contains(&extension)
    {
        return Err(format!("`{name}` cannot be written by a mod"));
    }
    let path = inside(root, name)?;
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| format!("{name}: {e}"))?;
    }
    // Again, now that the folders exist.
    let path = inside(root, name)?;
    std::fs::write(&path, data).map_err(|e| format!("{name}: {e}"))
}

/// Digits after the point `step` has, up to three: the menu's number display.
fn decimals_of(step: f64) -> u8 {
    (0..=3u8)
        .find(|&d| {
            let scaled = step * 10f64.powi(i32::from(d));
            (scaled - scaled.round()).abs() < 1e-9
        })
        .unwrap_or(3)
}

fn menu(lua: &Lua, shared: &Rc_<Shared>, idx: usize) -> Result<Table> {
    let menu = lua.create_table()?;
    let s = shared.clone();
    menu.set(
        "page",
        lua.create_function(move |lua, title: String| {
            let uid = {
                let mut s = s.borrow_mut();
                if s.pages.len() >= MAX_PAGES {
                    return Err(err("too many menu pages"));
                }
                let uid = s.next_page;
                s.next_page += 1;
                let title = if title.trim().is_empty() {
                    "Mod".to_owned()
                } else {
                    title
                };
                s.pages.push(PageDef {
                    uid,
                    mod_idx: idx,
                    title,
                    items: Vec::new(),
                });
                s.pages_dirty = true;
                uid
            };
            let page = lua.create_table()?;
            let owner = s.clone();
            let add = move |kind: ModKind,
                            label: String,
                            help: Option<String>,
                            callback: Option<Function>|
                  -> Result<usize> {
                let mut s = owner.borrow_mut();
                let page = s
                    .pages
                    .iter_mut()
                    .find(|p| p.uid == uid)
                    .ok_or_else(|| err("this page is gone"))?;
                if page.items.len() >= MAX_ITEMS {
                    return Err(err("too many items on a menu page"));
                }
                page.items.push(ItemDef {
                    label,
                    help: help.unwrap_or_default(),
                    kind,
                    callback,
                });
                let at = page.items.len() - 1;
                s.pages_dirty = true;
                Ok(at)
            };
            // page:toggle(label, initial, callback [, help])
            let (a, make) = (add.clone(), item_handle(lua, &s, uid));
            page.set(
                "toggle",
                lua.create_function(
                    move |lua,
                          (_this, label, initial, callback, help): (
                        Value,
                        String,
                        Option<bool>,
                        Option<Function>,
                        Option<String>,
                    )| {
                        let at = a(
                            ModKind::Toggle(initial.unwrap_or(false)),
                            label,
                            help,
                            callback,
                        )?;
                        make(lua, at)
                    },
                )?,
            )?;
            // page:number(label, initial, {min=, max=, step=, decimals=, suffix=, help=}, callback)
            let (a, make) = (add.clone(), item_handle(lua, &s, uid));
            page.set(
                "number",
                lua.create_function(
                    move |lua,
                          (_this, label, initial, opts, callback): (
                        Value,
                        String,
                        f64,
                        Option<Table>,
                        Option<Function>,
                    )| {
                        let get = |k: &str| -> Result<Option<f64>> {
                            opts.as_ref().map_or(Ok(None), |o| o.get::<Option<f64>>(k))
                        };
                        let (min, max, step) = (
                            get("min")?.unwrap_or(0.0),
                            get("max")?.unwrap_or(100.0),
                            get("step")?.unwrap_or(1.0),
                        );
                        if !(min.is_finite()
                            && max.is_finite()
                            && step.is_finite()
                            && min < max
                            && step > 0.0)
                        {
                            return Err(err("a number needs finite min < max and a step above 0"));
                        }
                        let decimals = get("decimals")?
                            .map_or_else(|| decimals_of(step), |d| d.clamp(0.0, 6.0) as u8);
                        let suffix = match &opts {
                            Some(o) => o.get::<Option<String>>("suffix")?.unwrap_or_default(),
                            None => String::new(),
                        };
                        let help = match &opts {
                            Some(o) => o.get::<Option<String>>("help")?,
                            None => None,
                        };
                        let value = if initial.is_finite() {
                            initial.clamp(min, max)
                        } else {
                            min
                        };
                        let at = a(
                            ModKind::Number {
                                value,
                                min,
                                max,
                                step,
                                decimals,
                                suffix,
                            },
                            label,
                            help,
                            callback,
                        )?;
                        make(lua, at)
                    },
                )?,
            )?;
            // page:action(label, callback [, help])
            let (a, make) = (add, item_handle(lua, &s, uid));
            page.set(
                "action",
                lua.create_function(
                    move |lua,
                          (_this, label, callback, help): (
                        Value,
                        String,
                        Option<Function>,
                        Option<String>,
                    )| {
                        let at = a(ModKind::Action, label, help, callback)?;
                        make(lua, at)
                    },
                )?,
            )?;
            Ok(page)
        })?,
    )?;
    Ok(menu)
}

/// Makes the handle `page:toggle(...)` and the others return: `:get()` and
/// `:set(value)` for the item at `at` of the page `uid`.
fn item_handle(
    _lua: &Lua,
    shared: &Rc_<Shared>,
    uid: u64,
) -> impl Fn(&Lua, usize) -> Result<Table> + Clone + use<> {
    let shared = shared.clone();
    move |lua, at| {
        let handle = lua.create_table()?;
        let s = shared.clone();
        handle.set(
            "get",
            lua.create_function(move |_, _this: Value| {
                let s = s.borrow();
                Ok(s.pages
                    .iter()
                    .find(|p| p.uid == uid)
                    .and_then(|p| p.items.get(at))
                    .map(|i| match &i.kind {
                        ModKind::Toggle(on) => Value::Boolean(*on),
                        ModKind::Number { value, .. } => Value::Number(*value),
                        ModKind::Action => Value::Nil,
                    })
                    .unwrap_or(Value::Nil))
            })?,
        )?;
        let s = shared.clone();
        handle.set(
            "set",
            lua.create_function(move |_, (_this, value): (Value, Value)| {
                let mut s = s.borrow_mut();
                let Some(position) = s.pages.iter().position(|p| p.uid == uid) else {
                    return Ok(false);
                };
                let Some(item) = s.pages[position].items.get_mut(at) else {
                    return Ok(false);
                };
                let shown = match (&mut item.kind, &value) {
                    (ModKind::Toggle(on), Value::Boolean(v)) => {
                        *on = *v;
                        MenuValue::Bool(*v)
                    }
                    (
                        ModKind::Number {
                            value: current,
                            min,
                            max,
                            ..
                        },
                        Value::Integer(_) | Value::Number(_),
                    ) => {
                        let v = match &value {
                            Value::Integer(i) => *i as f64,
                            Value::Number(n) => *n,
                            _ => return Ok(false),
                        };
                        if !v.is_finite() {
                            return Ok(false);
                        }
                        let v = v.clamp(*min, *max);
                        *current = v;
                        MenuValue::Number(v)
                    }
                    _ => return Ok(false),
                };
                let platform = s.platform.clone();
                drop(s);
                platform.set_menu_value(position, at, shown);
                Ok(true)
            })?,
        )?;
        Ok(handle)
    }
}

/// `leonida.memory`: checked reads and writes of the title's memory, and a scan.
fn memory_table(lua: &Lua, shared: &Rc_<Shared>, platform: &Rc<dyn Platform>) -> Result<Table> {
    let m = lua.create_table()?;
    fn read_vec(p: &dyn Platform, address: i64, n: usize) -> Option<Vec<u8>> {
        let at = memory::valid_range(address, n)?;
        let mut out = vec![0u8; n];
        p.read(at, &mut out).then_some(out)
    }
    fn write_all(p: &dyn Platform, address: i64, data: &[u8]) -> bool {
        memory::valid_range(address, data.len()).is_some_and(|at| p.write(at, data))
    }
    macro_rules! reader {
        ($name:literal, $n:literal, |$b:ident| $conv:expr) => {{
            let p = platform.clone();
            m.set(
                $name,
                lua.create_function(move |_, address: i64| {
                    Ok(read_vec(&*p, address, $n).map(|$b| $conv))
                })?,
            )?;
        }};
    }
    reader!("read_u8", 1, |b| i64::from(b[0]));
    reader!("read_u16", 2, |b| i64::from(u16::from_le_bytes([
        b[0], b[1]
    ])));
    reader!("read_u32", 4, |b| i64::from(u32::from_le_bytes([
        b[0], b[1], b[2], b[3]
    ])));
    reader!("read_i32", 4, |b| i64::from(i32::from_le_bytes([
        b[0], b[1], b[2], b[3]
    ])));
    reader!(
        "read_u64",
        8,
        |b| u64::from_le_bytes(b[..8].try_into().unwrap()) as i64
    );
    reader!("read_f32", 4, |b| f64::from(f32::from_le_bytes([
        b[0], b[1], b[2], b[3]
    ])));
    reader!("read_f64", 8, |b| f64::from_le_bytes(
        b[..8].try_into().unwrap()
    ));
    macro_rules! writer {
        ($name:literal, $arg:ty, |$v:ident| $conv:expr) => {{
            let p = platform.clone();
            m.set(
                $name,
                lua.create_function(move |_, (address, $v): (i64, $arg)| {
                    let bytes: Option<Vec<u8>> = $conv;
                    Ok(bytes.is_some_and(|b| write_all(&*p, address, &b)))
                })?,
            )?;
        }};
    }
    writer!("write_u8", i64, |v| u8::try_from(v).ok().map(|v| vec![v]));
    writer!("write_u16", i64, |v| u16::try_from(v)
        .ok()
        .map(|v| v.to_le_bytes().to_vec()));
    writer!("write_u32", i64, |v| u32::try_from(v)
        .ok()
        .or_else(|| i32::try_from(v).ok().map(|v| v as u32))
        .map(|v| v.to_le_bytes().to_vec()));
    writer!("write_i32", i64, |v| i32::try_from(v)
        .ok()
        .map(|v| v.to_le_bytes().to_vec()));
    writer!("write_u64", i64, |v| Some(v.to_le_bytes().to_vec()));
    writer!("write_f32", f64, |v| Some(
        (v as f32).to_le_bytes().to_vec()
    ));
    writer!("write_f64", f64, |v| Some(v.to_le_bytes().to_vec()));

    let p = platform.clone();
    m.set(
        "read_bytes",
        lua.create_function(move |lua, (address, n): (i64, Option<i64>)| {
            let n = usize::try_from(n.unwrap_or(16)).unwrap_or(0);
            Ok(match read_vec(&*p, address, n) {
                Some(bytes) => Some(lua.create_string(bytes)?),
                None => None,
            })
        })?,
    )?;
    let p = platform.clone();
    m.set(
        "write_bytes",
        lua.create_function(move |_, (address, data): (i64, LuaString)| {
            Ok(write_all(&*p, address, &data.as_bytes()))
        })?,
    )?;
    let p = platform.clone();
    m.set(
        "base",
        lua.create_function(move |_, ()| {
            Ok(match p.image() {
                Some((base, size)) => MultiValue::from_iter([
                    Value::Integer(base as i64),
                    Value::Integer(size as i64),
                ]),
                None => MultiValue::new(),
            })
        })?,
    )?;
    // scan(pattern [, {max=, start=, size=}]) -> list of addresses, or nil and why
    let (p, s) = (platform.clone(), shared.clone());
    m.set(
        "scan",
        lua.create_function(move |lua, (pattern, opts): (String, Option<Table>)| {
            let fail = |why: String| refused(lua, Value::Nil, why);
            let pattern = match Pattern::parse(&pattern) {
                Ok(p) => p,
                Err(why) => return fail(format!("bad pattern: {why}")),
            };
            let get = |k: &str| -> Result<Option<i64>> {
                opts.as_ref().map_or(Ok(None), |o| o.get::<Option<i64>>(k))
            };
            let Some((base, size)) = p.image() else {
                return fail("the title's image is not known".to_owned());
            };
            let start = get("start")?.map_or(base, |v| usize::try_from(v).unwrap_or(base));
            let len = get("size")?.map_or(size.saturating_sub(start.saturating_sub(base)), |v| {
                usize::try_from(v).unwrap_or(0)
            });
            let max = get("max")?
                .map_or(100, |v| usize::try_from(v).unwrap_or(0))
                .min(memory::MAX_MATCHES);
            if start < memory::LOWEST || start.saturating_add(len) > memory::HIGHEST {
                return fail("the range is not in the title's memory".to_owned());
            }
            let began = Instant::now();
            let found = memory::scan(&|at, out| p.read(at, out), start, len, &pattern, max);
            // A long scan is not the mod's own running time.
            let guard = s.borrow().guard.clone();
            guard.pause(began.elapsed());
            let list = lua.create_sequence_from(found.into_iter().map(|a| a as i64))?;
            one(lua, list)
        })?,
    )?;
    Ok(m)
}
