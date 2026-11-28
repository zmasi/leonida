# Running the game, per platform and GPU

How the retail game has been run on each platform and GPU so far: the build, the shader pack, the pipeline
pre-compile, the driver and OS settings, the environment and what to expect. Every value is marked:

- **measured by us**: by the maintainers who wrote this page, on the machine named in that section;
- **reported by others**: from a public issue or comment, linked; not checked by us;
- **TBD**: nobody has reported it yet. A report fills it in ([Reporting](#reporting)).

Paths are placeholders: `<checkout>` is the repository, `<build>` the folder with `gta6rt.exe` and
`gta6rt_tls_host.dll`, `<work>` the work folder `make_game.py` writes, `<cache>` a folder for pipeline caches,
`<package>` the game's package folder and `<exe>` its executable. Under Wine, a Linux path is passed as `Z:<path>`.

## Settings every platform needs

- **The title's hang watchdog.** The title exits on purpose (a write to address 0) when its main loop has not reported
  for 30 s, which a world load under the runtime exceeds. `LEONIDA_TITLE_WATCHDOG=0` keeps it from firing only together
  with `LEONIDA_TITLE_WATCHDOG_KICK=<trap RVA>,<kick RVA>` (hex), two addresses in the executable that each person
  measures in their own copy (`runtime/src/main.cpp` describes the trap instruction the first must point at). The
  values are never committed. With `=0` alone the loader logs that the watchdog stays armed. (Measured by us; the
  watchdog was identified in [#2019](https://github.com/ubiquitarianism/leonida/issues/2019#issuecomment-7172996360).)
- **The GPU timeout.** World frames take seconds each. The OS's GPU timeout must be longer, or the driver resets the
  GPU: amdgpu's `lockup_timeout` on Linux, `TdrDelay` on Windows (sections below).
- **Large ray-tracing programs.** 724 library compute programs have 2 MiB of SPIR-V or more (676 of them ray-tracing
  resolve). With wave64 emulation they took 89-167 s each in the driver ([reported by others](https://github.com/ubiquitarianism/leonida/issues/1608#issuecomment-7184700506);
  the count of 724 also measured by us). With a native wave64 pack compile time is no reason to leave them out: on an
  RX 7900 XTX (RADV, Mesa 26.3.1, Wine) all 721 such programs of that pack built, 360 of them in 170 s on 32 threads,
  2.5-32 s each, the largest (13 MiB) in 28.5 s alone, at a peak of 14 GiB of RAM (measured by us). The pipeline
  pre-compile builds them with the rest. Dispatched at play (measured by us, one run per condition: RX 7900 XTX 24 GB,
  `LEONIDA_RT_AS=off`, no skip list, all 721 in the journal), they all came from the journal (2,700 compute
  pipelines from it against 1,900 with the skip list set; none built in the game) and the load reached the world.
  One run (a local build of `main` with four then-open pull requests) stopped submitting after 404 s with the GPU idle
  and no GPU fault; the other (`main` `bd6ba97ab`) lost the device about 214 s in, after a read page fault, with
  every queue idle in the dispatch markers. The same idle-queue device loss also happens with the skip list set, so
  it is not shown to come from these programs, and running them got no further than the skip list's best runs
  (about 11 minutes, ending on a title breakpoint). Whether their output is correct is not measured. Our longest
  runs used `LEONIDA_SKIP_COMPUTE=<name>,<name>,...` with those programs' names (picked by module size from
  `<work>/shaders/spirv/library/index.json`), which drops their dispatches at play, and `LEONIDA_RT_AS=off`, which
  drops acceleration-structure builds. The picture then lacks what they draw.

## Linux, Wine, AMD RX 7900 XTX (RADV)

Measured by us on 2026-11-26: RX 7900 XTX, Ryzen 9 7950X3D, Linux 7.3, Mesa 26.3.1 (RADV), Wine. Details and the
reasons behind each step: [linux_proton.md](linux_proton.md#amd-on-linux-what-got-the-world-drawn).

- **Build:** the clang-cl + xwin cross-build ([testing.md](testing.md#the-windows-runtime-on-linux-under-wine-or-proton)),
  with SDL3 for the keyboard pad.
- **Shaders:** `python -I make_game.py --stages shaders --wave64-emulate off --work <work> --out <out>`. The GPU runs
  wave64 natively; emulation is not needed. (`auto` picks the same here since llvmpipe no longer decides.)
- **Pipeline pre-compile:** `wine <build>/gta6rt.exe --precompile-pipelines --threads 16` with the run's
  `LEONIDA_SHADERS`, `LEONIDA_PIPELINE_CACHE_DIR` and `LEONIDA_DEVICE_FAULT`, before the first run. It built 3,693
  compute pipelines. Not `make_game.py --stages pipelines`: its cache never matches the runtime's driver keys.
  The pre-compile builds the programs `LEONIDA_SKIP_COMPUTE` names too (`--skip-listed` leaves them out) and ends
  with one line that counts what it built, what was journaled already and what it left out.
- **Driver and OS:** `amdgpu.lockup_timeout=30000,60000,10000,10000` on the kernel command line (the default is
  2000 ms; reboot needed).
- **Environment:**

  | Setting | Why |
  |---|---|
  | `LEONIDA_D3D12=vulkan` | the Vulkan backend |
  | `LEONIDA_SHADERS=Z:<work>/shaders/spirv` | the native pack |
  | `LEONIDA_PIPELINE_CACHE_DIR=Z:<cache>/pso` | the journal the pre-compile filled |
  | `MESA_SHADER_CACHE_DIR=<cache>/mesa`, `MESA_SHADER_CACHE_MAX_SIZE=16G` | RADV's own cache, large enough for the pack |
  | `LEONIDA_STARTUP_CACHE_CHECKPOINTS=1` | session cache snapshots during the boot |
  | `LEONIDA_ASYNC_PIPELINES=0` | draws wait for their pipeline instead of being skipped |
  | `LEONIDA_SERIAL_BIG_COMPILES=0`, `LEONIDA_COMPUTE_WARMUP_MAX_BYTES=8388608` | warm-up limits, as in the NVIDIA report below |
  | `LEONIDA_RT_AS=off`, `LEONIDA_SKIP_COMPUTE=<list>` | the large ray-tracing programs, left out at play as in our longest runs ([above](#settings-every-platform-needs)) |
  | `LEONIDA_TITLE_WATCHDOG=0`, `LEONIDA_TITLE_WATCHDOG_KICK=<trap RVA>,<kick RVA>` | the title's hang watchdog ([above](#settings-every-platform-needs)) |
  | `LEONIDA_DEVICE_TYPE=6`, `LEONIDA_MAP_READBACK=1`, `LEONIDA_FRAME_PACING=1`, `LEONIDA_PRESENT_NO_TEAR=1` | taken over from the NVIDIA report below; their effect on AMD was not measured one by one |
  | `LEONIDA_ARENA_MB=4095`, `LEONIDA_TITLE_WINDOWS_MB=4096`, `LEONIDA_GEOMETRY_MB=6144` | room for the title's buffers in the world |
  | `LEONIDA_SHADOW_ALIAS_MS=5000`, `LEONIDA_CONSOLE_FIX_US=0` | carried from earlier boot runs; their effect here was not measured one by one |
  | `LEONIDA_DEVICE_FAULT=1` | a device-loss report if the GPU hangs |
  | `LEONIDA_KEYBOARD_PAD=always` | the keyboard as the pad |

- **Command:** from `<package>`, `wine <build>/gta6rt.exe --run Z:<exe> --game-dir Z:<package>`.
- **What to expect:** menus at 30 frames/s with no in-game compiling; the world after the story load, over eight
  minutes without a GPU hang, at about one frame per six seconds.
- **Known problems:** frames alternate between white-out, a correct picture and black; some geometry is drawn in the
  wrong place; VRAM reached 18.3 of 22.3 GB; the run ended on a breakpoint (`int3`) in the title after 12 minutes.

## Linux, Wine, AMD RDNA2 iGPU (Ryzen 7000, RADV)

Measured by us on 2026-11-25/26, the same machine's integrated GPU (gfx1036), headless, before the watchdog's KICK
was known. The settings that drew the world on the RX 7900 XTX have not been tried here: TBD.

- **Build, shaders, pipeline pre-compile:** as for the RX 7900 XTX; the iGPU also runs wave64 natively.
- **Driver and OS:** the same `lockup_timeout` applies (TBD whether the values above are enough here).
- **Environment:** select the iGPU with `MESA_VK_DEVICE_SELECT=1002:164e` and
  `MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE=1`. A cold compute warm-up made the title crawl at 0.12 frames/s behind
  our own warm-ups; `LEONIDA_COMPUTE_WARMUP_LIVE_MAX_BYTES=131072` holds large warm-ups back while the title submits
  (191 frames/s in the menus with it, measured on the RX 7900 XTX).
- **What to expect:** the boot, the first-run pages and the story load's loading screen (about 1,100 s into a run with
  a partly warm cache). Every run then ended in the title's watchdog exit, about two minutes into the world load,
  because only `LEONIDA_TITLE_WATCHDOG=0` was set. World frames: TBD.
- **Known problems:** TBD beyond the above.

## Linux, Proton, NVIDIA

Reported by others: [#1608, comment 7184700506](https://github.com/ubiquitarianism/leonida/issues/1608#issuecomment-7184700506)
(RTX 3090 24 GB, NVIDIA 590.48.01, GE-Proton11-14, Debian 13). Not run by us.

- **Build:** clang-cl + xwin cross-build; `main` plus several pull requests and two local changes (listed in the report).
- **Shaders:** the main corpus, the library and FSPX with llvm-mc 23.1.6. Wave64 emulation: TBD from the report (it is
  needed on NVIDIA's 32-wide subgroups; `auto` turns it on there).
- **Pipeline pre-compile:** caches warm from two earlier runs; no separate pre-compile is reported.
- **Driver and OS:** TBD.
- **Environment:** `LEONIDA_ASYNC_PIPELINES=0`, `LEONIDA_RT_AS=off`, `LEONIDA_SKIP_COMPUTE=<the 724 programs>`,
  `LEONIDA_DEVICE_TYPE=6`, `LEONIDA_TITLE_WATCHDOG=0`, `LEONIDA_MAP_READBACK=1`, `LEONIDA_HOST_IMAGE_COPY=0`,
  `LEONIDA_ARENA_MB=4095`, `LEONIDA_TITLE_WINDOWS_MB=4096`, `LEONIDA_SERIAL_BIG_COMPILES=0`, `LEONIDA_FRAME_PACING=1`,
  `LEONIDA_PRESENT_NO_TEAR=1`. The report gives the reason for the skip list only: without it the world start waits
  hours on the ray-tracing programs; with asynchronous pipelines instead the screen stayed black. Those compile times
  are the report's pack; a native wave64 pack builds them in seconds each ([above](#settings-every-platform-needs)).
  Where they still take minutes, `gta6rt --precompile-pipelines --big-threads N` bounds how many build at once (each
  held about 3.5 GB of host memory on an RTX 4070 Ti) and `--skip-listed` leaves the listed ones out.
  Between the two: `LEONIDA_ASYNC_PIPELINES=1` with `LEONIDA_ASYNC_MIN_BYTES=2097152` builds only the pipelines of
  2 MiB of SPIR-V or more asynchronously (the ray-tracing programs above), and the smaller lighting and post programs
  where they are needed, as with `=0` ([d3d12_vulkan.md](d3d12_vulkan.md), "Only the large ones asynchronous"). Whether
  that draws the world without the skip list: TBD. `LEONIDA_ASYNC_MIN_BYTES_GRAPHICS=0` with it leaves every graphics
  pipeline asynchronous, so the draw thread does not stop presenting to build graphics variants in place.
- **Command:** TBD.
- **What to expect:** the opening night street from about 650 s, at 0.1-0.2 frames/s, no device loss through 934 s.
  The black stretch before it is graphics pipelines of 1-2.6 MB SPIR-V compiling one at a time, 5-10 s each.
- **Known problems:** nothing ray-traced is in the picture.

## Windows, NVIDIA

Reported by others: [#2019](https://github.com/ubiquitarianism/leonida/issues/2019) (RTX 5090 32 GB, driver 616.92,
Windows 11). Not run by us.

- **Build:** `scripts/build_runtime.ps1` with SDL3, RelWithDebInfo.
- **Shaders:** `make_game.py --stages check,shaders,runtime` (26,503 shaders translated); wave64 emulation as `auto`
  decides (on for NVIDIA).
- **Pipeline pre-compile:** TBD.
- **Driver and OS:** `TdrDelay` and `TdrDdiDelay` raised to 60 s, after which an 8.9 s pipeline build no longer
  reset the device.
- **Environment:** `LEONIDA_D3D12=vulkan`, `LEONIDA_KEYBOARD_PAD=always`; the rest TBD.
- **Command:** `gta6rt.exe --first-run-ui game_scarlett_master.exe` from the package folder.
- **What to expect:** the first-run UI, then the title's watchdog exit at 145-204 s. With `LEONIDA_TITLE_WATCHDOG=0`
  (before the KICK variable existed) the boot went on at about 24 frames/s and the machine then bugchecked
  (`CRITICAL_PROCESS_DIED`) before a world frame. With the KICK values: TBD.
- **Known problems:** TBD.

## Windows, AMD

TBD: no public report of a game run on Windows with an AMD GPU is known to us. What the code does: on RDNA1 and RDNA2
under AMD's Windows driver, `make_game.py`'s `auto` translates native wave64, because that driver crashes on many
emulated wave64 compute pipelines (#857). `TdrDelay` applies as for NVIDIA.

## Intel

TBD.

## macOS (MoltenVK)

TBD.

## Reporting

Post a run in the coordination issue, with the fields of these sections: machine (OS, GPU, driver, RAM), the commit,
how the shaders were made, the pipeline pre-compile, the OS and driver settings, every `LEONIDA_*` variable that
differs from the defaults (never the watchdog's RVA values), and how far the run got with its frame rate. Keep logs,
traces and screenshots on your machine ([linux_proton.md](linux_proton.md#reporting-a-result)).
