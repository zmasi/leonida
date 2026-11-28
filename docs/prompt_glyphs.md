# Button prompt glyphs

The title draws its button prompts from one Scaleform movie, and it has only the Xbox set: the dump holds no
PlayStation or keyboard glyphs. This note records the movie's format, which export is which button and who asks for
them, measured on the user's own `data.rpf`. It holds no shapes. `tools/ps_prompt_glyphs.py` builds an original
PlayStation set, `tools/pc_prompt_glyphs.py` a keyboard-and-mouse set from pictures the player supplies, and the runtime
serves the one for the device in use.

## The movie

| fact | value |
|---|---|
| where | `data.rpf`, a zlib stream at archive offset `0x117d7000` (found by zlib scanning; the TOC is not used) |
| size | 9,644 bytes stored, 28,148 bytes inflated (the inflated length is also the length field of the header) |
| signature | `GFX` (uncompressed Scaleform container), version byte 42 (`GFX*`), then the u32 file length |
| stage | 1 frame, 30 fps, frame label `platform_xbsx_input_icons_Main` |
| tags | GFx exporter info (1000, library name `input_icons`), FileAttributes (69: ActionScript 3), background colour, frame label, 131 DefineShape3, 38 DefineSprite, ExportAssets (56), DoABC (82, 9,516 bytes, lazy-init name `frame1`), SymbolClass (76, 39 entries), ShowFrame, End |
| symbols | 38 exports `platform_xbsx.input_icons.Main_Button_<N>` (N = 4..39, 58, 59), each a one-frame sprite of 2 to 5 vector shapes (no bitmaps); the SymbolClass binds each sprite to the class `platform_xbsx.input_icons:Main_Button_<N>` and one more entry, id 0, to the document class `Main` |
| canvas | every sprite is 1280 x 1280 twips (64 x 64 px), registration at the top left; exceptions: N = 16 and 25 (4..1281 x 21..1265), 34 and 36 (the bumpers, about 1279 x 775 starting at y 252) and 35 and 37 (the triggers, x 31..1254) |
| drawing | counted over all 131 shapes: 103 have one solid fill and no stroke, 28 one stroke and no fill; the fills are opaque white (41), black (55) and grey 103 (7, the unlit D-pad), every stroke is opaque black and 60 twips (3 px) wide; all 131 placements are PlaceObject2 with a translation only (no scale, rotation or colour transform) |

The symbol name is built outside this movie, by the prompt class of the larger movies (next section); the executable
also holds the format string `platform_xbsx.input_icons.Main_Button_%d`. Character ids of the original sprites follow
the export order, 5, 10, 16, 20, ..., 169 (shapes in between); a replacement does not need them, only the export names
and the class names. Its 131 shapes and 38 sprites take 15,226 bytes; the other tags and the header take 12,922.

## Who asks for a glyph, and when

Read from the ActionScript blocks of the user's own movies with a disassembler written for this note (2026-11-27;
none of its output is kept here).

- **This movie holds no logic.** Its block (9,505 bytes of ABC, 44 classes) is a symbol library: the document class
  `Main` (a `Sprite`, through `rockstar.core.Gfx`) with 38 constants bound to 38 empty `mx.core.SpriteAsset`
  subclasses, and three helpers (a string hash, a stub of Scaleform's extensions, an `ExternalInterface` wrapper). It
  has no frame script, no event listener and no timer; its constructors turn Scaleform's extensions on and register a
  sound callback. Nothing in it reads a platform or a device.
- **The larger movies ask.** Three movies of the archive carry a copy of the class `rockstar.input_icons.InputIconV2`
  (exporter names `generic_movie`, `hud` and `map_scripted_movie`, at `0x1176f000`, `0x11786000` and `0x117da000`, each
  stored as zlib chunks of 64 KiB), and the HUD movie one more class with the same loader:
  - `loadAssets()`, static, called from the movie's `Main` constructor: unless it has already done so, it loads the
    URL `input_icons.gfx` through a `flash.display.Loader` and fixes the name prefix,
    `"platform_" + flash.system.Capabilities.os.toLowerCase() + ".input_icons.Main_Button_"`. The platform part is the
    player's `Capabilities.os`, a constant of the build, not a device.
  - `init(...)`, reached from `initializePress`, `initializeHold`, `initializeMash`, `initializeRotate` and their
    variants, so **each time a prompt is set up**: the icon container's children are removed, the class named
    `prefix + iconID + DEVICE_PREFIXES[device]` is looked up in the loaded movie (`applicationDomain.getDefinition`),
    and a new instance of it is added. Until the movie is loaded a white disc is drawn instead.
  - `DEVICE_PREFIXES` is a static list of name suffixes, one empty string to begin with; the static
    `setAvailableDevices(list)` replaces it. No ActionScript in the three movies calls `setAvailableDevices`, and their
    own calls of the initialisers pass device 0. What the executable passes when it calls them is not in the movies.

## N to button

Read from rendered sprites. `tools/xbox_prompt_glyphs.py` is the rasteriser of the DefineShape3 records:

```powershell
python -I tools/xbox_prompt_glyphs.py --from-archive "<package>/data.rpf" --out <folder outside the repository>
```

writes one PNG per export (named after it, the 64 x 64 canvas and a pixel around it at 4x), a contact sheet `sheet.png`
and a listing `README.txt`. The pictures are the game's artwork: the tool refuses a folder inside the repository, and
they are never committed. Some outlines leave the 64 x 64 canvas by up to 0.3 px (N = 6: x from -0.30), hence the
margin. It stops with an error on anything it does not draw (gradient or bitmap fills, colour transforms, clip layers);
this movie has none.

| N | button | N | button |
|---|---|---|---|
| 4 | D-pad up lit | 24 | right stick right |
| 5 | D-pad down lit | 25 | right stick press (R3) |
| 6 | D-pad left lit | 26 | right stick, plain |
| 7 | D-pad right lit | 27 | right stick up/down |
| 8 | D-pad, nothing lit | 28 | right stick left/right |
| 9 | D-pad, all lit | 29 | right stick rotate (arrow on the left) |
| 10 | D-pad up/down lit | 30 | A |
| 11 | D-pad left/right lit | 31 | B |
| 12 | left stick up | 32 | X |
| 13 | left stick down | 33 | Y |
| 14 | left stick left | 34 | LB |
| 15 | left stick right | 35 | LT |
| 16 | left stick press (L3) | 36 | RB |
| 17 | left stick, plain | 37 | RT |
| 18 | left stick up/down | 38 | Menu (three bars) |
| 19 | left stick left/right | 39 | View (two overlapping windows) |
| 20 | left stick rotate (arrow on the left) | 58 | left stick rotate (arrow on the right) |
| 21 | right stick up | 59 | right stick rotate (arrow on the right) |
| 22 | right stick down | | |
| 23 | right stick left | | |

Not identified: nothing is unknown by shape, but the Xbox/Guide button has no glyph in the set, and which of the two
rotate variants (20/29 against 58/59) the title picks for which prompt is not known. N 8 and 9 are told apart only
by the lighting; which one the title uses for a plain "D-pad" prompt is not measured.

## The PlayStation package (not read)

The user's PS5 copy is one package file, `EP1004-PPSA38500_00-GRANDTHEFTAUTO00-A0102-V0102.pkg` (212,421,953,822
bytes; header magic `7F 46 49 48`). Samples of 64 KiB at 5, 25, 50, 75 and 95 % of the file measure 7.85-8.00 bits a byte:
the game's files inside are encrypted (a package's image is, until the console's keys open it). Under the ground rules
nothing is decrypted, so the title's own PlayStation glyph movie, if the PS5 build has one, is not read from it; the
replacement below stays the PlayStation set. A PS5 build's files already extracted by the user's own console as a plain
folder could be read like the Xbox archive (2026-11-27).

## The PlayStation replacement

`tools/ps_prompt_glyphs.py` writes a movie with the same export names, canvas sizes and class names:

| Xbox N | replacement |
|---|---|
| 30, 31, 32, 33 (A, B, X, Y) | cross, circle, square, triangle in blue, red, pink, green |
| 34, 36 / 35, 37 | L1, R1 / L2, R2 |
| 38 / 39 | Options / touchpad (the console's View is the touchpad on the PlayStation layout) |
| 16, 25 | L3, R3 |
| 4..11 and 12..29, 58, 59 | the same d-pad and stick prompts, drawn again |

Shapes are discs, polygons, thick polylines and a small stroke font written in the tool. See the tool's docstring for
the options (`--out`, `--zlib`, `--pad-to`, `--selftest`). The movie is 13,551 bytes, 5,721 as one zlib stream, below the
original stored 9,644, so it can be padded to that size. The ActionScript block is minimal (empty `MovieClip`
subclasses, no `Main` behaviour, no sound hook); whether the title's loader needs more has not been tried in the game.

## The keyboard and mouse replacement

`tools/pc_prompt_glyphs.py` (standard library only) turns a folder of pictures, one `*Main_Button_<N>.png` per N, into
the shapes of the same 38 sprites. The pictures are the player's: the set this was written for was drawn by a friend
of the user from scratch (W/A/S/D and arrow keys with one or more keys lit, a mouse in a circle for the right stick,
key caps for the face buttons and shoulders), its licence is not stated, and neither the pictures nor a movie built
from them is in the repository.

- **Pictures.** 8-bit PNG, the 64 x 64 canvas with one more canvas pixel on every side, at a whole multiple (264 x 264
  is 4x), three flat colours with anti-aliased edges: black, white and the grey (103, 103, 103).
- **Tracing.** Each pixel is split into how much of it is covered at all, by white and by grey; the half-covered line
  of each is followed between the pixel centres, placing an edge by the coverage of the two pixels it lies between
  (exact for a straight edge; interpolating to one half would be up to 0.09 pixels out). Each closed line is one
  painted piece, outermost first: everything opaque in black, white and grey areas over it, a letter over those, its
  counter over the letter. Only a hole through the whole picture is cut out.
- **Edges.** Straight edges and quadratic curves that stay within `--tolerance` (0.03 canvas pixels) of the traced line,
  none across a bend. A corner the anti-aliasing rounded is put back where the long edges on its two sides cross, when
  that crossing is as near the traced line as a sharp corner leaves it; a corner the drawing rounds stays round.
- **Sharing.** A line that is the same in several pictures or at several places is defined once (the eight W/A/S/D
  pictures share their outline, letters and key faces), and pieces that always appear together are one shape.
- **Size.** On that set: 76 distinct lines for 300 painted pieces, 53 shapes placed 230 times, 7,067 bytes of shapes
  and sprites of the 15,226 the title's movie leaves (the Xbox set fills them; the PlayStation set takes 7,136).
- **Check.** The tool reads the movie it wrote back, draws every sprite at the pictures' scale with exact-area
  anti-aliasing and compares: on that set 0.01 % to 0.48 % of a glyph's opaque area differs (4,207 of some 1.6 million
  opaque pixels in all), no pixel is more than half wrong (the largest single difference is 40 %), and what is left
  sits on the edges: a twip is a fifth of a 4x pixel and the pictures have edges on quarters and thirds. The glyphs
  furthest out are the lit W/A/S/D and arrow sets (N 17, 9, 18) and the rotate arrows (N 58, 59, 20, 29).
  `--sheet <png>` writes those drawings as a contact sheet.
- **Not checked.** The movie has not run in the title (2026-11-27). It differs from the confirmed PlayStation movie in
  its shapes only, which use the level and upright edge records that movie does not (the title's own shapes have 807
  of them).

```
python -I tools/pc_prompt_glyphs.py --glyphs <folder> --from-archive "<package>/data.rpf" --out pc_prompt_glyphs.gfx
python -I tools/pc_prompt_glyphs.py --selftest      # pictures made by the tool itself, no game file
```

## How the runtime swaps the movie

The D3D12 runtime (`leonida-hle`, `modules/glyphs.rs`, hooked in `modules/dstorage.rs` `execute`) answers the title's
read of the movie with a movie built on the player's PC, chosen by the family in use at that moment
(`gameinput::active_family()`, see [gameinput.md](gameinput.md)): the PlayStation one for a PlayStation pad, the
keyboard one for the keyboard and mouse, the title's own for an Xbox pad. The archive is never written and its table of
contents is not touched or needed.

- **What is matched.** A DirectStorage request is a file object (our `dstorage` file keeps the path the title opened it by),
  an offset, a stored size, options and a destination size. A zlib request on a file named `data.rpf` at offset
  `0x117d7000` whose destination size is `28,148` is the movie. The stored size is not looked at, because the runtime
  never reads the archive for it: it fills the destination with the replacement itself. Whatever the table of contents says
  the stored size is (it is encrypted and unused here, so it is not known), the title gets the replacement as if
  inflated. A replacement stream written into the archive instead would have to fit that stored size (ours would:
  5,721 bytes against 9,644 for the standalone movie), but that needs the archive changed and the stored size known; the
  runtime does neither.
- **What is served.** `ps_prompt_glyphs.gfx` or `pc_prompt_glyphs.gfx` beside `leonida.exe` (or one of the six folders
  above it), or the file `LEONIDA_PS_GLYPHS` or `LEONIDA_PC_GLYPHS` names (`off` leaves the title's movie in place for
  that family). It must be exactly 28,148 bytes, begin with `GFX` or `FWS`, and say so in its length field; otherwise
  one warning is logged and the title keeps the Xbox movie. A file is read once, when its family first needs it.
- **When.** At the title's read, so the family at that moment decides: what is connected at start (a Sony pad counts as
  PlayStation, no pad at all as the keyboard and mouse, before any input), or `LEONIDA_PROMPTS`. The title reads the
  movie once, about 3 s after it starts: one `the title reads its glyph movie` line in the log of a 15 minute session
  with the menus opened (2026-11-27). Picking up another device later does not show until the next start; see the
  last section.
- **Builds.** `python -I tools/ps_prompt_glyphs.py --from-archive "<package>/data.rpf" --out ps_prompt_glyphs.gfx` builds
  over the title's own movie: the shapes are replaced and every other tag (the ActionScript, export and class tables) is
  kept as it is, so the title's code finds the symbols as before. The output holds those tags of the game's movie: keep it
  on your PC, never commit it. `--pad-inflated 28148 --out ps_prompt_glyphs.gfx` builds the standalone movie
  (with its own ActionScript block, which has never run in the title) padded to the same length.
- **Checking it.** Run with `LEONIDA_PROMPTS=playstation` (or `keyboard`) and look in the log for
  `prompt glyphs: PlayStation movie loaded` (or `keyboard movie loaded`) and `prompt glyphs: the title reads its glyph
  movie`, which names the family and the glyphs served. If the second line never appears, the title does not read the
  movie through a zlib DirectStorage request on `data.rpf` (or not at that offset); `LEONIDA_DSTORAGE_TRACE=2` lists the
  requests. The warning `a read at the glyph movie's offset is not the movie` means the offset matched but the file or
  size did not.
- **Tried in the title (2026-11-27, Series X profile, `--from-archive` movie, `LEONIDA_PROMPTS=playstation`, no pad).**
  Both log lines appeared about 4 s after start, and the pause menu's prompts showed the replacement glyphs with the
  usual PlayStation mapping (Photo Studio on square, Select on cross, Back on circle): Scaleform accepts the movie and
  the swap at the DirectStorage read works. Not tried yet: a real DualSense or DualShock 4 over HID, Bluetooth, and
  whether the title reads the movie again after start (a live switch).

## Following a device switch during play

Not possible by serving a movie, as far as the movies' bytes and one measurement say. What is certain, and what is a
guess:

- **Certain: a prompt takes its glyph when it is set up, from the movie loaded then.** `InputIconV2.init` makes a new
  instance of the symbol class on every `initialize...` call, so prompts set up after a new movie were loaded would
  show it; prompts already on screen keep their sprite.
- **Certain: nothing loads the movie twice.** Each movie that carries the class loads `input_icons.gfx` once, guarded by
  a static flag, and the runtime sees one read of it in a session. Guess: when another of the three movies is loaded
  later, Scaleform hands its loader the definition it already holds, which is why no second read arrives; whether the
  title ever drops all holders (a return to the start screen?) is not measured.
- **Certain: one movie cannot carry the families.** The served movie is exactly 28,148 bytes, 12,922 of them tags that
  must stay. The three sets' shapes and sprites are 15,226 (Xbox), 7,136 (PlayStation) and 7,067 (keyboard) bytes; the
  Xbox set alone fills the room.
- **Certain: the title's code has a way to pick a family per prompt, and nothing here reaches it.** The class name ends
  in `DEVICE_PREFIXES[device]`: with the list set through `setAvailableDevices` and a device index per prompt, one
  movie with suffixed export names serves several devices. On this build the list is one empty suffix, no
  ActionScript changes it, and the movies' own callers pass device 0. Guess: the executable passes 0 too (its only
  format string has no suffix, and the title is told every pad is an Xbox pad).
- **Certain: sprites of several frames do not help.** The instances are `Sprite` subclasses made with `new`; nothing
  calls `gotoAndStop` on them, and the glyph movie has no code that could.
- **What would do it, none of it done:** ActionScript of our own in the served movie (it runs in the title's player:
  it could find `InputIconV2` and change `DEVICE_PREFIXES`), which still has no way to learn the family in use, since
  file reads are all the runtime answers for the player and this file is read once; making the title load the movie
  again, which is a change inside the title; or replacing the glyphs where Scaleform draws them, in the D3D12 layer.
  All three put code or assumptions into the title beyond answering one read, so `LEONIDA_PROMPTS_LIVE` does not exist.
