# Lua mods

The D3D12 runtime (`d3d12/`, `leonida.exe`) runs Lua 5.4 mods. A mod is a folder in `mods\` with the same
`mod.json` the mod manager (`tools/leonida_mods.py`, `Mods.bat`) already lists, and a script next to it:

```
mods\
  my_mod\
    mod.json          {"name": "My mod", "enabled": true, "script": "main.lua"}
    main.lua
    helper.lua        require("helper")
```

The scripts run in a sandbox on a thread of their own and can use what the runtime itself controls: frame, key and
menu events, an overlay drawn over the picture, the live settings the F1 and F4 menus change, pages in the F4 trainer
menu, and checked reads and writes of the title's memory with a byte-pattern scan. Single player only; nothing
here touches anything online. The code is `d3d12/crates/leonida-lua`, `d3d12/crates/leonida-menu/src/overlay.rs` and
`mods.rs`, and `d3d12/crates/leonida-gpu/src/host/trainer.rs` (where the overlay is drawn).

Status: built and unit-tested (`cargo test -p leonida-lua --lib`, 35 tests, and the menu crate's); **not yet tried in
the running title**. What that leaves open is listed at the end.

## Trying it

1. Build with diagnostics for screenshots (`cargo build --profile diag -p leonida --features diagnostics`); the Lua
   mods themselves are in every build.
2. Find the mods folder: `LEONIDA_MODS=<folder>` if set, else `mods` beside `leonida.exe` or in a folder above it
   (so a build in `d3d12\target\...` finds the repository's `mods\`), else `mods` in the current folder. The log says
   which (`Lua mods` line, target `lua`).
3. Set `"enabled": true` in `mods\example_lua\mod.json` (it ships disabled), start the title, and look at the lower
   left corner: a frame-rate line. F4 opens the trainer menu; the last item, Mods, has the example's page.
   After editing a script, F4, Mods, Reload Lua mods loads them all again (a fresh Lua state).

`LEONIDA_LUA=0` turns it off completely: no thread, nothing per frame but one atomic load at present, no Mods page.

## Events

```lua
leonida.on("frame", function(dt) end)         -- once per presented frame (see below); dt in seconds
leonida.on("key", function(key, down) end)    -- "W", "F4", "Space", "Shift"...; true on press, false on release
leonida.on("menu_open", function() end)       -- the F4 menu was opened
```

Handlers run on the Lua thread, never on the present. If the Lua thread is slower than the frames, frames are
coalesced: the handler runs once for all the presents since its last run, with `dt` the time since then. Key events
come only while one of the runtime's windows is in front. Up to 64 handlers a mod. An unknown event name is an error.

## Input

```lua
leonida.key_down("W")            -- bool. Names: letters, digits, F1-F24, Enter Esc Tab Back Space Shift Ctrl Alt
                                 -- LShift RShift LCtrl RCtrl LAlt RAlt Up Down Left Right Insert Delete Home End
                                 -- PageUp PageDown Num0-Num9 NumAdd NumSub NumMul NumDiv NumDot CapsLock Pause
                                 -- MouseLeft MouseRight MouseMiddle Semicolon Equals Comma Minus Period Slash
                                 -- Tilde LBracket Backslash RBracket Quote. An unknown name is an error.
local p = leonida.pad()          -- the first connected pad: p.connected, p.a p.b p.x p.y p.lb p.rb p.back p.start
                                 -- p.ls p.rs p.up p.down p.left p.right (booleans), p.lt p.rt (0 to 1),
                                 -- p.lx p.ly p.rx p.ry (-1 to 1)
leonida.hold_keys({"W", "Shift"})  -- the title sees these keys down, as LEONIDA_KEYS_FILE feeds them
leonida.release_keys()
```

`hold_keys` sets the whole list (it replaces the mod's previous one); the title gets the union of what all mods hold.
The stand-in pad the keyboard drives when no pad is connected sees them too. A mod that fails lets its keys go.

## Overlay

```lua
leonida.text(x, y, "hello", {size = 20, color = "#FFE08A"})   -- (x, y) is the top left of the text
leonida.rect(x, y, w, h, {0, 0, 0, 150})
```

Coordinates and sizes are pixels of a 1080-line picture and scale with the real picture's height. The list is cleared
at the start of each pass: draw again each frame (in a `frame` handler) for what is to stay. Colours: a table
`{r, g, b[, a]}` or `{r=, g=, b=, a=}` (0 to 255), a string `"#RRGGBB"` or `"#RRGGBBAA"`, or an integer (`0xRRGGBB`, or
`0xRRGGBBAA` when it is above 24 bits); the default is white for text and translucent black for a rectangle. At most
1024 commands, 300 characters a text, text 6 to 200 px. The system's own fonts draw it (Segoe UI on Windows; the
built-in dot font where there are none).

The list is painted on the CPU into one picture the size of what is drawn (up to 4096 x 2304) and laid over the
frame in one blended quad, where the F3 panel and the F4 menu are; an unchanged list is not painted again, so many
small things cost little and a full-screen rectangle costs a fill of that size when it changes. A present pays one
atomic load while nothing is drawn. The drawing is part of the picture frame generation sees; screenshots and saved
frames do not have it.

## Settings

```lua
leonida.get("fps_cap")                 -- nil when this run does not have it
local ok, why = leonida.set("fps_cap", 60)
for _, name in ipairs(leonida.settings) do ... end
```

| Name | Value | Same as |
|---|---|---|
| `fps_cap` | frames a second 1 to 1000, `0` for none, `"game"` for the title's own pace | the F1 menu's FRAME RATE CAP, `LEONIDA_FPS_CAP` |
| `dlss_sharpness` | 0 to 1 (there when the title's upscale is DLSS) | DLSS SHARPNESS, `LEONIDA_DLSS_SHARPNESS` |
| `panel` | true or false (diagnostics builds) | the F3 panel |
| `menu_scale` | 0.6 to 1.6 | F4, Settings, Menu, Menu size |
| `game_speed` | 0.1 to 2 (experimental, `docs/mod-menu.md`) | F4, World, Game speed |

Any other name is refused: `set` returns `false` and why, `get` returns `nil`. Values outside the range are clamped.
Nothing else of the runtime is reachable.

## Trainer menu pages

```lua
local page = leonida.menu.page("My mod")
local on   = page:toggle("Enabled", true, function(value) end, "help text")
local num  = page:number("Amount", 2.5, {min = 0, max = 10, step = 0.5, decimals = 1, suffix = "x"}, function(value) end)
page:action("Do it", function() end, "help text")
on:get(); on:set(false)          -- read or change what the menu shows
```

The pages of all mods are under F4, Mods (the last item of the main menu), after "Reload Lua mods". Callbacks run on
the Lua thread when the player flips, moves or runs the item. At most 64 pages and 64 items a page (shared); labels
are cut at 48 characters. After a reload, or when a mod fails and its pages go, the menu is on the Mods page again.

## Memory

Single-player modding of the title's own memory, through the runtime's checked copies (`host::read_into`,
`safecopy`): an address that is unmapped, or a write to a protected page, gives `nil` or `false` and never faults.

```lua
local m = leonida.memory
m.read_u8 / read_u16 / read_u32 / read_i32 / read_u64 / read_f32 / read_f64 (address)   -- value or nil
m.read_bytes(address [, n = 16])                                                          -- Lua string or nil (n <= 1 MiB)
m.write_u8 / write_u16 / write_u32 / write_i32 / write_u64 / write_f32 / write_f64 (address, value)   -- true or false
m.write_bytes(address, string)                                                            -- true or false
m.base()                                      -- the title's executable image: base, size (nothing before it is mapped)
m.scan("48 8B ?? 05", {max = 100, start = address, size = n})   -- list of addresses, or nil and why
```

Little-endian, as the x86-64 title is; addresses are Lua integers (64-bit). Addresses below 64 KiB or above the user
range are refused. Patterns are hex bytes with `??` (or `?`) for any byte, spaces optional, at least one real byte;
`scan` covers the whole image by default, skips pages that are not mapped, never matches across a gap and returns at
most `max` (default 100, ceiling 10,000) addresses in order. A scan of the 114 MB image reads it once in 1 MiB steps
on the Lua thread, and the time it takes does not count against the mod's time budget.

What it does not do: it does not know what the title keeps where. There is no table of the title's data here (the
title's own script natives were not found, `docs/mod-menu.md`); a mod author finds addresses with `scan` and their
own tools and takes the risk that a write corrupts the title. A write is checked to land on a page that can be
written, not that it is sensible. Reads and writes race with the title's threads, as any trainer's do.

## Misc

```lua
leonida.api_version       -- 1. It changes only when something a mod may rely on does.
leonida.version           -- the runtime's version, a string
leonida.log("a", 1, tbl)  -- values joined by spaces, to the runtime's log (tracing target `lua`, the mod's name)
print(...)                -- the same, joined by tabs
leonida.screenshot()      -- the path the next frame is saved to, or nil and why (diagnostics builds)
leonida.frame()           -- presents counted so far (as the Lua thread last saw them)
leonida.time()            -- seconds since the mods started, a float
leonida.read_file("data.txt")           -- the contents, or nil and why
leonida.write_file("sub/data.txt", s)   -- true, or false and why
require("helper")         -- helper.lua (or helper/init.lua; dots are folders) of the mod's own folder, run once
```

Files: only the mod's own folder, plain relative paths (no `..`, drive letters, streams or reserved device names; a
link that leads out is refused), reads up to 4 MiB, writes up to 1 MiB, never `mod.json` or a program (`.exe`, `.dll`,
`.bat`, ...). There is no `io`.

## The sandbox

Each mod gets an environment of its own: copies of `assert error getmetatable ipairs next pairs pcall rawequal rawget
rawlen rawset select setmetatable tonumber tostring type xpcall` and of the `coroutine`, `table`, `string`, `utf8`
and `math` libraries, `os.time os.clock os.date os.difftime`, and `print`, `load`, `collectgarbage` and `require`
made for it. One mod changing `string.rep` or `math.floor` changes nothing for another, and `getmetatable("")` gives
a copy.

Not there: `io`, `package` (so `package.loadlib` and any C module), `debug`, `dofile`, `loadfile`, `string.dump`,
`os.execute`, `os.remove`, `os.rename`, `os.exit`, `os.getenv`, `os.tmpname`, `os.setlocale`. `load` takes text
chunks only (a precompiled chunk can take the interpreter down), in the mod's environment unless one is given;
`collectgarbage` does `"collect"` and `"count"`. There is no socket, HTTP or any other network function: the
runtime offers none and the standard libraries that could reach one are not loaded.

Limits: a call into a mod (loading it, a handler, a menu callback) may run for 200 ms (the load 2 s) of Lua; a loop
that does not end stops with an error, also inside `pcall`, `xpcall` and `coroutine.resume`, which hand that error
back instead of swallowing it. The whole Lua state may use 128 MiB. A coroutine can still yield through `pcall`.

An error anywhere (the script's own, a handler, a menu callback, a bad argument, an exceeded limit) is logged with
the mod's name and **disables that mod only**: its handlers, its menu pages and the keys it holds go, the other mods
go on. A syntax error or a missing script is the same at load. A Rust panic in the runtime stops the Lua thread's work
(the mods are dropped; three in all end the thread) and not the title.

`mod.json`: `"script"` (a `.lua` path inside the folder), `"enabled"` (default true; the example ships `false`),
`"name"` (shown in logs). A folder without `"script"` (an asset mod, `docs/modding.md`) is not a Lua mod and is left
alone; a mod can have both. Load order is the folder names', sorted.

## What is not here, and why

- `leonida.teleport(x, y, z [, hold_seconds])` (true line, or nil and why) and `leonida.player_position()` (x, y, z) move
  and read the player by memory, once the runtime knows where the player is (`docs/mod-menu.md`, "Teleport by memory").
- God mode, wanted level, vehicles, weapons, weather, time of day: there is no `leonida.player` and no
  `leonida.world` yet. With `LEONIDA_GAME_ACTIONS=1` the F4 menu does wanted level, time of day, weather and the
  title's own cheats through the title's own code (`docs/mod-menu.md`, "Game actions"), and
  `leonida.player_position()` is the ped's own position then; that is not offered to mods until it has run in the
  title. A mod that wants the rest has the memory API and has to find the addresses itself.
- Anything online, any file outside the mod's folder, any process or library: not offered, on purpose.
- No keys, decryption or protection workarounds, and no game data in the repository: a mod holds its own script and
  the addresses its author found, not pieces of the title.

## Not tried yet

Built, unit-tested and compiled into the runtime, but not run in the title: where the overlay lands on the real back
buffer and how it looks with frame generation; that the `hold_keys` keys reach the title as `LEONIDA_KEYS_FILE`'s do;
the checked write on the title's guarded memory; a scan of the real 114 MB image (the time it takes); the menu
callbacks over the real F4 menu; Linux (the code is portable, but key and pad reading return nothing off Windows
for now). `cargo test -p leonida-gpu --features diagnostics --lib host::trainer` was not run (it draws on a GPU);
the draw path for the mods' overlay is not covered by it.

## Tests

`cargo test -p leonida-lua --lib` (the sandbox, event dispatch, a mod failing alone, endless loops, memory on a local
buffer with unmapped, protected and out-of-range addresses, scans across chunks and holes, overlay, menu pages,
settings, files, require, manifests, the API version) and `cargo test -p leonida-menu --lib` (the Mods page, the
overlay painter, injected keys, `Menu::replace_pages`). No GPU, no title.
