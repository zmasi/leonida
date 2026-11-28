//! The trainer menu's game actions, done by the title's own code on the title's own thread
//! (`LEONIDA_GAME_ACTIONS=1`; `docs/mod-menu.md`, "Game actions").
//!
//! The title has a cheat system of its own, and the functions its cheats call take plain arguments: a ped and
//! a wanted level, an hour, a weather type. `leonida_loader::game_actions` finds them by the patterns of their
//! code and turns the title's once-a-frame call of its cheat input function into a call of [`frame_hook`]. The
//! menu (another thread) only queues a [`Request`]; the hook hands the title's deferred-call queue (the one its
//! own cheats go through) a function of ours, and that function does what was asked where the title would run
//! a cheat. Nothing of the title is called from a thread of ours.
//!
//! - `LEONIDA_GAME_ACTIONS=1` turns it on (off by default until it has been seen to work in the title).
//! - `LEONIDA_GAME_ACTIONS_RUN="<s>:<command>;..."` asks for commands that many seconds after the start, for a
//!   test without the keyboard: `info`, `cheat <n>`, `wanted <n>`, `time <h> [m [s]]`, `hours <+-n>`,
//!   `pause <0|1>`, `weather <n|name|free>`, `nudge <dx> <dy> <dz> [frames]`.
//! - `LEONIDA_GAME_ACTIONS_WARP=1` lets a teleport write the ped's position on the title's thread: an
//!   experiment, the title's own function for moving a ped was not found.
//!
//! What is done is logged to `%TEMP%\leonida-actions\log.txt` (addresses of the user's run: never committed).

use std::collections::VecDeque;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU64, Ordering};
use std::sync::{Mutex, OnceLock};
use std::time::Duration;

use leonida_loader::game_actions::{self as places, Site, Sites};
use leonida_menu::model::Value;

use crate::player::mem;

/// The title's deferred call takes four words: what the function is given, the function, how its argument is
/// copied into the queue and how the copy is dropped (the pattern of `Site::Defer` fills them in that order).
#[repr(C)]
struct Closure {
    data: *mut u32,
    call: unsafe extern "win64" fn(*mut u32),
    copy: unsafe extern "win64" fn(*mut u32, *const u32),
    drop: unsafe extern "win64" fn(*mut u32),
}

/// What the menu (or the test script) asks the title's thread to do.
#[derive(Clone, Debug, PartialEq)]
pub enum Request {
    /// One of the title's own cheats, by its number in the title's table.
    Cheat(u32),
    /// The player's wanted level; 0 clears it.
    Wanted(i32),
    /// The time of day.
    Time { hour: i32, minute: i32, second: i32 },
    /// The hour, the minute kept.
    Hour(i32),
    /// So many hours on (or back).
    Hours(i32),
    /// The clock stopped or running.
    ClockPaused(bool),
    /// The weather type with this index, kept until [`Request::WeatherFree`].
    Weather(u32),
    /// The weather forced to what it is now.
    WeatherFreeze,
    WeatherFree,
    /// The ped's position written for so many frames (`LEONIDA_GAME_ACTIONS_WARP=1`). Relative: added to where
    /// the ped is when the request is taken.
    Warp { to: [f32; 3], relative: bool, frames: u32 },
    /// What the runtime sees of the title, into the log.
    Info,
}

/// The title's functions, each at the address its pattern gave.
struct Game {
    base: usize,
    size: usize,
    sites: Sites,
    cheats_update: unsafe extern "win64" fn(),
    defer: unsafe extern "win64" fn(u32, *const Closure),
}

static GAME: OnceLock<Game> = OnceLock::new();
static QUEUE: Mutex<VecDeque<Request>> = Mutex::new(VecDeque::new());
/// Times the hook ran (the title's update steps), and times our deferred work ran: inside the hook's own call,
/// and later, at the queue's point of rest.
static FRAMES: AtomicU64 = AtomicU64::new(0);
static RAN_AT_ONCE: AtomicU64 = AtomicU64::new(0);
static RAN_LATER: AtomicU64 = AtomicU64::new(0);
static IN_HOOK: AtomicBool = AtomicBool::new(false);
/// The threads the hook and the work last ran on.
static HOOK_THREAD: AtomicU64 = AtomicU64::new(0);
static WORK_THREAD: AtomicU64 = AtomicU64::new(0);
/// A panic in our work: nothing more is done in the title.
static BROKEN: AtomicBool = AtomicBool::new(false);
/// The wanted level last seen (-1: not yet), and whether the menu wants it read again.
static WANTED: AtomicI32 = AtomicI32::new(-1);
static SAMPLE: AtomicBool = AtomicBool::new(false);
static NEVER_WANTED: AtomicBool = AtomicBool::new(false);
static CLOCK_SYNC: AtomicBool = AtomicBool::new(false);
/// The position a warp holds, and for how many more frames.
static WARP: Mutex<Option<([f32; 3], u32)>> = Mutex::new(None);
/// The deferred call copies four bytes of this; what they are does not matter.
static TICKET: AtomicU64 = AtomicU64::new(0);

/// Jenkins one-at-a-time of the lower-cased name: the title's hash of names (its weather types').
pub fn joaat(name: &str) -> u32 {
    let mut h = 0u32;
    for c in name.to_ascii_lowercase().bytes() {
        h = h.wrapping_add(u32::from(c));
        h = h.wrapping_add(h << 10);
        h ^= h >> 6;
    }
    h = h.wrapping_add(h << 3);
    h ^= h >> 11;
    h.wrapping_add(h << 15)
}

/// The names the menu's weather choices are looked up by in the title's table of weather types (which holds
/// the hashes of its types' names, loaded from the title's data), in the order of the menu's list.
pub const WEATHER_NAMES: [&str; 9] = [
    "EXTRASUNNY",
    "CLEAR",
    "CLOUDS",
    "OVERCAST",
    "FOGGY",
    "RAIN",
    "THUNDER",
    "SMOG",
    "WINDY",
];

/// The menu's vehicle items, in the order of the title's nine vehicle cheats (7 to 15). Which vehicle each
/// cheat holds is not known: the labels are the menu's, the order is the table's.
const SPAWNS: [&str; 9] = [
    "spawn.sports",
    "spawn.super",
    "spawn.muscle",
    "spawn.suv",
    "spawn.offroad",
    "spawn.bike",
    "spawn.boat",
    "spawn.heli",
    "spawn.plane",
];
/// The title's cheat for health and armour, and the first of its vehicle cheats.
const CHEAT_HEALTH: u32 = 2;
const CHEAT_VEHICLES: u32 = 7;

/// Where an index is in a list of hashes.
fn index_of(hashes: &[u32], hash: u32) -> Option<usize> {
    hashes.iter().position(|h| *h == hash)
}

/// One command of `LEONIDA_GAME_ACTIONS_RUN`. A weather name is given back as text: only the running title has
/// the table it is looked up in.
#[derive(Debug, PartialEq)]
enum Command {
    Do(Request),
    WeatherNamed(String),
}

fn parse_command(text: &str) -> Result<Command, String> {
    let mut words = text.split_whitespace();
    let name = words.next().ok_or("an empty command")?;
    let rest: Vec<&str> = words.collect();
    let int = |i: usize| -> Result<i32, String> {
        let word = rest.get(i).ok_or_else(|| format!("{name}: a number is missing"))?;
        word.parse::<i32>().map_err(|_| format!("{name}: {word} is not a number"))
    };
    let float = |i: usize| -> Result<f32, String> {
        let word = rest.get(i).ok_or_else(|| format!("{name}: a number is missing"))?;
        word.parse::<f32>().ok().filter(|v| v.is_finite()).ok_or_else(|| format!("{name}: {word} is not a number"))
    };
    Ok(Command::Do(match name {
        "info" => Request::Info,
        "cheat" => Request::Cheat(u32::try_from(int(0)?).map_err(|_| "cheat: a number from 0 on")?),
        "wanted" => Request::Wanted(int(0)?),
        "time" => Request::Time {
            hour: int(0)?,
            minute: if rest.len() > 1 { int(1)? } else { 0 },
            second: if rest.len() > 2 { int(2)? } else { 0 },
        },
        "hours" => Request::Hours(int(0)?),
        "pause" => Request::ClockPaused(int(0)? != 0),
        "weather" => match rest.first().copied() {
            Some("free") => Request::WeatherFree,
            Some("freeze") => Request::WeatherFreeze,
            Some(word) => match word.parse::<u32>() {
                Ok(index) => Request::Weather(index),
                Err(_) => return Ok(Command::WeatherNamed(word.to_owned())),
            },
            None => return Err("weather: a type's number or name, or free".into()),
        },
        "nudge" => Request::Warp {
            to: [float(0)?, float(1)?, float(2)?],
            relative: true,
            frames: if rest.len() > 3 { int(3)?.clamp(1, 600) as u32 } else { 30 },
        },
        _ => return Err(format!("{name}: not a command")),
    }))
}

/// `LEONIDA_GAME_ACTIONS_RUN`: `<seconds>:<command>` parts between `;`, in the order given.
fn parse_script(text: &str) -> Result<Vec<(f32, Command)>, String> {
    text.split(';')
        .map(str::trim)
        .filter(|part| !part.is_empty())
        .map(|part| {
            let (at, command) = part.split_once(':').ok_or_else(|| format!("{part}: expected <seconds>:<command>"))?;
            let at = at.trim().parse::<f32>().ok().filter(|s| s.is_finite() && *s >= 0.0);
            Ok((at.ok_or_else(|| format!("{part}: the seconds do not read"))?, parse_command(command)?))
        })
        .collect()
}

fn valid_time(hour: i32, minute: i32, second: i32) -> bool {
    (0..24).contains(&hour) && (0..60).contains(&minute) && (0..60).contains(&second)
}

fn out_dir() -> PathBuf {
    std::env::temp_dir().join("leonida-actions")
}

/// A line for the console and the log file.
fn log(line: &str) {
    use std::io::Write as _;
    eprintln!("Leonida: game actions: {line}");
    let dir = out_dir();
    let _ = std::fs::create_dir_all(&dir);
    if let Ok(mut file) = std::fs::OpenOptions::new().create(true).append(true).open(dir.join("log.txt")) {
        let _ = writeln!(file, "{line}");
    }
}

fn thread_id() -> u64 {
    unsafe extern "system" {
        fn GetCurrentThreadId() -> u32;
    }
    // SAFETY: a call without arguments or conditions.
    u64::from(unsafe { GetCurrentThreadId() })
}

/// The PC's local hour, minute and second.
fn local_time() -> (i32, i32, i32) {
    unsafe extern "system" {
        fn GetLocalTime(time: *mut [u16; 8]);
    }
    let mut time = [0u16; 8];
    // SAFETY: fills the eight words of a SYSTEMTIME.
    unsafe { GetLocalTime(&mut time) };
    (i32::from(time[4]), i32::from(time[5]), i32::from(time[6]))
}

fn read_u32(address: usize) -> Option<u32> {
    let mut b = [0u8; 4];
    mem::read(address, &mut b).then(|| u32::from_le_bytes(b))
}

/// Three floats at `address`, when they are numbers of a place in a world.
fn read_vector(address: usize) -> Option<[f32; 3]> {
    let b = mem::read_vec(address, 12)?;
    let v: [f32; 3] = std::array::from_fn(|i| f32::from_le_bytes(b[i * 4..i * 4 + 4].try_into().unwrap()));
    v.iter().all(|c| c.is_finite() && c.abs() < 1.0e6).then_some(v)
}

/// What the weather state holds.
struct Weather {
    hashes: Vec<u32>,
    prev: u32,
    next: u32,
    forced: u32,
}

const CHEAT_SITES: &[Site] = &[Site::CheatDispatch, Site::CheatTable, Site::PlayerPed];
const WANTED_SITES: &[Site] = &[Site::WantedSet, Site::WantedGet, Site::WantedLevelAt, Site::PlayerPed];
const CLOCK_SITES: &[Site] = &[Site::ClockSet, Site::ClockTime];
const PAUSE_SITES: &[Site] = &[Site::ClockPaused];
const WEATHER_SITES: &[Site] = &[
    Site::WeatherForce,
    Site::WeatherClear,
    Site::Weather,
    Site::WeatherPrev,
    Site::WeatherNext,
    Site::WeatherForced,
    Site::WeatherTypes,
];

impl Game {
    /// The site's address in this run.
    fn at(&self, site: Site) -> Option<usize> {
        self.sites.get(site).map(|rva| self.base + rva)
    }

    /// The player's ped, when there is one.
    fn ped(&self) -> Option<usize> {
        let ped = mem::read_u64(self.at(Site::PlayerPed)?)? as usize;
        (ped > 0x10000).then_some(ped)
    }

    fn position(&self) -> Option<[f32; 3]> {
        read_vector(self.ped()? + places::ENTITY_POSITION_AT)
    }

    fn forward(&self) -> Option<[f32; 3]> {
        read_vector(self.ped()? + places::ENTITY_FORWARD_AT)
    }

    /// Hour, minute, second.
    fn clock(&self) -> Option<(i32, i32, i32)> {
        let b = mem::read_vec(self.at(Site::ClockTime)?, 12)?;
        let n = |i: usize| i32::from_le_bytes(b[i * 4..i * 4 + 4].try_into().unwrap());
        valid_time(n(0), n(1), n(2)).then(|| (n(0), n(1), n(2)))
    }

    fn weather(&self) -> Option<Weather> {
        let count = read_u32(self.at(Site::Weather)?)? as usize;
        let types = mem::read_u64(self.at(Site::WeatherTypes)?)? as usize;
        // A table that is not loaded yet, or is not one.
        if count == 0 || count > 256 || types < 0x10000 {
            return None;
        }
        let hashes = (0..count)
            .map(|i| read_u32(types + i * places::WEATHER_TYPE_STRIDE))
            .collect::<Option<Vec<u32>>>()?;
        Some(Weather {
            hashes,
            prev: read_u32(self.at(Site::WeatherPrev)?)?,
            next: read_u32(self.at(Site::WeatherNext)?)?,
            forced: read_u32(self.at(Site::WeatherForced)?)?,
        })
    }

    /// Whether what `sites` name was found and the title's update runs.
    fn ready(&self, sites: &[Site], needs_player: bool) -> Result<(), String> {
        if BROKEN.load(Ordering::Relaxed) {
            return Err("Stopped: the runtime's own work in the title failed once (see the log).".into());
        }
        if !self.sites.has(sites) {
            return Err("Not in this executable: the title's code for it was not found (another build).".into());
        }
        if FRAMES.load(Ordering::Relaxed) == 0 {
            return Err("Not yet: the title's update has not run (still loading).".into());
        }
        if needs_player && self.ped().is_none() {
            return Err("Not yet: there is no player in the world.".into());
        }
        Ok(())
    }

    /// The wanted level of the ped at `ped`.
    ///
    /// # Safety
    /// On the title's thread, from the deferred call.
    unsafe fn wanted_level(&self, ped: usize) -> Option<i32> {
        let get: unsafe extern "win64" fn(u32) -> usize =
            // SAFETY: the address of the title's function, found by its pattern.
            unsafe { std::mem::transmute(self.at(Site::WantedGet)?) };
        let id = read_u32(ped + places::ENTITY_ID_AT)?;
        // SAFETY: the title's own lookup, called as the title's wanted-level function calls it.
        let state = unsafe { get(id) };
        if state < 0x10000 {
            return None;
        }
        read_u32(state + self.sites.get(Site::WantedLevelAt)?).map(|level| level as i32)
    }

    /// Does one request.
    ///
    /// # Safety
    /// On the title's thread, from the deferred call.
    unsafe fn perform(&self, request: &Request, frame: u64) -> Result<String, String> {
        let missing = "the title's code for it was not found";
        match *request {
            Request::Cheat(number) => {
                if number >= places::CHEATS {
                    return Err(format!("cheat {number}: the title's table has {} cheats", places::CHEATS));
                }
                let (Some(dispatch), Some(table)) = (self.at(Site::CheatDispatch), self.at(Site::CheatTable)) else {
                    return Err(missing.into());
                };
                // The table's entry is a function of the title, or the table is not what it was taken for.
                let handler = mem::read_u64(table + number as usize * 8).unwrap_or(0) as usize;
                if !(self.base..self.base + self.size).contains(&handler) {
                    return Err(format!("cheat {number}: the table's entry is not code of the title"));
                }
                let dispatch: unsafe extern "win64" fn(*const u32) =
                    // SAFETY: the address of the title's dispatcher, found by its pattern.
                    unsafe { std::mem::transmute(dispatch) };
                // SAFETY: what the title does with a typed cheat: the dispatcher with the cheat's number.
                unsafe { dispatch(&number) };
                Ok(format!("cheat {number} run by the title"))
            }
            Request::Wanted(level) => {
                let level = level.clamp(0, 6);
                let ped = self.ped().ok_or("no player")?;
                let Some(set) = self.at(Site::WantedSet) else { return Err(missing.into()) };
                // SAFETY: forwarded from this function's contract.
                let now = unsafe { self.wanted_level(ped) }.ok_or("the player has no wanted state")?;
                if now == level {
                    return Ok(format!("wanted level already {level}"));
                }
                let set: unsafe extern "win64" fn(usize, i32, u32, u32, u32) =
                    // SAFETY: the address of the title's function, found by its pattern.
                    unsafe { std::mem::transmute(set) };
                // The flags the title's own two cheats pass: (0, 1, 1) to raise, (1, 1, 1) to lower.
                // SAFETY: the title's function with the player's ped, as its cheats call it.
                unsafe { set(ped, level, u32::from(level < now), 1, 1) };
                // SAFETY: forwarded from this function's contract.
                let after = unsafe { self.wanted_level(ped) }.unwrap_or(-1);
                WANTED.store(after, Ordering::Relaxed);
                Ok(format!("wanted level {now} -> {level} asked, now {after}"))
            }
            Request::Time { hour, minute, second } => {
                if !valid_time(hour, minute, second) {
                    return Err(format!("{hour}:{minute}:{second} is not a time of day"));
                }
                let Some(set) = self.at(Site::ClockSet) else { return Err(missing.into()) };
                let before = self.clock();
                let set: unsafe extern "win64" fn(i32, i32, i32) =
                    // SAFETY: the address of the title's function, found by its pattern.
                    unsafe { std::mem::transmute(set) };
                // SAFETY: the title's own setter of the time of day, three numbers in range.
                unsafe { set(hour, minute, second) };
                Ok(format!("time {before:?} -> {:?}", self.clock()))
            }
            Request::Hour(hour) => {
                let (_, minute, _) = self.clock().ok_or("the clock does not read")?;
                // SAFETY: forwarded from this function's contract.
                unsafe { self.perform(&Request::Time { hour, minute, second: 0 }, frame) }
            }
            Request::Hours(by) => {
                let (hour, minute, second) = self.clock().ok_or("the clock does not read")?;
                let hour = (hour + by).rem_euclid(24);
                // SAFETY: forwarded from this function's contract.
                unsafe { self.perform(&Request::Time { hour, minute, second }, frame) }
            }
            Request::ClockPaused(paused) => {
                let at = self.at(Site::ClockPaused).ok_or(missing)?;
                // The title's own setter is this one store.
                if !mem::write(at, &[u8::from(paused)]) {
                    return Err("the clock's switch could not be written".into());
                }
                Ok(format!("clock {}", if paused { "stopped" } else { "running" }))
            }
            Request::Weather(index) => {
                let weather = self.weather().ok_or("the weather table does not read")?;
                if index as usize >= weather.hashes.len() {
                    return Err(format!("weather {index}: the title has {} types", weather.hashes.len()));
                }
                let Some(force) = self.at(Site::WeatherForce) else { return Err(missing.into()) };
                let force: unsafe extern "win64" fn(u32, u32) =
                    // SAFETY: the address of the title's function, found by its pattern.
                    unsafe { std::mem::transmute(force) };
                // SAFETY: the title's own function, with a type index below its count and no flag.
                unsafe { force(index, 0) };
                let after = self.weather().map(|w| (w.prev, w.next, w.forced as i32));
                Ok(format!("weather type {index} forced (was {} -> {}), now {after:?}", weather.prev, weather.next))
            }
            Request::WeatherFreeze => {
                let weather = self.weather().ok_or("the weather table does not read")?;
                // SAFETY: forwarded from this function's contract.
                unsafe { self.perform(&Request::Weather(weather.next), frame) }
            }
            Request::WeatherFree => {
                let Some(clear) = self.at(Site::WeatherClear) else { return Err(missing.into()) };
                let clear: unsafe extern "win64" fn() =
                    // SAFETY: the address of the title's function, found by its pattern.
                    unsafe { std::mem::transmute(clear) };
                // SAFETY: the title's own function, which takes nothing.
                unsafe { clear() };
                Ok("weather runs again".into())
            }
            Request::Warp { to, relative, frames } => {
                if !warp_allowed() {
                    return Err("moving the ped is an experiment: LEONIDA_GAME_ACTIONS_WARP=1 allows it".into());
                }
                let here = self.position().ok_or("no player")?;
                let to = if relative { [here[0] + to[0], here[1] + to[1], here[2] + to[2]] } else { to };
                *WARP.lock().unwrap_or_else(|e| e.into_inner()) = Some((to, frames.max(1)));
                Ok(format!("ped at {here:?}: its position is written as {to:?} for {frames} frames"))
            }
            // SAFETY: forwarded from this function's contract.
            Request::Info => Ok(unsafe { self.info(frame) }),
        }
    }

    /// What the runtime sees of the title now.
    ///
    /// # Safety
    /// On the title's thread, from the deferred call.
    unsafe fn info(&self, frame: u64) -> String {
        use std::fmt::Write as _;
        let mut s = format!(
            "info at update {frame}: hook thread {}, work thread {}, work ran at once {} times and later {} times\n",
            HOOK_THREAD.load(Ordering::Relaxed),
            WORK_THREAD.load(Ordering::Relaxed),
            RAN_AT_ONCE.load(Ordering::Relaxed),
            RAN_LATER.load(Ordering::Relaxed),
        );
        let ped = self.ped();
        let _ = writeln!(s, "  player ped {ped:x?}, position {:?}, forward {:?}", self.position(), self.forward());
        // SAFETY: forwarded from this function's contract.
        let wanted = ped.and_then(|ped| unsafe { self.wanted_level(ped) });
        let _ = writeln!(s, "  wanted level {wanted:?}, clock {:?}", self.clock());
        match self.weather() {
            Some(w) => {
                let names: Vec<String> = w
                    .hashes
                    .iter()
                    .map(|h| match WEATHER_NAMES.iter().find(|n| joaat(n) == *h) {
                        Some(name) => (*name).to_owned(),
                        None => format!("{h:08x}"),
                    })
                    .collect();
                let _ = writeln!(
                    s,
                    "  weather: {} types {names:?}, previous {}, next {}, forced {}",
                    w.hashes.len(),
                    w.prev,
                    w.next,
                    w.forced as i32
                );
            }
            None => s.push_str("  weather: the table does not read\n"),
        }
        if let Some(table) = self.at(Site::CheatTable) {
            let entries: Vec<String> = (0..places::CHEATS as usize)
                .map(|i| match mem::read_u64(table + i * 8) {
                    Some(v) if (self.base..self.base + self.size).contains(&(v as usize)) => {
                        format!("{:x}", v as usize - self.base)
                    }
                    Some(v) => format!("?{v:x}"),
                    None => "unreadable".into(),
                })
                .collect();
            let _ = writeln!(s, "  cheat handlers (offsets in the image): {entries:?}");
        }
        s.push_str(&format!("  not found: {:?}", self.sites.missing));
        s
    }

    /// Our work on the title's thread: what was asked, then what is kept up.
    ///
    /// # Safety
    /// On the title's thread, from the deferred call.
    unsafe fn work(&self) {
        let frame = FRAMES.load(Ordering::Relaxed);
        let asked: Vec<Request> = QUEUE.lock().unwrap_or_else(|e| e.into_inner()).drain(..).collect();
        for request in asked {
            // SAFETY: forwarded from this function's contract.
            match unsafe { self.perform(&request, frame) } {
                Ok(line) => log(&format!("update {frame}: {line}")),
                Err(why) => log(&format!("update {frame}: {request:?} not done: {why}")),
            }
        }
        let never = NEVER_WANTED.load(Ordering::Relaxed);
        if (SAMPLE.swap(false, Ordering::Relaxed) || never)
            && self.sites.has(WANTED_SITES)
            && let Some(ped) = self.ped()
        {
            // SAFETY: forwarded from this function's contract.
            let level = unsafe { self.wanted_level(ped) }.unwrap_or(-1);
            WANTED.store(level, Ordering::Relaxed);
            if never && level > 0 {
                // SAFETY: forwarded from this function's contract.
                if let Err(why) = unsafe { self.perform(&Request::Wanted(0), frame) } {
                    log(&format!("update {frame}: never wanted: {why}"));
                }
            }
        }
        if CLOCK_SYNC.load(Ordering::Relaxed) && self.sites.has(CLOCK_SITES) {
            let (hour, minute, second) = local_time();
            if self.clock().is_some_and(|(h, m, _)| (h, m) != (hour, minute)) {
                // SAFETY: forwarded from this function's contract.
                let _ = unsafe { self.perform(&Request::Time { hour, minute, second }, frame) };
            }
        }
        let mut warp = WARP.lock().unwrap_or_else(|e| e.into_inner());
        if let Some((to, left)) = *warp {
            let mut bytes = [0u8; 12];
            for (i, v) in to.iter().enumerate() {
                bytes[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
            }
            let written = self.ped().is_some_and(|ped| mem::write(ped + places::ENTITY_POSITION_AT, &bytes));
            if left <= 1 || !written {
                log(&format!("update {frame}: position write ended (written {written}); the ped is at {:?}", self.position()));
                *warp = None;
            } else {
                *warp = Some((to, left - 1));
            }
        }
    }
}

fn warp_allowed() -> bool {
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_GAME_ACTIONS_WARP").is_some_and(|v| v == "1"))
}

/// Whether the title's thread has something of ours to do this frame.
fn work_waits(frame: u64) -> bool {
    // Never waits for the menu's thread: a queue it holds now is looked at next frame.
    let asked = QUEUE.try_lock().is_ok_and(|q| !q.is_empty());
    asked
        || SAMPLE.load(Ordering::Relaxed)
        || WARP.try_lock().is_ok_and(|w| w.is_some())
        // What is kept up is looked at twice a second or so.
        || (frame.is_multiple_of(30) && (NEVER_WANTED.load(Ordering::Relaxed) || CLOCK_SYNC.load(Ordering::Relaxed)))
}

/// The title's update step calls this in place of its cheat input function.
extern "win64" fn frame_hook() {
    let Some(game) = GAME.get() else { return };
    // SAFETY: the title's own function, called where the title called it, with what it takes: nothing.
    unsafe { (game.cheats_update)() };
    let frame = FRAMES.fetch_add(1, Ordering::Relaxed) + 1;
    if BROKEN.load(Ordering::Relaxed) || !work_waits(frame) {
        return;
    }
    HOOK_THREAD.store(thread_id(), Ordering::Relaxed);
    let closure = Closure {
        data: TICKET.as_ptr().cast(),
        call: deferred,
        copy: copy_ticket,
        drop: drop_ticket,
    };
    IN_HOOK.store(true, Ordering::Relaxed);
    // SAFETY: the title's deferred call, as its cheat input function calls it for a typed cheat: the cheats'
    // queue and a closure of four words, which it copies before it returns.
    unsafe { (game.defer)(places::CHEAT_QUEUE, &closure) };
    IN_HOOK.store(false, Ordering::Relaxed);
}

/// What the title's queue calls: at once when the thread is not inside a parallel update, else at the queue's
/// next point of rest.
unsafe extern "win64" fn deferred(_ticket: *mut u32) {
    let Some(game) = GAME.get() else { return };
    if IN_HOOK.load(Ordering::Relaxed) {
        RAN_AT_ONCE.fetch_add(1, Ordering::Relaxed);
    } else {
        RAN_LATER.fetch_add(1, Ordering::Relaxed);
    }
    WORK_THREAD.store(thread_id(), Ordering::Relaxed);
    // SAFETY: this is the deferred call, on the title's thread.
    if catch_unwind(AssertUnwindSafe(|| unsafe { game.work() })).is_err() {
        BROKEN.store(true, Ordering::Relaxed);
        eprintln!("Leonida: game actions: the runtime's work in the title failed; nothing more is done there");
    }
}

/// Copies the closure's argument into the title's queue.
unsafe extern "win64" fn copy_ticket(to: *mut u32, from: *const u32) {
    if !to.is_null() && !from.is_null() {
        // SAFETY: the queue's storage for one entry and our ticket: four bytes each.
        unsafe { to.write_unaligned(from.read_unaligned()) };
    }
}

unsafe extern "win64" fn drop_ticket(_ticket: *mut u32) {}

fn ask(request: Request) {
    QUEUE.lock().unwrap_or_else(|e| e.into_inner()).push_back(request);
}

/// Where the player's ped is, when the game actions are on and there is one (the title's own global, which its
/// own position function reads).
pub fn position() -> Option<[f32; 3]> {
    GAME.get()?.position()
}

/// The horizontal direction the ped faces, as a unit vector.
pub fn heading() -> Option<[f32; 2]> {
    let v = GAME.get()?.forward()?;
    let len = (v[0] * v[0] + v[1] * v[1]).sqrt();
    (len > 1.0e-3).then(|| [v[0] / len, v[1] / len])
}

/// A teleport through the game actions: `None` when they are off or the experiment is not allowed.
pub fn warp(to: [f32; 3], hold_ms: u32) -> Option<Result<String, String>> {
    let game = GAME.get()?;
    if !warp_allowed() {
        return None;
    }
    Some(game.ready(&[Site::PlayerPed], true).map(|()| {
        // About 30 updates a second.
        ask(Request::Warp { to, relative: false, frames: (hold_ms / 33).clamp(1, 600) });
        format!("asked the title's thread to write the ped's position ({:.1}, {:.1}, {:.1}); an experiment", to[0], to[1], to[2])
    }))
}

/// Whether the game actions run in this process (the title's code was found and its update step hooked).
pub fn on() -> bool {
    GAME.get().is_some()
}

fn available(id: &str) -> Option<Result<(), String>> {
    let game = GAME.get()?;
    Some(match id {
        "player.heal" => game.ready(CHEAT_SITES, true),
        id if SPAWNS.contains(&id) => game.ready(CHEAT_SITES, true),
        "player.wanted" | "player.clear_wanted" | "player.never_wanted" => game.ready(WANTED_SITES, true),
        "time.hour" | "time.forward" | "time.back" | "time.system" => game.ready(CLOCK_SITES, false),
        "time.paused" => game.ready(PAUSE_SITES, false),
        "weather.type" | "weather.freeze" => game.ready(WEATHER_SITES, false).and_then(|()| {
            game.weather().map(|_| ()).ok_or_else(|| "Not yet: the title's weather types are not loaded.".to_owned())
        }),
        _ => return None,
    })
}

fn value(id: &str) -> Option<Value> {
    let game = GAME.get()?;
    Some(match id {
        "player.wanted" => {
            // Read on the title's thread at its next update; shown from then on.
            SAMPLE.store(true, Ordering::Relaxed);
            Value::Number(f64::from(WANTED.load(Ordering::Relaxed).max(0)))
        }
        "player.never_wanted" => Value::Bool(NEVER_WANTED.load(Ordering::Relaxed)),
        "time.hour" => Value::Number(f64::from(game.clock().map_or(0, |(hour, _, _)| hour))),
        "time.paused" => {
            let paused = game.at(Site::ClockPaused).and_then(|at| mem::read_vec(at, 1));
            Value::Bool(paused.is_some_and(|b| b[0] != 0))
        }
        "time.system" => Value::Bool(CLOCK_SYNC.load(Ordering::Relaxed)),
        "weather.type" => {
            let shown = game.weather().and_then(|w| {
                let hash = *w.hashes.get(w.next as usize)?;
                WEATHER_NAMES.iter().position(|name| joaat(name) == hash)
            });
            Value::Index(shown.unwrap_or(0))
        }
        "weather.freeze" => Value::Bool(game.weather().is_some_and(|w| w.forced != u32::MAX)),
        _ => return None,
    })
}

fn apply(id: &str, value: Value) -> Option<Result<Option<String>, String>> {
    let game = GAME.get()?;
    let asked = |request: Request, line: String| -> Result<Option<String>, String> {
        ask(request);
        Ok(Some(line))
    };
    Some(match (id, value) {
        ("player.heal", _) => asked(
            Request::Cheat(CHEAT_HEALTH),
            "Asked the title for its own health cheat.".into(),
        ),
        (id, _) if SPAWNS.contains(&id) => {
            let number = CHEAT_VEHICLES + SPAWNS.iter().position(|s| *s == id).unwrap_or(0) as u32;
            asked(
                Request::Cheat(number),
                format!("Asked the title for its vehicle cheat {number}. Which vehicle each of the nine holds is not known yet."),
            )
        }
        ("player.wanted", Value::Number(level)) => {
            let level = level.round().clamp(0.0, 6.0) as i32;
            // Shown at once; the title's answer replaces it at its next update.
            WANTED.store(level, Ordering::Relaxed);
            asked(Request::Wanted(level), format!("Asked for wanted level {level}."))
        }
        ("player.clear_wanted", _) => asked(Request::Wanted(0), "Asked for the wanted level to be cleared.".into()),
        ("player.never_wanted", Value::Bool(on)) => {
            NEVER_WANTED.store(on, Ordering::Relaxed);
            Ok(Some(if on { "The wanted level is cleared whenever it rises." } else { "The wanted level is the title's again." }.into()))
        }
        ("time.hour", Value::Number(hour)) => {
            let hour = (hour.round() as i32).rem_euclid(24);
            asked(Request::Hour(hour), format!("Asked for {hour}:00."))
        }
        ("time.forward", _) => asked(Request::Hours(1), "Asked for an hour on.".into()),
        ("time.back", _) => asked(Request::Hours(-1), "Asked for an hour back.".into()),
        ("time.paused", Value::Bool(on)) => asked(
            Request::ClockPaused(on),
            if on { "Asked for the clock to stop." } else { "Asked for the clock to run." }.into(),
        ),
        ("time.system", Value::Bool(on)) => {
            CLOCK_SYNC.store(on, Ordering::Relaxed);
            Ok(Some(if on { "The time of day follows the PC's clock." } else { "The time of day is the title's again." }.into()))
        }
        ("weather.type", Value::Index(choice)) => {
            let Some(name) = WEATHER_NAMES.get(choice) else { return Some(Err("No such weather.".into())) };
            match game.weather().map(|w| (index_of(&w.hashes, joaat(name)), w.hashes.len())) {
                Some((Some(index), _)) => asked(Request::Weather(index as u32), format!("Asked for {name}.")),
                Some((None, count)) => Err(format!("The title has no weather type named {name} (it has {count} types).")),
                None => Err("The title's weather types are not loaded.".into()),
            }
        }
        ("weather.freeze", Value::Bool(on)) => asked(
            if on { Request::WeatherFreeze } else { Request::WeatherFree },
            if on { "Asked for the weather to stay as it is." } else { "Asked for the weather to run again." }.into(),
        ),
        _ => return None,
    })
}

/// Runs `LEONIDA_GAME_ACTIONS_RUN` on a thread of its own, which only queues.
fn run_script(script: Vec<(f32, Command)>) {
    let spawned = std::thread::Builder::new().name("leonida-game-actions".into()).spawn(move || {
        let start = std::time::Instant::now();
        for (at, command) in script {
            let wait = Duration::from_secs_f32(at).saturating_sub(start.elapsed());
            std::thread::sleep(wait);
            let request = match command {
                Command::Do(request) => request,
                Command::WeatherNamed(name) => {
                    let found = GAME.get().and_then(Game::weather).and_then(|w| index_of(&w.hashes, joaat(&name)));
                    match found {
                        Some(index) => Request::Weather(index as u32),
                        None => {
                            log(&format!("at {at} s: the title has no weather type named {name}"));
                            continue;
                        }
                    }
                }
            };
            log(&format!("at {at} s: asking {request:?} (updates so far: {})", FRAMES.load(Ordering::Relaxed)));
            ask(request);
        }
    });
    if spawned.is_err() {
        log("the script's thread could not be started");
    }
}

/// With `LEONIDA_GAME_ACTIONS=1`: finds the title's code in the image just mapped, hooks its cheat update and
/// lends the menu the actions. Nothing is changed in the title otherwise.
///
/// # Safety
/// `base` is the title's executable `info` describes, mapped, and none of its code runs yet.
pub unsafe fn install(base: usize, info: &leonida_pe::ImageInfo) {
    if !places::wanted() {
        return;
    }
    // SAFETY: forwarded from this function's contract; the hook is a function of this program.
    let (sites, hooked) = unsafe { places::install(base, info, frame_hook as extern "win64" fn() as usize) };
    for line in &sites.missing {
        println!("game actions   not found: {line}");
    }
    if let Err(why) = hooked {
        println!("game actions   off: {why}");
        return;
    }
    let (Some(update), Some(defer)) = (sites.get(Site::CheatsUpdate), sites.get(Site::Defer)) else {
        return;
    };
    let game = Game {
        base,
        size: info.size_of_image as usize,
        // SAFETY: the addresses of the title's functions, found by their patterns.
        cheats_update: unsafe { std::mem::transmute::<usize, unsafe extern "win64" fn()>(base + update) },
        // SAFETY: as above.
        defer: unsafe {
            std::mem::transmute::<usize, unsafe extern "win64" fn(u32, *const Closure)>(base + defer)
        },
        sites,
    };
    println!(
        "game actions   on ({}=1): the title's cheat update is hooked; {} of {} things found; log in {}",
        places::ENV,
        game.sites.count(),
        places::SITES,
        out_dir().display()
    );
    if GAME.set(game).is_err() {
        return;
    }
    leonida_menu::knobs::provide_game(leonida_menu::knobs::GameHook { available, value, apply });
    if let Ok(text) = std::env::var("LEONIDA_GAME_ACTIONS_RUN") {
        match parse_script(&text) {
            Ok(script) => run_script(script),
            Err(why) => println!("game actions   LEONIDA_GAME_ACTIONS_RUN not used: {why}"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_hash_as_the_title_hashes_them() {
        assert_eq!(joaat("a"), 0xca2e9442);
        // The two weather names the title's own code asks for by hash.
        assert_eq!(joaat("RAIN"), 0x54a69840);
        assert_eq!(joaat("rain"), joaat("RAIN"));
        assert_eq!(joaat("SNOW"), 0xefb6eff6);
        // The menu's nine choices have nine different names.
        let mut hashes: Vec<u32> = WEATHER_NAMES.iter().map(|n| joaat(n)).collect();
        hashes.sort_unstable();
        hashes.dedup();
        assert_eq!(hashes.len(), WEATHER_NAMES.len());
    }

    #[test]
    fn a_weather_choice_is_found_by_its_name_in_the_titles_table() {
        let table = [joaat("CLEAR"), 0x1234_5678, joaat("RAIN"), joaat("THUNDER")];
        assert_eq!(index_of(&table, joaat("RAIN")), Some(2));
        assert_eq!(index_of(&table, joaat("Thunder")), Some(3));
        assert_eq!(index_of(&table, joaat("SMOG")), None);
        assert_eq!(index_of(&[], joaat("RAIN")), None);
    }

    #[test]
    fn the_spawn_items_are_the_nine_vehicle_cheats() {
        assert_eq!(SPAWNS.len() as u32, places::CHEATS - CHEAT_VEHICLES);
        assert!(CHEAT_HEALTH < CHEAT_VEHICLES);
        // The menu's own ids, each once.
        let pages = leonida_menu::tree::pages();
        let ids: Vec<&str> = pages.iter().flat_map(|p| p.items.iter().map(|i| i.id)).collect();
        for id in SPAWNS {
            assert_eq!(ids.iter().filter(|i| **i == id).count(), 1, "{id}");
        }
        for id in [
            "player.heal",
            "player.wanted",
            "player.clear_wanted",
            "player.never_wanted",
            "time.hour",
            "time.forward",
            "time.back",
            "time.paused",
            "time.system",
            "weather.type",
            "weather.freeze",
        ] {
            assert!(ids.contains(&id), "{id}");
        }
    }

    #[test]
    fn script_commands_read() {
        let d = |r| Ok(Command::Do(r));
        assert_eq!(parse_command("info"), d(Request::Info));
        assert_eq!(parse_command(" cheat 2 "), d(Request::Cheat(2)));
        assert_eq!(parse_command("wanted 3"), d(Request::Wanted(3)));
        assert_eq!(parse_command("time 12"), d(Request::Time { hour: 12, minute: 0, second: 0 }));
        assert_eq!(parse_command("time 7 30 15"), d(Request::Time { hour: 7, minute: 30, second: 15 }));
        assert_eq!(parse_command("hours -2"), d(Request::Hours(-2)));
        assert_eq!(parse_command("pause 1"), d(Request::ClockPaused(true)));
        assert_eq!(parse_command("pause 0"), d(Request::ClockPaused(false)));
        assert_eq!(parse_command("weather 4"), d(Request::Weather(4)));
        assert_eq!(parse_command("weather free"), d(Request::WeatherFree));
        assert_eq!(parse_command("weather freeze"), d(Request::WeatherFreeze));
        assert_eq!(parse_command("weather RAIN"), Ok(Command::WeatherNamed("RAIN".into())));
        assert_eq!(
            parse_command("nudge 0 0 1.5"),
            d(Request::Warp { to: [0.0, 0.0, 1.5], relative: true, frames: 30 })
        );
        assert_eq!(
            parse_command("nudge 1 2 3 5000"),
            d(Request::Warp { to: [1.0, 2.0, 3.0], relative: true, frames: 600 })
        );
        for wrong in ["", "fly", "cheat", "cheat x", "cheat -1", "wanted", "time", "weather", "nudge 1 2", "nudge 1 2 nan"] {
            assert!(parse_command(wrong).is_err(), "{wrong:?}");
        }
    }

    #[test]
    fn a_script_is_times_and_commands() {
        let script = parse_script("90:info; 95.5: time 12 0 0 ;100:weather RAIN;").unwrap();
        assert_eq!(script.len(), 3);
        assert_eq!(script[0], (90.0, Command::Do(Request::Info)));
        assert_eq!(script[1].0, 95.5);
        assert_eq!(script[2].1, Command::WeatherNamed("RAIN".into()));
        assert!(parse_script("").unwrap().is_empty());
        for wrong in ["info", "x:info", "-1:info", "5:fly", "5"] {
            assert!(parse_script(wrong).is_err(), "{wrong:?}");
        }
    }

    #[test]
    fn times_of_day_are_checked() {
        assert!(valid_time(0, 0, 0) && valid_time(23, 59, 59));
        assert!(!valid_time(24, 0, 0) && !valid_time(-1, 0, 0) && !valid_time(0, 60, 0) && !valid_time(0, 0, 60));
        let (hour, minute, second) = local_time();
        assert!(valid_time(hour, minute, second));
    }

    /// The deferred call's three functions as the title's queue uses them: the argument copied into storage
    /// of the queue's, and the work a no-op while no title is there.
    #[test]
    fn the_closure_copies_its_ticket_and_does_nothing_without_a_title() {
        let closure = Closure {
            data: TICKET.as_ptr().cast(),
            call: deferred,
            copy: copy_ticket,
            drop: drop_ticket,
        };
        assert_eq!(std::mem::size_of::<Closure>(), 32);
        TICKET.store(0x1122_3344, Ordering::Relaxed);
        let mut storage = [0u32; 4];
        // SAFETY: our own functions on our own memory, as the queue calls them.
        unsafe {
            (closure.copy)(storage.as_mut_ptr(), closure.data);
            (closure.copy)(std::ptr::null_mut(), closure.data);
            if GAME.get().is_none() {
                (closure.call)(storage.as_mut_ptr());
            }
            (closure.drop)(storage.as_mut_ptr());
        }
        assert_eq!(storage[0], 0x1122_3344);
        assert!(available("player.wanted").is_none() || on());
    }
}
