# VI-Patch on the Xbox build

[VI-Patch](https://github.com/ZiZc3/VI-Patch) by ZiZc3 is a set of game patches for the PS5 build of the title
(`PPSA38500`, version 01.002.001), written for [PS Patch System](https://github.com/illusionyy/ps-patch-system) by
illusionyy. This note records which of them Leonida applies to the Xbox Series build, where, and why the others are
not here. Read at commit `a4ee584` (2026-11-27), with its history and its tags `v1.0` and `v1.1`.

**Licence and credit.** VI-Patch has no licence, so nothing of it is copied here. Its patches are lines of
`Address` and `Value`: new bytes at addresses of the PS5 executable, with no original bytes. Neither is of use on
another executable. What is taken is what each patch does, from its README and the notes of an earlier README
(commit `2266e62`); the places below were found in the Xbox executable by what the code there does, and the bytes
written are Leonida's own. The idea of each patch is ZiZc3's.

None of the eight is a protection, licence or online patch: they change the frame rate, the picture and the game.

**Not run in the game.** The places were checked offline only (below). Nothing here has been seen working in a
run yet.

## The list

| VI-Patch | Here | Switch | Place in the 1.0.2.1 executable (RVA) |
|---|---|---|---|
| 60 FPS | ported, always on | none | `0x224359a`, `0x1e0fc62` |
| No Vignette | ported, off by default | `LEONIDA_VIPATCH_NO_VIGNETTE=1` | `0x496ab6`, `0x496b51` |
| Skip intro logos | not ported: no such place in the executable (Bink is a DLL here) | | |
| Weather fix | not found | | |
| First person | not found | | |
| No clothing | not ported: new PS5 code that calls PS5 functions by address | | |
| Mandy replaces Lucia | not ported: the same | | |
| No depth of field | not ported: its definition is not in the repository | | |

The switch is an environment variable read at load, `1` for on. The F1 settings menu has it as
`NO VIGNETTE (VI-PATCH)`; it applies at the next start.

## How a patch is applied

`d3d12/crates/leonida-loader/src/vipatch.rs` holds the table; `hooks::game_patches` applies it when the title's
executable is mapped, before any of its code runs (the title's DLLs are left alone).

- A place is a pattern of its bytes, `??` for the displacements another build changes. It is taken at the RVA
  above if the pattern is there, else searched for in the image's code, where it must be exactly once.
- The bytes replaced are bytes of the pattern, so they are checked before they are written.
- A patch is written only if all its places are found. The log has `hook applied` for each place, or
  `game patch not applied` with the place that failed.

The 1.0.2.1 executable is `game_scarlett_master.exe`, 120,181,760 bytes, SHA-256
`e95020b2fa4a721db3df59fdecf75621730ac559d8835252f6c3b658d5a08993`.

## 60 FPS

VI-Patch: removes the 30 fps cap; on a PS5 it needs a 120 Hz output forced. Its definition is only in the
`v1.1` release archive, not in the repository at any commit, and the archive was not read. The change below is
from the Xbox executable alone.

How the title limits its frame rate, measured in the executable:

| step | where (RVA) | what |
|---|---|---|
| limit | video settings, a `u32` at `+0x210` | frames a second; 0 is no limit |
| interval | `0x224359a` and `0x1e0fc62`, the same code in two functions that work on the video settings (`0x22434a0`, `0x1e0fb30`) | limit bounded to 1000, then 1 000 000 / limit, stored as microseconds in a global |
| rate and frame lock | function `0x247ab10` | rate = 1 000 000 / interval; frame lock = output refresh / rate, 4 at most (1 if the rate is above the refresh) |
| length | table at `0x61ece60`, by output refresh and frame lock | 60 Hz: 16 667, 16 667, 33 333, 50 000, 66 667; 120 Hz: 8 333, 8 333, 16 667, 25 000, 33 333 |
| the call | `0x247abd8` | device slot `0x270`, `SetFrameIntervalX(output, length, period, flags)`: Leonida's `frame::set_interval` |
| the title's own limiter | function `0x120530` | when it is on, waits out a frame time worked out from the same interval |

Untouched, the title asks for 33 333 microseconds: 30 frames a second. The patch replaces the load of the limit,
`mov eax, [register + 0x210]`, with `mov eax, 60` in both places, so every later step follows: the interval is
16 666, the frame lock 1 at a 60 Hz output (2 at 120 Hz), the length 16 667. That bound-and-divide code is in the
executable twice, these two places; the interval global has one more writer, a start-up initialiser that stores
1000 (no limit) before the settings are applied.

What can still hold the rate under 60:

- `LEONIDA_FPS_CAP` (the F1 menu's FRAME RATE CAP, the F4 menu's frame-rate cap): unset, the frame clock of
  `leonida-gpu/src/frame.rs` ticks at the title's interval, now 60 a second (the menu's `GAME (60)`); a value
  saved there replaces it, and `30` is how to have the old pace back. The play scripts do not set it.
- The present itself does not wait: `Present(0, 0)` on a flip-discard swap chain, no vertical sync.
- The PC: the title waits for a tick before each present, and a frame that takes longer than 16.7 ms is simply
  late.

## No Vignette

VI-Patch: no dark corners when driving fast or aiming. Two lines, each a conditional jump made unconditional; the
earlier README says which: fast driving and aiming have exposure profiles of their own, `SPOTMETER_fast` and
`SPOTMETER_aiming`, and the patch skips those two.

In the Xbox executable the function at `0x496970` blends exposure profiles looked up by the `joaat` of their
lower-cased names, one for each state that is on and `spotmeter_default` last. `joaat("spotmeter_aiming")`,
`0x1791bd90`, and `joaat("spotmeter_fast")`, `0xf2b6fe6c`, are each in the executable once, in that function,
each after a short conditional jump over its lookup and blend:

| place | before | after | skips |
|---|---|---|---|
| `0x496ab6` | `jne` (`75`) | `jmp` (`EB`) | the aiming profile (a flag byte compared with 1) |
| `0x496b51` | `jne` (`75`) | `jmp` (`EB`) | the fast profile, and the second profile the same block can pick in its place |

The other profiles are untouched.

## Not here

- **Skip intro logos.** VI-Patch adds a check to the Bink reader that is linked into the PS5 executable: a movie
  whose header has the logo movie's size field is given a frame count of 1, so it ends at once. The Xbox build has
  Bink in `Bink2Scarlett.dll`, so the executable has no such place. The movie is the same: `movies.rpf` holds one
  `KB2n` stream of 3840x2160 and 450 frames ([movies.md](movies.md)), at archive offset `0x3f9bc000`, and its
  size field is the value the PS5 patch compares with, `0x050e5fa4`. A port would be a hook of the runtime's on
  the title's Bink calls or a detour in the DLL, and it wants a game run to check; neither was done.
- **Weather fix.** Seven lines that rewrite arithmetic inside one PS5 function. The constants are the patch's
  own and there are no original bytes, so nothing names the function in the Xbox executable. Whether the Xbox
  1.0.2.1 build has the fault (weather stuck on sunny days) is not known either.
- **First person.** The repository has no definition for the current patch (R3 with D-pad right). The earlier
  README had a two-line one for L2 with R3: stores to two fields of one object, at `+0x98` and `+0xa8`. A search
  of the Xbox executable for a function of that shape (constant stores of both 0 and 1 at `+0x98` and a use of
  `+0xa8`) finds none, and another compiler need not have kept the shape: not found. The open questions of
  [modding.md](modding.md)'s first-person mod stay open.
- **No clothing** and **Mandy replaces Lucia.** Each is new code (890 bytes, and 233 and 1,127 bytes) put in
  unused space of the PS5 executable, with jumps into it from the game's code. The code calls about twenty
  functions of the PS5 executable by address, reads its data by address and passes arguments the PS5 way
  (System V; the Xbox build uses the Microsoft convention). Without the PS5 executable those functions cannot be
  named, so there is nothing to find here. Leonida's own mods swap models another way
  ([modding.md](modding.md)).
- **No depth of field.** Like 60 FPS, defined only in the `v1.1` release archive (`VI-Patch-v1.1.zip`), and the
  README says only that the effect is turned off, which does not name a place.

## Checking it

No game files are needed for the unit tests (the pattern search, the plan, the table):

```powershell
cargo test -p leonida-loader --lib vipatch
```

With `LEONIDA_GAME_DIR` set to the folder of the package, the same run also searches the code of
`game_scarlett_master.exe` for every pattern. On 1.0.2.1 (2026-11-27): each of the four found exactly once, at
`0x224359a`, `0x1e0fc62`, `0x496aaf` and `0x496b4e` (the two vignette patterns begin 7 and 3 bytes before the
byte they change).
