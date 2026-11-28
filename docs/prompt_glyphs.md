# Button prompt glyphs

The title draws its button prompts from one Scaleform movie, and it has only the Xbox set: the dump holds no
PlayStation glyphs. This note records the movie's format and which export is which button, measured on the user's own
`data.rpf`. It holds no shapes. `tools/ps_prompt_glyphs.py` builds an original replacement set.

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

The shared ActionScript builds the symbol name from `"platform_" + capabilities.toLowerCase()` and
`.input_icons.Main_Button_<iconID>`; the executable also holds the format string `platform_xbsx.input_icons.Main_Button_%d`.
Character ids of the original sprites follow the export order, 5, 10, 16, 20, ..., 169 (shapes in between); a replacement
does not need them, only the export names and the class names.

## N to button

Read from rendered sprites (our own rasteriser of the DefineShape3 records).

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

## How the runtime swaps the movie

The D3D12 runtime (`leonida-hle`, `modules/glyphs.rs`, hooked in `modules/dstorage.rs` `execute`) answers the title's
read of the movie with the movie built by `tools/ps_prompt_glyphs.py` while a PlayStation pad is in use
(`gameinput::active_family()`, see [gameinput.md](gameinput.md)). The archive is never written and its table of contents
is not touched or needed.

- **What is matched.** A DirectStorage request is a file object (our `dstorage` file keeps the path the title opened it by),
  an offset, a stored size, options and a destination size. A zlib request on a file named `data.rpf` at offset
  `0x117d7000` whose destination size is `28,148` is the movie. The stored size is not looked at, because the runtime
  never reads the archive for it: it fills the destination with the replacement itself. Whatever the table of contents says
  the stored size is (it is encrypted and unused here, so it is not known), the title gets the replacement as if
  inflated. A replacement stream written into the archive instead would have to fit that stored size (ours would:
  5,721 bytes against 9,644 for the standalone movie), but that needs the archive changed and the stored size known; the
  runtime does neither.
- **What is served.** `ps_prompt_glyphs.gfx` beside `leonida.exe` (or one of the six folders above it), or the file
  `LEONIDA_PS_GLYPHS` names (`off` turns the swap off). It must be exactly 28,148 bytes, begin with `GFX` or `FWS`, and say so
  in its length field; otherwise one warning is logged and the title keeps the Xbox movie. The file is read once.
- **When.** At the title's read, so the family at that moment decides: the pad held at start (a Sony pad connected counts as
  PlayStation before any input), or `LEONIDA_PROMPTS=playstation`. Whether the title loads the movie once at start or each
  time a menu loads is not known (nothing has run); if once, plugging in or picking up a PlayStation pad later does not
  show until the next start, and a live switch is impossible without a way to make the title load the movie again.
- **Builds.** `python -I tools/ps_prompt_glyphs.py --from-archive "<package>/data.rpf" --out ps_prompt_glyphs.gfx` builds
  over the title's own movie: the shapes are replaced and every other tag (the ActionScript, export and class tables) is
  kept as it is, so the title's code finds the symbols as before. The output holds those tags of the game's movie: keep it
  on your PC, never commit it. `--pad-inflated 28148 --out ps_prompt_glyphs.gfx` builds the standalone movie
  (with its own ActionScript block, which has never run in the title) padded to the same length.
- **Checking it.** Run with `LEONIDA_PROMPTS=playstation` and look in the log for `prompt glyphs: PlayStation movie loaded` and
  `prompt glyphs: the title reads its glyph movie`. If the second line never appears, the title does not read the movie
  through a zlib DirectStorage request on `data.rpf` (or not at that offset); `LEONIDA_DSTORAGE_TRACE=2` lists the
  requests. The warning `a read at the glyph movie's offset is not the movie` means the offset matched but the file or size
  did not.
- **Tried in the title (2026-11-27, Series X profile, `--from-archive` movie, `LEONIDA_PROMPTS=playstation`, no pad).**
  Both log lines appeared about 4 s after start, and the pause menu's prompts showed the replacement glyphs with the
  usual PlayStation mapping (Photo Studio on square, Select on cross, Back on circle): Scaleform accepts the movie and
  the swap at the DirectStorage read works. Not tried yet: a real DualSense or DualShock 4 over HID, Bluetooth, and
  whether the title reads the movie again after start (a live switch).
