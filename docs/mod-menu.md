# The trainer menu

Leonida's D3D12 runtime (`d3d12/`) has an in-game trainer menu, drawn by the runtime over the presented picture. It
is on by default; `LEONIDA_MENU=0` turns it off (no thread, no clock override, nothing drawn). Single player only:
nothing in it touches other players, money or anything online.

It is one of three things that runtime lays over the picture, each on its own key:

| Key | What | Code (`d3d12/crates/leonida-gpu/src/`) | Builds |
|---|---|---|---|
| F1 | the settings menu that came with the renderer merged at `00698e1e8`: launcher settings, saved to `leonida-settings.txt` | `host/menu.rs` | diagnostics |
| F3 | the numbers panel (frame rate, memory) | `host/panel.rs` | diagnostics |
| F4 | the trainer menu, this document | `host/trainer.rs`, and the crate `d3d12/crates/leonida-menu` | all |

Status: the menu was written against the renderer as it was at `db723a4c4` and brought onto the merged one
afterwards. It builds and its tests pass (below), the overlay's pipeline draws the menu on a D3D12 card offscreen,
but **it has not been tried in the running title yet**: where it lands on the real back buffer, how it looks with
frame generation, and whether the kept keys feel right there are not verified.

Code: `d3d12/crates/leonida-menu` (model, pages, input, run-time settings, CPU painter, a PNG encoder for the
previews) and `d3d12/crates/leonida-gpu/src/host/trainer.rs` (the overlay drawn at present, what the renderer lends
the menu, screenshots); `d3d12/crates/leonida-hle/src/modules/gameinput.rs` keeps the menu's keys from the title
and `modules/clock.rs` serves the title's clock.

Lua mods (`docs/lua-modding.md`) add pages to this menu: the last item of the main menu, Mods, has "Reload Lua mods"
and a page for each page a mod made. They are drawn and moved by this code (`leonida_menu::mods`), and a mod's
callbacks run on the mods' own thread, never on the menu's.

## Controls

| Key | Pad | Does |
|---|---|---|
| F4 | RB + d-pad Left | open / close |
| Up, Down or numpad 8, 2 | d-pad Up, Down | move (repeats while held, wraps round) |
| Left, Right or numpad 4, 6 | d-pad Left, Right | change a number or an option |
| Enter or numpad 5 | A | select: open a page, flip a toggle, run an action |
| Backspace or numpad 0 | B | back; on the first page, close |

The menu reads the keys only while one of the runtime's windows is in front. While it is open those keys (and the
pad's d-pad, A and B) are kept from the title (`gameinput.rs`), and so from the stand-in pad the keyboard drives
when no pad is connected; a key still held when the menu closes stays kept until it is let go, so the Enter or
Backspace that closed it does not reach the game. Closed, the title sees everything as before.

Beside the F1 menu: while the F1 menu is shown it has the keyboard (the title sees no key at all, as upstream has
it), and the trainer reads the pads only, so the arrows move one menu and not two; F4 is not read then either. The
two can be shown at once (F4 first, then F1): the F1 box is in the middle of the picture, the trainer on the left.

A thread of its own samples the keys and pads (every 16 ms closed, 8 ms open) and paints the menu on the CPU only
when it changes. A present asks one word what there is to do for the menu (`leonida_menu::work()`: a bit while the
menu is open, a bit while a screenshot is asked for), so a present with the menu closed pays one atomic load and
nothing else; the thread is started and the picture's height told when the presenter is made.

### Opening it without the keyboard

`LEONIDA_MENU_OPEN=1` starts the menu open on its first page, for a test that cannot press F4 (a window without the
keyboard focus). From the launcher: `scripts\play-d3d12.ps1 -Game <folder> -More @{LEONIDA_MENU_OPEN='1'}`. It
stays open until F4, Backspace or B closes it. The keys it keeps from the title are kept from the first present on;
the scripted keys of a test (`LEONIDA_KEYS`, `LEONIDA_KEYS_FILE`) are the title's and still reach it, and they do
not move the menu. `LEONIDA_MENU_OPEN` is the trainer's; the other `LEONIDA_MENU_*` variables (`_FILE`, `_PLAY`,
`_ARGS`, `_SMOOTH_MOTION`, `_CONSOLE`) belong to the F1 settings menu and its launcher.

## Look

The classic single-player trainer layout, drawn by Leonida's own code: a blue gradient banner with the title in a
script face, a black bar with the page's caption (light blue) and `n / N` (white), dark translucent rows with
white text, the selected row light with dark text, `>` on rows that open a page, on/off boxes for toggles,
`< value >` for numbers and options on the selected row, a bar with the scroll arrows and a box with the selected
item's help (amber when the item cannot work). 432 x 108 px banner, 34 px caption bar, 36 px rows, 11 rows at
once, 28 px from the top-left corner, all at a 1080-line picture and scaled with the picture's height (and with
Settings, Menu, Menu size). The F3 panel stands in the same corner: while it is shown the menu starts 8 px below
it. The highlight slides from row to row: two layers are painted (all rows unselected, all rows selected) and the
shader shows the second inside the band, so the band moves on the card without a new picture.

Where it is drawn: one blended quad onto the swap chain's buffer, after the title's picture is there (its two
planes laid over each other by `compose.rs`, or its one plane copied) and before `Present`, in the same list and at
the same point as the F3 panel and the F1 menu. With DLSS frame generation (`fg.rs`) the swap chain is Streamline's
and generated frames are made from what was presented, the three overlays included; no interface plane is tagged
for Streamline (upstream tags none either), so in generated frames the menu is treated like the rest of the
picture. Saved frames, the frame recorder and screenshots are read back before the overlays are drawn and show the
title's picture alone.

Fonts: the system's own, rasterised at run time by GDI (`GetGlyphOutlineW`): Segoe Script Bold for the banner,
Segoe UI and Segoe UI Semibold for the rows and the bar. Both ship with Windows 10 and 11; nothing of a font is in
the repository or the build. Off Windows (and in the model's tests) the built-in 5x7 dot font is used. No asset of
GTA V or GTA VI is used.

### Why not a port of the GTA V Native Trainer

Asked for: port the menu of the ScriptHookV SDK's NativeTrainer sample (or a "Native Mod Menu" project) if its
licence allows reuse. Found (2026-11-26):

- The NativeTrainer sample ships in Alexander Blade's ScriptHookV SDK from dev-c.com. No licence that grants reuse
  was found for the SDK or the sample; its source files carry only a copyright line. Without a licence the code
  cannot be copied into a GPL-3.0-or-later project, so none was.
- Enhanced Native Trainer (github.com/FIying-Scotsman/GTAV-EnhancedNativeTrainer), which grew from that sample, has
  a GPL-2.0 `LICENSE.md` with no "or any later version" notice found on the repository's page, and its menu is
  drawn with GTA V's own natives (rectangles and text through the game), so there is no portable layout code in it.
  Its provenance (built on the unlicensed sample) is a second reason not to take code from it.

So the look and behaviour were reproduced from scratch: proportions, colours, controls (F4, the number pad and the
arrows) and the grouping (Player, Vehicle, Weapons, World, Time, Weather, Teleport, Misc, plus Settings).

## What works

What is wired to something real, as built and unit-tested; none of it has been tried in the running title yet.

| Page, item | What it does | Where |
|---|---|---|
| Settings, Display, Sharpen strength | DLSS's sharpness, 0 to 1 in steps of 0.05: the same setting as the F1 menu's DLSS SHARPNESS (`LEONIDA_DLSS_SHARPNESS`, which `dlss.rs` reads at every upscale). There when the build has the `dlss` feature and `LEONIDA_UPSCALE=dlss`; dimmed otherwise. Whether the DLSS library in use still honours that parameter was not checked. (`LEONIDA_SHARPEN`, the menu's first target, went with the compose pass the merged renderer replaced.) | `knobs::SHARPNESS`, `host/trainer.rs` |
| Settings, Display, FPS panel | shows and hides the numbers panel (F3 does the same). Diagnostics builds; dimmed otherwise. | `knobs::PANEL`, `host/panel.rs` |
| Settings, Menu, Menu size | 0.6x to 1.6x | `knobs::MENU_SCALE` |
| Misc, Save screenshot | the next presented frame, without the menus or the panel, at full size, as a PNG in `LEONIDA_SCREENSHOTS` (else `screenshots` beside the runtime): 8-bit, 10-bit and half-float pictures. It is the present's own read-back (`capture::read_back`), so diagnostics builds; dimmed otherwise. | `host/trainer.rs`, `capture.rs` |
| Misc, Frame recording | every presented frame saved, by making and removing the file `rec` in the capture folder, which is what `capture.rs` looks for (a script that makes the file shows in the menu as on). Diagnostics builds with `LEONIDA_CAPTURE` set (the launcher's `-Capture`); dimmed otherwise. | `knobs::RECORD`, `capture.rs` |
| Misc, Frame-rate cap | Game, 30, 40, 60, 90, 120, 144, None: the renderer's own cap, the same setting as the F1 menu's FRAME RATE CAP (`LEONIDA_FPS_CAP`, the frame clock of `frame.rs`). Game is the title's own pace, None as fast as it renders; a rate set elsewhere that is not offered shows as the nearest. | `knobs::FPS_CAP`, `frame.rs` |
| World, Game speed / Slow motion | the rate of the title's `QueryPerformanceCounter` (0.1x to 2x; slow motion: one third) | `leonida-hle/src/modules/clock.rs` |
| Settings, About | the controls | |

The cap, the sharpness, the panel and the recorder are the renderer's own settings, lent to the menu
(`knobs::Owned`). The first three are in the F1 menu too: the two menus change the same thing, and each shows what
the other set (the F1 menu reads its live settings again when it opens; the trainer looks twice a second while
open). The F1 menu saves to `leonida-settings.txt`; what the trainer changes lasts for the run unless the F1 menu
is opened afterwards and saves it.

Game speed is experimental and not verified in game: the runtime serves the title's `QueryPerformanceCounter`
import (from `api-ms-win-core-profile-l1-1-0.dll`) and scales it continuously, so the title's time never jumps.
The executable calls that import from 273 places (counted as `call`/`mov` through its import slot), but it also
reads the processor's time-stamp counter directly 1,037 times (`rdtsc`, `0F 31`, counted over `.text`), which
no import reaches; what the title times that way keeps its pace, and which of the two its game clock uses was not
established. Until the rate is first changed, the host's count is passed through unchanged.

## Game actions: the title's own cheat system

`LEONIDA_GAME_ACTIONS=1` (off by default: **built and unit-tested, not yet run in the title**) makes these items
real. They are done by the title's own code, on the title's own thread, at the place where the title runs its own
cheats; a thread of ours only queues what was asked.

| Page, item | What is asked of the title | How sure |
|---|---|---|
| Player, Wanted level / Clear wanted level | its function that sets a ped's wanted level (ped, level, three flags; level 0 is its clear), with the flags its own two wanted cheats pass | found once; arguments read from its two callers in the cheat table |
| Player, Never wanted | the same, to 0, whenever the level is above 0 (looked at twice a second) | as above |
| Player, Heal and armour | cheat 2 of its table, through its dispatcher | the handler is in the table; that it is the health cheat is read from its code (it returns at once when the ped's health is 0 or less, resets the ped's health state, repairs the player's vehicle), not seen |
| Time, Hour / Hour forward / Hour back / Sync with system | its function that sets the time of day (hour, minute, second) | found once; three plain numbers, checked to be a time before the call |
| Time, Clock paused | the byte its clock's rate function tests (its own setter is that one store) | found once |
| Weather, Weather / Freeze weather | its function that forces a weather type by index, and the one that lets the weather run again | found once each; the index is looked up by the type's name (below) |
| Vehicle, Spawn vehicle, nine items | cheats 7 to 15 of its table, in the table's order | the handlers are in the table; **which vehicle each holds is not known**: the labels are the menu's, the answer line says so |
| Teleport, Save position (and `leonida.player_position()`) | nothing: the ped's own position is read | the title's own position function reads the same place |

The other items still say why they are not there (next section). `d3d12/crates/leonida-loader/src/game_actions.rs`
has the places and the hook, `d3d12/apps/leonida/src/game_actions.rs` the work, `knobs::GameHook` lends it to the
menu.

### How it was found (2026-11-27, the 1.0.2.1 executable, read locally)

- **Strings.** `.rdata` and `.data` hold 76,113 runs of four or more printable characters; two contain "cheat":
  the formats `CHEAT%d` and `CHEAT%d_%d`, both used by one function that shows a text label. It has 14 call
  sites, in 13 functions that take no argument, and those are what 13 of the 16 entries of a table of pointers
  in `.rdata` lead to; the table is used once, by a dispatcher that takes a pointer to a cheat's number and jumps
  through it (numbers 16 to 18 flip a byte each instead, and nothing else reads those bytes).
- **The table.** Cheats 0 and 1 show a label only; 2 is health (above); 3 raises and 4 lowers the wanted level by
  one; 5 is an empty function; 6 moves the weather to the next type of its cycle (three stores into the weather
  state); 7 to 15 ask for a vehicle by a 64-bit hash of its model's name, in front of the player (8 different
  hashes; 7 and 12 hold the same, and 7 and 10 do nothing in some state of the player). The hash function of
  model names is not known, so the vehicles have no names here. A handler first calls the title's own
  bookkeeping for a used cheat, as for a typed one: what the title does with that (a saved game marked as
  cheated, say) happens here too. The direct calls (wanted level, time, weather) do not go through it.
- **The player.** Cheats 2 to 4 read one global, the player's ped: 1,842 instructions in the code refer to it. The
  title's function that gives the player's position reads four floats at 70h of that ped; the vehicle cheats add
  the vectors at 40h and 50h, scaled, to it: a 4x4 matrix at 40h, right, forward, up, position. An entity's id is
  the `u32` at 9Ch; per-entity state (wanted, health) is looked up by that id.
- **Wanted level.** The function cheats 3 and 4 call takes (ped, level, flag, flag, flag) and tail-calls the clear
  function at level 0; it reads the current level from the ped's wanted state, raises the title's maximum when the
  level is above it, and reports a crime or lowers the level. The flags are (0, 1, 1) from cheat 3 and (1, 1, 1)
  from cheat 4, and the backend passes the first when it raises and the second when it lowers.
- **Weather.** Cheat 6 gave the weather state: a global whose first `u32` is the number of types, with the cycle,
  the previous and next type index, the blend between them and a forced index (-1: none). 37 instructions refer
  to its start. The function that forces a type sets previous, next and forced to the index; the one beside it
  sets forced to -1. The types are 38h bytes each behind a pointer, the `joaat` of the type's name first: the
  code compares a type's hash with `joaat("RAIN")` (11 places hold that constant), `joaat("THUNDER")` (5) and
  `joaat("SNOW")` (8). So a type is found by its name at run time, in the table the title loaded from its data:
  the menu's nine choices are looked up as `EXTRASUNNY`, `CLEAR`, `CLOUDS`, `OVERCAST`, `FOGGY`, `RAIN`,
  `THUNDER`, `SMOG`, `WINDY` (GTA V's names; only `RAIN` and `THUNDER` are confirmed by the code), and a choice
  the title has no type for says so. `info` (below) lists the table's types, named where a name matches.
- **Time of day.** The clock is a global of a date (weekday, day, month from 0, year) and a time (hour, minute,
  second, millisecond: four `i32`; the hour is read by 126 instructions, the minute by 66, the second by 28). No
  instruction writes the hour through its address: the setters take the clock's address. The one taken sets the
  time with the title's checked setter, marks the clock as set and re-bases its counters; the title's start-up
  calls it with the starting hour. A byte makes the rate function give 0 ms a minute: the pause.
- **Thread and moment.** The cheat input function (it turns twelve pad buttons into letters and matches the
  last ones against the cheats) is called from one place, a step registered by a hashed name in the function
  that builds the title's update graph. A typed cheat is not run there directly: the dispatcher is given to the
  title's deferred call (2,342 calls and jumps to it in the code), queue 4, as a closure of four words (argument, function,
  copy, drop). That function runs the closure at once when the thread is not inside a parallel update, else it
  copies it into the thread's queue, which the title runs later (where was not read: its own cheats go the same
  way). Our hook is the one
  `call` of the cheat input function: it calls that function, then, if something was asked, gives the same queue
  a closure of its own. So our work runs exactly where a cheat of the title's would.
- **Finding it again.** Ten places are named by patterns of 24 to 77 bytes (19 to 36 of them fixed, `??` for
  what another build changes: short signatures, no longer than they must be), which give 19 things: 8 functions,
  the hook's call, 9 globals and the place of the level in the wanted state. Each
  pattern is in the 99.9 MB of code exactly once (`cargo test -p leonida-loader game_actions` with
  `LEONIDA_GAME_DIR` set checks it, and that functions lie in code and globals behind it). None contains a load
  of the thread's TLS array, which the loader rewrites. A place that is not found leaves its items saying so; the
  hook and the deferred call must be found or nothing is changed.

What can go wrong, since none of it has run: the update step may not run in every state (a menu, a cutscene: the
items then say "the title's update has not run" or simply wait); a function may need a state the cheats' own
checks ensure and a direct call does not (the wanted function is called by the title with the player's ped only,
as here); a wrong reading of an argument would be a wrong call inside the title, which faults like any fault of
the title's. A panic in our own work stops the work and not the title.

### Trying it without the keyboard

`LEONIDA_GAME_ACTIONS_RUN="<seconds>:<command>;..."` asks for commands that many seconds after the start: `info`
(what the runtime sees: threads, whether the work ran at once or at the queue's point of rest, the ped, its
position and forward vector, wanted level, clock, the weather types, the cheat table), `cheat <n>`, `wanted <n>`,
`time <h> [m [s]]`, `hours <n>`, `pause <0|1>`, `weather <index|NAME|freeze|free>`, `nudge <dx> <dy> <dz>
[frames]`. Everything asked and done is written to `%TEMP%\leonida-actions\log.txt` (addresses of the run: not
for the repository).

## What is not there yet, and why

God mode, stamina, fast run and swim, super jump, invisibility, ragdoll, the vehicle items other than the nine
spawns, every weapon item, moon gravity, traffic, pedestrians, police, blackout, strong wind, moving the player
(teleports), HUD and radio. These items go through `GameActions` (`actions.rs`); for them the backend answers
with the reason, and the menu shows them dimmed with that reason in the help box. The title's functions for them
were not looked for one by one: the cheat table has none of them, and the script commands (below) have no names.

Moving the player: the ped and its position are known now (above), but the title's function that moves a ped was
not found. Looked for among the script commands (below) by their arguments: 46 commands take a 16-byte handle,
a vector and then at most plain numbers, and give no value back (by a reading of each one's first instructions
that is sometimes wrong); the handful read were other things (a query around a place, a sound at a place), and
without names the rest is a function at a time. `LEONIDA_GAME_ACTIONS_WARP=1` lets a teleport write
the ped's position on the title's thread for the hold time instead (`nudge` does the same, relative): an
experiment to see what the title does with it, since its physics most likely puts the ped back.

### Teleport by memory

The Teleport page and `leonida.teleport(x, y, z [, hold_seconds])` do not use the script natives: they write the
player's position into the title's memory (`apps/leonida/src/player.rs`). The player's transform is a float
matrix (three unit axes and a position row; a 4x4, a 3x4 or a packed 4x3) in the title's writable memory, and a
physics copy of it moves with it, so every transform that moves with the player is shifted by the same amount and
written again every 5 ms for a few seconds (`hold`), so that the physics does not put the player back and it does not
fall through ground that is not loaded yet. The menu items work only when a position is known:

1. **Find it once** (diagnostics build): `LEONIDA_FIND_PLAYER=<start s>,<seconds>` (`player_find.rs`) lists every
   candidate transform at `<start>`, samples their positions every 250 ms while the test script's keys
   (`LEONIDA_KEYS_FILE`) are held, and ranks the ones that stay put when no key is held and move with W/S/D and Shift.
   `LEONIDA_FIND_PLAYER` alone means `90,240`. Besides transforms (strict forms only, so props with an
   identity-like matrix drop out) it tracks position vectors (a float4 whose w is a flag word, or three
   doubles) in the map's bounds, scans on several threads, and drops candidates that never move while keys are held.
   The best candidate is often a copy the game rewrites, so a write probe then tries every triple near it (x and y
   within 0.5 m, or off by a multiple of a 512 to 4096 m sector; z from 0.5 m below to 2.5 m above, f64 1 m below to
   3 m above), 32 at a time (+3 m for 300 ms, halving a batch that has an effect). The one that moves the copy,
   or that moves other copies, is what the teleport writes. A `LEONIDA_TELEPORT` waits for the probe.

   **Status (honest): not working yet.** The finder and probe located a derived copy of the player's position and a
   triple that drives it, but that driver sat about 3.3 m above the feet and is most likely the camera's focus, not
   the player's authoritative position. A teleport through it moved only the camera view, the player stayed put, and
   one write was followed by a 16.8 s title frame stall and a watchdog fault. The writer therefore skips any member
   that is all zeros or more than 5 m from the primary, and the finder leaves bare vectors more than 1.5 m above the
   derived position (camera focus) out of the group. Both the menu items and `LEONIDA_TELEPORT` stay opt-in.
   Then it searches for pointers to the best ones from the title's image (up to `LEONIDA_PLAYER_LEVELS` = 3 levels).
   Everything is written under `%TEMP%\leonida-player\`: `summary.txt`, `candidates.txt`, `tracks.csv`,
   `comovers.txt`, `best.txt`, `pointers.txt` and `paths.txt`. The run that found them can teleport at once.
2. **Use it later**: `paths.txt` (or `LEONIDA_PLAYER_PATHS=<file>`) is read at start-up; a path is a pointer
   chain from a writable section of the image (an RVA, so it holds for the same build only).
3. `LEONIDA_TELEPORT=<delay s>,x,y,z[,hold ms]` teleports once; `LEONIDA_TELEPORT_LIFT=<m>` is added to z;
   `LEONIDA_VICE_CITY=x,y,z` (or `vice.txt` beside the paths) is the target of "To Vice City". Without a known position
   the items keep saying they are not available.

None of the addresses or paths is committed; they are the user's own run's.

With `LEONIDA_GAME_ACTIONS=1` none of this is needed to know where the player is: the position and the heading
are read from the title's own ped ("Game actions" above), which is what the finder's best candidate was a camera's
copy of. Save position and `leonida.player_position()` use it; moving the player is still open (below).

### Looking for the script natives

A trainer for a RAGE title calls the script engine's natives: GTA V's registers each command's handler under a
64-bit hash at start-up, and trainers find the table, or the registration calls, by pattern. This was looked for in
the user's own `game_scarlett_master.exe` (SHA-256 `e95020b2...5a08993`, 120,181,760 bytes; read locally, nothing
of it copied). The image is a plain PE32+ (base `0x140000000`, `.text` `0x5F3F966` bytes, `.rdata` `0x8A0C0C`,
`.data` raw `0x622E00`); no key or decryption is involved in reading it. Counted over the whole image:

| Looked for | Found |
|---|---|
| Names of GTA V natives as text (`PLAYER_PED_ID`, `GET_ENTITY_COORDS`, `SET_ENTITY_COORDS`, `GET_HASH_KEY`, `SET_TIME_SCALE`) and script-engine words (`scrThread`, `scrProgram`, `native`, `.ysc`) | none |
| GTA V-style registration: `mov r64, imm64` (a hash) and `lea r, [rip+x]` (a handler) before a `call`, grouped by the callee | the most-called such callee has 77 call sites; a table of thousands of natives would give one callee thousands |
| Dense blocks of 64-bit immediates that look like hashes (not pointers, not sign-extended), per 16 KiB of `.text` | at most 179 in any block; a registration function would hold some 800 per block |
| Tables of (hash, handler) pairs of qwords in `.rdata` and `.data`, either order, runs of 40 or more | none |
| Runs of 300 or more qword pointers into `.text` in `.rdata` | 1,769 runs, but each has few distinct targets (e.g. 1,261 entries, 136 distinct): vtables, not a table of thousands of distinct handlers |
| Runs of 1,000 or more 32-bit `.text` RVAs | none |
| 32-bit hash registration: `lea rcx, [rip+handler]; mov edx, imm32; call` | one callee with 554 such call sites in a 9.8 KiB function, handlers in `.text`; 4 of its 554 hashes match the one-at-a-time hash of strings in the image (`Water`, `TERRAIN`, `PED`, `decoration`), so it looks like a registry of types by name, not of script commands; too few for GTA V's thousands of natives in any case |

The scripts themselves are no help yet: `script.rpf` and `story000_script.rpf` hold their data in the framing and
proprietary coder `docs/coder.md` describes, so the scripts' native tables (the hashes they call) cannot be read to
match against the executable.

Conclusion: in this build the natives are not registered in any form these searches find (not as GTA V does it, and
not as a plain table). They may be registered from data built at run time or kept in a form that is not plain;
following that would need either a run-time trace of the script VM (finding where a script thread dispatches a
command) or the scripts decoded first. If it turns out to be obfuscated on purpose, the rules say to stop and write
it down here, which is where this stands.

### The script commands, found (2026-11-27)

The searches above looked for a registration (a hash next to a handler). There is none to find, because each
command carries its own hash. Following the title's time-of-day function back from the cheat code led to a
function that does nothing but unpack arguments and call it, and 6,811 functions of the code have that shape:

- one argument, a context: a pointer to the arguments (8-byte slots, which also take the first result), a
  pointer to the result slot at +8, a signed argument count at +10h;
- a test of that count at entry: when it is negative, the function first writes a 64-bit constant into the
  first argument slot, then goes on as usual. The 6,811 constants are all different, and each is in the code
  once: the command's hash. (Counted as `mov rax, [context]; movabs rcx, imm64; mov [rax], rcx` in a function
  that tests `[rcx + 10h]` in its first 48 bytes: 6,889 such stores, 6,811 in such functions.)
- 6,692 of them take a process-wide lock (one global, the same in every one) around the call of the command's
  own function, which takes plain arguments (integers, floats, pointers to 16-byte values);
- they lie in blocks: 1,481 in one MiB, 1,463, 1,230, 1,041, 684, 519 and 254 in six others, 139 elsewhere.

Arguments as read from 6,811 of them: an entity (or another object) is a 16-byte handle, turned into a pointer
by a lookup; a vector is 16 bytes too (three floats); 912 commands take one integer, 615 one 16-byte value, 600
nothing.

What is still not found is how a script reaches a command. Nothing in the file refers to any of the 6,811
functions: no pointer to one in any section, no `call`, `jmp` or `lea` (their starts are in the unwind table;
6 values in the data sections equal one by chance), and no table holds them encoded in a simple way. Tried over `.rdata` and `.data`, at strides of 4
to 48 bytes, as 32-bit and 64-bit values: the gaps between neighbouring functions as differences of consecutive
entries (0 runs of 8), every function plus or xor one constant (0 constants, by four probes), every function
plus, minus or xor its own hash (0 of 6,811), offsets from the entry's own address (1 chance hit), arrays of the
gaps themselves (0). The hashes are not in the data either (0 of 6,811). So the table is built at run time by
something these searches do not see, or is kept in a form made not to be read; that path stops here, as the
rules say. Whether the scripts are bytecode was not settled: the commands' own functions can be called without
them, which is what the game actions above do for the three they need.

A run-time look would now be exact: `LEONIDA_NATIVE_SCAN` counted every pointer into code and drowned (its
candidates in the run of 100 s were all 32-bit noise), but a scan for pointers to these 6,811 functions only
(found in the mapped code by the shape above) would show where the title keeps them and in what layout. Not
built: the actions did not need it.

### Looking for the tables at run time (stage 1)

The static searches above read the file; the title builds its script tables in memory. A diagnostics build
(`--features diagnostics`) can look there: `LEONIDA_NATIVE_SCAN=<seconds>[,<seconds>...]` starts a thread that, that
many seconds after the title starts, walks the process's committed memory (`VirtualQuery`; read through
`ReadProcessMemory` on the own process, so a page that vanishes fails the read instead of the process; nothing is
written to the title). Its own copy buffer is a separate allocation that the walk skips; the runtime's other
allocations cannot be told apart, so the output tags each region (`heap-rw`, `title-image-rw`, ...) instead.

It looks for runs of at least `LEONIDA_NATIVE_MIN_RUN` (64) elements that all point into an executable section of
the title: qwords with a stride of 1 to 8 qwords (an array, `(hash, handler)` pairs, wider buckets; any slot may
hold the handler) and 32-bit values that are RVAs into code. A stride-1 run hides the longer strides inside it. Runs
with at least `LEONIDA_NATIVE_MIN_DISTINCT` (200) distinct handlers are listed (the 200 best), with their overlap,
the share of the union of handlers each covers and, for strided runs, per slot how many distinct values and how
many look like a 64-bit hash. `LEONIDA_NATIVE_ALL=1` also scans read-only and DLL-backed memory.

Stage 1 (a first run, 100 s into the world) was noise: with every integer below the image's code size counted as an
RVA, the union was 2.5 million "handlers" over every MiB of code. Stage 2 makes the scan specific:

- A pointer (or an offset) only counts when its target is a function start: a `.pdata` function begin (299,257 of
  them in the title's `.pdata`), or a 16-aligned byte after `int3` padding (a leaf function has no unwind entry).
  `LEONIDA_NATIVE_LOOSE=1` drops this. 99.6 % of the `.pdata` functions are 16-aligned.
- 32-bit values are off unless `LEONIDA_NATIVE_U32=1`. Then it looks for arrays of offsets from an unknown base:
  handlers are 16-aligned, so offsets from one base share their low four bits; a run of at least 12 non-zero values
  with the same low nibble is checked against the function starts by trying every function start as the target of
  one of its first two values, and kept when at least 90 % of the run lands on function starts. This finds the base
  (the global the interpreter adds the offset to) without finding the interpreter, and base 0 would be an RVA table.
  Per base, the summary lists the arrays, the distinct handlers and the qword slots of the title's writable sections
  that hold the base as an address (the global itself).
- Defaults are now a run of 32 and 64 distinct handlers (`LEONIDA_NATIVE_MIN_RUN`, `LEONIDA_NATIVE_MIN_DISTINCT`).

Output, kept out of the repository because it holds addresses of the title: `%TEMP%\leonida-natives\scan-<s>s\`
with `summary.txt` (memory scanned, function starts, runs by distinct count, the union and its RVA histogram per
MiB, the bases), `candidates.txt`, `targets.txt` (handler RVAs per candidate), `union.txt`, and in 32-bit mode
`offset-arrays.txt` and `offset-handlers.txt` (the handlers per base). Results of a real run go here next, as counts
and method only.

Finding the interpreter statically (to read which global it adds the offset to) was tried on the file and did not
get anywhere, counted over `.text`: 10,789 `jmp reg`, 2,759 of them after a `cmp` and bounds check with a
`lea`-addressed table; only about 20 tables have 200 or more cases, and those are id-to-id maps (`mov eax, imm; ret`
cases), an entity-type switch and parsers on a byte with at most 97 cases; no function of the 161 over 12 KB has a
dispatch on an opcode byte of 150 or more cases, and no `add reg, [rip+global]` before a `call reg` sits in a large
function (the 14 such globals have 1 to 3 sites each, all in small functions). Nothing was de-obfuscated; the
interpreter is simply not found by these signatures, which is why the run-time search above does not need it.

## Tests

- `cargo test -p leonida-menu`: the model (navigation, wrap-around, scrolling, the back stack, toggles, numbers,
  choices, actions, unavailable items), key repeat and debounce, which keys are kept from the title, the clock's
  continuity, the pages, settings lent by an owner, the one word a present asks, `LEONIDA_MENU_OPEN`, the painter's
  layout, the PNG encoder, and GDI glyphs (Windows). No GPU.
- `cargo test -p leonida-gpu --features diagnostics --lib host::trainer`: draws the menu with the overlay's own
  pipeline onto an offscreen D3D12 texture over a made-up backdrop, at its place and below where the panel would
  be, reads both back, checks the banner, rows and backdrop, and saves a PNG (`LEONIDA_MENU_PREVIEW=<path>`, else
  `leonida-menu-preview.png` in the temporary folder); and how the cap and the sharpness are read and written.
  Skipped without a D3D12 device.
- `cargo test -p leonida-hle --features diagnostics --lib -- gameinput clock`: the input shim with the kept keys in
  place, and the title's clock.
- `cargo test -p leonida-loader --lib game_actions`: the ten places on made-up code (found wherever they lie,
  displacements and numbers read, a place missing, twice or pointing outside left out), the hook on made-up code
  that runs (the update step calls a function of ours and returns as before) and, with `LEONIDA_GAME_DIR` set to
  the package's folder, every place once in the retail executable, functions in code and globals behind it.
- `cargo test -p leonida game_actions`: the script's commands, the weather names and their hashes, the menu's
  ids, the closure the title's queue is given. `cargo test -p leonida-menu --lib actions`: the game hook answers
  its items and the rest say why not.
- `cargo run -p leonida-menu --example preview -- <out.png> [height] [main|player|display]`: the same picture
  painted on the CPU alone.

None of these runs the title. What only a run can show is listed under Status at the top.
