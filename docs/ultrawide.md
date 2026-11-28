# Ultrawide (21:9, 32:9)

How the output size goes from the window to the title and back to the screen, and what the opt-in ultrawide settings
change. **Measured** means a run, a test or the code shows it (file:line quoted, line numbers as of this note);
**hypothesis** means nobody has checked it yet.

## Settings

| variable | default | what it changes |
|---|---|---|
| `LEONIDA_ASPECT` | unset | `21:9`, `32:9`, `43:18`, any `W:H` or decimal from 1 to 4, or `auto` (the primary monitor's shape). The display mode the console output reports is widened to that aspect at the same height, and so is the title's window. `off`, `none` or empty: unset. |
| `LEONIDA_PRESENT_SCALE` | `stretch`, or `fit` with `LEONIDA_ASPECT` | `fit`: a frame whose shape is not the window's keeps its aspect, centred, with black bars beside it (pillarbox) or above and below (letterbox). A frame of the window's shape fills it. `stretch`: the frame fills the window, as before. |

`scripts/play.ps1 -Aspect 21:9` (and `-PresentScale fit|stretch`) sets them. The arithmetic is in
`runtime/src/aspect_fit.h` and `runtime/test/aspect_fit_test.cpp` tests it.

| `LEONIDA_DISPLAY_MODE` | `LEONIDA_ASPECT` | the title is told |
|---|---|---|
| unset (3840x2160) | `21:9` | 5040x2160 |
| unset | `32:9` | 7680x2160 |
| `2560x1440` | `21:9` | 3360x1440 |
| `2560x1440` | `43:18` or `auto` on a 3440x1440 monitor | 3440x1440 |
| `2560x1440` | `32:9` | 5120x1440 |

## From window to title to present

1. **The display mode the title is told.** `rt::d3d12::title_display_mode()` (`runtime/shims/xbox_display.cpp:324`)
   answers the console output's slot 21 (`output_modes`, `:230`), its `GetDesc` desktop coordinates (`:178`),
   `GetDisplayModeList` (`:202`) and `FindClosestMatchingMode`. It reads `LEONIDA_DISPLAY_MODE` (default 3840x2160,
   `runtime/src/display_mode.h`) and now widens the result with `LEONIDA_ASPECT` (`title_aspect_ratio()`, `:294`).
   Measured (docs/port_status.md, "The title makes its window at 3840x2160"): the 1.0.2.1 title calls slot 21 just
   before its first `SetFrameIntervalX`, but its window and back buffers stay 3840x2160 whatever slot 21 reports.
   1920x1080 was tried at 60 and 120 Hz.
2. **The size the title renders at.** This size is compiled into the executable: the Series X SDR mode at RVA
   `0x0615c100` (`runtime/src/display_profile.h:46`), the scanout reservation, the temporal scene scale and the
   Settings viewport. `runtime/src/main.cpp:65-82` patches these when `LEONIDA_DISPLAY_MODE=2560x1440`. Every other
   output is refused (`display_profile.h:100`). Measured: with the SDR mode set to 1920x1080 the title still made
   2560x1440 buffers. `LEONIDA_ASPECT` does not change the executable.
3. **The window.** The title makes its own top-level window. `runtime/shims/title_window.cpp:36` (`fit`) shrinks it
   to the work area, keeping its shape. With `LEONIDA_ASPECT` it now widens the window first, keeping the height. When
   the title has no window, the runtime makes one at the first plane's size (`runtime/shims/xbox_window.cpp:496-509`),
   and that one is widened the same way.
4. **The swapchain** follows the window's client area: `currentExtent`, or the `GetClientRect` callback when the
   surface leaves it open (`runtime/src/vulkan_present.h:1401-1405`; the callback is in
   `runtime/shims/d3d12_vulkan_device.cpp`, `create_window_presenter`).
5. **The present.** `XboxPresentation::PresentX` (`runtime/shims/xbox_present.cpp`) sends a plain SDR plane to
   `VulkanPresenter::present` (`:326`). It sends a plane with a placement, a filter, HDR10 or an overlay to
   `present_planes` (`:236`).
   - Before this change both paths stretched. The plain blit's destination was the whole swapchain image (`dstOffsets`
     0..extent). The composed planes' placements were fractions times the window size. A 16:9 frame in a 21:9 window
     was stretched by 1.31.
   - Now, with `Scaling::fit`, the plain blit clears the swapchain image to black and blits into `aspect::fit_box`
     (`vulkan_present.h:879-898`). The composed path puts every placement into the same box (`:991-1009`), and the
     pass already writes black outside plane 0 (`runtime/src/present_compose.comp:140`). The box comes from plane 0's
     source rectangle and placement (`aspect::content_aspect`): a 16:9 frame is pillarboxed and a frame of the window's
     shape fills it. Shapes that differ by less than a pixel fill the window.
   - The default stays `stretch` when `LEONIDA_ASPECT` is unset, so the existing present tests are unchanged. One of
     them, `xbox_present_test`, checks that a half-width source rectangle is stretched over the window.

## What is known and what is not

- Measured (unit test only): the parsers, the widened modes in the table above, and the pillarbox and letterbox
  boxes. `aspect_fit_test` passes on Windows with MSVC.
- Not yet run on a GPU: the fit blit and the fit compose. Neither has been checked on swapchain pixels yet. A
  `xbox_present_test` case with `set_scaling(fit)` in a 21:9 window would cover both.
- Hypothesis: the title keeps rendering 16:9 under `LEONIDA_ASPECT`, because its frame size is the compiled SDR mode
  (item 2). In that case the visible result is a wide window with a pillarboxed 16:9 frame: correct, but not yet
  "ultrawide".
- Unmeasured, needs the game running:
  - whether the title reads slot 21's width anywhere: camera aspect or FOV, HUD anchoring, the Settings screen's
    resolution list;
  - whether extending `display_profile.h` to a wide SDR mode (for example 3440x1440, with the scanout reservation
    from `scanout_pool_bytes`) makes the title render wide. 1920x1080 did not take (it gave 2560x1440), which
    suggests that the title clamps or picks from a list. A wide mode may be refused the same way;
  - if it does render wide: whether the projection follows the back buffer's aspect (Hor+) or is fixed at 16:9
    (vertical stretch or crop), and whether the HUD and menus stay in the centre 16:9. Any of these may need a patch
    of its own later.
- Not covered: exclusive full-screen and monitor mode changes. The window stays a desktop window.

## The D3D12 runtime (`d3d12/`)

The same two settings, read by the D3D12 runtime's own code. Nothing of this section has run in the game yet
(2026-11-27: the offline checks are named with each item).

| variable | where | what it does |
|---|---|---|
| `LEONIDA_ASPECT` | `d3d12/crates/leonida-hle/src/modules/lifecycle.rs` (`fit`), `leonida-gpu/src/aspect.rs` | the window the title asks for (3840x2160) is made in that shape at the same height (5040x2160 for `21:9`, 7680x2160 for `32:9`), then fitted to the screen as before. `auto`: the primary monitor's shape. |
| `LEONIDA_PRESENT_SCALE` | `leonida-gpu/src/host.rs` (`present_planes`), `aspect.rs` (`frame`) | `fit`: in a window of another shape the swap chain becomes a frame of the window's shape at the picture's height (or width), cleared to black, with the picture copied unscaled into its middle. `stretch`: the swap chain is the picture and the window stretches it, as before. Unset: `fit` with `LEONIDA_ASPECT`, else `stretch`. |

`scripts/play-d3d12.ps1 -Aspect 21:9 [-PresentScale fit|stretch]` sets them; the F1 menu has WINDOW SHAPE (next start)
and PICTURE IN ANOTHER SHAPE (live). With neither set nothing changes: the frame of a 16:9 window is the picture itself
and the present is the same `CopyResource` as before (`aspect::frame`, unit-tested: `cargo test -p leonida-gpu aspect::`).

- With `fit`, the window's client area is looked at every present; the buffers are made again when its shape changes
  by more than one part in a hundred. The F1 menu, the frame-rate line and the trainer menu are laid over the whole
  frame, bars included.
- With DLSS frame generation on, the generator is told where the picture is in the back buffer (a
  `kBufferTypeBackbuffer` tag with only an extent, `fg.rs`). Hypothesis: it then interpolates inside the bars only;
  not run.
- The title still draws 16:9: this is the honest fallback, not ultrawide.

### What a real ultrawide needs, and what is known of it

Read from the 1.0.2.1 executable (offsets are RVAs; none of this is measured in a run):

- **The output size** is the largest row of the title's mode table (`.rdata` 0x615c0c0 to 0x615c100: 1280x720,
  1664x936, 1920x1080, 2560x1440, 3840x2160, each with 60 and 1). The function at 0x5a38910 copies the rows into the
  output's mode list; two functions (0x314e740, 0x231eb10) ask that list for the mode closest to 3840x2160 and take
  the output for 4K when both of its sides are at least that. A row of 5040x2160 or 7680x2160 passes both; 3440x1440
  does not, as 2560x1440 does not.
- **The check `LEONIDA_OUTPUT_1440` tripped** (0x247af2d) is in the function at 0x247ad10 that makes the scanout
  buffers inside a reservation: `mov ecx, 0x4000000` at 0x247ad4a off Series S, and what is left of it afterwards has
  to be under 2 MiB. 0x4000000 is two RGBA8 surfaces of 3840x2160 in 64 KiB tiles of 128x128 texels, rounded up to
  2 MiB; the same sum for another size is `scanout_pool_bytes` in `leonida-loader/src/hooks.rs` (0x1E00000 for
  2560x1440, 0x5600000 for 5040x2160).
- **The Settings screen's viewport** reads a 3840x2160 pair of its own (the load at 0x58048d4).
- **The render size** follows the output: the function at 0x1de4030 takes the output's width and height from its
  settings object, stores them for the upscaler and asks the upscaler chosen for the smallest and largest render size
  (0x7e92f0, the upscaler's slots 10 and 11). The FidelityFX upscaler is made with those sizes, so DLSS, which takes
  its place, would get a wide input and a wide output.
- **Shapes**: the executable has a table of eight (0x654fc80: 16:9, 16:10, 4:3, 7:3, 17:9, 5:4, 3:2, 5:3) and a
  function (0x3ddc350) that gives the one nearest to a ratio. 21:9 is there as 7:3; 32:9 is not, and would be taken
  for 7:3 by whatever uses that function. The constant 16/9 at 0x615a1c0 is loaded by at least seven more
  instructions (0x73c7a7, 0x19148bc, 0x28c07bd, 0x3108ddd, 0x32be281, 0x3b6c5f5, 0x4163240), none of them read yet.

`LEONIDA_OUTPUT=WIDTHxHEIGHT` (`hooks.rs`, EXPERIMENTAL) changes the first three together: the 3840x2160 row, the
reservation that fits it and the Settings viewport pointed at the row, after VIctory's native 1440p profile
(`runtime/src/display_profile.h`). `LEONIDA_OUTPUT_1440=1` is now the same with 2560x1440. A unit test checks the
bytes each place holds against the 1.0.2.1 executable (`LEONIDA_GAME_DIR`).

What blocks calling it ultrawide, each needing a run:

1. Whether the title takes a wide row as its output at all. With 1920x1080 in the row it made a 2560x1440 window
   (docs above), so it picks from the list by a rule that is not read yet; the 4K checks alone do not say which mode
   it presents.
2. Whether the scanout reservation sum holds for a wide size (the surfaces' count and format are read from globals
   in that function; the sum is known right for 3840x2160 only). If it does not, the title stops at 0x247af2d a second
   after its window comes up, as it did with `LEONIDA_OUTPUT_1440`.
3. Whether the camera's projection follows the output's shape (a wider view) or stays 16:9 (a stretched picture), and
   what the loads of 16/9 named above are for.
4. Where the HUD, the menus and the subtitles anchor in a wide output (the shape table suggests 21:9 layouts exist).
5. The cost: 5040x2160 is 31% more pixels than 3840x2160 at the same render scale, on a frame that is GPU-bound.

To try it: `scripts/play-d3d12.ps1 -Game <folder> -Aspect 21:9 -More @{LEONIDA_OUTPUT='5040x2160'}`. The log says
`hook applied` three times (`output: ...`), then either the swap chain's size (`presenting to a window`, 5040x2160:
the title took the row) or the title's stop at its check. With a wide output the frame of a 21:9 window is the
picture, so no bars are drawn.
