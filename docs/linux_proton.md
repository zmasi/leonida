# Linux (Proton/Wine) and second-machine test report

This page describes how the runtime is built and booted on Linux through Proton's Wine, what a healthy boot looks
like there, and what was observed on the machines that tested this branch (one Linux, two native Windows). It holds
counts, timings and settings only: no game data, no traces.

Status in one line: on Linux/Proton and on native Windows the retail 1.0.2.1 package launches, shows its launch
movie, first-run pages and landing prompt, runs the story load to its end with the loading slideshow drawing its
artwork, and enters the world phase. A first world frame has been verified on Linux/AMD (RX 7900 XTX, RADV, 2026-11-26; see [AMD on Linux: what got the world drawn](#amd-on-linux-what-got-the-world-drawn)); it has not been verified yet on any of the three machines in the table below, and what stops it there is listed under [Where it stops](#where-it-stops).

## Test machines

| | Linux | Windows A | Windows B |
|---|---|---|---|
| OS | CachyOS (Arch), kernel 7.3 | Windows 11 | Windows 11 |
| Run through | Proton Experimental (Wine 11) | native | native |
| CPU | Ryzen 5 5600X | Core i9-13900K | Ryzen 7 9800X3D |
| GPU, driver | RTX 4070 12 GB, NVIDIA 620.73 | RTX 4070 Ti 12 GB, NVIDIA 630.20 | RTX 5070 |
| RAM | 16 GB (+32 GB swap file) | 64 GB | not reported yet |
| Toolchain | clang-cl 23 + xwin SDK, Ninja, ccache | MSVC 2022, SDK 26100 | - |
| Results here | yes, ~80 boots on 2026-11-24 | yes | joining; none yet |

Windows A reaches every stage three to five times faster than the Linux machine (initialisation ~70 s against
~180 s, story load from ~110 s against ~240-300 s), so it is the better machine for iterating on the world phase;
the Linux machine is the one that shows what 16 GB of RAM allows.

## Building on Linux

The runtime is a Windows executable. On Linux it is cross-built with clang-cl against an [xwin](https://github.com/Jake-Shadle/xwin)
copy of the Windows SDK:

```sh
scripts/linux-boot/configure-worktree.sh <checkout>      # XWIN_DIR=<sdk>, SDL3_SRC=<SDL3 tree> for an offline configure
cmake --build <checkout>/runtime/build/boot --target gta6rt -j6
```

`gta6rt_tls_host.dll` is built with the `gta6rt` target and must stay next to the executable. Unit tests are Windows
executables too and run under Wine (`scripts/run_wine_tests.py`, or directly with the same `wine` binary and a
prefix of their own: sharing the game's prefix with tests breaks its window).

## Booting on Linux

`scripts/linux-boot/boot.sh` is the plain launcher and `run-boot.sh` wraps it as a `systemd --user` unit with a
memory sampler, a GPU lock, a build lock (no compile while the game runs) and a digest after the boot. They assume
the layout the comments at their top describe (`/opt/leonida` for traces, overlay and caches; the package under
`/opt/gta6`); adjust the paths at the top to yours. What matters:

- **Wine:** Proton Experimental's `files/bin/wine` with its own prefix (`WINEPREFIX`), `WINEDLLOVERRIDES="mscoree,mshtml="`.
- **No network:** the title runs inside `unshare -rn` (new user and network namespace). Nothing is online.
- **Backend:** `LEONIDA_D3D12=vulkan`, `LEONIDA_SHADERS=<translated library>`.
- **NVIDIA shader cache:** `__GL_SHADER_DISK_CACHE_SIZE` raised (the library is ~19 GB of SPIR-V) and
  `__GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1`.
- **Input:** a pad through GameInput/SDL3, or the keyboard pad (`LEONIDA_KEYBOARD_PAD=always`), or a script:
  `LEONIDA_INPUT_SCRIPT=a@210,a@240` presses A at those seconds (any length since this branch; it was cut at 63
  characters before). The script also works in a build without SDL3, where it drives one scripted pad
  ([docs/gameinput.md](gameinput.md#scripted-input-without-sdl3)); pads and the keyboard pad need SDL3.
- **Window:** frames are presented in the title's own window; `LEONIDA_PRESENT_WINDOW=own|hidden` chooses otherwise.

Three setup mistakes each give a blank or empty picture without an error: the executable's embedded shaders missing
from the library (`exe/index.json`), a build without SDL3 and without `LEONIDA_INPUT_SCRIPT` (no pad, so nothing
gets past the first prompt; set a script, or build with SDL3 for a pad or the keyboard pad), and a BCPack predictor
file that was never generated (`tools/build_dstorage_history.py`; without it the loading art and most BC7 textures
stay empty).

## Running the game under Wine with one command

`scripts/run_game_wine.sh` makes one bounded, silent run of the retail game under Wine and prints counts only. It
complements `scripts/run_wine_session.sh` (the runtime's unit tests under Wine) and the `systemd --user` boot
wrappers described above: it needs no fixed paths, and its output is a summary that can go into an issue.

```sh
scripts/run_game_wine.sh --build <build folder or checkout> --exe <game executable> --game-dir <package folder> \
    --shaders <translated library> --out <new run folder> --device 1002:744c --seconds 600
```

`--help` lists every input. Each path comes from an argument or from `LEONIDA_GAME_EXE`, `LEONIDA_GAME_DIR` and
`LEONIDA_SHADERS`; `WINE` names another Wine (Proton's `files/bin/wine`), and `WINEPREFIX` (or `--prefix`) a prefix to
reuse instead of a new one in the run folder.

What it needs:

- **Wine** (the `wine` on `PATH`, or `WINE`), `timeout`, `flock`, `setsid` and Python 3.
- **A Vulkan driver** for the GPU (Mesa's RADV was tested). `--device VID:DID` pins it through
  `MESA_VK_DEVICE_SELECT`; without it the script warns that Mesa picks the device.
- **A display.** By default the run is headless: a private `kwin_wayland --virtual --xwayland` session under
  `dbus-run-session`, with its own `XDG_RUNTIME_DIR` in the run folder, so nothing appears on the desktop. Its
  rootless Xwayland offers DRI3, which RADV needs to present; Xvfb has none, so without `kwin_wayland` the script
  says what is missing instead of falling back. `--visible` opens the window on the current `DISPLAY` instead.
- **A runtime build**: a folder with `gta6rt.exe` and `gta6rt_tls_host.dll`, or a checkout, whose `runtime/` it
  cross-builds first with clang-cl and xwin (`XWIN_DIR`, see [linux.md](linux.md#the-runtime-under-wine-or-proton)).

What it does:

- **Sound off:** Wine's audio drivers are disabled through `WINEDLLOVERRIDES`, PulseAudio, PipeWire and ALSA point
  at nothing, and `LEONIDA_AUDIO=silent` makes the runtime serve its silent output endpoint
  ([runtime_imports.md](runtime_imports.md)); the summary says whether it was served.
- **Limits:** a hard time limit (`--seconds`, then SIGTERM and SIGKILL 10 s later), a lock so one run happens at a
  time (`--lock`, by default in `XDG_RUNTIME_DIR`), and teardown of Wine and the whole session on exit, time-out or
  signal.
- **Traces:** `gta6rt --run` with `--trace` and `--trace-d3d12`, a fresh `--overlay` and save folder in the run
  folder. Every other `LEONIDA_*` variable reaches the runtime unchanged, so `LEONIDA_INPUT_SCRIPT=a@210,a@240`
  presses A as above. Arguments after `--` go to `gta6rt`.
- **Summary:** `tools/game_run_summary.py` prints the run time, how it ended, exceptions by code, D3D12 calls by
  outcome, presents, the most frequent failing calls and the `[rt]` lines by category with numbers masked. It never
  prints a log line. `--screenshot-every S` captures the game window into the run folder (ImageMagick's `import` and
  `xprop`).

The run folder's `stderr.log`, `boot.jsonl` and screenshots can show or quote what the title prints, so they stay on
your machine; share the summary.

## What a healthy boot looks like (Linux machine)

| Time | Stage |
|---|---|
| 0-10 s | package splash |
| ~60-90 s | launch movie, then the landing prompt appears while initialisation finishes |
| ~180 s | initialisation done; A is accepted |
| ~240-300 s | story load starts: loading slideshow with its artwork |
| ~540-630 s | story load ends (about 38,000 streamed images registered), world phase begins |

On Windows A the same stages are at ~70 s, ~110 s and ~190 s.

## What this branch changes to get there

Each item was a stop on at least one of the machines. Tests named in the pull request cover them.

- **Presentation:** frames go to the title's own window (they went to a window that was never shown).
- **DirectStorage:** reads pin write-watched destination pages, page-list scatter/gather, BCPack and unshuffle, a
  locally generated predictor file, bounded staging.
- **Story-load allocation stall:** one device allocation per streamed texture exhausted the driver's CPU-visible
  mappings near 16,000 images; small streamed images now share pooled pages.
- **Images on plain device-local memory:** host-copy usage excluded it on NVIDIA and 4.8 GB of images spilled into
  host memory; streamed images are now filled through staging.
- **Deferred backing:** placed and streamed images get private memory at their first write or GPU copy. VRAM at the
  end of the load went from 11.8 GB to ~5 GB on Linux; on Windows A host private memory went from 22-24 GB to 11.9 GB.
- **Enhanced barrier into `LAYOUT_UNDEFINED`:** accepted; it failed `Close`, which the title treats as fatal.
- **Bindless table:** a table rebuild that cannot allocate no longer refuses the whole submission; larger image
  arrays; a hash finalizer for the descriptor table (runtime and translator halves, off until a library carries it).
- **Translator:** loop bounds on every emitted loop (5,128 compute modules have loops), the eight FidelityFX programs
  that used an instruction form the disassembler skips (all 29 translate), buffer bounds modes per view kind.
- **Directly indexed descriptor heaps:** the heap offset is supplied in the user-data dword after the explicit root
  arguments for compute and for VS/PS/GS draws. Mesh stages are not covered yet.
- **Title buffers at raw addresses:** a window outside the arena when title reservations fragment it; opt-in
  persistent device-local copies of streamed buffer views.
- **Native Windows:** write faults on shared pages retried under the tracker lock, one tracker per run of shared
  pages, write-watched title memory, a fault-lock that never waits for the process heap.
- **Diagnostics:** device-loss report with the last dispatches and NVIDIA checkpoints, a probe for the descriptors a
  compute dispatch receives, memory probe, press log. All off by default.

## Where it stops

- **Writes into title memory.** A window is a read-side copy, so a UAV or raw write used to stop at the copy: the
  missing return copy is the `WRITE_INVALID` device loss of #1537. The layer now copies the reached ranges back to
  the title's pages at the next refresh, before the forward copy, and the title's own newer words win. Not yet
  measured on a GPU with the world phase in front of it.
- **Streamed memory that reaches its destination late or not at all.** The title calls queue slot 31
  (`CopyPageMappingsBatchX`) about 300 times per boot. With #968 this branch maps each batch's pool pages at the
  destination and at the paired CPU view the layer reads (`docs/d3d12_vulkan.md`, section 4.9), so most streamed
  views find readable memory; before it, tens of thousands of them waited for memory that never became readable. Two gaps
  remain: the mappings change when the call is made, not in queue order, and a run with the CPU-view fix still had
  views waiting after 420 s, apparently for ranges no mapping call had reached yet.
- **Culling compute at world start** reads buffers named by descriptors in the directly indexed heap; where the
  memory behind them is not mapped, those reads leave mapped memory and the device is lost (NVIDIA Xid 13/109 on
  Linux).
- **Mesh stages** do not get the heap offset yet.
- **16 GB of RAM:** at the end of the load the process holds ~14.6 GB (RAM plus swap) on the Linux machine after the
  cuts above. zram made this worse (compressed pages still occupy RAM); a swap file on disk is the better choice.
- **Initialisation takes ~180 s under Proton** against ~70 s natively. Not investigated.
- **Under Wine** a thread that takes an access violation while holding the process heap lock can still deadlock
  with xmem's fault handler; the test for it is skipped there.

## AMD on Linux: what got the world drawn

On 2026-11-26 a Linux machine with an RX 7900 XTX (and a Ryzen 9 7950X3D's RDNA2 iGPU), Mesa 26.3.1 (RADV) and Wine
drew the first world frames: the opening night street and a sunset street with palms, cars and a character, over
eight minutes in the world without a GPU hang, at about one frame per six seconds. The runtime was `main` `a73a57e13`
with #1914, #1957, #1972, #2044, #2097, #2124, #2129, #2144, #2162, #2195 and #2240 merged; they went into `main`
together with this section. Each step below removed a stop that ended every earlier run; they are in the order
they were found.

1. **Raise amdgpu's GPU timeout.** `modinfo amdgpu -p` shows `lockup_timeout` with a default of 2000 ms: the kernel
   resets any ring whose job runs over 2 s. World frames take seconds, so a run ended about 30 s into the world with
   `ring gfx_0.0.0 timeout` in `journalctl -k`, no page fault, and a black last frame. Raise it as root and reboot:
   `amdgpu.lockup_timeout=30000,60000,10000,10000` on the kernel command line (graphics, compute, SDMA, video, in
   ms), or `options amdgpu lockup_timeout=30000,60000,10000,10000` in a file under `/etc/modprobe.d/` followed by an
   initramfs rebuild. A real hang then freezes that ring, and a desktop on the same GPU, for up to that long, so don't
   make it unlimited. Windows reporters raise `TdrDelay` for the same reason.
2. **Native wave64 shaders.** On a GPU that runs wave64 natively (AMD RDNA), translate with
   `make_game.py --stages shaders --wave64-emulate off`. Before `max_subgroup_decider` (`make_game.py`), the auto
   mode took the smallest `maxSubgroupSize` over every Vulkan device, so an installed llvmpipe (8) turned emulation on for all
   23,841 wave64 shaders. Emulated single-wave compute programs were then translated in the round-loop form, which
   hung the GPU on several library programs and drove RADV's compiler past 12 GiB (53 GB uncapped) on the largest; the
   translator no longer does that for single-wave programs ([design.md](design.md), row "Wave64 emulation:
   single-wave barrier fallback").
   The auto mode now leaves CPU devices out when a GPU is listed and names the device that decided.
3. **Fill the pipeline journal with the runtime itself.** `gta6rt --precompile-pipelines --threads 16`, under Wine,
   with the environment of the game run (`LEONIDA_SHADERS`, `LEONIDA_PIPELINE_CACHE_DIR`, `LEONIDA_DEVICE_FAULT`;
   `--precompile-pipelines` in [d3d12_vulkan.md](d3d12_vulkan.md)). It built 3,693 compute pipelines; the next boot took
   1,900 of them from the journal in 0.1 s of driver time, and the menus ran at 30 frames/s instead of under one.
   That run left out the programs `LEONIDA_SKIP_COMPUTE` named, which kept them out of the journal; the pre-compile
   now builds them too unless it is given `--skip-listed`.
   The `pipeline_cache.bin` that `make_game.py`'s pipelines stage writes with `vk_pipeline_check` gave no hits: 0 of
   5,526 journal pipeline keys were in its 29,861 objects (9 shader binaries were identical, so the machine code
   matched and the keys did not). Each of three differences is enough for a miss on RADV:
   - **The set of shaders:** the runtime's warm-up builds the library's compute programs (`library/index.json`,
     2,683 modules); the stage builds the main index and `bvh/`, and only 25 library modules are byte-identical to a
     main-index one.
   - **Robustness:** on RADV the runtime enables `robustBufferAccess`, `robustBufferAccess2` and `nullDescriptor`;
     `vk_pipeline_check` turns `robustBufferAccess` off and leaves robustness2 off. RADV puts the robustness state into
     every shader stage key, so every pipeline, compute and graphics, misses.
   - **Layout and subgroup request:** for compute with set-0 bindings (2,244 of the 2,683) the checker builds set 0
     from default bindless features and requests the subgroup size without the runtime's full-subgroups flag; RADV
     hashes the whole set layout and the stage flags. Graphics would miss anyway: the checker pairs each stage with a
     stub and fixed state, and RADV hashes the full graphics state.
4. **Leave out the ray-tracing programs.** 724 library compute programs have 2 MiB of SPIR-V or more (676 of them
   ray-tracing resolve). The 89-167 s each in the driver were measured with wave64-emulated packs; with the native
   pack of step 2 they build in seconds, so compile time is not the reason any more. Measured afterwards on the same
   machine (RX 7900 XTX, RADV, Mesa 26.3.1, Wine): all 721 such programs of that pack built, 360 of them in 170 s on 32
   threads, 2.5-32 s each, the largest (13 MiB) in 28.5 s alone, at a peak of 14 GiB of RAM. Two world runs with all
   of them in the journal and no skip list (one run per build) took them all from the journal and got no further
   than runs with the skip list: one stopped submitting after 404 s with the GPU idle, the other lost the device
   about 214 s in with every queue idle, a loss that also happens with the skip list set
   ([running.md](running.md#settings-every-platform-needs)). Whether their output is correct is not measured, so the
   longest runs still leave them out. `LEONIDA_RT_AS=off` drops acceleration-structure builds and copies, and
   `LEONIDA_SKIP_COMPUTE=<name>,<name>,...` with those programs' `lib_cs_NNNNN` names (picked by module size from
   `library/index.json`) drops their dispatches at play. The picture then lacks whatever those passes draw.
5. **Keep the title's hang watchdog kicked.** The title exits on purpose (a write to address 0) when its main loop has
   not reported for 30 s, which a world load under the runtime exceeds. `LEONIDA_TITLE_WATCHDOG=0` defuses it only
   together with `LEONIDA_TITLE_WATCHDOG_KICK=<trap RVA>,<kick RVA>` (hex; `runtime/src/main.cpp`); the two values are
   measured locally and never committed. With `=0` alone the loader logs that the watchdog stays armed.

The rest of that run's environment came from a Linux/Proton report on an RTX 3090: `LEONIDA_ASYNC_PIPELINES=0`,
`LEONIDA_DEVICE_TYPE=6`, `LEONIDA_MAP_READBACK=1`, `LEONIDA_TITLE_WINDOWS_MB=4096`, `LEONIDA_FRAME_PACING=1` and
`LEONIDA_PRESENT_NO_TEAR=1`. Still wrong after it: frames alternate between white-out, a correct picture and black;
some geometry is drawn in the wrong place; VRAM reached 18.3 of 22.3 GB; the run ended on a breakpoint in the title
after 12 minutes.

## Reporting a result

Say the machine (OS, GPU and driver, RAM), the commit, the environment switches that differ from the defaults, and
counts: presents, the last stage reached with its time, the first refusal or device-loss line. Keep logs and traces
on the machine that produced them.
