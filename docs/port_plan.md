# Port plan: what to take from VIctory into Leonida, in order

Leonida's runtime is the base; VIctory (`victory/`) is the reference that has
run the title. This plan lists what the title needs that Leonida's runtime does
not do yet, in the order the title hits it, and where each fix lands.
Path roots: `L\` = `runtime\`, `V\` = `victory\crates\victory-gpu\src\`,
`VH\` = `victory\crates\victory-hle\src\modules\`.

## Measured so far (`gta6rt` on the 1.0.2.1 package)

- `--dry-run`, `--init`: all five modules map and initialise (TLS callbacks,
  DLL entry points). 11 imports were unimplemented:
  `XCRDAPI.dll!DStorageGetFactory`, `xapu.dll!XApu{Connect,DequeueResult,
  Disconnect,EnqueueCommand,GetResultCount}`, `chad.dll!XDsp{Activate,
  ConnectWithMaximumStreamLimit,Deactivate,Disconnect,SubmitCommand}`. With
  the audio port (`L\shims\xapu.cpp`, `L\shims\chad_dsp.cpp`, the coprocessor
  and XMA decoder in `L\shims\acphal.cpp`) the dry run leaves only
  `DStorageGetFactory`.
- `--run`: the GDK runtime initialises offline, then the title calls
  `DStorageGetFactory` and stops (about 3 s in).

## Step 1: system calls the title needs before its first frame

| What | VIctory source | Lands in |
|---|---|---|
| DirectStorage (factory, queues, requests, status arrays, zlib, BCPack) | `VH\dstorage.rs` | new `L\shims\dstorage_xs.cpp`; inflate from `L\src\shader_library.h` |
| BCPack decode (tables supplied locally, SHA-256 checked, never committed) | `victory\crates\victory-formats\src\bcpack*` | new `L\src\bcpack.h/.cpp` |
| APU (`xapu.dll`) and DSP (`chad.dll`) | `VH\apu.rs`, `VH\dsp.rs`, `VH\audio\` | done: `L\shims\xapu.cpp` (Opus decoded as silence: no decoder in the repo), `L\shims\chad_dsp.cpp`, coprocessor and XMA in `L\shims\acphal.cpp`, `L\src\xma_decode.h` |

## Step 2: the console D3D12 ABI in front of the Vulkan layer

The title creates its device as `ID3D12Device11`, which selects the newer
console layouts: list slots from 59 and queue slots from 14 are two slots later
(`V\device.rs:149-168`, `V\list.rs:2134`, `V\queue.rs:985`). The proxies now
take every slot's meaning from the slot tables in `L\shims\d3d12_xs_slots.h`
(device, queue, list, fence; each entry cites its source), with that switch: a
device created as `ID3D12Device11` gets the console layouts, any other keeps the
PC order Leonida's own PC-interface tests use. `L\test\xs_proxy_mock_test.cpp`
checks every queue and list slot of the tables through the proxies.

Boot, in order:

| # | Title calls (VIctory reference) | Leonida now |
|---|---|---|
| 1 | Queue Signal 16, Wait 17, GetTimestampFrequency 18 (`queue.rs:960-987`) | PC 14/15/16; GetDesc 28 → PC 18, the return buffer first or second |
| 2 | Fence slot 12: address of the 64-bit value the title polls (`fence.rs:21-27,136-139`) | a word the proxy keeps at the fence's completed value (CPU signals at once, queue signals through one completion event per signal) |
| 3 | Device 69 (debug filter, no-op), 127 CreateCommittedResource3 for back buffers (`device.rs:442,457`; `resource.rs:2162-2186`) | 69 S_OK; 127 → PC CreateCommittedResource3 (PC description, ALLOW_DISPLAY cleared), else CreateCommittedResource |
| 4 | Queue 35 ExecuteX, 34 Signal2X, 32 PresentX (`queue.rs:597-610,757-841`); the title's window (`host.rs` `title_window`, `window`; `victory-hle` `lifecycle.rs` `CreateWindowExA`) | 35 → PC 10, 34 → PC 14; 32 → `L\shims\xbox_window.cpp` on the Vulkan device: the title's own window (its `CreateWindowExA` fitted to the screen, `L\shims\title_window.cpp`) or one of the runtime's, a swapchain on the presenting queue, `XboxPresentation` composing the planes; caption with fps; `LEONIDA_CAPTURE_DIR` screenshots (`capture.rs:170-185`). Tested by `run_tiny_present` |
| 5 | Device 78-81 frame clock (`frame.rs:1-130`) | VIctory's clock per device (S_FALSE time-outs, token on time-out, flips on consecutive ticks); `XboxPresentation` takes its tokens and does not pace (`use_device_frame_clock`) |
| 5a | Display mode (`dxgi.rs:222-279`, output slot 21; upstream `apps/victory/src/native_1440p.rs`) | `L\shims\xbox_display.cpp`: the console adapter and output report `LEONIDA_DISPLAY_MODE` (default 3840x2160), which the title reads but does not size by; `L\src\display_profile.h` patches the executable's SDR mode, scanout reservation, scene scale and Settings viewport for `2560x1440` (and `LEONIDA_RENDER_SCALE`): 2560x1440 window and back buffers, measured (`docs/runtime_imports.md`). VIctory's FSR 4K presets (`fsr_preset.rs`, `fidelityfx.rs`) are not ported |
| 6 | Lists from device slot 12 get the console layout (`list.rs:566`) | console list proxies (also as the console's List5/List6 IIDs); device 25/26/108 take the return buffer first; device 48 takes the console queue description |
| 7 | List 139 ResetX, 80 DispatchX, 71/72 Write32/64, 73/74 WaitValue, 62 CopyMemory, 86 CopyResourceX, 148 AllocateUploadMemory (`list.rs:2059-2149`) | 139 → PC Reset, 80 → Dispatch, 86 → CopyResource; on title memory (`L\src\title_memory.h`): 62 → copies between the imports, 71/72 → WriteBufferImmediate (MARKER_OUT; flag 1 a timestamp query the GPU writes when it gets there, flag 2 the performance counter once the submission has run), 100 → DispatchMesh(x, 1, 1) (indirect dropped, as V does), 148 → the device's imported upload blocks, given back at reset; 73/74 cut the list as V's `cut` does (`L\src\vulkan_backend.h`, `execute_parts`): the queue runs the parts in order and holds each at its wait until the value is in memory (a 250 µs poller while one is held; PresentX forces them; `LEONIDA_VALUE_HOLD_MS`, default 1000, bounds a hold) |
| 8 | Queries at slots 28-30 (`list.rs:2114-2117,1330-1345`) | → PC 52-54; 59 is the console marker (ignored); 27, 31, 56-58 and 60 are named by no source and refused |
| 9 | ~60,000 PSOs in the first 20 s; device 86 CreatePipelineState with streams (`pipeline.rs:6-11`) | creation checks the description and returns; the pipeline is made on worker threads (`L\src\pipeline_queue.h`, `make.rs`), started by the first bind or, for one an earlier run bound, at creation (`early-pipelines.txt`); a bind waits `LEONIDA_PIPELINE_WAIT_MS` (500) per frame and kind; draws and dispatches without a made pipeline are dropped and counted; 86 → PC CreatePipelineState with the console's AS/MS numbers (23/24) made the PC's |

Menus and movies: sampler stride 16, views 32 (`descriptor.rs:1-7`; done:
`L\shims\d3d12_com.h`, `descriptor_increment`); device 61 CreateSamplerX (done:
the S# from the console description's hardware fields); device 70/71 views over
title memory (`descriptor.rs:795-807`, `resource.rs:2383-2450`; done, every mip
and slice detiled with the console's layout, `L\src\gfx10_tiling.h`
`ConsoleTexture`, from `victory-formats` `texture.rs`); textures the title
writes after the first view (open: a view keeps its first copy); vertex,
index, constant and root views at title addresses outside any resource
(`shadow.rs:230-247`); rectangle lists via a geometry shader (`primitive.rs`;
done, `L\src\rect_list_gs.h`).

World: page pools (device 74/75, queue 31 CopyPageMappingsBatchX,
`resource.rs:2190-2230`, `queue.rs:897-948`; done: `rt::xmem::map_alias`,
the imports follow the remap); mesh draws (list 129/100,
indirect mesh dispatch, depth bounds); ExecuteIndirect with count buffers and
argument clamps (`indirect.rs`); GPU memory placement between 4 GiB and 2^40
(`VH\xmem.rs:73-87`).

## Step 3: port order inside the layer

1. Console ABI slot tables as data, with the Device11 layout switch (device,
   queue, list, fence). Test with `L\test\xs_proxy_mock_test.cpp`.
2. Synchronization: fence value words, Write32/64 labels, WaitValue holds
   (`L\src\queue_holds.h`), ExecuteX/Signal2X, timestamps, release at PresentX.
3. Pipelines built at first bind on worker threads with a per-frame wait
   budget; draws without a ready pipeline dropped; a list of pipelines earlier
   runs used, started early. Done on `port/pipelines` (`L\src\pipeline_queue.h`,
   docs/d3d12_vulkan.md "Pipelines made off the creating thread"); not yet
   seen on the title, which stops before its first frame. Open: VIctory's
   loading allowance (`make.rs:193-223`), and the attachment-format variants
   replay still compiles where it records (`VK_EXT_dynamic_rendering_unused_attachments`
   would remove them).
4. Title memory: a GPU address is the title's CPU address. Import xmem graphics
   memory with `VK_EXT_external_memory_host` (Leonida's `create_host_buffer`,
   `d3d12_vulkan_device.cpp:1974-2075`) as the default; keep copies only for
   memory that cannot be imported. Port xmem's address placement. Done on
   capture-replay devices (`L\src\title_memory.h` has the measurements: heap
   base 512 GiB, imports at heap base + address, sparse buffers holding the
   windows' device addresses, imports merged per 2 MiB block); no copy
   fallback yet: memory that cannot be imported (a GPU without
   `bufferDeviceAddressCaptureReplay`, the AMD iGPU here) is logged, and
   shaders do not reach it.
5. Descriptors (sampler stride 16, CreateSamplerX, views over title memory,
   slot lifetimes), then textures (upload queue, partial residency, detiling via
   `L\src\resource_detile.h`), then indirect (clamps, mesh indirect, counts),
   rectangle lists and frame pacing.

## Shaders: rules to take from VIctory into `tools/rdna2spirv.py`

`VS\` = `victory\crates\victory-shader\src\`. Some VIctory fixes exist only in
its history (commit `570a941e9` replaced the whole shader crate): `060195536`,
`2286e038c`, `77053a0c5`.

In order, each with a `tests/semantic` fixture (the suite needs no LLVM, SPIR-V
tools or GPU):

1. **Loop iteration cap** (stops GPU hangs): 128 pixel, 256 vertex/mesh, 4096
   compute, then discard/return (`VS\emit\lower.rs:328-335,11124-11173`,
   `15b0091e8`). Loops over unmirrored data that reads as zeros ran ~4 billion
   times and lost the card. Leonida has no cap; its loops follow the whole wave,
   so one counter per wave, and an early-exiting wave must not stall barrier
   rounds.
2. **Rectangle lists** (topology 19) through a generated geometry shader
   (`VS\emit\rect.rs:1-27`, `683ad7a99`). Full-screen passes and tiled
   lighting use it. Done in the runtime, not the translator:
   `L\src\rect_list_gs.h` makes the stage from the VS's SPIR-V outputs at
   pipeline creation (cached per output signature) and the pipeline takes
   triangle lists; tested by `tests/test_rect_list_gs.py` and
   `d3d12_rect_list_test` (RTX 4060 Ti).
3. **Literal sampler compare**: the console writes `DEPTH_COMPARE_FUNC =
   LESS_EQUAL` into every static S#, so whether a sample compares is decided by
   the instruction (`VS\emit\sampler.rs:13-15`, `2286e038c`). Leonida enables
   compare from the S# field (`runtime\src\gfx10_descriptors.h:534,609`), so a
   plain `image_sample` gets a compare sampler: undefined behaviour. Key literal
   samplers by (words, compare).
4. **Quad ops and helper lanes**: quad-confined DPP and `ds_swizzle` quad mode
   as quad operations in pixel and mesh shaders (`lower.rs:4335-4370,
   4464-4495, 6671-6760`, `921d8d3b4`); a helper lane's `v_readfirstlane`
   returns its own value (`060195536`: waterfalls with only helpers left spun
   until DEVICE_HUNG ~200 s into the world); a discarded lane stays a helper
   (`77053a0c5`).
5. **VS → PS linking by signature row**, not parameter number
   (`VS\native.rs:37-48,203-241`, `lower.rs:1090-1251`); run a census of the
   corpus first.
6. **Per-program wave64 emulation** for speed on 32-lane GPUs: run Leonida's
   partition-invariance tiers (`tools\wave64_halves.py`) on compute and mesh
   too, emulate only tier C, add VIctory's exact idioms (`lower.rs:483-533,
   996-1017`, `VS\emit\own_bits.rs`). Done on `port/wave64-tiers`: design.md
   rows "Wave64 compute and mesh: partition invariance" and "Wave64 halves:
   further exact forms". Not ported yet: the own-bits light loop (a wave
   OR / min / max steering a loop over the lanes' own bits); it needs the
   reduction's cross-half `v_readlane` rewritten to the half's own value and
   VIctory's proof that a pass outside a lane's own bits leaves nothing.
7. Compute implicit derivatives (both use LOD 0 today), cube `_d` LOD pin,
   waterfall collapse (measure first).

Leonida is ahead on BVH node numerics, f16 rounding and named-level gathers.

Runtime shader lookup (`runtime\src\shader_library.h`): key entries on the
parts the translator reads (ISG1/OSG1/PSG1/PSV0) with an exact match first;
write the translator commit and flag profile into `index.json` and refuse a
mismatch; on a miss, log the hashes, dump the container and let `make_game.py`
translate it.

## From VIctory `ab34a8b21` (merged 2026-11-25): candidates to port

- `apps/victory/src/native_1440p.rs`, `fsr_preset.rs`: the title told a 1440p
  display with FSR at Quality: the display-mode lever (`port/present`).
- `crates/victory-hle/src/modules/fidelityfx.rs` (+ `crates/victory-hle/tests/fidelityfx.rs`): the
  title's `amd_fidelityfx_upscaler_xbox.dll` served on the host; Leonida's
  shader stage translates only 18 of the 29 FidelityFX programs.
- `crates/victory-gpu/src/texture/ahead.rs`, `texture/upload.rs`, `tiles.rs`,
  `resource/small.rs`: texture streaming ahead of use, tiled resources, small
  resources; candidates for Leonida's texture step.
- `crates/victory-gpu/src/pipeline/ray_tracing.rs`: ray-tracing pipelines.

## From VIctory `64bd58f5f`: NVIDIA settings

VIctory's recommended NVIDIA run: `--no-rt` (no ray-tracing builder, traversal or
lighting resolve), `VICTORY_AMBIENT_FALLBACK=1` (supported material shaders get a
lightweight diffuse ambient fill from their base colour, so unlit areas are not
black) and `VICTORY_FSR_4K=0`; `--rt-static-bvh` traces only the prebuilt world
structure. The ambient fill is a cheap stand-in for the missing ray-traced light
worth porting to Leonida until ray tracing runs (see
`victory/crates/victory-gpu/src/pipeline/ray_tracing.rs`).

## Frame time: where Leonida can beat VIctory

VIctory's world frames cost 240-970 ms, mostly host copies of title memory
(2,655 MB compared and 95 ms of waiting per frame in City), every submission
waited for, and 391 MB/frame of texture staging while streaming.

- Import title GPU memory once (`VK_EXT_external_memory_host`): no refresh
  copies, write watch, snapshots, compares or copy-back. Commit pages before
  import, never import write-combined pages, free an import only after the GPU
  retired it, keep imports few and large (Windows drivers cap allocations at
  4,096).
- Buffer device addresses: a per-64 KiB page directory of device addresses (or
  one sparse 2^40 mirror buffer if imported memory can be sparse-bound) instead
  of `heap_base + va`; removes VIctory's 1,023-slot directory limit. Measured
  on the RTX 4060 Ti (616.64): imported memory cannot be sparse-bound (a sparse
  buffer takes memory type 1 only, host pointers import as types 2 and 3), and
  a 1 TiB sparse buffer is refused; but capture-replay device addresses keep
  `heap_base + va` as it is (`L\src\title_memory.h`).
- Resizable BAR (on, on the test machine): hot read-mostly buffers written
  straight into VRAM.
- Asynchronous submission on timeline semaphores once copy-back is gone; real
  occlusion queries; GPU detiling or `VK_EXT_host_image_copy` for textures.
