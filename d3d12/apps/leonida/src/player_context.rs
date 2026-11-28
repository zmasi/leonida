//! What the player is in, on foot or in a vehicle, read from the title and told to the keyboard and mouse layout
//! (`leonida_hle::modules::gameinput`, `kbm`): the right trigger shoots on foot and accelerates in a vehicle, so
//! the layout has two tables and has to know which one counts (`docs/gameinput.md`).
//!
//! A thread of ours reads a few words of the title's memory every 16 ms, through `ReadProcessMemory` on the own
//! process (a page that is gone is a failed read, not a fault). Nothing is written to the title and none of its
//! code is called, so this needs no `LEONIDA_GAME_ACTIONS=1`: it only needs the two globals found, by the
//! patterns of `leonida_loader::game_actions`.
//!
//! What says "in a vehicle", the first of these that there is:
//!
//! - `LEONIDA_PED_VEHICLE_AT=0x<offset>`: a field of the player's ped that points at its vehicle and is null on
//!   foot (for a measured offset; none is known).
//! - `LEONIDA_PLAYER_VEHICLE_AT=0x<offset in the image>`: a global of the title that holds the player's vehicle
//!   and is null on foot. Without the variable this is `Site::PlayerVehicle`, found by its pattern;
//!   `LEONIDA_PLAYER_VEHICLE_AT=off` leaves it unused.
//! - `LEONIDA_KBM_SPEED=1`: how fast the ped moves ([`SpeedRule`]). It only ever says "in a vehicle", and only
//!   once the ped has been faster than anyone runs for a second, so it is off unless asked for: the keys' meaning
//!   would change in the middle of a drive. It always runs as a check of the two above, into the log.
//!
//! With none of them the context stays unknown, and the layout is the on-foot one with Caps Lock as the switch.
//!
//! `LEONIDA_PED_DUMP=<folder>` (outside the repository) is for measuring: F8 in the game's window, or a file named
//! `dump` in that folder (its text is the dump's label; it is removed as it is seen), writes the first 0x4000
//! bytes of the player's ped to `ped-<time>-<label>.bin` there, the first 0x1000 of the vehicle the global names
//! to `vehicle-<time>-<label>.bin`, and a line to `index.txt`. Those are the title's memory: never commit them.

use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use leonida_hle::modules::gameinput::{self, PlayerContext};
use leonida_loader::game_actions::{self as places, Site, Sites};

use crate::player::mem;

/// The thread's step.
const TICK: Duration = Duration::from_millis(16);
/// A change of what is read counts once it was read so many times in a row.
const STEADY_READS: u32 = 3;
/// How much of the ped, and of its vehicle, a dump holds.
const PED_DUMP: usize = 0x4000;
const VEHICLE_DUMP: usize = 0x1000;
const PAGE: usize = 0x1000;
/// The key that asks for a dump: F8.
const VK_DUMP: i32 = 0x77;

/// A number as the switches give it: `0x1a2b` or `6699`.
fn parse_offset(text: &str) -> Option<usize> {
    let text = text.trim();
    match text.strip_prefix("0x").or_else(|| text.strip_prefix("0X")) {
        Some(hex) => usize::from_str_radix(hex, 16).ok(),
        None => text.parse().ok(),
    }
}

/// What the switches ask for.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
struct Settings {
    /// `LEONIDA_PED_VEHICLE_AT`.
    ped_field: Option<usize>,
    /// `LEONIDA_PLAYER_VEHICLE_AT`: an offset in the image, or `off` (`Some(None)`).
    global: Option<Option<usize>>,
    /// `LEONIDA_KBM_SPEED=1`.
    speed: bool,
    /// `LEONIDA_PED_DUMP`.
    dump: Option<PathBuf>,
}

impl Settings {
    fn parse(ped_field: Option<&str>, global: Option<&str>, speed: Option<&str>, dump: Option<&str>) -> Settings {
        Settings {
            ped_field: ped_field.and_then(parse_offset).filter(|at| *at < 0x10_0000),
            global: global.and_then(|text| match text.trim() {
                "off" | "0" => Some(None),
                text => parse_offset(text).map(Some),
            }),
            speed: speed.is_some_and(|v| v.trim() == "1"),
            dump: dump.map(str::trim).filter(|d| !d.is_empty()).map(PathBuf::from),
        }
    }

    fn from_env() -> Settings {
        let var = |name: &str| std::env::var(name).ok();
        Settings::parse(
            var("LEONIDA_PED_VEHICLE_AT").as_deref(),
            var("LEONIDA_PLAYER_VEHICLE_AT").as_deref(),
            var("LEONIDA_KBM_SPEED").as_deref(),
            var("LEONIDA_PED_DUMP").as_deref(),
        )
    }
}

/// What says whether the player is in a vehicle.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Source {
    /// A field of the player's ped that points at its vehicle (offset in the ped).
    PedField(usize),
    /// A global of the title that holds the player's vehicle (offset in the image).
    Global(usize),
    /// How fast the ped moves.
    Speed,
    Nothing,
}

impl Source {
    /// The first there is: the measured field, the global (named, or found unless turned off), the speed.
    fn pick(settings: &Settings, found_global: Option<usize>) -> Source {
        let global = match settings.global {
            Some(named) => named,
            None => found_global,
        };
        match (settings.ped_field, global) {
            (Some(at), _) => Source::PedField(at),
            (None, Some(at)) => Source::Global(at),
            (None, None) if settings.speed => Source::Speed,
            (None, None) => Source::Nothing,
        }
    }

    fn describe(&self) -> String {
        match self {
            Source::PedField(at) => format!("the ped's field at {at:#x} (LEONIDA_PED_VEHICLE_AT)"),
            Source::Global(at) => format!("the title's global of the player's vehicle, at {at:#x} in the image"),
            Source::Speed => "the ped's speed (LEONIDA_KBM_SPEED=1)".into(),
            Source::Nothing => "nothing".into(),
        }
    }
}

/// What a word that should point at the player's vehicle says: null is on foot, an address is in a vehicle, and
/// a read that failed or anything else says nothing.
fn pointer_says(word: Option<u64>) -> PlayerContext {
    match word {
        Some(0) => PlayerContext::OnFoot,
        Some(p) if (0x1_0000..0x8000_0000_0000).contains(&p) => PlayerContext::InVehicle,
        _ => PlayerContext::Unknown,
    }
}

/// A read counts once it is the same [`STEADY_READS`] times in a row: we read while the title writes.
#[derive(Clone, Copy, Debug)]
struct Steady {
    shown: PlayerContext,
    seen: PlayerContext,
    times: u32,
}

impl Default for Steady {
    fn default() -> Steady {
        Steady { shown: PlayerContext::Unknown, seen: PlayerContext::Unknown, times: 0 }
    }
}

impl Steady {
    fn step(&mut self, read: PlayerContext) -> PlayerContext {
        if read == self.shown {
            self.times = 0;
        } else if read == self.seen && self.times > 0 {
            self.times += 1;
            if self.times >= STEADY_READS {
                self.shown = read;
                self.times = 0;
            }
        } else {
            self.seen = read;
            self.times = 1;
        }
        self.shown
    }
}

/// The speed rule: a ped that moves faster than anyone runs is in a vehicle.
///
/// The ped's position is taken about every [`SpeedRule::WINDOW`] and gives a speed over the ground (the title
/// moves the ped 30 times a second, so a shorter step would read 0 and double by turns). At
/// [`SpeedRule::FAST`] or more (12 m/s, 43 km/h; a sprint is about 7 m/s, assuming the world is in metres as the
/// series' are) for [`SpeedRule::PROVEN`] without a break, the ped is driving, and stays so until it has not been
/// that fast for [`SpeedRule::HOLD`].
///
/// Its limits: it never says "on foot" (a car at a red light and a ped standing look the same), so below that
/// speed it says nothing; it cannot see a drive begin, so it never switches to driving from a standstill and the
/// first metres of every drive are on the on-foot keys; a ped carried at that speed without driving (a passenger,
/// a train's roof) counts as driving; a fall is left out (more down than along), and so is a jump of the position
/// (a teleport, a respawn: more than [`SpeedRule::JUMP`]), which also ends a drive.
#[derive(Clone, Copy, Debug, Default)]
struct SpeedRule {
    /// The position the next speed is measured from, and its time (seconds).
    from: Option<([f32; 3], f64)>,
    /// The speed last measured, metres a second over the ground.
    speed: f32,
    /// Since when the ped has been fast without a break, and when it last was.
    fast_since: Option<f64>,
    fast_last: Option<f64>,
    proven: bool,
}

impl SpeedRule {
    const WINDOW: f64 = 0.2;
    const FAST: f32 = 12.0;
    const JUMP: f32 = 120.0;
    const PROVEN: f64 = 1.0;
    const HOLD: f64 = 3.0;

    /// Takes the ped's position at `t` seconds.
    fn step(&mut self, position: [f32; 3], t: f64) {
        let Some((from, at)) = self.from else {
            self.from = Some((position, t));
            return;
        };
        let dt = t - at;
        if dt < Self::WINDOW {
            return;
        }
        self.from = Some((position, t));
        // A gap (the thread or the title stood still): no speed from it.
        if dt > 1.0 {
            self.lost();
            return;
        }
        let along = (position[0] - from[0]).hypot(position[1] - from[1]) / dt as f32;
        let down = (position[2] - from[2]).abs() / dt as f32;
        if along > Self::JUMP || down > Self::JUMP {
            self.lost();
            return;
        }
        self.speed = along;
        if along >= Self::FAST && down <= along {
            self.fast_since.get_or_insert(t);
            self.fast_last = Some(t);
            self.proven |= self.fast_since.is_some_and(|since| t - since >= Self::PROVEN);
        } else if !self.proven {
            self.fast_since = None;
        }
        if self.fast_last.is_some_and(|last| t - last > Self::HOLD) {
            self.lost();
        }
    }

    /// No position (no ped), a gap or a jump: nothing is known any more.
    fn lost(&mut self) {
        *self = SpeedRule { from: self.from, ..Default::default() };
    }

    fn reset(&mut self) {
        *self = SpeedRule::default();
    }

    /// Whether the ped is driving by this rule, at `t`.
    fn driving(&self, t: f64) -> bool {
        self.proven && self.fast_last.is_some_and(|last| t - last <= Self::HOLD)
    }
}

/// Where the title keeps what is read, as addresses in this run.
#[derive(Clone, Copy, Debug)]
struct Reader {
    base: usize,
    /// The global that holds the player's ped.
    ped_at: Option<usize>,
    /// The global that holds the player's vehicle, when one is known (also when it is not the source: dumps
    /// and the log show it).
    vehicle_at: Option<usize>,
    source: Source,
}

impl Reader {
    fn ped(&self) -> Option<usize> {
        let ped = mem::read_u64(self.ped_at?)? as usize;
        (ped > 0x10000).then_some(ped)
    }

    fn vehicle(&self) -> Option<usize> {
        let vehicle = mem::read_u64(self.vehicle_at?)? as usize;
        (vehicle > 0x10000).then_some(vehicle)
    }

    fn position(entity: usize) -> Option<[f32; 3]> {
        let b = mem::read_vec(entity + places::ENTITY_POSITION_AT, 12)?;
        let v: [f32; 3] = std::array::from_fn(|i| f32::from_le_bytes(b[i * 4..i * 4 + 4].try_into().unwrap()));
        v.iter().all(|c| c.is_finite() && c.abs() < 1.0e6).then_some(v)
    }

    /// What the source says now. With no ped in the world (loading, the menus) nothing is known.
    fn context(&self, ped: Option<usize>, speed: &SpeedRule, t: f64) -> PlayerContext {
        if self.ped_at.is_some() && ped.is_none() {
            return PlayerContext::Unknown;
        }
        match self.source {
            Source::PedField(at) => ped.map_or(PlayerContext::Unknown, |ped| pointer_says(mem::read_u64(ped + at))),
            Source::Global(at) => pointer_says(mem::read_u64(self.base + at)),
            Source::Speed if speed.driving(t) => PlayerContext::InVehicle,
            Source::Speed | Source::Nothing => PlayerContext::Unknown,
        }
    }
}

fn say(line: &str) {
    eprintln!("Leonida: player context: {line}");
}

/// The local date and time: year, month, day, hour, minute, second, millisecond.
fn local_time() -> [u16; 7] {
    unsafe extern "system" {
        fn GetLocalTime(time: *mut [u16; 8]);
    }
    let mut t = [0u16; 8];
    // SAFETY: fills the eight words of a SYSTEMTIME.
    unsafe { GetLocalTime(&mut t) };
    // (the third word is the day of the week)
    [t[0], t[1], t[3], t[4], t[5], t[6], t[7]]
}

/// A dump's label as part of a file name: letters, digits, `-` and `_`, at most 32; `dump` when nothing is left.
fn label_of(text: &str) -> String {
    let label: String =
        text.trim().chars().filter(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_').take(32).collect();
    if label.is_empty() { "dump".into() } else { label }
}

/// `len` bytes at `address`, page by page: what does not read is zero, and its offsets are listed.
fn read_pages(address: usize, len: usize) -> (Vec<u8>, Vec<usize>) {
    let mut data = vec![0u8; len];
    let mut unread = Vec::new();
    let mut at = 0;
    while at < len {
        // Up to the end of the page `address + at` lies in.
        let step = (PAGE - (address + at) % PAGE).min(len - at);
        if !mem::read(address + at, &mut data[at..at + step]) {
            unread.push(at);
        }
        at += step;
    }
    (data, unread)
}

/// What is known when a dump is asked for.
struct Moment {
    label: String,
    time: [u16; 7],
    seconds: f64,
    ped: usize,
    vehicle: Option<usize>,
    speed: f32,
    read: PlayerContext,
    shown: PlayerContext,
    source: Source,
}

/// Writes the dump's files into `dir` and its line into `index.txt`; gives the ped's file.
fn write_dump(dir: &Path, m: &Moment) -> std::io::Result<PathBuf> {
    std::fs::create_dir_all(dir)?;
    let [year, month, day, hour, minute, second, ms] = m.time;
    let stamp = format!("{year:04}{month:02}{day:02}-{hour:02}{minute:02}{second:02}-{ms:03}");
    let (bytes, unread) = read_pages(m.ped, PED_DUMP);
    let ped_file = dir.join(format!("ped-{stamp}-{}.bin", m.label));
    std::fs::write(&ped_file, &bytes)?;
    let id_at = places::ENTITY_ID_AT;
    let id = bytes.get(id_at..id_at + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap()));
    let position = Reader::position(m.ped);
    let mut line = format!(
        "{year:04}-{month:02}-{day:02} {hour:02}:{minute:02}:{second:02}.{ms:03}  {}  label {}  at {:.1} s  \
         ped {:#x}  id {:#x}  position {position:?}  speed {:.1} m/s  read {:?}  told {:?}  by {}",
        ped_file.file_name().unwrap_or_default().to_string_lossy(),
        m.label,
        m.seconds,
        m.ped,
        id.unwrap_or(0),
        m.speed,
        m.read,
        m.shown,
        m.source.describe(),
    );
    match m.vehicle {
        Some(vehicle) => {
            let (bytes, _) = read_pages(vehicle, VEHICLE_DUMP);
            let file = format!("vehicle-{stamp}-{}.bin", m.label);
            std::fs::write(dir.join(&file), &bytes)?;
            let away = position.zip(Reader::position(vehicle)).map(|(a, b)| {
                ((a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2) + (a[2] - b[2]).powi(2)).sqrt()
            });
            let away = away.map_or("?".to_owned(), |metres| format!("{metres:.1}"));
            line.push_str(&format!("  vehicle global {vehicle:#x} ({file}), {away} m from the ped"));
        }
        None => line.push_str("  vehicle global null or not known"),
    }
    if !unread.is_empty() {
        line.push_str(&format!("  not readable (zero in the file): pages at {unread:x?}"));
    }
    let mut index = std::fs::OpenOptions::new().create(true).append(true).open(dir.join("index.txt"))?;
    writeln!(index, "{line}")?;
    Ok(ped_file)
}

/// Whether a dump is asked for now: its label.
struct Trigger {
    dir: PathBuf,
    key_was: bool,
}

impl Trigger {
    fn asked(&mut self) -> Option<String> {
        #[link(name = "user32")]
        unsafe extern "system" {
            fn GetAsyncKeyState(key: i32) -> i16;
            fn GetForegroundWindow() -> isize;
            fn GetWindowThreadProcessId(window: isize, process: *mut u32) -> u32;
        }
        unsafe extern "system" {
            fn GetCurrentProcessId() -> u32;
        }
        let mut owner = 0u32;
        // SAFETY: plain queries; a null window gives process 0.
        let down = unsafe {
            GetWindowThreadProcessId(GetForegroundWindow(), &mut owner);
            owner == GetCurrentProcessId() && GetAsyncKeyState(VK_DUMP) < 0
        };
        let pressed = down && !std::mem::replace(&mut self.key_was, down);
        if !down {
            self.key_was = false;
        }
        if pressed {
            return Some("key".into());
        }
        let file = self.dir.join("dump");
        if !file.exists() {
            return None;
        }
        let label = label_of(&std::fs::read_to_string(&file).unwrap_or_default());
        // One file asks once; a file that cannot be removed would ask for ever.
        std::fs::remove_file(&file).is_ok().then_some(label)
    }
}

fn run(reader: Reader, tells: bool, dump: Option<PathBuf>) {
    let start = Instant::now();
    let mut steady = Steady::default();
    let mut speed = SpeedRule::default();
    let mut trigger = dump.map(|dir| Trigger { dir, key_was: false });
    let mut told = PlayerContext::Unknown;
    // When each check last spoke (seconds): at most once in half a minute.
    let (mut fast_on_foot, mut far_vehicle) = (f64::MIN, f64::MIN);
    let mut tick = 0u64;
    loop {
        std::thread::sleep(TICK);
        tick += 1;
        let t = start.elapsed().as_secs_f64();
        let ped = reader.ped();
        if tick.is_multiple_of(4) {
            match ped.and_then(Reader::position) {
                Some(position) => speed.step(position, t),
                None => speed.reset(),
            }
        }
        let read = reader.context(ped, &speed, t);
        let shown = steady.step(read);
        if tells {
            gameinput::set_player_context(shown);
        }
        if shown != told {
            told = shown;
            say(&format!("{shown:?} at {t:.2} s (speed {:.1} m/s)", speed.speed));
        }
        // The speed as a check of what is read: a ped this fast is not on foot.
        if shown == PlayerContext::OnFoot && speed.driving(t) && t - fast_on_foot > 30.0 {
            fast_on_foot = t;
            say(&format!(
                "on foot by {}, but the ped has moved at {:.0} m/s for over a second: what is read may be wrong",
                reader.source.describe(),
                speed.speed
            ));
        }
        if tick.is_multiple_of(16) {
            // And where the vehicle is: one that stays behind when the ped walks off is not the one it is in.
            if shown == PlayerContext::InVehicle
                && t - far_vehicle > 30.0
                && let Some(away) = ped
                    .and_then(Reader::position)
                    .zip(reader.vehicle().and_then(Reader::position))
                    .map(|(a, b)| (a[0] - b[0]).hypot(a[1] - b[1]))
                && away > 60.0
            {
                far_vehicle = t;
                say(&format!("in a vehicle by what is read, but that vehicle is {away:.0} m from the ped"));
            }
            if let Some(trigger) = trigger.as_mut()
                && let Some(label) = trigger.asked()
            {
                match ped {
                    Some(ped) => {
                        let moment = Moment {
                            label,
                            time: local_time(),
                            seconds: t,
                            ped,
                            vehicle: reader.vehicle(),
                            speed: speed.speed,
                            read,
                            shown,
                            source: reader.source,
                        };
                        match write_dump(&trigger.dir, &moment) {
                            Ok(file) => say(&format!("the ped is dumped to {}", file.display())),
                            Err(why) => say(&format!("the dump could not be written: {why}")),
                        }
                    }
                    None => say("a dump was asked for, but there is no player ped now"),
                }
            }
        }
    }
}

/// Starts the reading thread, when the keyboard and mouse layout follows the player's context or dumps are asked
/// for. `found` is what the game actions found already (`LEONIDA_GAME_ACTIONS=1`); without it the two globals are
/// looked for here, which reads the image's code twice over and changes nothing.
///
/// # Safety
/// `base` is the title's executable `info` describes, mapped, and none of its code runs yet.
pub unsafe fn install(base: usize, info: &leonida_pe::ImageInfo, found: Option<&Sites>) {
    let settings = Settings::from_env();
    let tells = gameinput::player_context_wanted();
    let dump = settings.dump.clone().filter(|dir| match leonida_core::ensure_outside_repo(dir) {
        Ok(()) => true,
        Err(why) => {
            println!("player context LEONIDA_PED_DUMP not used: {why}");
            false
        }
    });
    if !tells && dump.is_none() {
        return;
    }
    let began = Instant::now();
    let sites = match found {
        Some(sites) => sites.clone(),
        // SAFETY: forwarded from this function's contract.
        None => unsafe { places::find(base, info, Some(&[Site::PlayerPed, Site::PlayerVehicle])) },
    };
    let looked = began.elapsed();
    if found.is_none() {
        for line in &sites.missing {
            println!("player context not found: {line}");
        }
    }
    let found_global = sites.get(Site::PlayerVehicle);
    let source = match Source::pick(&settings, found_global) {
        _ if !tells => Source::Nothing,
        Source::Global(at) if at + 8 > info.size_of_image as usize => {
            println!("player context LEONIDA_PLAYER_VEHICLE_AT={at:#x} is not in the image: not used");
            Source::Nothing
        }
        source => source,
    };
    let named = match settings.global {
        Some(named) => named,
        None => found_global,
    };
    let reader = Reader {
        base,
        ped_at: sites.get(Site::PlayerPed).map(|rva| base + rva),
        vehicle_at: named.filter(|rva| *rva < info.size_of_image as usize).map(|rva| base + rva),
        source,
    };
    if tells && source == Source::Nothing {
        println!("player context not read: the on-foot keys, and Caps Lock switches to the driving keys by hand");
    } else if tells {
        println!(
            "player context read from {} ({} ms to find it): W accelerates and the mouse shoots in a vehicle",
            source.describe(),
            looked.as_millis()
        );
    }
    if let Some(dir) = &dump {
        println!("player context F8, or a file named dump in {}, dumps the player's ped there", dir.display());
    }
    if source == Source::Nothing && dump.is_none() {
        return;
    }
    let thread = std::thread::Builder::new().name("leonida-player-context".into());
    if thread.spawn(move || run(reader, tells, dump)).is_err() {
        println!("player context the reading thread could not be started");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const FOOT: PlayerContext = PlayerContext::OnFoot;
    const CAR: PlayerContext = PlayerContext::InVehicle;
    const UNKNOWN: PlayerContext = PlayerContext::Unknown;

    #[test]
    fn offsets_and_switches_read() {
        assert_eq!(parse_offset("0x1a2B"), Some(0x1a2b));
        assert_eq!(parse_offset(" 0X10 "), Some(16));
        assert_eq!(parse_offset("4096"), Some(4096));
        for wrong in ["", "0x", "xyz", "-1", "0x1g"] {
            assert_eq!(parse_offset(wrong), None, "{wrong:?}");
        }
        assert_eq!(Settings::parse(None, None, None, None), Settings::default());
        let s = Settings::parse(Some("0xd10"), Some("0xab760c8"), Some("1"), Some(r" D:\dumps "));
        assert_eq!(s.ped_field, Some(0xd10));
        assert_eq!(s.global, Some(Some(0xab7_60c8)));
        assert!(s.speed);
        assert_eq!(s.dump, Some(PathBuf::from(r"D:\dumps")));
        // An offset in a ped is small; `off` turns the global off; what does not read is as unset.
        assert_eq!(Settings::parse(Some("0x7ff600001000"), None, None, None).ped_field, None);
        assert_eq!(Settings::parse(None, Some("off"), Some("0"), Some(" ")).global, Some(None));
        assert_eq!(Settings::parse(None, Some("what"), Some("yes"), None), Settings::default());
    }

    #[test]
    fn the_source_is_the_first_there_is() {
        let none = Settings::default();
        assert_eq!(Source::pick(&none, None), Source::Nothing);
        assert_eq!(Source::pick(&none, Some(0x500)), Source::Global(0x500));
        let field = Settings { ped_field: Some(0x10), ..Default::default() };
        assert_eq!(Source::pick(&field, Some(0x500)), Source::PedField(0x10));
        let named = Settings { global: Some(Some(0x700)), speed: true, ..Default::default() };
        assert_eq!(Source::pick(&named, Some(0x500)), Source::Global(0x700));
        assert_eq!(Source::pick(&named, None), Source::Global(0x700));
        let off = Settings { global: Some(None), ..Default::default() };
        assert_eq!(Source::pick(&off, Some(0x500)), Source::Nothing);
        let speed = Settings { global: Some(None), speed: true, ..Default::default() };
        assert_eq!(Source::pick(&speed, Some(0x500)), Source::Speed);
        for source in [Source::Nothing, Source::Speed, Source::Global(1), Source::PedField(2)] {
            assert!(!source.describe().is_empty());
        }
    }

    #[test]
    fn a_pointer_says_where_the_player_is() {
        assert_eq!(pointer_says(Some(0)), FOOT);
        assert_eq!(pointer_says(Some(0x1_4a3f_2c80)), CAR);
        // No read, or a word that is no address: nothing.
        assert_eq!(pointer_says(None), UNKNOWN);
        assert_eq!(pointer_says(Some(7)), UNKNOWN);
        assert_eq!(pointer_says(Some(u64::MAX)), UNKNOWN);
    }

    #[test]
    fn a_read_counts_after_three_in_a_row() {
        let mut s = Steady::default();
        assert_eq!(s.step(UNKNOWN), UNKNOWN);
        assert_eq!(s.step(FOOT), UNKNOWN);
        assert_eq!(s.step(FOOT), UNKNOWN);
        assert_eq!(s.step(FOOT), FOOT);
        // One odd read (the title was writing) changes nothing, nor do two.
        assert_eq!(s.step(CAR), FOOT);
        assert_eq!(s.step(FOOT), FOOT);
        assert_eq!(s.step(CAR), FOOT);
        assert_eq!(s.step(CAR), FOOT);
        assert_eq!(s.step(UNKNOWN), FOOT);
        assert_eq!(s.step(CAR), FOOT);
        assert_eq!(s.step(CAR), FOOT);
        assert_eq!(s.step(CAR), CAR);
        assert_eq!(s.step(CAR), CAR);
    }

    /// A ped that moves `speed` metres a second along x (and `fall` down) from `t0` to `t1`, sampled every 64 ms.
    fn travel(rule: &mut SpeedRule, at: &mut [f32; 3], t0: f64, t1: f64, speed: f32, fall: f32) {
        let mut t = t0;
        while t < t1 {
            at[0] += speed * 0.064;
            at[2] -= fall * 0.064;
            t += 0.064;
            rule.step(*at, t);
        }
    }

    #[test]
    fn the_speed_rule_never_drives_from_a_standstill() {
        let mut rule = SpeedRule::default();
        let mut at = [100.0, 200.0, 30.0];
        // Standing, walking, sprinting: never driving, however long.
        for speed in [0.0, 1.5, 7.5, 11.0] {
            travel(&mut rule, &mut at, 0.0, 30.0, speed, 0.0);
            assert!(!rule.driving(30.0), "{speed} m/s");
            assert!((rule.speed - speed).abs() < 0.5, "{} for {speed}", rule.speed);
            rule.reset();
        }
        // Fast for less than a second (thrown by a car): not driving.
        travel(&mut rule, &mut at, 0.0, 0.7, 25.0, 0.0);
        travel(&mut rule, &mut at, 0.7, 5.0, 0.0, 0.0);
        assert!(!rule.driving(5.0));
        // A fall: fast, but more down than along.
        rule.reset();
        travel(&mut rule, &mut at, 0.0, 4.0, 14.0, 40.0);
        assert!(!rule.driving(4.0));
        // A teleport: one jump, then standing.
        rule.reset();
        travel(&mut rule, &mut at, 0.0, 1.0, 0.0, 0.0);
        at[0] += 2000.0;
        travel(&mut rule, &mut at, 1.0, 5.0, 0.0, 0.0);
        assert!(!rule.driving(5.0));
    }

    #[test]
    fn the_speed_rule_sees_a_drive_once_it_is_fast_and_lets_go_after_it() {
        let mut rule = SpeedRule::default();
        let mut at = [0.0, 0.0, 0.0];
        // Pulling away: not yet. At 20 m/s for over a second: driving.
        travel(&mut rule, &mut at, 0.0, 2.0, 6.0, 0.0);
        assert!(!rule.driving(2.0));
        travel(&mut rule, &mut at, 2.0, 2.8, 20.0, 0.0);
        assert!(!rule.driving(2.8), "not yet a second");
        travel(&mut rule, &mut at, 2.8, 4.0, 20.0, 0.0);
        assert!(rule.driving(4.0));
        // Slowing for a corner keeps it; three seconds below the speed end it.
        travel(&mut rule, &mut at, 4.0, 6.0, 5.0, 0.0);
        assert!(rule.driving(6.0));
        travel(&mut rule, &mut at, 6.0, 8.0, 20.0, 0.0);
        travel(&mut rule, &mut at, 8.0, 12.0, 0.0, 0.0);
        assert!(!rule.driving(12.0));
        // And a new drive has to prove itself again.
        travel(&mut rule, &mut at, 12.0, 12.6, 20.0, 0.0);
        assert!(!rule.driving(12.6));
        // A respawn in the middle of a drive ends it.
        travel(&mut rule, &mut at, 12.6, 15.0, 20.0, 0.0);
        assert!(rule.driving(15.0));
        at[1] += 900.0;
        travel(&mut rule, &mut at, 15.0, 15.5, 0.0, 0.0);
        assert!(!rule.driving(15.5));
        // No ped: nothing is known.
        travel(&mut rule, &mut at, 15.5, 18.0, 20.0, 0.0);
        assert!(rule.driving(18.0));
        rule.reset();
        assert!(!rule.driving(18.0));
    }

    #[test]
    fn a_reader_reads_this_process() {
        // A made-up ped and vehicle in our own memory, and the two globals that point at them.
        let mut ped = vec![0u8; PED_DUMP];
        let mut vehicle = vec![0u8; VEHICLE_DUMP];
        for (entity, x) in [(&mut ped, 10.0f32), (&mut vehicle, 13.0)] {
            for (i, v) in [x, 20.0, 5.0].iter().enumerate() {
                let at = places::ENTITY_POSITION_AT + i * 4;
                entity[at..at + 4].copy_from_slice(&v.to_le_bytes());
            }
        }
        ped[places::ENTITY_ID_AT..places::ENTITY_ID_AT + 4].copy_from_slice(&0x6_0123u32.to_le_bytes());
        let field = 0x898;
        let mut globals: Box<[u64; 2]> = Box::new([ped.as_ptr() as u64, 0]);
        let base = globals.as_mut_ptr() as usize;
        let reader = |source| Reader { base, ped_at: Some(base), vehicle_at: Some(base + 8), source };
        let speed = SpeedRule::default();
        let r = reader(Source::Global(8));
        assert_eq!(r.ped(), Some(ped.as_ptr() as usize));
        assert_eq!(Reader::position(r.ped().unwrap()), Some([10.0, 20.0, 5.0]));
        // On foot: the global is null, and so is the ped's field.
        assert_eq!(r.context(r.ped(), &speed, 0.0), FOOT);
        assert_eq!(reader(Source::PedField(field)).context(r.ped(), &speed, 0.0), FOOT);
        assert_eq!(reader(Source::Speed).context(r.ped(), &speed, 0.0), UNKNOWN);
        assert_eq!(reader(Source::Nothing).context(r.ped(), &speed, 0.0), UNKNOWN);
        // In a vehicle.
        globals[1] = vehicle.as_ptr() as u64;
        ped[field..field + 8].copy_from_slice(&(vehicle.as_ptr() as u64).to_le_bytes());
        assert_eq!(r.context(r.ped(), &speed, 0.0), CAR);
        assert_eq!(r.vehicle(), Some(vehicle.as_ptr() as usize));
        assert_eq!(reader(Source::PedField(field)).context(r.ped(), &speed, 0.0), CAR);
        // No ped in the world: unknown, whatever the global holds.
        assert_eq!(r.context(None, &speed, 0.0), UNKNOWN);
        // A global that cannot be read: unknown.
        let gone = Reader { base: 0x10, ped_at: None, vehicle_at: None, source: Source::Global(8) };
        assert_eq!(gone.context(None, &speed, 0.0), UNKNOWN);

        // The dump: the ped's bytes, the vehicle's, and a line that says what was read.
        let dir = std::env::temp_dir().join(format!("leonida-ped-dump-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let moment = Moment {
            label: label_of(" in a car! "),
            time: [2026, 11, 27, 21, 4, 5, 6],
            seconds: 12.5,
            ped: r.ped().unwrap(),
            vehicle: r.vehicle(),
            speed: 3.0,
            read: CAR,
            shown: CAR,
            source: r.source,
        };
        let file = write_dump(&dir, &moment).unwrap();
        assert_eq!(file.file_name().unwrap(), "ped-20261127-210405-006-inacar.bin");
        assert_eq!(std::fs::read(&file).unwrap(), ped);
        assert_eq!(std::fs::read(dir.join("vehicle-20261127-210405-006-inacar.bin")).unwrap(), vehicle);
        let index = std::fs::read_to_string(dir.join("index.txt")).unwrap();
        assert!(index.contains("2026-11-27 21:04:05.006") && index.contains("label inacar"), "{index}");
        assert!(index.contains("id 0x60123"), "{index}");
        assert!(index.contains("3.0 m from the ped") && index.contains("read InVehicle"), "{index}");
        assert!(!index.contains("not readable"), "{index}");
        // A file named `dump` asks once, and its text is the label.
        let mut trigger = Trigger { dir: dir.clone(), key_was: false };
        assert_eq!(trigger.asked(), None);
        std::fs::write(dir.join("dump"), "foot\n").unwrap();
        assert_eq!(trigger.asked().as_deref(), Some("foot"));
        assert_eq!(trigger.asked(), None);
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn what_does_not_read_is_zero_in_a_dump() {
        // Two pages of ours, then whatever follows a low address nobody has.
        let ours = vec![0xA5u8; 2 * PAGE];
        let (data, unread) = read_pages(ours.as_ptr() as usize, ours.len());
        assert!(data == ours && unread.is_empty());
        let (data, unread) = read_pages(0x1800, 0x1000);
        assert!(data.iter().all(|b| *b == 0));
        assert_eq!(unread, [0, 0x800]);
        assert_eq!(label_of(""), "dump");
        assert_eq!(label_of("on_foot-2"), "on_foot-2");
        assert_eq!(label_of(r"..\..\x"), "x");
        assert_eq!(label_of(&"a".repeat(80)).len(), 32);
    }
}
