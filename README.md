<h1 align="center">
  <br>
  <a>
  <img src="assets/logo.svg" alt="Leonida Continued" width="200">
</a>

  <br>
  <b>Leonida Continued</b>
  <br>
</h1>

<h4 align="center">Grand Theft Auto VI (Xbox Series) running natively on Windows, built on your PC from your own copy.
</h4>

<p align="center">
  <a href="docs/i18n/README.md">Other languages</a> |
  <a href="#project-status">Status</a> |
  <a href="#how-to-build">How to build</a> |
  <a href="#how-it-works">How it works</a> |
  <a href="#want-to-help">Want to help?</a> |
  <a href="#credits">Credits</a> |
  <a href="#license">License</a> |
  <a href="https://discord.gg/ETf4qj5acN">Discord</a> |
  <a href="https://t.me/mandyport">Telegram</a>
</p>

Leonida is a static recompilation project for **Grand Theft Auto VI (Xbox Series)**. Because the Xbox Series CPU is
x86-64 like a PC, the game's code is loaded and run natively while the console's system software is reimplemented, and
models, textures and audio are read from your package. Everything runs **on your PC, from your own copy of the game**.

The repository maintains **two runtimes**:

- **Direct3D 12 runtime ([`d3d12/`](d3d12/README.md), `leonida.exe`)** — **playable in the open world on Windows**
  (NVIDIA and AMD) via **`Launcher.bat`** or `scripts/play-d3d12.ps1`.
- **Recompiled Vulkan runtime ([`runtime/`](runtime/README.md), `gta6rt.exe`)** — translates the console's precompiled
  RDNA2 GPU programs to SPIR-V for Vulkan (`make_game.py`) and is still in development (not playable yet).

> [!IMPORTANT]
> This repository **does not contain the game, keys or an executable**, and it does not explain how to get them. You
> need your own copy, with its files already readable on disk. Nothing here decrypts or works around any protection.
> Everything the build creates (the generated shaders, extracted assets, the runtime folder and your saves) comes
> from your game and stays on your PC. Never share those files.

> [!NOTE]
> **This is a continuation of [NotRexed's Leonida project](https://github.com/ProjectLeonida/Leonida).** The original
> author has left the project, and development continues here with its full commit history. Credit for the work up to
> that point belongs to NotRexed and the contributors listed in the history.

Discord: https://discord.gg/ETf4qj5acN

Telegram: https://t.me/mandyport

<br>

## Project status

**Playable in the open world on Windows on the Direct3D 12 runtime ([`d3d12/`](d3d12/README.md)); the recompiled
D3D12 → Vulkan runtime ([`runtime/`](runtime/README.md)) is still in development and not playable yet.**

### Direct3D 12 runtime ([`d3d12/`](d3d12/README.md) — Windows, NVIDIA and AMD)

A separate program (`leonida.exe`) that grew from VIctory and is **playable in the open world**. The measurements below
are this project's own, taken by the maintainer on 2026-11-27, not figures quoted from another project. On `main` that
day it measured **about 10 frames a second** at the save point (median frame about 102 ms); on the maintainer's working
branch of that day (commit `37426fd9f`, carrying a GPU-wait fix not yet on `main`) it measured **12 to 13 frames a
second** at the same spot:

- PC: Ryzen 5 7600X (6 cores), GeForce RTX 4060 Ti 16 GB (driver 616.64), 32 GB of RAM, Windows 11.
- Series X profile, 2560x1440 picture, the title's own upscaler at render scale 1, no generated frames.
- A save loads into the open world and you play from there: the character walks and drives with a pad (Xbox or
  PlayStation, with matching button prompts) or with keyboard and mouse, with sound, the HUD, the map, the weapon
  wheel and the pause menu. **12 to 13 frames a second** standing at the save point on `37426fd9f` (median frame 75 to
  82 ms in three runs of about three minutes, about 7,100 draws a frame); the game uses about 5.6 of the 6 cores and
  the card is about 95% busy.
- Known to be wrong in the picture: a stair-stepped part of the screen near the camera gets no indirect light at
  that save point; ray-traced light is stood in for (every ray is a miss, the structure builder is off); the
  transmission-volumetrics programs are left out, which darkens water.
- Not measured: how far the story plays, and how long a session lasts.

### Recompiled Vulkan runtime ([`runtime/`](runtime/README.md) — D3D12 → Vulkan, not playable yet)

On `main`, the game boots from your own copy on native Windows and on Linux through Proton/Wine: it shows the launch
movie, the first-run pages and the landing prompt (responding to a pad or the keyboard pad), then runs the story load
to its end and enters the world phase. On native Windows a scripted pad alone (`LEONIDA_INPUT_SCRIPT`, no keyboard or
controller) gets through every first-run page, including the legal-terms page whose Confirm waits for both boxes, to
the loading screen of the story load ([the route](docs/first_run_ui.md#through-the-first-run-pages-with-the-scripted-pad)).
On Linux (RX 7900 XTX, Mesa 26.3.1 RADV, Wine), an experimental run drew world frames for over eight minutes without a
GPU hang at about one frame per six seconds, with alternating white/correct/black frames and misplaced geometry
([AMD on Linux: what got the world drawn](docs/linux_proton.md#amd-on-linux-what-got-the-world-drawn)).
[First-run UI on Windows](docs/first_run_ui.md) has the Windows build and launch steps;
[the Linux/Proton report](docs/linux_proton.md) has timings, workarounds and remaining stops.

| What | Status |
|---|---|
| Open world / playable (`d3d12/`, Direct3D 12) | ✅ **Playable on Windows (NVIDIA and AMD)** — loads a save into the open world with controls, sound, HUD, map, weapon wheel and pause menu (`~10 FPS` on `main`, `12–13 FPS` on `37426fd9f` on RTX 4060 Ti; story progression and long sessions not measured; [`d3d12/README.md`](d3d12/README.md)) |
| Booting & world frames (`runtime/`, D3D12 → Vulkan) | ⚠️ Boots through the first-run UI and story load on native Windows ([guide](docs/first_run_ui.md)) and Linux/Proton; an experimental Linux/RADV run drew world frames at ~1 frame per 6 s with visual artefacts ([report](docs/linux_proton.md#amd-on-linux-what-got-the-world-drawn)) |
| Story mode / playable (`runtime/`, D3D12 → Vulkan) | ❌ Not yet *(use [`d3d12/`](d3d12/README.md) to play in the open world on Windows)* |
| Finding and checking your copy (Xbox Series 1.0.2.1) | ✅ Works |
| Shaders: carve, unpack native RDNA2 code, disassemble | ✅ All 26,503 |
| Shaders: RDNA2 → SPIR-V translation (`runtime/`) | ✅ **26,503 / 26,503 translated**: 2,236 VS, 12,101 PS, 2,689 CS, 9,282 mesh, 195 geometry, with a per-shader metadata index for pipeline creation. Both the LLVM 23.1.5 Linux rerun at `c432cdc` and the LLVM 23.1.3 Windows rerun at `43d0015` translated all 26,503. Translation and SPIR-V validation are separate steps; validation results are listed below. ⚠️ 23,841 are wave64: exact on 64-wide subgroups (AMD). On 32-wide subgroups (NVIDIA, Intel), compute and mesh shaders are exact with wave64 emulation (`--wave64-emulate`, on by default there); cross-half wave ops in vertex, pixel and geometry shaders are still approximate |
| SPIR-V validation (`spirv-val`, `runtime/`) | ✅ **26,503 / 26,503 passed** in the LLVM 23.1.5 Linux rerun at `c432cdc` and in the full-corpus rerun at `43d4703`, after `dcd8ecf` fixed the SPIR-V ID-bound error. On Windows at `43d0015`, all 26,503 translated and 26,501 passed before `dcd8ecf` fixed the two ID-bound failures; the intermediate `dedd75c` run also passed 26,501 |
| Shader translator checked on a real GPU (`runtime/`) | ⚠️ Hand-written GPU tests (vertex fetch, texture sampling, exports, ALU ops, partial-register ops, barycentrics, wave-uniform branches, compute LDS/barriers/buffer stores, wave64 masks) have been run on more than 30 GPU and driver combinations, with results published in [`gpu-test-results/`](gpu-test-results): NVIDIA Turing, Ampere, Ada and Blackwell (Windows and Linux); AMD GCN3, RDNA1, RDNA2, RDNA3 and RDNA4 (Windows driver and RADV); Intel Gen9 (ANV); Apple M1/M3 (MoltenVK and KosmicKrisp, no geometry or mesh stages); and Mesa llvmpipe. Wave64 tests need 64-wide subgroups (AMD) or `LEONIDA_W64_EMULATE=1`. These are small test shaders: the game's own shaders are checked by `spirv-val` and by driver pipeline creation (`vk_pipeline_check`), not by their output |
| Models (OBJ with real names, UVs, material references) | ✅ Exported. A ten-archive scan at decoder pin `e293917` recovered **382,781 models** with zero readback decode failures. This includes **343,327** models from `levels.rpf`. A full OBJ export of that archive is pending. See [recount and export scope](docs/models.md#full-archive-re-count-at-e293917). |
| Texture dictionaries and world texture pools (DDS) | ✅ Exported. `rpf9dump` decodes BCPack BC1, BC3, BC4 and BC5 chunks in strict mode and skips BC6H, BC7 and chunks that need unconfirmed history; the assets stage then completes what it can with a clean-room decoder (`tools/bcpack.py`, which `tools/ttpf_assets.py` runs through `tools/ttpf_mips.py`) that rebuilds BC1, BC3, BC4, BC5, BC6H and BC7, and records the rest as a `BCPackLimitation` in `manifest.json` (most BC6H/BC7 chunks and the unconfirmed-history ones need a local history table, which the repo does not ship) ([notes](docs/textures.md)) |
| Audio banks (AWC: Opus, XMA2) | ✅ Exported whole. Opus repackaged **losslessly** as Ogg Opus; PCM (codec 0/2) as WAV; XMA2 decoded with ffmpeg. `--wav` adds 16-bit PCM WAV ([notes](docs/audio.md)) |
| Music/radio config | ? Measured. **1,589** audio-config records with exact durations and AWC stream ids; station grouping and content-id lists; no per-track start offsets ([notes](docs/audio_config.md)) |
| PE loader and import shims (`runtime/`) | ⚠️ Runs a stand-in executable end to end (`gta6rt --run`: CRT startup, statics, heap, files, threads, GDK imports through the shims, exit code back). Controllers through a GameInput shim on SDL3 (any controller SDL supports, rumble, DualSense trigger effects and lightbar; built when SDL3 is installed or with `-DLEONIDA_SDL3_FETCH=ON`, [notes](docs/gameinput.md)). `--trace-d3d12` logs every D3D12 call and its outcome for the first boot. Runs the game's own executable through the story load and into the world phase ([report](docs/linux_proton.md)) |
| Xbox D3D12 → Vulkan (`runtime/`) | ⚠️ In progress. A Vulkan-backed device behind the Xbox-ABI proxy creates queues, command lists, fences, descriptor heaps, committed, placed and reserved resources, render-target and depth views (3D targets included), texture views and samplers (2D, cube, 1D and 3D views, the depth and stencil planes of depth-stencil textures, packed formats such as B5G6R5, B4G4R4A4 and R9G9B9E5, buffer SRVs — raw, structured and typed; they go into the bindless set), root signatures, pipelines and query heaps, and draws run on Vulkan. Barriers, clears, uploads, copies (BC included), colour resolves, predication, CPU-side texture reads and writes, and occlusion, timestamp and pipeline-statistics queries run on Vulkan, checked on one GPU. The console-shaped command-list proxy, the measured `PresentX` queue slots (30/32, layout-detected) wired to the presentation core, and `AllocateUploadMemory` (list slot 148, a mapped-upload arena whose addresses translate to buffer device addresses in root descriptors and IA/SO views) cover the console-ABI surface the package measures. Still open: the console list tail and the descriptor-table record model ([plan](docs/d3d12_vulkan.md), [console ABI](docs/d3d12_xs_console_abi.md)) |
| Ray tracing (BVH, `runtime/`) | ⚠️ In progress. `BuildRaytracingAccelerationStructure` builds BLASes and TLASes on the GPU in the console's XDXR BVH4 layout (the default; `LEONIDA_RT_AS=khr` uses Vulkan's own acceleration structures instead), which the translated shaders walk themselves through `image_bvh_intersect_ray` (all four triangle node types, TRI0-TRI3, with the barycentric remap byte); the GPU builder writes TRI0 and TRI1 leaves. Updates refit instead of rebuilding, and clone, compaction and the compacted/current postbuild sizes work; checked against a CPU reference on synthetic scenes. The build descriptions are recovered from the package's `XboxBvhBuilder.dll` ([notes](docs/xdxr.md)). The game's `*_bvh4.rpf` packs are measured: offline-built, dehydrated BLASes that rehydrate through that DLL with their geometry from the drawables in `levels.rpf` (measured on 8 of the 10 sampled BLAS; [notes](docs/bvh.md)). Not done in the builder: serialization. TRI1 pairing (no memory saved yet) and BOX16 nodes for `MINIMIZE_MEMORY` BLASes (an assumed switch) are in ([notes](docs/bvh.md)). Open: whether the game rehydrates the packs or rebuilds them ([plan](docs/d3d12_vulkan.md)) |

The table above is written by hand. To measure it on your own checkout (unit and runtime tests, which rows of
the translator contract were confirmed on a GPU, measured imports, your corpus counts and, with `--github`, CI
and the `D3D-` items), run `python -I tools/progress_dashboard.py --out <folder outside the repo>` and open
`progress.html` ([notes](docs/progress_dashboard.md)).


## Platforms

| Platform | Direct3D 12 runtime (`d3d12/`) | Vulkan runtime (`runtime/`) & tools |
|---|---|---|
| Windows | **Playable in the open world** on NVIDIA and AMD via `Launcher.bat` or `scripts/play-d3d12.ps1` ([`d3d12/README.md`](d3d12/README.md)). | Everything builds and runs natively: `make_game.py`, asset/shader tools, and `runtime/` (`gta6rt` + D3D12 → Vulkan layer) with its tests (boots through first-run UI and story load; [guide](docs/first_run_ui.md)). |
| Linux | Experimental launcher `scripts/play-linux.sh` runs a pre-built Windows `leonida.exe` under Wine with bundled VKD3D-Proton DLLs (not yet verified end-to-end on Linux). | The shader translator, asset tools and Vulkan GPU tests build and run natively (RADV and NVIDIA). `gta6rt` cross-compiles with `clang-cl` + `xwin` and runs under Wine or Proton ([Running the loader on Linux](#running-the-loader-on-linux); experimental RADV/Wine run drew world frames, [report](docs/linux_proton.md#amd-on-linux-what-got-the-world-drawn)). `./Build.sh` starts the build ([docs/linux.md](docs/linux.md)). |
| macOS (Apple Silicon) | Not supported natively. | Python tools and Vulkan GPU tests run natively on MoltenVK (Homebrew) or KosmicKrisp; `runtime/` loader tests run under Wine (CrossOver, arm64 with FEX, or x86-64 under Rosetta). Known gaps: no geometry stage or `VK_EXT_mesh_shader`, 32-wide subgroups (wave64 emulated), and MoltenVK float quirks ([docs/testing.md](docs/testing.md)). |

**AMD RDNA1 (RX 5000) note:** supported target for the Vulkan shader translator — close to the console's RDNA2, with
64-wide subgroups running wave64 shaders natively. On RADV, pixel shaders without fragment barycentrics have a known
difference (*Known problems* in [docs/testing.md](docs/testing.md)).

GPU test results from every platform are published in [`gpu-test-results/`](gpu-test-results); how to run the tests is
in [docs/testing.md](docs/testing.md).

## How to build

The port isn't downloaded ready-made. It's built on your PC from your own copy.

First, download this project **with its submodule**:

```powershell
git clone --recursive https://github.com/ProjectLeonida/mandy
cd mandy
```

### Starting the game: the launcher

If you want to **play in the open world on Windows** with the Direct3D 12 runtime (`d3d12/`), you do **not** need the
~400 GB `make_game.py` pipeline, .NET 8, Vulkan SDK, or LLVM below:

1. **Prerequisites (Windows):** Windows 10/11 x64, a CPU with AVX2, a Direct3D 12 GPU with Shader Model 6.6 and mesh
   shaders, [Python 3.12+](https://www.python.org/downloads/) (with Tkinter, included in the Windows installer), Rust
   (toolchain pinned in `d3d12/`), Visual Studio 2022 **Desktop development with C++**, and your own copy of the package
   (the folder containing `MicrosoftGame.Config`, the title's executable, and the `.rpf` archives). See
   [`d3d12/README.md`](d3d12/README.md#how-to-run-it).
2. **Optional (NVIDIA DLSS and Frame Generation):** run `d3d12\Get-NVIDIA-DLSS.cmd` once to fetch NVIDIA's files
   (~70 MB from NVIDIA's GitHub into `d3d12\nvidia-sdk\`). Without them the game still runs, stretching the image to the
   screen; if `leonida.exe` was already built without DLSS, rebuild it afterwards (`-Rebuild` or the launcher's Tools
   page).
3. Double-click **`Launcher.bat`** (Linux: `python3 tools/leonida_launcher.py`, which needs `python3-tk`). The launcher
   UI itself needs only Python and Tkinter; choose your game folder in *Settings* and click **PLAY**. On Windows, PLAY
   runs `scripts\play-d3d12.ps1` (which compiles `d3d12/` automatically on the first run and precompiles shaders before
   launch) with the switches chosen in Settings (DLSS, ray tracing, Smooth Motion, etc.). On Linux, PLAY runs
   `scripts/play-linux.sh` with the game executable and the optional Series X switch. See
   [docs/launcher.md](docs/launcher.md).

From PowerShell on Windows, you can also call `scripts/play-d3d12.ps1` directly:

- Default (Series X): `powershell -ExecutionPolicy Bypass -File scripts\play-d3d12.ps1 -Game "D:\path\to\your\package"`
- Lighter Series S profile with DLSS: `powershell -ExecutionPolicy Bypass -File scripts\play-d3d12.ps1 -Game "D:\path\to\your\package" -SeriesS -Dlss quality`
- Inspect settings without building or starting: `powershell -ExecutionPolicy Bypass -File scripts\play-d3d12.ps1 -Game "D:\path\to\your\package" -DryRun`

### Building the Vulkan runtime and asset tools (`Build.bat` / `Build.sh` / `make_game.py`)

This pipeline builds the recompiled Vulkan runtime (`runtime/` / `gta6rt.exe`), translates the RDNA2 shaders to SPIR-V,
and optionally exports assets for research tools.

**The easy way:**

1. Install the prerequisites below.
2. Clone with `--recursive` as shown above (`git clone --recursive https://github.com/ProjectLeonida/mandy`). A
   "Download ZIP" from GitHub leaves out `third_party/rpf9-explorer` and has no `.git` directory, so you need a Git
   clone; inside a Git checkout without the submodule, the build fetches it automatically when `git` is installed.
3. Double-click **`Build.bat`**. On Linux or macOS, run **`./Build.sh`** from a terminal instead (see
   [docs/linux.md](docs/linux.md) for Linux).

It finds your copy of the game by itself: it looks in Downloads, Desktop, Documents (also where Windows keeps them,
such as in OneDrive), Games, your home folder and the top of every drive, two folders deep. It recognises the
version from a few file sizes, without reading anything large. If you have more than one copy it asks which one, and if it finds none it asks you to paste the
folder. Before starting it checks that everything below is installed and that there is enough free space, and it
lists anything missing in one go. The result goes to `Documents\Leonida`. A stopped build continues where it left
off.

Your game folder is the one that holds `MicrosoftGame.Config` and the `.rpf` archives, as plain files.

**Prerequisites:**

- **Your own copy** of the Xbox Series package, version 1.0.2.1.
- **Windows 10/11 x64**, with about **400 GB** free for work files (an assets export alone measured about 337 GB before its audio step, in about 2 h 35 min). Disk sizes here are as Windows Explorer shows them, which are GiB: 400 GiB is about 430 GB in decimal units. They can go on another drive with `--work`.
- [Python 3.12+](https://www.python.org/downloads/) (tick "Add python.exe to PATH").
- [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0).
- [Vulkan SDK](https://vulkan.lunarg.com/), for `spirv-as` / `spirv-val`.
- [LLVM 21+](https://github.com/llvm/llvm-project/releases) with the **AMDGPU target** for `llvm-mc`. The official
  Windows build has no AMDGPU target. On Windows, `scripts\build_llvm_mc.ps1` builds a native `llvm-mc.exe` with it
  (no WSL; needs git and the Visual Studio C++ tools below) and sets `LLVM_MC`; it takes about 20-40 minutes. A faster
  way, if you use [conda](https://docs.conda.io/): `conda install -c conda-forge llvm-tools` (its Windows build
  includes the AMDGPU target; [docs/testing.md](docs/testing.md) used version 23.1.6), then set `LLVM_MC` to the full
  path of the `llvm-mc.exe` it installs (`where.exe llvm-mc` shows it once the environment is active; in PowerShell a
  bare `where` is `Where-Object`). Or install `llvm-21`
  in WSL and set `LLVM_MC` to the whole command, `LLVM_MC="wsl -d Ubuntu -e /usr/lib/llvm-21/bin/llvm-mc"` (see
  [docs/design.md](docs/design.md#disassembling-shaders)). On Linux, `llvm-mc` on PATH is enough, and so is a
  versioned distribution install such as Ubuntu's `llvm-21` (`llvm-mc-21`, `/usr/lib/llvm-21/bin/llvm-mc`).
- Visual Studio 2022 with **Desktop development with C++** and the **Windows SDK 10.0.26100 or newer**, for the
  runtime stage (the runtime uses version 1.2 root signatures, which older SDKs don't declare).
- [ffmpeg](https://ffmpeg.org/download.html) on PATH (optional: decodes the XMA2 streams, and required for `--wav`).

**Is this PC strong enough?** [Can I Run Leonida](https://assttro.github.io/can-i-run-leonida/) is a lightweight browser-based checker that compares your PC against reference profiles. It's a rough estimate, not a benchmark.

**Low disk space:** the ~400 GB figure is for the full build. To *run* the port, `--stages check,shaders,runtime`
needs about 30 GB; the `assets` stage (~370 GB) is only for the asset tools, and `--wav` adds several times the
Ogg Opus size (the preflight prints this choice when the assets stage is what does not fit). `--work` and `--out`
can sit on another drive. The runtime maps the package in place, so a read-only package drive needs
`gta6rt --overlay <writable folder>`; `--overlay` is a `gta6rt` option, not a `make_game.py` one. Leave the `.s`
files in `work/shaders/dxbc/disasm` in place: the next run checks them against the list in `disasm.done` and
disassembles the missing ones again (all of them for a folder whose `disasm.done` predates that list), then
translates every shader again. A `disasm` folder that is a symlink is refused when the stage has to clear it.

**Disk space and drives:** the build uses three folders, and each can be on its own drive:

| Folder | Flag | What is in it | Size | Read when playing |
|---|---|---|---|---|
| your package | `--game` | your copy of the game; only read, never written | as copied | yes, in place (`game_path.txt`) |
| work | `--work` (default `<out>\work`) | `shaders` (carved, disassembled, translated) and `assets` (exports) | about 30 GB + about 370 GB | only `shaders\spirv` (`shader_path.txt`) |
| port | `--out` (default `Documents\Leonida`) | `gta6rt.exe`, its DLL, the path files, the pipeline cache, `overlay` | small | yes |

The free-space check before a build counts the `--work` drive only, because that is where the large files go.
Nothing in `<work>\assets` is read by the game, so with a small SSD you can keep what the game loads on the SSD and
put the exports on a hard drive, in two runs:

```powershell
# on the SSD: the package, the port and the translated shaders (about 30 GB of work files)
python -I make_game.py --game "S:\Games\package" --out "S:\Games\Leonida" --work "S:\Games\Leonida\work" --stages check,shaders,runtime,pipelines
# on the hard drive: the asset exports (about 370 GB), only for the asset tools
python -I make_game.py --game "S:\Games\package" --out "S:\Games\Leonida" --work "H:\leonida-assets" --stages check,assets
```

The second run does not rebuild the runtime, so `shader_path.txt` keeps pointing at the SSD. Pass the same
`--work` again for later runs of each stage so they resume. A single `--work "H:\leonida-work"` also works; the
game then loads its translated shaders from that drive.

**From a terminal**, with the same behaviour and more control:

```powershell
python -I make_game.py --find                          # list the copies found on this PC
python -I make_game.py --pick                          # choose the game folder in a folder dialog
python -I make_game.py --check                         # check your copy, build nothing
python -I make_game.py --check --full-check            # also compare SHA-256 of every file (slow)
python -I make_game.py --game "D:\path\to\your\package" --out "D:\Games\Leonida" --work "E:\leonida-work"
python -I make_game.py --stages shaders,assets         # only some stages
python -I make_game.py --game "D:\path\to\your\package" --wav   # also decode Opus to PCM WAV (large)
python -I make_game.py --wave64-emulate off            # auto (default), on or off; see below
```

`--wave64-emulate` decides whether wave64 compute and mesh shaders are translated to be exact on any subgroup size
(see "Wave64 emulation" in [docs/design.md](docs/design.md)). NVIDIA and Intel GPUs have 32-wide subgroups and need
it. The default, `auto`, asks `vulkaninfo` (part of the Vulkan SDK) and turns emulation on if any GPU lacks 64-wide
subgroups or its subgroup size is unknown. It also checks whether the runtime's GPU can run compute and mesh
shaders 64-wide. With `--mesh-compute`, only compute subgroup support is needed.

On AMD's Windows driver, `auto` uses native wave64 on RDNA1 and RDNA2 when the runtime's GPU supports it, even
if another GPU is 32-wide or `vulkaninfo` is unavailable. That driver can crash on emulated LDS code (#857).
For RDNA1 cards without mesh support, the decision uses the compute stage, which can run wave64 without native
mesh support.

The mode is saved next to the translated shaders, and changing it translates them again. `auto` can normally
reuse previously emulated shaders. The AMD Windows workaround rebuilds those shaders in native mode.
Use `--wave64-emulate on` to force emulation or `--wave64-emulate off` to force native wave64.

Four fallback flags are forwarded to the translator the same way, saved next to the translated shaders, and changing
them translates them again: `--no-float-controls` (drop `SignedZeroInfNanPreserve`, for devices whose pipeline check
skips a shader without `shaderSignedZeroInfNanPreserveFloat32`), `--bary-fallback`
(read raw barycentrics from an interpolated input, for devices without `VK_KHR_fragment_shader_barycentric`),
`--texel-offset-fallback` (fold `image_sample*_o` offsets into the coordinate, for devices without
`VK_KHR_maintenance8`) and `--mesh-compute` (mesh shaders as compute, for devices without `VK_EXT_mesh_shader`;
on by itself on Intel's Windows Vulkan driver, whose native mesh draws crash, BUILD-2202, unless `--native-mesh`).
Each is documented in [docs/design.md](docs/design.md).

The build records which llvm-mc command and version produced the disassembly. Changing either, or reusing a work
folder that predates this record, reruns disassembly and translation while keeping the extracted shader inputs.

The build records which translator sources produced the shaders. A change to those files reruns translation using the
saved disassembly. Older work folders need one translation after upgrading to record this information.

The build also records which decoder (the `third_party/rpf9-explorer` commit and the `tools/rpf9dump` sources) exported
each archive's models and textures and each texture pool. When it changes, the assets stage removes that export and
makes it again. Older work folders have no record, so their models, textures and pools are exported once more after
upgrading.

Stages: `check` always runs first; then `shaders`, `assets` and `runtime` run together when selected (they do not
depend on each other). `pipelines` starts once the translated shaders are ready and, when this run includes runtime,
that stage has finished rebuilding `vk_pipeline_check`, so it can overlap the rest of assets; it creates every
translated shader on your GPU driver, reports any it has to skip, and saves a pipeline cache to `pipeline_cache.bin`
in `--out`, which also holds `gta6rt.exe` after the `runtime` stage. The runtime seeds its Vulkan pipeline cache
from the file beside the executable when its header matches the device and pipeline cache UUID. It falls back to
an empty cache if the file is missing, incompatible, or rejected by the driver. Shader translation is the longest
CPU step. It runs on `--jobs` processes (half the CPU threads by default). When shaders and assets share the machine,
that budget is split between them and passed to `rpf9dump` as `--jobs`. A solo assets run still lets `rpf9dump` use
every core. Progress lines include percent and a rough ETA for translation shards, asset steps, and overall stage
completion.

On an interactive terminal, a LEONIDA setup screen lets you set jobs and whether stages overlap, then a key-switchable
per-stage log view (`1`–`4` / `Tab`) keeps parallel output demuxed; full stage logs go under `work/logs/`. Use
`--no-tui` (or redirect stdout) for the plain console.

`--wav` is off by default because 16-bit PCM WAV is several times larger than Ogg Opus. It decodes the repackaged Ogg
Opus streams to `work\assets\audio\wav` after the audio bank step; existing WAV files are skipped when a build resumes.

### Vulkan diagnostics on Linux

With CMake 3.24+, a C++20 compiler, and Vulkan development headers and loader installed:

```sh
cmake -S runtime -B runtime/build/linux
cmake --build runtime/build/linux --parallel
./runtime/build/linux/vk_pipeline_check /path/to/spirv
```

Pass the directory containing `index.json` and the translated SPIR-V files to `vk_pipeline_check`.

`make_game.py` runs on Linux too. The `pipelines` stage uses the `vk_pipeline_check` built above
(`runtime/build/linux`, or `runtime/build`), and builds it there itself before any other work if it is missing.

### Running the loader on Linux

The loader is a Windows program and runs under Wine or Proton; CI runs its tests under Wine on every push to
`main` that changes `runtime/`, and on demand from the Actions tab (the `wine` job of the Runtime workflow). On Linux the `runtime` stage cross-builds it into
`runtime/build/windows` with clang-cl and lld-link (LLVM 19+, but not 22.1.7 or 22.1.8, see [docs/testing.md](docs/testing.md#known-problems); plain names, or all five LLVM tools with one version
suffix such as `clang-cl-21`) against the MSVC CRT and Windows SDK that
[xwin](https://github.com/Jake-Shadle/xwin) lays out, in `XWIN_DIR` or `~/.local/share/xwin-sdk`
([docs/testing.md](docs/testing.md#the-windows-runtime-on-linux-under-wine-or-proton) has the one-time `xwin`
command). Without them the stage is left out of the default stages, and asking for it lists what is missing before
any work starts. A Windows build made elsewhere works too, such as the `runtime-windows` artifact of that workflow's
`windows` job, unzipped:

```sh
python3 -I make_game.py --runtime-build ~/Downloads/runtime-windows
```

Either way it copies `gta6rt.exe` and `gta6rt_tls_host.dll` into `--out` and writes `gta6rt.sh` next to them, which
starts `gta6rt.exe` with `wine`, or with the Wine named by `WINE` (for Proton, its `files/bin/wine`). The same option
skips the build on Windows.

For `tools/pe_inspect.py`'s import classes on Linux, set `WINEPREFIX` to the prefix the loader runs in (or use
`~/.wine`); without one, system DLLs are reported as `system (unverified)`
([docs/design.md](docs/design.md#day-one-when-the-exe-is-here)).

On Linux, run `<out>/gta6rt.sh --dry-run` to inspect the packaged title's imports, then
`<out>/gta6rt.sh --init` to check TLS callbacks and DLL initialization. Use `<out>/gta6rt.sh --run` for startup.
These commands use the package in `game_path.txt`, the port's `overlay` for writes, and Vulkan by default.

When the Vulkan backend is selected with `LEONIDA_D3D12=vulkan`, the packaged runtime finds the translated shaders
in `<work>/shaders/spirv` using `shader_path.txt` beside `gta6rt.exe`. Set `LEONIDA_SHADERS` to use another directory.

## How it works

| Piece | What it does | Where |
|---|---|---|
| Direct3D 12 runtime | Playable Windows runtime (`leonida.exe`) with launcher, shader cache/precompilation, optional DLSS and F1/F3/F4 in-game menus | [`d3d12/`](d3d12/README.md), `Launcher.bat`, `scripts/play-d3d12.ps1` |
| Version check | Every package file's name, size and SHA-256 for each supported version | `game/`, `make_game.py` |
| Shader pipeline (`runtime/`) | Carves DXBC containers from `shaders.rpf` without its encrypted table of contents. Unpacks the `XBCH` → `FSSM` native RDNA2 program, disassembles it with LLVM (`gfx1013`) and translates it to SPIR-V | `tools/shader_carve.py` ([optional Rust scanner](tools/shader_carve_native/README.md)), `fssm_unpack.py`, `shader_disasm.py`, `rdna2spirv.py` |
| Asset export | Finds validated resources inside RPF9 archives and the loose `.ttpf` texture pools | `tools/rpf9dump` (on rpf9-explorer's Core) |
| Vulkan runtime (`runtime/`) | Maps the PE image, resolves imports to shims (GDK, XMem, `d3d12_xs` → Vulkan) | [`runtime/`](runtime/README.md) |
| Research tools | PE/RTTI inspection, raw carving, texture tiling probes, GPU test harnesses | `tools/`, `runtime/test/` |
| Progress dashboard | One offline page of what passes, what changed since it last passed and what blocks next, built from test results and the docs' own tables; nothing is written inside the repo | `tools/progress_dashboard.py` |

Localized string tables are decoded by `tools/text_tables.py` ([docs/text.md](docs/text.md)); their language is
guessed from script (kana, hangul, CJK, Cyrillic, Arabic) or Latin words, and subtitles are joined to dialogue lines
across languages.

The translator's runtime contract (user SGPRs, push constants, memory model, bindless images, vertex fetch,
system values) and all the format notes are in [docs/design.md](docs/design.md); the format, asset, runtime
and validation notes are indexed in [docs/README.md](docs/README.md). The Xbox D3D12 → Vulkan plan is in
[docs/d3d12_vulkan.md](docs/d3d12_vulkan.md). Audio bank formats and the export tools are in
[docs/audio.md](docs/audio.md); [docs/related_projects.md](docs/related_projects.md) records the prior art we may
read and the licence limits on it.

## Want to help?

The biggest open pieces in the recompiled Vulkan runtime (`runtime/`) and research tools:

- **World rendering & stability (`runtime/`)**: an experimental Linux/RADV run renders world frames, but stability
  (device loss, 12-minute title breakpoint) and frame correctness (alternating white/black frames, misplaced geometry)
  remain open ([AMD on Linux](docs/linux_proton.md#amd-on-linux-what-got-the-world-drawn),
  [Where it stops](docs/linux_proton.md#where-it-stops)).
- **Shader translator on hardware (`runtime/`)**: compute, mesh and geometry now translate (see the translator contract in
  [docs/design.md](docs/design.md)); still open is wave64 cross-half code in VS/PS/GS on 32-wide subgroups. The
  vertex-shader input layout is measured: all 2,236 VS are legacy (non-NGG), with `v0` = `VertexIndex` (the
  *Inputs and outputs* row of [docs/design.md](docs/design.md)).
- **Xbox D3D12 → Vulkan (`runtime/`)**: remaining per-method gaps (mesh-shader, indirect-draw and stream-output edge
  cases, console list tail and descriptor-table record model) are tracked in [Plan and status](docs/d3d12_vulkan.md).
- **Model recovery**: investigate the 71 unrecovered fragment candidates in [`levels.rpf`](docs/models.md#full-archive-re-count-at-e293917).

See [CONTRIBUTING.md](CONTRIBUTING.md). Never attach game files, extracted assets or generated shaders to issues or
pull requests.

## Credits

- [Leonida](https://github.com/ProjectLeonida/Leonida) by NotRexed and contributors: the original project, which this
  repository continues.
- [rpf9-explorer](https://github.com/breadbbbb/rpf9-explorer) by breadbbbb: the RPF9 model, texture-pool and AWC
  decoders used by `tools/rpf9dump` (git submodule).
- [Mesa](https://gitlab.freedesktop.org/mesa/mesa) (MIT): GFX10 resource-descriptor layout (`tools/gfx10_formats.json`),
  and the ACO/RADV rules used as reference for 16-bit results and shader system registers.
- [libHttpClient](https://github.com/microsoft/libHttpClient) (MIT) by Microsoft: the XTaskQueue/XAsync implementation
  in `third_party/xtaskqueue`, used by the offline `xgameruntime` shim.
- [LLVM](https://llvm.org/): AMDGPU disassembler.
- Logo: ["Grand Theft Auto VI" icon by SlyDevil](https://www.steamgriddb.com/icon/122618) on SteamGridDB.
- [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools): assembly and validation.
- Aki-Developer: `aki-gta6/aki-runtime` (GPL-3.0), a sister port shared with the project. Taken from it into the Rust
  D3D12 runtime: the count of dropped draws by reason in the frame line, descriptor slots given back as soon as the
  card has run a submission, pixel colours held to the console's HDR range, the composer's clamp that keeps a NaN from
  coming out black, two planes composed into 10 bits under frame generation, the paired form of wave64 programs tried
  before the approximate ones (`LEONIDA_PAIRED_LANES=exact`), a pipeline's kept skip answer, and four small fixes it
  carried from fxceless/victory-fork. Each commit names the `aki-runtime` commit it is from.

Grand Theft Auto and all related names are trademarks of their owners. This project is not affiliated with or
endorsed by them.

## License

GPLv3 (or any later version). See [LICENSE](LICENSE). Third-party code keeps its own license.
