//! The runtime driven without a title: a fake host, mods written to a
//! temporary folder, passes called by hand.

use std::cell::{Cell, RefCell};
use std::collections::HashSet;
use std::rc::Rc;
use std::time::{Duration, Instant};

use leonida_menu::model::Value as MenuValue;
use leonida_menu::mods::{Event, ModKind, ModPage};
use leonida_menu::overlay::Cmd;

use crate::input::Pad;
use crate::platform::Platform;
use crate::runtime::{Config, Runtime, Status};
use crate::sandbox::Budget;

/// A host that records what the mods do and holds a buffer of "title" memory.
struct Fake {
    keys: RefCell<HashSet<u8>>,
    pad: Cell<Pad>,
    menu_open: Cell<bool>,
    overlay: RefCell<Vec<Vec<Cmd>>>,
    injected: RefCell<Vec<Vec<u8>>>,
    pages: RefCell<Vec<Vec<ModPage>>>,
    values: RefCell<Vec<(usize, usize, MenuValue)>>,
    events: RefCell<Vec<Event>>,
    logs: RefCell<Vec<String>>,
    /// The buffer standing for the title's image; its last 16 bytes are
    /// read-only.
    memory: RefCell<Vec<u8>>,
    base: Cell<usize>,
}

const READ_ONLY: usize = 16;

impl Fake {
    fn new() -> Rc<Fake> {
        let memory = vec![0u8; 4096];
        let base = memory.as_ptr() as usize;
        Rc::new(Fake {
            keys: RefCell::default(),
            pad: Cell::new(Pad::default()),
            menu_open: Cell::new(false),
            overlay: RefCell::default(),
            injected: RefCell::default(),
            pages: RefCell::default(),
            values: RefCell::default(),
            events: RefCell::default(),
            logs: RefCell::default(),
            memory: RefCell::new(memory),
            base: Cell::new(base),
        })
    }

    fn logs(&self) -> Vec<String> {
        self.logs.borrow().clone()
    }

    fn last_log(&self) -> String {
        self.logs.borrow().last().cloned().unwrap_or_default()
    }

    /// The mods' log lines of mod `name` only.
    fn of(&self, name: &str) -> Vec<String> {
        let prefix = format!("[{name}] ");
        self.logs
            .borrow()
            .iter()
            .filter_map(|l| l.strip_prefix(&prefix).map(str::to_owned))
            .collect()
    }
}

impl Platform for Fake {
    fn key_down(&self, vk: u8) -> bool {
        self.keys.borrow().contains(&vk)
    }
    fn pad(&self) -> Pad {
        self.pad.get()
    }
    fn menu_open(&self) -> bool {
        self.menu_open.get()
    }
    fn publish_overlay(&self, cmds: &[Cmd]) {
        self.overlay.borrow_mut().push(cmds.to_vec());
    }
    fn inject_keys(&self, keys: &[u8]) {
        self.injected.borrow_mut().push(keys.to_vec());
    }
    fn set_menu_pages(&self, pages: Vec<ModPage>) {
        self.pages.borrow_mut().push(pages);
    }
    fn set_menu_value(&self, page: usize, item: usize, value: MenuValue) {
        self.values.borrow_mut().push((page, item, value));
    }
    fn menu_events(&self) -> Vec<Event> {
        std::mem::take(&mut *self.events.borrow_mut())
    }
    fn take_reload(&self) -> bool {
        false
    }
    fn screenshot(&self) -> Result<String, String> {
        Ok("shot.png".into())
    }
    fn log(&self, mod_name: &str, line: &str) {
        self.logs.borrow_mut().push(format!("[{mod_name}] {line}"));
    }
    fn read(&self, address: usize, out: &mut [u8]) -> bool {
        let mem = self.memory.borrow();
        let base = self.base.get();
        if address < base || address + out.len() > base + mem.len() {
            return false;
        }
        // SAFETY: inside the buffer, which the fake owns.
        unsafe {
            std::ptr::copy_nonoverlapping(
                mem.as_ptr().add(address - base),
                out.as_mut_ptr(),
                out.len(),
            )
        };
        true
    }
    fn write(&self, address: usize, data: &[u8]) -> bool {
        let mut mem = self.memory.borrow_mut();
        let base = self.base.get();
        let len = mem.len();
        if address < base || address + data.len() > base + len - READ_ONLY {
            return false;
        }
        // SAFETY: inside the buffer, which the fake owns.
        unsafe {
            std::ptr::copy_nonoverlapping(
                data.as_ptr(),
                mem.as_mut_ptr().add(address - base),
                data.len(),
            )
        };
        true
    }
    fn image(&self) -> Option<(usize, usize)> {
        Some((self.base.get(), self.memory.borrow().len()))
    }
}

/// A mods folder with these mods (`(folder, mod.json text, main.lua text)`).
fn folder(mods: &[(&str, &str, &str)]) -> tempfile::TempDir {
    let dir = tempfile::tempdir().unwrap();
    for (name, json, code) in mods {
        let at = dir.path().join(name);
        std::fs::create_dir_all(&at).unwrap();
        std::fs::write(at.join("mod.json"), json).unwrap();
        std::fs::write(at.join("main.lua"), code).unwrap();
    }
    dir
}

const JSON: &str = r#"{"script": "main.lua"}"#;

fn rig(mods: &[(&str, &str)]) -> (tempfile::TempDir, Rc<Fake>, Runtime) {
    rig_with(mods, Config::default())
}

fn rig_with(mods: &[(&str, &str)], config: Config) -> (tempfile::TempDir, Rc<Fake>, Runtime) {
    let all: Vec<(&str, &str, &str)> = mods.iter().map(|(n, c)| (*n, JSON, *c)).collect();
    let dir = folder(&all);
    let fake = Fake::new();
    let mut rt = Runtime::new(fake.clone(), config).unwrap();
    rt.load_dir(dir.path());
    (dir, fake, rt)
}

fn pass(rt: &mut Runtime, frame: u64) {
    rt.pass(frame, Instant::now());
}

fn status(rt: &Runtime, name: &str) -> Status {
    rt.mods()
        .into_iter()
        .find(|(n, _)| n == name)
        .unwrap_or_else(|| panic!("no mod {name}: {:?}", rt.mods()))
        .1
}

#[test]
fn the_api_version_is_one() {
    assert_eq!(crate::API_VERSION, 1);
    let (_d, fake, _rt) = rig(&[(
        "a",
        "leonida.log('v', leonida.api_version, type(leonida.version))",
    )]);
    assert_eq!(fake.of("a"), ["v 1 string"]);
}

#[test]
fn what_is_forbidden_is_gone_and_what_is_safe_is_there() {
    let code = r#"
        local gone = {}
        local function chk(name, v) if v ~= nil then gone[#gone + 1] = name end end
        chk("io", io); chk("debug", debug); chk("package", package)
        chk("dofile", dofile); chk("loadfile", loadfile); chk("string.dump", string.dump)
        chk("os.execute", os.execute); chk("os.remove", os.remove); chk("os.rename", os.rename)
        chk("os.exit", os.exit); chk("os.getenv", os.getenv); chk("os.tmpname", os.tmpname)
        chk("os.setlocale", os.setlocale); chk("socket", socket); chk("require_c", package and package.loadlib)
        chk("loadlib", loadlib); chk("newproxy", newproxy); chk("getfenv", getfenv)
        leonida.log("gone:" .. table.concat(gone, ","))
        local there = {}
        for _, f in ipairs({ string.rep, math.floor, os.time, os.clock, os.date, os.difftime, table.insert,
                             utf8.char, coroutine.create, pcall, xpcall, select, setmetatable, tostring,
                             load, require, print, collectgarbage }) do
            there[#there + 1] = type(f)
        end
        leonida.log("there:" .. table.concat(there, ","))
        -- A precompiled chunk is refused; a text one runs, in this mod's environment.
        local f, e = load("\27Lua\84\0")
        leonida.log("binary:", f == nil, e ~= nil)
        leonida.log("text:", load("return 1 + 1")())
        Z = 5
        leonida.log("env:", load("return Z")(), _G.Z, rawget(_G, "leonida") ~= nil)
        -- No way to a C module, and no file outside the folder through require.
        leonida.log("req1:", pcall(require, "socket"))
        leonida.log("req2:", pcall(require, "../x"))
        leonida.log("gc:", pcall(collectgarbage, "stop"))
        leonida.log("gc2:", type(collectgarbage("count")))
    "#;
    let (_d, fake, _rt) = rig(&[("a", code)]);
    let log = fake.of("a");
    assert_eq!(log[0], "gone:", "{log:?}");
    assert_eq!(log[1], format!("there:{}", ["function"; 18].join(",")));
    assert_eq!(log[2], "binary: true true");
    assert_eq!(log[3], "text: 2");
    assert_eq!(log[4], "env: 5 5 true");
    assert!(
        log[5].starts_with("req1: false ") && log[5].contains("not found"),
        "{}",
        log[5]
    );
    assert!(
        log[6].starts_with("req2: false ") && log[6].contains("bad module name"),
        "{}",
        log[6]
    );
    assert!(log[7].starts_with("gc: false "), "{}", log[7]);
    assert_eq!(log[8], "gc2: number");
}

#[test]
fn a_mod_cannot_change_what_another_mod_sees() {
    let a = r#"
        string.rep = nil
        math.floor = function() return "mine" end
        getmetatable("").__index.shout = function() return "A" end
        Shared = 1
    "#;
    let b = r#"
        leonida.log(type(string.rep), math.floor(1.5), ("x").shout, Shared, type(getmetatable("").__index.rep))
    "#;
    let (_d, fake, _rt) = rig(&[("a", a), ("b", b)]);
    assert_eq!(fake.of("b"), ["function 1 nil nil function"]);
}

#[test]
fn frame_key_and_menu_events_reach_the_handlers_in_order() {
    let a = r#"
        local frames = 0
        leonida.on("frame", function(dt) frames = frames + 1
            leonida.log("A frame", frames, leonida.frame(), type(dt), dt >= 0) end)
        leonida.on("key", function(key, down) leonida.log("A key", key, down) end)
        leonida.on("menu_open", function() leonida.log("A menu") end)
    "#;
    let b = r#"leonida.on("frame", function() leonida.log("B frame") end)"#;
    let (_d, fake, mut rt) = rig(&[("a", a), ("b", b)]);
    // No presents yet: no frame handlers.
    pass(&mut rt, 0);
    assert!(fake.logs().is_empty());
    pass(&mut rt, 1);
    assert_eq!(fake.logs(), ["[a] A frame 1 1 number true", "[b] B frame"]);
    // The same frame again: the frame handlers wait for a present.
    pass(&mut rt, 1);
    assert_eq!(fake.logs().len(), 2);
    pass(&mut rt, 5);
    assert_eq!(fake.last_log(), "[b] B frame");
    assert_eq!(fake.of("a").last().unwrap(), "A frame 2 5 number true");
    // Keys: one event for the press and one for the release, named.
    fake.logs.borrow_mut().clear();
    fake.keys.borrow_mut().extend([b'W', 0x73, 0x10]);
    pass(&mut rt, 5);
    let mut keys = fake.of("a");
    keys.sort();
    assert_eq!(keys, ["A key F4 true", "A key Shift true", "A key W true"]);
    pass(&mut rt, 5);
    assert_eq!(fake.of("a").len(), 3, "a held key is one event");
    fake.keys.borrow_mut().remove(&b'W');
    pass(&mut rt, 5);
    assert_eq!(fake.of("a").last().unwrap(), "A key W false");
    // The menu opening is one event, when it opens.
    fake.menu_open.set(true);
    pass(&mut rt, 5);
    pass(&mut rt, 5);
    assert_eq!(fake.of("a").iter().filter(|l| *l == "A menu").count(), 1);
    fake.menu_open.set(false);
    pass(&mut rt, 5);
    fake.menu_open.set(true);
    pass(&mut rt, 5);
    assert_eq!(fake.of("a").iter().filter(|l| *l == "A menu").count(), 2);
}

#[test]
fn input_is_read_by_name() {
    let code = r#"
        leonida.log("keys", leonida.key_down("w"), leonida.key_down("F5"), leonida.key_down("Space"))
        local p = leonida.pad()
        leonida.log("pad", p.connected, p.a, p.b, p.rb, p.lt, p.lx, p.ly)
        leonida.log("bad", pcall(leonida.key_down, "NotAKey"))
        leonida.log("badev", pcall(leonida.on, "tick", print))
    "#;
    let dir = folder(&[("a", JSON, code)]);
    let fake = Fake::new();
    fake.keys.borrow_mut().insert(b'W');
    fake.pad.set(Pad {
        connected: true,
        buttons: 0x1000 | 0x0200,
        lt: 0.5,
        lx: -1.0,
        ..Pad::default()
    });
    let mut rt = Runtime::new(fake.clone(), Config::default()).unwrap();
    rt.load_dir(dir.path());
    let log = fake.of("a");
    assert_eq!(log[0], "keys true false false");
    assert_eq!(log[1], "pad true true false true 0.5 -1.0 0.0");
    assert!(log[2].starts_with("bad false ") && log[2].contains("unknown key"));
    assert!(log[3].starts_with("badev false ") && log[3].contains("unknown event"));
}

#[test]
fn a_mod_that_fails_is_switched_off_and_the_others_go_on() {
    let bad = r#"
        local n = 0
        leonida.on("frame", function() n = n + 1; if n == 2 then error("boom") end end)
        local page = leonida.menu.page("Bad page")
        page:action("Go", function() end)
        leonida.hold_keys({"W"})
    "#;
    let good = r#"
        local n = 0
        leonida.on("frame", function() n = n + 1; leonida.log("good", n) end)
        leonida.menu.page("Good page"):toggle("On", true, function() end)
        leonida.hold_keys({"A"})
    "#;
    let (_d, fake, mut rt) = rig(&[("a_bad", bad), ("b_good", good)]);
    assert_eq!(status(&rt, "a_bad"), Status::Running);
    // Both loaded: two pages, both keys held.
    assert_eq!(fake.pages.borrow().last().unwrap().len(), 2);
    assert_eq!(fake.injected.borrow().last().unwrap(), &[b'A', b'W']);
    for frame in 1..=3 {
        pass(&mut rt, frame);
    }
    match status(&rt, "a_bad") {
        Status::Failed(why) => assert!(why.contains("boom"), "{why}"),
        other => panic!("{other:?}"),
    }
    assert_eq!(status(&rt, "b_good"), Status::Running);
    assert_eq!(fake.of("b_good"), ["good 1", "good 2", "good 3"]);
    // Its page and its held key went with it; the other's stay.
    let pages = fake.pages.borrow().last().unwrap().clone();
    assert_eq!(pages.len(), 1);
    assert_eq!(pages[0].title, "Good page");
    assert_eq!(fake.injected.borrow().last().unwrap(), &[b'A']);
    assert!(
        fake.of("a_bad")
            .iter()
            .any(|l| l.contains("disabled") && l.contains("boom"))
    );
    // It does not run again.
    pass(&mut rt, 4);
    assert_eq!(
        fake.of("a_bad")
            .iter()
            .filter(|l| l.contains("boom"))
            .count(),
        1
    );
}

#[test]
fn errors_at_load_and_odd_errors_disable_only_their_mod() {
    let (_d, fake, mut rt) = rig(&[
        ("a_syntax", "this is not lua"),
        ("b_runtime", "local x = nil; x.y = 1"),
        ("c_table", "error({code = 7})"),
        ("d_event", "leonida.on('nope', function() end)"),
        (
            "e_ok",
            "leonida.on('frame', function() leonida.log('ok') end)",
        ),
        ("f_bad_arg", "leonida.rect('x')"),
        (
            "g_memory",
            "local t = {} for i = 1, 1e9 do t[i] = ('x'):rep(1000) .. i end",
        ),
    ]);
    for name in [
        "a_syntax",
        "b_runtime",
        "c_table",
        "d_event",
        "f_bad_arg",
        "g_memory",
    ] {
        assert!(
            matches!(status(&rt, name), Status::Failed(_)),
            "{name}: {:?}",
            status(&rt, name)
        );
    }
    assert_eq!(status(&rt, "e_ok"), Status::Running);
    pass(&mut rt, 1);
    assert_eq!(fake.of("e_ok"), ["ok"]);
}

#[test]
fn a_loop_that_never_ends_runs_out_of_time_even_inside_pcall() {
    let config = Config {
        budget: Budget {
            call: Duration::from_millis(60),
            load: Duration::from_millis(60),
        },
    };
    let (_d, fake, mut rt) = rig_with(
        &[
            ("a_load", "while true do end"),
            (
                "b_frame",
                "leonida.on('frame', function() while true do end end)",
            ),
            (
                "c_pcall",
                "leonida.on('frame', function() while true do pcall(function() while true do end end) end end)",
            ),
            (
                "d_coroutine",
                "leonida.on('frame', function() while true do coroutine.resume(coroutine.create(function() while true do end end)) end end)",
            ),
            (
                "e_ok",
                "leonida.on('frame', function() leonida.log('ok') end)",
            ),
        ],
        config,
    );
    assert!(matches!(status(&rt, "a_load"), Status::Failed(w) if w.contains("time budget")));
    let began = Instant::now();
    pass(&mut rt, 1);
    assert!(
        began.elapsed() < Duration::from_secs(5),
        "{:?}",
        began.elapsed()
    );
    for name in ["b_frame", "c_pcall", "d_coroutine"] {
        assert!(
            matches!(status(&rt, name), Status::Failed(w) if w.contains("time budget")),
            "{name}"
        );
    }
    assert_eq!(fake.of("e_ok"), ["ok"]);
    pass(&mut rt, 2);
    assert_eq!(fake.of("e_ok").len(), 2);
}

#[test]
fn a_yield_still_goes_through_pcall() {
    let code = r#"
        local co = coroutine.create(function() local ok, v = pcall(coroutine.yield, 1); return v end)
        local _, first = coroutine.resume(co)
        local _, last = coroutine.resume(co, "back")
        leonida.log("co", first, last)
    "#;
    let (_d, fake, _rt) = rig(&[("a", code)]);
    assert_eq!(fake.of("a"), ["co 1 back"]);
}

#[test]
fn memory_is_read_and_written_through_the_checked_path() {
    let (_d, fake, mut rt) = rig(&[(
        "a",
        r#"
        local base, size = leonida.memory.base()
        local m = leonida.memory
        leonida.log("base", base ~= nil, size)
        leonida.log("w", m.write_u32(base + 8, 0xDEADBEEF), m.write_f32(base + 16, 1.5),
                    m.write_bytes(base + 32, "\1\2\3\4\5"), m.write_u8(base + 40, 255), m.write_u64(base + 48, 1 << 40))
        leonida.log("r", m.read_u32(base + 8), m.read_f32(base + 16), m.read_u8(base + 40), m.read_u64(base + 48), m.read_u16(base + 8), m.read_i32(base + 8))
        leonida.log("bytes", #m.read_bytes(base + 32, 5), m.read_bytes(base + 32, 5):byte(1, -1))
        -- Not there: nil, and false for a write. Nothing faults.
        leonida.log("bad", m.read_u32(0), m.read_u32(16), m.read_u32(-5), m.read_u32(base + size - 2),
                    m.read_u32(base - 4), m.write_u32(0, 1), m.write_u32(base + size, 1), m.read_bytes(base, 0),
                    m.read_bytes(base, 1 << 30), m.read_u32(math.maxinteger))
        -- Read-only tail: readable, not writable.
        leonida.log("ro", m.read_u32(base + size - 4), m.write_u32(base + size - 4, 7), m.write_u8(base + 1, 256), m.write_u8(base + 1, -1))
        leonida.log("neg", m.write_u32(base + 64, -1), m.read_u32(base + 64), m.read_i32(base + 64))
        -- A scan, with a wildcard.
        m.write_bytes(base + 100, "\x48\x8B\x05\x11\x22\x33\x44")
        m.write_bytes(base + 900, "\x48\x8B\x0D\x99\x22\x33\x44")
        local hits = m.scan("48 8B ?? ?? 22 33 44")
        leonida.log("scan", #hits, hits[1] - base, hits[2] - base)
        leonida.log("scan2", #m.scan("48 8B ?? ?? 22 33 44", {max = 1}), #m.scan("48 8B", {start = base + 200, size = 1000}))
        leonida.log("scan3", m.scan("zz"))
        leonida.log("scan4", m.scan("?? ??"))
    "#,
    )]);
    let log = fake.of("a");
    assert_eq!(log[0], "base true 4096");
    assert_eq!(log[1], "w true true true true true");
    assert_eq!(
        log[2],
        format!(
            "r {} 1.5 255 {} {} {}",
            0xDEADBEEFu32,
            1u64 << 40,
            0xBEEFu16,
            0xDEADBEEFu32 as i32
        )
    );
    assert_eq!(log[3], "bytes 5 1 2 3 4 5");
    assert_eq!(log[4], "bad nil nil nil nil nil false false nil nil nil");
    assert_eq!(log[5], "ro 0 false false false");
    assert_eq!(log[6], format!("neg true {} -1", u32::MAX));
    assert_eq!(log[7], "scan 2 100 900");
    assert_eq!(log[8], "scan2 1 1");
    assert!(log[9].starts_with("scan3 nil bad pattern"), "{}", log[9]);
    assert!(log[10].starts_with("scan4 nil bad pattern"), "{}", log[10]);
    // The write really landed in the buffer.
    let mem = fake.memory.borrow();
    assert_eq!(&mem[8..12], &0xDEADBEEFu32.to_le_bytes());
    assert_eq!(&mem[16..20], &1.5f32.to_le_bytes());
    assert_eq!(mem[40], 255);
    drop(mem);
    // And a mod that has no memory to touch (no title) just gets nil and false.
    let _ = &mut rt;
}

#[test]
fn drawing_is_a_list_cleared_every_frame_and_free_when_nothing_is_drawn() {
    let (_d, fake, mut rt) = rig(&[(
        "a",
        r##"
        local on = true
        leonida.on("frame", function()
            if on then
                leonida.text(10, 20, "hello", {size = 30, color = "#FF8000"})
                leonida.text(1, 2, "plain")
                leonida.rect(5, 6, 70, 8, {1, 2, 3, 4})
                leonida.rect(0, 0, 3, 3, 0xFF000080)
                leonida.rect(0, 0, 3, 3, {r = 9, g = 8, b = 7})
            end
        end)
        leonida.on("key", function(k, down) if down and k == "Q" then on = false end end)
    "##,
    )]);
    // Loading drew nothing and published nothing.
    assert!(fake.overlay.borrow().is_empty());
    pass(&mut rt, 1);
    let first = fake.overlay.borrow().last().unwrap().clone();
    assert_eq!(first.len(), 5);
    assert_eq!(
        first[0],
        Cmd::Text {
            x: 10.0,
            y: 20.0,
            text: "hello".into(),
            size: 30.0,
            colour: [255, 128, 0, 255]
        }
    );
    assert!(
        matches!(&first[1], Cmd::Text { size, colour: [255, 255, 255, 255], .. } if *size == 20.0)
    );
    assert_eq!(
        first[2],
        Cmd::Rect {
            x: 5.0,
            y: 6.0,
            w: 70.0,
            h: 8.0,
            colour: [1, 2, 3, 4]
        }
    );
    assert!(matches!(
        &first[3],
        Cmd::Rect {
            colour: [255, 0, 0, 128],
            ..
        }
    ));
    assert!(matches!(
        &first[4],
        Cmd::Rect {
            colour: [9, 8, 7, 255],
            ..
        }
    ));
    // Cleared each frame: the same five again, not ten.
    pass(&mut rt, 2);
    assert_eq!(fake.overlay.borrow().last().unwrap().len(), 5);
    // Stops drawing: one empty list to clear the picture, then nothing at all.
    fake.keys.borrow_mut().insert(b'Q');
    pass(&mut rt, 3);
    assert!(fake.overlay.borrow().last().unwrap().is_empty());
    let published = fake.overlay.borrow().len();
    pass(&mut rt, 4);
    pass(&mut rt, 5);
    assert_eq!(fake.overlay.borrow().len(), published);
}

#[test]
fn a_bad_colour_is_a_mod_error() {
    let (_d, _fake, rt) = rig(&[
        ("a", "leonida.rect(0, 0, 1, 1, 'red')"),
        ("b", "leonida.rect(0, 0, 1, 1, {1, 2})"),
        ("c", "leonida.rect(0, 0, 1, 1, '#12345')"),
        ("d", "leonida.rect(0, 0, 1, 1, '#123456')"),
    ]);
    for name in ["a", "b", "c"] {
        assert!(matches!(status(&rt, name), Status::Failed(_)), "{name}");
    }
    assert_eq!(status(&rt, "d"), Status::Running);
}

#[test]
fn a_mods_menu_page_shows_its_items_and_calls_back_with_what_the_player_did() {
    let (_d, fake, mut rt) = rig(&[(
        "a",
        r#"
        local page = leonida.menu.page("Example")
        local t = page:toggle("Show it", true, function(on) leonida.log("toggle", on) end, "helps")
        local n = page:number("Amount", 2.5, {min = 0, max = 10, step = 0.5, suffix = "x"}, function(v) leonida.log("number", v) end)
        page:action("Do it", function() leonida.log("action") end)
        leonida.log("get", t:get(), n:get())
        leonida.log("set", t:set(false), n:set(99), t:get(), n:get())
        leonida.log("badset", t:set(3), n:set("x"))
    "#,
    )]);
    let pages = fake.pages.borrow().last().unwrap().clone();
    assert_eq!(pages.len(), 1);
    assert_eq!(pages[0].title, "Example");
    let kinds: Vec<_> = pages[0]
        .items
        .iter()
        .map(|i| (i.label.as_str(), i.kind.clone()))
        .collect();
    assert_eq!(kinds[0], ("Show it", ModKind::Toggle(false)));
    assert_eq!(pages[0].items[0].help, "helps");
    assert_eq!(
        kinds[1],
        (
            "Amount",
            ModKind::Number {
                value: 10.0,
                min: 0.0,
                max: 10.0,
                step: 0.5,
                decimals: 1,
                suffix: "x".into()
            }
        )
    );
    assert_eq!(kinds[2], ("Do it", ModKind::Action));
    assert_eq!(
        fake.of("a"),
        [
            "get true 2.5",
            "set true true false 10.0",
            "badset false false"
        ]
    );
    // The player flips, moves and runs.
    fake.logs.borrow_mut().clear();
    *fake.events.borrow_mut() = vec![
        Event {
            page: 0,
            item: 0,
            value: MenuValue::Bool(true),
        },
        Event {
            page: 0,
            item: 1,
            value: MenuValue::Number(3.5),
        },
        Event {
            page: 0,
            item: 2,
            value: MenuValue::None,
        },
        Event {
            page: 7,
            item: 0,
            value: MenuValue::None,
        },
    ];
    pass(&mut rt, 1);
    assert_eq!(fake.of("a"), ["toggle true", "number 3.5", "action"]);
    // The mod's own copy of the values follows.
    let shared = rt.shared().borrow();
    assert_eq!(shared.pages[0].items[0].kind, ModKind::Toggle(true));
    drop(shared);
    // The value a mod sets is told to the menu.
    assert!(
        fake.values
            .borrow()
            .contains(&(0, 0, MenuValue::Bool(false)))
    );
}

#[test]
fn menu_pages_have_limits() {
    let (_d, _fake, rt) = rig(&[
        (
            "a",
            "local p = leonida.menu.page('x') for i = 1, 100 do p:action('i' .. i) end",
        ),
        ("b", "for i = 1, 100 do leonida.menu.page('p' .. i) end"),
        (
            "c",
            "leonida.menu.page('x'):number('n', 1, {min = 5, max = 1})",
        ),
        ("d", "leonida.menu.page('x'):number('n', 1)"),
    ]);
    for name in ["a", "b", "c"] {
        assert!(matches!(status(&rt, name), Status::Failed(_)), "{name}");
    }
    assert_eq!(status(&rt, "d"), Status::Running);
}

#[test]
fn settings_are_read_and_set_by_name_and_others_are_refused() {
    let _one = crate::TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let (_d, fake, _rt) = rig(&[(
        "a",
        r#"
        leonida.log("names", table.concat(leonida.settings, ","))
        leonida.log("set", leonida.set("menu_scale", 1.25))
        leonida.log("get", leonida.get("menu_scale"))
        leonida.log("bogus", leonida.set("LEONIDA_FPS_CAP", 30))
        leonida.log("nil", leonida.get("bogus"))
        leonida.log("type", leonida.set("menu_scale", {}))
        leonida.log("str", leonida.set("menu_scale", "big"))
        leonida.log("nocap", leonida.get("fps_cap"), leonida.set("fps_cap", 60))
        leonida.set("menu_scale", 1)
    "#,
    )]);
    let log = fake.of("a");
    assert_eq!(
        log[0],
        "names fps_cap,dlss_sharpness,panel,menu_scale,game_speed"
    );
    assert_eq!(log[1], "set true");
    assert_eq!(log[2], "get 1.25");
    assert!(
        log[3].starts_with("bogus false `LEONIDA_FPS_CAP` is not a setting"),
        "{}",
        log[3]
    );
    assert_eq!(log[4], "nil nil");
    assert!(log[5].starts_with("type false "));
    assert!(log[6].starts_with("str false "));
    // The cap has no owner in a test: nil, and a refusal.
    assert!(log[7].starts_with("nocap nil false "), "{}", log[7]);
}

#[test]
fn keys_are_held_for_the_title_until_released_or_the_mod_fails() {
    let a = r#"
        leonida.hold_keys({"W", "shift", "W"})
        leonida.on("frame", function()
            leonida.log("held")
            if leonida.frame() == 2 then leonida.release_keys() end
            if leonida.frame() == 3 then leonida.hold_keys({"A"}) end
            if leonida.frame() == 4 then error("stop") end
        end)
    "#;
    let b = r#"leonida.hold_keys({"A"})"#;
    let (_d, fake, mut rt) = rig(&[("a", a), ("b", b)]);
    let held = |fake: &Fake| fake.injected.borrow().last().cloned().unwrap();
    assert_eq!(held(&fake), [0x10, b'A', b'W']);
    pass(&mut rt, 1);
    assert_eq!(held(&fake), [0x10, b'A', b'W']);
    pass(&mut rt, 2);
    assert_eq!(held(&fake), [b'A']);
    pass(&mut rt, 3);
    assert_eq!(held(&fake), [b'A']);
    let before = fake.injected.borrow().len();
    pass(&mut rt, 4);
    // a fails: its A goes, b's stays (and it is not told again if nothing changed).
    assert_eq!(held(&fake), [b'A']);
    assert!(fake.injected.borrow().len() <= before + 1);
    let (_d, fake, _rt) = rig(&[("a", r#"leonida.hold_keys({"NotAKey"})"#)]);
    assert!(fake.injected.borrow().last().is_none_or(Vec::is_empty));
}

#[test]
fn files_are_the_mods_own() {
    let code = r#"
        local l = leonida.log
        l("w1", leonida.write_file("data.txt", "hello"))
        l("w2", leonida.write_file("sub/deep/data.bin", "\0\1\2"))
        l("r1", leonida.read_file("data.txt"))
        l("r2", #leonida.read_file("sub/deep/data.bin"))
        l("r3", leonida.read_file("missing.txt"))
        l("up", leonida.read_file("../other/main.lua"))
        l("abs", leonida.read_file("C:/Windows/win.ini"))
        l("abs2", leonida.write_file("/x.txt", "x"))
        l("stream", leonida.write_file("a.txt:evil", "x"))
        l("json", leonida.write_file("mod.json", "{}"))
        l("exe", leonida.write_file("x.exe", "x"))
        l("big", leonida.write_file("big.txt", string.rep("x", 2000000)))
        l("own", (leonida.read_file("main.lua")):sub(1, 5))
    "#;
    let (dir, fake, _rt) = rig(&[("a", code), ("other", "-- the other mod")]);
    let log = fake.of("a");
    assert_eq!(log[0], "w1 true");
    assert_eq!(log[1], "w2 true");
    assert_eq!(log[2], "r1 hello");
    assert_eq!(log[3], "r2 3");
    assert!(log[4].starts_with("r3 nil "), "{}", log[4]);
    for (i, key) in [
        (5, "up"),
        (6, "abs"),
        (7, "abs2"),
        (8, "stream"),
        (9, "json"),
        (10, "exe"),
        (11, "big"),
    ] {
        assert!(
            log[i].starts_with(&format!("{key} nil "))
                || log[i].starts_with(&format!("{key} false ")),
            "{}",
            log[i]
        );
    }
    assert_eq!(log[12], "own \n    ");
    assert_eq!(
        std::fs::read(dir.path().join("a/data.txt")).unwrap(),
        b"hello"
    );
    assert_eq!(
        std::fs::read_to_string(dir.path().join("a/mod.json")).unwrap(),
        JSON
    );
    assert!(!dir.path().join("x.txt").exists());
}

#[test]
fn require_loads_the_mods_own_modules_once() {
    let dir = folder(&[(
        "a",
        JSON,
        r#"
        local m = require("helper")
        local again = require("helper")
        leonida.log("m", m.answer(), m == again, (require("pkg.sub")))
        leonida.log("loop", pcall(require, "loop"))
        leonida.log("missing", pcall(require, "nothing"))
    "#,
    )]);
    let at = dir.path().join("a");
    std::fs::write(
        at.join("helper.lua"),
        "COUNT = (COUNT or 0) + 1\nreturn {answer = function() return 42 + COUNT end}",
    )
    .unwrap();
    std::fs::create_dir_all(at.join("pkg")).unwrap();
    std::fs::write(at.join("pkg/sub.lua"), "return 'sub:' .. (...)").unwrap();
    std::fs::write(at.join("loop.lua"), "require('loop')").unwrap();
    let fake = Fake::new();
    let mut rt = Runtime::new(fake.clone(), Config::default()).unwrap();
    rt.load_dir(dir.path());
    let log = fake.of("a");
    assert_eq!(log[0], "m 43 true sub:pkg.sub", "{log:?}");
    assert!(
        log[1].starts_with("loop false ") && log[1].contains("loop"),
        "{}",
        log[1]
    );
    assert!(
        log[2].starts_with("missing false ") && log[2].contains("not found"),
        "{}",
        log[2]
    );
}

#[test]
fn only_enabled_lua_mods_are_loaded() {
    let dir = folder(&[
        ("on", JSON, "leonida.log('on')"),
        (
            "off",
            r#"{"script": "main.lua", "enabled": false}"#,
            "leonida.log('off')",
        ),
        (
            "assets",
            r#"{"name": "Swaps", "archetype_swaps": []}"#,
            "leonida.log('assets')",
        ),
        (
            "broken",
            r#"{"script": "../main.lua"}"#,
            "leonida.log('broken')",
        ),
        ("noscript", r#"{"script": "gone.lua"}"#, ""),
        (
            "named",
            r#"{"name": "A Nice Name", "script": "main.lua"}"#,
            "leonida.log('named')",
        ),
    ]);
    std::fs::create_dir_all(dir.path().join("nojson")).unwrap();
    std::fs::write(dir.path().join("nojson/main.lua"), "").unwrap();
    let fake = Fake::new();
    let mut rt = Runtime::new(fake.clone(), Config::default()).unwrap();
    let report = rt.load_dir(dir.path());
    let get = |n: &str| {
        report
            .iter()
            .find(|(name, _)| name == n)
            .map(|(_, s)| s.clone())
    };
    assert_eq!(get("on"), Some(Status::Running));
    assert_eq!(get("off"), Some(Status::Off));
    assert_eq!(get("assets"), None);
    assert!(matches!(get("broken"), Some(Status::Failed(_))));
    assert!(matches!(get("noscript"), Some(Status::Failed(_))));
    assert_eq!(get("A Nice Name"), Some(Status::Running));
    assert_eq!(get("nojson"), None);
    let logged: Vec<_> = fake
        .logs()
        .into_iter()
        .filter(|l| !l.contains("not loaded") && !l.contains("disabled"))
        .collect();
    assert_eq!(logged, ["[A Nice Name] named", "[on] on"]);
}

#[test]
fn log_misc_and_time_work() {
    let (_d, fake, _rt) = rig(&[(
        "a",
        r#"
        leonida.log("a", 1, 2.5, nil, true, {}, print)
        print("via print", 3)
        local t0 = leonida.time()
        leonida.log("time", type(t0), t0 >= 0, leonida.frame())
        leonida.log("shot", leonida.screenshot())
    "#,
    )]);
    let log = fake.of("a");
    assert_eq!(log[0], "a 1 2.5 nil true <table> <function>");
    assert_eq!(log[1], "via print\t3");
    assert_eq!(log[2], "time number true 0");
    assert_eq!(log[3], "shot shot.png");
}

#[test]
fn the_example_mod_ships_disabled_and_runs_when_enabled() {
    let source = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../mods/example_lua");
    let manifest = std::fs::read_to_string(source.join("mod.json")).unwrap();
    assert!(
        !crate::manifest::parse("example_lua", &manifest)
            .unwrap()
            .enabled
    );
    // Enabled, in a folder of its own.
    let dir = tempfile::tempdir().unwrap();
    let at = dir.path().join("example_lua");
    std::fs::create_dir_all(&at).unwrap();
    std::fs::write(at.join("mod.json"), manifest.replace("false", "true")).unwrap();
    std::fs::copy(source.join("main.lua"), at.join("main.lua")).unwrap();
    let fake = Fake::new();
    let mut rt = Runtime::new(fake.clone(), Config::default()).unwrap();
    let report = rt.load_dir(dir.path());
    assert_eq!(report, [("Example Lua mod".to_owned(), Status::Running)]);
    // Its page: a toggle, a number and two actions.
    let pages = fake.pages.borrow().last().unwrap().clone();
    assert_eq!(pages.len(), 1);
    let labels: Vec<_> = pages[0].items.iter().map(|i| i.label.as_str()).collect();
    assert_eq!(
        labels,
        [
            "Show the line",
            "Line size",
            "Take a screenshot",
            "Log the image base"
        ]
    );
    // A frame draws the line; F9 turns it off; the actions and events run.
    pass(&mut rt, 1);
    let drawn = fake.overlay.borrow().last().unwrap().clone();
    assert!(
        matches!(&drawn[1], Cmd::Text { text, .. } if text.starts_with("Lua example")),
        "{drawn:?}"
    );
    fake.keys.borrow_mut().insert(0x78);
    pass(&mut rt, 2);
    fake.keys.borrow_mut().clear();
    pass(&mut rt, 3);
    assert!(fake.overlay.borrow().last().unwrap().is_empty());
    *fake.events.borrow_mut() = vec![
        Event {
            page: 0,
            item: 2,
            value: MenuValue::None,
        },
        Event {
            page: 0,
            item: 3,
            value: MenuValue::None,
        },
    ];
    fake.menu_open.set(true);
    pass(&mut rt, 4);
    let log = fake.of("Example Lua mod");
    assert!(log.iter().any(|l| l == "screenshot: shot.png"), "{log:?}");
    assert!(
        log.iter()
            .any(|l| l.contains("the title's image:") && l.contains("4096 bytes")),
        "{log:?}"
    );
    assert!(log.iter().any(|l| l == "the F4 menu was opened"), "{log:?}");
    assert_eq!(status(&rt, "Example Lua mod"), Status::Running);
}
