# Leonida D3D12 runtime

This is Leonida's Direct3D 12 runtime: it builds as `leonida.exe`, takes `LEONIDA_*` settings (older `VICTORY_*` names
still work) and is launched by Leonida's `scripts/play-d3d12.ps1` on Windows and `scripts/play-linux.sh` on Linux
(with VKD3D-Proton bundled, `docs/linux.md`).

It grew from the VIctory runtime: Marvisak/VIctory, through trustno1-coder/VIctory-NVIDIA (merged into Leonida at
`bafb0e25b`) and the forks credited below. Like them it is GPL-3.0-or-later (`LICENSE`). Leonida renamed it and adds
its own work on top; everything below this section is trustno1-coder/VIctory-NVIDIA's README as it came, with the
project name changed where it named the program.

## Leonida's picture fixes

| What it fixes | Setting (default) | Where | Credit |
| --- | --- | --- | --- |
| A 540p picture | DLSS, the default of `scripts/play-d3d12.ps1` since the renderer merged at `00698e1e8`: the title renders its own 1280x720 on Series S and DLSS takes it to 1920x1080 in place of the title's upscaler (`d3d12/Get-NVIDIA-DLSS.cmd` fetches NVIDIA's files once). Before that: `-Resolution native`, the title's own upscaler at 1:1 | `scripts/play-nvidia.ps1`, `crates/leonida-gpu/src/dlss.rs` | DLSS: avatarmanz/VIctory-7900XTX pull request 203 through trustno1-coder/VIctory-NVIDIA `6395727` |
| A soft world | `LEONIDA_DLSS_SHARPNESS` (F1 menu, live). `LEONIDA_SHARPEN`, Leonida's own sharpening of the game plane, lived in the compose pass the merged renderer replaced and is gone with it | `crates/leonida-gpu/src/dlss.rs` | trustno1-coder/VIctory-NVIDIA `6395727` |
| Black skin, faces and clothes | `LEONIDA_PAGE_WRITES` (bits 1, 2 and 8): textures picked by index that were uploaded before DirectStorage or a plain read filled them are uploaded again | `crates/leonida-gpu/src/watch.rs` | fxceless/victory-fork `e4245f3` |
| Skin that turns black later and stays so | `LEONIDA_HEAP_RECHECK_EVERY` (300 frames; 0 off): uploaded textures picked by index are looked at again on a slow beat after the first five looks | `crates/leonida-gpu/src/draw.rs` | fxceless/victory-fork, after RyanCase94 `920423a9` |
| Black pixels where a NaN or infinity was scrubbed | `v_mac_legacy_f32` / `v_mad_legacy_f32` fused with flushed sources, as RDNA 2 runs them: a subnormal factor is a zero, so subnormal times infinity is no longer infinity (test `mad_legacy_takes_a_subnormal_factor_as_a_zero`) | `crates/leonida-shader/src/emit/lower.rs` | fxceless/victory-fork `f6a09d3` |
| White glitter on floors, glowing windows and eyes | `LEONIDA_REFLECT_MOST` (off unless set since the renderer merged at `00698e1e8`, where it is unproven; 3 was the default before. It is not what posterizes some runs: those come out so with it off too): after `CS_UpdateHistory`, reflections are capped at a multiple of the room's light and colours far from grey pulled towards it; without rays they hold the sky of every miss | `crates/leonida-gpu/src/ambient.rs` | Leonida's own pass; the program and its view from lepotane/VIctory-leonida |
| No trainer menu | `LEONIDA_MENU` (on; 0 off): F4 (or RB + d-pad Left) opens an in-game menu beside the F1 settings and the F3 panel; arrows or numpad 8/2/4/6 move, Enter or 5 selects, Backspace or 0 goes back; `LEONIDA_MENU_OPEN=1` starts it open. DLSS sharpness, the frame-rate cap and the FPS panel (the renderer's own settings, shared with the F1 menu), screenshots, frame recording and the title's clock rate work; script features (god mode, vehicles, weather, ...) wait for the title's natives. Built and unit-tested, not yet tried in the running title (`docs/mod-menu.md`) | `crates/leonida-menu`, `crates/leonida-gpu/src/host/trainer.rs` | Leonida's own; the classic single-player trainer layout, no code or art from one |
| No Lua mods | `LEONIDA_LUA` (on; 0 off, no thread): Lua 5.4 mods in `mods\<name>\` (`mod.json` with `"script": "main.lua"`, `LEONIDA_MODS` names the folder) run in a sandbox on a thread of their own, with the `leonida` API (version 1): frame, key and menu events, an overlay, the live settings (frame cap, DLSS sharpness, panel), pages in the F4 menu under Mods (and Reload Lua mods), keys held for the title, and checked reads, writes and pattern scans of the title's memory; a failing mod is switched off alone. `mods\example_lua` ships disabled. Not the title's own script functions (god mode, vehicles, weather: none can be called). Built and unit-tested, not yet tried in the running title (`docs/lua-modding.md`) | `crates/leonida-lua`, `crates/leonida-menu/src/{overlay,mods}.rs`, `crates/leonida-gpu/src/host/trainer.rs` | Leonida's own; Lua 5.4 through the `mlua` crate (MIT) |


## Shaders and pipelines are made before the title starts

`scripts/play-d3d12.ps1` (through `d3d12/scripts/play-nvidia.ps1`) precompiles before it starts the title, as
Hookshot's release builds do; `-NoPrecompile` skips it.

1. `leonida shaders <the title's shaders.rpf>` translates every shader into the cache with the settings of this
   start. That takes minutes the first time and after a change of the translator or of a setting a translation
   depends on (`crates/leonida-gpu/src/shader_cache.rs` lists them); later starts find every shader there.
2. `leonida warm --used` makes the host pipelines earlier runs drew with, in processes of their own, so that the
   driver's cache on disk has them (`crates/leonida-gpu/src/pipeline/known.rs`). Runs write the title's pipelines
   down for it (`LEONIDA_PIPELINE_BOOK=1`, set by the launcher), so the first session in a place still makes that
   place's pipelines as it meets them; from the next start on they are made ahead.

---

> **Before you play**
>
> - **You must install DLSS yourself:** run `Get-NVIDIA-DLSS.cmd` once (it is
>   in this folder; a double click does it), or have your AI assistant run it
>   for you. It downloads NVIDIA's DLSS and frame generation files, about 70 MB,
>   from NVIDIA's own GitHub; they are not part of this repository. Without
>   them there is no DLSS and no frame generation, and the picture is only
>   stretched to the screen.
> - **F1 in the game brings up the settings menu** (arrow keys pick and
>   change, Enter does it). F3 shows the numbers panel.
>
> The steps are under [How to run it](#how-to-run-it).

## From trustno1-coder/VIctory-NVIDIA's README: built from many projects

This repository did not come from one place. Over several weeks we built, ran
and measured the work of many projects on the same NVIDIA machine, kept what
worked on this card, and brought the best of each together here. No single one
of them runs this well on NVIDIA by itself; the combination is what gets it to
where it is now.

Code that is in this repository:

- [avatarmanz/VIctory-7900XTX](https://github.com/avatarmanz/VIctory-7900XTX):
  Its threading, write relay, card fills, copy queue and texture
  read-back are all kept.
- [Marvisak/VIctory](https://github.com/Marvisak/VIctory): the original
  compatibility layer under everything else (loader, runtime services,
  Direct3D 12 back end, shader recompiler).
- [fxceless/victory-fork](https://github.com/fxceless/victory-fork) (formerly
  leonida-gta6): the fork the 7900 XTX one grew from, the Series S interface
  plane (pull request 1189, Tissma95), which fixed the black menus, and the face
  network fix (pull request 927, tippythetaidum), which made faces move.
- leonida-perf, carrying patches from
  [ZigZagBoi1/ScarlettPort](https://github.com/ZigZagBoi1/ScarlettPort): the
  title patches for render scale, the title's own upscaler and the watchdog.
- Through the 7900 XTX fork:
  [DexNom/VIctory](https://github.com/DexNom/VIctory),
  [Belobug/VIctory-Belobug-AMD](https://github.com/Belobug/VIctory-Belobug-AMD),
  [FQY55kai/ZJOU-VICTORY](https://github.com/FQY55kai/ZJOU-VICTORY),
  [myokex/victory-amd](https://github.com/myokex/victory-amd), Geckomacabre and
  cxy0072019-sketch.

Projects we ran, tested or studied along the way, whose lessons shaped the
settings and fixes here (none of their code is in this repository unless listed
above):

- ProjectLeonida/Leonida (no longer public): the first attempt, where the
  retail executable's boot was worked through.
- Our own builds of Marvisak/VIctory and fxceless/victory-fork, where the
  ray-walk skip, the half render scale and most of the measurements in the
  table below came from.
- [Belobug/VIctory-Belobug-AMD](https://github.com/Belobug/VIctory-Belobug-AMD)
  builds with its fixes combined.
- [zm0kiemedia/Neptune_VI](https://github.com/zm0kiemedia/Neptune_VI) (formerly
  vicecitybaby).
- [xcl-dev/xcl](https://github.com/xcl-dev/xcl), another layer for the same
  title, read for how the title behaves there.

Full credits, with what each person contributed, are at the end of this README.

## What this fork changes, and how it gets its frame rate

Everything below is what `scripts\play-nvidia.ps1` turns on. Measured on an
RTX 5070 12 GB with a Ryzen 7 9800X3D, in the prologue:

| Setup | fps | GPU time a frame |
|---|---|---|
| The 7900 XTX fork's own settings, Series X | high, but thousands of draws a frame skipped (the picture jumps) | - |
| With the descriptor fix, Series X, full scale | 6.0 | 110 ms |
| The same, half render scale | 7.2 | 82-88 ms |
| Series S, half scale, own upscaler, ray walks skipped | 11.0 | 33 ms |
| Plus memory compares on the card | 21.8 | about 6 ms |

### Settings that lower the picture a bit

1. **Series S profile** (`--console series-s`). The title believes it runs on
   the smaller console and picks that console's settings. The biggest change
   is resolution: the game image is 1920x1080 instead of the Series X's
   4K-class target. Menus, the HUD and subtitles are still drawn at 4K on a
   separate plane, so they stay sharp.
2. **Half render scale** (`LEONIDA_RENDER_SCALE=0.5`). A patch to the title's
   1.0.2.1 executable pins its internal render scale: the 3D world is drawn at
   half width and half height (960x540 on Series S), a quarter of the pixels,
   then stretched back to 1080p. This is why the world is softer than the menus.
   Other resolutions: see [Picking a resolution](#picking-a-resolution).
3. **The title's own upscaler** (`LEONIDA_OWN_UPSCALER=1`). On Series S the
   title upscales with AMD FidelityFX (FSR), whose compute programs cannot be
   translated here, so that step drew nothing. A patch makes the title use its
   own temporal upscaler on every profile. It is not a quality setting in
   itself, but Series S needs it for a scale below 1.
4. **Ray walks end at the root** (`LEONIDA_BVH_MISS=1`). The part of the title
   that builds its ray-tracing structures is off, so their nodes are stale.
   Instead of walking stale nodes, every box and triangle test is a miss. That
   avoids a hang and saves about 10 ms a frame, but light bounced off nearby
   surfaces is lost, so some dim rooms look darker.

### Settings that make it faster without changing the picture

5. **Memory compares on the card** (`LEONIDA_CARD_BUFFERS_MIN_MB=1`,
   `LEONIDA_CARD_BUFFERS_MB=1536`). Once the settings above had cut the GPU's
   work, the CPU was the limit: every frame it compared about 190 MB of title
   memory to find what the title had changed. These move the compares of copies
   of 1 MiB or more to the graphics card. Copying back fell from about 27 to
   10 ms a frame, and the frame rate doubled (11 to 21.8 fps).
6. **Wait for new shaders** (`LEONIDA_PIPELINE_WAIT_MS=10000`). Draws wait for
   a pipeline still being made instead of being skipped: nothing is missing,
   with a short hitch the first time a place is seen.
7. **NVIDIA Smooth Motion** (by default in `play-nvidia.ps1`). The script
   starts a copy of the build named `leonida_sm.exe`; with Smooth Motion turned
   on for that exe in the NVIDIA App, the driver inserts a generated frame
   between real ones, so motion looks about twice as smooth. The title still
   runs at the real rate underneath, so controls respond at about 20 fps.

### Code changes (what makes it work on NVIDIA at all)

8. **No more skipped draws on NVIDIA** (`LEONIDA_DYNAMIC_SLOTS=524288`,
   `crates/leonida-gpu/src/descriptor/host.rs`). NVIDIA caps the shader-visible
   view heap at 1,000,000 views, below the 1,245,184 the title asks for. The
   pool of dynamic descriptor slots (2^18) then ran out, and 1,100-4,800
   descriptor tables a frame were refused, each one a dropped draw: the screen
   rarely updated and the character jumped from place to place. The pool can
   now take up to half the heap: 0-33 refused a frame.
9. **Series S interface plane** (`LEONIDA_COMPOSE=1`,
   `crates/leonida-gpu/src/compose.rs`, `host.rs`, `queue.rs`). A Series S
   title presents two planes: the game (1080p) and the interface (4K). Only the
   first was shown, so menus, loading screens and subtitles were black until
   gameplay. One compute pass now lays the interface over the game before it
   is shown. Ported from fxceless/victory-gta6 pull request 1189 (Tissma95),
   without its FSR stand-in (the own upscaler is used instead).
10. **Title patches** (`crates/leonida-loader/src/hooks.rs`, `lib.rs`,
    `windows.rs`): render scale, own upscaler, and `LEONIDA_NO_WATCHDOG`
    (keeps the title's watchdog from ending a slow frame), taken from the
    leonida-perf patches.
11. **Ray walks end at the root** (`crates/leonida-shader/src/emit/intersect.rs`,
    `crates/leonida-gpu/src/shader_cache.rs`): the `LEONIDA_BVH_MISS` switch, made
    part of the shader cache key so translated shaders with and without it are
    kept apart.
12. **`scripts/play-nvidia.ps1`**: the launcher with all of this list.
13. **Faces move** (`crates/leonida-shader/src/emit/halves.rs`, `lower.rs`,
    `crates/leonida-gpu/src/descriptor/host.rs`, `draw.rs`). Faces are driven by
    a network of compute programs whose numbers travel as 16-bit values through
    integer views; they were read as floats and came back as zeros on NVIDIA, so
    no face moved. They now move as integers. Ported from fxceless/victory-fork
    pull request 927 (tippythetaidum).

14. **Ray tracing mostly off** (`scripts/rt_off_hashes.txt`, used by
    `play-nvidia.ps1` through `LEONIDA_SKIP_PIPELINES`; `-RayTracing` keeps the
    passes). The title's ray-tracing structure builder cannot run on NVIDIA (its
    64-lane programs are refused; Marvisak/VIctory pull request 2311), so every
    ray misses. The passes that turn those misses into light made rooms go white
    and hazy depending on the view angle. The 1,000 `CS_RaytracingResolve` and
    34 traced-shadow shaders are skipped. The screen-probe and visibility traces
    keep running: skipping them too (all 1,052 shaders of fxceless pull request
    465's rule) left their outputs as garbage, rainbow glitter everywhere. Needs
    the `diag` build (the skip list is a diagnostics switch).
15. **The intro video no longer flickers** (`crates/leonida-gpu/src/texture.rs`).
    The movie's planes (linear, one-level R8 textures in a ring of three) were
    written by the decoder through another view of the same memory, judged
    clean when the frame was recorded, and copied a list late: frames from the
    ring showed out of order. Video planes now always record their copy.

### Settings

The default set of `play-nvidia.ps1` is the speed set (about 18-22 fps on the
RTX 5070). `-Quality` trades about 7 fps for fewer of the first two faults
below.

| Fault | Cause | Fix |
|---|---|---|
| Stretched bodies that jump | The relay's second stage and card steps hand skinned meshes back before they are done (avatarmanz issue 13; fxceless issue 89 on an RTX 5070) | `-Quality`: `VICTORY_RELAY=1`, `VICTORY_CARD_STEPS=0` |
| Rainbow, pixelated skin | A texture refresh that does not wait for the runtime's remapping fills its upload from pages not there yet | `-Quality`: `LEONIDA_TEXTURE_NO_WAIT=0` |
| Some views far too bright | Without rays the light probes keep the sky's light indoors, and the fog takes it too | Always: `LEONIDA_AMBIENT_AIR=1.5` (fog at most 1.5 times the light around), `LEONIDA_AMBIENT_MOST=2` (new, `ambient.rs`; was a fixed 4) |
| The picture stops until you look away | A pipeline that takes longer than the wait was waited for again in every frame it was in view | Always: each pipeline is waited for once at most (`VICTORY_PIPELINE_WAIT_ONCE`, on; `pipeline/make.rs`; the problem of fxceless issue 1181) |
| Black blobs on faces and in the air | A NaN that a shader writes is spread by blur and the temporal upscaler | Always: `LEONIDA_NAN_SCRUB=1` (new, `emit/lower.rs`): colours and image stores with NaN become 0 (an image store of 32 bits no float instruction made, which may be an integer's, loses only the NaN and infinity bit patterns a card's arithmetic gives; `LEONIDA_NAN_SCRUB=2` scrubs every image store as a float, as 1 did before); an infinity is held at 65504 with its sign instead of made 0, which had turned the brightest pixels black (`LEONIDA_NAN_SCRUB_INF=0`: 0 as before); NaN in the fog volume is cleared too |

Known faults left: the common room by its sunlit windows still goes beige and
hazy (sky light in the fog); smearing when the camera moves fast at half scale
(the title's own temporal upscaler); two orange glows on some corridor walls.
Render scale 1.0 on Series S has not been tested.

Also in, off by default:

- `VICTORY_CONSTANT_MARKS=1`, a port of fxceless pull request 1216
  (GamingRipperDoc): a slot picked from the heap is read as a constant buffer
  only if it holds one (else a texture's descriptor was read as one, in the
  meshlet culling passes).
- `VICTORY_TARGET_VARIANTS=1`, a port of fxceless a35c557393: a pipeline is made
  again for the render target formats bound at the draw.
- F9 frame capture (`capture.rs`): with `LEONIDA_CAPTURE=<folder>` and
  `LEONIDA_DUMP_TARGETS=1`, F9 saves the next frame's picture, its commands and
  every 2D target.
- `LEONIDA_ADAPTER=<part of a card's name, or #index>` (`host.rs`): the card to
  run on when there is more than one (`play-nvidia.ps1 -Adapter`).

---

A fork of [avatarmanz/VIctory-7900XTX](https://github.com/avatarmanz/VIctory-7900XTX),
itself a fork of [VIctory](https://github.com/Marvisak/VIctory), a compatibility
layer that runs an Xbox Series title natively on Windows from files the user
supplies. That fork is tuned for an AMD RX 7900 XTX. This one makes it **draw
everything on NVIDIA cards and run fastest there**, using the Series S profile.

**This repository contains no game files, code or assets.** You need your own
copy of the game. It is not affiliated with or endorsed by the game's developer
or publisher.

## New on 2026-11-27: DLSS and frame generation on Series X, a window of another shape

Written without a run of the game (the card was busy): the places changed in the title are checked against the
1.0.2.1 executable by a unit test, the rest by unit tests and the launcher's `-DryRun`. Nothing changes unless one
of the parameters below, or its line in the F1 menu, is used.

- **Why DLSS was Series S only.** DLSS (and the stretch) stand in for one call, the title's FidelityFX upscale
  (`ffxDispatch`), and the title only upscales through FidelityFX when its device type is a Series S. On Series X
  its own temporal upscaler runs instead, as passes of its own that hand nothing over. Frame generation was tied
  to the same call: its depth and motion vectors are the ones that upscale is given.
- **DLSS on Series X**: `-Dlss quality|balanced|performance|ultra` (UPSCALER and RENDER SCALE in the F1 menu).
  The loader makes the title upscale through FidelityFX there too (`LEONIDA_FSR_UPSCALER=1`), at the mode's
  render scale (2/3, 0.6, 1/2 or 1/3 of the 2160 lines: 1440, 1296, 1080 or 720), and DLSS makes the 3840x2160
  output. DLSS's own mode follows those sizes.
- **Frame generation on Series X**: `-FrameGen 2` (FRAME GEN in the menu). It needs the DLSS upscale there, so the
  launcher turns DLSS on with it (quality unless a mode is given) and says so. The log says `frame generation: the
  first upscale's numbers` once its inputs arrive, and after 600 presents without any says that too.
- **A window of another shape**: `-Aspect 21:9|32:9|W:H|auto` with `-PresentScale fit|stretch` (WINDOW SHAPE and
  PICTURE IN ANOTHER SHAPE in the menu). The game still draws 16:9: `fit` shows it centred with black bars,
  `stretch` fills the window. What a real ultrawide picture needs, and the experimental `LEONIDA_OUTPUT`, are in
  [docs/ultrawide.md](../docs/ultrawide.md).
- `-DryRun` prints the settings a start would run with, after the menu's saved ones, and stops.

## New on 2026-11-26: another renderer, DLSS, a settings menu

The default of `scripts\play-nvidia.ps1` changed. What is below this section
was written on 2026-11-25 for the settings before it, which `-Resolution`
still reaches.

- **The renderer is CGBJ's** ([codec-d/cool_fox](https://github.com/codec-d/cool_fox)
  `f4b48ad`, GPL-3.0 like this fork), with this fork's NVIDIA fixes on top.
- **DLSS does the title's upscale** (from pull request 203 of
  [avatarmanz/VIctory-7900XTX](https://github.com/avatarmanz/VIctory-7900XTX)):
  the game renders its own 720p and DLSS takes it to 1080p, in place of the
  render scale and the title's upscaler. Series S only.
- **F1 in the game opens a settings menu** (arrows pick and change, Enter
  does it): upscaler, DLSS model and sharpness, frame generation, frame rate
  cap, ray tracing, Smooth Motion, console profile, graphics card, and the
  speed switches. Lines marked LIVE change at once; the others are saved to
  `leonida-settings.txt` and apply at the next start, which "SAVE AND RESTART
  NOW" does for you. F3 shows the numbers panel.
- **DLSS frame generation** (NVIDIA Streamline), 2x to 6x, from the menu. The
  amount changes while playing. Frames are only generated while the game's
  window has the focus.
- **Frame rate cap**: the title asks for 30 frames a second on its own; the
  loader's frame-rate patch, always on, makes that 60
  ([docs/vi_patch.md](../docs/vi_patch.md)), and the menu's FRAME RATE CAP
  changes it (30, 60, 120 or none).
- **Game patches after VI-Patch** ([docs/vi_patch.md](../docs/vi_patch.md)):
  60 frames a second always; no vignette by `LEONIDA_VIPATCH_NO_VIGNETTE=1`
  or the menu.
- **Real ray tracing**, off by default: `-RayTracing` or the menu. On this
  renderer the title's structure builder runs on NVIDIA (53 programs), and
  the draws dropped for it go from hundreds or thousands a frame to one.

Measured on the RTX 5070 below (Series S, DLSS, opening rooms): about 15-17
fps without ray tracing and 13-15 with it, before generated frames.

Known to be wrong with it:

- **Ray tracing and DLSS frame generation together do not work**: about 0.1
  fps and a white picture. Each alone is fine. The launcher and the layer
  leave frame generation off when ray tracing is on (Smooth Motion can do the
  extra frames then). The cause is not found.
- **Without ray tracing, lit places further into the game blow out to white
  with black holes** (the bank, outdoors): the lighting runs with every ray
  missing. With ray tracing on, that has not been checked that far yet.
- **With ray tracing, shaders build for minutes** the first time a place is
  seen (about 1,700 pipelines for the opening alone).
- **A posterized "oil paint" look** comes in some runs and not in others.
- "SAVE AND RESTART NOW" can leave the old game's process behind without a
  window; it goes when the PC restarts.

## Where it stands (2026-11-25)

Tested on one machine only: Windows 11, AMD Ryzen 7 9800X3D, 64 GB RAM,
GeForce RTX 5070 12 GB, the Series S profile at half render scale
(`scripts\play-nvidia.ps1`).

| | |
|---|---|
| Prologue, indoors | about 20-22 fps on average, 24-31 fps in lighter places |
| Device losses | none in the measured runs |
| Menus, loading screens, subtitles | shown (they were black on Series S before) |
| First time a place is seen | slow for a moment while its shaders are made |

On the same card the fork's own settings skipped thousands of draws a frame
(the picture jumped and rarely updated), and the Series X profile at full scale
ran at about 6 fps.

What is known to be wrong:

- The picture is softer than Series X: half render scale, upscaled by the
  title's own upscaler.
- Rooms with little direct light can be dark: ray walks end at the root while
  the title's structure builder is off (`LEONIDA_BVH_MISS`).
- Everything listed in the 7900 XTX fork's README still applies (flicker of far
  plants outdoors, textures appearing late while the title streams, a stutter on
  the first run with a new driver or build).

## How to run it

You need Windows 10/11 x64, a CPU with AVX2, a Direct3D 12 GPU with shader
model 6.6 and mesh shaders, Rust (the toolchain is pinned), the MSVC build
tools, and your own dump of the title.

```powershell
git clone https://github.com/trustno1-coder/VIctory-NVIDIA
cd VIctory-NVIDIA
copy leonida.example.toml leonida.toml      # then set your paths, outside this folder
scripts\play-nvidia.ps1 -Exe <path of your copy's executable>
```

The first start builds the layer by itself (`scripts\build-nvidia.ps1`: the
`diag` profile with `diagnostics`).

**For DLSS and DLSS frame generation, run `Get-NVIDIA-DLSS.cmd` once** (double
click it, or `Get-NVIDIA-DLSS.cmd -NoStreamline` for DLSS alone). It downloads
the few NVIDIA files they need, about 70 MB, from NVIDIA's own GitHub
([NVIDIA/DLSS](https://github.com/NVIDIA/DLSS),
[NVIDIA-RTX/Streamline](https://github.com/NVIDIA-RTX/Streamline)) into
`nvidia-sdk\` (git-ignored), and the next start builds the layer again with
DLSS in it (about a minute). Those files are NVIDIA's, under NVIDIA's licences,
which are saved beside them; this repository does not carry them and may not.
A build with DLSS links NVIDIA's loader library: it is for your own use, not
for passing on.

Without that step the game runs all the same: its 720p picture is stretched to
the screen instead of going through DLSS (softer), and there is no DLSS frame
generation. Nothing is downloaded unless you run that file.

`scripts\play-nvidia.ps1` starts a copy of the build named `leonida_sm.exe`, so
NVIDIA Smooth Motion applies every time once it is turned on for that exe in the
NVIDIA App. `-NoSmoothMotion` runs `leonida.exe` itself; `-SeriesX` uses the
Series X profile (sharper, much slower; DLSS there with `-Dlss`); `-Quality` gives fewer
stretched bodies, slower; `-RayTracing` turns the title's real ray tracing on.
The fork's own `scripts\play.ps1` is still there for AMD cards.

### Picking a resolution

There is one script per console and resolution in `scripts\`. Each one runs
`play-nvidia.ps1` with `-Resolution` set, and takes the same `-Exe` path and
other switches:

```powershell
scripts\play-S-720p.ps1 -Exe <path of your copy's executable>
scripts\play-X-1080p.ps1 -Exe <path of your copy's executable> -Quality
```

The resolution is the height the 3D world is drawn at. The title's own
upscaler then stretches it to the console's output, and menus, the HUD and
subtitles stay sharp either way. Series S outputs 1920x1080 and Series X
3840x2160, so the same height is a different render scale on each:

| Script | Console | World drawn at | Render scale | Tested on the RTX 5070 |
|---|---|---|---|---|
| `play-S-1080p.ps1` | Series S | 1920x1080 | 1.0 | **crashes at start** (the title stops at a breakpoint; also reported on an RTX 2080) |
| `play-S-720p.ps1` | Series S | 1280x720 | 0.667 | not yet |
| `play-S-540p.ps1` | Series S | 960x540 | 0.5 | yes: the default, about 18-22 fps |
| `play-S-480p.ps1` | Series S | 853x480 | 0.444 | not yet |
| `play-X-2160p.ps1` | Series X | 3840x2160 | 1.0 | yes, about 6 fps |
| `play-X-1440p.ps1` | Series X | 2560x1440 | 0.667 | not yet |
| `play-X-1080p.ps1` | Series X | 1920x1080 | 0.5 | yes: the `-SeriesX` default, about 7 fps |
| `play-X-720p.ps1` | Series X | 1280x720 | 0.333 | not yet |
| `play-X-540p.ps1` | Series X | 960x540 | 0.25 | not yet |
| `play-X-480p.ps1` | Series X | 853x480 | 0.222 | not yet |

What to expect:

- **Lower is faster, up to a point.** On this machine the frame time is mostly
  the CPU preparing the GPU's work (about 33 ms a frame on Series S), not the
  GPU drawing pixels. Going below 540p saves GPU time, but the frame rate stops
  rising once the CPU is the limit.
- **Series S is much faster than Series X at the same resolution.** The console
  profile changes far more than resolution: the title picks the smaller
  console's level of detail, effects and memory budgets. `play-S-540p` and
  `play-X-540p` draw the same number of pixels, but the Series X one does
  more work for each frame.
- **Lower is softer and smears more.** The title's temporal upscaler rebuilds
  detail from earlier frames. At low resolutions and low frame rates, fast
  camera moves (driving, turning quickly) leave visible trails.
- **Series S at 1080p (render scale 1.0) does not work yet.** The title stops
  at a breakpoint as soon as it starts, on a 12 GB card too, so it is not video
  memory. Use 720p or lower on Series S.
- Series S cannot go above 1080p: its output is 1080p, and the script says so
  and stops if asked for more.
- **16 GB PCs: turned on for you, best with resizable BAR enabled.**
  On the tested 16 GB PC with an RTX 3060, shared GPU memory approached its
  limit about a minute into the world (`0x8007000E`, then a driver fault).
  The 3072 MiB cap from [PR #6](https://github.com/trustno1-coder/VIctory-NVIDIA/pull/6)
  let eligible title memory windows use CPU-mapped VRAM while the card had room.
  The launcher sets it by itself on PCs with under 24 GB of RAM (Windows lets
  the card share half the RAM) and leaves it off on larger ones. Pass a cap in
  MiB to `play-nvidia.ps1` or any resolution wrapper to choose for yourself:
  `-WindowsBarMB 0` disables it, and `-More @{LEONIDA_WINDOWS_BAR_MB='...'}`
  takes precedence.

### More than one card, and other cards

On a PC with two cards, `-Adapter` picks one by part of its name or its index
(`-Adapter 5060`, `-Adapter '#1'`); it sets `LEONIDA_ADAPTER`, and the log says
which card the game is on. Without it the game uses Windows' default card, the
one the main display is on.

Measured on the same PC (Ryzen 7 9800X3D), same build and settings
(`play-S-540p`, the default), one run each in the prison prologue:

| Card | Slot it sits in | fps | Preparing a frame (CPU) | On the GPU |
|---|---|---|---|---|
| RTX 5070 12 GB | PCIe 5.0 x16, drives the display | 19.3 | 34 ms | 4.6 ms |
| RTX 5060 Ti 16 GB | PCIe 4.0 x4, no display attached | 14.9 | 50 ms | 5.4 ms |

The GPU work itself is close; the gap is in preparing frames, mostly copies of
title memory to and from the card. This layer moves tens of MB a frame, so the
link to the card matters: the 5060 Ti here has about an eighth of the 5070's
bandwidth, and its finished frames are also copied to the 5070 to be shown.
In a full-speed slot with the display on it, a 5060 Ti should land closer to
the 5070. Neither run lost the device; both refused the same 7 programs.

`-Resolution` works on `play-nvidia.ps1` itself too
(`-Resolution 720 -SeriesX`), and `-More @{LEONIDA_RENDER_SCALE='0.6'}` still
sets any scale from 0.2 to 1.0.

Before the first run, ahead of time (both can be stopped and go on later):

```powershell
target\diag\leonida.exe shaders <the shader archives>   # every shader translated, minutes
target\diag\leonida.exe warm --used                     # pipelines earlier runs drew with, for a new driver or build
```

Commands, debugging switches and upstream's own description:
[docs/README-fxceless.md](docs/README-fxceless.md). Every `VICTORY_...` switch is
described in a comment where it is read.

## What is different here

This fork's own changes are listed at the top of this README. Everything the
7900 XTX fork added is kept: translation of list calls on threads
of ours, submissions prepared and joined on a thread of ours, the write relay,
fills done on the card, textures on their own copy queue, texture read-back
(boats float), and frame record and replay. Its
[docs/findings.md](docs/findings.md) has the reasons and the measurements.

## Credits

This is a tree of other people's work with more on top. The history is kept, and
commits taken from elsewhere name the commit and its author.

- **[avatarmanz/VIctory-7900XTX](https://github.com/avatarmanz/VIctory-7900XTX)**:
  the fork this one started from, and everything in it.
- **[VIctory](https://github.com/Marvisak/VIctory)** by Marvisak and its
  contributors: the loader, the runtime services, the Direct3D 12 back end and
  the shader recompiler.
- **[fxceless/victory-fork](https://github.com/fxceless/victory-fork)**: the fork
  the 7900 XTX one started from, the people whose pull requests it carries,
  pull request 1189 (Tissma95) for the Series S interface plane, 927
  (tippythetaidum) for the face network and 1216 (GamingRipperDoc) for the
  marked constant buffer slots.
- **[DexNom/VIctory](https://github.com/DexNom/VIctory)**: ray tracing fixes,
  cube gradients, sparse textures.
- **[Belobug/VIctory-Belobug-AMD](https://github.com/Belobug/VIctory-Belobug-AMD)**:
  the ambient light idea, pipeline workers, and work by romainhedouin.
- **[FQY55kai/ZJOU-VICTORY](https://github.com/FQY55kai/ZJOU-VICTORY)**.
- **[myokex/victory-amd](https://github.com/myokex/victory-amd)**: the guard over
  every view of a page (with Spexivus, joroyt and tightfall), and Joshua Odom's
  reads into title memory.
- **Geckomacabre** and **cxy0072019-sketch**: the texture read-back.
- **[XCL](https://github.com/xcl-dev/xcl)** (GPL-2.0-only) was read for how the same
  title behaves under another layer; no code of it is here.
- This fork's changes were made with [Claude Code](https://claude.com/claude-code).

## License

GPL-3.0-or-later, as VIctory. See [LICENSE](LICENSE); the one file that is not
covered is named in [docs/asset-policy.md](docs/asset-policy.md).
