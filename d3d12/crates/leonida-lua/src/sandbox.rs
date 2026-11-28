//! The Lua state and what a mod can reach in it.
//!
//! One state holds all the mods; each gets an environment of its own made
//! from a short list of what is safe: the base functions, the `coroutine`,
//! `table`, `string`, `utf8` and `math` libraries, and `os.time`, `os.clock`,
//! `os.date` and `os.difftime`. There is no `io`, no `package`, no `debug`,
//! no `dofile` or `loadfile`, no `os.execute`, `os.remove`, `os.rename`,
//! `os.exit`, `os.getenv` or `io.popen`, no way to load a C module or a
//! precompiled chunk, and nothing that opens a socket. A mod reads and writes
//! files through `leonida.read_file` and `leonida.write_file`, in its own
//! folder.
//!
//! Each mod's library tables are copies, so one mod changing `string.rep`
//! changes nothing for another. A time budget and a memory limit stop a mod
//! that loops or allocates without end; `pcall` and the like cannot swallow
//! the budget's error.

use std::cell::Cell;
use std::rc::Rc;
use std::time::{Duration, Instant};

use mlua::prelude::LuaChunkMode as ChunkMode;
use mlua::{
    Function, HookTriggers, Lua, LuaOptions, MultiValue, Result, StdLib, Table, Value, VmState,
};

/// The Lua state's memory limit.
pub const MEMORY_LIMIT: usize = 128 << 20;

/// How often (in VM instructions) the budget is looked at.
const HOOK_EVERY: u32 = 4000;

/// The time a mod's code may run in one go, and what loading it may take.
#[derive(Clone, Copy, Debug)]
pub struct Budget {
    pub call: Duration,
    pub load: Duration,
}

impl Default for Budget {
    fn default() -> Budget {
        Budget {
            call: Duration::from_millis(200),
            load: Duration::from_secs(2),
        }
    }
}

/// Tags a chunk source with the owning mod's unforgeable id so every nested
/// prototype (including `_ENV`-less closures and `__gc` finalizers) resolves
/// string methods to its own mod at stack level 1 in `O(1)`.
pub fn chunk_name(id: usize, label: &str) -> String {
    let label = label.trim_start_matches(['=', '@']);
    format!("={id}:{label}")
}

fn mod_id_from_source(source: Option<&str>) -> Option<usize> {
    let rest = source?.strip_prefix('=')?;
    let (id, _) = rest.split_once(':')?;
    id.parse().ok()
}

/// The state's guard: the moment the running call must end by, whether it
/// went past it, and how strings find the calling chunk's `string` table.
#[derive(Clone)]
pub struct Guard {
    deadline: Rc<Cell<Instant>>,
    tripped: Rc<Cell<bool>>,
    next_mod: Rc<Cell<usize>>,
    mod_strings: Table,
    env_strings: Table,
}

impl Guard {
    /// Starts a call that may run for `limit`.
    pub fn start(&self, limit: Duration) {
        self.tripped.set(false);
        self.deadline.set(Instant::now() + limit);
    }

    /// Time spent in a native function (a scan) does not count against the call.
    pub fn pause(&self, spent: Duration) {
        self.deadline.set(self.deadline.get() + spent);
    }

    pub fn tripped(&self) -> bool {
        self.tripped.get()
    }
}

/// A fresh state with its guard.
pub fn new_lua() -> Result<(Lua, Guard)> {
    let lua = Lua::new_with(
        StdLib::COROUTINE
            | StdLib::TABLE
            | StdLib::STRING
            | StdLib::UTF8
            | StdLib::MATH
            | StdLib::OS,
        LuaOptions::default(),
    )?;
    lua.set_memory_limit(MEMORY_LIMIT)?;
    let base_string: Table = lua.globals().get("string")?;
    base_string.set("dump", Value::Nil)?;
    let fallback_string = copy_table(&lua, &base_string, None, &["dump"])?;
    let mod_strings = lua.create_table()?;
    let env_strings = lua.create_table()?;
    let weak_keys = lua.create_table()?;
    weak_keys.set("__mode", "k")?;
    let _ = env_strings.set_metatable(Some(weak_keys));
    let mods_for_mt = mod_strings.clone();
    let map_for_mt = env_strings.clone();
    let mt: Table = lua.load("return getmetatable('')").eval()?;
    mt.set(
        "__index",
        lua.create_function(move |lua, (_s, key): (Value, Value)| {
            // Inspect only stack level 1 (the immediate function indexing the
            // string); never walk into caller frames, which would let a
            // foreign mod's `_ENV`-less `__gc` finalizer borrow the caller's
            // `string` table.
            let id = lua
                .inspect_stack(1, |d| mod_id_from_source(d.source().source.as_deref()))
                .flatten();
            if let Some(id) = id
                && let Value::Table(own) = mods_for_mt.raw_get::<Value>(id)?
            {
                return own.get::<Value>(key);
            }
            if let Some(Some(env)) = lua.inspect_stack(1, |d| d.function().environment())
                && let Value::Table(own) = map_for_mt.raw_get::<Value>(env)?
            {
                return own.get::<Value>(key);
            }
            fallback_string.get::<Value>(key)
        })?,
    )?;
    let guard = Guard {
        deadline: Rc::new(Cell::new(Instant::now() + Duration::from_secs(3600))),
        tripped: Rc::new(Cell::new(false)),
        next_mod: Rc::new(Cell::new(1)),
        mod_strings,
        env_strings,
    };
    let watch = guard.clone();
    lua.set_global_hook(
        HookTriggers::new().every_nth_instruction(HOOK_EVERY),
        move |_, _| {
            if Instant::now() > watch.deadline.get() {
                watch.tripped.set(true);
                return Err(mlua::Error::runtime(
                    "the mod used up its time budget (a loop that does not end?)",
                ));
            }
            Ok(VmState::Continue)
        },
    )?;
    Ok((lua, guard))
}

/// Base functions a mod keeps.
const BASE: [&str; 17] = [
    "assert",
    "error",
    "getmetatable",
    "ipairs",
    "next",
    "pairs",
    "pcall",
    "rawequal",
    "rawget",
    "rawlen",
    "rawset",
    "select",
    "setmetatable",
    "tonumber",
    "tostring",
    "type",
    "xpcall",
];
const LIBS: [&str; 5] = ["coroutine", "table", "string", "utf8", "math"];
const OS: [&str; 4] = ["time", "clock", "date", "difftime"];

fn copy_table(lua: &Lua, from: &Table, only: Option<&[&str]>, skip: &[&str]) -> Result<Table> {
    let to = lua.create_table()?;
    for pair in from.pairs::<Value, Value>() {
        let (k, v) = pair?;
        if let Value::String(name) = &k {
            let name = name.to_string_lossy();
            if skip.contains(&name.as_str()) || only.is_some_and(|o| !o.contains(&name.as_str())) {
                continue;
            }
        }
        to.set(k, v)?;
    }
    Ok(to)
}

/// A Lua value as text, for `print` and `leonida.log`.
pub fn display(value: &Value) -> String {
    match value {
        Value::String(s) => s.to_string_lossy(),
        Value::Nil => "nil".to_owned(),
        Value::Boolean(b) => b.to_string(),
        Value::Integer(i) => i.to_string(),
        Value::Number(n) if n.is_finite() && n.fract() == 0.0 && n.abs() < 1e15 => {
            format!("{n:.1}")
        }
        Value::Number(n) => n.to_string(),
        Value::Error(e) => e.to_string(),
        other => format!("<{}>", other.type_name()),
    }
}

/// What a mod's `load`, `require` and `print` need to know.
pub struct Env {
    pub id: usize,
    pub table: Table,
}

/// The environment of one mod: its own copies of what is safe, plus
/// `print`, `load` and `collectgarbage` made for it. The caller adds
/// `leonida` and `require`.
pub fn mod_env(lua: &Lua, guard: &Guard, print: impl Fn(&str) + 'static) -> Result<Env> {
    let mod_id = guard.next_mod.get();
    guard.next_mod.set(mod_id + 1);
    let globals = lua.globals();
    let env = copy_table(lua, &globals, Some(&BASE), &[])?;
    for lib in LIBS {
        let from: Table = globals.get(lib)?;
        let own = copy_table(lua, &from, None, &["dump"])?;
        env.set(lib, own)?;
    }
    let os: Table = globals.get("os")?;
    env.set("os", copy_table(lua, &os, Some(&OS), &[])?)?;
    env.set("_VERSION", "Lua 5.4")?;
    env.set("_G", env.clone())?;

    // An error from the budget is not for a mod to catch: pcall, xpcall and
    // coroutine.resume give it back to whoever called the mod. (Written in
    // Lua, so that a coroutine can still yield through them.)
    let watch = guard.clone();
    let check = lua.create_function(move |_, args: MultiValue| {
        if watch.tripped() {
            return Err(mlua::Error::runtime(
                "the mod used up its time budget (a loop that does not end?)",
            ));
        }
        Ok(args)
    })?;
    let watch_trip = guard.clone();
    let tripped = lua.create_function(move |_, ()| Ok(watch_trip.tripped()))?;
    let co: Table = env.get("coroutine")?;
    let (pcall, xpcall, resume) = lua
        .load(
            "local pcall, xpcall, resume, check, tripped = ...
             return function(f, ...) return check(pcall(f, ...)) end,
                    function(f, h, ...)
                        if type(h) ~= 'function' then
                            return check(xpcall(f, h, ...))
                        end
                        return check(xpcall(f, function(err)
                            if tripped() then return err end
                            return h(err)
                        end, ...))
                    end,
                    function(c, ...) return check(resume(c, ...)) end",
        )
        .set_name("=sandbox")
        .call::<(Function, Function, Function)>((
            env.get::<Function>("pcall")?,
            env.get::<Function>("xpcall")?,
            co.get::<Function>("resume")?,
            check,
            tripped,
        ))?;
    env.set("pcall", pcall)?;
    env.set("xpcall", xpcall)?;
    co.set("resume", resume)?;

    // The string metatable is shared by every string: a mod sees a copy that
    // points at its own `string` table, and string method syntax resolves
    // through `guard.mod_strings` (and `guard.env_strings` at level 1) to that
    // same table.
    let own_string: Table = env.get("string")?;
    guard.mod_strings.raw_set(mod_id, own_string.clone())?;
    guard.env_strings.raw_set(env.clone(), own_string.clone())?;
    let original: Function = env.get("getmetatable")?;
    env.set(
        "getmetatable",
        lua.create_function(move |lua, value: Value| {
            if matches!(value, Value::String(_)) {
                let t = lua.create_table()?;
                t.set("__index", own_string.clone())?;
                return Ok(Value::Table(t));
            }
            original.call::<Value>(value)
        })?,
    )?;

    env.set(
        "print",
        lua.create_function(move |_, args: MultiValue| {
            let line: Vec<String> = args.iter().map(display).collect();
            print(&line.join("\t"));
            Ok(())
        })?,
    )?;

    // Text chunks only: a precompiled chunk can take the interpreter down.
    let target = env.clone();
    env.set(
        "load",
        lua.create_function(
            move |lua,
                  (chunk, name, _mode, env): (
                Value,
                Option<String>,
                Option<String>,
                Option<Table>,
            )| {
                let source: Vec<u8> = match chunk {
                    Value::String(s) => s.as_bytes().to_vec(),
                    Value::Function(reader) => {
                        let mut all = Vec::new();
                        loop {
                            match reader.call::<Value>(())? {
                                Value::String(s) if !s.as_bytes().is_empty() => {
                                    all.extend_from_slice(&s.as_bytes());
                                    if all.len() > 1 << 20 {
                                        return Ok((
                                            Value::Nil,
                                            Some("chunk too large".to_owned()),
                                        ));
                                    }
                                }
                                _ => break,
                            }
                        }
                        all
                    }
                    _ => {
                        return Err(mlua::Error::runtime(
                            "bad argument #1 to 'load' (string or function expected)",
                        ));
                    }
                };
                let label = name.as_deref().unwrap_or("(load)");
                let chosen = env.unwrap_or_else(|| target.clone());
                let compiled = lua
                    .load(source)
                    .set_name(chunk_name(mod_id, label))
                    .set_mode(ChunkMode::Text)
                    .set_environment(chosen)
                    .into_function();
                Ok(match compiled {
                    Ok(f) => (Value::Function(f), None),
                    Err(e) => (Value::Nil, Some(e.to_string())),
                })
            },
        )?,
    )?;

    // Collecting and counting only.
    env.set(
        "collectgarbage",
        lua.create_function(|lua, opt: Option<String>| {
            match opt.as_deref().unwrap_or("collect") {
                "collect" => {
                    lua.gc_collect()?;
                    Ok(0.0)
                }
                "count" => Ok(lua.used_memory() as f64 / 1024.0),
                other => Err(mlua::Error::runtime(format!(
                    "collectgarbage(\"{other}\") is not available to mods"
                ))),
            }
        })?,
    )?;
    Ok(Env {
        id: mod_id,
        table: env,
    })
}
