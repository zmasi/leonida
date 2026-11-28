# Xbox D3D12 (`d3d12_xs`) to Vulkan

**Status: in progress. A Vulkan-backed `ID3D12Device` (queues, allocators, command lists, fences, descriptor heaps,
render-target and depth views, texture views and samplers, committed and reserved buffers, 2D textures and 3D texture transfers and color clears, and query heaps)
runs behind the Xbox-ABI device proxy, and a presenter puts back buffers in a window (section 3.2). The M0
milestone passes in a synthetic test (section 6), M1 with stand-in shaders in `d3d12_draw_test`, M1.5 with
hand-written RDNA2 run through the translator in `d3d12_translated_draw_test`, and the synthetic part of M2
(descriptor heaps, SRVs, a sampler and a translated textured-quad draw, 4,096 exact pixels) in
`d3d12_m2_textured_test`. `D3D12XboxCreateDevice` uses it when `LEONIDA_D3D12=vulkan` is
set and the host PC device otherwise. Root signatures (packed into the 32 user-data dwords), graphics and compute
PSOs, draws, dispatches, `ExecuteIndirect`, and presentation through `PresentX` or a DXGI swapchain are implemented
and have GPU tests; which present path the game uses is still open (question 6). Shader-resource views of buffers,
pipeline-state streams (`CreatePipelineState`), tessellation and stream-output pipelines are not implemented, and
mesh shaders are reported as not supported (stream PSOs and mesh shaders: D3D-29, #1712). The device reports
`RaytracingTier` as `NOT_SUPPORTED`, yet `BuildRaytracingAccelerationStructure` builds XDXR BVH4 structures on the
GPU (section 4.8).** This is the plan for
replacing the console's `d3d12_xs.dll` with an implementation that records D3D12 commands and replays them on
Vulkan, while producing exactly the shader-visible contract that `tools/rdna2spirv.py` already emits against (see
[design.md](design.md#translator-status-and-runtime-contract)).

The document is deliberately conservative about what is known. Every claim is marked:

- **Verified** — observed in this repository or in the game package, with a way to reproduce it.
- **Public** — documented in the Windows SDK, Microsoft's public GDK/D3D12 documentation, or the Vulkan
  specification.
- **Assumption** — a design choice that must be checked against the game's executable when it is available.
- **Open** — unknown; listed with a way to resolve it.

Nothing here uses game data, keys or decryption. The package is read-only; the executable that would answer most
of the open questions is not in it.

## 1. The job

The game is an Xbox Series build (`game_scarlett_master.exe`, `d3d12_xs.h` / `d3d12_xs.lib`). It was compiled
against the Xbox flavour of D3D12 and calls the console driver through COM interfaces. The layer must:

1. **Intercept the D3D12 surface.** Free functions exported by `d3d12_xs.dll` (device creation, root-signature
   serialization) and every COM method the game calls on the returned objects. The objects are our own C++
   objects; the vtables must have the exact method order the game was compiled against.
2. **Preserve D3D12 semantics that affect output.** Command lists record; execution is deferred; descriptors are
   values (GPU virtual addresses or CPU handles); resource states are explicit; root arguments are set at record
   time; allocators and fences control reuse.
3. **Produce the translator's shader contract.** The translated SPIR-V is already written against a bindless set
   0, a 144-byte push-constant block, a descriptor hash table and one GPU address space. Changing that contract
   would invalidate 26,503 translated shaders and the GPU tests that check them.

The layer is not a general D3D12-on-Vulkan compatibility layer. It only needs the calls this game makes, but each
call it accepts must behave like D3D12.

## 2. What is verifiable today

| Fact | Evidence | Confidence |
|---|---|---|
| The public D3D12 interface chain and method order are in the Windows SDK `d3d12.h` (10.0.26100.0 here). The Xbox public reference lists the same base interfaces and method names under `d3d12_xs.h` / `d3d12_xs.lib`; the measured Xbox device ABI is mostly that order, with deviations at slots 62/76 and heap slots 9/10 (section 7). | Local SDK header; [Xbox public graphics reference](https://devdocs.xbox.com/reference/graphics/d3d12); e.g. [CreateCommittedResource](https://devdocs.xbox.com/reference/graphics/d3d12/interfaces/id3d12device/methods/id3d12device_createcommittedresource_public) | Public; measured deviations in section 7 |
| The game's raytracing helper is a shipped DLL, `XboxBvhBuilder.dll`, whose export signatures name `ID3D12Device11`, `ID3D12GraphicsCommandList4`, `ID3D12GraphicsCommandList6` and `ID3D12GraphicsCommandList8`, plus Xbox XDXR batched/streaming acceleration-structure types. | PE export table of `XboxBvhBuilder.dll` (884,224 bytes); see Appendix A | Verified |
| That DLL imports only `d3d12_xs.dll!D3D12SerializeVersionedRootSignature` and `xmem.dll!XMemVirtualQuery` from console libraries. It receives the device and command lists from the game. | PE import table; Appendix A | Verified |
| `amd_fidelityfx_upscaler_xbox.dll` imports only `d3d12_xs.dll!D3D12CreateRootSignatureDeserializer` and contains the base `ID3D12Device`, `ID3D12Resource`, `ID3D12PipelineState`, `ID3D12RootSignature`, `ID3D12Heap`, `ID3D12DescriptorHeap` and `ID3D12CommandSignature` IIDs. | PE import table and GUID byte scan; Appendix A | Verified |
| None of the other scanned non-CRT DLLs (`amd_fidelityfx_loader_xbox`, `Bink2Scarlett`, `libHttpClient.GDK`, `XCurl`) imports `d3d12_xs`. | PE import scan; Appendix A | Verified |
| The package layout lists `game_scarlett_master.exe`, but the file is not in the package folder. | `layout_..._xs.xml`, chunk 1001; directory listing | Verified |
| `gameos.xvd` is an XVD (`msft-xvd` at file offset 0x200, format version 3) with a high-entropy payload and no plaintext PE headers or D3D12 strings. Extracting `d3d12_xs.dll` from it would require keys, which this project does not do. | Entropy and string scan; Appendix A | Verified |
| Prior art runs the package DLLs against a PC D3D12 device and measured the Xbox ABI: 6 Xbox-specific static imports across the 16 DLLs, `D3D12XboxCreateDevice`, device slots 62/76, descriptor-heap start handles in RAX, root-signature flags `0x03000000`, and the BVH builder's Xbox DXIL shaders. | [PLATFORM_SURFACE.md](https://github.com/realdody/americas-recomp/blob/xbox-series/docs/xbox/PLATFORM_SURFACE.md), [platform README](https://github.com/realdody/americas-recomp/blob/xbox-series/tools/xbox/platform/README.md) | Verified by the prior art (running the package DLLs / static imports; section 7) |
| The translator and its GPU tests already define the shader contract: set 0 bindings 0–19, 144-byte push constants, heap/code base, FNV-1a descriptor hash table, buffer device addresses. | [design.md](design.md#translator-status-and-runtime-contract); `runtime/test/vk_texture_test.cpp` | Verified |

What this does **not** tell us: the full `d3d12_xs.dll` export list, the `D3D12XboxCreateDevice` params/flag
layout, the remaining Xbox slot deviations (presentation, command lists, queues, fences), the
root-signature-to-`s0..s31` mapping, the console descriptor layout, the present path, the raytracing path, and the
GPU VA window. Section 8.2 lists each with a way to resolve it.

The first run with the executable should be a **trace build**: every free function and COM method is a logging
stub that returns a failure, and the methods are filled in in the order the game calls them. Section 3.1 describes
the interception; section 8.2 turns the trace into a work list.

## 3. Architecture

### 3.1 Interception and COM

The current shim (`runtime/shims/d3d12_xs.cpp`) forwards only two free functions to the host `d3d12.dll`. That is
enough for the bundled middleware's `d3d12_xs` calls, not for the game. The layer adds:

- **Free functions.** Every `d3d12_xs.dll` export the executable imports. The import table is the source of truth.
  Prior art names the device entry point `D3D12XboxCreateDevice(adapter, params, riid, out)`; its 24-word params
  struct is only logged there, and the signature is taken from public documentation rather than a header. Treat the
  params/flag layout as **Open**, accept the call, log the words, and create the device. Do not assume
  `D3D12CreateDevice` is the console entry point.
- **COM objects.** `Device`, `CommandQueue`, `CommandAllocator`, `GraphicsCommandList`, `RootSignature`,
  `PipelineState`, `DescriptorHeap`, `Resource`, `Heap`, `Fence`, `QueryHeap`, `CommandSignature`,
  `PipelineLibrary`, and the raytracing/lifetime objects if used.
- **Vtable layout.** Use the Windows SDK's `d3d12.h` declarations for the public chain, but treat the vtable as
  **mostly public, not universally public**. Prior art measured device slots 1, 15, 18, 19, 27 and 38 in the
  desktop order, but also an extra texture-creation method at slot 76, a `CreateComputePipelineState` call at
  slot 62 where the PC interface it forwarded to presents `CreateStateObject` (different arity), and descriptor-heap
  start handles returned in RAX at heap slots 9/10. Implement the public chain plus those measured overrides
  explicitly; do not assume every slot. On the retail executable's higher device slots (`runtime/shims/d3d12_xs_device.cpp`),
  slot 86 routes `CreatePipelineState(desc, riid, out)` to PC slot 47 (`ID3D12Device2::CreatePipelineState`) and keeps
  a deep copy of the stream on the PSO it returns; slot 55 (`CreateDerivedPipelineState(pBasePSO, n, pRecords, riid,
  out)`, `n` = 1..4 records of 0x14c bytes: a u32 tag, then the payload at +4) patches a copy of that stream and creates
  the PSO through the same path. Tags 0 (`D3D12_DEPTH_STENCIL_DESC`, on DEPTH_STENCIL or DEPTH_STENCIL1), 1
  (`D3D12_RASTERIZER_DESC`), 2 (`D3D12_BLEND_DESC`) and 3 (sample mask) are the TomBox layout (GPL-3.0,
  `create_derived_pso_x`), checked against the title's record writes in the retail build: tag 1 is the
  rasterizer (0x2c bytes, always record 0 when present), tag 0 the DEPTH_STENCIL (0x34), tag 2 the BLEND (0x148). Tag 3 (sample mask, from TomBox)
  is served although that function does not write it. Tag 5 (0x16 bytes: 16 zero bytes, then the StencilReadMask and
  StencilWriteMask bytes at the DEPTH_STENCIL offsets 0x10/0x11, then zeros) overlays only those two bytes of the base's
  DEPTH_STENCIL or DEPTH_STENCIL1, in record order; this is inferred from the title's writes, not measured at run time.
  Any other tag, or a base that
  was not created through slot 86/55, is refused (`E_INVALIDARG`/`E_FAIL`, null output). The stream
  copy lives in `runtime/shims/d3d12_xs_pso_stream.cpp` and goes with the PSO (private data, released with it),
  `D3D12_PIPELINE_STATE_SUBOBJECT_TYPE` values that the module does not walk (VIEW_INSTANCING) are not kept,
  so a PSO built from them is refused as a base,
  slot 101 routes `CreateStateObject(desc, riid, out)` to PC slot 62 (`ID3D12Device5::CreateStateObject`), and
  slot 108 routes `GetResourceAllocationInfo2` (accepting the console C-ABI struct return with the return buffer in
  `RCX` and the device in `RDX`, expanding 56-byte `D3D12_RESOURCE_DESC` entries to `D3D12_RESOURCE_DESC1` and
  normalizing Xbox layout words and flag bits on the copy) to PC
  slot 68 (`ID3D12Device8::GetResourceAllocationInfo2`). Device 11 adds `CreateSampler2`; command list 8 adds
  `OMSetFrontAndBackStencilRef` (public SDK).
- **Unknown IIDs and slots.** `QueryInterface` must log the IID and return `E_NOINTERFACE` if it is not in the
  public chain. If the game then fails, the IID and the call site identify the Xbox-specific interface to reverse.
  Do not invent a vtable for an unknown IID: a wrong slot is worse than a clean failure.
- **No host D3D12 or DXGI.** The PE loader currently lets unknown system DLLs bind to the host. `dxgi.dll` and
  `d3d12.dll` must be shimmed, not resolved from System32. Host DXGI would hand the game a desktop adapter and
  swapchain that cannot see our Vulkan device, and host D3D12 would create a second, unrelated device. This is an
  integration requirement, not an optimisation.
- **Trace first.** `gta6rt --trace-d3d12` emits numbered records for device entry points, Vulkan device operations,
  command-list recording and queue submission. Records contain fixed API names and outcomes only. See
  [the runtime guide](../runtime/README.md#first-boot-d3d12-trace) for collecting a trace.

**Measured, not assumed:** the Xbox device ABI is not uniformly the desktop order. Slots 1/15/18/19/27/38 matched
in the prior art, while slots 62 and 76 and the heap start-handle return convention did not. Treat every slot as
unverified until traced; the public interface names and IIDs may still be usable, but the slot mapping is not
guaranteed.

### 3.2 Record and replay

D3D12 command lists are recorded, then executed later. Vulkan command buffers are also recorded, but the layer
must not simply translate each D3D12 call into a Vulkan call as it arrives, because:

- descriptor heaps can be updated between recording and execution;
- a command list can be reset and rerecorded;
- a bundle is recorded once and executed many times.

The design is therefore an **IR per command list**:

- `SetPipelineState`, root arguments, vertex/index buffers, viewport/scissor, render targets, descriptor-heap
  bindings and the other sticky state update a state block.
- Each draw/dispatch/copy/barrier appends a compact command with a snapshot of the state that affects it.
  GPU descriptor tables can refer to shader-visible heap entries at execution. CPU descriptors such as RTVs
  are consumed while recording, so their view contents go into the IR. An application can reuse the CPU
  descriptor slot after the call. See Microsoft's [descriptor overview][descriptor-overview].
- `ExecuteCommandLists` replays the IR into a fresh `VkCommandBuffer`, resolving descriptors, uploading dirty
  descriptor ranges and the hash table, and emitting barriers. Bundles are inlined when they are executed (see `ExecuteBundle` below).
- `Reset` on a command list discards its IR; the allocator owns the memory. D3D12 already requires the application
  not to reset an allocator whose lists are in flight; the layer can rely on that and assert in debug builds.
- Command-list and allocator misuse returns what the PC runtime returns (D3D-51). `d3d12_vulkan_device_test` runs
  the same 25 calls on the host runtime and on this layer and compares the HRESULTs. One list at a time records into
  an allocator: creating or resetting a second list onto it gives `E_INVALIDARG`, and resetting the allocator while
  a list records into it gives `E_FAIL`. `Close` twice and `Reset` of an open list give `E_FAIL`. Resetting an
  allocator whose lists are still executing gives `S_OK` on the PC runtime too (keeping that safe is the
  application's job). Two differences remain. `Reset` with a null allocator crashes the PC runtime, while ours
  returns `E_INVALIDARG`, so that call is not compared. Executing a list that is still open removes the PC device
  (`GetDeviceRemovedReason` gives `DXGI_ERROR_INVALID_CALL`), while this layer submits nothing and keeps the device:
  an invalid call does not remove it.
  Creation answers as the host runtime too (`compare_invalid_arguments`, "command allocator:", "command list:" and
  "command list1:", RTX 5070 / 621.14): a type past `VIDEO_ENCODE` and a node other than node 0 are `E_INVALIDARG`,
  checked before the output; a null output is then `E_UNEXPECTED`. Video list types are created as graphics lists
  here, while the host hands out a video list (`E_NOINTERFACE` for `ID3D12GraphicsCommandList`), so they are not
  compared.
- `Close` freezes the IR. `ExecuteBundle` is accepted only on a direct list, with an already-closed bundle as its callee; the bundle's recorded commands and calls are replayed at the call site, so the source can be reused or reset without changing the parent. Command-list creation and reset require an allocator of the same type. A bundle cannot be submitted directly to a command queue.
- Bundle restrictions are checked while recording for the IR operations currently implemented here (barriers, clears, copies, resolves, `SetPredication`, `DiscardResource`, the atomic copies, `RSSetViewports`, `RSSetScissorRects`, `OMSetRenderTargets` and `ClearState`; `WriteBufferImmediate` is allowed in a bundle); those calls are not legal inside a D3D12 bundle and make `Close` return `E_INVALIDARG`. `d3d12_bundle_forbidden_test` measured the last four on the PC runtime without the debug layer: the bundle's `Close` fails (`E_FAIL`, where this layer gives `E_INVALIDARG`) on the default adapter and on WARP, and executing a bundle whose `Close` failed crashes the runtime, so the layer refuses it in `ExecuteBundle` instead. A bundle has no render targets, viewport or root signature of its own, so its state calls (pipeline state, root signature and arguments, vertex and index buffers, topology, blend factor, stencil references) and its draws and dispatches are kept as they were called, and `ExecuteBundle` plays them on the parent through the same members the application would have called. A draw then snapshots the parent's state plus what the bundle set, and what the bundle set stays on the parent afterwards. Commands that need no state (markers, immediate writes) are copied between them in order. `d3d12_bundle_test` measured the rule on the PC runtime, where the default adapter (an RTX 4060 Ti) and WARP drew the same picture in every case it ran. A bundle's draws see the parent's render targets, viewport and scissor, root signature and arguments (also when the bundle sets the same root signature first, so those arguments stay), and vertex and index buffers. A bundle starts with the pipeline state it was created or reset with, not the parent's. After `ExecuteBundle` the parent has that pipeline state and whatever else the bundle set: topology, root arguments, vertex and index buffers, and a different root signature with the bundle's arguments in place of the parent's. Topology: the default adapter takes the parent's when the bundle sets none. That is more lenient than Microsoft's stated rule (topology is undefined when a bundle begins), and WARP follows the rule by removing the device when a bundle draws or hands back without one, so a bundle should set it; the layer follows the default adapter. A bundle that sets a root argument before any root signature crashes the NVIDIA driver (the layer applies it to the parent's signature). The deferred calls are checked when they play, against the parent's root signature, so a bad one makes the parent's `Close` fail, not the bundle's. A per-list state setter must go through the deferred path (`defer()` in `d3d12_com.cpp`, replaying through a private member that does not trace) to work in a bundle. Not deferred yet: `ExecuteIndirect` in a bundle, and bundle `Dispatch` has no GPU test.
- What `Close` returns after one recorded call is compared with the PC runtime over WARP (no debug layer) by
  `d3d12_close_parity_test`, 116 cases on fresh lists, nothing executed. 85 agree (three of them enhanced texture
  barriers, two into `LAYOUT_UNDEFINED`: section 4.3); the other 31 are listed in the
  test as known differences. In 16 the layer fails a list WARP closes: draws and dispatches with no pipeline,
  root arguments before a root signature, a `CopyTextureRegion` box that is empty or inverted, footprint to
  footprint or at an unaligned footprint offset or pitch, `CopyResource` of a resource onto itself,
  `ClearDepthStencilView` with an unknown flag, vertex-buffer slots past 31,
  17 viewports, an `OCCLUSION` query ended as `BINARY_OCCLUSION`, and `WriteBufferImmediate` at an address that
  is not 4-aligned. Since one failed list fails the whole `ExecuteCommandLists`, each of these drops a submission
  D3D12 would run. In 15 the layer closes a list WARP fails (redundant or out-of-range barriers, copies past a
  resource's end or between formats of another family, resolves the PC runtime refuses, a query index past the
  heap); what the layer does with those at execution is not part of this test. A refused list's code is always
  `E_INVALIDARG` here, where the PC runtime also answers `E_FAIL`, so the test compares the verdict and prints both
  codes.
- A queue's `ExecuteCommandLists` submits in order with one timeline semaphore signal. `Wait`/`Signal` become
  timeline-semaphore waits/signals. `Present` queues after the frame's work.

The command-list surface maps to the IR as follows:

| Recorded call | IR / replay |
|---|---|
| `SetPipelineState` | state; bind the `VkPipeline` before the next draw/dispatch |
| `SetGraphicsRootSignature` / `SetComputeRootSignature` | state; select the pipeline layout and root map |
| `Set*Root32BitConstant(s)` / `Set*Root*View` / `Set*RootDescriptorTable` | update root args; build the push-constant block at the draw/dispatch |
| `SetDescriptorHeaps` | state; scan/populate the bindless arrays and hash table at execute |
| `IASetVertexBuffers` / `IASetIndexBuffer` / `IASetPrimitiveTopology` | state; bind arena buffer + offset, dynamic topology. The console's rectangle list (17, PSO topology type 19) and quad list (19) are drawn as triangles, see *Console rectangle and quad lists* below |
| `RSSetViewports` / `RSSetScissorRects` | state; dynamic viewport/scissor |
| `OMSetRenderTargets` / `OMSetBlendFactor` / `OMSetStencilRef` | state; rendering scope and dynamic state |
| `DrawInstanced` / `DrawIndexedInstanced` | command with a state snapshot |
| `Dispatch` / `DispatchMesh` | command with a state snapshot |
| `Copy*` / `Resolve*` / `Clear*` | copy/clear command |
| `ResourceBarrier` / `Barrier` | barrier command; state tracking |
| `DiscardResource` / `WriteBufferImmediate` / `AtomicCopyBufferUINT(64)` | discard, immediate-write and atomic-copy commands (section 4.2) |
| `BeginQuery` / `EndQuery` / `ResolveQueryData` | query commands |
| `ExecuteIndirect` | command; signature + argument buffer |
| `BeginRenderPass` / `EndRenderPass` | rendering scope |
| `ExecuteBundle` | play the bundle's state calls and draws on this list, copy its state-free commands in order |

The IR is the place to add the trace, validation and "unsupported call" reporting. It also lets the layer replay a
frame deterministically from a capture, which is valuable for comparing against the game.

What is implemented (all of it testable without the game):

- **IR** (`runtime/src/command_list.h`): legacy barriers (transition, UAV, aliasing; split barriers record their end
  half), enhanced barriers (global, buffer, texture; the half with `SyncAfter = SPLIT` is dropped and the other is
  replayed as a full barrier), `ClearRenderTargetView` / `ClearDepthStencilView`, whole view or rectangles,
  `CopyBufferRegion` / `CopyTextureRegion` / `CopyResource`, colour resolves,
  `ClearUnorderedAccessViewUint` / `ClearUnorderedAccessViewFloat` (whole view or rectangles), (D3D-26)
  `DiscardResource`, `WriteBufferImmediate` and `AtomicCopyBufferUINT` / `AtomicCopyBufferUINT64`, and `BeginQuery` /
  `EndQuery` / `ResolveQueryData` (section 4.6).
  Views and rectangles are copied while recording, so the CPU descriptor slot can be rewritten afterwards.
  A `ClearDepthStencilView` flag for a plane the view's format doesn't have is ignored, as the PC runtime (WARP)
  does: `STENCIL` on a `D32_FLOAT` view clears nothing and `DEPTH | STENCIL` clears the depth plane. Until 2026-11-23
  `prepare` returned `invalid_clear` for it and the whole list was dropped (D3D-437); `d3d12_clear_test` compares three
  such cases with WARP. The depth value is kept as given: on a `D32_FLOAT` target WARP stores 1.5, -0.5 and NaN
  unchanged, and a clear whose flags name no plane clears nothing, so neither fails the list any more
  (`d3d12_clear_test` reads the texels back on WARP and here; `d3d12_close_parity_test` compares the `Close` verdicts).
- **Replay** (`runtime/src/vulkan_replay.h`): `prepare` resolves and checks a list without writing anything, `emit`
  writes it. Legacy states map through the section 4.3 table (`vulkan_access`), enhanced barriers through
  `vulkan_sync` / `vulkan_barrier_access` / `vulkan_layout` to `vkCmdPipelineBarrier2`. Whole-view clears are
  transfer clears inside a layout round trip; rectangle clears are `vkCmdClearAttachments` in a dynamic-rendering
  scope, with rectangles clipped to the mip. Copies become `vkCmdCopyBuffer`, `vkCmdCopyBufferToImage`,
  `vkCmdCopyImageToBuffer` and `vkCmdCopyImage`. A placed footprint's row pitch becomes `bufferRowLength`, and
  block-compressed extents stop at the mip edge (D3D12 pads footprints to whole blocks, Vulkan does not; a
  texture-to-texture box of a block-compressed format is rounded up to whole blocks, as on the PC runtime, must fit
  the destination's padded mip, and the Vulkan extent is cut where it reaches an image's edge; Vulkan wants that per
  image, so when no single cut extent is a block multiple or at the edge of both images, for example a 4x4 box into
  a 3x3 mip, the copy goes through transfer scratch (`vulkan_scratch.h`, as depth <-> colour copies do):
  `vkCmdCopyImageToBuffer` with the source's own cut extent into whole blocks, then `vkCmdCopyBufferToImage` with the
  destination's. The box fits the source's padded mip, so the scratch holds every block the destination reads). Each
  range and buffer bound is checked before anything is written. A copy runs in `TRANSFER_SRC`/`TRANSFER_DST`, or
  in place in `GENERAL` when the texture is in `COMMON`: that is D3D12's implicit promotion to a copy state.
  A same-subresource `CopyTextureRegion` uses `GENERAL` for both sides; preparation rejects overlapping regions.
  Cross-format `CopyTextureRegion` from an uncompressed texture into a partial BC edge block also uses transfer
  scratch. Each source texel supplies one block, and the destination's write extent stops at the actual mip edge.
  A 3D mip is one subresource. Boxed volume transfers preserve the full footprint's row and slice strides.
  Whole-resource depth/stencil copies issue one `vkCmdCopyImage` per aspect, with a transfer write-to-read/write
  dependency between them; all mips and layers stay batched within each aspect. This avoids stencil corruption
  observed on RX 5700 XT, Windows driver 32.0.21045.5002, when both aspects share one multi-region copy command.
  A standalone synthetic Vulkan multi-mip array reproduces the corruption with validation clean; this is a
  conservative compatibility measure, not evidence that the Vulkan specification requires this dependency.
  `d3d12_stencil_copy_test` checks D24S8/D32S8 across mip/layer counts and odd dimensions against WARP (CTest)
  or `LEONIDA_HOST_ADAPTER` (manual runs), plus the deterministic upload pattern for both planes and sources.
  **Depth ↔ colour copies** (`CopyResource` / `CopyTextureRegion` between `D32_FLOAT` and `R32_FLOAT/UINT/SINT`,
  or `D16_UNORM` and `R16_FLOAT/UNORM`, single-sampled, single-plane depth) copy the bits as the PC runtime does.
  `vkCmdCopyImage` refuses mismatched aspects, so each region goes image → scratch buffer → image
  (`src/vulkan_scratch.h`: device-local chunks kept until the submission completes, on every GPU). Not covered:
  depth with stencil (two planes), a D24 stored as D32, and multisampled textures; they are still refused. A float
  written into a `D32_SFLOAT` image outside [0, 1] is undefined in Vulkan without `VK_EXT_depth_range_unrestricted`,
  where D3D12 keeps the bits; `d3d12_copy_xfmt_test` uses values in [0, 1).
  **D24_UNORM_S8_UINT without a native D24** (the Radeon 880M, AMD hardware generally, RADV): the layer stores
  `D32_SFLOAT_S8_UINT` and makes it behave as D24 (`src/depth24.h`, `src/vulkan_depth24.h`). A footprint
  of the depth plane holds 24-bit UNORM words (bits 0-23; the top byte is unspecified, the PC runtime reads it as
  nothing and writes 0), the stencil plane is plain bytes. An upload converts the words with a compute pass
  (`src/depth24_convert.comp`, integer arithmetic) into a scratch buffer of floats, the nearest float to
  `d / 16777215`, which `vkCmdCopyBufferToImage` then moves into the image; a readback copies the image into the
  scratch and the pass writes the words (a float is multiplied by 16777215 in float and rounded to nearest, ties to
  even, which is what WARP and AMD's D3D12 driver do; NaN and negatives are 0, 1 and above the maximum). Every
  24-bit word makes the round trip unchanged (checked on all 16777216 by `depth24_convert_test`). Texture to
  texture copies move the floats as they are. `ClearDepthStencilView` of any D24 texture, native or stored as D32, clears with the float of the quantised value (an exact 24-bit value, so no driver tie rounding is left),
  so the texture reads back as D3D's quantisation of the clear value. The scratch buffers and descriptors live
  until the submission that used them completes. A copy larger than the device's `maxStorageBufferRange` (128 MiB at
  least) is split into several dispatches, by slices or by rows of one slice. The dispatch runs even under a
  `SetPredication` that would skip copies: the layer runs copies unpredicated (section 4.7) and the conversion
  must go with them, so conditional rendering is ended around it. The D3D11.3 functional spec (3.2.3.6, FLOAT to
  UNORM) says: NaN is 0, the value is clamped to 0..1, multiplied by 2^n - 1 in float, 0.5 is added and the
  fraction dropped, with a tolerance of 0.6 ULP on the integer side; read literally a tie rounds up, and 3.2.2
  lists this conversion as an exception to round-to-nearest-even without fixing ties. WARP and AMD's D3D12 driver
  were measured to round the float product to nearest even (2.5 gives 2, 0.5 gives 0), which the tolerance
  allows; NVIDIA's D3D12 driver (and a native D24 clear in its Vulkan driver) breaks some of those ties the other way;
  the layer follows WARP and AMD there, a measurement and not a rule of the spec. **Draws** (D3D-309,
  `src/depth24_fragment.h`): a single-sampled graphics pipeline with depth enabled and a D24 DSV stored as D32 has
  its pixel stage write `FragDepth = quantise(z + bias)`, with D3D's bias in D24 units (`DepthBias / (2^24 - 1) +
  SlopeScaledDepthBias * max(|dz/dx|, |dz/dy|)`, clamped by `DepthBiasClamp`) and the same quantisation as
  `depth24_convert.comp`, so depth tests and stored depth match D24; the rasteriser's bias is off for these
  pipelines (so a clamp needs no `depthBiasClamp` there). A pipeline without a pixel shader, with or without a
  geometry shader, gets a built-in one; a shader that writes its own depth has it quantised, unbiased, as in D3D. The slope comes from the derivatives of the
  interpolated depth, so a slope-scaled bias can land up to two 24-bit steps from D24 hardware's plane-exact one
  (`d3d12_depth24_draw_test`). Not covered: multisampled targets (a `FragDepth` write is per pixel; they keep float
  bias and precision, logged), pixel shaders with `EarlyFragmentTests` (which ignore a `FragDepth` write; logged), a
  viewport depth range narrower than the biased depth (D3D clamps before quantising), and early depth testing, which
  a `FragDepth` write turns off for these pipelines. A depth texture sampled in a shader still sees the float.
  `ResolveSubresource` becomes `vkCmdResolveImage` in the same layouts as a copy: a multisampled colour
  subresource into a single-sampled one of the same size and format. Integer colour formats in the requested
  format or either resource are refused at recording; `Close` fails. `ResolveSubresourceRegion` uses the same
  Vulkan operation for non-integer colour with average mode, taking a source rectangle (or the entire subresource)
  and a destination offset. A `ResolveSubresourceRegion` of one depth or stencil plane (the subresource's plane
  slice, the same on both sides) runs in a rendering scope of its own with no draws: the multisampled plane is the
  depth or stencil attachment (load `LOAD`, store `NONE`), the destination its resolve target with the mode, the
  render area the source rectangle. Both move from their resolve layouts to `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` and
  back around it; the destination's other plane is not attached and keeps its contents. A rectangle smaller than the
  subresource has the texels around it copied into a scratch buffer before the scope and back after it, on every
  GPU: Vulkan resolves only inside the render area, but MoltenVK (1.4.2 and 1.4.3, Apple M3 Max) resolves the whole
  destination subresource, because it resolves through Metal's store action, which has no render area. Measured
  with a raw Vulkan program (a 4x D32 attachment, render area (2,1) 5x4 of 8x6, `MIN`, `MAX` and `SAMPLE_ZERO`,
  store `NONE` or `STORE`): MoltenVK writes all 48 destination texels, KosmicKrisp only the 20 inside. No device
  property reports it, so nothing is gated. The format is the depth
  format or the plane's own (`R32_FLOAT`, `R24_UNORM_X8_TYPELESS`, `X24_TYPELESS_G8_UINT`, and for D32S8
  `R32_FLOAT_X8X24_TYPELESS` and `X32_TYPELESS_G8X24_UINT`; all compared with the PC runtime, TEST-847). A depth-only source above array layer 0 is resolved in a
  `VkRenderPass` with a framebuffer of the same two views instead of the rendering scope, on every GPU (D3D-417, a
  driver workaround): AMD's Windows driver (32.0.21042.62, Ryzen 7 9700X iGPU) resolves a dynamic-rendering depth
  attachment from layer 0 of the image when the multisampled view starts at another layer, while it honours the
  resolve view's first layer. Measured with a raw Vulkan program on a 4x D32 image of four layers: a layer range
  cleared with `vkCmdClearDepthStencilImage`, a `CLEAR` load or `vkCmdClearAttachments` reads back right through a
  `texelFetch` of every sample, through a resolve of all four layers in one scope and through a copy of the layer
  into a one-layer image that is then resolved, but a one-layer view resolved in a rendering scope gives layer 0
  for every layer (view type `2D` or `2D_ARRAY`, store `NONE` or `STORE`), and views of layers 1-3 resolved in one
  scope give layers 0-2. The render pass gives the right layer there; an RTX 3070 (610.74) does in every variant.
  `d3d12_view_slices_test` reads a 4x D32 array back slice by slice. A depth-stencil source above layer 0 still
  uses the rendering scope, because its two planes can be in different layouts, which the render pass's attachment
  description cannot say without the planes' layouts at recording; a 4x D24S8 or D32S8 array is still read back
  as its slice 0 on that driver (measured). The format is the depth
  format or the plane's own (`R32_FLOAT`, `R24_UNORM_X8_TYPELESS`, `X24_TYPELESS_G8_UINT`, and the D32S8 pair, untested).
  What the PC runtime accepts was measured on an RTX 5070 (driver 621.24) and on WARP: `MIN` and `MAX` of either
  plane by either format, and `AVERAGE` of the depth plane by its own format only (its `Close` fails for `AVERAGE`
  by the depth format or of stencil); the layer accepts the same. WARP is no reference for two of them: a resolve
  with the format `D32_FLOAT` passes its `Close` but removes the device when run (`DXGI_ERROR_DRIVER_INTERNAL_ERROR`),
  and a one-plane D24S8 resolve writes both planes; the RTX 5070 resolves only the plane named. A scope resolves each pixel in place, so a
  destination offset other than the source rectangle's corner is refused, as is a mode missing from the device's
  `supportedDepthResolveModes`/`supportedStencilResolveModes` (queried at device creation; one plane per scope, so
  `independentResolve` never applies). Refused calls fail `Close` and record nothing. `d3d12_depth_resolve_test`
  compares D32, D24S8 and D32S8 resolves (typed and typeless resources, whole and rectangle) with the PC runtime and
  WARP. Its edge cases (TEST-848) draw a triangle over the cleared 4x source first (depth 0.5, stencil 0x44): the
  pixels on its diagonal edge hold differing samples, and MIN, MAX and AVERAGE over them match the PC runtime on an
  RTX 5070 (driver 621.01) and WARP byte for byte. `DECOMPRESS`,
  `ResolveSubresource` of depth (which D3D12 does not offer) and resolves between two formats of one typeless family
  remain open. A multisampled texture copies only to one with the same sample count
  and has no footprint.
  Discards, immediate writes and atomic copies replay as section 4.2 describes.
- **UAV clears** (`ClearUnorderedAccessViewUint` / `ClearUnorderedAccessViewFloat`). The CPU handle's slot is read
  while recording as the GFX10 descriptor a shader would load (section 3.4), so the clear works with any
  view-creation code that writes them and with descriptors the game builds itself; the GPU handle is not needed.
  A T# gives the view's format (FORMAT plus DST_SEL, which tells BGRA from RGBA), mip and layers. A tagged 3D
  UAV selects `FirstWSlice` and `WSize`. An untagged 3D T# covers every slice of the bound resource's selected
  mip. A V# gives a byte range: its address minus the resource's `GetGPUVirtualAddress`, and
  NUM_RECORDS bytes (stride 0, no format: raw), records (a stride: structured) or elements (a format: typed). A view
  that reaches past the end of the buffer stops there, as the hardware's bounds check would.
  - Values follow D3D's rules (`runtime/src/uav_clear.h`): Uint copies the low bits of each value into its channel
    with no conversion, whatever the format's type; Float converts (UNORM/SNORM clamp and round to nearest, NaN is
    0; 16-bit and the packed 11/10-bit floats round toward zero, a finite value too large becoming the largest
    finite). A raw or structured view takes value 0 for every 32-bit word (Float: its bits). D3D11 calls Float on an
    integer view invalid and D3D12 does not say; here the value is converted (toward zero, clamped, NaN 0) so the
    result is defined. These are checked against hand-computed elements in `d3d12_uav_clear_test`.
  - Buffers: elements of up to 4 bytes repeat in every aligned word, so `vkCmdFillBuffer` writes the aligned middle
    and `vkCmdCopyBuffer` copies the unaligned ends from it (an 8-bit view at an odd offset, say). 8- and 16-byte
    elements are written with `vkCmdUpdateBuffer` (up to 64 KiB) and doubled with copies. A rectangle is an
    element range `[left, right)` on row 0. If a range contains no complete aligned word, prepare reserves eight
    transfer-scratch bytes, retained until submission completion. Replay fills that scratch with the repeated
    element and copies only the requested bytes, preserving neighbouring bytes even at resource boundaries.
    Rectangles within one clear reuse that pattern with transfer dependencies between them. Ownerless replay
    callers without `copy_scratch` still refuse these short ranges before emitting anything.
    `d3d12_short_uav_clear_test` compares Uint/Float whole-view and clipped/overlapping rectangle clears against
    WARP (CTest) or `LEONIDA_HOST_ADAPTER` (manual runs), checking the entire buffer against literal patterns.
    Portable native comparisons keep rectangles on row 0. Out-of-row controls run separately against WARP:
    NVIDIA D3D12 was observed to ignore Y where WARP and AMD preserve the buffer, so those controls do not assert
    that native drivers behave uniformly. This test distinction does not change the replay policy.
    Portable UNORM Float clears use `0.25f` (R8: `0x40`, R16: `0x4000`), away from a conversion midpoint;
    `0.5f` remains covered by separate WARP-only midpoint controls. Native NVIDIA was observed to clear that
    midpoint to `0x7f`/`0x7fff` where WARP and AMD give `0x80`/`0x8000`. All comparisons remain byte-exact.
  - Textures: the values become one element of the view's format, and that element is cleared into the image as
    the value that decodes it in the image's own format, so a typeless texture viewed as UINT gets exactly the bits
    asked for. Exact for every typed-UAV format except the SNORM pattern `0x80..` (Vulkan writes -1.0 as `0x81..`)
    and NaN payloads on the Vulkan color-clear paths. A whole, matching-format 3D mip uses `vkCmdClearColorImage`.
    Direct lists can use `vkCmdClearAttachments` for rectangles on attachment-capable 1D/2D images.
    Unpredicated partial 3D W ranges (`FirstWSlice`/`WSize`) and 3D XY rectangles use the same transfer path
    on direct and compute lists. It encodes the clear value in the view format, fills up to 64 KiB of scratch,
    and copies tiles one Z slice at a time in `GENERAL`. This requires matching view and backing formats, with
    the existing exception for emulated `R9G9B9E5` on `R32_UINT`.
  - The clear is fenced as the UAV state widened to transfers, so a following UAV barrier, transition, or enhanced
    barrier from `SYNC_CLEAR_UNORDERED_ACCESS_VIEW` sees it.
  - Under `SetPredication`, a direct list clears an attachment-capable 1D/2D UAV through its attachment view.
    A whole view becomes one rectangle so conditional rendering applies. Buffer clears and the direct-list
    whole-mip transfer fallback run unconditionally and count toward `unpredicated_copies`. Direct and compute
    lists refuse predicated partial-W or rectangular 3D clears before submission.
- **Layout tracking** (`LayoutTracker`): the current Vulkan layout of every image subresource, so barriers start
  from the real layout instead of the application's `before`. This is how implicit promotion and decay work: a
  texture in `COMMON` sits in `GENERAL`, which every promotable access can use, so a later barrier naming the
  promoted state still starts from `GENERAL`, and decay changes no layout. Buffers have no layout, and
  simultaneous-access textures stay in `GENERAL` for life. With `separateDepthStencilLayouts` the depth and stencil
  planes are tracked apart, so a depth-only clear or a one-plane transition leaves the other plane where it is.
- **Execution** (`runtime/src/vulkan_backend.h`): `VulkanContext` (registered resources, the tracker, a view cache),
  `VulkanQueue` (`ExecuteCommandLists` prepares every list before writing any, so a bad list submits nothing and
  leaves the tracker as it was; `Signal`; `Wait`, which is attached to the next submission because a Vulkan wait
  only holds back its own batch) and `VulkanFence` (a timeline semaphore).
- **COM** (`runtime/shims/d3d12_com.cpp`): `ID3D12GraphicsCommandList8` (the GCL7 chain plus
  `OMSetFrontAndBackStencilRef`), `ID3D12CommandQueue`, `ID3D12CommandAllocator` and `ID3D12Fence1` (with
  `SetEventOnCompletion` events). Every method they do not implement is a stub generated at build time from the
  SDK's `d3d12.h` by `tools/gen_d3d12_stubs.py`. The stub reports its name once, so the vtables follow the SDK
  order exactly and nothing derived from the SDK is checked in. Unknown IIDs are logged and refused. Resources
  are identified by their `ID3D12Resource` pointer, and RTV/DSV CPU handles point at a `TargetView`.
  `OMSetFrontAndBackStencilRef` records its two `UINT`s on the list (and `Reset` clears them); each draw sets them
  with `vkCmdSetStencilReference` and a face mask.
- **Device** (`runtime/shims/d3d12_vulkan_device.cpp`): an `ID3D12Device11` (PC ABI, 80 slots) that the Xbox-ABI
  proxy (`d3d12_xs_device.cpp`) forwards to, so slot 14's heap-handle conversion and slot 76's resource creation
  work unchanged. It creates the objects above, plus:
  - descriptor heaps whose slots are 32 bytes (`GetDescriptorHandleIncrementSize`; the console's increments are
    question 4). RTV/DSV slots hold a `TargetView`; shader-visible heaps return a GPU handle, but nothing reads it
    yet;
  - `CreateRenderTargetView` / `CreateDepthStencilView` for whole resources, `TEXTURE1D`, `TEXTURE1DARRAY` (D3D-63),
    `TEXTURE2D`, `TEXTURE2DMS`, `TEXTURE2DARRAY` and `TEXTURE2DMSARRAY` (a slice range; `ArraySize` -1 is every
    slice from the first on, as on the host, `d3d12_view_slices_test`);
  - `CopyDescriptors` / `CopyDescriptorsSimple`. A descriptor is the bytes of its slot, so a copy is a byte copy:
    the destination and source range lists are walked together, one descriptor at a time, and null size arrays
    mean one descriptor per range;
  - committed buffers (upload and readback heaps mapped persistently and zeroed), 1D textures and 1D arrays (render
    targets and depth-stencils too: their attachment views are `VK_IMAGE_VIEW_TYPE_1D_ARRAY`), and
    2D textures in the common render-target, depth and texture formats (typeless families become mutable-format
    images; `R24G8_TYPELESS` and `R32G8X24_TYPELESS` are depth-stencil images with or without
    `ALLOW_DEPTH_STENCIL`, as Vulkan has no colour format with their layout), plus non-attachment 3D textures with
    mipmapped upload, image copy and readback, and multisampled 2D textures (single-mip render targets or
    depth-stencils, in the sample counts the GPU has for the format);
  - packed `R9G9B9E5_SHAREDEXP` textures map to `VK_FORMAT_E5B9G9R9_UFLOAT_PACK32` for uploads and sampled views.
    When `ALLOW_UNORDERED_ACCESS` is requested on a GPU whose `VK_FORMAT_E5B9G9R9_UFLOAT_PACK32` lacks native
    `STORAGE_IMAGE` support (for example NVIDIA), the layer backs the texture with a mutable `VK_FORMAT_R32_UINT`
    image (`MUTABLE_FORMAT | EXTENDED_USAGE` + `VkImageFormatListCreateInfo` `{R32_UINT, E5B9G9R9_UFLOAT_PACK32}`),
    refusing `ALLOW_RENDER_TARGET`, `ALLOW_DEPTH_STENCIL` and `DENY_SHADER_RESOURCE`: `CheckFeatureSupport` advertises
    `TYPED_UNORDERED_ACCESS_VIEW` and `UAV_TYPED_STORE` (plus `UAV_TYPED_LOAD` with `R32_UINT` storage atomics) while
    keeping `RENDER_TARGET` off; SRV and UAV descriptors emit the `E5B9G9R9` T# (`FORMAT 132`, RGB1) and reject
    incompatible casts, while set 0 creates a `VK_IMAGE_USAGE_SAMPLED_BIT` `E5B9G9R9` sampled view and a
    `VK_IMAGE_USAGE_STORAGE_BIT` `R32_UINT` storage view (bound in `as_uint` / `r32ui`, never `as_float`);
    `ClearUnorderedAccessViewFloat` packs RGB9E5 with round-half-up (clamping negative/NaN to 0 and +Inf/overflow to
    65408, bumping on 512) and `ClearUnorderedAccessViewUint` writes 0, matching WARP (`d3d12_r9g9b9e5_uav_test`,
    `d3d12_uav_clear_test`);
  - the descriptor shader samples `B5G5R5A1_UNORM` through a 2D SRV. Opaque and transparent red check bit 15,
    and mixed-channel texels check the five-bit color fields;
  - `B5G5R5A1_UNORM` with `ALLOW_UNORDERED_ACCESS` on a GPU whose `VK_FORMAT_A1R5G5B5_UNORM_PACK16` lacks
    `STORAGE_IMAGE` (for example NVIDIA) is backed the same way, by a mutable `VK_FORMAT_R16_UINT` image with
    `{R16_UINT, A1R5G5B5_UNORM_PACK16}` in its format list (`Format::uint_backed`). Unlike `R9G9B9E5`, render
    targets are kept: RTVs and clears go through an `A1R5G5B5` attachment view, SRVs through an `A1R5G5B5`
    sampled view, and the UAV's storage view is `R16_UINT` in `as_uint`. SRV and UAV descriptors name the texture's
    own T# format (`1_5_5_5_UNORM`, 134), so a shader store needs the translator to pack the 5-5-5-1 bits for that
    view; until it does, `CheckFeatureSupport` does not advertise the typed UAV. Multisampled UAV textures are still
    refused (`d3d12_b5g5r5a1_uav_test`);
  - `R8G8B8A8_UNORM_SRGB` and `B8G8R8A8_UNORM_SRGB` with `ALLOW_UNORDERED_ACCESS`, which the host runtime sizes and
    creates (RTX 4090 and WARP) and a retail title sizes as a 2048x2048, 12-mip render target, on a GPU whose sRGB
    format lacks `STORAGE_IMAGE` (most): a mutable image of the UNORM format of the same order, with both formats in
    its format list (`Format::uint_backed`). Its storage view is UNORM, as every D3D12 typed UAV of these textures
    is, so shaders and `ClearUnorderedAccessViewFloat` write raw bytes; RTVs and SRVs are sRGB, so clears and
    rendering encode and sampling decodes. BGRA8 needs `B8G8R8A8_UNORM` storage. Both clears match WARP byte for
    byte; multisampled ones are still refused (`d3d12_srgb_uav_test`);
  - `B8G8R8X8_UNORM`, `_UNORM_SRGB` and `_TYPELESS` (D3D-87) are `B8G8R8A8` images: clears, copies and footprints
    keep all four bytes (WARP writes the clear's alpha into the X byte too), and shader views read alpha as 1. Its
    typeless family is its own, so views to or from `B8G8R8A8` are refused, unless the texture's castable-format
    list names the view's format (relaxed casting, which WARP and AMD's D3D12 driver accept both ways). The view's
    format then decides the alpha: a `B8G8R8X8` view of a `B8G8R8A8` texture reads 1, a `B8G8R8A8` view of a
    `B8G8R8X8` one the stored byte. It decides the channel order too: an `R8G8B8A8` texture whose list names
    `B8G8R8A8` or `B8G8R8X8` reads red and blue swapped through those views, as on the host (both orders share one
    T# format code, so the T#'s swizzle carries the order). An RTV in a format the list names stores clears and
    draws in the view's channel order and encoding, as on the host (see the castable-list RTVs under D3D-275 below).
    As on the host runtime, the family has no typed UAV, and a typed `B8G8R8X8`
    texture cannot allow unordered access (the host accepts one whose castable-format list names `B8G8R8A8`; the
    layer doesn't yet). Blending into a `B8G8R8X8` render target reads destination
    alpha as 1, as WARP and AMD's D3D12 driver do: the PSO turns `DEST_ALPHA` into `ONE`, `INV_DEST_ALPHA` into
    `ZERO` and the colour `SRC_ALPHA_SAT` into `ZERO`, where the BGRA8 image would read the X byte. What a draw
    leaves in the X byte is not defined (WARP writes 0, AMD the blended alpha), and no view reads it;
  - `R10G10B10_XR_BIAS_A2_UNORM` maps to `VK_FORMAT_A2B10G10R10_UNORM_PACK32`, the same packed 10:10:10:2
    layout; the bias and scale are the application's shader work. Like the PC runtime, it accepts only a
    single-mip, single-sample 2D texture: extra mips, multisampling and 3D are refused in creation, allocation
    and footprints. Format support reports `TEXTURE2D` and `CAST_WITHIN_BIT_LAYOUT`, not the host's display,
    back-buffer-cast or video-processor bits;
  - `CheckFeatureSupport` for feature levels (up to 12_1), architecture, D3D12 options (`OPTIONS`, `OPTIONS1`–`5`,
    `7`–`9`, `11`–`21`), GPU virtual-address support, format support, format plane counts, multisample quality
    levels, shader model, root signature version, predication (`VK_EXT_conditional_rendering`), command-queue
    priority support, protected-resource session queries, shader cache, existing heaps, serialization, cross-node
    sharing, displayable resources, meta-command queries (`E_INVALIDARG`), application-specific driver state, and
    bytecode bypass hash support. `D3D12_OPTIONS8` reports unaligned block textures when the Vulkan logical device
    enables BC texture compression, and `D3D12_OPTIONS12` reports `RelaxedFormatCastingSupported = TRUE`, so a
    committed or placed texture accepts a castable-format list. Other queries (including `OPTIONS6` and `OPTIONS10`)
    are logged and refused until the trace shows what the game asks (question 11);
  - Single-sampled `A8_UNORM` 1D, 2D and 3D textures use one byte per texel in `VK_FORMAT_R8_UNORM`. Their SRVs expose
    (0, 0, 0, A) before applying the caller's component mapping. Format support includes `MIP` when the Vulkan
    image supports multiple levels. Where the GPU has `VK_FORMAT_A8_UNORM_KHR` (`VK_KHR_maintenance5`) and can
    sample, copy and blend it as a colour attachment, a texture created with `ALLOW_RENDER_TARGET` is stored in that
    format instead: its RTVs, PSO `RTVFormats` and clears use the alpha channel, its SRVs view it in the same format
    (the T# names its one channel X, gfx10's `channels_a`), and format support adds `RENDER_TARGET` and
    `BLENDABLE`. Vulkan puts A8 in a compatibility class of its own, so the two kinds of A8 texture cannot share
    an image, and `vkCmdCopyImage` refuses a copy between them (`VUID-vkCmdCopyImage-srcImage-01548`):
    `CopyResource` and `CopyTextureRegion` between the two go image → transfer scratch → image, as depth ↔ colour
    copies do, one byte per texel (`d3d12_a8_copy_test`). A castable-format list naming `A8_UNORM` itself is a no-op; any other format in it is refused with
    `E_INVALIDARG` for such a render target, since no other format can view its image. Without that format,
    render targets are refused as before; depth-stencil and UAV creation are
    unsupported for this format.
  - A `CopyTextureRegion` of the depth plane between two depth formats with the same depth encoding but not the
    same `VkFormat` (`D32_SFLOAT_S8_UINT` and `D32_SFLOAT`, `D16_UNORM_S8_UINT` and `D16_UNORM`,
    `D24_UNORM_S8_UINT` and `X8_D24_UNORM_PACK32`) goes image → transfer scratch → image too, depth aspect on both
    sides (`vulkan_replay.h`, `depth_format_pair`; `replay_copy_offset_test`). `vkCmdCopyImage` requires
    depth/stencil formats to match exactly (`VUID-vkCmdCopyImage-srcImage-01548`), and an NVIDIA driver (RTX 4070
    Laptop, 621.40) handed the `D32_SFLOAT_S8_UINT` → `D32_SFLOAT` pair wrote past the end of the `D32_SFLOAT`
    image: the world-start `WRITE_INVALID` device loss, with the fault at the first byte after the image.
  - reserved resources and `GetResourceTiling` (section 4.2);
  - query heaps for occlusion, timestamps and pipeline statistics, and `GetTimestampFrequency` on the queue
    (section 4.6);
  - `GetCopyableFootprints` for buffers, 1D and 1D-array textures, 2D textures and volumes, including BC1–7 and the
    two-plane depth-stencil formats.
    The test compares every output with the PC runtime's; that comparison found that planar
    depth-stencil rows are padded to 512 bytes, not 256, and that subresources are aligned to 512 bytes counting
    from `BaseOffset`, not from 0: the PC runtime moves the whole layout by the base, so an unaligned base (513,
    2^32 + 1) keeps the same relative offsets and the same total (measured on an AMD Radeon RX 7600, drivers 26.11.1
    and 26.11.2, Windows 10 in-box runtime; aligning from 0 gives mismatching offsets and totals at both unaligned
    bases and none at 0 and 1024, which is why it went unnoticed).
  - the `1`/`2`/`3` variants as wrappers over the methods above (D3D-28): `CreateCommittedResource1/2/3`,
    `CreateCommandQueue1` and `GetCopyableFootprints1`. `D3D12_RESOURCE_DESC1` drops to `D3D12_RESOURCE_DESC`,
    and the initial state or `D3D12_BARRIER_LAYOUT` is not used, because new textures start `UNDEFINED` and move
    on first use. The newer arguments are answered as the PC runtime answers them. `d3d12_method_variants_test`
    compares each answer below with the host runtime live, measured on an RTX 4090 (driver 621.14):
  - `CreateCommandSignature` and `ExecuteIndirect`: a signature consisting of a single DRAW or DRAW_INDEXED
    operation inherits the graphics state of its command list. The Vulkan replay snapshots the same pipeline, root
    data, render targets, IA bindings and raster state as a direct draw, then reads the records on the GPU inside
    that draw's rendering scope. A padded stride is supported; it is not evidence that a signature is simple.
    A compound graphics signature still emits no draw, but its written VB/IB and root bindings are reset for the
    next command, even if no record is interpreted. Untouched slots and previously recorded draws retain their state.
    See the [D3D12 state-leakage contract](https://microsoft.github.io/DirectX-Specs/d3d/IndirectDrawing.html#state-leakage).
    Fixed- and GPU-counted batches use the device's enabled indirect features and limits. The current graphics
    path covers triangle lists on direct command lists. A bundle can defer a single operation with no count
    buffer: its argument-buffer identity and byte offset are recorded once; each `ExecuteBundle` captures the
    direct parent's state at that call. Count buffers and compound signatures remain invalid in bundles.
    `d3d12_bundle_indirect_warp` and `d3d12_bundle_indirect_layer` compare DRAW and DRAW_INDEXED against direct
    pixels with two different parent bindings; the layer fixture also checks marker order and captured views.
    Compound graphics signatures, non-unit instance divisors
    and unsupported capabilities stay guarded. A GPU-counted draw currently requires
    `maxDrawIndirectCount == UINT32_MAX`: Vulkan also limits the raw counter value, not just the caller's maximum.
    To omit indirect graphics without changing direct drawing or compute, set
    `LEONIDA_INDIRECT_DRAWS=off` before starting the process. This applies to the Vulkan-backed D3D12
    command list, including ordinary bundle draws when replayed on a parent. Validation and compound
    root/IA resets still happen. Only exact lowercase `off` disables drawing; unset, empty, and other
    values (including the old `raw` spelling) retain the normal capability-checked path. There is no
    bare draw mode. The former `raw` setting logs an obsolescence notice on first policy use.
    Each skip reason logs once. Progress lines give cumulative recorded commands, Vulkan emissions,
    recording skips and replay skips. Replaying a command list can emit a recorded command more than once.
    By default, a GPU pass bounds draw vertex/index and instance counts, dispatch group counts, and total
    work across each command list (2^27 units). Unfilled all-ones records produce zero work and spend no budget.
    Cumulative observed clamp and zero counts are reported every 60 presented frames. Counted calls read at most MaxCommandCount records. The stateful draw or dispatch reads
    private scratch. Exhausting the diagnostic counter slots only disables readback of those counters.
    `LEONIDA_INDIRECT_CLAMP=0` disables this pass. `LEONIDA_INDIRECT_GROUP_BUDGET=N` further bounds each
    indirect dispatch, preserving X first, then Y and Z (1–65535 groups). It also works with the clamp disabled.
  - Optional `VK_KHR_acceleration_structure` + `VK_KHR_deferred_host_operations` + `VK_KHR_ray_query`:
    `ID3D12Device5::GetRaytracingAccelerationStructurePrebuildInfo`, `ID3D12GraphicsCommandList4::BuildRaytracingAccelerationStructure`
    (BLAS + TLAS), `vkGetBufferDeviceAddress` in `ID3D12Resource::GetGPUVirtualAddress()`, and RT-aware `UAV`
    barriers (cleanly disabled/skipped on GPUs without RT).

- **DXGI Swapchain** (`runtime/shims/dxgi_swapchain.h`, `runtime/shims/dxgi_com.{h,cpp}`): `IDXGIFactory4` and
  `IDXGISwapChain3` over `VK_KHR_win32_surface` + `VkSwapchainKHR`, exposed via `rt::d3d12::create_dxgi_factory`.
  Swapchain backbuffers are real `ID3D12Resource` COM objects (`BackBufferResource` inheriting from
  `ID3D12ResourceStubs`) registered directly with `VulkanContext`, supporting `CreateSwapChainForHwnd`,
  `GetCurrentBackBufferIndex`, `GetBuffer`, `CreateRenderTargetView`, `Present`, and `ResizeBuffers` (preserving
  explicit buffer dimensions even when the HWND client size is unchanged via an offscreen backbuffer + blit path,
  and exposing exactly the requested `BufferCount` even when Vulkan's native image count differs, in which case the
  offscreen ring is the app-visible swapchain and each present blits the current slot into the acquired image).
  The factory answers up to `IDXGIFactory7` (D3D-2). `EnumAdapterByGpuPreference` returns the one Vulkan adapter
  at index 0 for every preference. It accepts a preference outside the enum, as the host does, and gives
  `DXGI_ERROR_INVALID_CALL` for a null `out` and `DXGI_ERROR_NOT_FOUND` past the end. The adapter list never
  changes, so `RegisterAdaptersChangedEvent` never signals its event. It still gives the host's answers: a null
  event or cookie, or an event that is already registered, is `DXGI_ERROR_INVALID_CALL`. Cookies start at 1 and
  are separate from the occlusion cookies, and an unknown cookie is `DXGI_ERROR_NOT_FOUND`. All of these were
  measured on the host's `dxgi.dll` (Windows 11, RTX 3070) and are checked in `dxgi_swapchain_test`.
  explicit buffer dimensions even when the HWND client size is unchanged via an offscreen backbuffer + blit path,
  and exposing exactly the requested `BufferCount` even when Vulkan's native image count differs, in which case the
  offscreen ring is the app-visible swapchain and each present blits the current slot into the acquired image).

    | Argument | Answer |
    |---|---|
    | a protected resource session | `E_INVALIDARG`; the layer reports no `D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT` |
    | `SamplerFeedbackMipRegion` on a texture | `E_INVALIDARG` from `CreateCommittedResource2/3`; `GetCopyableFootprints1` returns its failure answer (every output all ones) |
    | `SamplerFeedbackMipRegion` on a buffer | ignored |
    | a buffer's initial layout other than `UNDEFINED` | `E_INVALIDARG` |
    | a castable-format count with a null list | no list (`S_OK`) |
    | a castable-format list | accepted on a texture when the formats map to Vulkan formats: the image is mutable and carries a `VkImageFormatListCreateInfo`; a buffer or an unmapped format is `E_INVALIDARG`. A cast whose texel size differs from the resource's is also `E_INVALIDARG` — the host refuses other sizes, and a size-mismatched format list is a Vulkan validation error (`VUID-VkImageCreateInfo-pNext-06722`). The list is kept on the resource, so an SRV whose format is neither the resource format nor in the list leaves a null descriptor. The resource's own format is always accepted and is added to the Vulkan format list when the D3D12 list omits it. A listed same-size cross-family cast, such as `R8G8B8A8_UINT` to `R32_UINT`, is accepted. An RTV in a listed RGBA8 family format on a typed `R8G8B8A8_UNORM` texture clears through the cast, the same reinterpretation a typeless RTV uses; without a list a typed resource still refuses a different-format RTV. `D3D12_OPTIONS12` reports `RelaxedFormatCastingSupported = TRUE` (this layer requires Vulkan 1.3, where the format list is core). `CreatePlacedResource2`'s list reaches its placed image through the same rules (D3D-90) |
    | `CreateCommandQueue1`'s creator ID | ignored |

    `CreatePlacedResource1/2` wrap the placed path above; `2` carries the castable-format list. `CreateHeap1`
    remains a stub.
  - `CreateSampler2` as a wrapper over `CreateSampler` (D3D-28, part 2). Without flags it writes the same S#.
    `D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR` reads the union as integers. `(0,0,0,0)`, `(0,0,0,1)` and `(1,1,1,1)`
    take the S#'s transparent-black, opaque-black and opaque-white types. The S# has no separate integer types,
    since the hardware reads them by the image's format. Vulkan does not: an integer image sampled with a
    `VK_BORDER_COLOR_FLOAT_*` border reads 0. Vonsek found this on an RTX 5080 (#538), and `d3d12_descriptor_test`
    reproduces it on an RTX 4090 when the layer uses the float enums. So the layer marks an integer border in the S#
    with `BORDER_COLOR_PTR = 0x800` (`gfx10::kIntegerBorderPtr`). The hardware reads that pointer only for a table
    colour, and Mesa writes it only for `SQ_TEX_BORDER_COLOR_REGISTER`. The mark gives the integer border its own
    key in the descriptor hash table, so it does not share the float border's `VkSampler`, and `vulkan_sampler`
    makes it `VK_BORDER_COLOR_INT_*`. Any other colour is a table colour (`BorderColor::table`). With
    `VK_EXT_custom_border_color` (D3D-73), the layer registers custom float and UINT border colours in a device-wide
    table of up to 4000 entries; the table index is stored in `BORDER_COLOR_PTR [11:0]` of the S#, so distinct custom
    colours produce distinct keys in the descriptor hash table and distinct `VkSampler` objects. On sampler creation,
    `VkSamplerCustomBorderColorCreateInfoEXT` passes the colour components with `VK_BORDER_COLOR_FLOAT_CUSTOM_EXT` or
    `VK_BORDER_COLOR_INT_CUSTOM_EXT` and `format = VK_FORMAT_UNDEFINED` (via `customBorderColorWithoutFormat`). Without
    the extension, custom colours fall back to transparent black with a diagnostic log line, using
    `VK_BORDER_COLOR_INT_TRANSPARENT_BLACK` for integer borders and `VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK` for
    float borders to satisfy Vulkan input validation rules on integer views. An integer border colour
    with the bits of `1.0f` is not white and receives a table colour index.
    `D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES` sets `FORCE_UNNORMALIZED` (word 0, bit 15) and changes nothing
    else; `vulkan_sampler` makes it `unnormalizedCoordinates`. The layer answers `D3D12_OPTIONS17` with
    `NonNormalizedCoordinateSamplersSupported` (core Vulkan, with the same restrictions) and no
    `ManualWriteTrackingResourceSupported`. That is what the RTX 4090 and WARP report. The runtime on a Radeon 680M
    (D3D12 driver 31.0.14084.6002) reports no non-normalized samplers, and leaves every non-normalized description
    unwritten, including those the table below says are written.

    A description the PC runtime refuses leaves the slot as it was. The runtime itself does this, without the
    debug layer, on the RTX 4090 (driver 621.14) and on WARP; the debug layer's message names the rule.
    `d3d12_sampler2_test` compares each row with the host runtime live:

    | `CreateSampler2` input | Answer |
    |---|---|
    | a null description | the slot is left as it was |
    | a flag other than the two | the slot is left as it was |
    | non-normalized, with MIN ≠ MAG, a linear MIP filter, anisotropy or comparison | the slot is left as it was; `MINIMUM`/`MAXIMUM` filters are accepted |
    | non-normalized, with `AddressU` or `AddressV` not `CLAMP`/`BORDER` | the slot is left as it was; `AddressW` is free, as in Vulkan |
    | non-normalized, with `MinLOD`, `MaxLOD` or `MipLODBias` not 0 | the slot is left as it was; `-0` is 0 |
    | non-normalized, a point filter with `MaxAnisotropy` or `ComparisonFunc` set | written; both are ignored (the debug layer warns about `ComparisonFunc`) |
    | both flags | written |

  BC1–7 textures need `textureCompressionBC`, which the device enables when the GPU has it, as it does
  `occlusionQueryPrecise` and `pipelineStatisticsQuery` for the query heaps.
- **Presentation** (`leonida::VulkanPresenter` in `runtime/src/vulkan_present.h`, which
  `rt::d3d12::create_window_presenter` makes for a device and a window). This is the part the
  `PresentX` front end uses; which of DXGI or `PresentX` the game uses is still open (question 6). It takes a back
  buffer, an ordinary texture in the D3D12 `PRESENT` state, and blits it to an acquired `VkSwapchainKHR` image,
  scaling when the window size differs. The blit goes on the same `VkQueue` after the application's work, so the
  frame contains everything submitted before the present. Details:
  - two frames in flight, and a present semaphore per swapchain image;
  - FIFO when vsync is asked for, otherwise MAILBOX or IMMEDIATE; with `VK_KHR/EXT_swapchain_maintenance1` the FIFO
    swapchain can also present one frame in IMMEDIATE or FIFO_RELAXED (`present(..., tear)`, section 4.5);
  - the swapchain is recreated on resize or when the surface reports it out of date or suboptimal, and a minimised
    window skips presents;
  - a back buffer that was never written is zero-filled like any other texture;
  - `presented()` counts the presents the engine took (`VK_SUCCESS` or `VK_SUBOPTIMAL_KHR`), not ones it reported
    out of date;
  - a recreation is published only when the swapchain, its images and every per-image semaphore exist. On a
    failure the partial objects are released and the next present tries again, so no image is acquired without
    its semaphore;
  - if the blit's submission fails after an image was acquired, an empty batch takes the acquire semaphore and
    signals the frame's fence, and the swapchain is recreated next time to get the image back. The next present
    therefore does not wait forever on a fence nothing signals. If even that batch fails, or a fence wait or reset
    fails, the presenter stops (`ok()` is false) and its owner makes a new one.
  - for `PresentX` (section 4.5): a source rectangle, a blank present, an sRGB swapchain format for an sRGB source,
    and an optional capture of each presented frame to host memory (what `xbox_present_test` reads back).

  The device needs `VK_KHR_swapchain` (and the instance the surface extensions). The standalone device enables
  them when present.

  New textures are not touched at creation. They are tracked as `UNDEFINED`, and the first command that uses a
  part of one zero-fills it (D3D12 committed resources start zeroed; `CREATE_NOT_ZEROED` skips this) and moves it
  to the layout that command expects, so creation needs no submission. Block-compressed textures are not
  zero-filled, because Vulkan cannot clear them; they start undefined until the application uploads them. Each
  submission ends with a barrier to the host, so a readback buffer holds the GPU's writes once the fence wait
  returns. Children hold a reference to the proxy, not
  the inner device, so `GetDevice` always returns the Xbox-ABI object. Each live D3D12 queue leases a distinct
  `VkQueue` while one is free (its own type's family first, then the graphics family); the standalone device
  requests up to 16 per family. The queues still share a submit lock for
  replay state, but Wait-before-Signal between them can make progress (D3D-97), and a batch that waits for a Signal
  not yet submitted is held on the CPU so layouts follow dependency order (section 4.4). Caller-owned Vulkan devices can
  supply additional handles through `VulkanDeviceInfo::command_queues`; without them they retain legacy single-queue
  behavior. When every handle is in use, the new D3D12 queue shares the `VkQueue` that has the fewest D3D12 queues
  (section 4.4, "More D3D12 queues than Vulkan queues"); it used to fail with `E_OUTOFMEMORY`.
  Dedicated compute/transfer
  *families* would also require resource queue-family ownership handling and remain future work.

Descriptors and the bindless set (milestone M2, without the pipelines that will bind the set), from the shader's
side inwards:
- **`runtime/src/vulkan_bindless.h`**: set 0 includes the single-sample bindings the translator declares, optional
  multisample bindings, and
  `DescriptorTable` (`runtime/src/descriptor_table.h`, section 5.1) behind binding 2. One set for the device.
  The translator declares every array as a runtime array, so the counts are the runtime's choice. Details:
  - The device sizes the arrays once, when it creates the set. It does not recreate the set, because pipeline
    layouts are built on `bindless_set_layout()`. `bindless::size_bindings` reads the update-after-bind limits
    (`maxPerStageDescriptorUpdateAfterBind*`, `maxDescriptorSetUpdateAfterBind*`,
    `maxPerStageUpdateAfterBindResources`, `maxUpdateAfterBindDescriptorsInAllPools`), or the plain limits for a
    set without update-after-bind. It starts from the default counts below (4096 sampled 2D images, 1024
    samplers, 1024 2D-array, 256 1D, 256 3D and 256 per storage array). Where a limit is lower, it clamps the
    sampler, storage and 2D arrays, in that order, and the device logs it. Next come the MS pairs that fit. Then it
    grows the arrays, in the same order, towards the maximums:
    - samplers: up to 4096, and at most `maxSamplerAllocationCount` less 64 for samplers outside the set. That
      covers a full 2048-sampler D3D12 heap plus room for literal and static S#s.
    - 2D images: up to 65536.
    - storage arrays: up to 4096 each, capped by their dimension's sampled array or by 256 for storage-only 1D arrays.
    - 2D-array, 3D and 1D images: up to 65536 each, as many as 2D, grown together (one count for the three while the
      limits stop them short of it), so no dimension is favoured. A texture keeps an index for each distinct T# of
      it for as long as it lives, and which dimensions its textures have is the title's choice: a native boot
      filled the default 1024 2D-array and 256 1D indices during the story load, and world runs filled the 4096 1D
      indices of the sizes before this with 4092 T#s of 4090 live 1D textures. The bound is memory, not the
      device's limits (RADV allows 8388606 sampled images): every element of every array is in each generation's
      descriptor pool, used or not.

    Growth leaves 64 descriptors of each limit to the other sets of a pipeline layout. It also stays within
    `maxPerSetDescriptors`, which never shrinks the default counts, because a layout past it may still be
    supported: past it, the device grows only where `vkGetDescriptorSetLayoutSupport` accepts the grown layout.
    Intel's UHD 770 (101.7092) reports 6159 and accepts the full sizes, so it gets the same set as an RTX (D3D-219).
    MoltenVK (1.4.2 and 1.4.3 on an M3 Max) reports 1212 and accepts only layouts with fewer descriptors than that
    (`MVKDevice::getDescriptorSetLayoutSupport` compares the sum with it), although it creates and samples a set of
    65536 2D images and 4096 samplers correctly (a native repro, and `d3d12_descriptor_test` under Wine with the set
    grown). So on MoltenVK the D3D12 device doesn't ask: the per-type update-after-bind limits, which MoltenVK takes
    from Metal's argument-buffer limits (500000 samplers on an M3 Max), decide alone, and the set grows as on other
    GPUs. Where a set still ends up short of a full 2048-sampler D3D12 heap, the device logs that at startup. Past
    the last free index, `CreateShaderResourceView` and `CreateSampler` log once that the array is full, and a
    shader reads the fallback. `d3d12_descriptor_test` then fills each short array to its last index and checks that
    the next view and sampler are refused.
    The MS arrays keep their default counts. On an RTX 5070 (driver 621.24, 1048576 of each type
    and 4000 samplers) the set has 65536 2D, 2D-array, 3D and 1D images each, 3936 samplers and 4096 storage indices
    in each dimension: 325729 descriptors with the MS pairs, 10-20 MiB at 32-64 bytes a descriptor. RADV (Mesa 26.3,
    the Raphael iGPU and the RX 7900 XTX) and llvmpipe get the same image counts and 4096 samplers. The device prints
    the counts it chose (`bindless set:`), `bindless_sizing_test` checks the rules on synthetic limits, and
    `vk_bindless_dimensions_test` reads 5000 live 1D, 2D-array and 3D images each through their indices in a set
    sized from the device's own limits.
  - A T# the title writes into a console descriptor heap that set 0 has no room for is refused, and counted in the
    heap's `unbound T#s by reason` line, as `the bindless array of the T#'s dimension is full` (or `the bindless
    storage arrays of the T#'s dimension are full` for a UAV whose dimension has free indices only above the shorter
    storage arrays), not as `no view for the T#'s format or range`. The first such refusal of a dimension also logs
    `a bindless image array is full; dimension N (name)` with the indices, the T#s and textures that hold them and how
    many of those textures are still registered at their address (a texture's indices go back when it goes).
  - An image view has one index in its dimension (2D, 2D array, 3D, 1D, 2DMS, 2DMS array or 1D array), shared by the
    bindings for that dimension. The runtime adds sampled-image bindings in pairs for 2DMS and 2DMS array:
    20/21 for floating-point and normalized color, 22/23 for unsigned integers, and 24/25 for signed integers.
    The runtime enables pairs that fit the device's descriptor limits and have a multisampled zero fallback
    of the matching numeric class. A missed key reads zero at texel (0, 0), layer 0, sample 0.
    The 1D-array dimension uses storage bindings 26–28 and has no sampled binding.
  - The storage arrays are shorter (`BindlessSet::storage_capacity`). Each array holds at most as many images
    as its dimension, and the 2D r32ui array is the same size as the other 2D storage arrays. So an image with a
    storage view gets an index the storage arrays hold, and a read-only one goes above where there is room.
    Index 0 is reserved, so 256 descriptors hold 255 live views in each 1D-array storage binding.
  - Index 0 of every array is a fallback (a 1x1 image of zeros, a nearest clamping sampler), because a miss is
    index 0. It stands for a null descriptor (a zero T#, an empty heap slot, a view the layer refused) and is of
    the kind the instruction asks for: the translator picks the binding by the instruction's dimension (a cube
    reads the 2D-array binding) and the view by the T#'s class, so each dimension and class reads zeros. The
    storage arrays' fallbacks never share an image with the sampled ones, so a store or atomic through a null
    UAV cannot show in a later read through a null SRV. Where the device has `VK_EXT_robustness2`'s
    `nullDescriptor` (`VulkanDeviceInfo::null_descriptor`, enabled alone) they are null descriptors, and the
    store or atomic is dropped and a load reads zero, as on the console; without it they are 1x1 images of their
    own. A null V# (all zeros) needs no fallback: every access through it is out of range (`NUM_RECORDS` 0), so
    loads give 0 and stores and atomics do nothing (`tests/semantic/test_null_vsharp.py`). Checked on the GPU by
    `vk_null_descriptor_test` (every dimension and class, both kinds of storage fallback).
  - Changes are queued and written by `flush()`, one write a slot, so a view that is removed before the next
    flush is never written.
  - `flush()` also uploads the table to its buffer: device-local memory, written from a mapped staging copy when
    the buffers are created (first use or growth) and with `vkCmdUpdateBuffer` for slot changes. Adding a key only
    puts a new entry in an empty slot, and the copy of an entry is not atomic; a running lookup that does not know
    the key cannot resolve to it, which is accepted. Erasing a key empties its slot and moves no other entry
    (every lookup, CPU and shader, compares all eight probe slots and never stops at an empty one), so it is
    uploaded like an add. Replacing a live key's index, an insert that relocates a live entry, and a table that
    has grown (a new buffer behind binding 2) change what running lookups see: `flush_replaces_table()` says when a
    flush would do one of those, and the flush then makes a new generation instead of writing in place (D3D-314; see the
    paragraph on the flush at every `ExecuteCommandLists`, below). Erasing used to compact the probe cluster, which
    moved other keys and made every texture release wait for the whole GPU.
- **`runtime/src/gfx10_descriptors.h`**: the T#, S# and V# structures (section 5.3), encoded and decoded, with
  their mapping to Vulkan formats, view types, component mappings and samplers. Field positions are Mesa's
  (`src/amd/registers/gfx10-rsrc.json`) and agree with what the translator and the GPU tests already use. Bits
  that only the console's driver could give a meaning (tiling, compression metadata, performance hints) are
  written as zero.
- **Device**: `CreateShaderResourceView` and `CreateUnorderedAccessView` of textures write a T# into the slot,
  `CreateSampler` an S#; each then makes sure set 0 has the view or sampler and the table has the key. Details:
  - The key is the descriptor's bytes, so a copied descriptor needs nothing more. An SRV and UAV with identical
    bytes can share an entry; a partial 3D UAV carries a W-range tag and remains separate from the unsliced SRV.
  - A D3D12 comparison sampler with function NEVER and an ordinary sampler both encode zero in the hardware
    depth comparison field. The layer marks the NEVER comparison in S# word 3, bit 12 (unused with
    DERIV_ADJUST_EN clear) to give it a separate lookup key and a comparison-enabled Vulkan sampler.
  - A view the layer cannot show (below) leaves a null descriptor, all zeros, which finds the fallback.
  - A typed multisampled color texture has one mip. Its default SRV covers every layer. An explicit
    `TEXTURE2DMS` view reads layer zero, while `TEXTURE2DMSARRAY` selects a range starting at
    `FirstArraySlice` (`ArraySize = UINT_MAX` selects the remaining layers). The format-support query
    advertises `MULTISAMPLE_LOAD` when the format can be sampled with MSAA and its binding pair is available.
  - A texture is named in its T#s by an address of its own, above 4 GiB and 256 bytes apart, that nothing
    dereferences; its entries leave the set and the table when it is destroyed.
  - `VulkanDeviceInfo::bindless` says the Vulkan device has the features this needs: `runtimeDescriptorArray`,
    dynamic indexing of sampled- and storage-image arrays, and update after bind for sampled images, storage
    images and storage buffers. The standalone device enables them where the GPU has them.
  - Every `ExecuteCommandLists` flushes the set before its commands are written.
  - `rt::d3d12::bindless_set` and `bindless_set_layout` hand out the set and its layout: what a pipeline whose
    shader declares set-0 bindings is created with and binds.
- **Layouts.** A descriptor in set 0 has one image layout for as long as it lives, but D3D12 lets a texture be
  sampled in a shader-resource state and also while promoted from `COMMON`, which is `GENERAL` here (section
  4.3). So with the bindless set, a texture's shader-resource states use `GENERAL` too
  (`ResourceBinding::sampled_in_general`), and every descriptor is written with `GENERAL`. This follows from the
  Vulkan rule that a descriptor's layout matches the image's; no test here can see it broken, because the
  validation layer does not follow which element of a runtime array a shader reads. A depth texture sampled
  while it is also bound read-only (`DEPTH_READ` with a shader-resource state) is in
  `DEPTH_STENCIL_READ_ONLY_OPTIMAL` and is not handled.

A typed single-sampled 3D texture can use a default SRV or an explicit `TEXTURE3D` SRV of the same format.
An `R8G8B8A8_TYPELESS` volume supports explicit `R8G8B8A8_UNORM` and `R8G8B8A8_UNORM_SRGB` SRVs.
The sRGB view decodes RGB and preserves linear alpha. A typeless volume requires an explicit typed view.
The T# records the base depth, the view's mip range and `ResourceMinLODClamp`. Binding 6 holds the 3D Vulkan view,
which has one array layer. `d3d12_descriptor_test` checks samples at four W coordinates through two views of the same
volume, plus minimum-LOD clamped views (inside the mip range via `VK_EXT_image_view_min_lod`, at or below
`MostDetailedMip`, and above the last mip).

Known gaps in the descriptor path: buffer shader-resource views and constant-buffer views (the V# a shader would
load for them; the structure is in `gfx10_descriptors.h`, the views are not written; buffer UAVs of every kind are,
section 3.4 "Buffer UAVs", `d3d12_m4_compute_test` and `d3d12_buffer_uav_test`);
1D-array SRVs, other typeless volume formats, multisampled depth/stencil and typeless
color SRVs, and shader-translator lowering for multisample loads. Single-sampled depth-plane views (`R24_UNORM_X8_TYPELESS`,
`R32_FLOAT_X8X24_TYPELESS`) are a T# of the texture's depth format (8_24_UNORM, X24_8_32_FLOAT) through the
depth aspect (the texture's backing format, so X24_8_32_FLOAT for a D24 texture where the layer stores it as
`D32_SFLOAT_S8_UINT`), each on its own family only (the other family's is a null
descriptor, as D3D12 refuses it), and stencil-plane views (`PlaneSlice` 1, as
`X24_TYPELESS_G8_UINT` or `X32_TYPELESS_G8X24_UINT`) are an 8_UINT T# that set 0 shows through the stencil
aspect, in its float-typed sampled arrays like other single-sampled UINT views. Other gaps include descriptors
the game writes itself, which need
the execute-time scan of section 3.4 (only descriptors made through the device are registered). A retail
descriptor heap census (BL-4, measuring the primary 1,245,184-slot console CBV/SRV/UAV heap across submissions
50, 500, and 5000 on RTX 5070 / Windows 11) confirms the slot distribution: 910,244 empty slots; 327,543 single-dword
free-list links (`value = slot_index + 1`, ending at `0xFFFFFFFF`); 261 T# with layer-assigned addresses above 4 GiB;
5,840 V# (3,024 with a null base, 2,816 with a sub-4-GiB arena base); 1,296 slots containing interleaved constant data
(IEEE-754 floats); and zero bare 32-bit GPU addresses. Console descriptors do not store standalone 32-bit pointer words;
buffer and image base addresses are packed into standard 48-bit V# and T# records conforming to the arena layout
(modeled by model-level reference contract suite `tests/test_descriptor_address_translation.py`); the literal S#s of shaders (`literal_samplers` in the index),
which `rt::d3d12::register_samplers` registers without a heap slot but which nothing passes to it yet (the call at
pipeline creation comes with the shader index, #291);
custom border colours without VK_EXT_custom_border_color, and minimum/maximum filters, anisotropy or mirror-once addressing without the Vulkan
feature, each logged and approximated (note: in Vulkan, `VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT` is
per-format; because bindless set 0 hashes `T#` and `S#` independently in the shader, `bind_sampler` cannot know
which image format a sampler will be paired with — on Radeon iGPUs `BC*` formats lack `MINMAX` while uncompressed
formats support it, whereas RTX supports all queried formats); the storage arrays hold 255 images a dimension at the default counts, more
where the device has room (4095 2D and 1023 2D-array on an RTX 5070); nothing writes through a
storage view in a D3D12-level test yet (`vk_bindless_test` goes through the storage bindings at the Vulkan
level). The set is flushed at every `ExecuteCommandLists`. A flush that moves the table to a larger buffer, changes
the index of a live key or relocates one (`BindlessSet::flush_replaces_table()`) does not write in place and does not
wait (D3D-314; D3D-76 first waited for the whole device, which stalled every queue and would have at every texture
release of a game that churns textures). It makes a new generation: new table buffers (the whole table uploaded to
them in that submission's command buffer), a new one-set descriptor pool and set (binding 2 names the new table, and
every descriptor written so far is written again from a CPU shadow of the set, in runs, so unwritten elements stay
unwritten under `partially_bound`), and replay binds that set at the dispatches and draws of the submission
(`ReplayFunctions::bindless_set`; the pipelines' own `bindless_set` only marks that they use one). Submissions made
before keep reading the old set and table, which nothing writes again. The old generation is stamped with the
latest submission ticket and freed at a later `ExecuteCommandLists` (and at device teardown) once
`completed_retire_ticket()` has reached it. Those tickets are the layer's per-submission records of (queue timeline
semaphore, value): the stamp is therefore the latest submitted value of every queue that could have bound the
set, and the free waits until each of those timelines has passed its value; nothing is waited for. The completed
prefix is global and in order, so a queue blocked on a fence only delays the free, never another queue. The stamp
is taken under `tracker_mutex_`, which `execute()` holds from the flush to the end of its submit, so no submission
that bound the old set is unsubmitted when it is read. `LEONIDA_BINDLESS_IDLE_WAIT=1` restores the old wait
(a counted `vkDeviceWaitIdle` before the flush) to compare frame times; nothing else uses it. The cost of the new
path is a full upload of the table and a rewrite of the set's descriptors per replacing flush, where the old one
uploaded the changed slots after the stall; `d3d12_bindless_churn_test` prints both.

The pattern is the one vkd3d-proton uses to retire GPU-visible objects without a device idle (approach only, no
code taken): stamp the object with what every queue has submitted and free it at a later submit once those
submissions have completed. Two further rules come with it here. The submission that carries a new generation's
table upload makes every other queue's next submission wait for its timeline value (`bindless_upload_semaphore_`,
like the storage-image initialisation), because that submission is the first to bind the new set and a queue with no
D3D12 fence on it could otherwise run before the copy; the table's buffers need no ownership transfer because every
D3D12 queue of the device is made in `info.queue_family`. And at most 8 retired generations are kept: past that, the
next flush waits (logged once, with the descriptor lock released) for the submissions up to the oldest one's ticket,
which is a wait for those queues' timelines and not for the device. The commit of a submit's result runs under
`tracker_mutex_`, so a later flush never sees an earlier submit's outcome unrecorded. If the new generation cannot be
made, the submission still goes ahead. A table buffer that finds device-local memory full goes to host-visible
memory (slower lookups, logged once). A freed generation is kept as a spare (at most one) and reused, pool and
buffers, by the next replacing flush, so steady texture churn allocates nothing. Failing that, the flush frees the
generations that have completed and retries; when every submission is done it rewrites the current table in place;
and otherwise it degrades: the pending descriptor writes go in place as on a flush that replaces nothing (they name
new or freed indices), the table keeps what it had, so keys added since read the fallback, and the change waits for
a later flush (logged at the first and each power of two, with what could not be made). It used to fail, and
`ExecuteCommandLists` refused the whole batch, which the title treats as fatal (a native boot died that way in the
story load once device-local memory was full). `vk_bindless_grow_test` covers the churn, the host fallback, the
in-place rewrite and the degraded flush, with allocations failed on purpose.
Two bindless hazards that vkd3d-proton documents for games and that this change does not remove: descriptors
written for resources allocated after the submit that reads them, and an image and a buffer descriptor aliasing one
heap slot (vkd3d-proton PR #3033, https://github.com/HansKristian-Work/vkd3d-proton/pull/3033). Future path, not
implemented: `VK_EXT_descriptor_heap` (https://www.khronos.org/blog/vulkan-introduces-roadmap-2026-and-new-descriptor-heap-extension)
would replace descriptor sets with application-sized heaps.

A typed 3D color target supports default and explicit RTVs. The default covers mip 0; `FirstWSlice` and
`WSize` select the depth slices in an explicit view's mip. `d3d12_clear_test --volume` checks full and
rectangular clears of overlapping slice ranges against both Windows WARP and expected RGBA bytes.

The compute path, end to end:
- `runtime/src/shader_library.h` finds the translated SPIR-V for a shader the game hands to D3D12 (section 3.6).
  It extracts the FSSM program from the DXBC container (DXIL → XDAT entry 0x0D → XBCH → zlib) and looks up its
  SHA-256 as `fssm_sha256` in `spirv/index.json`. Its own inflate, SHA-256 and JSON reader keep it free of
  dependencies (`shader_library_test`).
  - A container with two different 0x0D payloads (195 of 26,503) is read at the last one, as
    `tools/fssm_unpack.py` does by default and so as `fssm_sha256` was computed.
  - Index entries that share a program are all kept (789 groups of 1,182 entries in the corpus). A PSO takes the
    first entry that fits (for a VS: whose input signature the input layout feeds) and declares no set-0
    bindings (the cheaper pipeline: a binding-free entry has no set 0 in its layout).
  - A malformed index (bad numbers or `\u` escapes) is refused with an error instead of throwing into the shim,
    and an integer field outside 0..2^32−1 reads as 0.
- Root signatures use the model already on `main` (`runtime/src/root_signature.h`, `rt::rootsig`): `parse_rts0`
  reads the serialized blob and `pack()` lays out the 32 user-data dwords by the section 3.5 default mapping,
  refusing anything that would spill. `pack_stage()` lays them out in the console layout of one stage (section 3.5,
  D3D-448); the compute bind point uses it.
- **GPU address arena** (section 3.3, when the device has `bufferDeviceAddress`): one host-visible,
  device-addressable buffer, 256 MiB by default (`LEONIDA_ARENA_MB`, at most 4095 MiB, so every VA fits 32-bit
  table pointers).
  - It is also no larger than the device's `maxBufferSize` (Vulkan 1.3) and, counting the buffer's memory
    requirement, `maxMemoryAllocationSize` (Vulkan 1.1), whichever is smaller (`runtime/src/arena_size.h`). When a
    limit cuts the request, one log line names the limit and the size used. `arena_size_test` (CPU) checks the
    decision on synthetic limits; `d3d12_compute_test` checks the arena against the device's limits and a simulated
    32 MiB `maxMemoryAllocationSize`.
  - The RX 6800 (RDNA2, AMD driver 26.11.2, Windows 11, measured 2026-11-22) reports 2 GiB for both, so
    `LEONIDA_ARENA_MB` above 2048 gets a 2048 MiB arena there.
  - Before the cap, that driver did create a 3000 MiB arena. Buffers placed past 2 GiB in it (VA 0x89820000) were
    written by a compute shader and read back correctly, but the validation layer reports the allocation as over
    `maxMemoryAllocationSize`. Nothing relies on this out-of-spec behaviour.
  - The device logs the arena's high-water mark when it is destroyed: the peak end offset against the size, the
    most bytes live at once (by use: buffers, descriptor heaps, shaders, heaps), allocations and refusals (D3D-55).
    `LEONIDA_ARENA_LOG=1` also logs one line per VA handed out, per refused allocation, and per upload or readback
    buffer VA outside the arena.
  - Default-heap buffers are 64 KiB-aligned ranges in it, zeroed through its mapping; their console VA is the offset.
  - Every access to its free-block map is under the arena's mutex, and a walk holds no iterator across a call
    (`runtime/src/arena_blocks.h`): each step reads a block's offset and size, lets its iterator go, looks the next
    block up from the map again, and re-finds a candidate by offset before it cuts it. A map access outside the lock
    makes a concurrent walk step into a node an insert rebalanced away or a release freed — measured once: an access
    violation reading the nil flag of a null map child at 0x19, inside `arena_allocate` for a slot-133 shadow buffer
    (the arena's first block used to be inserted by `create_arena` with no lock, and an insert rewrites child links).
    The arena checks `free + live == size - reserved` on every allocation and release (O(1)) and walks the whole
    block map only when that fails or `LEONIDA_ARENA_CHECK=1` is set; a `double_free` or a release of a range past
    the arena (`outside_arena`) is reported instead of being let into the map, and those lines are rate-limited to
    one per 5 s. `arena_blocks_test` (CPU) covers first fit, coalescing, a title keep-out, the walk cut under it, the
    bad releases and one lock with many threads.
  - Shader-visible descriptor heaps also live in it: the CPU handle is its host mapping and the GPU handle is the VA,
    so descriptor bytes the application writes itself are what shaders load.
  - Upload and readback buffers keep their own memory, and their VA is their device address minus the heap base.
  - Shader programs (FSSM blobs) are copied in as the code base.
- **`CreateRootSignature`** parses the blob with `parse_rts0` and keeps each parameter's place in two layouts, both
  the console's (one dword per table; section 3.5, D3D-448): `pack_stage(Stage::Pixel)` for the graphics bind point
  (from s8; a signature past s31 there keeps `pack()`'s s0 layout, logged) and `pack_stage(Stage::Compute)` for the
  compute one (from s4). It also checks
  `pack_stage(Stage::Mesh)`: a signature past a mesh shader's 16-dword window is logged, and `DispatchMesh` with it
  invalidates the list. A signature neither bind point can place (past s31: the spill layout is open; the
  translated shaders have no below-base spill-pointer candidates) is refused with both reasons; one only a single layout places is created with a log line, and binding it at the other bind point
  invalidates the list. Before
  that it answers as the host runtime does (`compare_invalid_arguments`, "root signature:", RTX 5070 / 621.14):
  `S_FALSE` for a null output whatever the blob, then `E_INVALIDARG` for a node other than node 0 or a DXBC
  container whose size (the dword at +24) is not the size passed.
  **`CreateComputePipelineState`** (PC slot 11, Xbox slot 62 through the proxy) finds the shader by its FSSM
  hash, builds a pipeline with the 144-byte push-constant range and registers it for replay. Shaders that declare
  set-0 bindings get the device's bindless set 0 in their layout and bound at every dispatch (M2); on a device without
  that set they are refused with a log.
- **Command list**: `Set{Compute,Graphics}RootSignature` (a different signature clears the arguments, as D3D12
  leaves them undefined; setting the bound one again keeps them, as D3D12 does), `SetPipelineState`, the 32-bit
  constant, root view and descriptor-table setters, `SetDescriptorHeaps` and `ClearState`. A wrong parameter type,
  out-of-range constants, or an invalid `SetDescriptorHeaps` call makes `Close` return `E_INVALIDARG`.
  - `SetDescriptorHeaps` tracks the currently bound shader-visible `CBV_SRV_UAV` and `SAMPLER` heaps and their
    arena GPU base VAs, holds references to them until `Reset` or list destruction, unbinds omitted types (clearing
    both on `ClearState` and `Reset`), refuses `COPY` lists, > 2 heaps, null arrays/elements, non-shader-visible or
    `RTV`/`DSV`/duplicate-type/foreign-device heaps, and enforces bundle matching against the parent list (`parent`'s
    currently bound heap for each non-null heap the bundle specifies, `d3d12_set_descriptor_heaps`).
  - The list holds a reference to every root signature and pipeline state it is given, until `Reset` or its own
    release. D3D12 lets an application release them right after setting them, so the root layout the setters read
    and the pipeline `ExecuteCommandLists` looks up must outlive that. Keeping them alive while the GPU runs the
    submission stays the application's job, as in D3D12.
  `Dispatch` records the block as it is at that moment. Replay binds the pipeline, pushes s0..s31, the heap base
  and the code base, and dispatches.
- **Graphics pipelines and draws (M1).** `CreateGraphicsPipelineState` (PC slot 10) finds the VS and PS by FSSM hash.
  - **Vertex input:** each input-layout element is matched to the VS input signature by semantic name and index,
    and its location is the element's register (the translator's vertex-fetch rule). An input the layout leaves
    unfed is refused, as D3D12 refuses it.
  - **Fixed-function state:** rasterizer, blend and depth-stencil state map one to one. Viewport, scissor,
    topology, stencil references and the blend factor are dynamic, and so are vertex strides when the pipeline has
    vertex bindings. A pipeline without them (a full-screen pass from `SV_VertexID`) does not declare the stride
    state, since nothing would set it (`VUID-vkCmdDraw-pStrides-04913`).
    `d3d12_state_sweep_test` checks the mapping against the host on the same adapter: 482 full-screen draws over
    every blend factor pair, blend operation, write mask, comparison function and stencil operation, with the stencil
    plane read back, are byte-identical on an RTX 5080 and a Radeon iGPU. Sixteen more build the PSO from a stream
    with `DEPTH_STENCIL2` and different masks per face, front- and back-facing, with one reference or two
    (`OMSetFrontAndBackStencilRef`): 498 in all, byte-identical on an RTX 5070 Ti (driver 617.42), and 6 of the 16
    differ when the back face is given the front face's masks.
  - A triangle-family PSO can draw Xbox rectangle lists. When the input assembler selects topology 17, replay
    selects a geometry-stage variant that emits the fourth corner after vertex shading. It requires
    `geometryShader` and a vertex pipeline without an application geometry stage. A later triangle draw uses the
    original PSO. The translated-draw GPU test switches between both primitives on one PSO and checks every pixel.
  - **Logic ops** come from `RenderTarget[0]`, as in D3D12, and apply to every target (Vulkan has one per
    pipeline; it skips float formats). **`DepthClipEnable = FALSE`** is depth clamping: D3D12 clamps output depth
    to the viewport's range anyway.
  - **Conservative rasterization (Tier 1/2):** the standalone device enables `VK_EXT_conservative_rasterization`
    when available; caller-owned devices must set `VulkanDeviceInfo::conservative_rasterization` only if they
    enabled it. OPTIONS reports at least Tier 1 only with at least eight subpixel precision bits and a finite, nonnegative
    primitive overestimation no greater than `0.5 - 1/512` pixels, leaving half a minimum grid step within the
    Tier 1 uncertainty bound. `ConservativeRaster=ON` uses overestimate mode with zero extra overestimation,
    including unbound-attachment variants. OFF keeps the ordinary pipeline. Missing support, unknown modes,
    wireframe and non-triangle PSOs are refused (a mesh PSO needs no topology type: its module has to output
    triangles, `conservative_raster::mesh_outputs_triangles`); conservative TIR (`ForcedSampleCount != 0`) remains `E_NOTIMPL`.
    Tier 2 additionally requires `degenerateTrianglesRasterized` and primitive overestimation plus half a
    device subpixel step no greater than 1/256 pixel. Other eligible devices retain Tier 1. The standalone
    device does not expose Tier 3: the Xbox `SV_InnerCoverage` input mapping has not been confirmed. See
    [Microsoft's rules](https://learn.microsoft.com/en-us/windows/win32/direct3d12/conservative-rasterization)
    and [Vulkan's properties](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceConservativeRasterizationPropertiesEXT.html).
    `d3d12_conservative_tier2_test` checks the tighter geometric uncertainty bound, post-snap and exact
    degenerates with distinct indices, provoking-vertex colour, constant depth, back-facing classification,
    culling, indexed permutations, triangle strips, scissor and unbound-attachment variants. On RX 5700 XT
    (Windows driver 32.0.21045.5002), 16 tight-bound scenes and 480 degenerate draws pass across Vulkan and native
    AMD D3D12. The WARP comparison omits only its pixel-corner fixture: WARP drops that tiny triangle even though
    Vulkan and native AMD cover it. `--warp-corner` retains the strict failing software-reference reproducer;
    Vulkan always checks the corner. Variable `SV_Position.z` is not a provoking-vertex oracle: both native
    references and Vulkan exhibit extrapolation for screen-space degenerates. This test checks constant depth
    and separately varying colour, and does not claim a depth-buffer readback or variable-depth conformance.
    `d3d12_conservative_raster_test` checks geometric required/forbidden coverage, subpixel/sliver and degenerate
    triangles, culling, scissor, 1x/4x MSAA, variants, feature reporting and refusals. Uncertain edge pixels are
    allowed to differ. On RX 5700 XT (Windows 32.0.21045.5002), 48 scenario pairs pass on Vulkan and native AMD;
    24 single-sample pairs pass on WARP. WARP's 4x subpixel fixture covers outside the geometric bound; it remains
    reproducible with `--warp-msaa4`, separately from CTest's 1x software comparison. Vulkan's 4x oracle remains
    strict. On RTX 3060 (Windows 11, NVIDIA 610.88), 48 pairs pass on Vulkan and native NVIDIA. The oracle judges
    the layer alone. A native render that fails it is printed as "host not comparable", with its normal and
    conservative pixel counts, and that scenario isn't compared with the host. For example, Windows 10's WARP
    (`d3d10warp.dll` 10.0.19041, the host CTest selects with `LEONIDA_HOST_ADAPTER=warp`) misses pixel (4,4) of
    scene 4 with conservative rasterization on: seen on an RTX 5060 Ti and an RTX 2060 machine, both of which pass
    with their NVIDIA GPU as the host (TEST-907).
    The GPU test needs the current graphics path's 144-byte push-constant support; CPU capability checks
    do not need a GPU. No game-pass usage is claimed by these synthetic tests.
    `d3d12_conservative_tier3_reference` is a native D3D12 reference probe, not Tier 3 support in the Vulkan layer.
    **Explicit InnerCoverage library contract (experimental):** a caller-owned device can set
    `VulkanDeviceInfo::inner_coverage_abi` only for a shader library whose InnerCoverage input is already lowered
    to the boolean SPIR-V `FullyCoveredEXT` built-in (false maps to uint 0, true to uint 1). Tier 3 then also
    requires all Tier 2 properties and `fullyCoveredFragmentShaderInputVariable`. The default remains false,
    including for the standalone device; this is not an inferred FSSM/VGPR layout or automatic game-shader support.
    Graphics PSOs inspect the actual SPIR-V capability, even if index capability metadata is absent, and refuse
    `FragmentFullyCoveredEXT` with `E_NOTIMPL` before shader-module creation unless the contract and device support
    are present. A module using `PostDepthCoverage` additionally requires
    `conservativeRasterizationPostDepthCoverage`. The tests use explicit synthetic GLSL; no new translator rule
    or retail shader coverage is claimed.
    `inner_coverage_oracle` checks false positives, missed interior values and missing invocations on the CPU.
    `d3d12_inner_coverage_guards` checks that neither opt-in nor metadata omission can bypass missing support.
    `d3d12_inner_coverage_native` checks 48 native D3D12 scenarios on the matching adapter: six triangles,
    1x/4x MSAA, scissor and unbound-target variants. These pass on RX 5700 XT (Windows 32.0.21045.5002) with
    Vulkan validation active. `d3d12_conservative_tier3` runs the same 48 scenario pairs on Vulkan and native D3D12
    when Vulkan has the property and native D3D12 has Tier 3; it skips on this RX 5700 XT. The positive Vulkan
    path still requires independent execution on a capable GPU. InnerCoverage with conservative rasterization
    off is undefined in D3D12 and is not compared. Edge pixels within the uncertainty region are not required
    to match byte-for-byte; both backends must independently satisfy the geometric guarantees.
    To validate the explicit path, configure with Vulkan SDK/glslang available, build
    `d3d12_conservative_tier3_test`, select an adapter with `LEONIDA_VK_DEVICE` if needed, and run
    `ctest --test-dir runtime/build -C RelWithDebInfo -R "^(inner_coverage_oracle|d3d12_inner_coverage_guards|d3d12_inner_coverage_native|d3d12_conservative_tier3)$" -V`.
    Check that the positive test prints 48 Vulkan/native paired scenarios rather than `Skipped`.
    `d3d12_conservative_tier3_reference` remains a native D3D12 reference probe; it does not opt in to that Vulkan contract.
    It reports the selected Vulkan device's `fullyCoveredFragmentShaderInputVariable`, matches that device to its
    native D3D12 adapter by LUID, then checks the adapter's Tier 3 `SV_InnerCoverage` output on four synthetic
    triangles. On Windows with Visual Studio 2022 and Windows SDK 10.0.26100 or newer, run
    `.\scripts\run_conservative_tier3_reference.ps1` from the repository root; by default this uses Vulkan's default
    adapter. Pass `-Device <name substring>` to select a specific one when multiple GPUs are present. It builds only
    this test, needs no glslang, and fetches the pinned Vulkan headers only if none are installed. The verbose CTest
    output reports the capability and result; it skips if the selected device lacks a matching native D3D12 adapter
    or Tier 3.
  - **Formats:** dynamic rendering takes the RTV and DSV formats. A draw that leaves a slot unbound for which the
    PSO names a format (D3D12 allows it and discards those writes) uses a variant of the pipeline with that format
    undefined, built on first use and kept with the PSO. Bit i of the variant key is colour slot i, and bit 8 is
    depth/stencil.
  - **Instance step rate 0:** every instance reads element 0, done with a stride of 0 for that slot at draw time.
  - **Features:** independent blend state, wireframe fill, a depth-bias clamp, SRC1 blend factors, logic ops and
    disabled depth clipping need `independentBlend`, `fillModeNonSolid`, `depthBiasClamp`, `dualSrcBlend`, `logicOp`
    and `depthClamp`. The standalone device enables those the GPU has and reports them in
    `VulkanDeviceInfo::features`. A PSO that needs a missing one is refused with a log.
  - **Shader features:** the standalone device also enables `shaderInt64` (declared by 25,803 of 26,503 index
    entries), `shaderFloat64`, `shaderInt16`, `shaderClipDistance` and `shaderCullDistance` where the GPU has them.
    Without one, `vkCreateShaderModule` reports the capability (`VUID-VkShaderModuleCreateInfo-pCode-08740`). The
    index's `features` list is not read yet, so a shader that needs a feature the GPU lacks is not refused up front.
  - **Stores and atomics outside compute:** a pixel shader that stores to an image or does an image atomic declares
    that storage image without `NonWritable` (of the 1,038 translated 1.0.2.1 modules that contain an image atomic,
    counted as modules with an `OpImageTexelPointer` and split by the index's stage, 1,018 are pixel shaders and 20
    compute),
    which Vulkan allows only with `fragmentStoresAndAtomics` (`VUID-RuntimeSpirv-NonWritable-06340`); a vertex or
    geometry shader needs `vertexPipelineStoresAndAtomics` (`-06341`). The standalone device enables both where the
    GPU has them. A graphics PSO whose stage declares writable storage (read off the module's declarations,
    `src/spirv_stores.h`) on a device without the feature is refused with `E_NOTIMPL` and a log naming the shader.
    `d3d12_stores_atomics_test` checks both under the validation layer.
  - **Geometry shaders:** `desc.GS` is the translator's Vulkan geometry stage (docs/design.md, "Geometry
    shaders": triangles in, a triangle strip out) between the VS and PS. It needs `geometryShader`, which the
    standalone device enables where the GPU has it and reports in `VulkanDeviceInfo::features`; without it such a
    PSO is `E_NOTIMPL`. The PSO's topology type must be `TRIANGLE`, the GS's input (`E_INVALIDARG` otherwise). The
    push-constant range names the geometry stage only on such a device (`d3d12_geometry_test`).
  - **Refused with a log:** hull and domain stages, stream output, MSAA, set-0 bindings, and two stages
    that both read their code base (question 14, section 5.2). More than eight render targets is `E_INVALIDARG`.
  - **Push constants:** both PSO paths declare the 144-byte block as one range. Its stage flags are the whole
    stage table the graphics path accepts, and the replay pushes with the same flags. A device whose
    `maxPushConstantsSize` is below 144 (#224's `push_constants_need_fallback`) has every PSO refused, with the
    limit in the log, until the uniform-buffer tail exists.

  The command list keeps the input-assembler, viewport, scissor, render-target (read at `OMSetRenderTargets`, as
  D3D12 does), blend-factor and stencil-reference state, including CommandList8's front/back references. Each
  `DrawInstanced` / `DrawIndexedInstanced` records a copy of that state. A draw with no viewport (never set, or
  unbound with `RSSetViewports(0, …)`) or an empty one rasterizes nothing in D3D12. Such a draw is not recorded,
  since Vulkan forbids a 0×0 viewport.

  At replay:
  - **Buffers:** vertex and index views resolve to buffers by VA (arena or upload). A view that runs past its
    buffer is cut at the buffer's end. An unbound vertex slot, or an indexed draw with no index buffer, reads the
    arena's reserved first 64 KiB, which is kept zero (stride 0), so it gets the zeros D3D12 gives.
  - **Attachments:** they move to attachment layouts through the tracker, and the draw runs in a
    dynamic-rendering scope. Its layer count is the smallest layer count of the bound views.
  - **Viewport:** it is flipped (negative height), because translated vertex shaders write D3D clip coordinates;
    front-face winding then matches D3D12's.
  - Only the first viewport and scissor are used, and primitive restart is not modelled yet.

  **Resources and heaps:**
  - A buffer of 0 bytes, or one whose size wraps when aligned, is `E_INVALIDARG`. One the arena cannot hold is
    `E_OUTOFMEMORY`, and so is a shader-visible descriptor heap the arena cannot hold (in host memory its GPU
    handle would be 0).
  - If the arena cannot be created, its buffer is released, so other buffers keep their own memory and get no VA.
  - With `VK_EXT_conditional_rendering`, the arena is created with the conditional-rendering usage, and
    `SetPredication` on a default-heap buffer reads at the buffer's offset in the arena plus the call's offset.
  - `CreateHeap` reserves the requested size in the arena at 64 KiB, and a buffer placed in it gets the heap's
    place plus the placed offset as its console VA. A placed texture has memory of its own (a range of a
    shared block or its own `VkDeviceMemory`, section 3.3) and a null VA,
    1D, 2D and 3D alike (arrays and mips included), and a dimension outside the enum is `E_INVALIDARG`, as on the
    host (RTX 2080 Ti and WARP, `d3d12_placed_dimensions_test`);
    tiled (`64KB_UNDEFINED_SWIZZLE` and Xbox `0x101`) 2D/3D placed textures are backed the same way with
    tile-multiple `GetResourceAllocationInfo`, `GetResourceTiling`, and host-matched `E_INVALIDARG` checks
    (D3D-191). `CreatePlacedResource2`'s castable-format list reaches the placed texture's image as the committed
    path's does (D3D-90). `CreateHeap` refuses with `E_INVALIDARG`, as the host does (RX 5700 and WARP, D3D-238),
    an alignment other than 0, 64 KiB and 4 MiB, a heap type outside the enum, a custom heap without a CPU page
    property and memory pool or a non-custom heap with either, and a heap with all three `DENY_*` flags. It also
    refuses `ALLOW_DISPLAY` (committed resources only), as an RTX 4070 SUPER and a UHD 770 host do. It accepts
    `HARDWARE_PROTECTED`, as those hosts do, though the `D3D12_HEAP_FLAGS` documentation calls it unsupported.
    (D3D-191). Multisampled 2D placed textures (`UNKNOWN` and `4x` tiled) share that backing with host-matched heap
    and offset alignment checks (`4 MiB` vs `64 KiB`, `D3D-222`). `CreatePlacedResource2`'s castable-format list
    reaches the placed texture's image as the committed path's does (D3D-90).

Known gaps: set 0 in pipelines (the device keeps the bindless set, but no pipeline declares or binds it yet) and
therefore every translated shader that declares bindings; tessellation, geometry and mesh pipelines,
multisampled pipelines, several viewports, primitive restart; `requiredSubgroupSize = 64` for wave64 compute;
the game's present entry point (`PresentX`; the DXGI factory/swapchain and the presenter above are what it
- **Subgroup size (compute and graphics PSOs).** A translated shader assumes one subgroup size, and a GPU whose
  default differs runs it inexact: a wave64 shader reads lanes 32-63 of a 32-wide subgroup, and a wave32 pixel
  shader on a 64-wide default reads the other half's lanes (AnyPS5 #487 on RDNA4). `runtime/src/subgroup_size.h`
  decides, per stage, from the shader's index entry and what the device offers; the PSO chains the result.
  - **Index fields:** `wave64`, `subgroup_size` (64 for wave64, 32 otherwise, `null` when emulated) and
    `wave64_exchange` (present: translated with `--wave64-emulate`, exact on any size).
  - **Device:** `subgroupSizeControl` and `computeFullSubgroups` are enabled where supported;
    `minSubgroupSize`, `maxSubgroupSize`, `maxComputeWorkgroupSubgroups`, `requiredSubgroupSizeStages` (from
    `VkPhysicalDeviceSubgroupSizeControlProperties`) and the default `subgroupSize` are kept in
    `VulkanDeviceInfo::subgroup` and printed at device creation.
  - **Emulated shaders:** nothing is required.
  - **Native shaders:** `VkPipelineShaderStageRequiredSubgroupSizeCreateInfo{subgroup_size}` when the feature is
    enabled, the stage is in `requiredSubgroupSizeStages`, the size is a power of two in [min, max] and, for
    compute and mesh, the workgroup fits in size x `maxComputeWorkgroupSubgroups` (VUID-VkPipelineShaderStageCreateInfo-pNext-02755
    and -02756, VUID-VkPipelineShaderStageRequiredSubgroupSizeCreateInfo-requiredSubgroupSize-02760..02762).
    A compute stage also gets `VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT` when
    `computeFullSubgroups` is enabled and local size X is a multiple of the size (-flags-02785 for the feature,
    -flags-08988 for the stage, -pNext-02757 for X). Wave32 shaders ask for 32 the same way. A device whose only size is the wanted one (NVIDIA,
    wave32) needs no chain. Where the size cannot be required (stage not listed, feature off) but the device default is
    the wanted one, the shader works as it is (`device_default`; nothing guarantees the default, so it is not logged
    as inexact); any other size is `unavailable` and logged.
  - **Which stages can be required:** it is the driver's list, not a fixed rule, and AMD's Windows driver gives
    different lists for different GPUs, on the same 26.11.2 release too:
    - Radeon 880M (RDNA3.5, gfx1150), 26.11.2: FRAGMENT | COMPUTE | TASK | MESH, sizes 32..64, so wave64 and wave32
      pixel shaders run exact there.
    - RX 6800 (RDNA2 like the console; gfx1030), 26.11.2 (Windows driver 32.0.21045.29003), measured 2026-11-22:
      COMPUTE only, sizes 32..64, default 64 (`vulkaninfo`; the device log says `requirable in stages 0x20`). No
      size can be requested for pixel or mesh shaders (`vk_harness` chains none for `VKH_SUBGROUP_SIZE`), and at the
      default they run 64-wide: a GLSL probe pixel shader drawn by `vk_harness` counts 64 live invocations per
      subgroup (`subgroupBallotBitCount`) on a full-screen draw (SPIR-V 1.5 and 1.6), and `ms_w64x_ballot` (both
      halves of a wave64) passes natively. For the translated SPIR-V 1.5 modules, made without
      `ALLOW_VARYING_SUBGROUP_SIZE`, the specification fixes `SubgroupSize` at that default ("Varying Subgroup
      Size"); only the driver's default could change. So wave64 pixel and mesh shaders work at the default
      (`device_default`; the layer has no mesh PSO yet), and a wave32 pixel shader is `unavailable`: two of its
      waves share a subgroup. `ps_bitmask_perm` shows it: lanes 0-31 exact, while the
      other 2,048 of 4,096 pixels read `v_mbcnt_lo` = 32 instead of their lane (the fixture's check judges lanes
      0-31 only). Of the 1,127 wave32 pixel shaders in the corpus, 1,010 contain a `v_readlane`/`v_writelane`/
      `v_readfirstlane`, 146 a `v_mbcnt`, 105 an `s_bcnt1`/`s_ff1` of a lane mask, and all 1,127 a branch on EXEC
      or VCC or an SCC vote on a mask, which the translator decides by a subgroup vote (117 have only those). 926
      also have `v_permlanex16` and row DPP and 826 quad ops, which stay inside a 32-lane half as translated
      (`tools/wave32_pixel_scan.py`; counts of candidates, not verdicts).
    - Ryzen 7 9800X3D iGPU (RDNA2, gfx1036), Windows driver 32.0.21045.5002: COMPUTE only, sizes 32..64; RX 9070 XT
      (RDNA4, gfx1201), Windows driver 32.0.31041.1004: FRAGMENT | COMPUTE | TASK | MESH, sizes 32..64
      (@AC-Storm-YT, `vulkaninfoSDK` from Vulkan SDK 1.4.363.1,
      [#313](https://github.com/ItsNotRexed/Leonida/issues/313#issuecomment-7086133354)). The same two lists on
      AMD 26.11.2 (Windows drivers 32.0.21045.29003 and 32.0.32029.2010), where `ps_bitmask_perm` also shows the
      live fragment width: the gfx1036 iGPU gets the RX 6800's split (2,048 of 4,096 pixels read lane 32), the
      RX 9070 XT is exact on all 4,096 (@AC-Storm-YT,
      [#3982](https://github.com/ItsNotRexed/Leonida/pull/3982#issuecomment-7088523396) and #4225).

    Current Mesa RADV on GFX10+ lists COMPUTE | FRAGMENT | task/mesh (Mesa main at 152b9cde,
    `src/amd/vulkan/radv_physical_device.c`, `requiredSubgroupSizeStages`; older Mesa reported compute only), so
    pixel shaders run exact there. NVIDIA lists every stage but only size 32, so a native wave64 shader cannot get 64
    there. Vertex and geometry stages are in neither list.
  - **A size the device cannot give** is logged once per stage with the first shader's name (and again at 10, 100,
    1000 shaders of that stage): `[rt] d3d12: <name> (<stage>) is a 64-wide shader but this device cannot require that
    subgroup size ...`. For compute and mesh the fix is the build, `--wave64-emulate` (`make_game.py
    --wave64-emulate auto` picks it for this GPU); a wave64 vertex, geometry or pixel shader has no emulation yet
    (the ~10,000 wave64 pixel shaders are the open case on NVIDIA, SHADER-10). A wave64 VS/PS/GS whose index entry
    carries `wave64_graphics` tier A or B is not logged as inexact: the line says it runs at the device's size and
    that the translator's partition-invariance analysis puts it in that tier, since cutting its wave into two 32-lane
    halves does not change its results (tier C and entries without the analysis keep the conservative line).
  - **Test:** `subgroup_size_test` (CPU) checks the decision against AMD-like, NVIDIA-like and old-RADV
    properties and the resulting create-info chain; `d3d12_subgroup_test` (GPU) runs `cs_w64x_readlane` natively
    and with `--wave64-emulate`, and `cs_wqm_w32`, through D3D12 PSOs and compares the read-back words.

Known gaps: set 0 in pipelines (the device keeps the bindless set, but no pipeline declares or binds it yet) and
therefore every translated shader that declares bindings; tessellation, geometry and mesh pipelines,
multisampled pipelines, several viewports, primitive restart; subgroup size for mesh pipelines (the decision covers them, no mesh PSO exists to chain it into), and for wave64 vertex, geometry and, on a GPU that cannot require 64 for pixel shaders (NVIDIA), pixel shaders, which have no emulation (SHADER-10); wave32 pixel shaders on a GPU that runs the fragment stage 64-wide and cannot require 32 for it (RDNA2 under AMD's Windows driver, "Subgroup size" above), where two waves share a subgroup;
the game's present entry point (DXGI factory/swapchain or `PresentX`; the presenter above is what either
calls), HDR and fullscreen; buffer and constant-buffer views (UAV clears read the descriptor bytes, so they do
not wait on view creation), typeless volume RTVs, depth-stencil resolves to a destination offset other than
the source rectangle's;
default-heap buffers are not zero-filled yet; a fence cannot be signalled to a lower value (Vulkan timelines only
increase; such a signal is reported and dropped); a CPU `Signal` above a queue `Signal` the GPU has not reached
yet is held until it has (`vkSignalSemaphore` must stay below every pending signal,
`VUID-VkSemaphoreSignalInfo-value-03259`), so `GetCompletedValue` shows it only then, and a fence released
while it holds one drops it; instance step rates above 1 (they need vertex attribute
divisors); read-only DSVs (the depth image always moves to `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`, which matters once
depth SRVs are sampled next to a read-only DSV, M2); the index buffer is bound without its view's size
(`vkCmdBindIndexBuffer2`, Vulkan 1.4, takes one).

Known gaps found in review, not fixed here:
- **Queues:** every D3D12 queue submits to one `VkQueue`, which runs batches in order. So a queue that waits on a
  fence another queue signals later (`B->Wait(f, n)` recorded before `A->Signal(f, n)`) holds back that signal,
  and the GPU never progresses (woreksami's probe). Queues on separate Vulkan queues (#306 for copy and compute)
  or holding back a batch until its wait value is submitted would fix it.
- **Lookups:** `buffer_at` searches every resource for each vertex and index view; an ordered map by VA would do
  when it shows.
- **Fences:** a CPU `ID3D12Fence::Signal` checks only the semaphore's current value, so it can signal below a GPU
  signal that is still pending (`VUID-VkSemaphoreSignalInfo-value-03258`).

Build `runtime/` (Vulkan SDK and Python 3 on `PATH`) and run `ctest --test-dir <build> --output-on-failure` for the
CPU tests:
- `command_list_test` covers the recording rules.
- `d3d12_state_map_test` covers both state tables, subresource and barrier-range decoding, layout tracking, the
  subresource ranges `DiscardResource` prepares, Resizable BAR detection, and the memory type order on a no-ReBAR
  heap layout. On Windows it also checks every D3D12 constant the IR copies against the SDK's `d3d12.h`.
- `gfx10_descriptors_test` covers the T#, S# and V#: every field alone at its bit position, round trips, what
  decoding refuses and ignores, the T#s the image test shaders build, sampler words as radv writes them, and the
  format table. `tests/test_gfx10_descriptors.py` compares that table with `tools/gfx10_formats.json`.
- `subgroup_size_test` covers the subgroup-size decision of section 3.2 without a GPU: synthetic index entries
  (native and emulated wave64, wave32; compute, pixel, mesh, vertex) against AMD-like (32..64, FRAGMENT | COMPUTE |
  TASK | MESH), NVIDIA-like (32 only, every stage) and old-RADV (COMPUTE only) properties, the limits (feature off,
  `maxComputeWorkgroupSubgroups`, a size that is not a power of two), and the create-info chain.

The GPU tests read their results back. With `LEONIDA_VK_VALIDATION=1` they run under `VK_LAYER_KHRONOS_validation`,
and any validation error fails them.
- `vk_command_replay_test`, part A: only Vulkan 1.0 functions and no tracker.
- `vk_command_replay_test`, part B: rectangle clears, a depth clear while the stencil plane is read-only, a promoted
  texture, and enhanced barriers including a discard.
- `vk_command_replay_test`, part C: a 4x4x4 volume uploaded from a footprint, copied as a 2x2x2 block and whole,
  and read back; a 4x multisampled target cleared and resolved; and the copies and resolves that must be refused
  (a z range past the volume or its footprint, a footprint past the buffer, a resolve from a single-sampled
  texture or into another size, a multisampled texture copied to a footprint or to another sample count).
- `vk_command_replay_test`, part D: every D3D12 sync bit against every access bit (360 pairs), each as a global,
  a buffer and a texture barrier. Under the validation layer none may be rejected; before
  `vulkan_barrier_scope` there were 462 validation errors on Intel(R) Graphics.
- `vk_command_replay_test`, part F (D3D-26): `WriteBufferImmediate` in all three modes at console VAs, one of them
  in a binding that is a range inside another buffer (as arena resources are); UINT and UINT64 atomic copies
  ordered between the writes so that a missing barrier would copy a stale or a later value; a buffer discard
  (kept), a texture cleared, discarded and cleared again, and a rectangle discard (kept); and the lists that must
  be refused (an address past a binding or in none, no `vkCmdUpdateBuffer`, a copy or dependent range past its
  buffer, an unknown dependent resource, a subresource the resource does not have, and an explicit discard count of
  `UINT_MAX`, which is a range and not the whole resource).
- `vk_command_replay_test`, part I: a 4x D32 `MIN` resolve of a rectangle; the texels around it must keep the
  destination's clear value (MoltenVK overwrote all 28 of them before the scratch copy). It is skipped where the
  device has no `MIN` depth resolve (llvmpipe), and without `copy_scratch` the list is refused.
- `d3d12_vulkan_device_test` also runs both atomic copies and the discards through COM, and checks that
  `WriteBufferImmediate` is refused at execution while no buffer has a GPU virtual address on `main`.
- `vk_bindless_test` (run by `runtime/test/shaders/check.py` and `scripts/run_gpu_tests.py`): the translator's
  own lookup against the table and the set. `ps_tex.asm`, a hand-written RDNA2 pixel shader built by `build.py`,
  loads a T# and an S# from a descriptor table and samples. The test draws the gradient with the T# at the fourth
  slot probed, the fallback after the key is erased, the gradient again after the table has grown into a new
  buffer, another view at the same index, and the fallback after removal, with and without the
  descriptor-indexing flags. Last, it draws through a 2D image past 5000 others and a sampler past 2100 others.
- `vk_sampler_test` (run the same way): the S# and T# mip fields as `gfx10_descriptors.h` maps them. `ps_ssharp.asm`
  loads a T# and an S# from the heap and samples a 128x128 two-level texture twice, with `image_sample_lz` (LOD 0)
  and `image_sample` (LOD 1 at this size). The sampler is `vulkan_sampler(decode(S#))` and the view's levels come
  from `decode(T#)`. `check.py`'s `SAMPLER_CASES` cover a point mip filter, MAX_LOD 0, MIN_LOD 1, LOD_BIAS ±1,
  MIP_FILTER_NONE with and without MIN_LOD, and a T# whose BASE_LEVEL is 1. Each pixel is judged against levels
  worked out from the S# and T# bits in Python, independently of the header. On Vulkan, the sampler's minLod and
  mipLodBias apply to an explicit LOD as well. The T#'s MIN_LOD (D3D12's ResourceMinLODClamp) is not covered by
  this test.
- **Measured on RDNA2** (SHADER-539): `tools/sampler_lod_probe.py --oracle hw` samples a linear 64x64 R32_SFLOAT
  texture with seven levels at its top-left texel. The texture memory is a dword ramp, so each result names the
  level it was read from, and a calibration row per level finds the level starts. It runs 43 rows of
  `image_sample_l` and `image_sample_lz` on a gfx1036 (Radeon iGPU, Windows 11, AMD 26.10.1, `tools/hw_oracle`
  HIP launcher, rocm-sdk-core 10.0.0 clang). wave32 and wave64 gave the same level in every row, and three runs
  were identical.
  - **The S# fields apply to explicit LODs.** LOD_BIAS (±), MIN_LOD and MAX_LOD apply to `image_sample_l`'s
    explicit LOD and to `image_sample_lz`'s LOD 0, bias before the clamp, as radv assumes. MIP_FILTER none ignores
    them and reads the view's base level.
  - **The T#'s MIN_LOD applies too.** It counts from the resource's level 0, not the view's BASE_LEVEL: BASE_LEVEL
    1 with MIN_LOD 2 reads level 2.
  - **Where the hardware is not Vulkan:**
    - **Rounding.** The point mip filter's level is floor(lambda + 0.5), so an exact .5 rounds up: LOD 0.5, 1.5 and
      2.5 read levels 1, 2 and 3, and so do S# MIN_LOD 1.5 or MAX_LOD 1.5 and LOD 1 with bias +0.5. Vulkan's
      nearest is ceil(lambda + 0.5) - 1, which reads 0, 1 and 2.
    - **The T#'s MIN_LOD clamps the still fractional LOD before that rounding.** MIN_LOD 2.25, 2.5 and 2.75 at
      LOD 0 read levels 2, 3 and 3. `VK_EXT_image_view_min_lod` raises the integer level to floor(minLod), which
      gives 2, 2 and 2.
    - **MIP_FILTER none with the T#'s MIN_LOD.** The level is the base raised to floor(MIN_LOD): 2.5 reads level 2,
      the same as Vulkan.

    So a host whose Vulkan driver follows the spec's nearest rule reads one level lower than the console on 8 of
    the 43 rows: an exact half-level LOD, or a fractional ResourceMinLODClamp. LOD 1.49 reads level 1 on both. The
    AMD driver does not follow it. Through the runtime's own path (`vk_sampler_test`, `vulkan_sampler(decode(S#))`),
    an S# LOD_BIAS of +0.5 on `image_sample_lz` reads level 1, like the console, on the same gfx1036 and on an RX 7800
    XT (both AMD 26.10.1). So on AMD hosts the S# case matches. On other hosts the result splits: Mesa llvmpipe
    (Mesa 23.2.1, LLVM 15.0.7) and an Apple M2 (macOS 26.4, MoltenVK 1.4.2) follow the spec's nearest rule — the
    same `vk_sampler_test` S# with LOD_BIAS +0.5 reads level 0 (on the M2 the same explicit `image_sample_lz` with
    bias 0 and -0.5 also reads level 0; llvmpipe switches to level 1 at 129/256, for LOD_BIAS and MIN_LOD alike),
    while NVIDIA reads level 1, like the console: an RTX 5070 (driver 610.88) switches at exactly 128/256. Intel
    is not measured here.
  - **Layout.** gfx1036 lays a linear mip chain out smallest level first, each row padded to 256 bytes. Levels 6..0
    of the 64-wide R32 chain start at dwords 0, 64, 192, 448, 960, 1984 and 4032.

  `tests/test_sampler_lod_probe.py` checks the descriptor fields against `check.py` and `gfx10_descriptors.h`, and
  the model against these levels, without a GPU.
- `d3d12_com_test`: drives the COM objects through their vtables. It also checks that a queue `Wait` on a fence
  the CPU has not signalled holds the GPU back.
- `dxgi_swapchain_test`: creates a Win32 HWND and exercises `rt::d3d12::create_dxgi_factory`,
  `IDXGIFactory4::CreateSwapChainForHwnd`, `IDXGISwapChain3::GetCurrentBackBufferIndex`, `GetBuffer` + `GetDesc()`,
  backbuffer `CreateRenderTargetView` + clear + readback verification + `Present`, and `ResizeBuffers` with 0
  validation errors.
- `d3d12_stateful_indirect_pixels`, `d3d12_stateful_indirect_multi_pixels` and
  `d3d12_stateful_indirect_counted_pixels`: positive D3D12-to-Vulkan graphics tests. Compare every readback
  pixel with both a direct-draw reference and a known pattern for each draw kind; the counted variant fills
  its argument and count buffers by GPU copy before use.
- `d3d12_indirect_rt_test`: validates `ID3D12Device::CreateCommandSignature` + `ExecuteIndirect` with a GPU
  `CountBuffer` (`vkCmdDrawIndirectCount`, `count = 0` skipped and `count = 1` rendered), a draw whose record asks
  for four billion instances (clamped on the GPU, section 4.7), and a native Vulkan counted-draw reference over the
  same suballocated argument/count offsets (`count = 0` leaves the clear colour, `count = 1` renders; it is
  independent of the D3D12 command replay); optional
  `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` (`GetRaytracingAccelerationStructurePrebuildInfo`, BLAS + TLAS
  `BuildRaytracingAccelerationStructure`, and a `RayQueryKHR` compute shader verifying hit vs. miss; clean SKIP on
  GPUs without RT).
- `indirect_clamp_test`: the `ExecuteIndirect` argument clamp pass itself (section 4.7), with no game files: every
  argument kind (draw, draw indexed, dispatch, mesh dispatch) and the counted variants, in-range records
  bit-identical, garbage records bounded, records and counts past `max_command_count` empty, and the per-batch work
  budget saturating; dispatch records bounded by the device's `maxComputeWorkGroupCount` (70 000 x 1 x 1 kept and run
  where the device allows it, cut and counted where it does not); clean SKIP without a Vulkan device.
- `d3d12_gcl8_test`: the public `ID3D12GraphicsCommandList8` surface: `QueryInterface` for its IID,
  `OMSetFrontAndBackStencilRef(1, 2)` with the recorded values, and the reference count around the query. It
  creates only an allocator and a list, so it needs no GPU or Vulkan loader.
- `d3d12_depth_resolve_test`: depth and stencil `ResolveSubresourceRegion` of a 4x D32 or D24S8 target cleared with
  three overlapping values, whole and by rectangle, compared with the PC runtime on the layer's own adapter and WARP;
  the refused calls must fail `Close` and leave the destination as cleared.
- `d3d12_vulkan_device_test`: the Vulkan device behind the Xbox-ABI proxy, called the way the game will call it.
  It creates heaps through slot 14 and reads their start handles in RAX, creates a render target through slot 76
  with the Xbox layout word, then creates views, records clears and barriers, executes and waits on a fence.
  Results come back through D3D12 itself (`CopyTextureRegion` into a readback heap, then `Map`): the render
  target, the depth and stencil planes, and a never-written texture (which must be zero). A second list uploads a
  pattern and copies it on: whole into a texture created in `COMMON`, a boxed part into a smaller texture, a BC1
  round trip, a 1D texture and a 1D-array layer, and a buffer region. A 4x multisampled render target is cleared through a `TEXTURE2DMS` view and
  resolved into a texture that is read back; the multisampled shapes D3D12 refuses are refused, and so is a
  resolve from a single-sampled texture. `GetCopyableFootprints` is compared with the PC runtime's, and a short
  run repeats on a standalone device, the path `LEONIDA_D3D12=vulkan` takes. That standalone run also covers
  heaps and placed resources: a castable list on a placed texture is cleared through an RTV in a cast format and
  read back against the host's answer, and refused lists match the host's `E_INVALIDARG` (D3D-90).
  The comparison tests run against a PC D3D12 device: the layer's own adapter (the default adapter when the Vulkan
  device has no LUID or that adapter gives no D3D12 device), or the one named by
  `LEONIDA_HOST_ADAPTER` (`warp`, or part of an adapter's name; the adapter used is printed). `d3d12_vulkan_device`
  picks it through `runtime/test/d3d12_copy_compare.h`; `d3d12_copy_xfmt`, `d3d12_depth_resolve`,
  `d3d12_fence_events`, `d3d12_indirect_zero`, `d3d12_node_mask`, `d3d12_query_rules` and `d3d12_resource_desc`,
  which also compare with WARP, through `runtime/test/host_adapter.h`. Some answers are the
  driver's choice, so pick the host side when two adapters disagree: `GetResourceTiling` splits standard and packed
  mips differently on an NVIDIA, an AMD and a WARP host, and the test accepts any split.
  - Copies against the host (D3D-46, `d3d12_copy_compare.h`): the same scenario runs on the layer and on the host and
    the bytes read back are compared; a failing case prints its name, the region (subresource, mip, slice, plane)
    and the first differing byte. Every case uploads deterministic bytes into a texture and then runs
    `CopyResource`, `CopyTextureRegion` texture to texture (an inner box to the origin, and the origin box into
    another array slice at an offset), footprint to texture (an inner box at an offset) and texture to footprint
    (an inner box at an offset into a readback buffer prefilled with `0xcd`), and reads every subresource back.
    Formats: R8, R8G8 SNORM, RGBA8 (UNORM, SRGB, typeless), BGRA8, R16 UNORM, RG16 and RGBA16 float, R32 UINT,
    RG32 UINT, RGBA32 float, R10G10B10A2, R11G11B10, R9G9B9E5, B5G6R5, B5G5R5A1, D16, D32, D24S8, D32S8 and BC1-BC7
    (SRGB and signed variants included), as a 36x20 texture with three slices and six mips (odd mip sizes), 3D
    textures with boxes in depth, a single-mip single-slice texture and a one-block BC7. Buffers: `CopyBufferRegion`
    at odd offsets and sizes, a `CopyResource` of a 4093-byte buffer, and an upload buffer straight to a readback
    buffer. Only the bytes inside a footprint row are compared (row padding is the driver's; a difference there is
    printed, not failed); every byte a partial copy must leave alone is compared. It found:
    - `R8G8_SNORM`, `R16_UNORM`, `R32G32_UINT`, `R9G9B9E5_SHAREDEXP`, `B5G6R5_UNORM` and `B5G5R5A1_UNORM` textures were
      refused (no Vulkan mapping); they map to `R8G8_SNORM`, `R16_UNORM`, `R32G32_UINT`, `E5B9G9R9_UFLOAT_PACK32`,
      `R5G6B5_UNORM_PACK16` and `A1R5G5B5_UNORM_PACK16`.
    - A texture-to-texture box on a block-compressed texture that reaches the padded edge of a mip (for example a
      box of whole blocks over a 9x5 mip) was refused as `invalid_copy`; the PC runtime accepts it.

    Copies between different mips of one texture are compared too (mips 24, 12, 6, 3, 1; RGBA8, BC1, BC7): sizes that
    are no block multiple, boxes to a padded edge, and boxes that do not fit the destination, which both sides must
    refuse (the host at `Close` with `E_INVALIDARG`, the layer at execution with `invalid_copy`). It found:
    - **Block-compressed copies into a mip whose size is no block multiple** (a 0..8 box into a 6-wide mip, a 6x6
      mip into a larger one, a 4x4 box into a 3x3 mip) were refused with `invalid_copy`: the host copies whole
      blocks, and one `vkCmdCopyImage` cannot (its extent must be a block multiple or reach each image's edge). They
      are staged through transfer scratch now (above) and match the host byte for byte, as do a box 4..8 of a 6x6
      mip into a 3x3 one and a 3x3 mip into a 6x6 one at (4, 4). Measured on an RTX 5070 Ti and an RTX 3060 (driver
      621.24) and an integrated AMD Radeon(TM) Graphics (`0x1002:0x164e`, Vulkan driver 2.0.353), Windows 11, each
      against its own host. `d3d12_state_map_test` checks the staged regions on the CPU.

    Known gaps shown by it and by the formats, not fixed:
    - **`TEXTURE1D`** resources are not supported by the layer (creation, footprints and allocation info refuse
      them), so 1D copies are not compared.
    - On a GPU with a native D24, a readback leaves the unspecified `X8` byte of the depth plane untouched where the PC
      runtime writes 0; the comparison ignores it. Where the layer stores D24 as D32 (see section 3.2) it writes 0.
    A PC driver's copy flushes denormal float depth to zero, so the test keeps its values normal. A
    `LEONIDA_HOST_ADAPTER` that matches no adapter fails the run instead of skipping the comparisons.
  - D24 against the host (`d3d12_depth24_compare.h`, `depth24_convert_test`): the D24S8 row of the copy test
    above runs with random 24-bit words on every GPU, the Radeon 880M's `D32_SFLOAT_S8_UINT` fallback included
    (72 regions, byte for byte, upload, readback and copies). `d3d12_depth24_compare.h` clears 5215 depth values
    (steps, half steps and the floats around them, powers of two, random) as a whole-view clear of a 4x4 texture
    (the first 160) and as one rectangle clear per value of a wide texture, with a stencil value, and compares both
    planes read back. The layer is checked against the reference (`depth24_quantize`), and against the host byte for byte
    where the host agrees with it; NVIDIA's host rounds some ties the other way (478 of 5215 values) and is logged as a host quirk, not a failure. It also clears an
    `R24G8_TYPELESS` texture through a typed D24 DSV (200 values, compared the same way), and reports whether
    shader-resource views of both planes (`R24_UNORM_X8_TYPELESS`, `X24_TYPELESS_G8_UINT`) write the T# of the
    texture's backing format (D3D-75): X24_8_32_FLOAT for the depth plane where the layer stores the texture as
    `D32_SFLOAT_S8_UINT`, 8_24_UNORM where the GPU has D24, and 8_UINT for the stencil plane. An SRV read is not compared with the host (no shader runs in these tests and the host's
    descriptor is opaque). It also runs an upload and readback under a `SetPredication` that skips copies, which the
    layer must still convert and which is compared with the host's unpredicated run. `depth24_convert_test` runs the
    compute pass on the GPU for all 16777216 words (to float, and back), 16777216 float bit patterns against
    `depth24_quantize` (with a 1 MiB range so the copy splits into 65 dispatches), and a pitched copy at an offset
    split by a 1 KiB range. It tests the pass, which needs no D24 absence, on whatever device
    `VK_LOADER_DRIVERS_SELECT` picks; the layer's own D24 path (the D3D12 tests above) is used only on a GPU without
    D24 depth-attachment support, so run those with the AMD driver selected (the Radeon 880M, RADV).
- `d3d12_translated_draw_test`: milestone M1.5 (D3D-53), the same path with translator output.
  - **Shaders:** `test/shaders/vs_m15.asm` and `ps_m15.asm` are hand-written RDNA2 (gfx1013). The build assembles them
    with `llvm-mc` and translates them with `tools/rdna2spirv.py` through `test/shaders/build.py`. `build.py` also writes
    each one's synthetic FSSM program (`; fssm-at` / `; fssm-data` in the header) to `fssm/` and the translator puts its
    SHA-256 in `spirv/index.json`, so the layer's lookup is the one the game uses; the test loads that index as it is.
    An unknown program is refused.
  - **User data:** root parameter 0 is a descriptor table (`s8`, the 32-bit VA of its first slot), parameter 1 four
    root constants (`s9..s12`: the draw's clip offset, and a bias for the PS), in the console's graphics layout.
  - **Vertex fetch:** the VS has no input layout. It `s_load`s a V# the application wrote into a shader-visible slot and
    fetches position and colour with `buffer_load_dwordx4 ... idxen`; the V# points at a DEFAULT-heap buffer in the arena.
  - **Code base:** the PS starts with `s_getpc_b64` and `s_load`s four floats from offset 0x100 of its own FSSM program,
    so the layer must have uploaded that program at the push-constant code base (dwords 34-35).
  - **Draws:** a quad and a second V# over the same buffer, with different table slots and constants, into a
    `R32G32B32A32_FLOAT` target. Every value is a short binary fraction, so the comparison is bit for bit: 4064 of 4096
    pixels are exact, and the 32 whose centres lie on the triangle's long edge are counted apart.
  - **Zero-based ids:** all draws use `StartVertexLocation` 0 and `StartInstanceLocation` 0, so `v0` is the same with
    and without the translator's zero-based VS ids (#436, not in this tree: with its translator change applied the
    result was identical, checked on both GPUs). Both devices enable `shaderDrawParameters` where supported, which
    #436's shaders need. A non-zero base would differ (hardware: zero-based, the base in an SGPR), so the test avoids one.
- `d3d12_method_variants_test`: each variant against the method it wraps, on the device directly (the proxy's
  slot 76 is the Xbox texture method, not `CreateCommittedResource3`). It runs 12 creation cases (buffers on the
  three heap types, textures, a volume, a multisampled target, and three shapes the base refuses) through every
  `CreateCommittedResource` variant, which must give the same result, description, heap properties and `Map`
  answer. It runs the edge cases in the table above and compares them with the host's PC runtime when there is
  one; protected sessions are checked on the layer alone, and castable lists have their own success/refusal
  checks. `GetCopyableFootprints1` must match `GetCopyableFootprints` field by field, including the failure
  answer. A texture from `CreateCommittedResource3`
  is uploaded, copied back and compared on a queue from `CreateCommandQueue1`.
- `d3d12_sampler2_test`: `CreateSampler2` against `CreateSampler`, on the slots' bytes. It checks:
  - 10 descriptions without flags, which must be byte for byte the same;
  - 6 UINT and float-bit border colours against their float equivalents (the three built-in UINT colours with
    the integer mark, which must decode to `VK_BORDER_COLOR_INT_*`);
  - custom float and UINT border colours registered in the table on `VK_EXT_custom_border_color` (D3D-73),
    verifying index reuse for identical colours and distinct S# descriptors for distinct colours or integer vs
    float representations;
  - 3 non-normalized samplers, which must be `CreateSampler`'s S# with `FORCE_UNNORMALIZED` set;
  - the `D3D12_OPTIONS17` answer;
  - the 22 written-or-refused cases behind the table in section 3.2, compared with the host's PC runtime when
    there is one.
- `xbox_present_test`: `PresentX` and the frame events (section 4.5, "what is implemented"), checked on swapchain pixels.
    there is one. A host that reports no non-normalized samplers is compared only on the 3 cases without the
    flag. The other 19 are then checked against their recorded answers only.
- `d3d12_subgroup_test`: subgroup size control through D3D12 PSOs, on the PSOs of the device proxy.
  - **Shaders:** `cs_w64x_readlane` (a `v_readlane` across both halves of a wave64) translated natively and with
    `--wave64-emulate`, and `cs_wqm_w32`, built by `test/shaders/build.py` (needs `llvm-mc`: `LLVM_MC`).
  - **Check:** each is dispatched through a PSO found by FSSM hash and the eight words it stores are compared. The
    PSO's request (`pipeline_subgroup_requests`) must equal the decision function's. The native wave64 shader
    must be exact where the device can give 64 (AMD: required size 64 plus full subgroups) and is reported, not
    judged, where it cannot (NVIDIA); the emulated one must be exact everywhere.
  - **Control:** a second run with nothing requested is printed. On the Radeon 880M the default compute subgroup is
    already 64, so the wave64 controls are exact as well.
  - **Probes:** GLSL shaders (`cs_sg_probe.comp`, `ps_sg_probe.frag`, `vs_sg_fullscreen.vert`) store `gl_SubgroupSize`,
    so the size the driver used is observed for a wave64 entry, a wave32 entry and an emulated one. Where the PSO
    required a size, the observed size must equal it. For a wave32 compute shader on a device whose default is not 32,
    the control is asserted to differ (the request matters); a wave32 pixel shader on the 880M goes from 64-wide to
    32-wide with the request.
  - **Wave64 pixel shader:** `ps_w64x_readlane.asm` (`v_readlane` of lanes 40, 5, 33 and 63) is drawn over 64x64
    pixels through a graphics PSO into a float target, and every pixel must carry the four values derived in the
    .asm header where the PSO required 64: 4096 of 4096 on the 880M. The control is asserted to differ only where the
    default pixel subgroup is not 64; the 880M runs pixel shaders 64-wide by default, so its control is exact too. On
    the RTX 5070 the shader gets no 64 (0 of 4096 exact, not judged).
  - **RX 6800 (RDNA2, AMD Windows driver 26.11.2, 2026-11-22):** the compute requests take effect (wave32 compute
    goes from 64-wide to 32-wide), but nothing can be requested for the fragment stage: the pixel probe runs at the
    64-wide default for the wave64, wave32 and emulated entries (outcomes `device_default`, `unavailable`, `emulated`), and
    `ps_w64x_readlane` is not judged; its control, at the 64-wide default, is 4096 of 4096 exact.
- `d3d12_descriptor_test`: descriptors through the proxy.
  - Copies, checked on the slots' bytes: one run, range lists of different shapes on each side, null size
    arrays, and a destination longer than its source.
  - What views and samplers write, decoded: a view of an uploaded 8x8 texture and a sampler, mips 1 and 2 of a
    BGRA cube with a component mapping, an unordered-access view (the same bytes as the shader-resource view of
    that mip), and the null descriptors of a missing resource, a texture without `ALLOW_UNORDERED_ACCESS`, a view
    past the mips and a volume.
  - UINT border colours (Vonsek's case on #538): an `R8G8B8A8_UINT` texture sampled one tile to the right
    through `CreateSampler2` samplers with a UINT `(0,0,0,0)` and `(1,1,1,1)` border reads 0 and 1 in every
    channel. A float white border on the UNORM texture reads 255; its S# differs from the UINT white one only in
    the integer mark.
  - That set 0 holds them: a compute shader (`test/glsl/cs_sample.comp`, GLSL, compiled with `glslangValidator`;
    skipped without it) is handed a slot's T# and the sampler's S#, looks them up in the hash table as a
    translated shader does, and samples. The sampler wraps in U and the shader samples one tile to the right,
    so a missed S# shows too: the fallback sampler clamps. The test creates the pipeline in Vulkan and binds
    the device's set. It reads the texture through the view's own slot and through
    a copy of it, and gets the fallback from a slot of bytes no view was made for and, after the texture is
    destroyed, from the view's own bytes.
  - Capacity: 5000 distinct SRVs of that texture and one more, and 2100 distinct wrapping samplers in two
    2048-slot heaps. The last view through the last sampler reads the texture one tile to the right. With the
    sizes of before, both were past their arrays ("array is full") and the shader read the fallback.
- `d3d12_texture_uav_test` (D3D-78): creates 1D, 1D-array, 3D and 2D control UAVs through the D3D12 device,
  checks rejected descriptors, copies descriptors between slots, and dispatches a translated hand-written
  shader from a Vulkan compute pipeline on the actual set 0, not through D3D12 `Dispatch`. It reads back every
  texel; the UINT read/modify/write cases (not atomics) also run on Windows WARP. The D3D12 command-list path
  is used separately to flush the set and to test clears.
  The tests dispatch past the selected array/W range, so writes to an adjacent layer or slice fail the check.
- `d3d12_compute_test`: the compute path through the proxy. It creates a root signature from the PC serializer's
  blob, and a PSO through slot 62 from a container shaped like the game's, found in a translator-style index by its
  FSSM hash (an unknown program is refused). It sets root constants, a root UAV and a descriptor table whose
  application-written entry points at an upload buffer, then runs two dispatches that change s0 and the UAV in
  between. The results come back through a readback buffer. The shader, `test/glsl/cs_user_data.comp`, is GLSL
  written to the translator's push-constant and address contract, compiled with `glslangValidator`. It also checks:
  - the dispatches' root signature and PSO are released right after being set, and the same signature is set
    again after the arguments, so the list must hold both and keep the arguments (without the hold the test
    crashes);
  - the index has an entry with set-0 bindings for the same program ahead of the binding-free one, and the PSO
    must take the binding-free one;
  - a device that reports 128 bytes of push constants refuses the PSO.
- `d3d12_fspx_compute_test`: the FidelityFX DLL's two ways of passing an FSPX program, with synthetic programs
  ([fspx.md](fspx.md), "Build and run-time lookup"). A PSO through slot 62 from a `DXBC` container whose `ACCC` part
  is an FSPX payload, and one through `CreateComputePipelineState` from a bare kind-6 program, as the slot-67
  description carries it. Both are found by `fspx_code_sha256` in `<shaders>/fspx/index.json` and dispatched once with
  `cs_user_data.comp`, which adds dword 1 of its code base: each must read its own kind-6 program. Ten inputs are
  refused with `E_INVALIDARG`: an untranslated program, bare and in a container; kind-6 entries that end past the
  payload, start inside the directory, wrap 32 bits, are too large or are empty; a container without `ACCC`; bytes
  without the prologue; and the program with four bytes appended.
- `d3d12_bindless_churn_test`: the table churn of D3D-314 through D3D12 alone, on a direct and a compute queue with
  three frames in flight. Each of 96 frames makes 24 textures with SRVs in a ring of shader-visible heap slots and
  releases those of seven frames earlier (about 190 live keys, so the table grows), the direct queue uploads, the
  compute queue waits for it on the GPU, and each queue runs the `cs_set0_sample.comp` shader eight times on SRVs
  of the frame and the three before it. All 98,304 sampled texels per run are compared with what the CPU wrote.
  The two D3D12 queues get distinct `VkQueue`s where the device's family has two (`queue_count_wanted = 2`,
  `command_queues`; the test prints which it got and says so when the family has only one). Two queues on one
  `VkQueue` are ordered by submission, which hides a missing wait between them. It runs four times: as the layer is (0
  `vkDeviceWaitIdle` for the bindless set required, at least one generation made, a retired generation seen pending
  right after the submit that made it); with the compute queue never waiting on the direct queue's fence (it samples
  textures the CPU has seen finished, so only the grown table is new to it); and both again with
  `LEONIDA_BINDLESS_IDLE_WAIT=1` (the old wait, counted above 0). Times are printed, not asserted. The async runs
  are what the layer's wait on the uploading submission keeps correct: with that wait removed, the RTX 5070 run
  failed at frame 5 (compute read a zeroed table: the grown binding 2 with the upload not yet run), as @duudiis
  reported on #2740; the old-wait path shares that bug on `main` (the idle wait only covers work already submitted,
  not another queue's later submission that binds the new table). The Radeon 880M did not reproduce it in two
  runs. `d3d12_bindless_churn_validated` runs it under the validation layer.
  The table's growth is the only replacing flush this scenario produces (3 generations). The other kinds (a live
  key's index replaced, a relocation) and the failed-submit retry are checked at the Vulkan level by
  `vk_bindless_test` cases 3 to 6 (the set changes; `release_retired` frees only what its ticket allows). That test
  needs the translated `vs_ids`, `ps_tex` and `ps_imgatomic`; it was not in ctest before D3D-314 (only
  `scripts/run_gpu_tests.py` and `check.py` ran it), and is now, as `vk_bindless_ps_tex` and `vk_bindless_ps_imgatomic`.
  without the prologue; and the program with four bytes appended. It also covers the dispatch state:
  - one program in containers with three kind-0 entries picks its own entry for each, an unknown kind 0 is refused, and
    the bare program is refused as ambiguous;
  - slot 67 with all four pairs picks by kind 0 too (one entry's module is missing on purpose, so the pick shows as
    `E_FAIL`), and four inconsistent pairs are refused before they are read;
  - slot 67 with the description built the way the DLL builds it (`dll_slot67`: the reader's pair order over the
    container's own directory, kinds 0, 1 and 7 pointing into the payload, kind 6 copied into a 256-rounded buffer
    padded with `0xcc`) resolves each program to its own translation with its own state;
  - console slot 11 on a slot-67 and a slot-62 pipeline returns an object whose slot 3 points at the dword 0 and whose
    slot 4 gives 4 bytes; it releases to 0, and the pipeline's reference count is unchanged.
- `d3d12_set0_compute_test`: set 0 from the compute bind point, through the proxy and D3D12 alone. The index entry
  of its shader lists set-0 bindings, so the PSO's layout holds the device's bindless set and replay binds it at
  each dispatch; nothing in the test binds it. Four dispatches of `test/glsl/cs_set0_sample.comp` (GLSL written to
  the translator's contract) load a T# and an S# from two descriptor tables and sample one tile to the right in U:
  - two SRVs give their textures texel for texel, so the T# decides the image;
  - 32 bytes no view was made for give the fallback image, one colour everywhere;
  - a clamping sampler gives the texture's right edge, so the S# decides the sampler;
  - a device without the bindless set refuses the PSO.

  Removing the set-0 bind from the dispatch path makes this test fail (`VUID-vkCmdDispatch-None-08600`, measured on
  llvmpipe); `d3d12_m2_textured_test` covers the bind at draws in the same way.
- `d3d12_draw_test`: milestone M1, through the proxy.
  - **Pipeline:** a VS+PS PSO found by FSSM hash. The input layout is POSITION then COLOR, while the VS has them in
    the opposite register order, so a match by position would fail.
  - **Buffers:** the vertex buffer is copied into the arena, and the index buffer is read from an upload heap.
  - **Draws:** two indexed draws with different root constants and root CBVs, then a back-facing triangle that must
    be culled. Each of these also checks a fix:
    - the first draw uses a PSO with a second RT format and a DSV format, with neither bound (a variant);
    - before the second draw, the same root signature is set again after the arguments, which must keep them;
    - two more draws, through a viewport on the top-right quadrant, read COLOR per instance with step rate 0 (two
      instances that must both read element 0) and from an unbound slot (zeros).
  - **Second pass** on the same target, cleared again: a full-screen triangle from a VS with no inputs
    (`test/glsl/vs_fullscreen.vert`), first in its submission so no vertex buffer was bound before it, through a
    viewport on the bottom-right quadrant; then, where the GPU has `logicOp`, the red quad XORed over the blue
    top-left quadrant, which must read `(255,0,255,0)`.
  - **Refusals:**
    - PSOs that need wireframe fill, independent blend, a depth-bias clamp or depth clamping are refused on this
      device, which enables none of them;
    - nine render targets is refused, at PSO creation and at `OMSetRenderTargets`;
    - 0-byte and wrapping buffer sizes are refused, and a buffer or shader-visible heap larger than the arena runs
      out of memory;
    - a draw with no viewport executes cleanly and draws nothing.
  - **Check:** all 4096 pixels of the read-back target are compared. Where the red and green quadrants land also
    checks the viewport flip and the winding. The test fails with any of these fixes undone: the rebind, step
    rate and logic op show wrong pixels, the variant breaks `dynamicRenderingUnusedAttachments-08912`, and the
    stride state breaks `pStrides-04913`.

  The shaders are GLSL written to the translator's contract (`test/glsl/vs_draw.vert`, `vs_fullscreen.vert`,
  `ps_draw.frag`).
- `d3d12_present_test`: milestone M0. It opens a window and presents 60 frames, each one: reset the list, move
  the back buffer `PRESENT` → `RENDER_TARGET`, clear, move it back, execute, present, signal a fence. It then
  resizes the window and presents 10 more (70 in all), which recreates the swapchain, scales the blit and switches
  to unsynchronised presents. The presenter's capture copies every presented image back, and the test checks
  that the whole image holds the clear colour at the swapchain's size (after the first 60 frames, after the
  resize and at the end). Last, it reads the colour on screen with GDI when the window is in front, no other
  window lies over its centre (an always-on-top one can, even over the foreground window) and GDI can read the
  screen. On a GPU that does not drive the window's monitor (the UHD 770 with the desktop on an RTX 4080 SUPER) the
  desktop never shows the swapchain, so the test prints the pixel, does not assert it and says why. Under Wine's macOS driver it can't: `GetPixel` on the screen DC returns `CLR_INVALID` even
  for a pixel GDI painted itself, and the window's own DC holds Wine's GDI surface (white), not the image
  presented through the Metal layer. There the capture checks are what confirms the colour.
  - The injected failures below make the runtime log `[rt] d3d12: present failed` three times (VkResult
    -1000001004 twice, then -1, `VK_ERROR_OUT_OF_HOST_MEMORY`). The test announces them on stderr first; they
    are expected and are not out-of-date presents from the driver (the closing line counts those).
  - The device and the presenter get their Vulkan functions through a driver double in the test. It forwards to
    the real driver, counts the presents the driver reports out of date (the frame counts allow for them), and
    tracks the binary semaphores alive.
  - Through the double, the test reports one successful present as out of date (not counted) and one as
    suboptimal (counted).
  - It fails the second per-image semaphore of a recreation: only the two acquire semaphores may remain, and a
    retry at the same vsync must make a complete swapchain.
  - It fails one blit submission: the next four presents must go through without hanging.
  - Run against the presenter before these fixes, the test fails at the frame count after the resize, where the
    driver reported a real out-of-date present (RTX 5060 Ti, driver 621.24).

**Open:** whether the game uses command-list flags, bundles, or `ExecuteCommandLists` with many lists at once. All
are supported by the IR; the trace will show which are hot.

[descriptor-overview]: https://learn.microsoft.com/en-us/windows/win32/direct3d12/descriptors-overview

### 3.3 One GPU address space

The shader contract is the constraint that shapes everything else. The translator computes:

```
host_address = heap_base + console_va
```

where `heap_base` is push-constant dwords 32–33 and `console_va` is an Xbox GPU virtual address the game writes
into a descriptor, a root argument, or a buffer. This applies to buffer, descriptor-table and shader-code
addresses. Image descriptors (T#) are hashed by the translator, not dereferenced, so an image's base can be a
separate key range (below). Most shaders also force the high dword of a descriptor-table pointer to zero (25,865
of the 26,503 in package 1.0.2.1, counted 2026-11-22; see "Descriptor-table high-dword truncation" below), so
**shader-visible descriptor heaps must live below 4 GB** in this address space.

The layer therefore owns the console VAs. It returns them from `ID3D12Resource::GetGPUVirtualAddress` and from
`GetGPUDescriptorHandleForHeapStart`; the game only sees values the layer chose. The design is:

| Piece | Design |
|---|---|
| Address space | Two ranges. A **BDA arena** holds buffers, descriptor heaps and shader code; its device address is `heap_base`. A separate **image key range** supplies unique console VAs for images; those keys are never added to `heap_base`. The arena's `VkBuffer` is created with all buffer usages the game can bind (uniform, storage, vertex, index, indirect, transfer) plus `SHADER_DEVICE_ADDRESS`, and its memory with `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`. |
| Buffers | A buffer resource is a range in the arena's single `VkBuffer`: offset = its console VA, size = the resource size. Buffer commands bind the arena buffer with that offset, so its device address is exactly `arena_bda + offset`; no per-resource `VkBuffer` object is needed. |
| Images | Each image gets a unique console VA key and has device-local memory with optimal tiling: a range of a 64 MiB block that other textures share (`runtime/src/image_memory.h`, `VK-IMAGE-MEMORY-POOL`), or its own `VkDeviceMemory` when it is a render target, a depth buffer, a CPU-visible texture or larger than an eighth of a block, or when device-local memory has no room for another block. A range goes back to its block when the last submission that could use the image has completed. One reused from an earlier texture is zeroed before the next submission that runs shaders, as a committed resource reads as zero. Blocks hold only optimal-tiled images, so `bufferImageGranularity` does not apply. Shader image access goes through the T#/hash table, so nothing dereferences the key. Keeping images out of the arena avoids forcing optimal-tiled images into the arena's memory type and keeps the arena sized by buffers, not textures. |
| Heaps and placed resources | `CreateHeap` reserves the requested size in the arena, so a buffer placed at a 64 KiB-aligned offset is that offset into the arena and its console VA is the heap's place plus the offset. A placed texture (1D, 2D or 3D) gets its own memory (a range of a shared block or its own `VkDeviceMemory`, above); one the title placed at its own address through the console's `CreatePlacedResourceX` (Xbox device slot 133) reports that title address from `GetGPUVirtualAddress`, as the console does — a title turns it into the CPU pointer it clears the texture through — while its pixels do not alias the title's pages yet. Any other texture answers a null VA, as on D3D12. Tiled (`64KB_UNDEFINED_SWIZZLE` and Xbox `0x101`) 2D/3D placed textures (D3D-191) and multisampled 2D placed textures (D3D-222) share that backing, answer `GetResourceAllocationInfo` and `GetResourceTiling`, and check heap/offset alignments. Reserved buffers can use heap tile mappings (section 4.2). |
| Descriptor heaps | Shader-visible CBV/SRV/UAV and sampler heaps are allocated in the low 4 GB of the arena. RTV/DSV heaps are host-side only. |
| Shader code | Each FSSM blob is uploaded to the arena; its console VA is the code base. |
| Upload/readback | `D3D12_HEAP_TYPE_UPLOAD`/`READBACK` resources are separate host-visible buffers outside the arena. Their console VAs are only used by copy APIs; if the game binds one as a vertex/index buffer or reads it in a shader, the layer biases the view/descriptor by `-heap_base` so the shader's addition lands on the host BDA (**ASSUMPTION**; **measured to fail for a V#**, see below). |

**Measured (XCL, retail title, PR #168):** the console answers a placed texture's `GetGPUVirtualAddress` with the
address the title placed it at; the title clears a 256×256 `R32_FLOAT` texture through that value as a CPU pointer.
On the PC, where D3D12 answers 0 for a texture, that write went to address 0. XCL returns the title address when the
PC answers 0 and leaves buffers and resources with a PC address unchanged
([`real.rs`](https://github.com/ivall/xcl/blob/main/crates/xcl/src/hle/d3d12x/real.rs), PR
[#168](https://github.com/ivall/xcl/pull/168)). Facts only: XCL is GPL-2.0-only and no code is copied. This layer
sets a slot-133 fallback texture's `GetGPUVirtualAddress` to the title address; the image itself still does not
alias the title's pages.

Non-arena buffers (upload/readback) can still be addressed by the shader without putting them in the arena: pass
`console_va = host_bda - heap_base` (mod 2^64) in root descriptors and vertex/index/indirect views, so the shader's
`heap_base + console_va` equals the host BDA. For descriptors, patch the V# base field the same way when the layer
copies or scans them. This relies on the game not using the raw VA for anything except shader addressing
(**ASSUMPTION**); if it does, the resource must move into the arena instead.

**Measured (D3D-53, `d3d12_translated_draw_test`):** the bias works for 64-bit root arguments (`d3d12_compute_test`: a root
CBV and a table slot that point at an upload buffer) but **not for a V#**. A V#'s base field is 48 bits
(dword0 and dword1[15:0]), and an upload buffer's console VA, its device address minus `heap_base`, is negative on both test
GPUs (`0xffffffffffce0000` on the AMD Radeon 880M, `0xffffffffffedc000` on the RTX 5070 Laptop; the buffer's device
address lay below the arena's, an allocation-order fact of these runs, so it is not guaranteed either way). The shader
adds `heap_base` to the truncated 48-bit value, which is a wild address: the draw faults and the whole queue submission is lost
(the clear that came before it is lost as well). Any resource a game can put in a V# (a vertex, constant or structured buffer
behind a descriptor) must therefore be an arena range, whatever the allocator happens to do, upload and readback heaps included. Until the layer does that, the
translated-draw test fetches from a DEFAULT-heap buffer and does not draw from the upload buffer.

**Measured (static, `XboxBvhBuilder.dll`): the BVH builder only takes destination addresses below 2^40.** Its
validation function at `0xc8f0` loads `DestAccelerationStructureData` (the first field of the build description it
reaches through `[rcx+0x68]`) and checks it twice:

| Check | Instruction | On failure |
|---|---|---|
| 256-byte alignment | `test r8b,r8b` | message "…must be 256-byte aligned…" |
| below 2^40 | `cmp r8,10000000000h` / `jae` | message "…can not have more than 40b of address set" |

On a failed check it prints its message through `OutputDebugStringW` after a "D3D12 XDXR Error: " prefix, and then
throws `E_INVALIDARG`: a C++ exception out of the DLL, not an `HRESULT` return.
- The alignment rule is the public `D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT`.
- The 40-bit rule has no PC counterpart.

Arena VAs (at most 4095 MiB, so 32-bit) pass. A VA biased from outside the arena does not: the negative upload-heap
VAs above are at or above 2^40 as unsigned values. That is one more reason for the rule above that every buffer
a console client can address is an arena range, whichever heap it comes from.

**Title-chosen addresses.** Two kinds of buffer address are the title's choice, not the layer's:
- device slot 133 (`CreatePlacedResourceX`) puts a buffer at the CPU address of the title's own pages;
- the title points root views and V#s at its own memory, for instance a per-frame constant ring (#8862, section
  3.5 of `docs/retail_boot_chain.md`).

On Windows these are user-mode addresses far above 4 GiB, past the arena (at most 4095 MiB, and descriptor
tables must stay below 4 GiB), so `heap_base + VA` names nothing the layer made (the D3D-53 failure above). The layer
therefore replaces such an address, wherever it sees one, by the console VA of a GPU copy of that memory
(`runtime/src/title_va_alias.h`):

| Where the address is | When it is replaced |
|---|---|
| Root CBV/SRV/UAV (`Set*Root*View`) | when the argument is set |
| CBV and buffer SRV/UAV V#s the layer writes (`Create*View`, slot 70, counter V#s included) | when the descriptor is written |
| Buffer V#s the title writes into a shader-visible CBV/SRV/UAV heap itself | before each `ExecuteCommandLists`, in place, for the tables the submitted lists bind and only the slots their root signatures declare |

The copies:
- A **slot-133 buffer** whose pages cannot be imported is the per-submission shadow, and the shadow is an **arena
  range** when it fits. Its console VA is its arena offset, which holds in a root argument, in a V#'s 48 bits and in
  a 32-bit table pointer alike. Without room in the arena it keeps its own memory and is biased like an upload buffer
  (logged once; raise `LEONIDA_ARENA_MB`). An **imported** slot-133 buffer is reached at its device address minus
  `heap_base` (mod 2^64): exact for root arguments, and for a V# only when the driver put the import above the arena
  (logged once otherwise).
- Any **other address in the title's memory** gets a window: the 64 KiB-aligned pages it covers, clipped to the
  title's range, copied into the arena now and before every submission, through the same shadow refresh as slot 133
  (so dirty-page tracking for that refresh applies to both). A window never covers bytes an existing range already
  serves: it stays inside the free span around the address (and a stream-pool extent inside the free span in both of
  the pair's views), because a range entered over a live one replaces it whole, and the copy that range answered from
  would be left reached by nothing. Windows stay while an address range still answers from them, up to
  `LEONIDA_TITLE_WINDOWS_MB` (64 MiB by default). A window that no range answers from any more (a slot-133 buffer the
  title placed over its pages, or a BVH input's exact copy, replaced its ranges) is **retired** after the next
  submission's refresh: the alias table's listener records the removed ranges' copies, and a window none of its
  ranges is left for loses its stream-pool extent and its bytes of the budget at once. Its memory is freed once every
  `ExecuteCommandLists` replay in progress then has recorded its submissions, every command list whose recording
  began before the retirement is gone (reset, released, or a held batch's or the async worker's copy of it run), and
  the submissions recorded by then have completed: a list resolves its root views when they are recorded, so one
  recorded before the retirement and executed, or executed again, after it still names the window
  (`src/command_list_epochs.h`). Until then its copy stays a target of its pages' tracker, refreshed like the others,
  and what the GPU wrote in it goes back to the title's pages: with its submissions' write-back, and once more when it
  is freed (a range noted when a list was recorded can be copied back before a held batch has run). A retired copy goes
  back only on the bytes no live copy over them changed, byte by byte (the window and the buffer that replaced it can
  each have written their own bytes of one 256-byte piece), and every copy is compared with the title's bytes as
  they were before any copy went back. The bytes a retired copy returns are also written into every other copy over
  them that still held the title's older bytes, as the next refresh would: a window is freed after its submission's
  refresh, and the range that replaced it reads its last bytes in that same submission. `d3d12_title_window_orphans` covers it, with a writing batch held on a fence
  while the next submission's refresh retires its window. A window is
  not retired when its pages are released or remapped: a retail run measured no release at all of the pages windows
  cover (the title remaps its pool frames instead), and the tracker follows the pages.
  A tracker the window shared with a copy still live keeps its extent (a retirement never splits one). A window that
  cannot fit between the title's address reservations can use a separate mapped buffer. The buffer's device address
  must fit as a positive 47-bit offset from the arena base. The same window cap and dirty-page refresh apply.
  `LEONIDA_STREAM_BUFFER_WINDOWS=0` requires the window to fit in the arena.
- **What the GPU wrote goes back.** The same return copy serves slot-133 shadows (a buffer the driver would not
  import, a compute result the title reads again): the ranges are recorded through the alias of the buffer's pages and
  the tracker there carries them back. `d3d12_placed_write_back` covers it. A copy is a read-side mirror, so a UAV or a raw write into a window would land in
  the copy and the title would never see it ([#1537](https://github.com/ubiquitarianism/leonida/issues/1537), the
  `WRITE_INVALID` stop at the world start). Every title range a submission's bindings reach is kept, and the next
  refresh copies those ranges from the copy to the title's pages *before* it copies the pages forward, so a page the
  title itself wrote in between keeps the title's newer bytes and its GPU result is dropped. The copy back asks the
  write watch first (the refresh's own asking comes after it, so a hot page's last answer is a submission old), passes
  over pages whose mapping changed, that a kernel write may be filling or that xmem maps at another address too, and
  writes only the bytes where the copy differs from the title's (compared in 256-byte pieces): a read-only view writes nothing and
  its pages stay guarded. The copy runs under the tracker's lock and can still fault on a page another guard keeps
  read-only (the geometry watch, another view's shadow): the fault handler asks every tracker over the address, this
  one included, and on the thread whose copy faulted it does not take that lock again (an SRW lock is not
  recursive; on 621.40 the title's submitting thread hung there for good, the main loop frozen with no device loss).
  Up to 8,192 ranges and 64 MiB a submission (a V# has no read/write bit, so every view
  reached is compared; past the caps the ranges are left behind, logged once). `LEONIDA_TITLE_WRITEBACK=0` turns it off. A batch is fenced: it is kept with the ticket
  of the submission that will use it and copied back only when the context reports that ticket retired, so a range
  another submission is still writing is never read half done. A queue held deep (console ordering) can leave batches
  waiting: past 64 of them the oldest go back without their ticket, logged once.
- A **window that is refused** (the budget is spent, the arena is full, or the pages are not committed yet) is backed
  by a persistent geometry copy of exactly the requested range (`LEONIDA_GEOMETRY_MB`) instead; when no copy can be
  made either, the view is written with **no base and no records** — loads return 0 and stores are dropped. A raw
  title address is never left in a V# or a root view: `heap_base + a raw VA` is not an allocation, and a store there
  loses the device. The refusal and the count of zero-size views are logged.
- A **stream pool** (the title's CPU view paired with a 16-24 GiB GPU-only
  alias) is not windowed per request. The GPU-only pages are translated to the CPU view of the same frames (rule B in
  that note), and **one mapping serves a page-aligned readable run** of the pool (at most 1 MiB, and never past the
  next mapping in either view; a geometry copy when the cap allows, a window otherwise). A request whose byte a
  mapping already holds in either view is served by it when the mapping holds all of the request's bytes, and the
  other view is entered as far as that mapping reaches. A request never gets a mapping shorter than its bytes (a V#'s
  size, 64 KiB for a root view): one that runs into the next mapping, or past the end of the one holding its first
  byte, gets one mapping over itself and every window it reaches, each whole, in both views (`title_pool_cover`,
  `TitleVaAliases::replace_twin`), and those windows are retired at the next submission. `replace_twin` checks, under
  the lock it replaces them under, that every range it would erase is one of those windows: a slot-133 buffer is
  entered without the window lock, so one placed in the span after the plan makes it refuse, the new window is dropped
  and the request falls back as below. Only with a range in the way
  that is not a window (a slot-133 buffer, a geometry copy) or that reaches past the pool in its view, or pages that are
  not readable, does it get a zero-size
  view: a refusal there would also have the root views a title rings through a pool read zeros where it served them
  before. `vsharp_address` asks again, like `TitleVaAliases::resolve`, when the range holding a V#'s base ends before
  the V# does. Both addresses of the pair are entered in the table, so a V# naming either reaches that one mapping instead of
  asking for a second one; `LEONIDA_GEOMETRY_MB` is the preferred backing.
  Acceleration-structure destinations and scratch also resolve through the table, and a structure no window holds
  whole gets a copy of its whole range, so the BVH builder's commands are recorded.
- `LEONIDA_GEOMETRY_MB` sets the device-local memory cap for streamed buffer views. The default is 0.
  A positive cap enables persistent copies of placed buffer SRVs and buffer descriptors in title memory.
  A GPU-only reservation uses its paired CPU mapping. Matching source ranges share one copy.
  The resource descriptor heap keeps its console indices and original CPU bytes. Its shader-visible copy
  encodes each resolved buffer's offset from the arena base. CPU writes through either physical mapping,
  file reads and XMem remaps invalidate the affected copies before their next submission.
  During an untyped heap scan, an extent above 16 MiB with no format or an unrecognized descriptor
  encoding reuses only an independently established geometry view. An explicit table or placed SRV can
  establish that view. The Vulkan memory probe reports populated views, payload and reserved bytes,
  upload totals, cap refusals and allocation failures.
  - **Where the copies live**: device-local memory through the BAR by default. `LEONIDA_GEOMETRY_HOST=1` puts them
    in system memory outside VRAM (write-back where the device offers a cached host type, `=2` write-combined),
    as VIctory keeps its copies of title memory (approach only). In a world run on the RX 7900 XTX the copies held
    4.4-6 GB of VRAM. The cost moves to the bus: the GPU reads every vertex and index of a copy from system memory,
    so a geometry-heavy pass can take longer; measure frame time with and without it before making it a default.
  - **When a copy goes**: a view is retired when its title range is freed, decommitted or unmapped. A remap of
    physical pages retires nothing: the copies follow the new pages through the refresh. A retired
    view leaves the lookups at once, its alias ranges and translated heap slots are invalidated, its stream-pool
    extent is forgotten, and its memory is freed only after every ExecuteCommandLists replay in progress at the
    retirement has recorded its submissions and those have completed (`kAfterExecutes`). Before it leaves the
    refresh it is copied once more from its pages if they are still readable (a heap block freed in memory that
    stays committed). The next view of the range makes a new copy.
    `geometry_views_test` cases 14-16 cover this without a GPU.
- **Which pages changed** since the last refresh: where xmem reserved the title's memory with `MEM_WRITE_WATCH`
  (`XMemVirtualAlloc`'s ordinary regions and the title's `VirtualAlloc` reservations, not placeholders or
  physical-page windows), the kernel says (`GetWriteWatch` with reset, its answers kept for every copy over the page:
  `leonida::title_write_watch` in `runtime/src/title_page_watch.h`); elsewhere a copied page is made read-only and the
  title's first write faults (`ShadowPages`). `LEONIDA_WRITE_WATCH=0` guards everything; under Wine that is the
  default and `=1` turns write watching on. A reservation the title releases and reserves again (at the same address
  too) is new to every copy, all of it taken as written once. The title's own `XMemGetWriteWatch` and
  `XMemResetWriteWatch` refuse a reservation write-watched for the copies, as the host refuses one that isn't. The
  write-fault handler waits for the trackers' locks while the faulting thread may hold the process heap's lock (the
  title's `XMemAlloc` blocks are `_aligned_malloc`'s), so the state changed under those locks allocates from a heap of
  the runtime's own (`leonida::host_heap`), never the process heap.
- **What the GPU wrote into a copy stays** until the title changes the bytes under it. Some pages are copied at
  every refresh without a sign that the title wrote them (mapped at two addresses, left writable, reopened with the
  64 KiB around a written page, write-watched or pinned); copied whole, they overwrote a value a compute program had
  stored in the copy for the next frame with the title's unchanged bytes, so it read back as the title's old value
  every other frame. `ShadowPages` keeps a 64-bit sum per 256-byte piece of the bytes it copied last
  (`runtime/src/shadow_keep.h`) and copies a piece only when the title's bytes differ; a piece zeroed because its page
  went away is copied again in full once it comes back. Limit: a title that writes the same bytes again to reset a
  value the GPU changed is not seen (that needs the GPU's writes to reach the title's pages). `LEONIDA_SHADOW_KEEP=0`
  copies every page as before.
- **Only the title's memory** gets a copy: the blocks and ranges the xmem shims hand the title (`XMemAlloc`,
  `XMemAllocDefault`, `XMemVirtualAlloc`, the title's `VirtualAlloc`). An address anywhere else, the runtime's or a
  host DLL's memory included, is left as it was and counted, as is an arena VA or a buffer the layer made.

The title's own VA is still what `GetGPUVirtualAddress` answers, and vertex, index, copy and immediate-write calls
still resolve it through the buffer VA index, which knows slot-133 buffers by that VA. The copies are one way: what
the GPU writes into a copy does not reach the title's pages. A pointer the title hides elsewhere (a raw VA inside its
constant data that a shader dereferences) is not seen by the layer and keeps the title's VA.

Why not give each buffer its own BDA and set `heap_base = 0`? Because 25,865 of the 26,503 shaders (97.6%)
truncate descriptor-table pointers to 32 bits, and host BDAs are not guaranteed to be below 4 GB. The arena keeps console VAs low and stable.

Why one arena rather than several? Because the shader gets exactly one `heap_base` for buffer, descriptor-table and
code accesses. A draw can reference a descriptor table in one heap and a vertex buffer in another; both must
satisfy the same `heap_base + va` equation. Several non-contiguous VkBuffers cannot provide that equation.

Image keys are 256-byte-aligned byte addresses. The [RDNA 2 ISA, §8.2.6, Table 45][rdna2-image-resource] defines
the 40-bit T# base field as address bits 47:8. `gfx10_descriptors.h` encodes bits 39:8 in word 0 and bits 47:40
in word 1[7:0]. The largest representable key is `0xFFFFFFFFFF00` (2^48 − 256 bytes). Bit 48 is not stored,
so keys separated by 2^48 bytes produce the same base field.

The runtime allocates image keys from `0x100000000` (4 GiB) in 256-byte steps, above the 4095 MiB arena cap.
**Open:** whether any shader in the game uses an image VA as a raw pointer rather than through its T#.

[rdna2-image-resource]: https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/rdna2-shader-instruction-set-architecture.pdf

**Memory type policy.** The first boot needs correctness, not bandwidth:

- M0–M3: allocate the arena as `HOST_VISIBLE | HOST_COHERENT` (preferably also `DEVICE_LOCAL` when large BAR is
  available). The CPU can then write descriptor heaps, shader blobs and uploaded buffers directly. Images are
  already device-local, so rendering still uses fast memory for textures. This is slow for dynamic buffers but
  removes staging and host/console address translation from the bring-up path.
  - A memory type is only tried when its heap can hold the whole allocation (`memory_type_order` in
    `runtime/src/vulkan_memory.h`, beside D3D-96's `resizable_bar`). Without Resizable BAR the only `DEVICE_LOCAL | HOST_VISIBLE` type sits in a
    heap of 256 MiB or less (214 MiB on an RTX 3090 with driver 616.56), so the 256 MiB default arena lands in
    system memory there. Asking that heap for it broke VUID-vkAllocateMemory-pAllocateInfo-01713 even though the
    driver returned `VK_SUCCESS` (reported by CharlyRien).
  - With Resizable BAR a driver can still leave the `DEVICE_LOCAL | HOST_VISIBLE` type out of a large buffer's
    `memoryTypeBits`. On an RTX 5070 Ti (NVIDIA 617.42, Windows 11) an arena-shaped buffer of 4,090 MiB may use
    it and one of 4,092 MiB may not, so `LEONIDA_ARENA_MB=4096` (4,095 MiB) put the whole arena in system memory.
    `create_arena` tries smaller sizes in 16 MiB steps, by at most an eighth, and keeps the largest whose
    requirements allow that type and fit its heap. Placing the arena in ReBAR does not guarantee a reduction in
    host commit. An [RTX 4080 comparison](https://github.com/ubiquitarianism/leonida/pull/1732#issuecomment-7159574385)
    confirmed ReBAR placement and measured higher process private bytes. The runtime logs the size cap:
    `GPU address arena: 4080 MiB of the 4095 MiB asked for, capped by the device's Resizable BAR memory types`.
    Without Resizable BAR nothing changes.
- M4+: allocate the arena `DEVICE_LOCAL` and stage CPU writes (descriptor heaps, shader blobs, buffer uploads)
  through a host-visible staging buffer, copied before the commands that use them. The hash table is not in the
  arena either: it is a normal storage buffer in `DEVICE_LOCAL` memory, uploaded from a mapped staging copy before
  the commands that read it (D3D-76).
- Upload-heap buffers prefer `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT` memory when the GPU has Resizable BAR
  (D3D-96): the CPU writes them through the BAR and the GPU reads them from VRAM. `leonida::resizable_bar`
  (`runtime/src/vulkan_memory.h`) calls it Resizable BAR when such a type sits in a device-local heap larger than
  256 MiB. Without Resizable BAR that type lives in a window of 256 MiB or less (214 MiB on an RTX 3090) that every
  upload buffer would share, so they stay in system memory. If VRAM is full, the allocation falls back to system
  memory. The device logs which one it uses (`[rt] d3d12: upload heaps in ...`), and `LEONIDA_UPLOAD_SYSMEM=1`
  keeps them in system memory to compare the two. Readback buffers keep preferring `HOST_CACHED`.
- Textures and default-heap buffers outside the arena prefer `DEVICE_LOCAL` and need no host access. When VRAM is
  full they still get memory, from the next type `memory_type_order` lists, which is system memory. The first one
  that does is logged once (`[rt] d3d12: a N-byte allocation did not fit in device-local memory ...`, D3D-299,
  `leonida::spilled_to_host`), so a run on a 4 GB GPU shows that it spilled (#2375).

**Capacity.** The arena is sized by buffers, descriptor heaps and shader code, not by textures. The allocator
tracks the high-water mark; start with a configurable capacity (for example 4 GiB for the loading/menu path) and
fail with a clear message when a console VA would fall outside it. Growing means creating a new arena, copying,
and updating `heap_base` for all subsequent commands; do it at a frame boundary, never with work in flight.

**Open:** the game's real buffer VA range and peak working set are unknown until it runs. `maxBufferSize` (with
`VK_KHR_maintenance4`) and `maxMemoryAllocationSize` bound the arena; a large open-world buffer working set may not
fit on smaller PC GPUs. This is a product requirement, not something the layer can hide.

**Reserved resources.** `CreateReservedResource` and the queue tile APIs use Vulkan sparse binding for buffers and
compatible single-mip RGBA8 2D textures (section 4.2). Other descriptions have fully allocated backing. Wider
texture coverage and the game's use of tile streaming remain open.

**Diagnostics for descriptors the title builds itself** (all off by default, logging only unless said otherwise):

- `LEONIDA_UNKNOWN_TSHARP=1`: one line for each distinct base address a title-written T# names with no texture there
  (its shape, the tiling bits and whether the address is title memory), up to 300 addresses, and every 8192 such
  lookups a summary by format, type and tiling.
- `LEONIDA_UNBACKED_VSHARP=1`: the buffer V#s `fix_vsharp` writes empty because nothing backs their base; one line for
  each of the first 120 distinct bases and every 8192 a summary by 4 GiB range of the base.
- `LEONIDA_UPLOAD_LOG=1` (`d3d12_xs_device.cpp`): each upload chunk (CPU base, device address, size) and the first 96,
  then every 8192nd, `AllocateUploadMemory` result as an offset into its chunk.
- `LEONIDA_UPLOAD_ALIAS=1` (an experiment, changes behaviour): enters each upload chunk's CPU range in the title VA
  table, so a V# the title wrote with the CPU pointer `AllocateUploadMemory` handed it is rewritten to the chunk
  instead of left empty. A chunk below the arena needs `LEONIDA_IMPORT_BELOW_ARENA=1` as well.

#### Address guard (`LEONIDA_ADDRESS_GUARD=1`, `rdna2spirv.py --address-guard`)

**Prototype, off by default.**

**Why.** The layer rewrites the addresses it can see: root views, the V#s it writes, and the V#s the title writes into
shader-visible heaps. It writes an empty V# when nothing backs an address (above). The translator checks a V#'s
record count but not its base. Any other address becomes `heap_base` plus an arbitrary value:
- a V# a shader builds in SGPRs from data;
- a raw `global_`/`flat_` address (530 of the 26,503 shaders of package 1.0.2.1, 69 of the 2,684 pipeline-library
  programs);
- a stale or offset-like value.

On RADV such an access page-faults. Two RX 7900 XTX runs (Mesa 26.3.1, under Wine) ended with the same sequence:
- a `gfxhub` fault from client TCP with PERMISSION_FAULTS 0x3;
- a compute-ring timeout and "MES failed to respond";
- a MODE1 reset ("VRAM is lost").

Both faults were 4.8 GiB and 20 GiB above `heap_base`, past the arena's end.

"Inside the arena" is not the right bound. Several buffers are legitimately outside it, at a positive offset from
`heap_base` (a negative one with `LEONIDA_IMPORT_BELOW_ARENA=1`):
- geometry copies;
- upload and readback buffers;
- imported title memory;
- separate window buffers.

The guard checks each access against what the runtime actually backs. An access that fails reads 0 and drops its
store, so a wild address never reaches the GPU. The overall process follows VIctory
([`crates/victory-gpu/src/memory.rs`](https://github.com/marvisak/victory), GPL-3.0). Its page table of 64 KiB pages
routes every computed address, and memory no copy holds reads 0 and takes no writes. The structure below is our own:
VIctory's table names descriptor views, this one is a bitmap over device addresses.

**Structure: a two-level page table of 4 KiB pages over the 48-bit offset space, behind an arena compare.** The
check works on the heap offset, the sign-extended 48-bit value `Gen.host_addr` adds to `heap_base`:
- **Level 1:** 65,536 words indexed by offset bits 47..32. Each word names the level-2 block of one 4 GiB chunk.
- **Level 2:** blocks of 32,768 words, one bit per 4 KiB page (bits 31..12).
- **Two fixed blocks:** block 0 is empty and block 1 is full; allocated blocks start at 2.
- **Size:** the table is a host-visible buffer of its own, 256 KiB plus 128 KiB per block (8.25 MiB for 62 blocks).

Why this structure:
- **Pages of 4 KiB** are the GPU's own mapping granularity. A page any byte of a backing touches is mapped as a whole,
  so marking pages outward is exact for faults. A 64 KiB page would mark memory that may not be mapped.
- **Level 1 by 4 GiB chunk** covers the whole signed 48-bit space (negative offsets index from 0x8000 up). A real run
  touches a handful of chunks, so the table stays small.
- **Other structures cost more per access.** A sorted range table needs a binary search: about 10 dependent loads for
  a thousand backings, against 2 here. A coarse region bitmap plus a fine range table needs a search in the fine part.
- **The arena needs no lookup.** It is one fully bound `VkBuffer`, so any offset below its size passes on a compare.
  That is the common case: descriptor heaps, default-heap buffers and windows in the arena.

**The header.** It sits at arena offset `0x70000`, after the zero page and the quad indices, and holds:
- a 64-bit magic (`"LGUARD01"`);
- the arena's size (the compare's limit);
- the table's address minus `heap_base`;
- a refusal count and the last refused offset, which the shaders write.

Two sinks follow the header: `0x71000` (zero area, what refused loads read) and `0x78000` (dump area, where refused
stores and atomics go). None of `[0x70000, 0x80000)` is handed out. With `LEONIDA_ARENA_NULL_MB` the range lies in the
null region, so its offset never moves. `runtime/src/address_guard.h` and `tools/address_guard.py` (the Python
reference) hold the format.

**Each access.** `Gen.host_addr` takes the access kind (load, store or atomic) and its span. Under `--address-guard`
it calls one function per module, `%guard_fn`, which is straight-line at the call site, so no caller's block
structure changes. The function computes `end = offset + span - 1` and then decides:
- **Passes on a compare:** the runtime's guard is off (no magic), or `offset` and `end` are both below the limit. The
  result is `heap_base + offset`. The cost is a 64-bit add, two 64-bit compares and a select.
- **Otherwise, a lookup:** the bits of the pages of `offset` and of `end`. That is two independent pairs of dependent
  32-bit loads (level 1, then level 2).
- **Refused:** an access whose bits are not both set adds 1 to the header's count, writes its offset, and goes to the
  zero area (a load) or the dump area (a store or atomic), at its offset within its page. Alignment is kept, and every
  span fits inside a sink.
- **Atomics** run on the dump area, and their returned value is replaced by 0 (`Gen.guard_zero`). The CAS loops (float
  min/max, inc/dec) end there too, as on any other memory.

The entry block reads the header's first six dwords once per invocation, from the arena, so that read is always
backed. When the base is uniform, as for `s_load`, `s_buffer_load` and a V# from SGPRs plus a uniform offset, the
compiler keeps the whole check scalar: one check per instruction, not per lane. A per-lane address (`global_` with a
VGPR address, `offen`/`idxen`) is checked per lane, and the slow path runs only for lanes outside the arena.

**What is checked.** Every `heap_base` access goes through `host_addr`:
- buffer loads and stores (dword, sub-dword, typed, format; swizzled spans are computed at run time);
- `global_` loads, stores, sub-dword and atomics;
- buffer atomics, `_x2` included;
- `s_load` and `s_buffer_load`;
- the BVH node reads of `image_bvh*_intersect_ray` (span by node type).

These are not checked:
- images (T#s go through the hash table, not an address);
- LDS;
- the `--mesh-compute` output buffer (a runtime address);
- `s_getpc` code reads.

The translator does not translate `flat_` instructions (they raise `Unsupported`), so none reaches memory today. A
translation added later must go through `host_addr` like `global_` does.

**Keeping the table current.** The spans are the same backings the console heap pass's backed table uses
(`update_gpu_backed`):
- every `gpu_backings` entry outside the arena (upload and readback buffers, imports, slot-133 buffers, windows
  outside the arena);
- the geometry copies' blocks;
- the separate window buffers' blocks.

`update_address_guard` runs in the queue's `before_execute`, after the heap refresh, the windows and the geometry
copies. It differs from that table in four ways:
- It rebuilds whenever `backing_generation` or a block layout changed, with no 100 ms limit, because a backing made
  since the last submission must not read 0.
- It writes only the words that changed, level 2 before level 1.
- A chunk keeps its block for good, so a submission still in flight never reads a block that now means another chunk.
- When the 62 blocks run out, a new chunk gets the full block (unchecked, as without the guard), and that is logged.

A release clears its bits at the next submission. D3D12 forbids releasing memory the GPU still uses, so only a wild
access in flight at that moment could still pass.

**Cost per access:**
- **Fast path:** about 8 scalar or vector ALU operations.
- **Outside the arena:** about 40 operations and 4 loads, from a table small enough to stay in cache.
- **Per invocation:** 6 uniform header loads.
- **Per submission (runtime):** nothing until a backing changes, then a rebuild of the used blocks (128 KiB each)
  and a diff.

**Switches.**
- `--address-guard` records `"address_guard": true` in `index.json`.
- `LEONIDA_ADDRESS_GUARD=1` makes the runtime reserve the range, create the table and write the header.
- Either alone does nothing: a guarded module on a runtime without the guard finds no magic, and every access passes
  after one compare.
- Without the flag the module is byte for byte what it was.

**What a run with the guard shows.**
- One line when the device is made: `[rt] d3d12: address guard on ...`.
- At most one line a second: `[rt] d3d12: address guard: N access(es) refused so far (+k), the last at heap offset
  0x...`. Each refusal is a wild access that would have reached the GPU. The offset says where to look (above the
  arena's end, as in the two faults).
- A run that reset the GPU before should keep going, with wrong pixels where a refused access mattered.

**Risks and open points.**
- A refused access that mattered (a backing the layer never registered) now reads 0 instead of faulting. That is
  silent apart from the count.
- An access is checked as a whole. A multi-dword load whose first dwords are inside a V# and a backing, but whose
  last dword is past both on an unbacked page, reads 0 in full.
- The table is read through the GPU's caches. A page whose bit changes while a submission is in flight can be seen
  either way by that submission.
- Sparse buffers outside the arena are guarded only if they are registered as backings.

**Checked:**
- `tests/semantic/test_address_guard.py`: the fixture `cs_address_guard` with the guard on, with it off at run time
  and without the flag, through `spirv_mini_interp`; the kind and span of each instruction family; the table model;
  the constants shared by the translator, the runtime and `tools/address_guard.py`.
- `runtime/test/address_guard_test.cpp`: the page table, incremental writes, negative offsets, wrap, chunks and
  overflow.
- `scripts/run_address_guard_gpu_test.py`: the fixture on a 2 MiB heap whose table backs one page outside the
  "arena". Every address it touches is inside the buffer, so no run can fault. With the guard on, the refused loads
  read 0 although memory is there, the store and the atomic land in the dump area and the count is 6. With the guard
  off, the same module reads that memory. It passed on lavapipe (Mesa 26.3.1).
- Corpus (package 1.0.2.1, 2026-11-26):
  - `--shard 0/8` of the shaders: 3,313 translated and 3,313 `spirv-val`-valid, with the guard and without.
  - The pipeline library: 2,683 of 2,684 translated and valid, with the guard and without (the same one fails
    either way).
  - Without the flag, every module is byte for byte the same as before the change.
  - With it, the SPIR-V grows by 1.8% (shaders) and 1.6% (library).

### 3.4 Descriptor heaps and the bindless model

The game can use D3D12 descriptors in three ways, and the layer must handle all of them:

1. **Root descriptor tables.** `SetGraphicsRootDescriptorTable` supplies a GPU handle; the shader receives the
   table's console VA in its user data and loads T#/S# structures from it.
2. **Direct heap indexing.** The root signature has `D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`
   and/or `..._SAMPLER_HEAP_DIRECTLY_INDEXED`, and the shader indexes `ResourceDescriptorHeap` /
   `SamplerDescriptorHeap` directly. The console driver supplies the bound heap base and stride; the shader loads
   the descriptor at `base + index * stride`. **Measured (D3D-329): each heap base arrives as a 32-bit pointer in
   an input SGPR the shader reads directly, inside the user data for compute; no stride arrives at all.**
   `tools/heap_index_census.py` over the 1.0.2.1 corpus (26,503 listings; 1,473 with embedded data words skipped)
   follows every descriptor-width `s_load_dwordx4/x8/x16` back to the SGPRs it starts from. Of 770,412 such loads,
   769,366 (99.86%) start from an entry SGPR used as a 32-bit pointer (high word a constant 0), 0 from a pointer
   loaded from memory, and 1,041 could not be followed (0.14%, all but 63 in heap-indexing shaders). That holds for
   the 18,066 shaders whose `SFI0` sets either heap-indexing bit as much as for the 6,964 that set neither. The
   heap-indexing shaders add a base read with a run-time offset (the heap index): for example `s10` in CS, `s23` in
   MS and `s15` in PS for the resource heap, and a separate base for the sampler heap (`s11` in CS, `s24` in MS,
   `s16` in PS). For compute this is settled: every base lies below the shader's `COMPUTE_PGM_RSRC2` `USER_SGPR`
   count, so it is user data. For VS, PS, MS and GS no user-SGPR count is measured; every base lies below `s32` and
   none is the NGG `s3`, so they are user data under the translator's 32-SGPR assumption, and the user count of the
   graphics stages (question 3 in the table below) is what would confirm it. Which SGPR holds which heap varies
   with the stage and the root signature, so placing the bases belongs to the root-signature mapping, not to a new
   translator input. No stride is read at run time: static offsets of T# loads are multiples of 32 and of S# loads
   multiples of 16, so the stride is compiled into the code. In the runtime shim, `CreateRootSignature` records the
   `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` and `SAMPLER_HEAP_DIRECTLY_INDEXED` root-signature flags and `SetDescriptorHeaps`
   tracks the bound shader-visible heap GPU bases. A matched compute root signature and dispatch settle the
   resource base for the Prop and Tree BatchCull programs. Their two explicit tables and one constant occupy
   s4, s5 and s6. Both programs read the resource heap from s7, the next user dword. Supply its arena-relative
   byte address in push-constant dword 7. The shader zero-extends this dword and adds the 32-bit resource
   index shifted left by five. A 32-byte slot contains a four-dword buffer descriptor or an eight-dword image
   descriptor. The host arena BDA remains in dwords 32–33. A matching runtime trace found s7 zero with the
   resource heap bound. The PreSkin compute programs use s9. Graphics examples with resource-direct indexing
   include PS_NoStripImmutableTable at s11 and VS_TransformD at s12. Their shader inputs establish the
   destination dwords. The explicit graphics layouts and ordering for signatures with spills still need
   independent root-signature evidence.
   **Inline compute layout (integration #984).** Run
   `python -I tools/compute_heap_layout_census.py <carve>/disasm` on each compute carve. Of 5,450 indexed compute
   entries, 2,959 advertise direct indexing and none of those embeds its root signature. Adding
   `--group fspx_compute --dispatch-log <dispatch-log>` joined seven resource-indexing entries to independently
   recorded layouts. In all seven, a dynamically indexed resource descriptor used the first SGPR after the explicit
   arguments. There were zero mismatches and zero cases without room **among those seven**. For the remaining
   2,952 entries the layout is unknown. These are entries across carves, which can contain the same program.
   No sampler-indexing entry had a matched layout. Putting the resource heap first when both flags are set, then the
   sampler in the next dword, remains a hypothesis. D3D-329's adjacent SGPR examples alone do not establish it.
   The runtime snapshots the bound resource and sampler heap pointers in that order after the explicit
   arguments for direct and indirect compute dispatches with an inline signature. Bound console descriptor heap
   base addresses are remapped through `console_descriptor_table_va` so that user SGPRs receive the arena copy's
   shader virtual address (where GPU descriptor reads land, preventing `READ_INVALID` device loss during world
   load), while unshadowed host heaps remain unchanged. An unbound heap contributes
   zero, and rebinding affects later commands. A heap that cannot fit in the inline user data is reported once,
   and a signature whose compute arguments spill gets nothing written (logged once). Set
   `LEONIDA_COMPUTE_DIRECT_HEAPS=0` to restore the previous compute arguments.
   Draws do the same in the graphics layout (`pack_stage(Stage::Pixel)`, from s8, one dword per table): the
   resource heap's 32-bit shader VA goes in the dword after the explicit arguments, then the sampler heap's, each
   only when the signature has its flag, snapshotted per draw. Vertex, pixel and geometry shaders share one
   user-data block and that layout, so one write serves all three; a signature whose explicit arguments take
   s8..s10 gives s11, one whose arguments take s8..s11 gives s12 (the two program reads above). A mesh shader reads
   push dword d as s(d + 8) through its root window (section 3.5), so `DispatchMesh` records the same dwords and
   the mesh stage finds the resource heap at s(16 + end − 8), right after its arguments from s16, where DadaCap
   reports every directly indexed program of the retail library reads it, for compute from s4, VS/PS/GS from s8
   and mesh from s16 (issue #14, comment 7143020922, on a `boot-prototype` run; not reproduced in this
   repository). A mesh signature that fills the window leaves no room there: a heap past s31 of the mesh shader
   (push dword 24 or later) is logged once. A signature that keeps the s0 graphics layout, or whose arguments fill
   s31, gets none (logged once). `d3d12_graphics_direct_heaps_test` checks the recorded draws and reads back
   what vertex and pixel shaders saw with and without the flags. On a mesh-capable device it also checks the
   heap pointers read by the mesh and pixel stages after a rebind. `d3d12_mesh_test` checks the recorded dwords
   for a vertex draw, a resource-only rebind, a signature ending at s30 and one on the s0 layout.
   Set `LEONIDA_GRAPHICS_DIRECT_HEAPS=0` to restore the previous draw arguments.
   Missing console V# backing leaves a zero descriptor in the copy. Each V# half of a console resource heap's copy
   (the buffer V# and the UAV counter) has a record in host RAM of what it resolved to: the title's base and span,
   the device address its copy names, and whether it resolved or waits (`src/console_heap_slot_records.h`, written
   only by `fix_console_slot`). Two indexes find the halves near a range, by base and by target, per power-of-two
   span class and 64 KiB-or-larger granule. Releases (a backing, a title-window or geometry block), alias ranges
   removed or replaced (`TitleVaAliases` tells its listeners as it happens, `forget_copy` included) and additions
   (a backing, an alias range, title memory) go on an unbounded per-device list; at the next submission each heap
   fixes again exactly the slots whose records a change touches: a release or removal every half naming it, an
   addition the waiting halves over it. Nothing walks the heap or reads the copy for this. A zeroed range, a slot
   the title rewrote and a heap destroyed drop their records. `console_heap_slot_records_test` checks that a release
   of a buffer 10 of 60,010 slots name looks at those 10, and compares the copy with a full re-resolution through
   random writes, releases, additions and alias changes.
   Console resource heap copies and shader-visible CBV/SRV/UAV heaps with at least 65,536 slots request
   64 MiB of zeroed padding after their last descriptor. `LEONIDA_HEAP_GUARD_MB` sets the padding in MiB
   (0 disables it). When arena space is tight, allocation can fall back to the heap alone. Only indices
   inside the reserved padding read null slots. Console slots containing data instead of a recognized image
   or buffer descriptor have their first 16 bytes cleared in the GPU copy. The title keeps its original bytes.
3. **Game-built descriptor memory.** The game writes raw T#/S# bytes into a heap itself, without calling
   `CreateShaderResourceView`. The shaders then read those bytes exactly as in case 1.

The layer's descriptor model:

- A `DescriptorHeap` has a host shadow and, if shader-visible, an arena range. CPU handles point into the shadow;
  GPU handles are arena VAs. `Create*View`, `CopyDescriptors` and `CopyDescriptorsSimple` write the shadow and mark
  the range dirty. The execute-time scan below is the correctness path, so a game that writes raw descriptor bytes
  through a CPU handle still works; dirty tracking is only an optimisation.
- **Heap start-handle ABI.** Prior art measured `GetCPUDescriptorHandleForHeapStart` and
  `GetGPUDescriptorHandleForHeapStart` (heap slots 9/10) returning the 8-byte handle in RAX on the Xbox build,
  while the PC call returns it through a hidden pointer in RDX. A generic sret/IDL thunk must special-case these
  two slots or the handle is garbage. This is a method-level ABI quirk; the heap object is still the public
  `ID3D12DescriptorHeap` interface.
- The bytes written for an image/sampler descriptor are **GFX10 T#/S# structures**, the same structures the
  translator reads. The layer writes them itself in `Create*View`; this does not depend on what the console driver
  wrote. If the game builds descriptors in memory itself, it must use the same format (the shader reads raw T#/S#),
  which is the assumption to verify. If it uses an opaque format, the layer must translate at bind time.
- At execution, for each bound table (or the whole bound heap when direct indexing is enabled), the layer walks
  the declared descriptor range with the heap's stride, parses each T#/S#, and ensures a bindless entry exists:
  - image T# → `VkImageView` (format, view type, mip range, array layers) → bindless image index;
  - sampler S# → `VkSampler` (filter, address modes, LOD, compare, anisotropy) → bindless sampler index;
  - buffer V# / CBV → no Vulkan view; the descriptor already contains a console VA the shader dereferences.
- **Buffer UAVs.** `CreateUnorderedAccessView` on a buffer writes a V# in the first 16 bytes of the slot (the rest of
  the 32 bytes is zero): `BASE_ADDRESS` = the resource's GPU VA + `FirstElement` * the element size, and by view kind
  - *raw* (`R32_TYPELESS` + `RAW`): stride 0, `NUM_RECORDS` = bytes, `OOB_SELECT` 3 (raw);
  - *structured* (`UNKNOWN` + `StructureByteStride`): stride, `NUM_RECORDS` = records, `OOB_SELECT` 0;
  - *typed* (any other format the typed-UAV table has: `uav_clear.h` layouts that `gfx10_descriptors.h`'s
    `format_table` names with a code below 128): `FORMAT` = the `GFX10_FORMAT_*` code, stride = the element size in
    bytes, `NUM_RECORDS` = elements, `DST_SEL` = the format's own channel order (`format_swizzle`: R8 reads as
    (x, 0, 0, 1), B8G8R8A8 as (z, y, x, w)), `OOB_SELECT` 1 (structured: only the index is checked against
    `NUM_RECORDS`, `docs/buffer_bounds.md`). This is what `buffer_load_format_*` / `buffer_store_format_*` / the
    `buffer_atomic_*` of a translated `RWBuffer<T>` read; the translator decodes the element from the V#'s `FORMAT`
    at run time (`rdna2spirv.py` `FORMATS`) and bounds-checks the whole element against `NUM_RECORDS`. A format the
    layer cannot name (a typeless or 96-bit one, a compressed one, anything whose code is 128 or more, `R16G16_SNORM`
    and the other texture formats `vulkan_format` has no row for) leaves a null slot, logged once. Typed clears
    (`ClearUnorderedAccessViewUint/Float`) read the same V# back, so they work on these views and on game-built ones.
  - **UAV counters** (`pCounterResource` + `CounterOffsetInBytes`, structured views only). D3D12 keeps the counter in
    an application-visible buffer: there is no `CopyStructureCount` (that is D3D11); a game copies the counter with
    `CopyBufferRegion` from `pCounterResource` at `CounterOffsetInBytes` (this is "CopyStructureCount" in D3D12 and
    needs nothing from the layer), and resets it with `ClearUnorderedAccessViewUint` on a raw view of that word or a
    copy from an upload buffer. The layer refuses (null slot, logged once) a counter on a non-structured view, a
    counter resource that is not a buffer created with `ALLOW_UNORDERED_ACCESS`, an offset that is not a multiple of
    `D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT` (4096) or leaves no room for 4 bytes, and a non-zero offset with no counter
    resource, the combinations D3D12 documents as invalid. **Hypothesis for the console contract (not measured):** the
    shader gets the counter as a second V#, raw, 4 bytes (stride 0, `NUM_RECORDS` 4, `OOB_SELECT` 3) at address
    counter VA + offset, in **bytes 16..31 of the same descriptor slot**, which is otherwise unused for a buffer
    (a V# is 16 bytes in a 32-byte slot), and `AppendStructuredBuffer` / `ConsumeStructuredBuffer` compile to
    `buffer_atomic_add` / `buffer_atomic_sub` with `glc` on it, the returned old value (minus one for consume) being
    the record index. Why this guess: the translator has no `ds_append` / `ds_consume` / GDS path and the corpus scan
    (open-questions table below, row 10) found no GDS instruction, so a counter in memory is the only form left; where the
    counter V# sits (a second slot, a user SGPR pair, a spill) is open until a shader that uses one is traced
    (row 16 of the same table). `d3d12_buffer_uav_test` runs `cs_buf_counter.asm`, a hand-written shader of exactly that shape:
    256 lanes append, 128 consume, the counter reads 256 and then 128 through `CopyBufferRegion`, and the 256
    appended records are each present once. If the console puts the counter elsewhere, only `write_buffer_uav` moves.
- Unbounded descriptor ranges (`NumDescriptors = UINT_MAX`) have no end. Scan from the table handle to the end of
  the bound heap, and cache parsed descriptors by key so unchanged slots cost nothing. **Open:** whether the game
  uses unbounded tables; the trace will show the root-signature ranges.
- The heap stride is the value the layer returns from `GetDescriptorHandleIncrementSize`. Prior art forwards device
  slot 15 (`GetDescriptorHandleIncrementSize`) unchanged, but the actual increment values are still **Open**. Choose
  32 bytes for CBV/SRV/UAV (the T# size) and 32 bytes for samplers (16 bytes of S# plus padding) pending the console
  value. The game may hardcode the console increments or infer them from shader address arithmetic; the shader's
  shift amount gives the stride it expects, and the layer must match it.
- `CopyDescriptors` copies raw bytes. If the descriptor was produced by our `Create*View`, the copy is a valid
  T#/S#; if it came from game memory, it is still valid raw data. No re-encoding is needed.
- Handle validation: a GPU descriptor handle is an arena VA; the heap registry maps the containing range to a
  heap. A table handle outside the bound heaps is a validation error; log it and resolve to fallback index 0
  rather than reading arbitrary memory.
- RTV/DSV creation builds a `VkImageView` and stores it in the host-side heap; `OMSetRenderTargets` resolves those
  handles to views for the rendering scope. RTV/DSV descriptors are never shader-visible.
- View creation maps `D3D12_SRV_DIMENSION`/`D3D12_UAV_DIMENSION` to `VkImageViewType`, the mip and layer ranges,
  sample count, and `D3D12_SHADER_COMPONENT_MAPPING` to `VkComponentMapping`. Single-layer 1D textures have default and
  explicit shader-resource views on sampled binding 5. Single-sampled 1D, 2D, 2D-array, cube and 3D colour SRVs apply
  the T# minimum LOD in resource mip coordinates. A clamp inside the exposed mip range uses
  `VK_EXT_image_view_min_lod`. Vulkan allows the image-view minimum to be rounded down to an integer.
  With nearest mip filtering, the UHD 770 and Arrow Lake Xe-LPG returned mip 0 for a 2D minimum of 0.75.
  The Xe-LPG also returned mip 1 for a 2D-array minimum of 1.75. RTX and Radeon returned mips 1 and 2.
  `d3d12_descriptor` accepts either rule for a fractional clamp (all texels of a view following the float or the
  floored minimum), and prints which one the driver used.
  See [Vulkan image-level selection](https://docs.vulkan.org/spec/latest/chapters/textures.html#textures-image-level-selection)
  and the [Intel readbacks](https://github.com/ItsNotRexed/Leonida/pull/1015#issuecomment-7075135647).
  If the logical device lacks the feature, the runtime logs the unsupported view
  and its descriptor resolves to the fallback. A clamp at or below the first mip needs no extension. Above the
  last mip, the view retains its dimensions and maps sampled channels to zero while preserving constant channels.
- D3D-78: a `Texture1DArray` UAV gets a 1D-array view with the requested array subrange and storage bindings
  26–28 (float, UINT, SINT); `ArraySize = UINT_MAX` selects the remaining layers. A non-array `Texture1D`
  view of a 1D array selects layer 0, as checked on native D3D12. A `Texture3D` UAV gets a 3D view with a single mip
  and Vulkan array range `{0, 1}`.
  A partial W range requires `VK_EXT_image_sliced_view_of_3d` and `robustImageAccess` on the logical device:
  `VkImageViewSlicedCreateInfoEXT` rebases storage-image Z to `FirstWSlice` and limits it to `WSize` (`UINT_MAX`
  means the remaining mip depth). It does not slice the sampled view. The generated T# tags the W range for
  whole-mip and partial UAVs, distinguishes them from SRVs in the hash table, and rebases typed sampled fetches;
  see the contract in `docs/design.md`.
  Without those features a partial W view is logged and left null, not made writable across the whole mip.
  Shader-visible 1D, 1D-array and 3D UAVs require both formatless storage read and write support for the actual
  Vulkan view format. `image_load ... glc` uses the storage view for 1D and 3D, and 1D-array loads always use it.
  The emulated RGB9E5 raw `R32_UINT` storage view is formatted and is exempt. Vulkan 1.3 permits these SPIR-V
  capabilities even without the legacy `shaderStorageImage{Read,Write}WithoutFormat` feature bits. The
  caller-owned device test leaves both legacy bits off and masks each per-format bit to verify safe rejection.
  `ClearUnorderedAccessView*` reads the W range from these descriptor bytes even after a descriptor copy.
  General 1D/3D backing-format reinterpretation remains unsupported.
  Shader access to the new UAVs requires `robustImageAccess` (whole-mip CPU clears do not).
  A separate native D3D12 probe on the RTX 4090 and UHD 770 clipped typed writes to the view's W range.
  WARP 10.0.26100.9818 rebased the view but did not clip writes past a finite `WSize` in that probe. The
  regression compares in-range writes with WARP and checks preservation of out-of-range slices independently.
- Set 0 is a fixed bindless layout, sized once when the device creates it. The defaults are 4096 sampled 2D
  images, 1024 samplers, 1024 2D-array images, 256 1D, 256 3D and 256 per storage dimension/class. The 2D, sampler and
  storage arrays grow to what the device's limits allow, up to 65536, 4096 and 4096 (1D-array storage stays
  capped at 256; see `vulkan_bindless.h` above). The set is not recreated to grow, because recreating it would invalidate the pipeline
  layouts built on it. Use `UPDATE_AFTER_BIND` and `PARTIALLY_BOUND` so a new view does not require rewriting
  every descriptor; without those features, rebuild the set when it changes.
- The command buffer binds set 0 once; descriptor-heap changes only update the arrays and the hash table.

**Descriptor lifetime.** A bindless index may be referenced by in-flight command buffers after the resource is
destroyed. Views/samplers are retired on a fence-value queue; the hash-table entry can stay (a stale key is only a
miss for a new descriptor, not a correctness problem) until the slot is reused. Reusing a slot for a new resource
updates the index in place; the key match makes the lookup correct.

### 3.5 Root signature to user SGPRs

This is the least verifiable part of the layer. D3D12 gives the application a root signature; the console driver
turns it into the user SGPRs (`s0..s31`) that the shader reads. The translator already copies those 32 dwords from
push constants into `s0..s31`. The layer must produce them.

The first mapping the layer used (`rt::rootsig::pack` in `runtime/src/root_signature.{h,cpp}`, kept for the tools and
tests that compare the two models; the runtime now uses the console layout below at both bind points) follows root
parameter order:

| Root parameter | User-data dwords |
|---|---|
| `D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS` | one dword per constant |
| `D3D12_ROOT_PARAMETER_TYPE_CBV` / `_SRV` / `_UAV` | two dwords: the 64-bit console VA passed to `Set*Root*View` |
| `D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE` | two dwords: the table's console VA (a `D3D12_GPU_DESCRIPTOR_HANDLE`) |
| static sampler | none; registered in the hash table at root-signature creation |

Before the draw/dispatch, the layer copies the current root arguments into a 32-dword block in that order and
uploads it as push constants. Missing parameters are zero.

Measured on the corpus (`python -I tools/user_sgpr_census.py <spirv/index.json> --base ps=8 vs=8 gs=8 cs=4 ms=16`, all 26,503 shaders). These are physical user SGPRs; the tool cannot see logical root-parameter indices.
- The lowest user SGPR read as a pointer is s8 in most PS (9,830 of 12,101) and VS (1,660 of 2,236), s4 in CS (2,364 of 2,689) and s16 in mesh shaders (7,977 of 9,282).
- Pointers are 32-bit (`ptr32_lo`) in every PS/VS/CS/MS shader that reads one; only the 195 GS read a 64-bit pointer (`ptr64_lo` at s8).
- Consecutive pointer SGPRs in one shader are most often adjacent (distance 1: 22,008 PS, 14,548 MS, 3,059 CS, 1,836 VS), and the SGPR after a 32-bit pointer is never read as a `ptr64_hi`.
- Below those bases only `const32` reads appear, at odd SGPRs in PS/VS/CS (s1, s3, s5, s7), and s2 in GS: a reserved block whose meaning is not established here.

Taken together this disagrees with the packing above (from s0, two dwords per table) and is consistent with a per-stage base (s8 VS/PS, s4 CS, s16 MS) and one dword per table pointer. It is a census of shader reads, not a confirmed mapping: it does not show which root parameter feeds which SGPR, and the GS behave differently. `--expect ps=8 vs=8 cs=4 ms=16` turns it into a check; `tests/test_user_sgpr_census.py` shows that the two packing models give different results.

**Console layout, measured for compute (D3D-448).** `rt::rootsig::pack_stage` lays the parameters out for one stage:
in root order from the stage base, each while it fits whole, a constant taking one dword per value, a root
CBV/SRV/UAV two (its 64-bit VA), a descriptor table **one** (the 32-bit VA of its first descriptor; the heaps live
below 4 GiB). A parameter that does not fit goes, with every later one, to the spill array behind `s[0:1]`
(section 7), which is not modelled, so such a signature is refused. How each part is known:
- **Compute, base s4: measured three ways.**
  - The console compiler's output (section 7).
  - The package's FidelityFX DLL, whose 29 compute containers carry their `RTS0` next to the console-compiled
    program (`tools/rootsig_census.py <fspx> --disasm <fspx>/disasm`, Xbox Series 1.0.2.1). Base s4 plus the
    signature's span in this layout equals the program's RSRC2 `USER_SGPR` count in 29 of 29: 27 signatures
    (table, table, four root CBVs) span 10 dwords and launch with 14 user SGPRs, 2 (table, table, one root CBV)
    span 4 and launch with 8. The `pack()` layout spans 12 and 6, two short of both.
  - The reads: `tools/root_sig_contract.py <fspx index.json> --rootsig <that shape> --model console` finds all 20
    translated programs compatible (tables read at s4/s5 as 32-bit pointers, each root CBV's VA copied whole as
    two dwords from s6). The `pack()` layout contradicts 7 of them, which read the second table at s5, the first
    table's high half in that layout.
- **Vertex/pixel/geometry s8 and mesh s16: a hypothesis.** It rests on the census above only; no signature
  confirms these bases, and visibility is not modelled (every parameter takes its place in every stage).

The runtime uses the console layout at **both** bind points. Every stage of a pipeline reads one push-constant block
(`ShaderUserData` in `runtime/src/vulkan_replay.h`), so the graphics bind point lays a signature out once with
`pack_stage(Stage::Pixel)` (the same s8 base as vertex and geometry): root dword k at push dword 8 + k. A vertex,
pixel or geometry shader reads SGPR i from push dword i, so it finds the arguments at s8.. as the console puts them.
A graphics signature past s31 (more than 24 dwords from s8) keeps `pack()`'s layout (from s0, two dwords per table),
logged as "keeps the s0 graphics layout": the graphics spill array is not modelled. `LEONIDA_GRAPHICS_STAGE_LAYOUT=0`
keeps `pack()` for every graphics signature (`RootArgumentLayout::graphics_console` false).
**The graphics root window (D3D-448-GRAPHICS):** a mesh shader's root arguments start at s16, so the translator loads
its s16..s31 from push dwords 8..23 instead (`user_data_dword` in `tools/rdna2spirv.py`); a mesh and a pixel shader
of one pipeline then find each argument in the same dword. A mesh shader's s8..s15 are below its base and read 0;
s0..s7 stay push dwords 0..7 for every stage. The index records the window as `"root_window": {"sgpr": 16, "dword":
8}`, and a mesh PSO whose mesh shader lacks it (translated before) is refused with a log line asking for the shaders
stage again. The mesh window holds 16 dwords (s16..s31) where the others hold 24 (s8..s31): a signature that fits the
graphics layout but not the mesh window, or one on `pack()`'s s0 layout, is created (the first is logged), and
`DispatchMesh` with it invalidates the list (`RootArgumentLayout::mesh_ok`, `d3d12_mesh_test`). Live evidence for s8
and one dword per table: on the retail frame loop the VS of a textured draw reads table slots 0 and 2 through s8 and
its PS slot 1, which hold exactly the buffer, constants and texture the title bound (issue #14, comment 7131556366,
native Windows, RTX 5080). What mesh shaders read below s16 was counted on the 1.0.2.1 corpus
(`user_sgpr_census.py --base ms=16`, issue #14 comment 7138532347): of 9,282 mesh shaders, 9,279 read nothing below
s16; three read one 32-bit constant there (`s13` in one, `s15` in two), never a pointer, and they read it as 0 now.
Its meaning is not established. A root signature carries both layouts; one that only one of them can place is
created, and binding it at the other bind point invalidates the command list (`Close` returns `E_INVALIDARG`).
`s0..s3` of a compute dispatch are the driver's block; the layer leaves them zero (109 CS read `s1` as a constant,
42 `s3`; their meaning is open).

**Measured: a real signature beside the count it produced.** `tools/fspx_unpack.py` decodes `COMPUTE_PGM_RSRC2` of
each of the 29 compute programs in `amd_fidelityfx_upscaler_xbox.dll`, and its `USER_SGPR` field is the count the
console launched the dispatch with. Every one of those 29 containers also carries an `RTS0`, which makes this the
only place in the package where a serialized root signature sits beside the dword count it produced
(`tools/rootsig_census.py`). The game's own 26,503 containers carry no `RTS0`, so their read sets cannot be joined
with a signature and cannot settle it either.

All 29 are version 1.1 with `Flags = 0`, 168 parameters in total (58 tables and 110 root CBVs, no 32-bit constants,
SRVs or UAVs), every parameter at `D3D12_SHADER_VISIBILITY_ALL`, and 27 of them carry one static sampler. They come
in two shapes:

| Parameters | Signatures | `USER_SGPR` | Layer model (2 dwords per table) | One dword per table |
|---|---|---|---|---|
| 4 CBV + 2 tables | 27 | 14 | 12 | 10 |
| 1 CBV + 2 tables | 2 | 8 | 6 | 4 |

The surplus is 2 dwords under the layer's model and 4 under one-per-table, and it is the same for both shapes: the
driver put a fixed block below the parameters. Arithmetic alone therefore does not separate the two models. The
disassembly does (`tools/rootsig_census.py --disasm`, over the same 29 programs):

- `s4` is copied into the low half of a pair, the high half zeroed, and passed to `s_load` as the base in 29 of 29;
  `s5` is in 16. The two declared tables therefore sit at s4 and s5, one dword each, in signature order, and the 13
  programs that never read `s5` are using only the first.
- A 64-bit pair is copied whole from `s6:s7` in 27 of the 29. Five of those also copy from `s8:s9`, `s10:s11` and
  `s12:s13`, which matches their four root CBVs at two dwords each. The other two read none of `s6` to `s13`: they
  declare four CBVs and never touch them.
- 27 read none of `s0` to `s3`. In `_05` the program adds `s0` and `s3` to `s4`, and `_07` multiplies by `s0`, so
  the block below the tables is not always dead, but no program here uses one of those dwords as a pointer.

That is `USER_SGPR = 4 + 2 per table + 2 per CBV`, which gives the measured 14 and 8, and it is the one-dword-per-
table model behind a four-dword prefix. The CS census above agrees: its lowest pointer is s4, where a four-dword
block puts the first table, while two dwords per table would put it at s2.

Still open: which CBV index lands in which pair. The four pairs do appear in declaration order, but nothing in the
package proves that order, and every parameter in these 29 is `D3D12_SHADER_VISIBILITY_ALL`, so they say nothing
about the visibility question below.

Spill policy: a signature needing more than 32 user SGPRs, or more than the 64-dword D3D12 cost limit, is
refused loudly (`pack` returns the offending parameter) instead of being truncated. The console spills through
`__XB_GetRootSignatureSpillPointer`; until that layout is measured, refusing is the only correct behaviour.

Measured (D3D-315): zero below-base spill-pointer candidates in the translated shaders.
`python -I tools/root_spill_census.py <spirv/index.json> --carve <dxbc/index.json> --expect-no-spill` exits 0 on
the 26,501 translated shaders (all but the 2 compute shaders refused for workgroup memory under wave64 emulation),
Xbox Series 1.0.2.1:
- **No root signatures in the containers.** 0 of the 26,503 carved containers have an `RTS0` part. The game
  builds its root signatures at run time, so the number that exceed 32 dwords is open until they are dumped from
  the executable.
- **No spill-pointer candidates.** No user-SGPR read used as an address (`ptr32_lo`, `ptr64_lo`, `ptr64_hi`) sits
  below the stage base (ps/vs/gs s8, cs s4, ms s16) in any stage. For compute that block holds the measured spill
  pointer `s[0:1]` (section 7); here `s0` and `s1` are never read as an address, only `s1` as a constant
  (109 CS, 551 PS, 12 VS).
- **Compute stays far below the budget.** The largest RSRC2 `USER_SGPR` count is 14 (31 of 2,687 shaders), so at
  most 10 of the 28 root dwords above s4 are used.
- **The highest reads are not spills.** PS reads reach s31 (8 shaders), s29 (7), s27 (1) and s25 (8); MS s29 (2)
  and s27 (8); VS s25 (2). Each of those top reads is a 32-bit constant after a gap, not an address.

What this does not settle:
- **How many runtime-created signatures exceed the budget.** The containers carry none, so that count waits for
  signatures dumped from the executable.
- **Why a particular `pack()` refusal happens.** `pack()` counts two dwords per table from s0, which the census
  above disagrees with, so its 32-dword limit and the console's budget need not agree. Whether a given refusal is
  that model or a real spill is open until the signature and its shaders are compared.
- **A spill behind a pointer at or above the base.** It reads the same as a table pointer, so the census can't
  tell the two apart.

The spill layout stays unimplemented: no shader in the translated set reads a candidate a spill could sit behind.

Reconciliation: `tools/root_sig_contract.py` checks a candidate root signature against every shader's
`user_sgpr_reads` from `spirv/index.json` (const32 vs constant dwords, 64-bit VAs vs view/table pairs) and fails
the build on the first contradiction. Until the console mapping in §3.5 is confirmed by a PSO, this check — not
a rendered frame — is the acceptance gate for the mapping above.

Blob format: `rt::rootsig::parse_rts0` reads the `RTS0` chunk of a serialized root signature, versions 1.0, 1.1
and 1.2. `root_signature_oracle_test` checks it on Windows against blobs the PC `d3d12.dll` serializes. These
layouts are measured, not assumed:
- a descriptor table's payload is `{num_ranges, ranges_offset}`, with the ranges at that offset;
- ranges are 20 bytes, or 24 with the 1.1/1.2 flags dword, in `D3D12_DESCRIPTOR_RANGE(1)` order:
  `{type, count, base register, space, offset}` in 1.0 and `{type, count, base register, space, flags, offset}` in
  1.1/1.2, so the flags come *before* `OffsetInDescriptorsFromTableStart` (a range with `DATA_STATIC` and offset 7
  serializes as `… 8 7`). The parser keeps all six fields;
- root descriptors carry a flags dword from 1.1 on;
- static samplers are 13 dwords (`D3D12_STATIC_SAMPLER_DESC` order). In 1.2 they are 14: `D3D12_STATIC_SAMPLER_DESC1`
  appends `Flags`, i.e. UINT border colours and non-normalized coordinates.

Static samplers take no user-data dwords, so 1.2 changes nothing in `pack`.

**Root-signature flag quirk.** Prior art measured Xbox root-signature descriptors carrying flags `0x03000000`; the
PC serializer rejects them with `E_INVALIDARG` because its flags live in the low 16 bits. Clear the high byte on a
private copy before calling the host serializer/deserializer; never modify the game's descriptor. The meaning of the
bits is unknown. The v1.0 `D3D12SerializeRootSignature` path keeps flags at offset +32, and a descriptor with
`0x3000001` also fails on the PC runtime.

**Measured: where the bits come from in the package.** Of the two console-compiled clients, only `XboxBvhBuilder.dll`
sets them, and it does so twice.

- **In its shaders:** all 52 `RTS0` parts in its DXBC containers (version 2, i.e. 1.1) carry `Flags = 0x03000000`.
- **At run time:** the builder's only `D3D12SerializeVersionedRootSignature` call (`0xc32f`, import address table
  `0x180024250`, `d3d12_xs.dll`) is in the function at `0xc2d0`, which ten call sites reach. That function copies the
  48-byte `D3D12_VERSIONED_ROOT_SIGNATURE_DESC` to the stack (`[rsp+40h]`) and, with no branch before it, ORs the
  bits into the copy's v1.1 `Flags` (`+0x28`): `or dword ptr [rsp+68h],3000000h` at `0xc30e`.

`amd_fidelityfx_upscaler_xbox.dll` does neither. Its 29 `RTS0` parts carry `Flags = 0`, and no instruction in it uses
the constant `0x3000000`. So the bits are not a property of every console root signature; this builder adds them. The
retail executable's root signatures do not carry them either (old upstream PR 7140): its direct call passes a 1.1
description with `Flags = 0x80` (`LOCAL_ROOT_SIGNATURE`), and its root-signature decoder reads `Flags` as a
zero-extended 16-bit value.

The mask stays unconditional, because it can't touch a public flag: it clears bits 24-31
(`kXboxRootSignatureFlagHighByte`, `runtime/shims/d3d12_xs_rootsig.h`), and every public
`D3D12_ROOT_SIGNATURE_FLAGS` value is in bits 0-11 (the highest is `SAMPLER_HEAP_DIRECTLY_INDEXED`, `0x800`). So it is
safe on every descriptor and blob, whatever built it. `root_sig_public_flags_test` pins this:

- the helpers keep every flag value in `[0, 0xFFFF]`;
- the shims' serializers give the host's blob byte for byte for each public flag the host takes. Where the host
  refuses only `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` or `SAMPLER_HEAP_DIRECTLY_INDEXED` (Windows 10's in-box `d3d12.dll`),
  they serialize on purpose: the blob is the host's blob without those bits, with the full flags in `RTS0` and a valid
  container hash (`kDirectlyIndexedFlags`, `runtime/shims/d3d12_xs_rootsig.h`);
- a blob with `0x03000080` comes back as `0x80`.

**Version 1.2 on older runtimes.** The in-box `d3d12.dll` of Windows 10 (measured on 22H2, 10.0.19045, without the
Agility SDK) and of Windows 11 23H2 (#361) serializes root signatures only up to 1.1. On Windows 10 it rejects even an
empty 1.2 description with `E_INVALIDARG`, and `D3D12_FEATURE_ROOT_SIGNATURE` reports `HighestVersion` 1.1 (#528).
Version 1.2 differs from 1.1 only in
`D3D12_STATIC_SAMPLER_DESC1::Flags`. So on such a host, the `d3d12_xs` serializer rewrites a 1.2 description whose
static samplers all have `Flags == 0` as the same root signature in 1.1. It refuses one that uses sampler flags (a
UINT border colour or non-normalized coordinates) with a log line, since 1.1 can't express them. The host is probed
once with an empty 1.2 description. `root_sig_test` checks the rewrite field by field and through the host
deserializer.

**Heap-directly-indexed flags on older runtimes.** The same in-box `d3d12.dll` refuses
`D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` and `..._SAMPLER_HEAP_DIRECTLY_INDEXED` in the
serializer at every descriptor version, with `E_INVALIDARG` and "Unsupported bit-flag set (root signature flags c00)"
(measured on 10.0.19041.4355; 1.0 and 1.1 both fail). Its *deserializer* and its devices take a blob that carries
them: a blob serialized without the bits, patched back in and re-hashed deserializes with `Flags = 0xc00` and creates
a WARP root signature. So the `d3d12_xs` serializer, when the host refuses and the description sets either bit,
serializes again with the bits cleared and puts them back into the produced `RTS0` chunk, recomputing the container
hash. A host that takes them (Windows 11) gets one call and its own blob back. `d3d12_xs_root_signature_test` covers
both descriptor versions and checks the flags through the host deserializer and a device; the test-side
`d3d12test::serialize_root_signature` helper does the same retry for tests that serialize directly.

Things that can invalidate this simple model, all **Open**:

- **Shader visibility.** The layer starts with one global block in root-parameter order and treats visibility as
  access control, not as a per-stage reordering. If the console driver instead omits invisible parameters per
  stage, a VS and a PS would need different values at the same push-constant offsets, which Vulkan cannot express
  with one block; that would force per-stage blocks or a per-stage indirection. **Measured, VS/PS (D3D-318):**
  consistent with one block in root order. The game's shaders carry no `RTS0` and the executable that pairs them
  into pipelines is not in the package, so `python -I tools/user_sgpr_census.py <spirv/index.json> --pairs` pairs
  a VS with a PS by technique name instead (names alike apart from the `VS_`/`PS_` token) and compares the user
  SGPRs both read. Over the 1.0.2.1 corpus: 166 techniques have both stages (225 VS, 203 PS, 358 combinations).
  - The 379 SGPRs both stages read have the same kind (pointer or constant) in every case. Over all 26,196,976
    VS × PS combinations, the control, 25.5% of the shared reads differ in kind.
  - In 79 combinations one stage reads an SGPR the other skips, below an SGPR both read. If that parameter were
    omitted from the other stage, the parameters above it would move down one SGPR there; all 111 shared reads
    above such an SGPR still agree.

  It can't tell a hidden parameter from a visible one a stage doesn't read, and it doesn't see the root signature,
  so the executable's pipeline table would settle it; `tests/test_user_sgpr_census.py` shows that the two models
  give different counts. Mesh shaders (base s16) and the compute signatures are not covered here.
- **Root signature larger than 32 dwords.** D3D12 allows up to 64 dwords. The console has 32 user SGPRs, so a
  driver must spill the rest to memory, probably behind a pointer in the user data. The current shader contract has
  no representation for that. The likely answer is that this game keeps its root signatures small (bindless
  descriptors, a few constants), but it must be measured. If a large root signature appears, the spill layout has
  to be reverse-engineered from the shader's loads. Measured on the shader side (D3D-315, spill policy above):
  zero below-base spill-pointer candidates in the translated shaders. The root signatures themselves, and how many
  exceed the budget, are still to be dumped from the executable.
- **Direct heap indexing.** The bound heap base and stride are not root parameters. Measured (D3D-329, section
  3.4): each heap's base reaches the shader as a 32-bit pointer in an entry SGPR, inside the user data for compute
  (and below `s32` for the graphics stages, whose user count is not measured yet), and the stride is compiled into
  the shader's offsets. So the translator needs no new runtime value; the layer has to write the bound heap bases
  into the user-data slots the root-signature mapping assigns them. The root signature
  flag tells us direct indexing is enabled; the shader's address arithmetic identifies the base.
- **Static samplers.** D3D12 static samplers do not consume root parameters. The console driver may place their
  S# values in a fixed table. The layer registers every static sampler's S# in the hash table, so a shader that
  loads it by value works; a shader that expects a driver table needs that table's location (**Open**).
  `CreateRootSignature` does the registering, for 1.0, 1.1 and 1.2 blobs: each static sampler is encoded as
  `CreateSampler2` encodes the same description (the five `D3D12_STATIC_BORDER_COLOR` values, two of them UINT, and
  the 1.2 flags), and gets a sampler in set 0 and a key in the hash table without a heap slot. The entry outlives
  the root signature. `d3d12_descriptor_test` checks the key before and after and samples with it.
  loads it by value works; a shader that expects a driver table needs that table's location. **Measured
  (SHADER-735): no sampling instruction in the corpus was found reading an S# from a fixed table** (0 of 366,628;
  0.66% stay undetermined under the conservative reading), so on this evidence the layer has none to emulate; the
  counts and their limits are under "Where the sampled S# comes from" below.
- **Descriptor-table high-dword truncation.** If a shader forces the high dword of a table pointer to zero, the
  table must be below 4 GB; the layer allocates all shader-visible heaps there. If a shader forces a resource VA's
  high dword to zero, the resource must also be low; that would constrain the arena layout. Measured on 2026-11-22
  over package 1.0.2.1 (`python -I tools/ptr_truncation_census.py <work>/shaders/disasm`, all 26,503 shaders, the
  `truncates_high` / `ptr32_lo` rule of `tools/shader_meta.py`): 25,865 shaders truncate at least one pointer
  (VS 2,038 of 2,236, PS 11,666 of 12,101, CS 2,684 of 2,689, GS 195 of 195, MS 9,282 of 9,282), 86,036 truncated
  pointers in all, at most 5 per shader. Every one is the base of a 4/8/16-dword descriptor load (`s_load_dwordx4/x8/x16`,
  a table pointer) from a user SGPR, and no 1–2 dword `s_load` base is truncated. No `global_*` in the corpus
  takes its base from SGPRs: its 10,548 `global_*` instructions (530 shaders) address through VGPR pairs, which the
  classifier does not follow. 25,669 shaders truncate every user-SGPR pointer they dereference, 196 only some (the
  195 GS, whose 64-bit s8 pointer has a high dword the classifier cannot resolve, and one CS), and the other 638
  dereference no user-SGPR pointer. Bases not rooted in a user SGPR (462 shader/access combinations) never have a constant-zero
  high dword. So what is measured to need the low 4 GB is the descriptor heaps; buffer addresses reach the shaders
  inside V#s (48-bit bases) or in VGPRs for `global_*`, and this count sees neither, so it does not show that
  buffers may sit above 4 GB.

**Where the sampled S# comes from** (SHADER-735; counts only). `tools/sampler_source_census.py` follows the four
SGPRs of the sampler operand of every `image_sample*`, `image_gather4*` and `image_get_lod` back to where their
values are made: through constants, scalar loads, SGPRs spilled to a VGPR and read back, and joins, where it keeps
the sources of every path. Over the 1.0.2.1 corpus (26,503 listings, none skipped; the ones with embedded data
words are re-anchored as the translator does) 14,025 shaders sample, with 366,628 such instructions:

| stage | sampling instructions | loaded through user data | literal | held in user SGPRs | other base | undetermined |
|---|---|---|---|---|---|---|
| CS | 120,816 | 63,141 | 57,674 | 0 | 0 | 1 |
| MS | 21,992 | 10,142 | 9,723 | 0 | 0 | 2,127 |
| PS | 221,157 | 115,109 | 106,048 | 0 | 0 | 0 |
| VS | 2,663 | 1,020 | 1,351 | 0 | 0 | 292 |
| all (the 195 GS do not sample) | 366,628 | 189,412 | 174,796 | 0 | 0 | 2,420 |

- **Other base** is what a driver table would be: some dword of the S#, on some path, held in or loaded through a
  constant address, a system SGPR (at or above `USER_SGPR` in CS; `s32` and up in the graphics stages, and the
  NGG `s3` in MS and GS) or a code-relative address. It wins over every other class, the undetermined one
  included, and it is empty in every stage.
- **Loaded through user data**: an `s_load_dwordx4` or wider at an address that starts from a user SGPR, 179,602
  at a static offset (0x540 at most) and 9,810 at a run-time one. No constant is added to any of these pointers,
  so `constant table address + user index`, which is the same arithmetic as `user pointer + constant` and which
  the classes could not tell apart, does not occur either. All 189,412 are in shaders whose `SFI0` sets the
  sampler-heap bit: no shader without it loads an S# from memory. Whether one of these loads goes through a
  descriptor table or through the sampler heap base of section 3.4 the shader cannot say; both are a user SGPR.
- **Literal**: four immediates, 105,235 in shaders with the sampler-heap bit and 69,561 in shaders without it,
  which sample with nothing else. 28,659 of them, in 6,824 shaders, build one dword with `s_brev_b32`.
  `tools/shader_meta.py` does not fold that instruction, so `literal_samplers` in the index does not list those
  S#s yet and the layer cannot register them from it.
- **Undetermined** (0.66%, in 300 shaders): with every edge followed, some path brings something else into the S#
  registers. 2,355 read the S# back from a VGPR it was spilled to, and one path does not save it there (on the
  paths that do, it is a load through user data); in 64 the paths bring a literal and a load through user data;
  in 1 a path brings a value the dataflow does not follow. All of them go away with one kind of edge: the S# is
  made or saved inside a region the compiler jumps over with `s_cbranch_execz` when no lane is active, and the
  sample comes after the region. Without the taken edge of those branches (the region always runs, as it does
  whenever a lane is active) the same analysis gives all 2,420 a class: 2,419 loaded through user data, 1
  literal. The table keeps them apart because that second reading trusts that nothing samples on the skipping
  path; lane masks are not modelled.

So, as far as the shaders can answer it, this corpus gives no evidence of a driver sampler table: no instruction
lands in the other-base class, and the 2,420 undetermined ones are not shown to be free of it by the conservative
reading, only by the one that assumes nothing samples on an exec-zero skip.
What the census does not show: it reads the shaders, not what the driver writes at the addresses they load from;
the graphics stages' user count is the translator's 32, not a measured one (question 3 in section 8.2); and it
cannot say which S# is a root signature's static sampler. The literals are the candidates, since a static sampler
is known when the shader is compiled and half of the sampling instructions build their S# from immediates, but
that is a hypothesis until the root signatures are matched against them. Reproduce with
`python -I tools/sampler_source_census.py <work>/shaders/dxbc/disasm --jobs 4 --expect-none other_base`, which
exits 1 if an instruction lands in that class.

**What the console's own D3D12 libraries show** (names only, read statically from a user's console system-file
dump; nothing from those files is in this repository):

- Two root-signature ABIs exist. The driver's validation messages name a flag
  `D3D12XBOX_ROOT_SIGNATURE_FLAG_FORCE_MEMORY_BASED_ABI`, and require it for ray tracing (XDXR) and for pipelines
  with an amplification shader. So the default is the register-based ABI this section describes, and a root
  signature can opt into a memory-based one. The shader compiler has the matching switch,
  `XBOX_FORCE_MEMORY_BASED_ABI`.
- The ABI is decided when the shader is compiled, not by the driver at bind time. The shader compiler has the
  intrinsics `__XB_GetShaderUserData` and `__XB_GetRootSignatureSpillPointer`. That answers the "larger than
  32 dwords" question above: parameters that don't fit in the user SGPRs spill to memory, and the shader reads a
  spill pointer from its user data. Because the game ships precompiled shaders, each shader's own user-SGPR reads
  (and any loads through a spill pointer) are the authoritative map; the metadata below is how to recover it.
- `d3d12_xs.dll` is a thin front end (about 50 KB). It loads the user-mode driver, `xgs12_10.dll`, which implements
  the device and command lists and exports `UMD*` entry points. `xg_xs.dll` exports the texture layout functions
  (`XGComputeTexture2DLayout`, `XGComputeOptimalSwizzleMode`, ...), the reference for untiling textures.
- The driver keeps the standard D3D12 root-signature cost limit: 64 dwords, with descriptor tables costing 1 and
  root views 2.

**Required build metadata.** To solve the mapping without guessing, `spirv/index.json` should gain, per shader:

- the set of user-SGPR dwords read before first write, and whether each is used as a 32-bit constant or a 64-bit
  pointer;
- the descriptor stride(s) implied by descriptor address arithmetic;
- whether the shader uses the code base (`s_getpc_b64`) and, if so, in which stage;
- the literal S# values it builds from immediates;
- the input signature (semantic name/index, register, mask, component type), not just the register numbers;
- the FSSM content hash (the PSO lookup key, section 3.6);
- the descriptor-table / heap-direct-indexing mode the shader was compiled for.

The runtime can then reconcile the root signature with the shader's reads and fail loudly on a mismatch instead of
rendering silently wrong.

`tools/shader_meta.py` (invoked by `tools/rdna2spirv.py` and verified by `tests/test_shader_meta.py`) now emits these
fields into `spirv/index.json`: `user_sgpr_reads` (`const32`, `ptr64_lo`/`ptr64_hi`, `ptr32_lo`, `vsharp`, `tsharp`,
`ssharp`), `descriptor_strides`, `uses_code_base`, `literal_samplers`, `input_signature` (ISG1 semantic name/index,
register, masks, system value, component type), `fssm_sha256`, and `descriptor_access` (DXBC `SFI0` direct-indexing
bits 25/26 plus table-load and high-dword-truncation flags). A missing or malformed ISG1 signature leaves the vertex shader
without recorded inputs. In that case the runtime accepts any input layout when filtering shader candidates.

### 3.6 Shader identity, PSOs and feature selection

The Xbox native graphics pipeline descriptor identifies stages by the digest in a DXBC HASH part (see
`docs/fspx.md`, kind 7). The shaders stage writes `<work>/shaders/spirv/native_hashes.txt` from the user's
carved DXBC containers and translation index. After the version header, each line contains the 16-byte digest
in lowercase hex, the translated stage and a container path relative to the index, separated by tabs.
Containers sharing a digest must agree on stage and FSSM hash. A collision fails the stage and preserves the
previous catalog. Rerunning the shaders stage checks its source fingerprint, including the catalog writer.
To add or refresh just the catalog in an existing translation, run
`python -I tools/native_pso_hashes.py "<work>/shaders/dxbc" "<work>/shaders/spirv"`.

A D3D12 PSO carries DXBC containers, not SPIR-V. The layer identifies a shader by hashing the **FSSM payload** (the
native RDNA2 program inside the container) and looking it up in a build-time table keyed by `(stage, hash)`. The
build emits that hash for every translation; the runtime extracts the FSSM chunk from the DXBC container the same
way `tools/fssm_unpack.py` does. The current `spirv/index.json` is keyed by carve name, which the runtime may not
have; add the content hash to the index.

Pipeline creation:

- `D3D12_GRAPHICS_PIPELINE_STATE_DESC` is supported (`CreateGraphicsPipelineState`), as are the compute desc and the
  `D3D12_PIPELINE_STATE_STREAM_DESC` subobjects (`CreatePipelineState`; "Implemented (D3D-29)" below): the stream's
  subobjects are parsed in order, and an unknown type is `E_INVALIDARG`.
- Build `VkPipelineShaderStageCreateInfo` from the translated module and the index entry. Mesh shaders use
  `VK_EXT_mesh_shader` (`AS`/`MS` subobjects → task/mesh stages); geometry uses the geometry stage; compute uses a
  compute pipeline.
- The index's `max_vertices`/`max_primitives` already size the SPIR-V mesh output arrays. Vulkan has no
  per-pipeline mesh output limit: check them against
  `VkPhysicalDeviceMeshShaderPropertiesEXT::maxMeshOutputVertices`/`maxMeshOutputPrimitives` and fail at pipeline
  creation if the device is smaller.
- Vertex input comes from the PSO input layout plus the shader's ISG1 signature. The translator's rule is
  **location = D3D input register**, not the semantic's HLSL slot. Match the layout element's semantic name/index
  to the ISG1 element to get the register, then create a `VkVertexInputAttributeDescription` with that location
  and the DXGI format. Resolve `D3D12_APPEND_ALIGNED_ELEMENT` against the preceding element and honour
  `InputSlotClass`/`InstanceDataStepRate`. The index entry needs the semantic map (section 3.5).
- Enable the features the index entry lists: barycentrics, fragment interlock, mesh, geometry, Float64, Int64,
  buffer device address, storage image without format, maintenance8.
- Request the subgroup size each shader was translated for (`requiredSubgroupSize` 64 or 32) where the device lists the stage
  in `requiredSubgroupSizeStages`, and say so in the log where it cannot (section 3.2, "Subgroup size").
- Pipeline caches: graphics and compute PSOs use one `VkPipelineCache` per device. On first use the runtime reads
  the `pipelines` build stage's `pipeline_cache.bin` beside the executable if its Vulkan header matches the device
  and pipeline-cache UUID. When the file is absent or incompatible, the runtime starts with an empty cache.
  The offline checker matches the runtime's layout and subgroup request for compute shaders with no descriptor
  bindings and the standard 144-byte push-constant range.
  The runtime writes its own `pipeline_cache.session.bin` on device teardown, and, since a title never releases its
  device, when `gta6rt` ends with the device alive: at the run's exit, at a console close and from an
  unhandled-exception filter (a fault in the title or its DLLs, the int3 after a device loss). The title's own
  `SetUnhandledExceptionFilter` is served by the runtime, which keeps its filter first and passes the exception on.
  That exit write (`save_pipeline_caches_at_exit`, `shims/d3d12_compile_exit.h`) waits 1 s for a build still running
  on a title thread, then writes the pipelines finished so far, with 3 s for the write itself. A compatible session file
  initializes the live cache directly if there is no offline seed. With both files present the session cache
  merges into the offline seed. `LEONIDA_DIRECT_SESSION_CACHE=0` also uses a merge when only the session exists.
  The files must match the Vulkan vendor, device and pipeline-cache UUID. Vulkan supplies an opaque snapshot.
  Replacing part of the previous file would corrupt it.
  `LEONIDA_PIPELINE_CACHE_BYTES` caps what a session adds, at a checkpoint or at teardown (an offline `pipeline_cache.bin` seed, which the driver hands back as part of the union, is not counted; 4 GiB by default, zero disables
  the session file).
  The session file can't keep a crashed run's compiles: it is written at teardown and at the optional checkpoints
  below only. The heavy pipeline cache (RT-HEAVY-PIPELINE-CACHE, `runtime/src/heavy_pipeline_cache.h`) keeps the graphics ones; compute pipelines go to the per-pipeline journal
  described below. Every graphics build first asks the corpus cache with
  `VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT`. A pipeline the corpus doesn't have is built against a
  second cache, seeded from `pipeline_cache.heavy.bin` next to `pipeline_cache.bin`, so that cache holds only the
  corpus's misses.
  A build that takes at least `LEONIDA_HEAVY_PIPELINE_CACHE_MS` (500 by default) marks the heavy cache for writing.
  A thread of its own writes it whole: 3 s after the newest slow build, and no later than 20 s after the first unwritten
  one, through the same staging file and rename as the session cache. A crash therefore loses at most the last 20 s
  of slow builds, and the next launch starts with the rest. A write that fails, or whose
  `vkGetPipelineCacheData` returns anything but `VK_SUCCESS` (`VK_INCOMPLETE` means the cache grew after the size
  query, so the blob is truncated), leaves the previous file in place and the cache dirty: it is tried again 5 s
  later, the delay doubling with each failure in a row up to 2 min, so the cache is not left unsaved until teardown
  and a write that keeps failing does not spin. At device teardown the thread stops, and the cache is
  written if any build went into it. Fast builds are kept that way: without an offline seed every build is a miss,
  so they no longer reach `pipeline_cache.session.bin`. `LEONIDA_HEAVY_PIPELINE_CACHE_MS=0` turns the heavy cache off, and with it the
  `pipelineCreationCacheControl` feature unless `LEONIDA_PIPELINE_COMPILE=cache-only` asks for it. A cache-only create
  (`cache-only`) is left as it is.
  `LEONIDA_SKIP_COMPUTE=<part of a name>[,<part>...]` (`all`: every compute program) never builds the matching
  compute programs at play and drops their dispatches, direct and indirect; the first dropped dispatch of each is
  logged. It is a way past a program that hangs the GPU while the program or its input is fixed, not a fix.
  `--precompile-pipelines` (below) builds them anyway unless it is given `--skip-listed`.
  `LEONIDA_STARTUP_CACHE_CHECKPOINTS=1` also schedules a snapshot after a successful pipeline build,
  at most once per 120 seconds. It starts on the compile worker when queued builds have drained and the device
  has no active builds. The driver fills a mapped staging
  file directly. A successful snapshot atomically replaces the session. An interrupted write preserves the
  previous cache; the next launch removes unheld staging files older than ten minutes.
  While the compute warm-up has builds queued that idle checkpoint never starts, and a boot that ends by a crash,
  a device loss or a kill never reaches teardown, so neither file used to keep that boot's compute pipelines.
  Compute pipelines are therefore journaled one file each (`src/pipeline_cache_journal.h`):
  `pipeline_cache.d/cs-<hash>.bin` beside the session file, named by a hash of the SPIR-V words, the stage flags and
  subgroup request, the push-constant range and the bindless layout. A build creates a `VkPipelineCache` from its
  file (if the header names this device and pipeline-cache UUID) and creates the pipeline against it; when the
  driver reports a compile (creation feedback without `APPLICATION_PIPELINE_CACHE_HIT`, or no file), that cache is
  written back to the file at once, through a staging file renamed over it. Without a file, and with
  `pipelineCreationCacheControl`, the shared cache is first asked with
  `VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT` when it was seeded from a file, so the offline seed
  still serves a first boot. NVIDIA 630.20 answers that flag with a compiled pipeline (seconds) instead of
  `VK_PIPELINE_COMPILE_REQUIRED`, so the first answer that took 250 ms or more without the cache-hit bit stops the
  asking, and that pipeline is built again against its journal cache so it gets a file (a fast answer without the
  bit is the driver's own disk cache, and a valid one; `LEONIDA_PIPELINE_JOURNAL_ASK_SHARED=0`: never ask). All
  driver calls of a compute build (shader module, journal cache, pipeline) run in one turn of the compile gate, with
  no file I/O in it: from the journal a build takes milliseconds, the warm-up worker makes them back to back, and
  three warm boots whose cache and module calls were outside the gate lost the device in a `vkQueueSubmit` 20-25 s
  in (no GPU fault, no TDR). The files take 2-3 MiB a pipeline on that driver (about 3.6 GiB for the 1700 a boot
  builds); `LEONIDA_PIPELINE_JOURNAL_BYTES` (32 GiB by default) caps them. At a device's first pipeline, staging files
  older than ten minutes are removed and, past the budget, the least recently used files until three quarters of it
  is left: a file's modification time is its last use (written, or touched, at most hourly, when a build finds its
  pipeline there), so files no build names any more after a translator regeneration go first.
  Measured on an RTX 4070 Ti (driver 630.20): 17 compute modules of 0.6-0.7 MB SPIR-V took 222 s to compile and
  0.04 s to create from their cache; a boot's ~1000 warm-up builds of 128 KiB or more took 81 s cold and 1.8 s from
  the journal, and the submissions' compile-gate waits went from 77 s to none. Nsight Aftermath crash dumps
  (`LEONIDA_AFTERMATH=1`) appear to make the driver compile again whatever the caches hold: boots loading the same
  317 MB session file logged no compute build of 2 s or more without Aftermath and 88-95 with it. The
  VK_NV_device_diagnostics_config flags alone do not (vk_pipeline_check `--nv-diagnostics`: 17 of 17 cache hits).
  `LEONIDA_PIPELINE_JOURNAL=0` (or `LEONIDA_PIPELINE_CACHE_BYTES=0`) turns the journal off.
  `LEONIDA_PIPELINE_CACHE_DIR=<folder>` moves the journal with the other files, so builds of gta6rt in several
  folders can share one; the file names and the driver's checks keep a pipeline from one build from being taken
  for another's.
  `gta6rt --precompile-pipelines [--threads N] [--big-threads N] [--all] [--smallest-first] [--limit N]
  [--min-bytes N] [--max-bytes N] [--filter PART]... [--skip-listed] [--list]` fills the journal ahead of play, with
  no game: it
  creates the Vulkan device as `--run` does (so the same environment gives the same journal names and driver keys:
  LEONIDA_SHADERS, LEONIDA_PIPELINE_CACHE_DIR, and LEONIDA_DEVICE_FAULT/LEONIDA_AFTERMATH, whose diagnostics config
  makes other pipelines) and builds every compute entry of the library through the dispatch path
  (`build_compute_pipeline`), largest first, on below-normal-priority threads. Entries with a usable journal file are
  skipped (only their header is read), so it can be stopped and started again. Running as gta6rt.exe it also fills
  NVIDIA's disk cache, which that driver keys by the executable's name. Measured on an RTX 4070 Ti (driver 630.20),
  4 threads: modules of 3.6-5.8 MB took 46-145 s each, 12 in 312 s, about 3.5 GB of host memory each while in the
  driver (14.5 GB peak for four). Graphics pipelines are not covered: they are built at a title's draw from its PSO
  description and the bound formats, and take milliseconds.
  `LEONIDA_SKIP_COMPUTE` does not narrow the build: it is a play-time setting, players run the pre-compile with the
  game's environment, and a program left out here would stay out of the journal after the list is shortened. The run
  says how many entries the list names and builds them; `--skip-listed` leaves them out on purpose. The last line
  counts what was built (compiled, or found in a cache), what was journaled already or being built by another
  process, what failed, and what was left out and why: by `--skip-listed`, not buildable on this device (no SPIR-V,
  set-0 bindings without a bindless set, workgroup memory over the limit), or filtered out (`--filter`, the size
  bounds, `--limit`). The 89-167 s per program behind the skip list of the large `library/` programs came from
  wave64-emulated packs. Measured with a native wave64 pack on an RX 7900 XTX (RADV, Mesa 26.3.1, Wine): all 721
  programs of 2 MiB or more built, 360 of them in 170 s on 32 threads, 2.5-32 s each, the largest (13 MiB) in 28.5 s
  alone, at a peak of 14 GiB of RAM for the process. With all of them in the journal and no skip list, two world runs
  took them all from the journal (none built in the game) and ended as runs with the skip list do, in an idle-queue
  device loss or a stop with the GPU idle ([running.md](running.md#settings-every-platform-needs)); whether their
  output is correct is not measured.
  `LEONIDA_COMPILE_HELPER=1` (`shims/d3d12_compile_helper.h`) keeps first-ever compiles out of the title's process:
  the driver's locks, which a long `vkCreateComputePipelines` holds and a `vkQueueSubmit` of the same process waits
  for, are per process. A compute build of `LEONIDA_COMPILE_HELPER_MIN_BYTES` or more (128 KiB) with no journal file
  writes a request beside the journal (`requests/<name>.req`: the entry and a priority, 0 for a use, the module size
  for a warm-up) and starts the helper at the first one (`gta6rt --compile-server --parent <pid>`, the same executable
  and environment, so the same device and journal names). With asynchronous builds (`LEONIDA_ASYNC_PIPELINES=1`, the
  intended mode) nothing waits: the build answers deferred, the variant goes back to idle, the dispatch is skipped and
  counted, and each later use looks at the request (a file check, the module is not read again) until the journal
  file is there and the pipeline is created from it, a cache hit. A warm-up only asks, in either mode, so the warm-up
  worker writes its requests ahead and the helper's threads (`LEONIDA_COMPILE_HELPER_THREADS`, 2) build them in
  parallel, the most urgent first; a warm-up the helper builds is never held back while the title submits. Without
  asynchronous builds a dispatch waits for its file, holding nothing of the driver's (`LEONIDA_COMPILE_HELPER_WAIT_MS`,
  default 120 s, 0 for no limit). A failure (`requests/<name>.failed`, the helper gone) builds the pipeline here as
  before, and so does a request that is no longer listed (`<name>.req` gone with no answer) or one still pending after
  `LEONIDA_COMPILE_HELPER_PENDING_MS` (default 120 s): the helper cannot hold a dispatch forever. The helper
  checks that its journal name is the one asked for before it compiles (the name is a hash of the module and the
  device, no driver call), ends with the process that started it, runs alone per journal,
  and with nothing asked fills the rest of the library, smallest module first, on
  `LEONIDA_COMPILE_HELPER_IDLE_THREADS` (1, always one fewer than its threads) at idle priority; the journal names it
  worked out are kept in `requests/names.idx` (with each module's size and time and what the names depend on), so a
  later helper does not read every module again. `requests/<name>.building` is a lock file a process holds for the
  whole build (no sharing, deleted by the OS when that process ends, killed or not), whose contents name the process:
  the helper and `--precompile-pipelines` leave an entry its holder is building to it, and a file no process holds is
  stale whatever process id it names. Measured, RTX 4070 Ti,
  driver 630.20, a boot with an empty journal and the helper waiting (no asynchronous builds): the helper compiled 171
  pipelines of 128 KiB or more (148 s in its driver), every one of them was a hit in the title's process (at most
  2 ms), and no submission waited at the compile gate.
  Under Proton, pass a Wine path, for example
  `LEONIDA_PIPELINE_CACHE_DIR='Z:\opt\leonida\cache'`. Async warm-up workers publish to the same directory.
  For a disposable session, `LEONIDA_PIPELINE_JOURNAL_SEED_DIR=<absolute existing directory>` can name a separate,
  trusted read-only set of per-key compute journal files. The runtime prefers a compatible writable entry, then tries
  the same key in the seed; both use the existing size and device/driver header checks. A seed hit neither copies an
  entry nor refreshes its mtime. All writes, LRU touches, staging cleanup, eviction and byte accounting belong to the
  writable journal only, including replacement of an incompatible local file. The seed must be distinct from the
  writable `pipeline_cache.d`, not its ancestor or descendant; an explicitly unusable or overlapping root disables
  the journal. This setting does not grant access or authenticate cache data. Use only trusted, privately produced
  files on a read-only mount/ACL with stable, disjoint directories and no linked files between them. Leave unset for
  the original single-directory behavior. Per-key names are unchanged: externally precompiled files for the same
  pipeline identity can be read without a persistent *writable* shared journal.
  During normal runtime, a compatible local or external entry is used before requesting the helper.
  The offline precompiler and compile-server instead read and populate only the writable journal, even
  when a separate seed is configured. An external hit is not a published response to a helper request;
  its readiness check still requires a compatible file at the requested writable path. Both modes validate
  the configured directory pair before enabling journaling.
  To check the two tiers without game files, build `d3d12_compute_journal_test`, then run
  `python scripts/check_compute_journal_overlay.py <path-to-d3d12_compute_journal_test.exe> --require-hit`.
  This uses four fresh processes, two authored compute shaders and a disposable cache. It checks dispatch results,
  reported application-cache hits (where supported), local publications, and that old seed contents/mtime stay put.
  `LEONIDA_TRACE_PIPELINE_COMPILE=1` records each graphics and compute build's trigger, thread, elapsed time,
  SPIR-V size, shader names and Vulkan application-cache feedback (1 hit, 0 miss, -1 unavailable). It also
  times cache reads, merges, writes and draw/dispatch pipeline lookups that take at least 1 ms.
  `LEONIDA_ASYNC_PIPELINES=1` schedules cold first-use builds on the warm-up worker ahead of speculative work; how long
  a submission waits for them, and what is skipped, is "Pipelines still being built" below. Strict replay builds
  synchronously and waits for every build. `LEONIDA_COMPUTE_WARMUP=0` restricts the worker to pipelines requested by
  the title.
  `LEONIDA_PIPELINE_ORDER_LOG=1` records the order in which the title first asks for pipelines
  (RT-PIPELINE-PREWARM-ORDER, phase 1; `runtime/src/pipeline_order.h`). It is off by default: nothing reads the file yet,
  so a run that does not ask for it writes nothing and its pipelines carry no hook. For each pipeline and variant (the
  unbound-slot mask for graphics, 0 for compute) that a dispatch or draw names for the first time, one line is
  appended to `pipeline_order.log` beside `pipeline_cache.session.bin` (so `LEONIDA_PIPELINE_CACHE_DIR` moves it):
  `P <c|g|m> <key> <variant hex> <created ms> <first use ms> <build ms or -> <ready 0|1> *<checksum>`, with the
  times in milliseconds since the process started. The key is the SHA-256 of the title's own description of the PSO
  (`GetCachedBlob`'s key; a mesh pipeline's MS is folded in), not of the SPIR-V, so a translator change keeps it. A
  build that ends after its line was written adds `B <key> <variant hex> <build ms> *<checksum>`; a session starts
  with `S 1 <vendor hex> <device hex>`. A compute pipeline's `P` line is followed by
  `E <key> <variant hex> <name hash hex> *<checksum>`: the name hash is 16 hex digits of FNV-1a 64 over the name of
  the library entry the pipeline was made from (the name itself is not written). A reader from before `E` lines
  skips them (counted as lines not read). Each line ends with an FNV-1a checksum: a cut or damaged line is skipped on
  read, never fatal. The hook sits in replay's `ComputePipeline::requested` / `GraphicsPipeline::requested`, called
  where a submission is prepared, also for a handle that exists already (a warm-up build, a cached one); the
  recorder decides "first" with one compare-and-swap in a table made at start, never blocks and allocates nothing (a non-blocking `try_lock` on the first request of a pipeline and of a variant),
  and a thread of its own appends every 250 ms, so a killed process loses at most that. The record holds 8192 lines
  per run (the rest is counted, not kept); a file past 8 MiB is set aside as `pipeline_order.log.prev`. At exit (the
  exit cache save, or the device's release) one line gives the counts: `[rt] pipeline-order: requested N, already
  ready M, median lead X ms`. "Lead" is first use minus creation. A PSO made from a pipeline state stream on the
  console path is created when it is first bound, so its lead is the title's, not an upper bound on a head start.
  A root signature the runtime does not know is hashed by its address, so the key of a pipeline made with one can
  change from one run to the next. The file is local, like the cache files, and is not meant to be attached to a
  report: quote the counts. It changes no build, order or cache.
  `LEONIDA_PIPELINE_PREWARM_ORDER=<file>` reads such a log at boot and builds, ahead of their first draw, the graphics
  and mesh pipelines an earlier run asked for first (RT-PIPELINE-PREWARM-ORDER, phase 2; `runtime/src/pipeline_prewarm.h`).
  `1` names `pipeline_order.log` beside the session cache; it is read before this run's own record is appended, so
  both variables can be on together. Of the runs recorded on this GPU (vendor and device id), the one with the most
  records is read, the newest on a tie. When the title creates a PSO whose key is in it, each recorded variant is
  queued on the compile workers as a warm-up at its recorded rank: after every demanded build, one warm-up at a time
  and none while a demanded build runs (`PipelineCompiler`; the compile-gate finding in "Compile workers" below is
  why), ahead of the compute warm-up's size order and before the cache checkpoint. A variant 0 built at create is
  skipped. A console stream kept for its first bind (`LEONIDA_PIPELINE_COMPILE=on-demand`) is otherwise parsed only
  there, which would leave the prewarm no lead: when its pipeline is in the plan, the parse is queued as its first
  prewarm job instead (at the rank of its first recorded variant, on a compile worker), and its variants follow with
  their ranks. A bind that comes first waits for that parse, which it would otherwise have done itself. Records are taken in order until their recorded build times add up to
  `LEONIDA_PIPELINE_PREWARM_BUDGET_MS` (default 300000; a record with no build time counts 1000 ms), and at most
  `LEONIDA_PIPELINE_PREWARM_MAX_ENTRIES` (default 4096). Compute records are left to the compute warm-up and to
  `gta6rt --precompile-pipelines`. A missing or unreadable file, a file with no readable record and a file from
  another GPU are ignored with one `[rt] pipeline-prewarm:` line; a damaged line is skipped, as are line types a v1
  reader does not know (`E`). A queued prewarm holds no reference: a PSO released first drops it. One that is already
  running when the title submits still holds the compile gate (`LEONIDA_COMPILE_GATE`), as any warm-up does.
  A queued build holds no reference to its PSO or device (`PipelineCompiler`, `runtime/shims/d3d12_pipeline_compile.h`).
  A PSO's last release drops its queued builds and returns at once; one already running is left to finish, and the
  worker deletes the PSO after it (`PipelineState::last_release`). The PSO's device reference goes at the release, so
  the release that tears the device down waits for that build, drops the device's cache checkpoint, and the last
  device stops and joins the worker before any Vulkan object goes; the session cache is written after that. gta6rt,
  which ends with the device alive, drops the queue and waits up to 2 s for a running build before `ExitProcess` and on
  a console Ctrl+C, Ctrl+Break or close; after that nothing is queued, and a first use builds in place
  (`d3d12_compile_teardown_test`).
  `GetCachedBlob` records the vendor, device, driver version, `pipelineCacheUUID` and a SHA-256 of the description
  without `Flags`, sealed by a checksum (`runtime/src/cached_pso.h`). The answers are the host's, measured on an
  RTX 3060 and WARP: the same description is accepted; another
  shader, root signature content or state, a changed byte, a truncated blob or a pointer with size 0 give
  `E_INVALIDARG`; a null pointer with a size is ignored; another adapter gives `D3D12_ERROR_ADAPTER_NOT_FOUND` and
  another driver `D3D12_ERROR_DRIVER_VERSION_MISMATCH`. `ID3D12PipelineLibrary` is an in-memory map plus the Vulkan
  cache. `d3d12_pipeline_library_host_test` compares public argument checks with the host. It runs each host
  call in a separate process because a fault can leave the device unusable. It reports serialized sizes and the
  result of flipping a trailing byte for each device's own format. `d3d12_pipeline_library_test` checks that the
  layer rejects a nonzero reserved field in its `LPL1` header. On Windows 11 build 26100 (RTX 5070 Ti,
  driver 610.74), all 72 public-argument comparisons matched. The previous 74-value run (RTX 3060, driver
  610.88) included the two results from the trailing-byte case.
  - `CreatePipelineLibrary`: a foreign, truncated or corrupt blob, the other device's blob, or a null pointer with a
    size is `E_INVALIDARG`, and `*ppPipelineLibrary` is left as it was. One of the layer's own blobs from another
    format version is `D3D12_ERROR_DRIVER_VERSION_MISMATCH`, the documented code for a cache from another driver.
    A pointer with size 0 is an empty library; a longer blob than the header is accepted; a null output is
    `S_FALSE`.
  - The library is a device child, not a pageable object: `QueryInterface(ID3D12Pageable)` is `E_NOINTERFACE`.
  - `LoadComputePipeline`/`LoadGraphicsPipeline`/`LoadPipeline` with a null output are `E_INVALIDARG`, as is an
    unknown or empty name. `Serialize` with a buffer smaller than `GetSerializedSize()` is `E_INVALIDARG`. The
    serialized size and bytes are each device's own (the host's empty library is 900 bytes, the layer's 16).
  - Not compared: the calls the host runtime faults on (`Serialize(nullptr, n)`, `StorePipeline` with a null PSO,
    `Load*` with a null name or description); the layer answers `E_INVALIDARG` for each.
- Compiles at draw time: the first draw of a graphics pipeline, the first dispatch of a compute pipeline the warm-up
  has not built, and each variant are compiled for the submission that needs them. `prepare()` leaves each one that is
  not built yet to `PipelineCompiles`, once per pipeline and variant; `VulkanQueue::execute_now` then compiles them
  side by side on the device's workers (`runtime/src/pipeline_workers.h`), the submitting thread among them, and writes
  the command buffer when all are built. Every draw and dispatch has its own pipeline: none is left to a later frame. A
  pipeline the driver refuses has the result it had when `prepare()` built it: a draw or an indirect dispatch fails the
  submission, a dispatch is dropped (strict replay: fails it too). The resource lock is held while they compile, as it
  is when each is compiled alone, so a PSO released meanwhile waits for the submission. The workers have ended before
  the session cache is read at device teardown. `LEONIDA_PIPELINE_THREADS=<n>` sets the workers: unset, half the
  logical processors and at most 8; `0` compiles each pipeline on the submitting thread where `prepare()` meets it.
  Large pipelines (below) are compiled within a memory budget, and the variants of one PSO one after another.
  `LEONIDA_ASYNC_PIPELINES=1` is not changed: its builds stay on the compile worker. Measured with synthetic programs
  never compiled before and the warm-up off (RTX 3080, NVIDIA 621.24, Windows 11, 16 logical processors, 8 workers,
  three runs each): the first submission of 192 graphics pipelines takes 126 to 143 ms instead of 861 to 882, of 192
  variants 124 to 130 instead of 856 to 863, of 96 compute pipelines 74 to 81 instead of 486 to 519, and the process
  holds about 50 MiB more (the driver keeps memory for each thread that compiles). One `VkPipelineCache` is enough: one
  cache per thread, merged, was about as fast. Tested by `pipeline_workers`, `replay_pipeline_compiles` and
  `d3d12_pipeline_workers`.
- Large pipelines: a compile takes the driver some hundred bytes of memory for each byte of SPIR-V
  ([pipeline_check.md](pipeline_check.md), "Limiting concurrent compilation of large modules"), so compute modules over
  128 KiB and graphics pipelines of 1 MiB or more compile side by side only while their estimates (512 bytes for each
  byte of SPIR-V) fit a budget: three quarters of the smaller of the free RAM and the room under the commit limit, read
  again whenever a place at the gate (`runtime/src/large_module_gate.h`, one for the process) is given or returned. One
  always gets in when none is, so a pipeline larger than the budget compiles alone. An estimate counts in full until
  the compile ends, although the driver has taken part of it by then. They get in in the order they came; while the
  first in line does not fit, the next behind it that does may pass, as long as all that have passed it would fit the
  budget together with it, so that a stream of small ones does not keep it out. The ones a submission waits for (and
  those of `LEONIDA_ASYNC_PIPELINES`) go before the warm-up's, which take a quarter of the budget at most and none while
  a submission that has large ones is compiling on its workers. A thread that waits for its place holds no PSO's lock,
  and one that has a place waits for none. `PipelineCompiles::build` gives a submission
  as many jobs for large ones as its smallest could be in at once, largest first; where the next one does not fit, the
  job compiles small ones in the meantime. `LEONIDA_SERIAL_BIG_COMPILES=1` compiles them one after another instead, as
  before there was a budget, `=0` has no limit, `LEONIDA_BIG_COMPILE_BUDGET_MB` fixes the budget. Measured with
  synthetic integer modules never compiled before and the warm-up off (RTX 3080, NVIDIA 621.24, Ryzen 7 5800X3D, 8
  workers and the submitting thread, three alternating runs each): the first submission of 16 modules of 256 KiB takes
  0.8 s instead of 4.0 to 4.8 s, 16 of 512 KiB 2.0 to 2.2 s instead of 10.2 to 11.4, 15 of 1 MiB 5.8 to 6.1 s instead
  of 29.2 to 31.2, 12 of 0.5 to 2 MiB 8.0 to 10.5 s instead of 38.9 to 39.9. While the 15 of 1 MiB compile, private
  memory peaks 656 to 676 MiB over what it was, instead of 140 to 147 (one compile takes 85 to 131 bytes for each byte
  of SPIR-V here, and the driver keeps memory for each thread that compiles). With a budget of 1536 MiB, two at once,
  they take 18.5 s and peak 202 MiB over. Not measured: other drivers, and the title. Tested by `big_compile_gate`,
  `replay_pipeline_compiles` and `d3d12_big_compute`.

**Pipelines still being built** (`PipelineState::queue_build` in `runtime/shims/d3d12_vulkan_device.cpp`). Lazy
pipelines are built at first use, and on the 4070 Ti (NVIDIA 630.20) a large compute pipeline takes the driver 3-10 s,
some 37 s. A submission builds the ones it needs through `PipelineCompiles` and `PipelineWorkers` (above), with
`resource_mutex_` released while they compile and the large ones within the memory budget of `LargeModuleGate`.
#2098 had a second path here, a pre-pass in `VulkanQueue::execute` that built them one after another on the
submitting thread; at the maintainers' decision it was replaced by this one when #2098 was merged.

- **Asynchronous builds** (`LEONIDA_ASYNC_PIPELINES=1`). Prepare asks `queue_build`, which answers the handle once
  built and otherwise queues one compile-worker job at replay's priority and answers null: the draw or dispatch is
  skipped, the list's other commands replay as recorded, and a later submission uses the pipeline. The job also starts
  at record time already: at `SetPipelineState` for compute and at each draw for graphics, with the variant its bound
  targets give (`request_pipeline_build`). A compute build the compile helper is still making (`LEONIDA_COMPILE_HELPER`)
  publishes nothing, so its next use asks again.
- **Compile workers** (RT-ASYNC-COMPILE-POOL, after MoonEyes' design in #2077). The jobs above, the compute warm-up
  and the cache checkpoint run on `LEONIDA_COMPILE_THREADS` workers (half the logical processors, 2 to 8). With one
  worker a driver compile of minutes held every pipeline a submission asked for behind it, and their draws and
  dispatches were skipped meanwhile. A worker now takes the most urgent job it may: demanded builds first, in the
  order they came, then warm-ups by module size, then the checkpoint, which waits until no worker compiles. Large
  builds (compute over 128 KiB, graphics of 1 MiB or more) and warm-ups together take at most threads-1 workers, so
  one is always free for a small demanded build; at most `LEONIDA_COMPILE_HEAVY_MAX` large builds run at once (default
  threads-1, 1 with `LEONIDA_SERIAL_BIG_COMPILES=1`), each still within the memory gate; warm-ups run one at a time,
  as before, and none starts while a demanded build is queued or running, since each holds the compile gate for its
  whole create (MoonEyes measured in #1608 that with the gate on, submissions waited 142 s in all behind warm-ups of
  about 800 KB, and that with `LEONIDA_COMPILE_GATE=0` the same boot reached game state 3); and a PSO has one build
  running at a time, so
  `after_running` and the teardown rules hold as they were. `LEONIDA_COMPILE_THREADS=1` is the layout before the pool:
  one general worker and the graphics-only one; there too a PSO has one build running at a time (two variants of one
  graphics PSO, one on each worker, let its last release delete it under the other build, qhukz13). A submission still
  waits at the compile gate for every compute create in flight (and graphics creates too with `LEONIDA_COMPILE_GATE=adaptive` or `1`; the classic default gates compute builds only), so more workers
  do not shorten that wait (`SubmitMutex::CompileTurn`; `LEONIDA_COMPILE_GATE=0` turns it off).
- **Only the large ones asynchronous** (`LEONIDA_ASYNC_MIN_BYTES=<n>`, with `LEONIDA_ASYNC_PIPELINES=1`). With every
  pipeline asynchronous, the small lighting and post programs are skipped with the large ones until the worker gets to
  them, and on an RTX 4070 Laptop (NVIDIA 621.40) the world phase was reached with the screen black; with none, a
  submission waits for each ray-tracing compute program of 2-26 MB of SPIR-V, 2-3 minutes each there, and 17 were built
  in 30 minutes (measured by OreoFR on the wave64-emulated pack). With the variable, a pipeline under n bytes of SPIR-V
  (compute: its module; graphics: all its stages, the size the large-compile rules use) takes the path it takes with
  asynchronous builds off: no record-time request, prepare's `PipelineCompiles` builds it, and a compile-helper build
  waits for its file. One of n bytes or more is asynchronous as above. The choice is made once per pipeline at create
  (`async_build` in `src/vulkan_replay.h`, decided by `src/async_pipeline_size.h`). Unset, `0` or text that is not a
  decimal number of bytes (which is logged): every pipeline is asynchronous, as before the variable. A large build
  still goes through the compile gate and `LargeModuleGate` as before, so a smaller one a submission waits for can
  queue behind it there; `async_pipeline_size_test` covers the decision and `d3d12_m4_compute_async_small_waits` a
  submission whose two pipelines are under the threshold with the worker held (both dispatches run).
  `LEONIDA_ASYNC_MIN_BYTES_GRAPHICS=<n>` gives graphics and mesh pipelines (all their stages) a threshold of their
  own; unset, they take `LEONIDA_ASYNC_MIN_BYTES`, as before the variable, and compute always does. `0` makes every
  graphics pipeline asynchronous, as `LEONIDA_ASYNC_PIPELINES=1` did on its own. The reason: @MoonEyes's thread dumps
  (RTX 4060, `LEONIDA_ASYNC_MIN_BYTES=1048576`) showed the draw thread building lazy graphics variants under 1 MiB in
  place (`PipelineState::variant` -> `build` -> `vkCreateGraphicsPipelines`, 2.5-11 s each, back to back), so no
  present came for about 100 s. A graphics pipeline that is not built yet costs one skipped draw, while the image
  needs the small lighting and post compute programs, so graphics can stay asynchronous while those are built where
  they are needed. One startup line states both thresholds. `d3d12_pipeline_workers_async_graphics_zero` holds the
  compile worker with compute over its threshold and graphics at `0`: every dispatch runs, every draw is skipped, and
  the draws come once the worker is let go; `d3d12_pipeline_workers_async_graphics_unset` checks that graphics follow
  `LEONIDA_ASYNC_MIN_BYTES` without the variable.
- **Warm-ups while the title submits.** A compute warm-up holds the compile gate for its whole create, and every
  submission waits behind it: a native boot's submissions waited 140 s in all at the gate behind compute warm-ups, and
  past the story load the worker went through 694 warm-ups of modules over 128 KiB (those over 640 KiB take 4-25 s
  each) against 25 such builds a dispatch asked for (measured with a 128 KiB limit). With
  `LEONIDA_COMPUTE_WARMUP_LIVE_MAX_BYTES=<n>` a warm-up of a module over n bytes of SPIR-V is not started while the
  title submits (a submission in the last second, `src/pipeline_wait.h`); the pipeline is built at its first use
  instead. Opt-in: unset or 0, every warm-up runs, as on `main`, and `LargeModuleGate` (#1889) orders them behind a
  submission's compiles. Smaller warm-ups, and every warm-up before the first submission, run as before.
- **A bounded, adaptive compile gate, where the driver was measured for it** (`SubmitMutex::enter_submit`,
  `src/vulkan_device_fault.h`; `LEONIDA_COMPILE_GATE=adaptive` forces it, `classic` forces the classic one). The
  classic gate: compute creates only, each waited for however long it takes, no probes, as before. Unset or `auto`,
  the device measures which one this driver gets before the title runs (the next item). The adaptive gate is not on
  everywhere because of what it costs on a driver that does hold submissions behind a create (630.20, below): no run
  of it there has been reported yet. The gate keeps a
  `vkQueueSubmit` out of the driver while a create is in it, since on NVIDIA 630.20 one that entered during a long
  `vkCreateComputePipelines` waited 10 s and lost the device. The waiting submission holds the submit lock, so all
  queues stop: on an RTX 4070 Laptop (NVIDIA 621.40) one compute create of a 2.3 MB module stayed in the driver for
  more than 54 s during the story load, the submission waited for all of it and the title's 30 s hang watchdog ended
  the process; with `LEONIDA_COMPILE_GATE=0` the same build went past that point. With the adaptive gate a
  submission waits at most `LEONIDA_COMPILE_GATE_WAIT_MS` (15 s by default there) and then goes ahead; the creates in
  the driver at that moment hold no later submission back, and creates that enter after it are waited for again. On
  630.20 that loses only creates over 25 s (15 s here, then the driver's 10 s), which the watchdog ends anyway.
  On the same laptop the three submissions that went ahead came back from the driver in 11, 1 and 0 ms with the
  create still there, and no device was lost; but consecutive long creates made submissions wait the full 15 s again
  and again, and the title's watchdog still ended the run. So the adaptive gate adapts: every submission that
  enters the driver next to a create it no longer waits for is a probe. Two that come back in under 250 ms, at least
  1 s apart, with that create still in the driver turn the gate off for the rest of the run (a driver that holds a
  submission behind a create holds it as long as the create stays, and 630.20 held one for the full 10 s). Two, and a
  second apart, because back-to-back creates take and release that driver's lock, so one submission can slip in
  between; the second costs no extra wait, since after one has gone ahead the next enter at once while the create is
  still there. One probe that stays 2 s or more keeps the gate on for the rest of the run (presents and idle waits do
  not probe, below). Both verdicts are logged once. `LEONIDA_COMPILE_GATE=1` gates always and never probes.
  In the next run on that laptop the story load passed, but no wait reached 15 s, so no probe came: 80 submissions
  waited 3-7 s each (90 s in all, the longest 6.7 s), back to back, and the title's watchdog ended the run at 433 s.
  So until a verdict is in, a submission also goes ahead once the submissions' waits at the gate in the last 10 s, its
  own included, add up to `LEONIDA_COMPILE_GATE_PROBE_MS` (2500 ms). A budget over a
  window rather than a shorter limit per wait, because it also counts many short waits, and one wait reaches it as
  soon as a 2.5 s limit would. Before the verdict the gate then stops the queues for at most about a quarter of any
  10 s, after a quick verdict not at all. On a driver that does hold submissions (630.20) the first probe now enters
  the driver after 2.5 s of waiting instead of 15 s, and loses the device if the create stays there 10 s more (the
  classic gate waits that create out). One probe per kind and run: it holds the submit lock while it is in the
  driver, so no other submission enters until it comes back, and it then gives the held verdict (2 s or more, back
  within 500 ms of the create leaving), after which no submission goes ahead beside a create of that kind before the
  held limit. The held threshold stays 2 s, well inside the 10 s such a driver gives before the loss. The limit and the
  budget apply with the title's watchdog kept kicked too (`LEONIDA_TITLE_WATCHDOG=0` and
  `LEONIDA_TITLE_WATCHDOG_KICK`): nothing ends the run then, but the stops still cost the frames, and an RTX 4060 run
  that way had 459 waits, 155 s in all and the longest 2.9 s, by t=202 s. Only `LEONIDA_COMPILE_GATE_WAIT_MS=0` (with
  no `LEONIDA_COMPILE_GATE_PROBE_MS`) or `LEONIDA_COMPILE_GATE=1` (with no `LEONIDA_COMPILE_GATE_WAIT_MS`) waits
  however long the creates take and never adapts.
  Tested by `submit_mutex` (no GPU); the probe budget is not yet measured in the title.
  The verdict is kept per kind of create, compute and graphics (mesh pipelines included), and graphics creates
  pass this gate and `LEONIDA_COMPILE_GATE=1` too (the classic gate, as before, only compute builds). On 621.40 every probe beside a compute create came back in
  0-12 ms (and on an RTX 4060 with 610.88, 133 of them), but a run whose gate those probes had turned off lost the
  device 9 s later with a submission held 7.8 s beside a mesh create, and with `LEONIDA_COMPILE_GATE=0` a
  `vkQueueSubmit` sat 54 s in the driver beside a 56 s mesh create (541 KB) and the device was lost. A probe counts
  for each kind that had a create in the driver from before the submission entered until it came back; a held one
  counts against all of them, since which create held it cannot be told. A kind that is off is still timed: a
  submission that stays 2 s or more beside one of its creates turns that kind back on for the rest of the run. A
  held probe (pending or off) counts only if the submission came back within 500 ms after a create of that kind,
  there when it entered, left the driver: a submission held behind a create comes back when the create does. On the
  4070 Laptop (621.40, 8 GB nearly full) a 36 s submission came back 18 s before the 58 s compute create beside it;
  counted as held, it re-armed the gate, which then kept the next submission 78 s behind compute creates. A
  synthetic probe on the same machine (one to three threads creating 2 MB graphics pipelines of 8-10 s each, with
  and without `VK_NV_device_diagnostics_config` and a shared pipeline cache) never held a submission past 80 ms.
  The recent leaves are kept in a ring, so a create that entered after the submission and left after the one it
  entered beside does not hide that one. Only submissions (`vkQueueSubmit`, `vkQueueBindSparse`) probe: presents and
  idle or semaphore waits pass the gate the same way but can take seconds on their own, so they judge nothing, and
  they never go ahead at the probe budget, only at the limit. While a create of a kind that holds submissions is in
  the driver, no submission goes ahead at the limit or the budget, and such a create is waited for even when one went
  past it before the verdict, up to the held limit (`LEONIDA_COMPILE_GATE_HELD_WAIT_MS`, 60 s; 0 waits however long,
  and a held limit under the plain one applies on its own): then the creates in the driver are gone past and the
  next submissions no longer wait for them. This is checked on every pass of the wait: once such a create leaves
  while creates of a kind with no verdict stay, the plain limit and budget apply again. A false
  "holds" verdict so costs at most that per create, not the rest of the run. On a device loss the log names every
  create in the driver (kind, program, SPIR-V size, thread, how long it had been there, whether submissions waited
  for it) and each kind's gate. Long graphics creates would leave the title's process only through the compile
  helper, which builds compute pipelines alone today.
- **`LEONIDA_COMPILE_GATE=auto`, the default: a calibration at device creation** (`src/compile_gate_policy.h`,
  `src/compile_gate_calibration.h`, `decide_auto_compile_gate` in `shims/d3d12_vulkan_device.cpp`). Before the
  `VulkanContext` (and its `SubmitMutex`) exists, so nothing is gated before the decision, a worker thread creates a
  compute pipeline written in code (a chain of `fract(sin(...))` steps and branches over a storage buffer, its
  constants from specialization constant 0 with a new seed each run, so neither a pipeline cache nor the driver's
  shader cache answers it; no game data) while this thread submits an empty command buffer to the device's queue and
  waits for its fence, over and over, until the create returns. At least 3 submissions back in 50 ms or less, before
  the create left and some in its second half, with none slower: adaptive. One of 250 ms or more that came back
  within 200 ms of the create leaving: the driver holds submissions, classic. A create under 300 ms is tried again
  larger (up to 4 trials, none started after 6 s); a slow submission the create does not explain, too few samples
  or any failed call: classic. The verdict is kept per vendor, device, driver version and pipeline cache UUID in
  `compile_gate_calibration.txt` beside the pipeline cache, so it runs once per driver; a failed call or an unclear
  verdict (a slow submission the create does not explain) is not kept, and the next launch measures again, so one
  busy moment on the GPU does not hold every later launch on the classic gate. After a wait that times out, the fence
  and command pool are destroyed only once the submission's fence signals (left alive on a lost device).
  `LEONIDA_COMPILE_GATE_RECALIBRATE=1` measures again. It is logged once, `[rt] compile-gate: auto: adaptive
  (calibrated on NVIDIA 621.40 in 0.7 s: 115 submissions came back in <= 0.5 ms beside a 722 ms create; cached in
  ...)` (`classic` and the reason otherwise). Only gta6rt's title process calibrates; `--precompile-pipelines`,
  `--compile-server` and the tests keep the classic gate, and a forced value never waits for it.
  Why a calibration can ask what an in-game probe cannot: the adaptive gate learns by letting a submission into the
  driver beside a create of the title's, which can take 30-60 s, and on a driver that holds submissions such a probe
  loses the device once it waits 10 s more. The calibration's create takes about a second and its submissions are
  empty, so on such a driver one is held about a second and comes back with the create; that cannot reach the 10 s
  the driver allows. What is known for the verdict to agree with, `MEASURED` but small (#2412,
  [comment 7217585003](https://github.com/ubiquitarianism/leonida/pull/2412#issuecomment-7217585003)): on an RTX 4070
  Laptop with NVIDIA 621.40 a synthetic probe never held a submission past 80 ms beside 8-10 s graphics creates and
  retail adaptive runs had no gate hang after #2418, and this calibration gives adaptive there (in 6 runs of
  `vk_compile_gate_calibration_test`, 53-125 submissions each, none over 0.9 ms, beside creates of 354-783 ms); an RTX 4060 with 610.88 turned the adaptive gate off with no
  regression reported; other 621.x users report fewer crashes with it; one freeze at about 231 s on an RTX 5070
  (621.x) was at a head without #2418 and is being rechecked. On 630.20 the calibration has not run yet: the
  expected verdict there is classic, from the 10 s hold measured beside a long compute create.

**Open:** whether the game uses `D3D12_PIPELINE_STATE_STREAM_DESC` for mesh/RT and which subobject types appear.
The trace will enumerate them.

**Implemented (D3D-29): pipeline state streams, mesh PSOs and `DispatchMesh`.**
- **Device:** `VK_EXT_mesh_shader` with `meshShader` is enabled where the GPU has it (`VulkanDeviceInfo::mesh_shader`).
  `taskShader` is not enabled, since the corpus has no amplification shaders. `D3D12_OPTIONS7` reports
  `MeshShaderTier` 1 only then; without the extension it reports `NOT_SUPPORTED` and mesh PSOs are `E_NOTIMPL`.
- **Streams:** `CreatePipelineState` parses the stream in the d3dx12.h layout: a subobject type, then its
  description at that description's alignment, each subobject pointer-aligned.
  - Every type up to `RASTERIZER2` is read.
  - The Vulkan backend selects console numbering when the PC reading fails or gives AS without MS and the
    console reading has a verified DXIL kind-13 mesh stage. Slot 86 keeps that selected layout with canonical
    tags and owned shader bytes so slot 55 can derive another PSO. `LEONIDA_XS_STREAM_ENUM=pc` selects PC only.
  - With D3D12 tracing, each unsupported stream has one `PipelineStream::Refused` reason. Accepted mesh PSOs
    have a separate `PipelineStream::Mesh` count. `LEONIDA_PSO_STREAM_AUDIT=1` also prints raw tags,
    shader stages and FSSM hashes for unsupported or failed streams. The call number joins to the backend result.
  - `LEONIDA_PSO_PROFILE=1` measures wall time and calling-thread CPU time for each Vulkan pipeline creation.
    Begin and end records share an ID.
    Compute records distinguish warmup, dispatch and eager creation. Graphics records name the lazy recipe's
    stages and unbound attachment mask. Mesh draw totals are reported per command buffer with its submit result.
  - Missing subobjects take the runtime's defaults: depth test on with `LESS`, back-face culling, no blending, sample
    mask `0xFFFFFFFF`, one sample.
  - The stream becomes the same descriptions `CreateGraphicsPipelineState` and `CreateComputePipelineState` take, so
    a VS stream builds the same PSO as the classic call.
  - `E_INVALIDARG`, as on the PC runtime: a type given twice, an unknown type, a truncated stream, MS with VS,
    tessellation or GS, CS with a graphics stage, or no stage at all.
  - `E_NOTIMPL` with a log, for what is not built yet: an AS, a depth-bounds test, view instancing.
  - `DEPTH_STENCIL2`'s per-face read and write masks go to each face's `VkStencilOpState` (the front face's in the
    description, the back face's beside it), so `OPTIONS14.IndependentFrontAndBackStencilRefMaskSupported` is TRUE:
    with `OMSetFrontAndBackStencilRef`'s per-face references, both of that cap's parts.
  - `RASTERIZER1`/`2`'s float depth bias is truncated to the integer the PSO path takes, with a log.
- **Mesh PSO:** the MS (`ms` index entry by FSSM hash) and PS on the mesh stage. It has no vertex input or input
  assembly; the input layout, topology type and strip cut are ignored, as D3D12 ignores them for mesh PSOs. Topology
  and strides are not dynamic state there (VUID-VkGraphicsPipelineCreateInfo-pDynamicStates-07065/07066). Creation
  is refused when the index's `max_vertices`/`max_primitives` or the workgroup size exceed the device's
  `VkPhysicalDeviceMeshShaderPropertiesEXT`.
- **`DispatchMesh`:** recorded as a `Draw` with `mesh` set; the thread-group counts go in `count`/`instances`/`first`.
  It takes the same state snapshot as `DrawInstanced`: render targets, root arguments, viewport, scissor, blend
  factor, stencil references. Replay issues `vkCmdDrawMeshTasksEXT`.
  - **Recording errors** (`Close` returns `E_INVALIDARG`): a grid above 65535 per count or 2^22 groups.
  - **Not recorded:** an empty grid.
  - **Execution error** (`ReplayResult::invalid_draw`): a mesh draw with a VS pipeline, or the reverse.
- **Tests:** `d3d12_mesh_test` checks the refusals and three draws pixel by pixel on an RX 9060 XT (RADV, under
  Wine), validation-clean.
  - **The three draws:** a 2×2×1 and a 1×1×2 mesh grid, plus a VS PSO from a stream.
  - **What it doesn't cover:** whether the console flattens a 3D `DispatchMesh` grid the same way. That is an open
    question in `docs/design.md`, "Mesh shaders"; the test shader reads the workgroup ID directly.

### Console rectangle and quad lists

The console has two list topologies Vulkan lacks: rectangle list (`IASetPrimitiveTopology(17)`, with a PSO whose topology type is 19) and quad list (`IASetPrimitiveTopology(19)`). The facts are from xvalkyrie (GPL-3.0, commit `53af7df77c10`; no code taken): a rectangle is three corners and the fourth is v2 + v3 - v1 for the position and every interpolant, after the vertex stage; a quad is four vertices. The layer draws both as triangles (`d3d12_rect_list_test`, RTX 4090: 4096 of 4096 pixels, the colour plane exact, quad list bit-exact; `d3d12_quad_index_test` for indexed quads).
- **Rectangle list**: PSO type 19 builds a triangle-list pipeline plus a generated geometry stage (`src/rect_list_geometry.h`, written from the vertex shader's own outputs) that takes the three corners and emits the strip v1 v2 v3 v4 (same winding). It has to be post-vertex-stage: the translated vertex shaders fetch by V# themselves, and the fourth corner is a function of what they write. A corner value with the same bits at all three corners (a flat attribute) is copied. Needs `geometryShader` (else `E_NOTIMPL`), no application GS, and a vertex shader with no ClipDistance, CullDistance, Layer or ViewportIndex output.
- **Quad list**: one triangle-list draw whose indices are `i0 i1 i2, i0 i2 i3` for each quad. Both triangles start
  at the quad's first vertex, the one Vulkan takes flat inputs from, and one draw keeps D3D12's instance order
  (instance by instance, not quad by quad). The PSO type is the ordinary triangle type.
  - **Non-indexed**: a shared index buffer in the arena holds the pattern `0 1 2, 0 2 3` for 16384 quads (a draw
    with more is cut off there, with a log line). The draw's first vertex becomes the vertex offset.
  - **Indexed**: a compute pass (`src/quad_index_expand.comp`, run by `src/quad_index_expand.h`) reads the
    application's index buffer on the GPU and writes the six 32-bit indices of each quad into copy scratch
    (`src/vulkan_scratch.h`); the draw binds that scratch with the application's instance count, base vertex and
    first instance. The host never reads the indices: a copy or a shader earlier in the same list can have written
    them. The pass runs before the draw's rendering scope, after a barrier that makes earlier writes visible to it
    and before one that makes its indices visible to the draw, and between two segments of an active
    pipeline-statistics query so that `CSInvocations` does not count it (as for the other helper dispatches,
    D3D-162).
    - Index values are copied as they are, a 16-bit one zero-extended, so `VertexIndex - BaseVertex` is still the
      application's index in a translated vertex shader.
    - The strip-cut values `0xFFFF` and `0xFFFFFFFF` are ordinary indices, as in every list topology: the replay
      enables primitive restart for strips only. What the console does with one inside a quad is not measured.
    - An index past the end of the index buffer view reads 0, as in D3D12. The pass reads a 16-bit index from its
      aligned 32-bit word, so standalone shader-addressable Vulkan buffers round their backing size up to a word.
      The D3D buffer size and view bounds are unchanged. A draw's last one to three indices that do not make a quad
      are dropped.
    - Needs `bufferDeviceAddress` (the pass uses device addresses, no descriptors). Without it, and inside a
      statistics query that cannot be split, the list fails as unsupported instead of drawing something else.
  - **Why not a triangle fan per quad** (the first version of the indexed path): a fan takes a flat input from
    each triangle's own first vertex, the quad's second and third, and a draw per quad runs every instance of one
    quad before the next quad. `d3d12_quad_index_test` shows both on an RTX 4070 SUPER: a quad with a red first
    vertex came out black and blue (0 of 4096 pixels red), and of two overlapping instances the first one's
    second quad ended on top (2048 of 4096 pixels right). With the expansion the seven test frames cover 16-bit and
    32-bit indices, a first index at an address of 2 modulo 4, a base vertex, two instances, and a view that ends
    inside the second quad, plus a 16-bit draw that reaches the end of a standalone upload buffer.
  - **Not covered**: quad and rectangle lists through `ExecuteIndirect`, whose draws do not record a topology.

## 4. Object model mapping

| D3D12 / Xbox object | Vulkan / layer object | Notes |
|---|---|---|
| `ID3D12Device` … `ID3D12Device11` | one `Device` object with the public chain plus measured Xbox overrides | The shipped BVH builder names Device11; slots 1/15/18/19/27/38 matched desktop in prior art, slot 48 is called as `(desc, riid, out)` with `IID_ID3D12CommandQueue` (PC slot 48 is `OpenExistingHeapFromAddress`; the 32-byte desc is a PC `D3D12_COMMAND_QUEUE_DESC` plus four Xbox words, logged once, and the proxy takes the slot 8 path with the PC prefix and only `DISABLE_GPU_TIMEOUT` kept), slot 62 is called as `CreateComputePipelineState`, slot 76 is Xbox texture creation, and heap slots 9/10 return in RAX. Unknown Xbox IIDs/slots are logged and refused. |
| `ID3D12CommandQueue` | one leased `VkQueue` per live queue: direct queues on the graphics family, compute queues on an async-compute family and copy queues on a transfer-only (DMA) family when the GPU has them (`src/queue_families.h`), else graphics-family leases | Distinct Vulkan queues allow cross-queue waits and signals to make progress. With more than one family in use, buffers, images, the arena and staging are created `VK_SHARING_MODE_CONCURRENT` over those families, so no ownership transfers are needed. Barriers on copy and compute queues are narrowed to the stages their family has; render-target and depth clears are refused there (as in D3D12), and the D24/RGB96 copy conversions and bindless flushes stay off a transfer-only queue. Tested by `vk_queue_test`. `PresentX`, `SuspendX`, `ResumeX` are Xbox queue methods, not in the public vtable; public GDKX samples present only through them (§4.5). Call shapes known, slots **Open**. |
| `ID3D12CommandAllocator` | IR arena + `VkCommandPool` | Reset only when the previous submission's fence is complete. |
| `ID3D12GraphicsCommandList` … `8` | `CommandList` with IR | Replay at `ExecuteCommandLists`; bundles inlined. Supports render passes, mesh dispatch, enhanced barriers; records the GCL8 front/back stencil references (applied only once a pipeline path consumes them). `Draw` views are inlined and moved into the command stream (D3D-289), eliminating per-draw heap allocations. |
| `ID3D12RootSignature` | `RootSignature` + `VkPipelineLayout` | Deserialize the blob with the host `D3D12CreateVersionedRootSignatureDeserializer` when possible; clear the measured `0x03000000` flag byte on a copy first; fall back to a local parser. Build the user-SGPR map and static samplers. |
| `ID3D12PipelineState` | `VkPipeline` + a `PipelineInfo` | Shader hash → SPIR-V + index entry; PSO state → Vulkan state; dynamic state where possible. The BVH builder's compute PSOs arrive at device slot 62 with a plain `D3D12_COMPUTE_PIPELINE_STATE_DESC`. |
| `ID3D12DescriptorHeap` | `DescriptorHeap` (shadow + optional arena range) | CBV/SRV/UAV and sampler heaps can be shader-visible; RTV/DSV are host-only. Start handles use the measured RAX return at heap slots 9/10. Increments are layer-chosen (32/32) pending the console value. |
| `ID3D12Resource` | `Resource` (a range in the arena buffer; a separate `VkImage` for textures) | Committed, placed and reserved. Reserved buffers use sparse memory where supported (section 4.2). Xbox textures may arrive through device slot 76. `GetGPUVirtualAddress` returns a console VA (a buffer offset or an image key). |
| `ID3D12Heap` | `Heap` (a reserved VA range) | Buffer ranges are backed by the arena; images get ranges of shared blocks or their own `VkDeviceMemory` (section 3.3). Residency calls are no-ops. True buffer/image aliasing inside a heap is **Open**. |
| `ID3D12Fence` / `ID3D12Fence1` | `Fence` over a timeline `VkSemaphore` | `GetCompletedValue` = counter value; `SetEventOnCompletion` = wait thread + Win32 event; CPU `Signal` = host signal. |
| Resource barriers (legacy) | `vkCmdPipelineBarrier` | Per-subresource state tracking; `BEGIN_ONLY`/`END_ONLY` become conservative full barriers at the begin point. |
| Enhanced barriers (`ID3D12GraphicsCommandList7::Barrier`) | `vkCmdPipelineBarrier2` | Direct mapping of sync/access/layout; prefer it when the game uses it. |
| `IDXGISwapChain` / `IDXGIFactory` | Factory/adapter/output: fake objects. `Swapchain` over `VkSwapchainKHR` + Win32 surface: only for the PC-style path | Shim DXGI; do not bind host DXGI. A GDKX title likely creates no swapchain (§4.5); it needs `IDXGIDevice1`→`GetAdapter`→`EnumOutputs` and a `PresentX` + frame-event presenter instead. |
| `ID3D12QueryHeap` | `VkQueryPool` | Occlusion, binary occlusion, pipeline statistics, timestamps. Binary results clamped to 0/1. |
| `ID3D12CommandSignature` | `IndirectLayout` | Simple draw/dispatch signatures map to indirect calls; root-constant/vertex/index/descriptor changes need expansion. |
| `ID3D12PipelineLibrary` | map + `VkPipelineCache` | D3D12 cached blobs are ignored. |
| `ID3D12StateObject` (ray tracing) | likely unused | RT work includes compute traversal (`image_bvh*`) plus `XboxBvhBuilder`; a DXR state-object path is stubbed until the trace shows it. |
| `ID3D12ShaderCacheSession` | `VkPipelineCache` | Stub with logging. |
| DRED, protected sessions, meta commands, shared handles | stubs | Not needed for single-player boot; return a clean failure and log. |

### 4.1 Device and feature queries

`CheckFeatureSupport` is a boot-critical diagnostic. Answer from the Vulkan device, not from a wish list:

- `D3D12_FEATURE_ARCHITECTURE`: report the host GPU's UMA property. The console is UMA; a discrete PC GPU is not.
  Reporting the truth may change the game's upload strategy; reporting a lie can make it map memory it cannot map.
  Start truthful and log the game's reaction.
- `D3D12_FEATURE_LEVELS`: report the highest feature level the Vulkan device can satisfy (12_2 when mesh/RT
  features are present).
- `D3D12_FEATURE_SHADER_MODEL`: answer the shader model the layer can honour, not the driver's maximum. Nothing in
  the runtime compiles DXIL: the game's shaders arrive as SPIR-V from the offline translator, and the BVH
  builder's container also carries a console FSSM program that the same translator handles. A request above 6.0 is
  clamped down to 6.0, a request for 5.1 is answered with 5.1 (D3D12's own baseline, which a caller asking for the
  minimum expects), and a value below 5.1 is refused with `E_INVALIDARG`. Measured against the host PC runtime
  (616.86) on an RTX 5070: 5.1-6.8 echo the request, 6.9 clamps to 6.8, and `0` is `E_INVALIDARG`; `0x5f` crashed
  the host runtime, which the test records in a comment rather than repeating. WARP answers a 5.1 request the same
  way.
- `D3D12_FEATURE_ROOT_SIGNATURE`: 1.1, the version the proxy can always produce. A 1.2 blob is rewritten as 1.1 when
  the host's serializer stops at 1.1 and nothing is lost (`d3d12_xs_rootsig.cpp`), and Windows 10's `d3d12.dll` stops
  there, so 1.1 is what the game can rely on everywhere. A 1.2 request is clamped down to 1.1, and an undefined value
  (0, or anything above 1.2) is `E_INVALIDARG` with the value left as it was. Measured against the host PC runtime
  (616.86): 1.0-1.2 echo the request, and a wrong `DataSize` is `E_INVALIDARG`; the host crashed on a null buffer, so
  the test never gives it one. The refusal above 1.2 was measured on an RTX 4070 Laptop (621.24) and WARP.
- `D3D12_FEATURE_D3D12_OPTIONS1`: answer from the Vulkan device's subgroups. `WaveOps` is true only when every
  operation group the wave intrinsics map to is present (`BASIC`, `VOTE`, `BALLOT`, `SHUFFLE`, `ARITHMETIC`),
  because D3D12 promises them all behind that one bool. `WaveLaneCountMin`/`Max` are the subgroup size range
  (`VkPhysicalDeviceSubgroupSizeControlProperties`, or the single `subgroupSize` without those 1.3 properties).
  The exception is an Intel integrated GPU of the pre-Xe-LPG lines (D3D-121): Vulkan reports 8..32 there, Intel's
  D3D12 driver a fixed 16..16, and the layer answers 16..16 like the host. Measured on Xe-LP: UHD 770
  (32.0.101.6129), Raptor Lake-S Mobile (101.7092) and Tiger Lake 0x9a78 (101.7088). From Xe-LPG on the driver
  answers the same range Vulkan reports (D3D-280, measured Meteor Lake 0x7d55 on 32.0.101.6127: 8..32 on both
  sides), so those lines keep the Vulkan range (Meteor/Arrow Lake device IDs 0x7d40-0x7dc5, Lunar Lake
  0x64a0-0x64b6). Discrete Arc keeps the Vulkan range until it is measured (TEST-57).
  `TotalLaneCount` has no Vulkan equivalent: D3D12 counts every lane of the device, and the layer answers one full
  wave, so a game that sizes work from it plans for less parallelism than the GPU has, never more. Measured
  against the host PC runtime (616.86) on an RTX 5070: `S_OK` with `WaveOps` 1, lanes 32..32 and a total of 6144,
  and `E_INVALIDARG` for a `DataSize` one field too short or too long. The host crashed the process on a null
  buffer, so the layer refuses one without asking the host (as with root signatures).
- `D3D12_OPTIONS` families: report the tier the Vulkan feature set can honour (resource binding tier 3 when
  descriptor indexing and update-after-bind are available), plus wave ops, 16-bit, barycentrics, render passes,
  VRS, mesh shader tier, and RT tier 1.1 only if the layer can honour them. The game may branch on these; a false
  "supported" is worse than a false "unsupported" at boot. For `OPTIONS2`–`5`, `7`, `9`, `11`, `13`–`16`, and
  `18`–`21` (D3D-86; leaving `OPTIONS1` to #745, `OPTIONS6`/`10` to #469, `OPTIONS8` to #715, and `OPTIONS12` to
  #693):
  - `OPTIONS4` reports `MSAA64KBAlignedTextureSupported = TRUE` (implemented in `GetResourceAllocationInfo`);
  - `OPTIONS19` maps `SupportedSampleCountsWithNoOutputs` from `framebufferNoAttachmentsSampleCounts`, ties
    `AnisoFilterWithPointMipSupported` to `VulkanDeviceInfo::sampler_anisotropy`, and reports the baseline heap
    limits (`2048` samplers, `1000000` views);
  - `OPTIONS21` reports baseline `D3D12_EXECUTE_INDIRECT_TIER_1_0`, and unimplemented features/tiers (`DepthBounds`,
    `CastingFullyTypedFormat`, MSAA UAVs, independent stencil masks, render passes, RT, mesh shaders, sampler
    feedback, wave MMA, work graphs, GPU upload heaps) stay `FALSE`/`NOT_SUPPORTED`/`TIER_0`. Checked live against
    the host D3D12 runtime on the same adapter (`NVIDIA GeForce RTX 5080`, driver `616.56`, Windows 11): 15/15 feature
    queries compared with `0` mismatches (`MSAA64KBAlignedTextureSupported = TRUE`,
    `SupportedSampleCountsWithNoOutputs = 0x1d`, `LEONIDA_VK_VALIDATION=1` with `0` validation errors).
- `D3D12_FEATURE_FORMAT_SUPPORT`: map to `vkGetPhysicalDeviceFormatProperties`; multisample render-target
  and resolve bits also require the format's image-usage/sample-count query to succeed. D3D12's per-format
  rules then apply, as native D3D12 reports them (RX 9070 XT, AMD 26.11.2, and WARP):
  - `MIP` wherever the image can have more than one level.
  - `SHADER_GATHER` with `SHADER_SAMPLE`, including `A8_UNORM` where the 49-footprint WARP regression verified alpha selection.
  - `UAV_TYPED_LOAD` with every typed UAV. Ordinary typed UAV texture reads use the sampled binding
    (`image_read_typed`), while coherent `glc` loads and storage-only 1D-array loads use storage-image reads.
    Typed buffer UAVs: the layer writes their V# (section 3.4, "Buffer UAVs"); `rdna2spirv.py` `FORMATS`
    still limits decoded buffer formats (including gaps in 8/16-bit and 10_11_11 formats).
  - The UAV atomic bits other than exchange for `R32_UINT` where Vulkan supports storage image atomics.
    The translator and set 0 use the formatted `r32ui` views for 2D and 3D; `R32_SINT` has no `r32ui` view.
  - `MULTISAMPLE_RENDERTARGET` for depth formats, from the same usages `MULTISAMPLE_QUALITY_LEVELS` queries.
  - No integer resolve, no 1D block-compressed. Depth formats are 2D and cube only: no 3D, shader reads, UAVs,
    buffer or vertex use, and no 1D, which native D3D12 allows but the layer's resource creation refuses.
  - TYPELESS formats report texture dimensions and `MIP` only.

  `d3d12_vulkan_device_test` checks these bits for every format that both the layer and native D3D12 on the same
  adapter (matched by LUID or PCI ID) support. Some checks compare with the host and some check the layer's own
  consistency; the comment on `compare_format_support` lists which are which. Formats the layer answers with no
  uses, such as D24 without Vulkan D24, are listed but not checked. It skips when there is no host on the same
  adapter (Wine, or no LUID/PCI match).
- `D3D12_FEATURE_FORMAT_INFO`: report the DXGI plane count independently of Vulkan texture support. Both
  depth/stencil families (including their view formats), NV12, P010, P016 and NV11 have two planes. The
  device test compares counts and HRESULTs with native D3D12 on the same adapter. If an older host rejects the
  sampler-feedback formats or `A4B4G4R4_UNORM` with `E_INVALIDARG`, the test still requires the layer to return
  `S_OK` and one plane for them.
- `D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS`: query `vkGetPhysicalDeviceImageFormatProperties` for the matching
  colour or depth/stencil attachment usage, plus the transfer and sampled usages used by the resource implementation.
  Vulkan exposes one standard sample pattern, so a supported sample count reports one quality level (quality index 0);
  only D3D12's legal sample counts (powers of two up to 32) are exposed.
- `D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT` (`D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT`): report the
  arena's limits, not the console's.
- `GetResourceAllocationInfo` and `GetCopyableFootprints`: implement D3D12's rules locally; the game uses them to
  suballocate and to lay out uploads. They are deterministic and public. Validate against a host D3D12 device in
  tests if one is available; do not require one at runtime. `GetResourceAllocationInfo` (and `1`/`2`): a buffer
  takes its width rounded up to 64 KiB, aligned to 64 KiB. A texture's size is the memory its `VkImage` needs
  (`vkGetDeviceImageMemoryRequirements`), rounded up to its D3D12 alignment. That alignment is 64 KiB, or 4 MiB with
  MSAA. The small alignments (4 KiB, or 64 KiB with MSAA) apply only when the application asks for them. 4 KiB
  also needs the texture to fit in 64 KiB and not be a render target or depth buffer; if it doesn't qualify, the
  description is invalid (`UINT64_MAX`), as the PC runtime answers. The MSAA 64 KiB alignment has no size limit
  on the PC runtime, so none here either. Texture sizes are the driver's on both runtimes, so only buffer sizes,
  alignments and invalid descriptions are compared with the host. One known difference: a mip count beyond the
  full chain is invalid here (Vulkan can't create it), while the PC runtime still answers with a size.
  A total over several resources that passes 2^64 saturates at `0xFFFFFFFFFFFF0000` here. Hosts disagree on it:
  WARP and the RTX 5070 host of #1204 saturate too, while on Windows 26300.9974 (D3D12Core 10.0.26100.9818) the
  Intel Raptor Lake-S (101.7092) and RTX 5060 Laptop (621.24) hosts wrap, two 2^63 buffers to 0, and WARP on that
  PC still saturates. A wrapped total could make a caller allocate less than one of its resources, so the layer
  keeps saturating, and the test accepts a host that wraps.
  Behind the Xbox-ABI proxy (device slots 25 and 56) a 2D render target of a listed colour format with one sample is
  padded to whole console tiles: the size is the sum over its mips (MipLevels 0 is the full chain, an odd extent
  rounds up), each mip rounded up to the RDNA2 SW_64KB_D tile shape (`runtime/src/gfx10_tiling.h`: 256 x 128 pixels
  for 2-byte elements, 128 x 128 for 4-byte, 128 x 64 for 8-byte, 64 x 64 for 16-byte), with the first mip that fits
  in half a tile's width and a whole tile's height, and the mips after it, sharing one tail tile, times the array
  size. A 1920 x 1080 RGBA8 target is 135 tiles, 8,847,360 bytes, where the PC answer is 8,323,072. The proxy lays
  the descriptions out as the PC device does (each at its own alignment, the total rounded up to the largest
  alignment, sums saturating) with each render target at the larger of its console size and its PC size; a 4 MiB
  aligned multisampled neighbour therefore keeps its place (a 1 x 1 four-sample target and a 1080p target need 16 MiB,
  not 12.5). Slot 56 reports the same offsets. The PC layout stands when no description grows
  (`d3d12_xs_alloc_size.h`; `xs_proxy_mock_test`). **Assumption**: the
  retail title's own sizes are not measured (#6982, stop 17), so that its allocation is these shapes and no DCC or
  CMask memory is open until they are.
  The descriptions the title passes to slot 25 carry console values (layout word `0x101`/`0x1`, flag bits above the PC
  ones). For console-form calls or descriptors explicitly using layout `0x101`, the proxy gives the PC device a copy
  with the PC flags only and ROW_MAJOR (buffer) or UNKNOWN (texture); the title's array is never written. Other
  PC-form descriptors reach the host unchanged for validation (`d3d12_xs_desc_clean.h`, the same cleaning as slot 76;
  #6982 section 3.7).
  A committed resource made through slots 76 and 127 also allocates that size through the title's
  `__XMemAllocationHooks__`, as the console runtime's `XMemAlloc` for the resource does, and frees it when the
  resource is destroyed (a lease kept with `SetPrivateDataInterface`; the bytes are not used). The retail title
  budgets its first full-screen render targets with such a hook: it bump-allocates the allocations whose XALLOC
  page-size field is 2 out of a range it reserved and asserts that less than 2 MiB is left (RT-5 stop 17). The
  lease's attribute word (allocator ID 0x80, page-size field 2, object type 0) is an **assumption**; the measured
  hook reads only the page-size field.
- `GetAdapterLuid` (implemented): `VkPhysicalDeviceIDProperties::deviceLUID`, which on Windows is the DXGI
  adapter's LUID; `d3d12_fence_timing_test` finds it with `IDXGIFactory4::EnumAdapterByLuid` (RTX 4090, driver
  621.14). A driver that reports `deviceLUIDValid = VK_FALSE` gets a zero LUID, logged.
- `GetCustomHeapProperties` (implemented): the D3D12 documentation's table for a NUMA adapter, matching the
  `UMA = FALSE` that `D3D12_FEATURE_ARCHITECTURE` reports: `DEFAULT` is `NOT_AVAILABLE`/`L1`, `UPLOAD`
  `WRITE_COMBINE`/`L0`, `READBACK` `WRITE_BACK`/`L0`, `GPU_UPLOAD` `WRITE_COMBINE`/`L1`, all with type `CUSTOM` and
  node masks 1. `CUSTOM`, unknown types and node masks above 1 get an all-zero answer, logged. The test compares
  `DEFAULT`, `UPLOAD` and `READBACK` byte for byte with the host runtime's answer (RTX 4090: identical).
  The retail executable calls it (device slot 26) with the buffer for the returned struct in RCX and the device in
  RDX, node mask in R8 and heap type in R9: three calls right after device creation, node mask 1 and types 1, 2
  and 3 (measured under `gta6rt --run` with a register probe in the slot's thunk). A PC C++ method has the device
  in RCX and the buffer in RDX, so the Xbox-ABI proxy takes the console form when RDX holds the proxy
  (`d3d12_xs_device.cpp`; `xs_proxy_mock_test`, `device_proxy_test`). Slot 43 returns a struct too and the
  proxy still forwards it in the PC form: no call of slot 43 has been seen. Slots 25 and 56 take the same form by the
  same rule (reported later in the boot, #6982; not measured), as #6973 first did for slot 25.
- Residency and power (D3D-33) are no-ops that still check their arguments (`d3d12_vulkan_device.cpp`, tested in
  `d3d12_vulkan_device_test`). Vulkan has no residency API: memory from `vkAllocateMemory` stays usable until
  `vkFreeMemory`, and paging under pressure is the kernel driver's job, so every pageable object is always resident.
  The residency answers are the host runtime's: `d3d12_residency_test` runs the same 58 steps on both devices
  with a committed buffer, a heap, a placed buffer, a descriptor heap, a query heap and a fence, and compares every
  HRESULT, the device-removed reason after each step, and the value `EnqueueMakeResident` leaves the fence at
  (110 values; RTX 3060, driver 610.88). Steps that leave the host runtime unusable run there in a child process with a
  fresh device.
  - `MakeResident` and `Evict` return `S_OK` for any count of this device's pageable objects, the same one twice,
    an evicted one or a resident one. D3D12 documents `Evict` as a hint the runtime may defer, so dropping it is
    allowed.
  - `SetResidencyPriority` (Device1): only objects that own their memory take a priority, committed resources and
    heaps; any `UINT` is a valid priority for them. A placed resource (a reserved one too, by the same rule; not measured), a descriptor heap, a query heap
    or a fence is `E_INVALIDARG`, also inside a longer list. The host runtime then removes its device as well
    (`DXGI_ERROR_INVALID_CALL`); the layer doesn't, since a console title that sets priorities on its placed
    resources would lose its device over a hint.
  - `EnqueueMakeResident` (Device3) signals its fence from the CPU with the given value before returning, since
    the work is already complete. A queue `Wait` on that fence must not hang, which is why the signal stays. A null
    fence is accepted and nothing is signalled. `E_INVALIDARG` for flags other than `NONE` and `DENY_OVERBUDGET`;
    a refused call does not signal. A value below the fence's current one moves the host's fence back; the layer's
    stays where it was (see the fence note above).
  - Not compared: a nonzero count with a null array, and a null entry in an array, which the host runtime
    dereferences (an access violation). The layer answers `E_INVALIDARG` for the null arrays, skips a null entry in
    `MakeResident`, `Evict` and `EnqueueMakeResident`, and refuses one in `SetResidencyPriority`.
  - `SetStablePowerState` returns `S_OK`: Vulkan has no clock control, and the result of a frame does not depend
    on the clocks.
  - `SetBackgroundProcessingMode` (Device6): no Vulkan API steers driver background work, so nothing runs. The
    completion event is set at once and `*pbFurtherMeasurementsDesired` is `FALSE`. A mode or action outside the
    SDK's enum is `E_INVALIDARG`.

### 4.2 Resources, heaps, placed and reserved

- **Xbox texture creation is a separate device method at slot 76**, not `CreateCommittedResource`. Prior art
  reports its shape as heap properties / heap flags / description / initial state / clear value / iid / output,
  forwarded on the PC side to `CreateCommittedResource` slot 27. The description is 56 bytes plus extension words,
  and its layout field is `0x101` (tiled) or `0x1` (row-major buffer). The PC proxy replaces the layout with a
  driver-chosen one; our layer must recognise slot 76 and `0x101` explicitly and must not pass `0x101` to the public
  layout enum. The FidelityFX upscaler creates 10 default-heap resources through this slot, so it is not optional
  for middleware. The console tiled data layout is GFX10 `SW_64KB_D` (Mesa addrlib `GFX10_SW_64K_D_RBPLUS_PATINFO`),
  and the detile and tile path is implemented in `resource_detile.h` and verified by `resource_detile_test` (D3D-316).
- `CreateCommittedResource`: allocate an aligned range in the arena for a buffer, or a range of a shared block
  or a separate `VkDeviceMemory` for an image (section 3.3); record the initial state. Map `D3D12_HEAP_TYPE_DEFAULT` to device-local memory, `UPLOAD`/`READBACK`
  to separate host-visible buffers. This remains the path for buffers and for any PC-style committed resource.
- `ID3D12Resource::GetDesc` returns the description with its defaults filled in, as the PC runtime does: `MipLevels`
  0 becomes the full chain, and `Alignment` 0 becomes the placement alignment, 64 KiB, or 4 MiB for a multisampled
  texture (an alignment the description asked for is kept). **Implemented** and compared field by field with the
  PC runtime and WARP by `d3d12_resource_desc_test`: 25 committed and reserved buffers and 2D, array, volume and
  multisampled textures (measured on an RTX 3060 host and WARP, Windows 10).
- **Invalid arguments (D3D-49).** `CreateCommittedResource`, `CreateDescriptorHeap`, `CreateCommandQueue`,
  `CreateFence` and `Map` return the host PC runtime's HRESULTs (no debug layer) for the cases in
  `compare_invalid_arguments` (`d3d12_vulkan_device_test`): invalid ones, their valid neighbours, and
  `ALLOW_RENDER_TARGET`/`ALLOW_DEPTH_STENCIL` on every format both devices create. Rules the host keeps that are easy
  to get wrong: a committed resource takes no heap flag except `SHARED`, `ALLOW_DISPLAY` (textures only),
  `HARDWARE_PROTECTED`, `CREATE_NOT_RESIDENT`, `CREATE_NOT_ZEROED` and `ALLOW_SHADER_ATOMICS` (only with
  `ALLOW_UNORDERED_ACCESS`; measured on an RTX 4070 Laptop and WARP); a buffer refuses `ALLOW_SIMULTANEOUS_ACCESS`;
  MSAA takes quality 0 or one of the two standard patterns; a buffer may be `ALLOW_RENDER_TARGET` but not
  `ALLOW_DEPTH_STENCIL`, and is aligned 0 or 64 KiB only; upload heaps start in any part of `GENERIC_READ` (or
  `COMMON`), readback heaps in `COPY_DEST` or `COMMON`, neither with UAVs; a texture starts in exactly
  `GENERIC_READ`, in one write state alone (`RENDER_TARGET`, `UNORDERED_ACCESS` and `DEPTH_WRITE` only with the
  matching resource flag, `COPY_DEST`, `RESOLVE_DEST`) or in a mix of read states (`DEPTH_READ` only with
  `ALLOW_DEPTH_STENCIL`, the shader-resource states not with `DENY_SHADER_RESOURCE`), never with a buffer-only or
  undefined state bit, while a buffer takes any bits except `RAYTRACING_ACCELERATION_STRUCTURE` without UAV access
  (RTX 3060 / 610.88; the same rule applies to placed and reserved resources), or at all where the host reports no
  ray tracing (an integrated Radeon without DXR, @aguedou-34 on #2462): the layer refuses it on a device with
  neither KHR acceleration structures nor the XDXR builds; `ALLOW_DEPTH_STENCIL` is accepted for every
  format of the R32, R16, R24G8 and R32G8X24 families and nothing else; a clear value needs a render target or depth
  buffer and a format of the resource's family; descriptor heaps may have zero descriptors; non-monitored fences are
  refused; a null output is `S_FALSE` (a capability check) except for `CreateCommandQueue`, which answers
  `E_UNEXPECTED`, and `CreateDescriptorHeap` answers it before its flag, type and node rules (`S_FALSE` even for a
  shader-visible RTV heap) where committed resources and fences check their arguments first; a `GPU_UPLOAD` heap is
  refused, since `CheckFeatureSupport` reports `OPTIONS16.GPUUploadHeapSupported` FALSE (a host with GPU upload
  heaps, which needs Resizable BAR, accepts it, so that case is compared only on a host without them); a
  global-realtime queue is `DXGI_ERROR_ACCESS_DENIED` without the privilege; `Map` refuses a
  non-empty read range past the buffer. Not compared, because the host faults or the layer cannot follow it: a null
  heap-properties or descriptor-heap description (the host faults), more mips than the full chain (the RX 9070 XT
  runtime keeps up to 16 on a 16×16 texture and dies on 17), `D32_FLOAT` and `D16_UNORM` as render targets and the R32
  and R16 colour formats as depth buffers (accepted by the host; the layer creates the Vulkan format of the same name,
  and the Vulkan drivers measured can't make that attachment, so the test prints these instead of failing),
  cross-adapter resources (the host takes a row-major texture on a `SHARED | SHARED_CROSS_ADAPTER` heap; the layer has
  no cross-adapter sharing), an unknown descriptor heap type (accepted by the host), and the driver's own answers:
  R32G32B32 render targets, 2D textures wider than 16384, `L1` custom heaps, 16x MSAA and video formats and queues
  differ between the RX 9070 XT, an integrated Radeon and WARP. The rest of the table agrees on all three.
- `CreateHeap`: reserve a VA range with the requested size/alignment. `CreatePlacedResource`: assign the resource
  the range at `heap_va + offset`; a buffer uses that range in the arena, an image uses the offset as a key and
  binds its own memory. Vulkan image alignment can exceed D3D12's 64 KiB; because images do not share the arena,
  this mostly affects true aliasing, which is **Open** until tested on the target GPU.
  Its arguments are answered as the host runtime answers them (`compare_placed_reserved` in
  `d3d12_vulkan_device_test`, RTX 3060 / 610.88): the description, initial-state and clear-value rules of a committed
  resource (but any initial state for a buffer in an upload or readback heap, and no UAVs there); a buffer at a
  64 KiB-aligned offset; a texture at a multiple of its `GetResourceAllocationInfo` alignment (4 KiB for a small
  texture), in a heap aligned at least as much, and whole inside the heap; the heap's `DENY` flags against the
  resource; then a null output is `E_UNEXPECTED` and nothing is made.
- `CreateReservedResource` **implemented** (`d3d12_vulkan_device.cpp`, `src/resource_tiling.h`, checked by
  `d3d12_vulkan_device_test` and `resource_tiling_test`):
  - `CreateReservedResource`, `CreateReservedResource1` (no protected sessions) and `CreateReservedResource2`
    (castable formats ignored) create sparse buffers when the device can bind and alias 64 KiB tiles and guarantees
    zero reads from unbound pages. Reserved textures and buffers on other devices use a fully backed resource.
    D3D12's description rules are kept, as measured on the PC
    runtime: buffers are `ROW_MAJOR`, textures `64KB_UNDEFINED_SWIZZLE` (anything else, or a format without a tile
    shape, is `E_INVALIDARG`; `64KB_STANDARD_SWIZZLE` too, since `CheckFeatureSupport` reports
    `StandardSwizzle64KBSupported` as FALSE), and `GetHeapProperties` on a reserved resource is `E_INVALIDARG`.
    The description, initial-state and clear-value rules are a committed resource's on a default heap, and a null
    output is `E_UNEXPECTED` after them (`compare_placed_reserved`, RTX 3060 / 610.88).
    `GetHeapProperties` answers as the host runtime's (`d3d12_heap_properties_test`: 561 values for committed
    resources in every heap type, custom ones and SHARED / ALLOW_DISPLAY / CREATE_NOT_ZEROED / CREATE_NOT_RESIDENT
    included, placed resources in heaps of each kind, reserved ones, and null arguments; RTX 3060, driver 610.88).
    A committed resource's flags are the caller's without the two creation-only bits, plus the DENY bits of the
    resource classes it isn't; a placed resource reports its heap's flags as they are, so one in an `ALLOW_ALL`
    heap reads back 0.
    `CheckFeatureSupport` still reports `TiledResourcesTier` as not supported, so a game that checks it creates
    committed resources instead. Buffer mappings alone do not implement a full tiled-resources tier.
    The Xbox tiled layout word `0x101` is accepted as `64KB_UNDEFINED_SWIZZLE` (**ASSUMPTION**:
    that the console uses it for reserved resources too).
  - `GetResourceTiling`: the standard 64 KiB tile shapes, standard and packed mips (a mip stays standard while it is
    wider than half the tile or taller than the tile, depth not counting, as AMD's PC runtime does on RDNA2 and RDNA4
    and so, most likely, the console (D3D-65); an NVIDIA GPU packs from the first mip smaller than a tile on any axis.
    D3D11.3 functional spec 5.9.2.8.5 only requires that
    a mip filling a tile in every dimension stays standard, a smaller one "may or may not be packed, depending on the
    IHV"), each standard mip's tile counts, and the tile order (each array slice holds
    its standard mips, then its packed tiles). The test compares the tile shape and the tile counts of the mips both
    sides keep standard with the PC runtime, and checks that no mip filling a tile is packed, for 18
    buffer, 2D, array, block-compressed and volume layouts, and skips a layout the host refuses: which ones a PC
    driver accepts varies (an RTX 3090 on driver 616.56, at tier 3, refuses texture arrays whose mips reach the
    packed tail, measured by @CharlyRien in #295; an RTX 5070 on 621.01 accepts them). How many tiles the packed mips take is the driver's
    choice: here it is one tile per tile-depth slice the tail spans, as AMD reports (below), and never fewer than
    their tight linear size in whole tiles; the PC runtime on an RTX 5070 reported one tile more than the tight size
    for 1000x300 and 100x100 RGBA8 and a 128^3 R8 volume, and the same count for the other layouts probed. Two
    further measured differences: on array slices after the first, the PC runtime
    writes a tile index into packed subresources instead of the documented `D3D12_PACKED_TILE` (ours follows the
    documentation), and a resource that is not reserved has no tiles (all outputs zero), as on the PC runtime.
  - **Measured on AMD** (RX 9070 XT, RDNA4, driver 32.0.31041.1004, Windows 11, `main` `91b36cf`): the AMD PC
    runtime splits standard and packed mips differently, which `d3d12_vulkan_device_test`'s tiling comparison used
    to report as a failure; the spec leaves the split to the driver, so the comparison now takes any split (a Radeon
    880M on 26.11.2 behaves the same, and WARP keeps every mip standard: it packs nothing, down to 1x1).
    Tile shapes agree in all 17 texture layouts, and 11 of them match exactly; in the other 6, AMD keeps more
    mips as standard tiles before the packed tail:

    | Layout | Tile | Standard mips: NVIDIA (the layer before D3D-65) | AMD (the layer now) |
    |---|---|---|---|
    | 1000x300 RGBA8 sRGB, 3 slices | 128x128 | 2 | 4 |
    | 2048x1024 BC7, 2 slices | 256x256 | 3 | 4 |
    | 1000x600 BC3 | 256x256 | 2 | 3 |
    | 100x100 RGBA8 | 128x128 | 0 | 1 |
    | 4096x64 BGRA8 | 128x128 | 0 | 6 |
    | 256x256x64 RGBA8 volume | 32x32x16 | 3 | 4 |

    An RDNA2 GPU (the Ryzen 7 9800X3D's, driver 32.0.21045.5002) gives the same splits as the RDNA4 one
    ([#374](https://github.com/ItsNotRexed/Leonida/issues/374#issuecomment-7071869388)). With 30 more layouts that
    probe the edges (a mip exactly at or just past half a tile, thin and tall textures, non-square 2D and volume
    tiles), the rule that fits all 47 on both GPUs is: **a mip stays standard while it is wider than half the tile or
    taller than the tile; depth does not count**. An earlier "every axis at most half the tile's largest side" fitted
    the first 17 but missed four tall or deep edge cases. All 47 layouts and AMD's values are in
    `runtime/test/resource_tiling_test.cpp`. AMD's packed tile count is one tile per tile-depth slice the tail spans,
    `ceil(depth of the first packed mip / tile depth)`: 1 for every 2D layout, and for 32 further volumes whose tail
    is deeper than a tile it steps at each tile depth (8x8x32 R8 takes 1 tile, 8x8x33 takes 2, 8x8x256 takes 8), even
    where the bytes would fit in fewer (16x16x128 RGBA8: 8 tiles for 3 tiles of data). Those 32 are in the test too,
    79 layouts in all, identical on both GPUs. The console is RDNA2, so the layer follows this rule (D3D-65;
    **ASSUMPTION**: that the console's runtime splits like the PC's AMD driver).
  - `UpdateTileMappings` and `CopyTileMappings` bind pages in reserved buffers to default heaps from D3D-16.
    Sequential, NULL, SKIP and single-tile reuse ranges are supported. Queue timeline waits order binding between
    ordinary submissions, and the previous heap allocation is retained until an in-flight unbind completes.
    `CopyTiles` copies 64 KiB tiles between sparse buffers and linear buffers in either explicit direction.
    `d3d12_buffer_tiles_test` compares three full readbacks with native D3D12, including a fence-gated queue.
  - Drivers that exclude the host-visible arena memory type from sparse buffers (including NVIDIA 621.01 on the
    RTX 3090) use a dedicated device-local allocation per heap for its tiles. A heap with this dedicated backing
    cannot mix placed buffers and sparse mappings.
  - D3D-285 extends these mappings to single-sample `R8G8B8A8_UNORM` reserved 2D textures with one mip, one
    layer and no resource flags. The Vulkan image must support sparse aliases and zero reads from unmapped tiles,
    with 128 × 128 color blocks aligned to 64 KiB. Images requiring an opaque mip tail or separately bound metadata
    keep the fully allocated fallback. The image is transitioned while still unbound and kept in `GENERAL` so
    mapping an alias preserves contents. Image tiles use dedicated device-local heap memory and can alias tiles
    of compatible sparse images. Buffer and placed-resource aliases with texture tiles are outside this path.
  - `CopyTiles` transfers RGBA8 tiles in either direction. Partial edge blocks use the full 512-byte linear row
    pitch and transfer only in-bounds texels. Byte-unaligned offsets use transfer scratch.
    `d3d12_texture_tiles_test` compares initially unmapped tiles, column mappings, aliases, and a fence-delayed
    remap with native D3D12, including clipped edges and byte-unaligned offsets.
- `D3D12_RESOURCE_DESC::Layout`: the public `ROW_MAJOR` and `64KB_STANDARD_SWIZZLE` values plus the Xbox `0x101`
  tiled value from slot 76. Row-major textures need `VK_IMAGE_TILING_LINEAR` (not usable for RT/DS) or a staging
  copy; `64KB_STANDARD_SWIZZLE` and `0x101` have no direct Vulkan equivalent. If the game uploads console-tiled data,
  the layer uses a detile step. `runtime/src/resource_detile.h` (`detile_surface_64k` / `tile_surface_64k`, `resource_detile_test`)
  and `runtime/src/gfx10_tiling.h` provide GFX10/RbPlus `SW_64KB_D` detiling. Which mode `0x101` uses, the mip tail and
  upload wiring remain **Open**.
- Formats: a `DXGI_FORMAT` → `VkFormat` table, including sRGB and typeless variants. Typeless resources get a
  concrete image format plus `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` and a `VK_KHR_image_format_list` when views cast
  within the family; unsupported casts are logged and fall back to the resource format. `CreateCommittedResource3`'s
  castable-format lists are mapped to a `VkImageFormatListCreateInfo`; buffers and unmapped formats are refused.
  `CreatePlacedResource2`'s list is mapped the same way on the placed path (D3D-90). Depth/stencil, BCn and packed
  formats map directly; any format the device rejects is a hard error with the DXGI value.
- `Map`/`Unmap`: upload/readback buffers map their host-visible memory; default-heap resources return
  `E_INVALIDARG` as D3D12 does. A texture in a CPU-visible custom heap maps only with a null pointer (its layout is
  the driver's), as on D3D12.
- GPU-writable slot-133 images on a default heap bind memory when replay, presentation or creation of a shader
  view first needs their Vulkan image. D3D12 placement and logical attachment views can precede that bind.
  A CPU-fillable, typed BC texture (filled by host copies or the staged upload) can initially sample
  an immutable zero image of the same format, extent,
  layer count and mip count. A CPU upload or explicit GPU command gives the placement private backing and
  republishes its existing descriptor keys. The cache holds at most 256 MiB. `LEONIDA_PLACED_IMAGE_DEFER=0`
  restores eager binding.
- A slot-70 sampled texture whose source pages are unreadable reserves its image backing on the first
  complete CPU read. Copies of descriptors written earlier resolve to the fallback until that upload succeeds.
  `LEONIDA_TITLE_IMAGE_DEFER=0` allocates these textures eagerly. The memory probe reports the live unbound
  image count and its Vulkan memory requirements in MiB.
- With `LEONIDA_TITLE_PARTIAL_MIPS=1`, such a texture can be filled before all its pages are readable. The XG
  layout of a 2D texture of one slice stores its mips from the smallest to the largest, so the mips from `m` on are
  the first `mip_chain_bytes(layout, m)` bytes from its base (`xg_xs_layout.h`; checked in `xg_xs_test` for tiled,
  tail and linear layouts). The probe (`texture_memory_readable_bytes`) returns how many bytes from the base are
  readable and `first_mip_within` the first mip those hold. When the last slot-70 view starts at that mip or later,
  the mips from there on are filled and only the descriptor keys of views that start there or later are published.
  The others stay on the fallback (`PendingImage::first_mip`) until a later fill has their mips. The image still has
  memory for every mip: `LEONIDA_TITLE_PARTIAL_MIPS_MB` (2048) limits the image memory of the textures filled in
  part. Arrays, cubes and volumes are filled whole or not at all.
- Small optimal Vulkan images synthesized for slot-70 texture views share memory pages. Each slice meets the
  image's size, alignment and memory type requirements. The GPU retirement queue destroys the image before its
  slice can be reused. Large images, dedicated-only images, and failed pool allocations use individual memory
  allocations. `LEONIDA_TITLE_IMAGE_POOL=0` selects individual allocation for every image. With
  `LEONIDA_MEM_PROBE` enabled, the texture census also reports logical Vulkan backing bytes for the registered
  slot-70 and slot-133 images and the portion on non-device-local types.
- A refresh pass visits a snapshot of the title-image registry, taken under its mutex, instead of walking the
  registry itself. Filling a texture can submit, a submission destroys a retired image, and that image's destructor
  erases its registry entry. Windows' mutex is recursive for its owner, so the erase lands inside the pass, and a walk
  over the map itself would step into a freed node (measured: an access violation reading the nil flag of a null
  child, 0x19, in `Device::refresh_title_textures`). The snapshot also holds a reference to each entry, so an entry
  retired during the pass is still alive when its turn comes.
- Sampled title images can use device-local memory even when Vulkan host image copies require a
  host-visible memory type. The runtime queries the same image with and without host-transfer usage.
  When only the latter permits plain device-local backing, it detiles the complete image into a reusable
  staging buffer and transfers all its mips and layers together. Linear slot-133 placements retain host
  copies. On a device without `VK_EXT_host_image_copy` every colour title image, linear placements too, takes the
  staged upload: the refresh before a submission has no other way to fill one, and without it the title's textures
  stayed zero (a flat green intro movie). An allocation failure retries the original host-copyable image.
  `LEONIDA_TITLE_IMAGE_STAGING=0`
  restores the original placement policy. With `LEONIDA_MEM_PROBE`, each title image class logs its
  Vulkan memory type mask, mask without host transfer, and selected type.
- `WriteToSubresource`/`ReadFromSubresource` (implemented in `d3d12_vulkan_device.cpp`) complete before they
  return, as D3D12 requires; a read cannot wait for a later submit. Textures in CPU-visible custom heaps are
  created with `VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT` when the device supports that usage with the requested
  image format and flags, moved to
  `GENERAL` and zero-filled on the host at creation, and copied with `vkCopyMemoryToImageEXT`/
  `vkCopyImageToMemoryEXT` while they stay in `GENERAL` (D3D12's `COMMON`). Every other case (no extension, a
  texture a barrier has moved, a default-heap texture, which D3D12 does not allow but the layer copies and logs) is
  a `CopyTextureRegion` from or to a host-visible staging buffer, replayed on the device's queue and waited for, so
  the layout tracker and a first use's zero fill behave exactly as for the application's own copies.
  Their arguments answer as the host runtime's do (`d3d12_subresource_copy_test` runs the same 117 steps on both
  devices and compares every HRESULT and the whole destination of every read; RTX 3060, driver 610.88): a
  subresource past the last, a box whose right, bottom or back edge lies past the mip's, or one that cuts a
  compressed block is `E_INVALIDARG`; an empty or inverted box that passes those checks is `S_OK` and copies
  nothing; the row and depth pitches are not checked, so rows or slices that overlap (a pitch smaller than a row,
  or 0) are copied in order. Not compared: null data, which the host faults on (the layer answers `E_INVALIDARG`);
  which bytes win when a read's rows overlap, which is the driver's copy order; and a default-heap texture, which
  the host refuses and the layer copies on purpose.
  - A driver can report `VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT_EXT` for a format, create the image with
    host-transfer usage, and then give it no memory type (`memoryTypeBits = 0`). This was measured on a GTX 1050 Ti
    with NVIDIA 582.78, and the validation layer of Vulkan SDK 1.4.363 reports nothing. Such a texture is made again
    without host-transfer usage before it is bound, and takes the staging path. `d3d12_vulkan_device_test` and
    `d3d12_subresource_copy_test` force this branch on any GPU with `VulkanDeviceInfo::force_host_transfer_retry`
    (their third CPU-copy path; the second compares it with the host runtime).
  - `LEONIDA_HOST_IMAGE_COPY` chooses whether the device enables the extension at all (`src/host_image_copy_policy.h`,
    tested by `host_image_copy_policy_test`): `1` wherever the device has it, `0` never, and `auto`, the default, as
    `1` except on NVIDIA (`vendorID` `0x10DE`), where it is off. On an RTX 4070 SUPER (Windows 11) a real boot lost
    the device with `WRITE_INVALID` in 7 of 7 runs that used host image copies and in 0 of 2 that did not; why is
    still under investigation. Off, the extension is neither enabled nor chained, no texture gets host-transfer
    usage, and every copy takes the staging path, as on a device without the extension. Device creation logs one
    line, `[rt] d3d12: host image copy: on (available)` or `off (...)` with the reason. Results with
    `LEONIDA_HOST_IMAGE_COPY=1` on other NVIDIA GPUs and drivers are welcome in an issue.
- 96-bit textures (`R32G32B32_FLOAT`/`_UINT`/`_SINT`/`_TYPELESS`, D3D-62) are `R32G32B32A32` images
  (`ResourceBinding::rgb96`), on every GPU. Most GPUs have no optimal-tiling RGB32 image (an RX 9070 XT reports
  no optimal features for `R32G32B32_SFLOAT`), and one that has would not avoid the repack: D3D12 places
  subresources at multiples of 512 bytes and Vulkan requires a copy's buffer offset to be a multiple of the
  12-byte texel (VUID-vkCmdCopyBufferToImage-dstImage-07975). What the application sees stays D3D12's:
  `GetCopyableFootprints` gives 12-byte texels (compared with WARP in `d3d12_rgb96_test`), and a view's T# names
  `GFX10_FORMAT_32_32_32_*`, whose swizzle reads alpha as one, so the image's fourth channel is never read.
  - `CopyTextureRegion` between a texture and a buffer is repacked by the D24 compute pass
    (`src/depth24_convert.comp` modes 2 and 3, `Depth24Job::rgb96`): the same scratch buffers, parts and lifetime,
    with texels of three words in the footprint and four in the scratch, copied as they are (an upload writes the
    fourth word as 0). The words are not interpreted, so NaN payloads, denormals and integers pass unchanged. On
    an RX 9070 XT (AMD 26.11.2) an upload and a readback take about 5 and 2 ms at 1024x1024 and about 20 and 45 ms
    at 4096x4096 (`d3d12_rgb96_test` with `LEONIDA_RGB96_LARGE` set; single runs that vary by a few ms, each the
    first submission of a new queue). The converter is created on every device for this, whether or not the
    GPU has a native D24.
  - Texture-to-texture copies and `CopyResource` are plain image copies, and only between two 96-bit textures.
  - `WriteToSubresource`/`ReadFromSubresource` always take the staged copy, as a D24 texture stored as D32 does:
    a host image copy would not repack.
  - Not done: render targets. WARP creates 96-bit render targets; this layer refuses `ALLOW_RENDER_TARGET` and
    leaves `RENDER_TARGET` out of `FORMAT_SUPPORT`, since a writable fourth channel changes destination-alpha
    blending. `ALLOW_UNORDERED_ACCESS` and `ALLOW_DEPTH_STENCIL` are refused, as on WARP (measured in the test).
    `GetResourceAllocationInfo` gives no size for a description creation refuses. A typed 96-bit texture has
    views of its own format only; the `TYPELESS` one has `FLOAT`, `UINT` and `SINT` views (sampled in
    `d3d12_descriptor_test`). `GetResourceTiling` reports no tiling for a committed 96-bit texture on both
    Microsoft WARP and an NVIDIA GTX 1660 SUPER: total tiles, the tile shape and every packed-mip field are zero,
    and the returned subresource-tiling count is zero (so its output element is untouched). This holds for
    `FLOAT`, `UINT`, `SINT` and `TYPELESS`; there is no standard tile shape to implement for a 12-byte texel.
- Resource flags map to Vulkan usage: `ALLOW_RENDER_TARGET` → `COLOR_ATTACHMENT`, `ALLOW_DEPTH_STENCIL` →
  `DEPTH_STENCIL_ATTACHMENT`, `ALLOW_UNORDERED_ACCESS` → `STORAGE`, `ALLOW_SIMULTANEOUS_ACCESS` → concurrent
  sharing, `DENY_*` → omit the corresponding usage. Buffers get the union of usages their states and flags imply;
  images copy dimension, format, mip levels, array layers and sample count.
- Xbox depth textures with `ALLOW_DEPTH_STENCIL | ALLOW_UNORDERED_ACCESS` can use legacy committed creation
  (slot 76), `CreateCommittedResource3` (forwarded by Xbox slot 127 in the boot stack), or placement at a guest
  address (slot 133). Device10 and slot 133 take an initial `D3D12_BARRIER_LAYOUT`. Both check the layout
  against the logical texture flags and validate the optimized clear against the native depth format.
  Xbox allocation queries return the attachment's placement size for XMem accounting; the writable
  planes are private backend allocations. The supported shapes are single-sample, single-mip 2D
  textures and 2D arrays. For `R32G8X24_TYPELESS`, use `R32_FLOAT` on depth plane 0 and `R8_UINT` on stencil
  plane 1. For `R16_TYPELESS`, use `R16_UINT` on plane 0 to access the raw D16 bits. An array UAV selects a
  contiguous range of layers. The depth/stencil and shader-resource views continue to use the attachment image.
  Slot 133 keeps the guest address. Identical, live placements share their image and writable planes across
  distinct COM objects. An incompatible reinterpretation at the same address is refused. A placement's extent is
  the PC allocation size, which can exceed the title's own block (RADV sizes a large D16 target above it, so the
  title's next block starts inside the range); a placement that starts inside another one therefore gets its own
  image when both layouts are opaque, since D3D12 leaves an alias's contents undefined until reinitialized, and is
  refused only for a non-opaque layout. Before this, the retail boot stopped on the title's assert while it created
  its D16 depth targets on an RDNA2 iGPU under RADV. A refused depth placement is logged (no trace line covers it).
  Where the provider (`Resource::enable_depth_stencil_uav`) has no unordered-access form of a texture it answers
  `E_INVALIDARG` or `E_NOTIMPL`, and slots 76 and 133 then make the texture without unordered access, as before the
  provider: slot 76 as a committed depth-stencil texture, slot 133 through its stand-in (the title-placed texture, or
  the committed one). The first such refusal per slot is logged (`xs_proxy_mock` checks the refusal against a mock
  provider). Any other failure (out of memory, the backing itself, a refused placement) is passed on as before.
  Console D16S8 (format 118) made as `D24_UNORM_S8_UINT` via `pc_format()` (the default on GPUs with D24S8
  attachments, such as NVIDIA) keeps its UAVs through the widened `R16_TYPELESS` + `R8_UINT` storage planes:
  plane-0 barriers convert between 24-bit UNORM words in transfer scratch and `R32_SFLOAT` via `Depth24Converter`
  and blit between the intermediate `R32_FLOAT` image and `R16_UNORM` (viewed as `R16_UINT` or `R16_UNORM`), while
  plane 1 copies stencil bytes directly (`xbox_d16s8_d24_storage` with `LEONIDA_CONSOLE_D16S8=d24`).
- Legacy and enhanced texture transitions copy the selected planes and layers to or from their storage images.
  Fence-ordered graphics and compute queues can share UAV access. A dedicated compute family can perform
  the plane transfer itself when `VK_KHR_maintenance10` and the format's depth/stencil copy flags allow it.
  Otherwise the graphics queue issues the explicit transitions on either side of the compute work. The two
  queue-specific COMMON layouts allow implicit UAV access, so replay refuses them for these textures.
  Generic COMMON remains available for copies, shader reads and queue handoffs.
  `d3d12_depth_stencil_uav_test` verifies these paths with a synthetic shader and per-pixel readback.
- Copies: `CopyBufferRegion` → `vkCmdCopyBuffer` with arena offsets; `CopyTextureRegion` →
  `vkCmdCopyBufferToImage`/`vkCmdCopyImageToBuffer`/`vkCmdCopyImage`, using the `D3D12_PLACED_SUBRESOURCE_FOOTPRINT`
  row pitch and height; `CopyResource` → a whole-subresource copy; colour resolves → `vkCmdResolveImage`;
  depth and stencil resolves → a `vkCmdBeginRendering` scope with a resolve attachment (section 3.2).
  Partial resolve regions to another offset need a shader or a region copy.
  - A placed footprint must have an `Offset` that is a multiple of 512 and a `RowPitch` that is a multiple of 256
    (`D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT`, `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT`). Any other is refused when it is
    recorded, so `Close` returns `E_INVALIDARG` (`CommandList::valid(const CopyLocation&)` in
    `runtime/src/command_list.h`; `command_list_test` has both cases). That is D3D12's rule for a device that
    reports `UnrestrictedBufferTextureCopyPitchSupported` as `FALSE` in `D3D12_FEATURE_D3D12_OPTIONS13`, which this
    device does on purpose (`d3d12_vulkan_device_test` fails on a `TRUE`). The PC runtime answers differently: on an
    RTX 4090 (driver 621.14) and on WARP it reports the capability as supported, and a list with `Offset` 16 and
    `RowPitch` 260 for an `R8G8B8A8_UNORM` footprint closes with `S_OK` (measured with a stand-alone D3D12 program,
    Windows 11 26200). The layer cannot simply say the same: Vulkan takes a copy's buffer offset as a multiple of
    the texel block size and its row length in texels (VUID-vkCmdCopyBufferToImage-dstImage-07975), while the
    capability lifts D3D12's two alignments and is not documented as keeping offsets and pitches texel multiples
    (not tested here), so a footprint that is not texel-aligned would have to be repacked through scratch memory
    the way a 96-bit one is (section 4.2). The replay already accepts any pitch that is a texel multiple and any
    offset that is one, except that a depth, stencil or 96-bit copy needs an offset that is a multiple of 4
    (`prepare_texture_copy` in `runtime/src/vulkan_replay.h`; anything else is `unsupported` there), so accepting
    those needs only the record-time check changed; reporting the capability needs the repack as well. What the
    console's runtime reports is not measured, and no title code that depends on it has been seen.
- `WriteBufferImmediate` (D3D-26) → `vkCmdUpdateBuffer` of 4 bytes. The address is resolved when the list is
  executed, through `ReplayFunctions::buffer_at`: the buffer whose `ResourceBinding::va` range holds it, at
  `buffer_offset + (address - va)`. `VulkanContext` finds it by binary search over the buffers' VA ranges
  (`runtime/src/buffer_va_index.h`, kept in step with the registered resources); aliasing placed buffers can
  overlap: the one whose range starts closest below the address and also holds the bytes the view or write needs
  wins, or, when none holds all of them, the one that starts closest below (`buffer_va_index_test`). These are the same fields and lookup as #291's arena, and the device fills
  `va` once `GetGPUVirtualAddress` lands (#157/#291); until then, a list with an immediate write is refused as
  `unresolved_resource`. `DEFAULT` adds no barrier: it is ordered like any copy, by the application's barriers.
  `MARKER_OUT` waits for every earlier command to finish and sees its writes (`ALL_COMMANDS`/`MEMORY_WRITE` →
  `TRANSFER`/`TRANSFER_WRITE`). `MARKER_IN` only needs earlier commands to have started. Vulkan cannot express
  that, because `TOP_OF_PIPE` in a first synchronisation scope is no stage at all, so it gets an execution-only
  `ALL_COMMANDS` → `TRANSFER` dependency: stricter than D3D12 requires, never weaker. Recording refuses a null
  or unaligned address and an unknown mode (the debug layer's `WRITEBUFFERIMMEDIATE_*` messages, IDs 1090-1092 in
  `d3d12sdklayers.h`).
- `AtomicCopyBufferUINT` / `AtomicCopyBufferUINT64` (D3D-26) → one `vkCmdCopyBuffer` region of 4 or 8 bytes, after
  a `ALL_COMMANDS`/`MEMORY_WRITE` → `TRANSFER`/`TRANSFER_READ|WRITE` barrier. That barrier covers every earlier
  write, those to the dependent resources among them, so the copy sees them all, as D3D12 requires. The dependent
  ranges are checked against their buffers (`end == UINT64_MAX` resolves to the buffer's size) but add nothing a
  global barrier does not already cover. Recording matches live Windows 11 WARP and hardware D3D12 `Close`
  validation: offsets aligned to and within the buffer for the copy size, zero dependencies allowed (with null
  arrays), buffer dependency ranges with `begin < end` and either `end == UINT64_MAX` or `end <= width`, and
  texture dependency ranges `{0, UINT64_MAX}` on a valid subresource or `ALL_SUBRESOURCES`. **ASSUMPTION:** Vulkan
  promises no atomicity for a transfer, and the layer relies on a naturally aligned 4- or 8-byte copy being a
  single write on the GPU. This matters only to a reader racing the copy on another queue or on the host.
- Clears: a full-subresource clear maps to `vkCmdClearColorImage`/`vkCmdClearDepthStencilImage` (UAV buffers to
  `vkCmdFillBuffer`); a rect clear uses `vkCmdClearAttachments` inside the rendering scope. UAV image clears use
  `vkCmdClearColorImage` in `GENERAL`, and rectangles `vkCmdClearAttachments`. Buffer patterns of other sizes
  need no compute clear: 8/16-bit elements are a 32-bit fill with copied ends, 64/128-bit ones an update doubled by
  copies (section 3.2, UAV clears).
- **Typed UINT RTV clears:** `ClearRenderTargetView` still takes `FLOAT[4]`. For the currently exposed
  `R8G8B8A8_UINT`, `R32_UINT` and `R32G32B32A32_UINT` resources, convert those values numerically into
  `VkClearColorValue::uint32` for both image and rectangle clears; copying float bits gives incorrect stored
  integers without a Vulkan validation error. **Measured:** Windows D3D12 on WARP and AMD RX 5700 XT clamps
  negatives/NaN to zero, positive overflow/infinity to the component maximum, and truncates fractions.
  `d3d12_clear_test` (renamed from `d3d12_uint_clear_test` in #525, D3D-45) compares full and rectangular D3D12
  readbacks with WARP over eleven integer input sets on every UINT and SINT render-target format it lists, beside
  the UNORM/SNORM/FLOAT and depth/stencil cases. ctest runs only its `--rgb9e5` mode (`d3d12_rgb9e5_rtv`), so the
  WARP comparison is run by hand until #521's 0.5 tie rounding lands (`runtime/CMakeLists.txt`).
  `clear_color_test` checks both replay paths without a GPU, including unchanged float values.
  This does not add typed-view reinterpretation of typeless resources or new integer formats.
- **RGBA8 typeless RTV clears:** explicit UNORM, SRGB, UINT, SINT and SNORM RTVs on a
  `R8G8B8A8_TYPELESS` 2D resource with sample count 1, selecting a mip and a contiguous array-layer range.
  `TargetView` captures its format by value; copies and layout tracking keep the resource's base format.
  Reinterpreted full clears use dynamic rendering because `vkCmdClearColorImage` cannot select a view format.
  Rectangle clears use the same typed attachment view. The view-cache key includes the format, and views
  request only color-attachment usage so SRGB does not inherit an unrelated storage requirement.
  Same-format image clears and legacy float-only replay clients retain their existing path.
  On an RX 5700 XT, 13 synthetic full/rectangle readbacks match Windows WARP (UINT exact, normalized
  channels within one byte), including descriptor rewrites, format switches and an SRGB RTV on a UAV resource.
  The same tests fail without this change and when the cache ignores format. CPU replay covers 31 scenarios;
  17 unsupported/invalid frontend views fail `Close` without submission. Typed-resource casts, MSAA and 3D
  typeless RTVs remain unsupported.
  Phase 2 adds 54 mip/array and 12 non-array higher-mip readbacks against WARP, including untouched sentinels.
  D3D-161 adds SINT and SNORM views when the Vulkan format supports color attachments. SINT uses the existing
  integer clear conversion, and SNORM passes floating-point values to the attachment. Signed channels matched
  WARP byte for byte in full and rectangular clears on an RX 9070 XT (driver 32.0.32029.2010) and the integrated
  Radeon in a Ryzen 5 9600X (32.0.21045.29003), including mip and array selections. The SNORM vectors avoid
  half-step rounding ties.
- **Typed RTVs of the other typeless families (D3D-275):** an RTV of any TYPELESS color texture (B8G8R8A8,
  R16G16B16A16, R32G32B32A32, R32G32, R16G16, R10G10B10A2, R8G8, R32, R16, and R8 once it is mapped) names a typed
  format of its family; `TargetView` carries that view's VkFormat (`ColorViewFormat` holds VkFormat numbers) and
  `color_view_format` accepts any format of the resource's family (`typeless_view_family`), so the clear writes
  through a view in that format. Typeless images are already mutable without a format list. Before, only RGBA8 was
  reinterpreted and the others cleared through the resource's own format (an `R8_UINT` RTV cleared to 7 wrote 0xff).
  `d3d12_typeless_families_test` clears every typed RTV format of each family with two value sets and compares the
  bytes with the host D3D12 on the same adapter: 62 of 62 equal on an RTX 3060 (NVIDIA 32.0.16.1714) with
  `LEONIDA_VK_VALIDATION=1`; WARP differs from that host in 8 (rounding of 0.5 to UNORM/SNORM and the
  R10G10B10A2_UNORM alpha). Formats the layer does not map yet (R16G16B16A16_SNORM, R16G16_UNORM/SNORM,
  R16_UINT/SNORM/SINT, the R8 family; #1914) are reported and skipped. Draws need no change: the replay builds a
  draw's attachment view in the PSO's `RTVFormats[slot]` (`GraphicsPipeline::color_formats`, applied in
  `vulkan_replay.h`'s draw setup), which D3D12 requires to equal the RTV's format, so pipeline and attachment
  already agree and the mutable image is written in the typed format. The same test draws a full-screen pass (GLSL
  stand-ins for translated shaders found by FSSM hash; HLSL through d3dcompiler_47 on the host) writing
  `uint4(7, 300, 1000, 3)` or `float4(0.5, -2, 1.5, 0.25)` into R16G16B16A16_UINT/FLOAT/UNORM, R32_UINT/FLOAT,
  R10G10B10A2_UINT/UNORM and R8G8B8A8_UINT views of the matching TYPELESS textures: 8 of 8 equal the host, with no
  validation error, before and after the clear fix.
- **RTVs through a castable-format list in the other channel order:** an `R8G8B8A8` or `B8G8R8A8` texture (UNORM or
  SRGB, `B8G8R8X8` included) whose castable-format list names the RTV's format in the other channel order, across
  `B8G8R8X8` and `B8G8R8A8`, or a typed `B8G8R8A8` texture's listed SRGB format, gets an RTV in that format.
  `CreateRenderTargetView` checks the list as the application gave it (`lists_castable_format`), and
  `color_view_format` lets a castable image's clear cross between the RGBA8 and BGRA8 orders. WARP and AMD's D3D12
  driver (RX 9060 XT, 26.11.2) store clears and draws in the view's channel order and encoding: an `R8G8B8A8` texture
  cleared to (0.2, 0.4, 0.6, 0.25) through a `B8G8R8A8` RTV holds the bytes 153, 102, 51, 64. Before, the RTV was
  refused for an `R8G8B8A8` texture (its clear failed `Close` and its draws were dropped), and a typed `B8G8R8A8`
  texture's RTV kept the image's format whatever the view named, so its clears came out in BGRA order and without
  the sRGB encoding (its draws were right already, as they use the PSO's format). A format the list doesn't name
  is refused, on a typed `B8G8R8A8` or `B8G8R8X8` texture as on an `R8G8B8A8` one (the `B8G8R8A8` and
  `B8G8R8X8` ones kept the image's format before); a typed `B8G8R8A8` texture without a list still ignores the
  RTV's format, as before. `d3d12_clear_test` compares 8 such textures with WARP in full and rectangle clears
  within 1 (the X byte of an X8 view left out: WARP and AMD write different values there) and checks that an
  `R8G8B8A8`, a `B8G8R8A8` and a `B8G8R8X8` texture each take their own and their listed format and refuse 3
  formats their lists don't name, `d3d12_draw_test` draws through the same views, and `typeless_rtv_test` checks
  the replay's crossing and its refusals.
- **Typed RTV clears:** `ClearRenderTargetView` still takes `FLOAT[4]`. For every integer RTV format the device
  exposes (`R8G8`, `R8G8B8A8`, `R16G16`, `R16G16B16A16`, `R32`, `R32G32` and `R32G32B32A32` in `_UINT` and `_SINT`,
  plus `R10G10B10A2_UINT`; `R8`/`R16` `_UINT`/`_SINT` are handled in replay too) convert those values numerically
  into `VkClearColorValue` for both image and rectangle clears; copying float bits gives incorrect stored integers
  without a Vulkan validation error. **Measured:** Windows D3D12 on WARP clamps negatives/NaN to zero, positive
  overflow/infinity to the component maximum (and signed below the minimum), and truncates fractions; signed formats
  use `int32` with the same NaN/overflow rules. `R10G10B10A2_UINT` clamps RGB to 1023 and alpha to 3 (inputs 4, 4.5
  and 2^32 in alpha match WARP). `d3d12_clear_test` compares full and rectangular D3D12 readbacks with WARP across
  UNORM, SNORM, FLOAT, all 15 exposed UINT/SINT render targets (11 value vectors, including 8-/16-/10-/2-bit range
  boundaries) and depth/stencil; `clear_color_test` checks both replay paths for 19 integer formats without a GPU,
  including unchanged float values. SRGB clears are omitted from the comparison: the
  linear→sRGB encoding rounds differently at the `.5` boundary on WARP vs Vulkan (a 1-ulp driver difference).
  **Open:** on NVIDIA RTX 5080 (driver 616.56) clears that land exactly on a `.5` UNORM/SNORM step round down where
  WARP rounds up: `R8G8B8A8_UNORM`/`SNORM` 0.5 store `7f`/`3f` vs WARP `80`/`40`, and a `D16_UNORM` 0.5 depth clear
  stores `0x7fff` vs WARP `0x8000` (all on `main` before this change; same 1-ulp class as SRGB, not addressed here).
  `D32_FLOAT` and `D32_FLOAT_S8X24_UINT` match WARP.
  This does not add typed-view reinterpretation of typeless resources.
- **Depth clears.** Microsoft documents a `[0, 1]` clamp. On Windows 11 build 26100, both WARP and native D3D12 on
  an RTX 5070 Ti (610.74) preserved finite out-of-range `D32_FLOAT` values and quiet NaN in full and rectangle
  clears. Replay preserves those values when the Vulkan device has enabled `VK_EXT_depth_range_unrestricted`.
  With the extension disabled, replay clamps finite values to `[0, 1]` and uses zero for NaN. The UNORM depth
  format conversions still apply. [D3D12 reference](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-cleardepthstencilview)
- `D3D12XBOX_RESOURCE_FLAG_FORCE_TEXTURE_COMPATIBILITY` (public release note) and DCC: ignore the flag, let the
  Vulkan driver choose compression, and fail loudly if the game aliases a buffer over a compressed image or reads
  raw DCC metadata.
- **G-buffer render targets stay 8-bit UNORM.** Reported by @Silliana in #603 (not reproduced here): the material
  shaders pack data into plain bytes (the normal is octahedral-packed across three bytes of one target, and ID
  bytes sit in another) and the lighting shaders unpack them with a `* 255` and a truncate. If the layer or a test
  gives those targets another format (say f16 for everything), a byte is lost in many pixels: the normals go wrong
  and the specular sparkles. Create them with the format the game asks for (`R8G8B8A8_UNORM` and the like) and don't
  substitute a wider or float format for them.

### 4.3 Barriers and layouts

Plain buffer copies order earlier transfer writes on the same queue before reading or writing the next range.
Set `LEONIDA_ORDER_BUFFER_COPIES=0` to omit this dependency when comparing replay behavior.

Track a current state/layout per subresource. D3D12 states are bitmasks; a combined state takes the union of the
mapped stages/accesses, and if no single layout serves all uses, use `GENERAL` and log it. Legacy
`D3D12_RESOURCE_STATE_*` maps to a (stage, access, layout) triple:

| D3D12 state | Vulkan layout | Stage / access |
|---|---|---|
| `COMMON` | `GENERAL` for images, no layout for buffers | Promote on first use |
| `RENDER_TARGET` | `COLOR_ATTACHMENT_OPTIMAL` | `COLOR_ATTACHMENT_OUTPUT` / write |
| `DEPTH_WRITE` / `DEPTH_READ` | `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` / `DEPTH_STENCIL_READ_ONLY_OPTIMAL` | early/late tests |
| `PIXEL_SHADER_RESOURCE` | `SHADER_READ_ONLY_OPTIMAL` | fragment shader / read |
| `NON_PIXEL_SHADER_RESOURCE` | `SHADER_READ_ONLY_OPTIMAL` | vertex/compute/etc. / read |
| `UNORDERED_ACCESS` | `GENERAL` | shader / read+write |
| `COPY_DEST` / `COPY_SOURCE` | `TRANSFER_DST_OPTIMAL` / `TRANSFER_SRC_OPTIMAL` | transfer |
| `RESOLVE_DEST` / `RESOLVE_SOURCE` | `TRANSFER_DST_OPTIMAL` / `TRANSFER_SRC_OPTIMAL` | resolve |
| `STREAM_OUT` | no layout for buffers | transform feedback write (if used) |
| `VERTEX_BUFFER` / `INDEX_BUFFER` / `INDIRECT_ARGUMENT` | no layout for buffers | vertex input / index / draw indirect |
| `PRESENT` | `PRESENT_SRC_KHR` for swapchain images, `GENERAL` otherwise | present |
| `RAYTRACING_ACCELERATION_STRUCTURE` | buffer, no layout | acceleration structure read/write |

- Split barriers (`BEGIN_ONLY`/`END_ONLY`, and enhanced `SYNC_SPLIT`) are an optimisation. The resource is unusable
  between the halves, so one full barrier at either point is correct; the IR records it at the end point
  (`command_list.h`). Use `VK_KHR_synchronization2` events if profiling needs the overlap.
- Implicit promotion and decay need no extra barriers: `COMMON` is `GENERAL`, and the layout tracker makes later
  barriers start from it (section 3.2).
- Aliasing barriers: if image aliasing is implemented (one `VkDeviceMemory` per D3D12 heap range, with
  `VK_IMAGE_CREATE_ALIAS_BIT`), transition the old image to `UNDEFINED`, issue a global memory barrier, then
  transition the new image. Until then, log aliasing and allocate distinct memory; do not silently return the
  wrong contents.
- Enhanced barriers (`D3D12_BARRIER_SYNC_*`, `D3D12_BARRIER_ACCESS_*`, `D3D12_BARRIER_LAYOUT_*`) map directly to
  `VkPipelineStageFlags2`, `VkAccessFlags2` and `VkImageLayout` (`vulkan_sync`, `vulkan_barrier_access`,
  `vulkan_layout`). Queue-specific layouts map like their generic forms; ray tracing, video and split scopes use
  `ALL_COMMANDS`; `LayoutBefore = UNDEFINED` or the discard flag becomes `oldLayout = UNDEFINED`. `SYNC_COPY` is
  `COPY | CLEAR`, not `COPY` alone: `WriteBufferImmediate` is recorded as `vkCmdUpdateBuffer` and
  `ResolveQueryData`'s never-written queries as `vkCmdFillBuffer`, which synchronization2 runs in the `CLEAR` stage
  (D3D-838; `vk_command_replay_test` part K is a read-after-write hazard under synchronization validation without
  it).
- `LayoutAfter = UNDEFINED` from a defined layout (D3D-BARRIER-INTO-UNDEFINED) releases the subresources: their
  contents are no longer needed, as before their memory is aliased. Vulkan has no transition into `UNDEFINED`
  (VUID-VkImageMemoryBarrier2-newLayout-01198), so each piece gets a barrier with `oldLayout = newLayout`, the
  layout the tracker has for it: the accesses are ordered and nothing in the image changes. The tracker keeps those
  layouts. A piece never used yet gets no barrier and is not zero-filled for it. The next barrier on these
  subresources has to start from `UNDEFINED`, which discards from whatever layout they are in, as before. The list
  closes, as it does on WARP (measured on Windows 11 26340 in #984). #984 also reports that the retail title ends a
  depth buffer's use this way and takes a failed `Close` as fatal; the layer failed such a list's `Close` until
  this change. `UNDEFINED` to `UNDEFINED` is a separate case (#600).
  - **Tests:** `command_list_test` and `replay_texture_barrier_test` (CPU: the barriers recorded, the kept layouts,
    an unused image, the barrier that follows); three rows of `d3d12_close_parity_test`; `vk_command_replay_test`
    part L (a colour target, a depth buffer whose planes are in two layouts and an unused image released, the target
    used again and read back); and `d3d12_com_test`, where a depth buffer is cleared, released, taken from
    `UNDEFINED` again, cleared and read back through the D3D12 list and queue (16 depth and 16 stencil texels
    exact). Run on an RTX 4070 Ti (NVIDIA 620.70.06, Linux): part L natively with the validation layer 1.4.363 and
    no error, where the same barriers with `newLayout = UNDEFINED` give VUID-VkImageMemoryBarrier2-newLayout-01198
    four times; the D3D12 tests under Wine.
- A rejected recording call logs `<method> rejected a call (d3d12_com.cpp:<line>)` the first time each call site
  rejects one (Barrier and CopyTextureRegion also print their arguments), and a batch the queue's replay refuses
  logs `queue replay refused a batch at vulkan_backend.h:<line>`, so a failed `Close` or
  `ExecuteCommandLists failed: unsupported` names its cause.
- `DiscardResource` (D3D-26): each discarded image subresource gets a barrier from `oldLayout = UNDEFINED` to the
  layout the tracker already has for it, so the driver may drop the contents while later commands see no layout
  change. The other cases are no-ops, which "contents undefined" allows: buffers, discards with rectangles (only
  part of each subresource), a part never used yet (it is still to be zero-filled on first use), a list replayed
  without a tracker, and one plane of a combined depth-stencil image without `separateDepthStencilLayouts`
  (discarding one plane would also discard the other). A null region is recorded as `whole`; a region is always an
  explicit range, even with `NumSubresources = UINT_MAX`, so its `FirstSubresource` is checked like any other.
- Access bits and sync scopes are not one-to-one. D3D12 lets an access be named with a sync scope whose Vulkan
  stages cannot perform it: `SYNC_ALL_SHADING` with `ACCESS_VERTEX_BUFFER` is valid D3D12, but
  `VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT` needs `VERTEX_ATTRIBUTE_INPUT`, which shading stages are not. Vulkan
  rejects such a barrier. `vulkan_barrier_scope` therefore widens each side's stages by the narrowest stage that
  can perform each access without one. The table is the synchronization2 chapter's "supported access types". The
  stages it adds:
  - `DRAW_INDIRECT` for indirect arguments;
  - `INDEX_INPUT` and `VERTEX_ATTRIBUTE_INPUT` for index and vertex buffers;
  - all shading for constant buffers, SRVs and UAVs;
  - `COLOR_ATTACHMENT_OUTPUT` for render targets;
  - the fragment tests for depth/stencil;
  - `ALL_TRANSFER` for copies and resolves. `CLEAR` alone cannot read, and `ALL_GRAPHICS` contains no transfer stage.

  The access itself is never dropped, so the dependency still covers everything the application named, over a
  slightly larger scope.
- Render passes: `BeginRenderPass`/`EndRenderPass` map to `vkCmdBeginRendering`/`vkCmdEndRendering` (dynamic
  rendering). The `D3D12_RENDER_PASS_BEGINNING_ACCESS`/`ENDING_ACCESS` types give the load/store ops and resolve
  targets. Suspending/resuming passes have no direct Vulkan equivalent; split them into two rendering scopes with
  the same attachments and `LOAD`/`STORE`. If the game does not use render passes, build a rendering scope per draw
  from the bound RTVs/DSV and let explicit `Clear*View` calls do the clears.

### 4.4 Fences, queues and frames in flight

- One timeline semaphore per `ID3D12Fence`. Queue `Signal` = `vkQueueSubmit` signal; queue `Wait` = submit wait.
- **Wait before Signal across queues, with resources handed over** (`src/queue_holds.h`, `VulkanQueue`; test
  `d3d12_queue_handover_test`). Vulkan accepts a timeline wait whose signal comes later, but the layer replays every
  `ExecuteCommandLists` against one `LayoutTracker` in the order the calls arrive, so a queue that waits for work
  submitted afterwards would be recorded against the layouts from before that work (a copy list that reads a texture
  a compute list has not yet written was replayed against `UNDEFINED`: zero-filled, and a validation error). The layer
  therefore tracks, per fence, the highest value a Signal has been *submitted* for (or the CPU has set; the creation
  value counts), which is not the completed value. A queue that is asked to run a batch (or a `Signal`) while a
  `Wait` it carries names a value above that, or while an earlier batch of the queue is held, copies the lists
  (`CommandList` is a value; the application may `Reset` its list at once) and holds the operation. Everything the
  queue is asked to do after it (batches, Signals, queue markers) goes behind it, so per-queue order is kept.
  The held operations are recorded and submitted, in order, on their own `VkQueue` as soon as the Signals they wait for
  have been submitted: by the thread that submits that Signal (or makes the CPU `Signal`), after it has dropped its own
  queue's state lock. There is no worker thread and no GPU wait is added, but the call that releases is not free: that
  thread records the held batches (what the `ExecuteCommandLists` would have cost, including whatever `execute` does
  for the lists, such as the bindless upload's wait for the device until D3D-314 removes it) while it still holds the
  mutex of the D3D12 queue whose `Signal` or `Wait` it is in, so other calls on that queue wait meanwhile; and
  `remove_fence`, queue destruction and presenter destruction wait for a release in progress. One thread releases at a
  time (the others leave a note and the running one makes another pass); a release that submits a Signal lets the
  next queue's front run in the same pass, so chains (C waits B, B waits A, in any submission order) and operations
  with several waits resolve in dependency order, and the layout tracker follows that order. A wait whose value has
  been reached by a submitted Signal never blocks again (vkd3d-proton's rule too). The normal case (the Signal
  submitted first) is unchanged: no copy, no hold, the same `vkQueueSubmit`. Holding on the CPU also avoids a Wait and
  its Signal landing in the wrong order on one `VkQueue`, which would not make forward progress on the GPU
  (Vulkan-Docs issue 1262): the test also runs with every D3D12 queue on one `VkQueue` and with two.
  What a held operation keeps alive, as D3D12's queue does: the fences it waits on and signals, until its submission
  has completed on the GPU (a released fence stays until then, which also removes the destroy-while-pending
  validation error the unheld path could hit); the pipeline states and root signatures of its lists (copied from the
  recording before the application may `Reset` it, so releasing a PSO right after `ExecuteCommandLists` is fine).
  Resources are not kept: a resource released while a batch that names it is held fails that batch at release.
  Order against other work: a held `Signal(F, v)` released after an unheld `Signal(F, w)` with `w >= v` leaves the
  Vulkan signal out (timelines only increase; waits for `v` already pass), decided under the submit lock so the
  order of the checks is the order of the submissions, and the value counts as submitted only once the
  `vkQueueSubmit` succeeded (a failed Signal releases nobody and its retry goes through); `Present` is queued behind
  held work too (below). Fence keep-alives are released outside the queue lock, at the next `ExecuteCommandLists`,
  `Signal`, `Wait` or marker once their submission has completed. Edge cases:
  a queue destroyed with held operations reports them once (stderr: `queue destroyed with N held operation(s) never
  submitted: waits for a fence value (V) that no Signal reached`) and goes on without waiting, and the CPU waits on
  the fences of its held Signals are completed, as a released fence completes them; queues that wait for those fences
  stay held and are reported when they go; a fence nobody signals holds back only the queues that wait for it (and
  for what they would have signaled), never the others; a Signal value that is not above the fence's is still
  dropped with a note at call time, as before (vkd3d-proton instead maps virtual fence values to physical timeline
  values; the layer does not yet). A held `ExecuteCommandLists` that fails when it is released does not return its
  result to a call that has returned: it is printed, `last_execute_result` and `VulkanQueue::take_held_failure`
  report it, the `Signal` behind it still signals (the work did not run; nothing waits forever, as for a failure that is
  not held), and its waits stay for the queue's next submission, as after a failed `vkQueueSubmit`. A queue does not
  hold `UpdateTileMappings` / `CopyTileMappings`: they still submit at once, before earlier held work, and take the
  queue's pending waits (documented limit). `VulkanQueue::idle` waits for submitted work only.
  `Present` (`VulkanPresenter`): when any queue holds work, a presented back buffer would be blitted as it was before
  that work, so the Present is queued instead (`QueueHolds::after_held`), returns success as a held
  `ExecuteCommandLists` does, and is submitted, in order with the presents queued before it, by the thread that
  releases the last operation held when it was called. It is not blocked on the calling thread because the Signal that
  releases the work may be one that thread makes after `Present` returns. Held work that is never released holds back
  the presents after it (device-wide, since the layer cannot tell which batch draws into which back buffer). A
  blank present reads nothing and is only ordered behind earlier queued presents. At most 8 presents queue up; the
  next one waits for held work to release (logged once; `set_abort` ends the wait), as a full frame-latency queue
  does. A queued present reports `last_present_tore()` false, which the Xbox PresentX path turns into the default
  vblank sync for the frame pacer. The presenter's state is guarded by one mutex (the counters are atomics, so
  reading them never waits for a present blocked on a swapchain image); `set_abort` has its own.
  Facts from vkd3d-proton (LGPL, approach only, nothing copied): it keeps a CPU-side virtual value and pending signals
  per fence and blocks the queue's submission thread until a signal that satisfies a wait has been submitted, so Vulkan
  never sees a wait without a submitted signal. Resources are `VK_SHARING_MODE_EXCLUSIVE` and every D3D12 queue is in
  the graphics family, so no ownership transfer is needed; with dedicated compute/transfer families they would need
  `CONCURRENT` (vkd3d-proton does that, because D3D12 gives no information for transfers).
  Every mode of the handover test ends with the same cases on the host runtime (the expected data is the CPU's
  pattern in all of them, so that run shows the cases are valid D3D12 and holds the layer's modes to one bar).
  The pipeline-state and root-signature pins are covered end to end by `d3d12_m4_compute_test`: the compute list is
  executed while held, its two PSOs and root signature are released and the list is Reset, the copy that releases
  it is submitted, and the output is compared with the CPU (it reads zeros without the pins).
  Measured: `d3d12_queue_handover_test` (three modes: the standalone device with a `VkQueue` per D3D12 queue, then the
  host runtime; `--two-queues`, a caller-owned device with two `VkQueue`s handed over; `--shared-queue`, one) runs
  592 cases on the layer on the Radeon 880M and the RTX 5070 (also with one shared `VkQueue`; 8 with two handed over) and 587 on the host runtime: pairs and three-stage chains (produce, relay, consume)
  over the queue type assignments that fit and all 6 submission orders (consumer first among them), two producers
  (texture from one queue, buffer from another) with one consumer, a chain released by a CPU `Signal`, a CPU
  `Signal` before and after the `Wait`, a never-signaled fence, a fence released while a batch waits, a held Signal
  of a fence the application released, a held Signal below an unheld one on a shared fence, teardown with a held
  Signal under a CPU wait, and 200 rounds with two threads signalling and releasing at once; results compared byte
  for byte, sync validation clean. With holding disabled the test fails 236 cases and validation reports
  `VUID-vkCmdDraw-None-09600`; with the monotonic check disabled it reports
  `VUID-VkSubmitInfo-pSignalSemaphores-03242`; `vk_presenter_sync_test` and `d3d12_m4_compute_test` (which
  submits the compute list first and releases its PSOs and root signature before the copy that feeds it) fail
  without the Present ordering and the pins.
- **More D3D12 queues than Vulkan queues** (`pick_queue_slot` in `src/queue_families.h`, `CreateCommandQueue`;
  test `d3d12_queue_share_test`). A GPU can expose fewer `VkQueue`s than a title creates D3D12 queues: the Intel
  UHD 770 on Windows and lavapipe have one family with `queueCount` 1, so the second D3D12 queue of any type was
  refused with `E_OUTOFMEMORY` ("no independent Vulkan queue available") and a title with a DIRECT and a COMPUTE
  queue stopped at boot (PingasMech, #4863). A new D3D12 queue takes an unused `VkQueue` of its own type's family,
  then of the graphics family, as before (D3D-97: one `VkQueue` per live D3D12 queue while there are enough, so
  nothing changes on a GPU with enough queues; an RTX 5080 gives 16 graphics, 8 compute and 2 transfer queues); when
  every one is in use it shares the one with the fewest D3D12 queues (its own family's first on a tie) and logs that
  once per type. D3D12 queues on one `VkQueue` stay separate objects: each keeps its own batches in order, its own
  progress timeline and its own held operations; the `VkQueue` runs their batches one after the other, in the order
  they reach it, so they lose concurrency, not correctness (as on one family, below). What would break is a GPU
  wait that no submitted signal satisfies landing on the shared `VkQueue` ahead of the signal it waits for, which
  never makes progress. The holding above already rules that out: a batch reaches `vkQueueSubmit` only once every
  fence value it waits for has a *submitted* Signal (earlier on the same `VkQueue`, or on another one) or has been
  set by the CPU. One case let the CPU half slip: a CPU `Signal` that has to wait behind a queue Signal the GPU has
  not reached yet (`VUID-VkSemaphoreSignalInfo-value-03259`, applied later by the fence wait thread) never told the
  held queues, so a queue waiting for that value was held for ever, on separate `VkQueue`s too. The wait thread now
  reports the value once it has set it, and releases what it satisfies; reporting it earlier, when the CPU called
  `Signal`, would send the waiting batch ahead of the queue Signal it depends on, which on a shared `VkQueue` never
  completes. `vkQueueSubmit`, `vkQueueBindSparse`, `vkQueuePresentKHR`, `vkQueueWaitIdle` and the queue labels
  need the `VkQueue` externally synchronised; every call the layer makes holds the device's one submit lock
  (`VulkanContext::submit_mutex`), which covers shared `VkQueue`s as it covers the staging queue the first DIRECT
  queue already shares. A per-`VkQueue` lock (#3841) must key on the `VkQueue` handle, not the D3D12 queue. Costs on
  a shared `VkQueue`: `GetClockCalibration` without calibrated timestamps and the destruction of a D3D12 queue wait
  for the other D3D12 queues' submitted work too, and queue debug labels (`BeginEvent`/`EndEvent`) of the D3D12
  queues interleave on it. Two test settings make the standalone device look like such a GPU, so the sharing path
  runs anywhere: `LEONIDA_VK_MAX_QUEUES_PER_FAMILY=N` (at most N `VkQueue`s per family) and
  `LEONIDA_VK_GRAPHICS_FAMILY_ONLY=1` (COMPUTE and COPY in the graphics family); both together are one family with
  one queue (`docs/testing.md`, "Choosing the GPU"). Only the test executables apply them: the game's device
  (`d3d12_xs`, `LEONIDA_D3D12=vulkan`) ignores them and says so, so a variable left in the environment cannot put
  every queue of the game on one `VkQueue` (`run_tiny_frame_queue_test_settings`). Measured on an RTX 5080 (Linux, driver 580.65.06, Wine 11)
  and on lavapipe, under the validation layer with synchronization validation: `d3d12_queue_share_test` passes
  with no setting, with one queue per family, with one queue in all and on lavapipe (36 queues, a buffer copied
  exactly through all of them consumer first and producer first, 9 deferred CPU Signal cases, 4 threads on 9 queues:
  2,000 iterations with all 9 on one `VkQueue` of the RTX 5080, 80,000 on lavapipe; a fence released right after
  the GPU passed the queue Signal its deferred CPU Signal waited behind, 900 times per mode, the held batch running
  every time), 0 validation messages; on main
  it fails at the 27th queue on the RTX 5080 and at the 2nd on lavapipe, and the first deferred case hangs. The layer
  half of `d3d12_queue_handover_test` runs its 371 cases exact in all four (on lavapipe main ran 0: one queue).
- `SetEventOnCompletion(value, hEvent)`: a small wait thread calls `vkWaitSemaphores` and `SetEvent`. Reuse one
  thread with a condition variable; do not create a thread per fence.
- `GetCompletedValue` is `vkGetSemaphoreCounterValue`; CPU `Signal` is `vkSignalSemaphore`.
- `SetEventOnMultipleFenceCompletion` (implemented). With a null event it is one blocking `vkWaitSemaphores` over
  every fence's semaphore, with `VK_SEMAPHORE_WAIT_ANY_BIT` for `D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY` and no flag for
  `ALL`. With an event, the device's fence wait thread counts down one shared waiter (1 for `ANY`, the fence
  count for `ALL`, kept as the API's unsigned `UINT`) and the hit that reaches zero sets the event, so no thread is
  added; later hits stay at zero (`d3d12_fence_waiter_test` checks counts past `INT_MAX`). Values already reached are
  counted before the call returns. Zero fences, other flags, a null array or a fence from elsewhere are
  `E_INVALIDARG`, as on the PC runtime. A fence released while an event waits on it counts as reached for that
  wait, as on the PC runtime (RTX 5070 and WARP): `SetEventOnCompletion` and `ANY` set the event, `ALL` counts it
  as one fence (`d3d12_fence_events_test`).
- The swapchain owns acquire/present semaphores and N frames in flight. A present waits for the frame's
  render-finished semaphore; the next acquire waits for the image's present-complete semaphore.
- Queue types: a direct queue maps to a graphics+compute queue, a compute queue to a compute-only family if one
  exists, a copy queue to a transfer-only family. If the host exposes only one family, serialize and log; async
  compute is a performance feature, not a correctness one.
- `GetTimestampFrequency`/`GetClockCalibration` use `VK_EXT_calibrated_timestamps` where available.
  `GetClockCalibration` (implemented) returns the GPU stamp in device ticks (`1e9 / timestampPeriod` per second,
  the units of `GetTimestampFrequency`) and the CPU stamp in `QueryPerformanceCounter` units. When the device has
  `VK_KHR_calibrated_timestamps` or `VK_EXT_calibrated_timestamps` enabled (`VulkanDeviceInfo::calibrated_timestamps`;
  the standalone device enables whichever the GPU has) and offers the `DEVICE` and `QUERY_PERFORMANCE_COUNTER`
  domains, it is one `vkGetCalibratedTimestampsKHR` call. Otherwise the queue waits until idle, submits a
  `vkCmdWriteTimestamp` and waits for it, and answers with the midpoint of the `QueryPerformanceCounter` reads
  around that submission. This stalls the queue, which is acceptable for a call made once per profiling capture. A
  queue family with `timestampValidBits = 0` fails with `E_FAIL`. `d3d12_fence_timing_test` takes two calibrations
  50 ms apart and compares the GPU and CPU intervals. On an RTX 4090 (driver 621.14) the gap is 2 us with calibrated
  timestamps, 20 us with the fallback, and 0.4 ms with the fallback under the validation layer. The test fails
  above 2 ms.

### 4.5 Swapchain and present

The game package does not include `dxgi.dll`, so the executable imports it from the system (or from the GDK).
That import must be shimmed. The shim provides:

- `CreateDXGIFactory`/`CreateDXGIFactory2` → one fake factory with one adapter describing the host Vulkan device.
  `IDXGIAdapter::GetDesc` reports host memory/LUID; the game may use these for settings. **Open:** whether it
  expects Xbox Series X values (16 GB, fixed clocks) instead.
- `IDXGIFactory::CreateSwapChain`/`CreateSwapChainForHwnd` → `VkSurfaceKHR` for the HWND + `VkSwapchainKHR`.
  `GetBuffer(i)` wraps each swapchain image as an `ID3D12Resource` so the game can create RTVs.
- `Present` → `vkQueuePresentKHR`; `ResizeBuffers`/`ResizeTarget` recreate the swapchain; `SetFullscreenState`
  toggles borderless/fullscreen; `GetFrameStatistics` is a stub.
- `DXGI_PRESENT_TEST`/`DO_NOT_WAIT` map to a no-op/`VK_NOT_READY` path.

**What exists.** The DXGI shim's `Present` (`runtime/shims/dxgi_com.cpp`) has its own surface, swapchain and
blit (`leonida::VulkanSwapchain`, `runtime/shims/dxgi_swapchain.h`); `PresentX` (`runtime/shims/xbox_present.cpp`)
is implemented over `leonida::VulkanPresenter` (section 3.2) instead. `PresentX` takes ordinary committed
textures and blits them at present time, which frees the game's buffer count and format from the surface's. The
DXGI shim's `GetBuffer(i)` returns the swapchain images themselves, and switches to offscreen images blitted at
present only when the requested size or format differs from the surface's (`runtime/shims/dxgi_swapchain.h`); its
buffer count is the swapchain's image count.

**PresentX and frame events (GDKX).** Public Microsoft samples present on Xbox without a DXGI swapchain. Facts
below come from MIT/zlib-licensed code and docs, read directly; none is from the game or the SDK. Sources:
[SimpleDeviceAndSwapChain](https://github.com/microsoft/Xbox-GDK-Samples/tree/main/Samples/IntroGraphics/SimpleDeviceAndSwapChain)
(readme: "Instead of using DXGI for presentation, this uses the new PresentX API"),
[FramePacing](https://github.com/microsoft/Xbox-GDK-Samples/tree/main/Samples/Graphics/FramePacing),
[DirectXTK12 wiki, DeviceResources](https://github.com/Microsoft/DirectXTK12/wiki/DeviceResources) ("The GDKX
platform uses PresentX, so the method WaitForOrigin has to be called each frame loop"),
[directxtk12modelviewer `DeviceResourcesGXDK.cpp`](https://github.com/walbourn/directxtk12modelviewer) (MIT) and
SDL2 `src/render/direct3d12/SDL_render_d3d12_xbox.cpp` (zlib).

*Fact (call sequence in those samples):*

1. `D3D12XboxCreateDevice(nullptr, &params, iid, &device)`: adapter pointer null, a params struct (ring and scratch
   sizes, `D3D12XBOX_DEFAULT_SIZE_BYTES` = use default), IID, out pointer. SDL asks for `ID3D12Device1`.
2. Back buffers are ordinary `CreateCommittedResource` calls with `D3D12_HEAP_FLAG_ALLOW_DISPLAY`, initial state
   `D3D12_RESOURCE_STATE_PRESENT`, `ALLOW_RENDER_TARGET`, 2D, one mip (SDL: `B8G8R8A8_UNORM`). They are the
   game's own resources, not objects owned by a swapchain.
3. DXGI is used only to find the output: QI the device for `IDXGIDevice1`, `GetAdapter`, `EnumOutputs(0)`.
4. `device->SetFrameIntervalX(output, D3D12XBOX_FRAME_INTERVAL_60_HZ, n-1, D3D12XBOX_FRAME_INTERVAL_FLAG_NONE)`:
   4 arguments; the third is the allowed frame latency (SimpleDeviceAndSwapChain passes buffer count minus 1,
   SDL passes 1). Then `device->ScheduleFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, 0, nullptr, ..._FLAG_NONE)`:
   4 arguments (event, a `UINT` passed as 0, a pointer passed as null, flags).
5. Per frame: `device->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, INFINITE, nullptr, ..._FLAG_NONE, &token)`:
   5 arguments, the last an out pointer to a `D3D12XBOX_FRAME_PIPELINE_TOKEN` (SDL stores it in a `UINT64`; the
   null value is `D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL`). Then render, `ExecuteCommandLists`, then
   `queue->PresentX(planeCount, planes, params)`: 3 arguments (`UINT`, pointer to an array of
   `D3D12XBOX_PRESENT_PLANE_PARAMETERS`, optional pointer to `D3D12XBOX_PRESENT_PARAMETERS`; null in the simple
   samples).
6. Plane fields used: `Token` (the value from the wait), `ResourceCount` (1), `ppResources` (pointer to the
   resource pointer(s)), `pSrcViewRects` (pointer to a rect, HDR case only) and `ColorSpace` (a
   `DXGI_COLOR_SPACE_TYPE`). The HDR path uses 2 planes sharing the token: plane 0 `RGB_FULL_G2084_NONE_P2020`
   (HDR10), plane 1 `RGB_FULL_G22_NONE_P709` (the GameDVR/SDR copy, its own back-buffer set). Non-HDR uses 1
   plane. This agrees with the UMD limit of 2 planes in `d3d12_xs_surface.md` §7.
7. `D3D12XBOX_PRESENT_PARAMETERS` has at least `ImmediateThresholdPercent` (float). Per the FramePacing text: a
   late frame is shown immediately only if scanout has not passed that percentage of the screen; 0 disables
   limited tearing, 100 allows a late flip anywhere in the scan.
8. Suspend: `queue->SuspendX(0)` (one argument, passed 0; no other thread may be rendering). Resume:
   `queue->ResumeX()`, then **re-run** steps 4's `SetFrameIntervalX`/`ScheduleFrameEventX`. The samples
   say frame events must be registered again after resume.
9. Shutdown: `PresentX(0, nullptr, nullptr)` presents a blank screen before resources are released.
10. Statistics: `device->GetFrameStatisticsX(token, typeMask, &count, array)`: 4 arguments. Type bits seen:
    `FRAME_INTERVAL`, `FRAME_EVENT`, `RENDER`, `PRESENT`, `DISPLAY`; `count` is in/out; `S_FALSE` or a short
    count means "not available yet"; no statistics for the first presented frame; render/present/display
    entries carry a `PlaneIndex`; display entries carry `PercentScanned`.
11. Xbox titles do not handle `DXGI_ERROR_DEVICE_REMOVED`/`RESET`.

*Not measured:* vtable slots of all `*X` methods, exact `D3D12XBOX_*` struct layouts and sizes, enum values, and
whether the token is exactly 64 bits in the ABI. The samples give argument counts and field names only.

*Hypothesis (the game is not observed):* a GDKX title follows this pattern, so the DXGI swapchain shim (D3D-2)
is probably **not** on its boot path. What is on it: `D3D12XboxCreateDevice`, `IDXGIDevice1`/`IDXGIAdapter`/
`IDXGIOutput` enumeration, committed `ALLOW_DISPLAY` resources, the frame events and `PresentX`. Without the
frame events and `PresentX` no frame reaches the screen. To confirm: trace which of `CreateSwapChain*` or the
queue's `PresentX` slot the game calls first; check the executable's `dxgi`/`d3d12_xs` imports.

Layer plan:

- Keep the DXGI factory, adapter and output enumeration (adapter `GetDesc`, `EnumOutputs`); no host DXGI.
- Step 3 of the sequence is on the device proxy: it answers `IDXGIObject`, `IDXGIDevice` and `IDXGIDevice1` as one
  COM object with the device (a PC `ID3D12Device` has no `IDXGIDevice`, so nothing is forwarded), and `GetAdapter`
  and `GetParent` return the adapter of the device behind it: the layer's own on the Vulkan device, the host adapter
  with the device's LUID on the host backend. The 14 slots are the public `dxgi.h` order, an assumption like the
  queue's. `SetMaximumFrameLatency` and `SetGPUThreadPriority` are kept and read back only. `dxgi_device_test`
  walks it on both backends; the output itself is `EnumOutputs` (D3D-313).
- `ALLOW_DISPLAY` committed resources are normal `Resource`s that the presenter may blit to a Vulkan
  swapchain image; `PRESENT` state maps per the resource-state table (`PRESENT_SRC_KHR` only for swapchain images).
- Presentation core: `PresentX` with 0-2 planes presents into the `VkSwapchainKHR` (one plain plane is a blit; scaling,
  placement, HDR10 and an overlay plane 1 go through a compose pass, see "Plane composition" below); `SetFrameIntervalX`/`ScheduleFrameEventX`/`WaitFrameEventX` run on a vblank
  timer and hand the token through to the present; `GetFrameStatisticsX` minimal; `SuspendX`/`ResumeX`; 0
  planes = blank screen. Test the methods directly with a fake clock, before slots are known.
- The DXGI `CreateSwapChain*` path stays as the fallback and for PC-style tests, not as the first-boot target.

**What is implemented (the presentation core).** Everything in the plan above that does not need a slot exists as
methods of `rt::d3d12::XboxPresentation` (`runtime/shims/xbox_present.h`), on top of the presenter of section 3.2 and
`leonida::FramePacer` (`runtime/src/frame_pacer.h`, no Vulkan, no Windows headers). The struct layouts, constant
values, HRESULTs and the 64-bit token are **our own provisional definitions**: the samples give call shapes and field
names only, so the real slots, layouts and enum values are still unmeasured and the class is the surface the proxy
will forward to. The queue-side calls ignore which queue they came on (one `VkQueue` behind all of them).

- *Frame pacing* (`FramePacer`, time from an injectable clock; real time is a steady timer, a host vblank source
  can replace it). `SetFrameIntervalX` takes the interval as a period in microseconds and the latency, and starts
  the vblank grid. `ScheduleFrameEventX` arms the origin event, delayed by an offset inside the frame.
  `WaitFrameEventX` returns at the next origin with the next token (1, 2, 3, ...; 0 is the null token) and does not
  return while `latency` presented frames are still waiting for their vblank. A frame presented before the origin event
  is armed is not paced (`PresentX` does not hold it for a vblank), so it is shown at once and never waits for a vblank:
  blank presents of a loading screen do not delay the first wait. At most `latency` frames are in flight once the event
  is armed: a present into a full queue (several presents with no wait between them) replaces the newest queued frame
  and takes its vblank, like a mailbox, so the replaced frame is never shown (no display statistic) and the older ones
  keep their vblanks. However many presents come between two waits, the next wait passes when the oldest frame is
  shown. A wait that comes late returns at once
  with the newest passed origin; the missed ones are skipped. A finite timeout gives
  `HRESULT_FROM_WIN32(WAIT_TIMEOUT)`; nothing registered, or suspended, gives `HRESULT_FROM_WIN32(ERROR_INVALID_STATE)`.
  `LEONIDA_PRESENT_UNCAP` (any non-empty value other than "0", read when the object is made) takes the frame-rate
  cap off: a wait hands its token out at once (no vblank grid, no latency throttle) and `PresentX` presents
  unsynced — MAILBOX or IMMEDIATE, not FIFO — so the frame rate is whatever the title renders; registration and
  suspension errors are unchanged, and the statistics of an uncapped frame report its present time. This covers
  the frame-event cap only: a cap inside the game's own code would still apply, and the DXGI swapchain fallback
  keeps the app's sync interval.
  `GetFrameStatisticsX` keeps the last 64 frames and reports the event, present and display times (not render;
  `PercentScanned` only for a frame shown at once by `ImmediateThresholdPercent`, else 0); `S_FALSE` with count 0 for what it does not have.
  *Pacing drift reporting (BL-6):* `FramePacer` keeps fixed-capacity ring buffers (`frame_pacing_window = 1024`) of
  present-to-present frame intervals and origin-to-present latencies (clearing the last-present anchor across
  `SuspendX`/`ResumeX` so suspended pauses are excluded), computing nearest-rank `p50`/`p95`/`p99` and the mean drift
  against the active target interval (`FramePacer::pacing_summary()`). On teardown after at least one present,
  `XboxPresentation` logs a single `[rt] present: pacing summary ...` line on stderr, which `tools/boot_trace.py`
  surfaces beside the D3D12 call counts. The intervals are between `PresentX` calls, not between frames shown: a frame
  replaced in a full queue (see the latency limit above) still counts as a present with its real interval, so the
  percentiles describe the title's present cadence, and a burst of presents between two waits shows up as short
  intervals. Confirmed on a fake clock by `frame_pacer_test` (known 100-frame distribution,
  positive and negative drift, ring-buffer wrap-around, suspend/resume exclusion, uncapped mode, replaced frames) and by
  `tests/test_boot_trace.py`.
- *`PresentX`*: 0 planes presents a black frame (no token needed). 1 plane blits plane 0's first resource (a
  single-sample 2D colour texture in `PRESENT`) to the swapchain image, limited to `pSrcViewRects[0]` when set
  (bilinear, so the rectangle's outer texels blend with their neighbours). The blit converts between formats; an sRGB
  source gets an sRGB swapchain format and every other source a UNORM one, so the encoded bytes go through unchanged.
  A source the GPU cannot blit gives `DXGI_ERROR_UNSUPPORTED` and presents nothing: the checks are
  `vkGetPhysicalDeviceFormatProperties` (`BLIT_SRC` on the source, `BLIT_DST` on the swapchain format, cached per
  format; the scaling filter is linear only with `SAMPLED_IMAGE_FILTER_LINEAR`, else nearest), plus integer formats,
  which a UNORM swapchain cannot take. Multisampled planes are refused the same way: resolving one into a temporary
  image is not done. 2 planes must share one token (see "Plane composition" for what happens to plane 1). More than 2
  planes, null planes, or an empty/outside rectangle give `E_INVALIDARG`. A missing `ALLOW_DISPLAY` heap flag is logged
  once and presented anyway.
- *Plane composition* (M7). *Fact* (public MIT sample, Xbox-GDK-Samples `Samples/Graphics/HDR10/DeviceResources.cpp` and
  its readme): the two-plane form there is **not** an overlay. Plane 0 is the HDR10 back buffer
  (`RGB_FULL_G2084_NONE_P2020`), plane 1 is a second back-buffer set holding the SDR rendering of the same frame
  (`RGB_FULL_G22_NONE_P709`), both with the same full-output `pSrcViewRects`; the readme says the SDR signal "is still
  required for GameDVR and screenshots" and the SimpleHDR readme says the system can generate it itself
  (`D3D12XBOX_RESOURCE_FLAG_ALLOW_AUTOMATIC_GAMEDVR_TONE_MAP`). *Fact* (field names seen in a public Go binding of the
  header, hajimehoshi/ebiten, Apache-2.0; no layout or values): `D3D12XBOX_PRESENT_PLANE_PARAMETERS` is `Token`,
  `ResourceCount`, `ppResources`, `pSrcViewRects`, `pDestPlacementBase` (a `D3D12XBOX_VIEW_RECT*`, four floats
  left/top/right/bottom), `ColorSpace`, `ScaleFilter`, `ExtendedDescCount`, `pExtendedDescs`, `Flags`;
  `D3D12XBOX_PRESENT_PARAMETERS` is `ImmediateThresholdPercent`, `ViewCount`, `ExtendedDescCount`, `pExtendedDescs`,
  `Flags`. *Fact* (GDK reference text for `XGameStreamingSetResolution`, read through a third-party mirror of the docs, not
  on Microsoft's site): `pDestPlacementBase` scale factors of 0.0-1.0 refer to the output size (the streaming resolution,
  else 1920x1080 or 3840x2160), and `pSrcViewRects` with `pDestPlacementBase` is how a plane is scaled. Not found in public
  material (so **hypotheses**, all labelled ours in `xbox_present.h`): the values of `ScaleFilter` and `Flags`, whether
  plane 1 can be a blended overlay at all on the console (the plane limit of 2 and the field set suggest a display-plane
  model, but no public sample uses it that way), premultiplied or straight alpha, what is shown outside a plane's
  placement, the blending space, the SDR white level, and every colour space but the two of the HDR sample.

  What the layer does. A plain single plane (SDR colour space, no placement, default filter) is the old blit, unchanged
  (`composed_presents()` does not move; the test compares the bytes of a blit with the compose pass at 1:1 and they are
  equal). Anything else goes through `VulkanPresenter::present_planes`: a compute pass (`runtime/src/present_compose.comp`,
  SPIR-V embedded as `depth24_convert` is, built with `glslc --target-env=vulkan1.1 -O -mfmt=c`) writes an RGBA16F image of
  the swapchain's size, and a `vkCmdBlitImage` (same size, nearest) puts it in the swapchain image, so the layer writes
  the swapchain only by transfer (no storage usage) and the capture path is unchanged. The swapchain also asks for
  `COLOR_ATTACHMENT`, which the layer never uses: overlays that hook the swapchain (Bandicam's
  `VK_LAYER_bandicam_helper`, measured) draw into its images through a framebuffer, and without it every frame raised
  `VUID-VkFramebufferCreateInfo-pAttachments-00877` and `VUID-VkImageViewCreateInfo-image-04441`. Surfaces always
  report it, and the DXGI swapchain (`dxgi_swapchain.h`) already requested it. The planes are read with `texelFetch` (no sampler, so any
  sampled float/UNORM/sRGB format works and filtering never reads outside the source rectangle); an sRGB-format source is
  decoded by the texture unit and encoded again, so its bytes are read as stored. Plane 0: `pSrcViewRects[0]` (else the
  whole texture) scaled to `pDestPlacementBase` (fractions of the output; null is the whole output; outside it is black),
  `ScaleFilter` point (the texel containing the sample) or linear/default (bilinear, clamped inside the source rectangle).
  Plane 1 is blended over plane 0 (in linear light after conversion, see "Linear-light filtering and blending"; alpha premultiplied by default or
  straight with `xbox_plane_flag_straight_alpha`) **only** when it carries our `xbox_plane_flag_overlay`; without it plane 1
  is not composed (logged once), as before, because in the public sample it is the GameDVR copy of the frame and blending
  a plane of unknown alpha would draw the frame twice (hypothesis: whether the console blends a second plane at all is
  unmeasured). The extra fields are read defensively, since their types and values are unmeasured: a placement that is not
  a sub-rectangle of 0..1 (empty, inverted, outside, NaN), an unknown `ScaleFilter` and `ExtendedDescCount` are ignored and
  logged once, and the plane is shown as if they were absent; a single SDR plane with null/zero extras stays on the plain
  blit. Planes the pass cannot read (multisampled, depth, integer, block-compressed) give `DXGI_ERROR_UNSUPPORTED`. The struct
  in `xbox_present.h` follows the field order of the binding cited above exactly (`Token`, `ResourceCount`, `ppResources`,
  `pSrcViewRects`, `pDestPlacementBase`, `ColorSpace`, `ScaleFilter`, `ExtendedDescCount`, `pExtendedDescs`, `Flags`); the
  types and sizes are ours. The compose target follows the swapchain when it is recreated (resize, out-of-date acquire);
  `xbox_present_test` resizes the window between composed presents.

  Colour. Only `RGB_FULL_G22_NONE_P709` (0, SDR, also the default for a zeroed struct) and `RGB_FULL_G2084_NONE_P2020` (12,
  HDR10) are composed; any other value is composed as SDR and logged once. The swapchain is UNORM. If a plane is HDR10 and
  the surface lists an ST.2084 format (`VK_COLOR_SPACE_HDR10_ST2084_EXT`, `A2B10G10R10` or `A2R10G10B10`; needs
  `VK_EXT_swapchain_colorspace` enabled on the instance, the test device does) the swapchain is created in it and an HDR10
  plane goes through the working space unchanged; else the swapchain is SDR (the logged fallback). The real runtime's
  instance (`create_vulkan_device_standalone`) enables `VK_EXT_swapchain_colorspace` when the loader has it (it did not
  before; only the test device did), so the HDR10 surface formats can be listed there; `xbox_present_test` checks this on
  the standalone path. Limited-range and YCbCr colour spaces are not modelled.

  *Linear-light filtering and blending* (decision from the sources below; the console's own blending space is unmeasured).
  Every texel of a plane is decoded to linear light before it is weighted, the filtered plane is converted to the output's
  working space, plane 1 is blended over plane 0 there, and the sum is encoded once:

  1. Decode. SDR planes: the SDR EOTF (below) gives linear Rec.709 with 1.0 = SDR white. HDR10 planes: PQ inverse to linear
     nits (Rec.2020). Premultiplied data is divided by alpha for the decode and multiplied again after, so what is filtered
     and blended is linear premultiplied colour; straight-alpha colour is premultiplied after the decode, so a transparent
     texel adds no colour of its own (found in review by @Flyander).
  2. Convert, still linear. HDR10 output: SDR to Rec.2020 (the Rec.709 to Rec.2020 matrix from the primaries; rows sum to 1)
     times the SDR white level in nits. SDR output: HDR10 to Rec.709 (the inverse matrix), divided by the SDR white level, then
     tone mapped (below). A plane already in the output's space is not converted.
  3. Blend `over + (1 - alpha) * base` (premultiplied; straight alpha was premultiplied in step 1); black outside plane 0.
  4. Encode once: PQ of the nits for an HDR10 output (argument clamped to 0..10000 nits), the inverse SDR EOTF (clipped to
     0..1) for an SDR one.

  Why. Microsoft's Advanced Color documentation says DWM composes in linear scRGB (FP16, "excellent precision for linear
  gamma blend operations"), that an app which puts SDR and HDR content into one surface must apply the SDR boost itself, in
  linear space (multiply by SdrWhiteLevel / 80), and, for a non-linear space such as HDR10, to "consider converting into
  linear gamma to apply the adjustment"; it describes no blending in PQ, and a weighted sum of PQ values is not a sum of
  light. (Some compositors blend in a gamma space to avoid banding; KDE's KWin is reported to do so since Plasma 6.2,
  https://planet.kde.org/xavers-blog-2024-11-01-hdr-and-color-management-in-kwin-part-4-nonlinear-blending, **unverified**: only
  a search snippet of that post was seen. Our intermediate image is RGBA16F, so banding is not what limits us here.) Each plane costs a decode and an encode per texel; a single SDR plane
  still comes out byte for byte (`xbox_present_test`: within the 8-bit rounding of the RGBA16F intermediate, and equal to the
  plain blit at 1:1). What changes against the first version of this section: bilinear filtering and alpha now weight light,
  not encoded values, so a 50% white overlay on black is byte 188 (piecewise sRGB) or 186 (gamma 2.2), not 128, and a
  bilinear edge between a dark and a bright texel is brighter than the same edge filtered in the encoded values. Whether
  the title's art expects the encoded-space result (UI is often authored that way on SDR consoles; inference) is unmeasured
  and is to be settled by a comparison with the title; the blend space is the one place that would then get a switch.

  *SDR EOTF* (selectable). Default: the piecewise sRGB curve (IEC 61966-2-1: `e / 12.92` up to 0.04045, else
  `((e + 0.055) / 1.055)^2.4`). Option: a pure power of 2.2 (`XboxPresentation::set_sdr_eotf`, or the environment variable
  `LEONIDA_SDR_EOTF=gamma22` (or `2.2`, any case; `srgb` or unset is the default, other values are logged once and ignored) read when the object is made; `PresentPlanes::sdr_eotf` in the presenter). The inverse of the
  same curve encodes an SDR output, so SDR in an SDR output stays the identity. Why piecewise as the default:
  **community-sourced**, not a Microsoft statement. KDE developer Xaver Hugl reports that Windows decodes SDR content with
  the piecewise sRGB curve when HDR is on (raised darks, "washed out") while SDR displays use gamma 2.2
  (https://planet.kde.org/xavers-blog-2025-03-31-on-brightness-and-calibrating-your-displays/), and the ledoge
  win11hdr-srgb-to-gamma2.2-icm project exists to make Windows use 2.2 instead
  (https://github.com/ledoge/win11hdr-srgb-to-gamma2.2-icm). What a PC player compares against is Windows, so that is the
  default; art mastered on a gamma 2.2 display looks closer to its intent with the option. The Xbox's own SDR-in-HDR decode
  is unmeasured. An sRGB-format source is encoded again with the sRGB curve first (the texture unit had decoded it), so its
  stored bytes are decoded by the chosen EOTF exactly as UNORM bytes would be.

  *SDR white level* (the nits that SDR white 1.0 maps to in an HDR10 output, and what HDR10 is divided by for an SDR one).
  `XboxPresentation::set_sdr_white_nits(n)` with n > 0 overrides everything (n = 0 goes back to automatic). Otherwise, while
  the swapchain is HDR10, Windows' SDR content brightness for the output the window is on:
  `DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL)` on the active display path whose GDI source name
  is the window's monitor, `nits = SDRWhiteLevel / 1000 * 80` (1000 is 80 nits, the scRGB 1.0;
  https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_sdr_white_level), read at most once a
  second **by a background thread** (the query is not run on the present path, see below); `PresentX` reads the cached value, and a failed
  query keeps the last good one. Otherwise (an SDR swapchain, or nothing read yet) 203 nits, the HDR reference white of ITU-R BT.2408 (graphics white, 58% PQ;
  https://www.itu.int/dms_pub/itu-r/opb/rep/R-REP-BT.2408-9-2026-TOC-HTM-E.htm; the W3C CSS Color HDR module uses 203 too,
  https://www.w3.org/TR/css-color-hdr-1; the report's text itself was not parsed here). Windows' setting is a PC desktop's;
  the console's is unknown, which is why the override exists. Why a thread: the query is a `QueryDisplayConfig` (with the documented retry on
  `ERROR_INSUFFICIENT_BUFFER`) plus two `DisplayConfigGetDeviceInfo` calls; measured by `xbox_present_test` on the author's
  machine (12 us mean, 70 us worst over 50 calls on the AMD run; 47 us mean, 361 us worst on the NVIDIA run; this laptop's display setup, HDR off). That is
  small here, but the call goes into the display driver stack and its worst case with several displays or during a mode change
  is not bounded by anything measured (inference), and a stall under the present lock would feed `ImmediateThresholdPercent`
  lateness; so it is kept off the present path. What is
  measured about the value: the query returns 80.0 nits on the SDR desktop of both test GPUs' output, which is only the path
  where the layer ignores it (it uses 203 nits unless HDR10 is active); the HDR value itself has not been read on real HDR
  hardware here. 1000, 2500 and 6000 convert to 80, 200 and 480 (arithmetic only).

  *HDR10 content on an SDR output*: tone mapped, no longer clipped. After step 2, the luminance Y of the Rec.709 value
  (0.2126 R + 0.7152 G + 0.0722 B, 1.0 = SDR white) goes through a shoulder: identity up to a knee at 0.8 of SDR white
  (162 nits at 203), and above it extended Reinhard (Reinhard, Stark, Shirley, Ferwerda, "Photographic Tone Reproduction for
  Digital Images", SIGGRAPH 2002, eq. 4) applied to the excess: with `t = 1 - knee`, `u = (Y - knee) / t`,
  `w = (Lw - knee) / t`, `Yd = knee + t u (1 + u / w^2) / (1 + u)`, where `Lw = peak / SDR white` and `peak` is the content's
  peak (default 1000 nits, `set_hdr_peak_nits`; no metadata is read, so this is an assumption). The colour is scaled by
  Yd / Y. This is continuous in value and slope at the knee (slope 1 there), monotonic, and maps `Y = Lw` to exactly 1.0;
  colours with Y below the knee (SDR-range content, up to 162 nits) are not touched. Above the knee it compresses: 203 nits
  (SDR white) comes out at 0.90 of white at peak 1000 (byte 243), 300 nits at 250. Per-channel values above 1 after the
  scaling (saturated highlights) are still clipped; there is no gamut mapping and no dither. Microsoft's Advanced Color
  document lists tone mapping choices, among them a scaled Reinhard (`x / (1 + x)`) and the BT.2390 EETF, as simple options
  (https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range). The BT.2390 EETF itself is not done
  (its constants were not checked against the Recommendation here).

  *HDR metadata*: not sent. The presenter never calls `vkSetHdrMetadataEXT`. Microsoft's page for
  `IDXGISwapChain4::SetHDRMetaData` (https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_5/nf-dxgi1_5-idxgiswapchain4-sethdrmetadata,
  fetched in the research) says calling it is not recommended, that Windows does not guarantee the metadata is sent to the monitor
  and that monitors handle it inconsistently, and that an app should tone map into the range of `IDXGIOutput6::GetDesc1`
  instead. vkd3d-proton forwards `SetHDRMetaData` to `vkSetHdrMetadataEXT` only for an explicit
  `DXGI_HDR_METADATA_TYPE_HDR10` block (libs/vkd3d/swapchain.c, https://github.com/HansKristian-Work/vkd3d-proton; **partly read**:
  the first ~100k of ~175k characters of the file, which include the HDR10 conversion and the setter), so a title that sets none gets none there either.
  We invent no mastering values (Vulkan treats unknown `VkHdrMetadataEXT` fields as 0). Forwarding a title's explicit HDR10
  block is possible once a `SetHDRMetaData` slot exists.

  Sources for the PQ chain: the constants are the exact rationals of SMPTE ST 2084 / ITU-R BT.2100 (m1 = 2610/16384,
  m2 = 2523/4096 x 128, c1 = 3424/4096, c2 = 2413/128, c3 = 2392/128; https://en.wikipedia.org/wiki/Perceptual_quantizer),
  1.0 = 10,000 nits; the Rec.709 to Rec.2020 matrix is computed from the primaries (ITU-R BT.2087 specifies the same
  conversion; its PDF was not parsed). Microsoft's Advanced Color document:
  https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range.

  *Verified* by `xbox_present_test` against a CPU model written from the above (not from the shader; it decodes, converts,
  blends and encodes in double precision): point filtering between opaque SDR planes is byte exact; bilinear, alpha blending
  (premultiplied and straight, point and bilinear), the sRGB and gamma 2.2 EOTFs and the linear 50% overlay are within 1
  level of 255; the HDR10 to SDR shoulder tone map (peaks 1000 and 600 nits; the expected bytes of an eight-step ramp and of an HDR10 plane 1 over an SDR plane 0 are hand calculations, as are the 50% overlay bytes 188 and 186) and the PQ encoder (an 8-bit swapchain forced to hold
  PQ values with `force_hdr_encoding`, a test hook) are within 2 levels (GPU `pow`; the PQ codes of the source are 10-bit).
  The HDR10 swapchain itself (format choice, 10-bit
  capture, pass-through): **verified on an RTX 5080 (driver 616.56, Windows 11) with an HDR10 surface by @JavierBlanco23
  in #2069** (native ST.2084 swapchain within 1 code, GameDVR plane drop, tearing); **not verified on AMD**: the
  author's Radeon 880M and RTX 5070 Laptop (desktop without HDR enabled) list no ST.2084 surface, where the test logs
  that case as skipped and checks the 10-bit codes only where the surface has the format.
- *`ImmediateThresholdPercent`* (hypothesis from the FramePacing text, item 7 above; not measured on the console):
  a frame is due at the vblank after the origin that handed out its token. One that is presented later than that
  is late, and when the scan would not be further down the screen than the threshold percentage (late time modulo the
  interval, as a fraction of the interval) it is flipped at once, mid-scan, instead of waiting a whole interval. 0,
  negative and NaN never tear; 100 or more tears wherever the scan is; a frame on time never does. The decision is
  the pure `leonida::decide_present_sync(late_us, interval_us, threshold)` in `frame_pacer.h`. A frame behind one
  that still waits for its vblank is not torn (it would overtake it), and neither is an unpaced present. The tear
  is shown in `GetFrameStatisticsX` (the display entry's `PercentScanned` is set and its time is the present time)
  and in `XboxPresentation::immediate_presents()`. In Vulkan: with `VK_KHR_swapchain_maintenance1` (else the EXT
  original; the structures are the same and the tests' GPUs list both; the instance gets `VK_KHR_get_surface_capabilities2`
  and the matching `*_surface_maintenance1`) a vsync swapchain is created with `VkSwapchainPresentModesCreateInfoKHR`
  listing FIFO plus IMMEDIATE (FIFO_RELAXED when the surface does not list IMMEDIATE as compatible with FIFO,
  checked with `VkSurfacePresentModeCompatibilityKHR`), and a present that tears chains `VkSwapchainPresentModeInfoKHR`
  with that mode. The extension and its feature are enabled only where the GPU has them. Without them, or without
  either mode, the threshold is ignored and logged once, and a late frame waits for its vblank as before.
  `LEONIDA_PRESENT_NO_TEAR=1` turns it off. Caveats: FIFO_RELAXED tears only when the queue is empty and a vblank was
  missed (close to the semantics, but the driver decides), and the lateness is measured on the pacer's clock (a
  timer, not the display's real scan), so on a real display the tear position is whatever the driver does at that
  moment. What is checked is that the immediate mode is chosen and presented with 0 VUIDs in our runs with `LEONIDA_VK_VALIDATION=1`; no scan-out
  or tearing was observed.
- *Tokens*: what the console does with a null, repeated or out-of-order token is **unmeasured**, and a title that
  presents a loading screen with `Token = 0`, presents again with one token, or presents from a render thread out of
  order must not freeze. So those are presented anyway and logged once per kind; only a token that was never handed
  out gives `E_INVALIDARG` (nothing presented). Skipping a token is fine.
- *Frame capture* (`LEONIDA_CAPTURE=<dir>`, `LEONIDA_CAPTURE_EVERY=<n>`, `LEONIDA_CAPTURE_LIMIT=<n>`): writes
  every n-th presented frame (default 30, at most 24) to `<dir>/frame_NNNNN.ppm`, a binary P6 of the swapchain image
  converted to 8-bit RGB, and appends one line per frame to `<dir>/index.txt` with the extent and the mean RGB. Off
  unless the variable is set, and the folder is created. Disk-only sampling copies frames 1, 1+n, 1+2n, … to host
  staging until the file limit is reached; intervening frames still present normally. This avoids a full-image
  transfer on every present when only occasional samples were requested. An explicit continuous readback request
  and the temporary splash inspection still copy each frame they need. This lets a check inspect what the layer
  actually presented: the window is not readable in every setup (under Wine it is not even mapped), and the
  mean is enough to tell a real frame from a black one. `d3d12_present_test` turns it on and checks the files against
  the colour it clears to.
- *`SuspendX`/`ResumeX`*: suspend drops the interval, the origin event and the frames in flight, wakes blocked waits
  with `ERROR_INVALID_STATE` and refuses presents (a `PresentX` waiting for a swapchain image polls the suspended
  state every 50 ms and gives up, so `SuspendX` never waits on it); resume allows presents again, but waits fail until
  `SetFrameIntervalX` and `ScheduleFrameEventX` are called again, as the samples do. Tokens keep counting. Calling
  `SetFrameIntervalX` again outside suspend keeps the origin event armed and re-bases the vblank grid on that moment.
- *Not done*: the BT.2390 tone map and gamut mapping for HDR10 on an SDR swapchain (a Reinhard shoulder only), scRGB, limited-range
  and YCbCr colour spaces, `ResourceCount` above 1 (refused with `DXGI_ERROR_UNSUPPORTED` before either path, logged once, since only resource 0 would be read; found in review by @Flyander), extended descriptors (`pExtendedDescs`) and `ViewCount`, capturing a
  second (GameDVR) output, resolving multisampled planes, a mutable-format swapchain (an sRGB/UNORM
  alternation recreates the swapchain; `VK_KHR_swapchain_mutable_format` is not enabled on the device), a host-vblank clock (the timer drifts from the
  display; with FIFO presents the swapchain holds the queue to the real refresh anyway), render statistics,
  `PercentScanned` of frames shown at a vblank (a scan position is never measured), and the vtable wiring. Slots to measure (all unknown): the device's `SetFrameIntervalX`,
  `ScheduleFrameEventX`, `WaitFrameEventX`, `GetFrameStatisticsX` and the queue's `PresentX`, `SuspendX`, `ResumeX`
  (the list is in the header).
- *Tests*: `frame_pacer_test` (fake clock: interval, offset, late waits, timeouts, latency blocking, tokens,
  statistics, suspend/resume, a real 10 ms timer; no GPU) and `xbox_present_test` (the Vulkan device, reading the
  swapchain image back: blank, solid, two colours, source rectangles, RGBA/BGRA/sRGB sources, 2 planes, plane composition
  (placement, point/bilinear, alpha overlay, HDR10 and PQ conversions, refusals; against a CPU model), token errors,
  statistics, suspend/resume, `ImmediateThresholdPercent` with a late frame at several thresholds (immediate mode
  chosen, or not for 0, below the lateness, on time) and with `LEONIDA_PRESENT_NO_TEAR=1`, 30 frames at 60 Hz in real
  time; skipped, exit 77, without a Vulkan device). `frame_pacer_test` also sweeps `decide_present_sync`, and checks
  that 1000 presents between two waits leave at most `latency` frames in flight (latency 1, 2 and 3).

### 4.6 Queries

Implemented (`QueryHeap` in `runtime/shims/d3d12_vulkan_device.cpp`, the query commands in `command_list.h` and
`vulkan_replay.h`), checked on one GPU by `d3d12_vulkan_device_test` under the validation layer:

- **Heaps.** One `VkQueryPool` per `ID3D12QueryHeap`, registered with the `VulkanContext` by its pointer, as
  resources are. `OCCLUSION` heaps are `VK_QUERY_TYPE_OCCLUSION` pools and hold both `OCCLUSION` and
  `BINARY_OCCLUSION` queries, as in D3D12. `TIMESTAMP` and `COPY_QUEUE_TIMESTAMP` heaps are
  `VK_QUERY_TYPE_TIMESTAMP` pools. `OPTIONS3.CopyQueueTimestampQueriesSupported` and copy timestamp heap creation
  agree: both require timestamp bits and graphics or compute support in every family a copy queue can use (its
  dedicated family, if configured, and the graphics fallback). Transfer-only families can write timestamps on some
  devices but cannot reset or resolve their queries. Query commands and `GetTimestampFrequency` also check the
  queue's actual family.
  `PIPELINE_STATISTICS` heaps count all eleven Vulkan statistics (`0x7ff`), whose result order is the field order
  of `D3D12_QUERY_DATA_PIPELINE_STATISTICS` (IA vertices, IA primitives, VS, GS invocations, GS primitives,
  clipper invocations and primitives, PS, HS, DS, CS), so one 88-byte result per query is copied as is. They need
  `pipelineStatisticsQuery`. `VIDEO_DECODE_STATISTICS` and `PIPELINE_STATISTICS1` (mesh and amplification counts)
  are logged and refused with `E_NOTIMPL`. Invalid descriptions are answered as the host runtime answers them
  (`compare_invalid_arguments`, "query heap:", RTX 5070 / 621.14): `E_INVALIDARG` for no queries, a node other than
  node 0, or a type past `PIPELINE_STATISTICS1` (7), checked before the output; a valid description with a null
  output is then `E_UNEXPECTED`, not `S_FALSE`.
- **Stream-output statistics** (D3D-302). The layer still refuses `StreamOutput` PSOs and does not implement
  `SOSetTargets`, so no primitive reaches a stream yet. With `VK_EXT_transform_feedback` enabled on the device
  (`D3D-TRANSFORM-FEEDBACK-DEVICE`) and its `transformFeedbackQueries` property set, an `SO_STATISTICS` heap is a
  real `VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT` pool begun and ended indexed on the stream
  (`vkCmdBeginQueryIndexedEXT` / `vkCmdEndQueryIndexedEXT`), and `ResolveQueryData` copies the two 64-bit results per
  query (16 bytes: `NumPrimitivesWritten`, `PrimitivesStorageNeeded`), in the same order; with no stream output they
  are 0. A stream at or above the device's `maxTransformFeedbackStreams` runs nothing and resolves zero, as if the
  heap had no pool. Without the feature, the property, or a nonzero `maxTransformFeedbackStreams`, the heap is made
  without a `VkQueryPool`: `BeginQuery`/`EndQuery` run nothing in Vulkan (D3D12's call rules are still kept, and the
  heap's queries track ended or not as the others do), they overlap no other query, and `ResolveQueryData` writes
  zeros (`vkCmdFillBuffer`, 16 bytes per query), with no statistics split since nothing is dispatched. Two
  overlapping `SO_STATISTICS` queries on one stream would be two active indexed queries in one pool
  (`VUID-vkCmdBeginQueryIndexedEXT-queryPool-04753`), so `prepare()` refuses such a list
  (`ReplayResult::invalid_query`); queries on different streams are fine.
- **Recording.** The list keeps D3D12's rules for the calls: timestamps only have `EndQuery`; every other query is
  begun and ended in the same list with the same type, is not begun again while it runs, and is not resolved while
  it runs; resolve offsets are multiples of 8. A list closed with a query still running reports `E_INVALIDARG` and
  is refused at execution. Heap types, index ranges and the destination buffer are checked at
  `ExecuteCommandLists`, when the heap and buffer are known (`ReplayResult::invalid_query`).
- **Replay.** D3D12 queries need no reset; a Vulkan query does. Each `BeginQuery` and each timestamp `EndQuery` is
  preceded by `vkCmdResetQueryPool` of that one query. Nothing in the layer runs inside a render pass yet, so the
  reset is legal where it stands; a draw path that opens a render pass around queries must reset them before it.
  `OCCLUSION` begins with `VK_QUERY_CONTROL_PRECISE_BIT` when `occlusionQueryPrecise` is enabled. A timestamp is
  `vkCmdWriteTimestamp` at `BOTTOM_OF_PIPE` (every earlier command finished). `ResolveQueryData` is
  `vkCmdCopyQueryPoolResults` with `VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT`. A query keeps its result
  across executions until it is begun or written again, as in D3D12.
- **Binary occlusion.** The queue resolves to a scratch buffer in chunks, reduces each 64-bit value to 0 or 1
  with a compute dispatch, and copies it into the destination. This works with readback buffers, which need
  only transfer-destination usage. The helper runs outside conditional rendering so a predicate cannot suppress
  the resolve. `vk_binary_occlusion` checks more than one scratch chunk, gaps, suballocated destinations,
  consecutive submissions and an ordinary occlusion count on a real Vulkan device.
- **Queries never ended.** D3D12 leaves their result undefined; a Vulkan copy with `WAIT_BIT` would wait for them
  forever. Each heap records on the host which queries were ended, in submission order, and a resolve writes zeros
  (`vkCmdFillBuffer`) for the ones that were not.
- **Overlapping occlusion queries and statistics (D3D-252).** On a graphics queue with compute, the replay opens
  a new physical query whenever the set of active queries changes. Ordinary and binary occlusion share the one
  physical pool (both are `VK_QUERY_TYPE_OCCLUSION`, so one physical query is open at a time, and the shared
  intersection interval is counted by both logical queries); pipeline-statistics queries split the same way into
  the statistics pool, where every non-timestamp resolve and every helper dispatch while a query runs also closes
  and reopens the open segment (D3D-162), for every query open at that point. Each logical query keeps a
  contiguous range in the submission's pool. Resolving it sums the results on the GPU, then copies them to the
  destination; a binary resolve normalizes the summed counts to 0 or 1 (`sum_binary_occlusion`, the sum followed
  by the existing normalize). Both occlusion compute helpers reuse one scratch buffer per queue. Logical results
  and in-flight submissions retain the pools independently, so later resolves and overwrites can be recorded
  before the earlier commands complete. `vk_overlapping_occlusion` checks crossing and nested scopes, more than
  one scratch chunk, alternating binary and ordinary resolves under a predicate, gated submissions, crossing
  statistics queries against three unsplit references word by word, and an ordinary query around a binary one, on
  a GPU; `replay_query_predication` pins the segment layouts, the helper dispatches inside an overlap and the
  binary sum-and-normalize without a GPU. On a device without `pipelineStatisticsQuery` (MoltenVK) the test
  leaves the statistics lists out; `vk_overlapping_occlusion_no_statistics` runs it that way (`--no-statistics`)
  on a device that has the feature, so that path is also run where no such device is at hand.
- **Helper dispatches during a pipeline-statistics query** (D3D-162). The helper dispatches above (binary
  normalisation, occlusion sums), statistics sums, `SetPredication` captures, predicated buffer copies and D24/RGB96 footprint
  conversions would count in the application's `CSInvocations` if they ran inside its query. Such a query is split.
  Its counters go to physical `PIPELINE_STATISTICS` segments in a pool of the submission's own; a segment ends
  before each helper and a new one begins after it. Non-query helpers and resolves share the segment numbering.
  Its result is the sum of its segments, added per counter by a compute dispatch (`statistics_sum.comp`) on the
  same scratch buffer, which is itself kept out of any statistics query still running.
  Whether a resolve dispatches is only known once the list is planned against the heaps, so every such resolve
  splits the query. `vk_binary_occlusion` checks that a binary resolve and a statistics sum inside statistics queries
  leave every counter at 0. `vk_overlapping_occlusion` puts a native draw in each segment and checks the split sums
  against unsplit queries with the same draws, word by word. `vk_predicated_copy` checks captures, rebindings and
  predicated copies; `depth24_convert` checks both conversion directions for D24 and RGB96. Their byte-exact
  results still match and all 11 statistics counters are zero. Without a statistics pool (single-list `replay()` or
  a queue without the summation helper), these three commands are refused before helper scratch allocation or
  recording. The resolve fallback is unchanged: only resolves needing dispatches are refused there.
- **Mesh draws during a pipeline-statistics query** (D3D-291). D3D12 allows `DispatchMesh` inside a statistics query
  and reads its input-assembly, vertex, geometry and tessellation fields as 0. Vulkan forbids `vkCmdDrawMeshTasksEXT`
  while a query with the input-assembly, vertex, geometry, tessellation or clipping counters is active
  (`VUID-vkCmdDrawMeshTasksEXT-pipelineStatistics-07076`), and the heap's pool has all eleven. So a statistics query
  that holds a mesh draw is split as above, and a split before a draw stays outside its rendering scope. A segment's first draw decides its pool: the submission's full pool, or a second pool
  with only the fragment and compute invocations, the two the VUID leaves allowed. A later draw of the other kind ends
  the segment and begins one of its own kind; dispatches and segments without draws use the full pool, and
  `ExecuteIndirect` draws count as vertex draws. The sum copies a mesh segment's two counters into fields 7 and 8 of
  its record, and `statistics_sum.comp` adds them as `PSInvocations` and `CSInvocations`, so the other fields get
  nothing from it. D3D12 counts the clipping invocations and primitives of a mesh pipeline, but Vulkan has no counter
  for them during a mesh draw, so `CInvocations` and `CPrimitives` read 0 for a mesh segment. A query that holds only
  mesh draws is one mesh segment, resolved through the same sum. Without the compute helper there is no segment pool
  and such a list is refused (`unsupported`) rather than run unsplit; so is one on the single-list `replay()` (D3D-399),
  whose `create_segment_pool` gives no mesh pool. Overlapping statistics queries (D3D-252) split the same way inside
  their shared segments: a draw of the other kind than the open segment's opens a new one, so every query that segment
  belongs to sums both kinds. `replay_query_predication_test` (cases 15 to 17) checks the split points, the segment
  kinds, the resolve plan and the counters the mesh pool may enable on the CPU. `d3d12_mesh_test` resolves a query
  around a vertex, two mesh and a vertex draw, one around a mesh draw and one around the two vertex draws: the vertex
  fields count only the two triangles, the mixed query has the same clipping count as the vertex-only one and more
  pixel-shader invocations, and the mesh-only query has no clipping count without the mesh-primitives query below.
- **The clipping invocations of a mesh segment** (D3D-MESH-CLIPPING-COUNTER). D3D-291 leaves `CInvocations` and
  `CPrimitives` of a mesh segment at 0, because Vulkan has no clipping counter during a mesh draw. Where the device
  has the `meshShaderQueries` feature of `VK_EXT_mesh_shader`, the number of primitives the mesh shader emitted
  comes from a `VK_QUERY_TYPE_MESH_PRIMITIVES_GENERATED_EXT` query instead: a third pool of the submission, begun
  and ended with each mesh segment, next to the two the statistics split needs. Its count is copied to field 5 of the
  segment's record and `statistics_sum.comp` adds it as `CInvocations`; `CPrimitives` stays 0, because Vulkan has
  no clipped-primitive counter and D3D12's `CPrimitives` is the clipper's output (0 when a triangle is dropped, 2
  when it is split). The count is not D3D12's number: Vulkan counts a primitive the mesh shader culled with
  `gl_CullPrimitiveEXT`, which D3D12 does not count, so a mesh segment's `CInvocations` can be higher than D3D12's.
  Without the feature the field reads 0, as in D3D-291.
- **Timestamps.** `GetTimestampFrequency` is `1e9 / timestampPeriod` (1 GHz on the test GPU). Every D3D12 queue
  shares one `VkQueue` for now (question 12), so they share one frequency.
- **Timestamp resolves on MoltenVK.** On Apple GPUs, MoltenVK delays every `vkCmdWriteTimestamp` to the end of
  the current Metal encoder, where it is sampled by a blit pass of its own (`encodeTimestampStageCounterSamples`,
  because Apple GPUs sample only at stage boundaries). `vkCmdCopyQueryPoolResults` resolves the counters with a
  blit that nothing orders after that pass. The copy then reads the previous value, or 0 for a timestamp never
  written before. This happens when the copy is in the timestamp's open blit encoder, in a later encoder, in a later
  command buffer or in the next `vkQueueSubmit`. The spec needs no barrier here: `WAIT_BIT` waits for the query.
  `vkGetQueryPoolResults` on the host is right. A minimal Vulkan program (two timestamps around a
  `vkCmdCopyBuffer`, resolved with `64_BIT | WAIT_BIT`, compared with `vkGetQueryPoolResults`, 200 runs per case
  on an M3 Max, macOS 27.3) got the right values 0 to 13 times out of 200 for each of these placements with
  MoltenVK 1.4.3 (`main` 5189f8de). With a pipeline barrier or a fill before the copy it was 9 and 3 out of 200.
  MoltenVK 1.4.2 got 0 to 43 out of 200, measured while other GPU work ran. Every case was right 200/200 on
  KosmicKrisp (Mesa 26.3.1) and llvmpipe. Two things order the copy on MoltenVK, right in every run (200 or 500
  runs, both versions). One is `vkCmdSetEvent` + `vkCmdWaitEvents` on a fresh event before the copy, which MoltenVK
  encodes as a Metal event signal and wait on the command buffer. The other, across submissions, is a timeline
  semaphore wait for the earlier submission. So where `driverID` is `VK_DRIVER_ID_MOLTENVK` (Wine reports the
  host's driver), the queue sets and waits for an event before each timestamp resolve that copies, and a
  submission with such a resolve waits on the queue's own timeline for the submission before it. The values are
  still the GPU's own timestamps, nothing is emulated. MoltenVK picks at submit time, from the event's host state,
  whether a set or wait does anything. So each resolve gets an event of its own, which goes back to the queue's pool
  after a host `vkResetEvent` once the submission completes. `d3d12_vulkan_device` checks the same list (timestamp,
  clear, copy, timestamp, resolve) and a resolve in the next execution with no wait between them. Under Wine
  (CrossOver 26.3, MoltenVK 1.4.3) the first case failed every run before this change (`t1` read 0).

The single-list `replay()` helper replays overlapping occlusion kinds and statistics queries too (D3D-399) when
the caller passes a `keep` vector and sets `ReplayFunctions::create_segment_pool` with the four sum helpers: it
makes one occlusion and one pipeline-statistics segment pool sized to the list, plans them as the batched path does,
and hands them to `keep`, which the caller holds until the command buffer has completed. Without them such a list is
still refused with `ReplayResult::unsupported`. `replay_query_predication_test` case 14 checks both, on case 13's
statistics list and case 10's occlusion pair (no GPU).

Not implemented yet:

- **Overlapping queries in the single-list `replay()` helper.** It has no segment pool, so a list whose occlusion
  kinds or statistics queries overlap each other is refused there with `ReplayResult::unsupported`; the batched
  path above replays them.
- Mesh-shader statistics (`PIPELINE_STATISTICS1`, D3D-132), and stream-output statistics that count real
  primitives (they wait for stream output itself). (`GetClockCalibration` is implemented: see section 4.4,
  "Fences, queues and frames in flight".)
- Stream-output and mesh-shader statistics. (`GetClockCalibration` is implemented: see section 4.4, "Fences,
  queues and frames in flight".)
- Xbox-specific query heap types (if any) are **Open**; unknown types are logged.

### 4.7 Indirect commands and command signatures

- `D3D12_COMMAND_SIGNATURE_DESC` describes the argument stride and a list of argument types. Simple draw,
  draw-indexed, dispatch and mesh-dispatch signatures map to `vkCmdDraw*Indirect`, `vkCmdDispatchIndirect` and
  `vkCmdDrawMeshTasksIndirectEXT`; use the `*Count` variants (`VK_KHR_draw_indirect_count`, core 1.2) when the
  `ExecuteIndirect` call supplies a count buffer.
- Signatures that change root constants, vertex/index buffers or descriptors per argument cannot be expressed by
  one Vulkan indirect call. Use a compute expansion pass that writes a Vulkan-shaped indirect buffer, or
  `VK_NV_device_generated_commands` on NVIDIA. Start with the simple signatures; the trace will show which
  signatures the game creates.
- The indirect arguments are clamped on the GPU before the command reads them (D3D-17's replay,
  `runtime/src/vulkan_indirect_clamp.h`, shader `runtime/src/indirect_clamp.comp`). The title's own compute writes
  them, and a pass the replay drops (an async pipeline that is not built yet, a stub pipeline,
  `LEONIDA_SKIP_COMPUTE`) or mistranslates leaves whatever the memory held: a dispatch of billions of groups or a
  draw of four billion vertices runs until the host resets the display driver. One small compute pass per
  `ExecuteIndirect` call copies the active records into device-local scratch with their values bounded - 2^24
  vertices or indices, 2^26 vertex instances (count times instances), the device's `maxComputeWorkGroupCount` groups
  per dimension for a dispatch (65 535 for a mesh dispatch), 2^20 groups per dispatch and a 2^27-unit work budget per
  call that saturates through an atomic compare-and-swap - and writes an
  empty record past `min(count, max_command_count)` and past the budget, and the clamped count word a `*Count` draw
  then reads (so the count is at most `maxDrawCount`, VUID-vkCmdDrawIndirectCount-countBuffer-02717). A draw record
  of 2^23 vertices or indices an instance or more is a slot the title has not filled: it runs empty and charges
  nothing (RT-INDIRECT-DRAW-UNFILLED-COUNT). Before, each was cut to 2^24 and charged that, so eight of them used up
  the budget and every later draw in the list ran empty; a census in a VIctory fork found 90 such records in one
  list. A dispatch record's groups per dimension follow the device (#2230): the title's tile
  passes ask for 67 440 groups in x at 2560x1440 (found in the VIctory-Belobug-AMD fork of VIctory,
  `crates/victory-gpu/src/indirect.rs`), RADV runs 2^32 - 1 in x in one dispatch, and the cut at 65 535 dropped the
  last 1 905 tiles of the pass. A device that allows less (llvmpipe: 65 535) still gets the record cut at its
  limit: running the rest would take more dispatches with a group-ID base the translated shader does not read. The
  counters' `indirect clamp dispatch:` line (every 10 s while dispatch records are clamped) gives the records, those
  wider than 65 535 in x, the widest x that ran and those cut at the device's limits, so a run shows which case it
  is. The indirect command reads that scratch. A draw emits the pass before its rendering scope opens: `vkCmdCopyBuffer` and a
  memory barrier with a transfer access are not allowed inside a dynamic-rendering scope. The records are
  bit-identical to the application's when they are within the limits, the argument and count buffers are left
  untouched, and the scratch comes from a per-submission pool (`vulkan_scratch.h`) that a list with many indirect
  calls reuses. `LEONIDA_INDIRECT_CLAMP=0` reads the application's buffer again, as before. The pass's counters
  (records checked, records clamped, counts clamped, records past the budget) are copied to one host-visible slot
  per call, read once the submission completes, logged in full at the first use and then rate-limited. Covered by
  `indirect_clamp_test` (every kind and the counted variants,
  in-range and garbage records, the budget), by `d3d12_m4_compute_test` (a garbage dispatch record beside the
  in-range one) and by `d3d12_indirect_rt_test` (a draw whose instance count is garbage).
- Predication (`SetPredication`) maps to `VK_EXT_conditional_rendering` (D3D-18). Done: `GraphicsCommandList::
  SetPredication` records a `Predication` command (`runtime/src/command_list.h`; offset 8-byte aligned, op
  `EQUAL_ZERO` or `NOT_EQUAL_ZERO`, a null buffer turns it off). The replay (`runtime/src/vulkan_replay.h`) begins
  conditional rendering on the captured condition, plain for `EQUAL_ZERO` (Vulkan discards when the word is 0) and
  `VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT` for `NOT_EQUAL_ZERO`, ends it before a new predicate (it cannot
  nest) and at the end of the list (predication does not carry over to the next list). While a predicate is set,
  whole-view clears are replayed as one rectangle with `vkCmdClearAttachments`, because `vkCmdClearColorImage` and
  `vkCmdClearDepthStencilImage` ignore conditional rendering. The standalone device enables the extension and its
  `conditionalRendering` feature when the GPU has them, and then creates buffers with
  `VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT`; without it a predicated list is rejected (`unsupported`).
  `D3D12_RESOURCE_STATE_PREDICATION` is the same bit as `INDIRECT_ARGUMENT` (and `SYNC_PREDICATION` /
  `ACCESS_PREDICATION` the same as `EXECUTE_INDIRECT` / `INDIRECT_ARGUMENT`), so with the extension enabled
  barriers into and out of that state also cover `VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT` at
  `VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT`, where the predicate is read: without it a `ResolveQueryData`
  into the predicate had no dependency on the read (`vulkan_access`, `vulkan_barrier_scope`).
  Checked by part E of `vk_command_replay_test` on an NVIDIA GeForce RTX 3070, and end to end by
  `d3d12_vulkan_device_test` (an occlusion query resolved into a predicate, through legacy and enhanced barriers)
  on an NVIDIA GeForce RTX 5070.
  The 64-bit value is captured on the GPU at each `SetPredication`: an internal eight-byte copy and a
    one-invocation compute pass OR the two dwords into a private 32-bit condition. Later writes to the source do
    not change that snapshot. A null buffer disables predication; rebinding ends the previous conditional scope
    before the capture, so the internal dispatch is never predicated. Bounds checks require all eight bytes.
    Capture arenas and descriptors remain alive until their command buffer's timeline ticket completes. No
    CPU readback or per-predicate wait is used. This helper requires a compute-capable configured queue family;
    unsupported queues fail preparation instead of falling back to the low word. The synthetic GLSL source and
    embedded SPIR-V are shipped together; normal builds do not need a shader compiler.
    Capturing a new predicate inside an active pipeline-statistics query splits the query around the capture
    dispatch (D3D-162, section 4.6), keeping its invocation out of the application's result. Without a statistics
    segment pool, preparation still refuses the capture before allocating its scratch.
    Integration notes for #291 and D3D-27: the capture helper binds its own compute pipeline, descriptor set 0
    and push constants. The compute path must restore those bindings afterward, or bind its application state
    before each dispatch. Copies and compute dispatches cannot execute inside dynamic rendering, so render-pass
    integration must place the capture outside the rendering scope while preserving capture at `SetPredication`.
    These are requirements for those future integrations, not compute-dispatch or render-pass support added here.
  `CopyBufferRegion` under a predicate instead uses a byte-granular compute dispatch inside the captured
  conditional scope; both operations, unaligned offsets and lengths, and the full 64-bit predicate are covered.
  Zero-byte copies are no-ops, as checked against native WARP. Unpredicated copies retain `vkCmdCopyBuffer`.
  The compute helper requires enabled Vulkan 1.2 `storageBuffer8BitAccess` (not `shaderInt8`) and a compute queue;
  without it, preparation rejects the predicated buffer copy rather than executing it unconditionally.
  Descriptor windows respect storage-buffer alignment/range and dispatch limits, including arena offsets.
  Copy descriptors are owned by command-buffer slots until the submission timeline completes. Memory dependencies order prior writes,
  subsequent reads and successive windows. Direct and compute queues support the helper; copy queues reject
  active predication. Preparation is serialized under the tracker lock, preserving resource -> tracker -> submit
  lock order and the copy-then-commit tracker. Readback writes accumulate COMPUTE_SHADER for the helper and COPY
  for ordinary copies; mixed lists accumulate both before main's queue-specific host-stage filtering.
  This helper also binds compute pipeline/set 0/push constants, so the
  application-state restoration requirement above applies. A predicated copy inside an active pipeline-statistics
  query splits that query around the helper dispatches (D3D-162); without a statistics segment pool it is refused
  before copy scratch allocation.
  Synthetic checks: `predicated_copy_test` covers preparation and rejection paths without a GPU;
  `d3d12_predicated_copy_test` compares 180 byte-exact cases against WARP and an independent literal oracle,
  plus 16 query-to-copy cases: real empty ordinary/binary occlusion queries resolve zero into a default-heap
  predicate at offsets 0/16, both predicate operations govern copies, and executing overwrite copies replace zero
  with a high-word-only value. Four paired controls verify that recapturing the same address changes exactly one
  13-byte destination region; without recapture, later copies keep using the captured zero. Predicate-buffer guards
  and all untouched destination bytes are checked too. These query cases do not cover nonzero visible-draw counts;
  `vk_predicated_copy_test` forces small windows and dispatches, arena prefixes, dependent copies, offset rebinding,
  reset, 32 gated plus 32 reused submissions, failed-submit cleanup and host stages on all three queue kinds.
  Binary and overlapping occlusion fixtures also verify that query results resolve under a false predicate while
  a dedicated copy destination remains unchanged; disabling predication copies into a separate region.
  The embedded shader is generated from `predicated_copy.comp`
  with glslang 16.6.0 (`-V --target-env vulkan1.2 -Os`) and validated for Vulkan 1.2.

  Open:
  - Texture copies, `CopyResource` and resolves. Vulkan's conditional rendering does not cover transfer
    commands, so these still run unconditionally and `PreparedList::unpredicated_copies` counts them.

  - Draws, dispatches and `ExecuteIndirect` are covered by conditional rendering as soon as they are replayed
    (D3D-3, D3D-4, D3D-17); `ClearUnorderedAccessView*` and `ResolveSubresource` need the same check when added.

### 4.8 Ray tracing and XDXR

This is the largest Xbox-specific area, and the shipped `XboxBvhBuilder.dll` gives concrete evidence. The DLL's
own names use the `XDXR` prefix for Xbox's raytracing path:

- Its constructor takes `ID3D12Device11*`; its build/copy/rehydrate methods take
  `ID3D12GraphicsCommandList4/6/8*`.
- Prior art measured its constructor creating 53 compute pipelines through device slot 62, called with the
  compute-PSO shape (a plain `D3D12_COMPUTE_PIPELINE_STATE_DESC`, two zero words, iid, out); on the PC interface used
  there slot 62 is `CreateStateObject` (a different arity), so the proxy overrides it. The layer must implement
  slot 62 as compute-PSO creation, not as a state object.
- It exposes standard `D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC` builds, batched Xbox builds
  (`D3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2`), XDXR stream descriptions
  (`D3D12XBOX_XDXR_STREAM_DESC`), post-build offline statistics, and a `DEHYDRATE` build flag. The Xbox types are
  not public. **Measured** from the DLL's own reads ([xdxr.md](xdxr.md), `runtime/src/xdxr_descs.h`): `BATCHED_DESC2`
  is the public build desc, the postbuild count and pointer, and a 16-byte `XDXR_STREAM_DESC` (`0x50` bytes); the
  stream desc is a size and a pointer to 16-byte typed records. The DLL rejects the offline-statistics postbuild type
  (`0x10`) for every build, so that layout is not in the package.
- The translator's `image_bvh_intersect_ray` / `image_bvh64_intersect_ray` already emulate the AMD hardware BVH
  node format: node id = address >> 3 | type, box16/box32/triangle nodes, 256-byte base units. That is the format
  the runtime must produce for the game's compute traversal.

**The builder's own shaders are console FSSM, not Xbox DXIL.** Each of the 52 containers carries an LLVM
bitcode blob and an `XDAT`/`XBCH` record; the PC experiment below compiled the bitcode, but the console runtime
executes the `XBCH` record. Measured from that record: 52/52 unpack to `FSSM`, and all 52 translate and pass
`spirv-val` with the same translator the game's 26,503 shaders use; the last one, `Box16Rehydration`, since
`buffer_store_short` landed (`8f208d09a`; [embedded_shaders.md](embedded_shaders.md)). The
raw 64-bit addresses are the corpus's existing raw-address path, not a separate lowering. Open: whether the game
uses the builder at all (RT-5) and whether the translated programs match a reference on a GPU.

The design choice follows from that: **build AMD-format BVH data, not a `VkAccelerationStructureKHR`, unless the
shaders are changed to `OpTraceRayKHR`.** The game's 64 RT compute shaders traverse the nodes themselves; a
Vulkan AS would be invisible to them. Two paths:

1. If `XboxBvhBuilder` does the build with its own compute dispatches (the likely case, given it ships as a DLL
   with a device and command lists), the layer must translate and run its compute shaders. The console programs
   are `FSSM` and all 52 pass `spirv-val`. Buffers,
   UAVs, descriptors, barriers and indirect dispatch are necessary but not sufficient. The standard
   `BuildRaytracingAccelerationStructure` calls may never reach our command list.
2. If the game calls `ID3D12GraphicsCommandList4::BuildRaytracingAccelerationStructure`, the layer must implement
   it. Produce the AMD node layout in the arena, with the same geometry/instance rules the translator assumes,
   and make `GetRaytracingAccelerationStructurePrebuildInfo` return sizes for that layout.
   The layout is the XDXR BVH4 runtime form ([bvh.md](bvh.md) §3.1). `tools/xdxr_bvh.py` encodes it on the
   CPU, with a reference traversal and a validator ([bvh.md](bvh.md) §7a). `runtime/src/xdxr_build.h` builds it on
   the GPU and is checked against that reference. The standalone device uses it for `BuildRaytracingAccelerationStructure`
   and answers `GetRaytracingAccelerationStructurePrebuildInfo` from its layout, exactly. Acceleration-structure
   buffers then keep their console VAs, the addresses the records name each other by. `LEONIDA_RT_AS=khr` keeps
   #743's `VkAccelerationStructureKHR` builds (for `RayQueryKHR`), which the game's shaders can't read. Updates refit
   the source structure instead of rebuilding it, `CopyRaytracingAccelerationStructure` clones and compacts, and the
   postbuild compacted and current sizes are written ([bvh.md](bvh.md) §7a). A bottom-level build with
   `MINIMIZE_MEMORY` gets BOX16 nodes (half boxes, about a fifth smaller; top-level builds stay BOX32), and one
   without `ALLOW_UPDATE` pairs sibling triangles that share an edge into TRI0/TRI1 leaves. Serialization and the Xbox
   #743's `VkAccelerationStructureKHR` builds (for `RayQueryKHR`), which the game's shaders can't read. A top-level
   build with `D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS` is supported there: the console VAs in the pointer list are
   translated to Vulkan device addresses in a staged copy, without changing the guest's input. The list must be in
   CPU-writable memory (an UPLOAD or write-back CUSTOM heap; WRITE_COMBINE CUSTOM is CPU-writable too); GPU-written
   DEFAULT and READBACK lists are refused as `unsupported` before submission, since their mapped bytes can still hold
   an earlier frame's pointers when the CPU prepares the build. Completed staging buffers
   are reused, so a per-frame rebuild does not allocate memory each frame. Updates refit the source structure instead of rebuilding it, `CopyRaytracingAccelerationStructure` clones and compacts, and the
   postbuild compacted and current sizes are written ([bvh.md](bvh.md) §7a). Serialization and the Xbox
   batched/streaming builds are not implemented. The
   reported `RaytracingTier` is unchanged (`NOT_SUPPORTED`). Checked by `runtime/test/d3d12_xdxr_rt_test.cpp` through
   the D3D12 API.

**Open:** which path the game uses. Resolve by logging the first call to the standard AS methods and by tracing the
builder's command-list usage. If XDXR batched/streaming builds are used, their structs and compression formats need
to be recovered from call sites (or the NDA GDK headers, if a developer has access); they cannot be guessed.

### 4.9 Xbox-specific surface

| Feature | Public evidence | Design |
|---|---|---|
| No ESRAM | Xbox Series X\|S uses unified GDDR6; ESRAM was Xbox One | Do not implement ESRAM-era APIs. The `d3d12_xs.cpp` comment about "ESRAM-era extensions" should be read as Xbox One, not Series. |
| Binning | The GDK topic index names Xbox binning modes (NDA content) | Should be a no-op if the game uses it only as an optimisation: it changes how a render pass uses on-chip memory, not the result. Log and ignore for the first boot; revisit for performance. **Open** whether the game relies on binning for correctness. |
| DCC / texture compatibility | Public release note for `D3D12XBOX_RESOURCE_FLAG_FORCE_TEXTURE_COMPATIBILITY` | Let Vulkan choose compression; fail if the game aliases/reads raw compressed data. |
| PresentX / present planes / frame events | Public GDK samples (MIT/zlib), §4.5 | Likely the only present path of a GDKX title. Presenter + vblank-timed frame events; 1-2 planes composed into the Vulkan swapchain (placement, filter, alpha overlay, HDR10 conversions). Call shapes known; slots/layouts **Open**. |
| Device slots 48/73/78/79/80 | Public HLE `Freminet`/`ScarlettX` (third-party, not Microsoft documentation) | `d3d12_xs_device.cpp` routes `CreateCommandQueueX` (48), `CreateCommandListX` (73), `SetFrameIntervalX` (78), `ScheduleFrameEventX` (79) and `WaitFrameEventX` (80) before it indexes the PC vtable. Queue and list creation use the existing create paths. The frame-event calls are a per-device token counter: `WaitFrameEventX` returns at once with a fresh non-zero token, because there is no vblank source before a presenter exists. The five indices can be moved with `LEONIDA_XS_QUEUE_X_SLOT`, `LEONIDA_XS_LIST_X_SLOT`, `LEONIDA_XS_FRAME_INTERVAL_SLOT`, `LEONIDA_XS_SCHEDULE_EVENT_SLOT` and `LEONIDA_XS_WAIT_EVENT_SLOT` (3..127; a value that names a slot the proxy already handles keeps that slot's handler); a trace that disagrees wins. |
| Indirect argument type 105 | The retail title's startup creates five one-argument command signatures (device slot 41) with no root signature; one has type 105 and a 12-byte stride. Its two `ExecuteIndirect` sites, read statically, set the compute state first, as the `DISPATCH` sites do, and pass one command from the same argument buffer, on a second command list | `d3d12_xs_device.cpp` creates it as `DISPATCH` (`D3D12_DISPATCH_ARGUMENTS`). What the console does differently for it is **Open**. |
| Device slot 61 | The retail title's boot under `gta6rt --run` (stop 20 of the RT-5 measurements in #6910): called as `(desc, dest handle)` once per sampler | `d3d12_xs_device.cpp` routes it to the PC `CreateSampler` (slot 22) with the description's PC `D3D12_SAMPLER_DESC` prefix, for a caller in an image the guest loader mapped; the PC slot 61 is `CreateMetaCommand`, which any other caller (a host test driving the device through the proxy) still gets. The layout of the description beyond the PC fields is **Open**. |
| Device slot 74 | The retail title's boot under `gta6rt --run`: called as `(pointer, u32, pointer)` | **Stand-in.** `d3d12_xs_device.cpp` answers `S_OK` and does nothing, for a caller in an image the guest loader mapped; the PC slot 74 is `ShaderCacheControl`, which the call does not fit and which any other caller still gets. What the console method is, and what it should do, is **Open**. |
| Command-allocator slot 9 | The retail title's boot under `gta6rt --run`: called past the 9-slot PC allocator, as `(function pointer, context, ...)`; `SetAllocatorCallbackX` is the name the commit that added it uses | **Stand-in.** The allocator proxy records the callback and its context (`allocator_proxy_callback`, `allocator_proxy_callback_context`) and answers `S_OK`. **Nothing calls the callback**: the Vulkan layer backs command buffers itself. Whether the title depends on being called back is **Open**. |
| Device slot 81, queue `PresentX`/`SuspendX`/`ResumeX` | XCL (GPL-2.0-only, third-party): `GetFrameStatisticsX` at device slot 81, `PresentX` at queue slot 30 (older runtime layout) or 32 (newer); `SuspendX`/`ResumeX` slots unmeasured | `set_presentation_handler` forwards 78-81 and the queue routes to a presenter (`attach_xbox_presentation`). Without one, slot 81 answers `S_OK` with a zero count (no statistics yet; the answer the retail boot through the story load was measured with, where the public samples' "not available yet" is `S_FALSE`; a null count, or a count above zero with no array, is `E_POINTER`); `PresentX` goes to the presentation the device proxy builds itself on the first call (`queue_present_x`: in the title's window or one of the runtime's, `LEONIDA_PRESENT_WINDOW`), which takes its frame tokens from the same source as the device's `WaitFrameEventX`; every other queue slot past the PC table answers `S_OK` and does nothing, except slot 31 (next row). `PresentX` is slot 30 on a queue in the older layout and slot 32 on a queue detected as the newer one (a fence reaching slot 16 or 17, or `SignalX` at 25), never both: in the newer layout slot 30 is another method. Slot 81 moves with `LEONIDA_XS_FRAME_STATISTICS_SLOT` (3..127); `LEONIDA_XS_PRESENT_X_SLOT` pins `PresentX` to one slot whatever the layout; `SuspendX` and `ResumeX` are routed only when `LEONIDA_XS_SUSPEND_X_SLOT` / `LEONIDA_XS_RESUME_X_SLOT` are set (queue slots 19..127; two routes on one slot are refused with a log). |
| Queue slot 31, `CopyPageMappingsBatchX` | Arguments `(count, batches, pool, pool pages, flags)`, a 24-byte batch `{UINT64 destination; UINT32 range count; const range* ranges}` and a 12-byte range `{UINT32 kind, start, count}`: read statically from the retail executable's call site and the code that fills the batches (#968), corroborated by the same call shape in a public project's queue (VIctory, `copy_page_mappings`, where the newer layout puts it at 29), and the pool-size argument equalled the page count of the title's own `XMemAllocatePhysicalPages` call in a boot | Every range goes to xmem's `copy_page_mappings_fn` (`d3d12_xs_device.cpp`, `queue_copy_page_mappings`): kind 0 maps pool pages `start, start + 1, ...` at the destination, 1 maps pool page `start` at every destination page, 2 unmaps, 3 leaves the destination as it is; each range moves the destination on by `count` pages. A destination page gets a second view of the physical page mapped at its source page, and the same view at the paired CPU range when the destination is a GPU-only reservation (`title_gpu_views`), where the layer reads that memory. `S_OK` when every range was applied, `E_INVALIDARG` (logged once) for a range past the pool, an unknown kind or one xmem refuses; without xmem linked (tests of the proxy alone) the slot answers `S_OK` like the other console-only slots. Tested by `xmem_test` and `xs_proxy_mock_test`. **Known gap:** the mappings change when the call is made, not when the queue reaches the batch, as on the console; whether earlier queued work still reads a destination page then is not measured. |
| VRS | Public `D3D12_SHADING_RATE` / `D3D12_AXIS_SHADING_RATE` | `VK_KHR_fragment_shading_rate`; `RSSetShadingRate`/image-based rate map to `vkCmdSetFragmentShadingRateKHR`. |
| Mesh shader tier | Public `D3D12_MESH_SHADER_TIER` | `VK_EXT_mesh_shader`; the 9,282 translated mesh shaders make this a hard requirement for any content that uses them. |
| Wave MMA | Public `D3D12_WAVE_MMA_TIER` | Not used: no shader sets `WAVE_MMA` in SFI0 and none contains `v_mfma`/`v_wmma` (all 26,503, `tools/feature_scan.py`). Answer the tier query truthfully; no cooperative-matrix mapping is needed. |
| Enhanced barriers | Public Xbox Series `D3D12_BARRIER_GROUP` | `VK_KHR_synchronization2`; implement directly. |
| List slot 146: texture barrier with no resource | The retail title records `D3D12_BARRIER_TYPE_TEXTURE` barriers whose `pResource` is null in its `Barrier(NumBarrierGroups, pBarrierGroups)` calls; on the console such a barrier is a global memory and execution barrier | `d3d12_xs_device.cpp` turns each into a `D3D12_GLOBAL_BARRIER` with the same `SyncBefore`/`SyncAfter`/`AccessBefore`/`AccessAfter` (layouts, subresource range and flags dropped) before it calls GCL7 `Barrier` (PC slot 80). A texture group with both kinds is split into runs, in order, in storage that lives for the call; a call with nothing to convert goes through with the title's own pointer. Without this the PC list goes into its error state, `Close` fails and `ExecuteCommandLists` drops the submission. A group array the proxy cannot read safely (null with a nonzero count, a group type it does not know, more than 1,024 groups or 65,536 barriers in one group) is refused whole, nothing recorded, and logged once per reason. **Confirmed by the mock tests only** (`xs_proxy_mock_test`), not by a retail run. |
| List slot 52: clear of an RTV made with no resource | The title creates some render-target views with `CreateRenderTargetView(nullptr, ...)` and clears them every frame; on the console that clear does nothing | The device proxy marks the real handle when device slot 20 gets no resource; a view made there with a resource removes the mark, and `CopyDescriptors`/`CopyDescriptorsSimple` of RTVs move it with the descriptors. Releasing an RTV heap drops the marks in its real CPU range, so a heap made later at the same address starts unmarked. List slot 52 skips the clear of a marked view (logged the first time) and the list stays valid. The mark is the proxy's own because the real descriptor cannot tell: the Vulkan layer writes the same empty record for a view it could not make, and a PC runtime's handles are opaque. Any other view, valid or not, still reaches `ClearRenderTargetView` (PC slot 48); RTV records the title copies with its own `memcpy` are not followed. **Confirmed by the mock tests only** (`xs_proxy_mock_test`), not by a retail run. |
| XDXR batched/streaming BVH | `XboxBvhBuilder.dll` exports | See 4.8. |

The null-resource barrier and null-view clear behaviour were first reported by luizwidmer/palmetto.

### 4.10 Debug markers and private data

Games and middleware call these every frame (PIX events), so they must be cheap and never fail. Implemented
(D3D-24) in `runtime/shims/d3d12_com.cpp`, `runtime/shims/d3d12_com_object.h`, `runtime/src/command_list.h`,
`runtime/src/vulkan_replay.h` and `runtime/src/vulkan_backend.h`; tested by `runtime/test/d3d12_debug_markers_test.cpp`.

- **Labels, only with `VK_EXT_debug_utils`.** It is an instance extension, so `VulkanDeviceInfo::debug_utils` says
  whether the instance enabled it; `create_vulkan_device_standalone` enables it when the loader lists it. Without
  it every marker call is a no-op (nothing logged, nothing recorded on the queue).
- **Command lists.** `SetMarker`/`BeginEvent`/`EndEvent` are recorded in the IR (`DebugMarker`) in order with the
  other commands and replayed as `vkCmdInsertDebugUtilsLabelEXT`/`vkCmdBeginDebugUtilsLabelEXT`/
  `vkCmdEndDebugUtilsLabelEXT` into the submission's command buffer.
- **Queues.** The same three methods on `ID3D12CommandQueue` call `vkQueueInsert/Begin/EndDebugUtilsLabelEXT` under
  the device's submit lock, so they fall between the submissions issued before and after them.
- **Balancing.** D3D12 does not make a list balance its events; Vulkan requires every end to have a begin.
  One `ExecuteCommandLists` is one command buffer, so a begin in one list may be ended by a later list of the
  same call. An end with nothing open is dropped, and labels still open when the last list ends are ended
  before the command buffer is ended. Queue labels are balanced per queue the same way (an unmatched end is
  dropped; open ones are ended when the queue is destroyed). Command-buffer labels never span two
  `ExecuteCommandLists` calls.
- **Labels.** The `Metadata` argument says what the data is, following WinPixEventRuntime's `pix3_win.h`:
  0 (`PIX_EVENT_UNICODE_VERSION`) a UTF-16 string, 1 (`PIX_EVENT_ANSI_VERSION`) an ANSI string, both cut at the
  terminator or at `Size` bytes and converted to UTF-8; 2 (`PIX_EVENT_PIX3BLOB_VERSION`) PIX's own encoded event,
  which is not decoded and gets the fixed label `PIX event` so the nesting is still visible (question 21). Other
  values get the same label and log once.
- **Private data, on every object.** `SetPrivateData`, `GetPrivateData`, `SetPrivateDataInterface` and `SetName`
  live in the shared `Object` base, so the device, queues, allocators, lists, fences, descriptor heaps and
  resources all have them. Every answer is the host runtime's: `d3d12_private_data_test` runs the same 672 steps
  on the device, a fence, a buffer, an allocator and a descriptor heap of both devices and compares each HRESULT,
  returned size, byte and reference count (RTX 3060, driver 610.88, no debug layer):
  - `SetPrivateDataInterface` holds a reference until the entry is replaced (by bytes or another interface),
    removed or the object is destroyed; `GetPrivateData` returns the pointer with a reference added. A null
    interface releases the one held and stays as a pointer-sized entry that reads back as null.
  - A buffer too small for the entry gets `DXGI_ERROR_MORE_DATA`, nothing copied and no reference added, and
    `*pDataSize` set to the size needed; a missing entry is `DXGI_ERROR_NOT_FOUND` with `*pDataSize` 0; a null
    `pDataSize` is `E_INVALIDARG`.
  - `SetPrivateData` with null data removes the entry whatever the size: `S_OK` when there was one, `S_FALSE` when
    there wasn't. Zero bytes with data keep an empty entry (`S_OK`, size 0).
  - `SetName` is `SetPrivateData` with `WKPDID_D3DDebugObjectNameW` (`{4cca5fd8-921f-42c8-8566-70caf2a9b741}`) and
    the name with its terminator, as D3D12 documents: `GetPrivateData` reads it back, an empty name is a 2-byte
    entry, and the narrow `WKPDID_D3DDebugObjectName` is not touched. A null name, which the host runtime faults on,
    changes nothing (`E_INVALIDARG`). Before #2533 the layer stored the name under a mistyped GUID.
  Names are not forwarded to `vkSetDebugUtilsObjectNameEXT` yet: one D3D12 object may own several Vulkan objects.

### 4.11 Vulkan device-loss diagnostics

Set `LEONIDA_DEVICE_FAULT=1` before creating the runtime's Vulkan device to enable the supported device-fault and
NVIDIA checkpoint extensions. On the first observed device loss, stderr logs driver fault addresses, vendor data
and binary size, plus the checkpoints reached on each queue. Checkpoints identify the replay submission, its
original `ExecuteCommandLists` call, the zero-based list and command indices, and the most recent shader names
in that list when available. A checkpoint shows progress at a reported pipeline stage and can help narrow down
the faulting work. The loss report also lists the last 64 compute dispatches submitted across queues, including
direct group dimensions or indirect argument-buffer locations, even without driver checkpoints. These entries show
submission order. Set `LEONIDA_DEVICE_FAULT_ROOTS=1` to include each dispatch's 32 recorded root words in the local
diagnostic log. While this mode is enabled, a host thread samples the submission timelines once a second. If the
retired ticket stays behind submitted work for 15 seconds, it prints the CPU dispatch history. It also reports
when it cannot sample the submission lock for that long. A progress warning leaves D3D12 fence values and the
device state intact. The driver checkpoint and fault-info queries run after Vulkan reports device loss. This mode
is off by default.

Each dispatch line carries a `seq`: a number every recorded dispatch gets, across queues and submissions, in the
order the submissions were recorded. The list keeps the last 64, so its numbers can have gaps where a submission
recorded more than that.

**AMD breadcrumbs.** NVIDIA's checkpoints say where a queue stopped; RADV has none. With `LEONIDA_DEVICE_MARKERS=1`
as well, on a device with `VK_AMD_buffer_marker`, every numbered dispatch writes its `seq` into its queue's slot of
one persistent buffer twice (`src/vulkan_device_markers.h`): at top of pipe before the dispatch (started) and at
bottom of pipe after it (finished), with `vkCmdWriteBufferMarker2AMD` (or `vkCmdWriteBufferMarkerAMD` without
synchronization2). A bottom-of-pipe write waits for all earlier work on the queue, so the finished value is the last
dispatch before which everything had completed, and the queue's dispatches after it up to the started value are the
ones the GPU began and did not finish. The oldest of them is the likely hang. The buffer is host-visible and
coherent, in system memory where the device has such a type, so it can still be read after a device loss. The loss
and stall reports then add lines like these (the shader names are made up):

```
[rt] device-markers: AMD buffer markers, read after the device loss; best-effort: only what the GPU wrote before it stopped, and a reset may have lost the last writes
[rt] device-markers: queue 0000000012345678 (compute) finished seq=1203 started seq=1205; started, not finished: 1204 1205
[rt] device-markers: likely hang: seq=1204 execute=88 queue=0000000012345678 (compute) list=0 command=17 shader=cs_example_a
[rt] device-markers: queue 00000000123456a0 (direct) idle: finished seq=1199, nothing started after it; next submitted seq=1206 not started (waiting, or behind other work of the queue)
[rt] device-fault: submitted dispatch seq=1204 execute=88 queue=0000000012345678 (compute) list=0 command=17 shader=cs_example_a groups=64,1,1 <- likely hang (device-markers)
```

A separate history of the last 16,384 dispatches names the likely hang when it has already left the 64-entry list.
An idle queue (finished equals started) is not called hung: if it still has dispatches submitted, it was waiting
on a semaphore or stuck in work that is not a dispatch (a draw, a copy). The values are what the GPU wrote before
it stopped; after a loss and a ring reset they may be stale, so read them as a pointer, not a proof. The cost with
the switch on is two marker writes per dispatch (a command-processor write and an end-of-pipe event, no wait) plus
one entry in the history; with it off nothing is recorded. Without the extension, or without
`LEONIDA_DEVICE_FAULT`, the runtime logs one `device-markers: unavailable` line and runs as before.

For a local pre-submit inspection, also set `LEONIDA_COMPUTE_INPUTS` to comma-separated shader name fragments and
`LEONIDA_COMPUTE_INPUT_SLOTS` to comma-separated decimal `root/slot` pairs. For example, `4/86,5/0,5/2,7/0` reads
four 32-byte descriptors. The probe logs the bound heap and root layout, the saved user SGPRs, observed descriptor
writer, buffer address, stride, record count and readable backing. It also follows the per-group scene indices of
the Prop and Tree cullers when both `5/0` and `4/86` are selected. Each sample is taken after CPU-side shadow
refresh and before queue submission. Earlier GPU writes may still be in flight. An imported heap can have a valid
shader offset beyond the arena. Keep probe output local. Unset `LEONIDA_COMPUTE_INPUTS` to disable it.

## 5. Producing the shader contract

The table below is the contract from [design.md](design.md#translator-status-and-runtime-contract) and where the
runtime gets each piece. The shader side is already proven by `runtime/test/vk_texture_test.cpp` and the compute,
store, load, mesh, geometry and BVH tests; this layer is the other half.

| Contract | Runtime source |
|---|---|
| `s0..s31` user data | Root signature state at draw/dispatch: root constants copied, root CBV/SRV/UAV addresses and descriptor-table base addresses as 64-bit console VAs. Mapping per section 3.5; absent parameters zero. |
| `heap_base` (dwords 32–33) | The arena's `vkGetBufferDeviceAddress`. One value per command buffer; updated only if the arena is recreated. |
| `code_base` (dwords 34–35) | The console VA of the current shader's FSSM blob in the arena. Per-stage conflict: see below. |
| Descriptor hash table (set 0, binding 2) | `uint mask` + entries of `key[8], index, pad`; FNV-1a over the 8 key dwords; linear probe up to 8; miss = index 0. Implemented in `runtime/src/descriptor_table.h` (`DescriptorTable`). The layer inserts every T#/S# it sees and keeps a fallback image/sampler at index 0. |
| Bindless images (bindings 0, 3, 5, 6) | 2D, 2D-array/cube, 1D, 3D sampled-image arrays; typed single-sample shader declarations alias the same binding numbers. Cube textures are 6-layer 2D arrays and cube arrays 6N-layer ones. The runtime also reserves optional MS SRV pairs at 20–25; the translator does not use them yet. |
| Samplers (binding 1) | One `VkSampler` per distinct S#; literal S# values from shader constants are pre-registered. |
| Storage images (bindings 4, 7, 8–19, 26–28) | Bindings 4 and 7 are formatted `r32ui` for 2D and 3D atomics. Bindings 8–19 are float/UINT/SINT triples for 2D, 2D-array, 3D and 1D. Bindings 26–28 are the float/UINT/SINT triple for 1D arrays, which have no sampled binding. The translator picks the numeric class from the T# format; the arrays for a dimension share the same index. |
| Buffer/image binding | Descriptors contain console VAs, never host BDAs. Buffers are in the arena at their VA; images are keyed by T# through the hash table. |
| Vertex fetch | PSO input layout + ISG1 signature; location = D3D input register; format from `DXGI_FORMAT`; instance step rate from the layout. The fetch subroutine is already replaced by Vulkan inputs. |
| PS system inputs | `FragCoord` (xy/z/w), `FrontFacing`, barycentrics; enable `fragmentShaderBarycentric` and `fragmentShaderPixelInterlock` when the index entry asks. |
| Mesh/geometry stages | `VK_EXT_mesh_shader`; the index's max vertices/primitives size the SPIR-V output arrays and are checked against the device's mesh properties; geometry stage for the 195 voxelize shaders; `DispatchMesh` → `vkCmdDrawMeshTasksEXT`. |
| POPS collision | The runtime puts 0 (no collision, bit 31 clear) in the POPS collision SGPR; the translator uses fragment-shader interlock for ordering. **Open:** the exact SGPR location is a translator/driver detail not named in the current contract. |
| Wave size | The PSO asks for the size the shader was translated for (`requiredSubgroupSize` 64 or 32, section 3.2 "Subgroup size") where the stage is in `requiredSubgroupSizeStages`; a shader translated with `--wave64-emulate` asks for nothing. A size the device cannot give is named in the log. |
| Code-embedded data | The whole FSSM blob is in the arena; `s_getpc_b64` becomes `code_base + next-instruction offset`. |

### 5.1 Descriptor hash table details

The exact layout the shader reads (from `tools/rdna2spirv.py`, `desc_lookup`):

```
uint32 mask;                 // slot count - 1
struct entry {               // 10 uints = 40 bytes
    uint32 key[8];           // T#: 8 dwords; S#: 4 dwords then 0xFFFFFFFF x4
    uint32 index;            // bindless index
    uint32 kind;             // 0 empty; bit 31 occupied; bits 0..5 the sampled arrays the index is bound in,
                             // bits 8..11 its storage arrays (one bit per ImageDimension)
} entries[mask + 1];
```

- Hash: FNV-1a over the eight key dwords as dwords (not bytes): `h = 2166136261; h = (h ^ key[i]) * 16777619`.
- Optional finalizer (`fnv1a_fmix32`): a module whose `index.json` meta says `"descriptor_hash": "fnv1a_fmix32"` runs
  MurmurHash3's `fmix32` on that word before probing (`h ^= h >> 16; h *= 0x85EBCA6B; h ^= h >> 13; h *= 0xC2B2AE35;
  h ^= h >> 16`, logical shifts). An absent key means bare `fnv1a`. `DescriptorTable` fixes one kind per table before its
  first insert, taken from the modules' metas: all agree is that kind, mixed is the majority (the minority's lookups miss
  to index 0), and `LEONIDA_DESCRIPTOR_HASH=fnv1a|fnv1a_fmix32` overrides. The two kinds' shared vectors are in
  `runtime/test/descriptor_table_test.cpp`.
- Probe: `slot = (h + k) & mask` for `k = 0..7`; the first entry whose eight key dwords match yields its
  `index`; no match yields 0.
- `kind` says which arrays that index is good for, because it was given out in the T#'s own dimension's index space
  (`runtime/src/vulkan_bindless.h`, `ImageDimension`): bit `d` is dimension `d`'s sampled array, bit `8 + d` its
  storage arrays. A module whose `index.json` meta says `"descriptor_table": "tagged"` reads it and uses the index
  only in an array the entry names, else index 0 of the array its instruction reads; an absent key is the old plain
  layout, which reads only the index. `DescriptorTable::insert` takes the kind, and `BindlessSet::image_kind_at`
  derives it from what was bound at the index (its own arrays, storage views, and the 2D copy of a 2D-array image's
  first layer, whose slot records that image as its owner so another dimension's image with the same slot number does
  not inherit it). One layout serves the device: any module whose meta says `tagged` makes the table tagged, because
  the kind word is additive (a plain module ignores the word and reads the index, which the tagged layout keeps at
  the same place), so a plain majority never turns it off; no tagged module is the old plain layout.
  `LEONIDA_DESCRIPTOR_TABLE=plain|tagged` overrides, and the runtime says so loudly when an override forces plain
  while tagged modules exist.
- The layer must keep the table sparse enough that no live key needs more than eight probes. When all eight slots are occupied,
  the runtime tries to move one resident to the next empty slot within that resident's own probe window.
  A relocation requires an idle flush because it overwrites a live slot. Grow when this fails or when the next
  insertion would exceed 50% load. Update binding 2 after
  the rebuild, at a point where no in-flight command buffer reads the old table.
- Insert on `CreateShaderResourceView`, `CreateUnorderedAccessView`, `CreateSampler`, `CopyDescriptors` and heap
  scans. A duplicate key updates the index in place.
- Index 0 is the fallback: a 1×1 texture and a default sampler (and, for the storage arrays, a null descriptor
  with `VK_EXT_robustness2` `nullDescriptor` or else a storage image apart from the sampled ones). The translator
  returns 0 on a miss, so index 0 must always be valid.
- The table buffer is a normal storage buffer, not part of the arena. It can be host-visible and updated directly
  (host-coherent, or flushed before submit); if the game is sensitive to PCIe traffic, stage it like any other
  upload. The shader only reads it.
- Inserts can come from several threads (view creation is often parallel). Serialise them with a mutex, or stage
  per-thread and merge at submit. The shader only reads the table, and D3D12 already forbids modifying a descriptor
  that an in-flight command list is using, so the same rule applies.

**Measured (D3D-56, `runtime/test/vk_descriptor_bench.cpp`).** Measured on an NVIDIA GeForce RTX 5080 (discrete, `0x10de:0x2c02`, subgroup 32, driver `NVIDIA 621.24`, Vulkan 1.4.357), Windows 11 Pro 26H2 (build 26300), revision `ec8ec4c9`, using 64-bit Vulkan GPU timestamps (`1.000 ns` period) against `leonida::DescriptorTable` (90% T# / 10% S#) with the exact `tools/rdna2spirv.py` `desc_probe` loop. Unless noted, dispatches run `4096 x 64` invocations x `64` iterations (`16.78M` single / `33.55M` T#+S# pair lookups); wave-uniform sweeps use `group_stride = iters` (`64`), `lane_stride = 0` so all distinct entries are visited (at 100k: `90,000` T#, `10,000` S#, `100,000` T#+S# pairs, `131,072` miss keys; `uniform_hot_tsharp` repeats `1` key and measures `2.6–4.4 us/1M` in `device_local`). This run includes `bc8e604`'s bounded slot relocation, so its 100k row is at the same `524,288`-slot geometry as the AMD RDNA3/RDNA4 tables below and directly comparable to them, and it refreshes an earlier pre-relocation RTX 5080 table. `device_local` and `host_device_local` are the median of 4 processes, `host_visible` one; every row is `timing_status=ok` with no dispatch auto-scaling:

| Entries (cap / load / bytes / mean probes) | Placement | `uniform_tsharp_hit` (us/1M, Glk/s) | `uniform_pair_ts` (us/1M, Glk/s) | `divergent_tsharp_hit` (us/1M, Glk/s) | `divergent_pair_ts` (us/1M, Glk/s) | `divergent_miss` (us/1M, Glk/s) |
|---|---|---|---|---|---|---|
| 1k (4,096 / 24.4% / 160 KiB / 1.316; 900 T#, 1k pair, 1,024 miss) | `device_local` | 3.322 (301.06) | 2.917 (342.84) | 24.008 (41.65) | 23.759 (42.09) | 96.175 (10.40) |
| 1k | `host_device_local` `host_visible` | 3.317 (301.49) 4.890 (204.48) | 2.918 (342.73) 3.583 (279.10) | 23.973 (41.71) 27.582 (36.26) | 23.732 (42.14) 26.426 (37.84) | 96.148 (10.40) 395.433 (2.53) |
| 10k (65,536 / 15.3% / 2.50 MiB / 1.310; 9k T#, 10k pair, 16,384 miss) | `device_local` | 12.842 (77.87) | 5.142 (194.49) | 38.202 (26.18) | 39.618 (25.24) | 206.654 (4.84) |
| 10k | `host_device_local` `host_visible` | 12.838 (77.89) 211.889 (4.72) | 5.166 (193.59) 16.257 (61.51) | 38.223 (26.16) 10261.465 (0.10) | 39.468 (25.34) 3334.286 (0.30) | 209.528 (4.77) 181717.455 (0.01) |
| 100k (524,288 / 19.1% / 20.00 MiB / 1.416; 90k T#, 100k pair, 131,072 miss) | `device_local` | 6.007 (166.47) | 7.159 (139.69) | 71.939 (13.90) | 47.306 (21.14) | 483.346 (2.07) |
| 100k | `host_device_local` `host_visible` | 6.025 (165.97) 213.404 (4.69) | 7.161 (139.64) 183.383 (5.45) | 71.895 (13.91) 15149.069 (0.07) | 47.277 (21.15) 16289.665 (0.06) | 482.358 (2.07) 205561.481 (0.00) |

Three consequences for the runtime and table layout (additional GPU measurements follow, including AMD RDNA3):

1. **In `DEVICE_LOCAL` VRAM, the hash probe itself is cheap at all three sizes:** wave-uniform full-table sweeps (`uniform_tsharp_hit`) run at `3.3–12.8 us / 1M`, divergent lookups at `24.0–71.9 us / 1M`, and the 8-probe miss at `96–483 us / 1M`.
2. **`BindlessSet::create()` currently allocates PCIe `HOST_VISIBLE | HOST_COHERENT` system memory (`runtime/src/vulkan_bindless.h`):** once working sets exceed cache at 10k–100k entries, `host_visible` slows `uniform_tsharp_hit` by **16.5x–35.5x**, divergent T# hits by **210x–269x**, and 8-probe misses by **425x–879x** (`181,717 us / 1M` at 10k, `205,561` at 100k). `host_device_local` (ReBAR `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT`) matches `device_local` within **1.4%** on every row here (largest delta 10k `divergent_miss`, `+1.4%`). Staging binding 2 to `DEVICE_LOCAL` (or preferring `DEVICE_LOCAL | HOST_VISIBLE` when small) avoids this cliff without touching the shader contract.
3. **`bc8e604`'s bounded slot relocation keeps the table dense:** 100k settles at `524,288` slots (`19.07%` load, `20.00 MiB`, `max_probes=8`) instead of doubling to `2,097,152` on a 9-probe collision, so the large-table cost is bounded by the 8-probe window; the dominant divergent cost is the 8-probe miss (`483 us / 1M` at 100k).

Raw CSV rows (297, 9 processes): [D3D-56 RTX 5080 report](https://github.com/ItsNotRexed/Leonida/issues/313#issuecomment-7084711283).

**Measured (D3D-56, AMD RDNA3).** The same benchmark and 64-bit timestamps on an AMD Radeon RX 7900 XTX (discrete, `0x1002:0x744c`, subgroup 64, driver `AMD proprietary 26.10.1 (LLPC)`, Vulkan 1.4.359), Windows 11 build 26200, revision `2aed144`. Dispatches, iterations and reps match the NVIDIA run above, but the table code does not: this revision includes `bc8e604`'s bounded slot relocation, so at 100k it settles at `524,288` slots (`20.00 MiB`, `19.07%` load, `max_probes=8`), the same geometry as the refreshed RTX 5080 table above. The two tables now describe the same `DescriptorTable` code and their 100k rows are comparable; the remaining caveat is subgroup size (AMD `64` vs NVIDIA `32`).

| Entries (cap / load / bytes / mean probes) | Placement | `uniform_tsharp_hit` (us/1M, Glk/s) | `uniform_pair_ts` (us/1M, Glk/s) | `divergent_tsharp_hit` (us/1M, Glk/s) | `divergent_pair_ts` (us/1M, Glk/s) | `divergent_miss` (us/1M, Glk/s) |
|---|---|---|---|---|---|---|
| 1k (4,096 / 24.4% / 160 KiB / 1.316) | `device_local` | 6.220 (160.76) | 5.705 (175.27) | 15.285 (65.42) | 14.051 (71.17) | 50.461 (19.82) |
| 1k | `host_device_local` `host_visible` | 8.783 (113.85) 9.508 (105.17) | 5.360 (186.58) 5.783 (172.93) | 15.380 (65.02) 15.869 (63.02) | 13.974 (71.56) 13.465 (74.27) | 49.765 (20.09) 51.978 (19.24) |
| 10k (65,536 / 15.3% / 2.50 MiB / 1.310) | `device_local` | 5.152 (194.09) | 4.880 (204.90) | 27.998 (35.72) | 20.506 (48.77) | 146.217 (6.84) |
| 10k | `host_device_local` `host_visible` | 5.066 (197.38) 7.536 (132.69) | 4.841 (206.57) 6.236 (160.36) | 28.322 (35.31) 72.932 (13.71) | 20.641 (48.45) 54.183 (18.46) | 144.532 (6.92) 334.096 (2.99) |
| 100k (524,288 / 19.1% / 20.00 MiB / 1.416) | `device_local` | 7.618 (131.28) | 6.005 (166.54) | 53.558 (18.67) | 54.927 (18.21) | 290.279 (3.45) |
| 100k | `host_device_local` `host_visible` | 7.660 (130.54) 47.905 (20.87) | 6.040 (165.55) 29.887 (33.46) | 53.983 (18.52) 3492.770 (0.29) | 54.804 (18.25) 2240.356 (0.45) | 292.561 (3.42) 32108.853 (0.03) |

On this GPU:

1. **`device_local` is stable and cheap at every size.** Wave-uniform sweeps run `4.9–7.6 us / 1M` (the 100k `uniform_tsharp_hit` is `7.618`), divergent lookups `14.1–54.9 us / 1M`, and the 8-probe miss `50.5–290.3 us / 1M`. Repeating the 10k measurement keeps `divergent_miss` at `141–143 us / 1M` net, so this column is not noise.
2. **`host_visible` falls off a cliff at 100k, and is unreliable below it.** At 100k it is `5.0x–6.3x` slower than `device_local` on wave-uniform sweeps, `41x–65x` on divergent hits and pairs, and `111x` on 8-probe misses. At 10k the same rows varied by up to 6x between runs (`divergent_miss` net `147–845 us / 1M` over five runs), so the 10k host row above is one representative run; the uniform rows moved much less (`6.1–8.7 us / 1M`). The conclusion does not change: `device_local` is stable, `host_visible` is not.
3. **`host_device_local` (ReBAR) is a safe fallback on AMD.** It is within 1% of `device_local` at 100k and within 2% at 10k; at 1k it is within 1.5% on the divergent and miss rows, `uniform_pair_ts` is 6% faster (`5.36` vs `5.71 us / 1M`) and `uniform_tsharp_hit` is 41% slower (`8.78` vs `6.22 us / 1M`). The `DEVICE_LOCAL | HOST_VISIBLE` allocation succeeds and runs here; it is not guaranteed on every Windows driver.
4. **At the shared `524,288`/8-probe geometry, the 100k rows are comparable across the tables.** The RX 7900 XTX `divergent_miss` (`290 us / 1M`) is below the refreshed RTX 5080 (`483`), with the RX 9070 XT (RDNA4) between at `422` ([#313](https://github.com/ItsNotRexed/Leonida/issues/313)); the RX 7900 XTX is also faster on 100k divergent T# hits (`54` vs `72`) but slower on wave-uniform sweeps (`7.6` vs `6.0 us / 1M`). The subgroup size differs (AMD `64`, NVIDIA `32`), so this is a device-plus-subgroup comparison, not a pure microarchitecture one.

Raw CSV rows: [D3D-56 report](https://github.com/ItsNotRexed/Leonida/issues/313#issuecomment-7074181793), addendum with the repeatability runs: [same issue](https://github.com/ItsNotRexed/Leonida/issues/313#issuecomment-7074220711).

**Measured (D3D-56, Intel integrated + RTX 5070 Ti Laptop).** These synthetic runs use `vk_descriptor_bench` from revision `f85582e`, Windows 11 Pro Insider Preview `10.0.29704`, MSVC 19.51, Vulkan SDK 1.4.363.0, and the exact `4096 x 64` invocations x `64` iterations (`16.78M` single / `33.55M` T#+S# pair lookups, median of 25 reps). NVIDIA runs used `--sizes 1000,1000,10000,100000 --warmup 200`; the Intel run additionally duplicated 100k (`--sizes 1000,1000,10000,100000,100000 --warmup 200`). The first 1k block is excluded because its `baseline_uniform` timing differed from the duplicate (Intel device-local: `0.771 ms` vs `0.594 ms`; NVIDIA device-local: `0.0555 ms` vs `0.0765 ms`). For Intel 100k, the second block is retained after the duplicate confirmed stable timing per allocation. Retained rows were not auto-scaled. Intel's device-local and host-visible placements ran together on the integrated GPU. NVIDIA device-local and ReBAR were measured in separate processes. The RTX 5070 Ti Laptop is `0x10de:0x2f58` (driver `621.01`, Vulkan `1.4.357`); Intel Graphics is `0x8086:0x7d67` (driver `32.0.101.8132`, Vulkan `1.4.325`).

| GPU / entries (cap / load / bytes / mean probes) | Placement | `uniform_tsharp_hit` (us/1M, Glk/s) | `uniform_pair_ts` (us/1M, Glk/s) | `divergent_tsharp_hit` (us/1M, Glk/s) | `divergent_pair_ts` (us/1M, Glk/s) | `divergent_miss` (us/1M, Glk/s) |
|---|---|---|---|---|---|---|
| Intel iGPU, 1k (4,096 / 24.4% / 160 KiB / 1.316) | `device_local` | 85.669 (11.67) | 76.483 (13.07) | 468.524 (2.13) | 431.528 (2.32) | 1,735.402 (0.58) |
| Intel iGPU, 1k | `host_visible` | 85.356 (11.72) | 76.500 (13.07) | 468.750 (2.13) | 425.419 (2.35) | 1,741.669 (0.57) |
| Intel iGPU, 10k (65,536 / 15.3% / 2.50 MiB / 1.310) | `device_local` | 86.806 (11.52) | 76.642 (13.05) | 597.170 (1.67) | 507.250 (1.97) | 2,495.724 (0.40) |
| Intel iGPU, 10k | `host_visible` | 87.665 (11.41) | 76.415 (13.09) | 597.099 (1.67) | 506.874 (1.97) | 2,496.882 (0.40) |
| Intel iGPU, 100k (524,288 / 19.1% / 20.00 MiB / 1.416) | `device_local` | 101.132 (9.89) | 84.286 (11.86) | 672.021 (1.49) | 964.268 (1.04) | 5,580.289 (0.18) |
| Intel iGPU, 100k | `host_visible` | 100.269 (9.97) | 84.437 (11.84) | 668.788 (1.50) | 975.549 (1.02) | 5,581.019 (0.18) |
| RTX 5070 Ti Laptop, 1k (4,096 / 24.4% / 160 KiB / 1.316) | `device_local` | 11.028 (90.68) | 9.652 (103.60) | 48.388 (20.67) | 48.201 (20.75) | 195.543 (5.11) |
| RTX 5070 Ti Laptop, 1k | `host_device_local` (ReBAR) | 11.072 (90.32) | 9.682 (103.29) | 51.245 (19.51) | 48.427 (20.65) | 194.840 (5.13) |
| RTX 5070 Ti Laptop, 10k (65,536 / 15.3% / 2.50 MiB / 1.310) | `device_local` | 11.562 (86.49) | 10.164 (98.38) | 63.725 (15.69) | 66.871 (14.95) | 367.062 (2.72) |
| RTX 5070 Ti Laptop, 10k | `host_device_local` (ReBAR) | 11.494 (87.00) | 10.184 (98.19) | 63.858 (15.66) | 66.909 (14.95) | 297.104 (3.37) |
| RTX 5070 Ti Laptop, 100k (524,288 / 19.1% / 20.00 MiB / 1.416) | `device_local` | 11.993 (83.38) | 46.172 (21.66) | 116.020 (8.62) | 82.513 (12.12) | 788.086 (1.27) |
| RTX 5070 Ti Laptop, 100k | `host_device_local` (ReBAR) | 9.186 (108.86) | 46.162 (21.66) | 99.737 (10.03) | 141.439 (7.07) | 632.504 (1.58) |

On these devices:

1. **The Intel placements are close on the lookup workloads.** Gross dispatch times for `device_local` and `host_visible` track within 1.5%; after subtracting `baseline_uniform`, differences are within 1.7% through 10k and within 11.2% on 100k uniform hits. At 100k, the baseline itself is repeatable per allocation but differs between them (`0.730 ms` device-local vs `0.608 ms` host-visible), so the net uniform rows carry more uncertainty. Both allocations use the iGPU's shared system memory; this does not predict discrete-GPU PCIe behavior.
2. **NVIDIA ReBAR is close to device-local for uniform sweeps at 1k–10k and at 100k T#+S# pairs.** The 100k divergent and miss cases differ by workload (ReBAR is not uniformly faster or slower), so this run does not support a blanket placement ranking.
3. **The NVIDIA host-visible run is omitted.** A separate full run returned `vkWaitForFences -> -4` while measuring the 10k host-visible cases; those partial timings are not reported. A safer capped profile is needed before drawing conclusions about PCIe system-memory lookup on this laptop.

The run is synthetic and contains no game data. Raw retained rows and the corrected methodology are recorded in [the D3D-56 report on issue #313](https://github.com/ItsNotRexed/Leonida/issues/313#issuecomment-7078191651).

### 5.2 Per-stage code base

The current 144-byte block has one 64-bit code base at dwords 34–35. A graphics pipeline has at least a vertex and
a pixel shader, and each may use `s_getpc_b64` for its own constant table. One block cannot hold two different
code bases at the same offset. The pipeline layout declares the block for every stage that may read it (or for all
graphics stages), and `vkCmdPushConstants` is called with those stage bits. This is a real gap in the current
contract, not a runtime bug:

- Only 155 shaders use code-embedded data, and a graphics pipeline has at least two stages (typically VS+PS, or
  MS+PS). If only one stage in a pipeline uses it, the layer can set that stage's code base; the other stage never
  reads it. This probably covers the first boot. **ASSUMPTION.**
- If both stages use it, the layer cannot satisfy both with the current contract. The clean fix is a translator
  change: give each stage its own code-base slot (for example dwords 34–35 VS, 36–37 PS, 38–39 CS, 40–41 GS/MS,
  extending the block to 168 bytes) and have each compiled shader read its stage's slot. A 168-byte push-constant
  block is still under the 256-byte limit of common desktop GPUs, but above Vulkan's 128-byte minimum; the device
  requirement must be checked.
- A second option is a per-stage uniform/storage buffer for the code base, patched in at pipeline creation. That
  adds a descriptor binding and a load per `s_getpc`, but avoids growing push constants. Decide after measuring
  how many real PSOs have two code-base stages.

Measured per stage (all 26,503, `tools/feature_scan.py`): `s_getpc_b64` is kept to read code-embedded data in 54
VS, 93 PS, 266 MS and 263 CS (0 GS); the 147 VS/PS match the translator's own note. Both stages of a VS+PS or
MS+PS pipeline can therefore need a code base.

**Measured:** across the GPU test fleet (Issue #166), desktop NVIDIA (Turing, Ampere, Ada, Blackwell: 256 bytes), desktop AMD (RDNA2: 256 bytes) and Apple Silicon (MoltenVK: 4096 bytes) all satisfy the 144-byte contract natively (`maxPushConstantsSize >= 144`). Intel Arc is not uniform: the earlier fleet entries report the 128-byte specification minimum (`maxPushConstantsSize == 128`) for Arc, but an Arc B580 (Battlemage) on Windows 11, driver 32.0.101.9175, reports 256 bytes (#166, @rtrexmass), so there D3D-14's fallback never engages. The 128 figure may be specific to another OS or driver (Mesa ANV on Linux is the guess; no Arc run on Linux that records the OS is cited here, and an ANV result would settle it). So does an integrated AMD RDNA2 GPU, a Radeon 680M on the Windows driver 22.40.84.06, so the minimum is not Intel-only. That figure follows the driver as well: a Radeon 610M reports 128 on 24.10.16.14 and 256 on 26.11.2 (#166). 128 bytes cannot fit the 16-byte tail (dwords 32–35: heap base and code base) inside push constants. The runtime detects `maxPushConstantsSize < 144` at device creation (`d3d12_vulkan_device.cpp`) and flags `push_constants_need_fallback` for the uniform-buffer fallback.
**Open:** the exact count of such PSOs (the pairing is in the executable), and the minimum `maxPushConstantsSize` across target GPUs. If a GPU only
supports 128 bytes, the current 144-byte contract already fails; the layer needs a fallback (uniform buffer for the
tail) regardless.

At run time, `CreateGraphicsPipelineState` refuses a pair where both stages read their code base, with a log, so
the first boot measures the pair count. A device whose `maxPushConstantsSize` is below 144 has every PSO refused,
with the limit in the log, until the uniform-buffer tail exists.

### 5.3 Descriptor T#/S# parsing

The runtime needs a C++ decoder for the structures the shaders read. The fields it needs:

- **T# (image SRV):** base console VA, format (GFX10 format code), width/height/depth, mip levels, array layers,
  sample count, destination swizzle, dimension. The format table is `tools/gfx10_formats.json` (Mesa's
  `gfx10-rsrc.json`), the same one the translator uses.
- **S# (sampler):** filter modes, address modes (U/V/W), border color, min/max LOD, LOD bias, compare function,
  anisotropy, coordinate normalization.
- **V# (buffer):** base console VA, stride, record count, format, destination swizzle. Buffer descriptors are
  dereferenced directly by the shader; the layer only needs to validate the VA and the format for typed loads.

If the game calls `CreateShaderResourceView`/`CreateSampler`, the layer can encode these structures itself from
the D3D12 descriptors. If the game writes them, the layer decodes them. Both paths meet in the same parser. The
GPU texture test already proves the shader-side contract on a synthetic T#/S#; the runtime parser is new code.

## 6. Boot milestones

Each milestone is a capability gate with a synthetic test; the game is the integration test once it is available.
M0–M2 do not need the executable.

| # | Scope | What it proves | Done when |
|---|---|---|---|
| **M0** | `D3D12XboxCreateDevice` and the measured device overrides (heap start handles in RAX), one direct queue, allocator/list, fence, fake DXGI factory/adapter/output, `ALLOW_DISPLAY` back buffers, frame events + `PresentX` (DXGI swapchain only if the game creates one), clear + present | COM interception, the known Xbox device entry point and ABI overrides, presentation (`PresentX` + frame events, or swapchain), frame loop, clean shutdown | A window shows a stable clear colour for 60 frames, no Vulkan validation errors, every D3D12 call traced. **Synthetic part met** by `d3d12_present_test`: 60 frames through the proxy on Vulkan, validation clean, colour checked in the presented images and on screen where GDI can read it. Still open: the game's present front end (frame events + `PresentX`, or a DXGI swapchain) and the per-call trace |
| **M1** | Root signature with root constants (mask the measured `0x03000000` flag byte on a copy), one translated VS+PS, PSO, vertex buffer in the arena, draw, push constants `s0..s31` + heap base | PSO translation, shader hash lookup, vertex input, root-constant packing, heap base, present | The triangle matches a CPU/GPU reference; the existing texture-test style comparison passes with error 0. **Met with stand-in shaders** by `d3d12_draw_test`: every pixel of two indexed draws and a culled triangle matches, through the proxy, validation clean. With translated game shaders most of them declare bindings, which M2 (pipelines bind set 0) lets through. **M1.5 (D3D-53) met with translator output** by `d3d12_translated_draw_test`: see below |
| **M1.5** | The same, with real translator output: hand-written RDNA2 VS+PS, PSO found by FSSM hash, vertex fetch through a V# loaded from the user-data table, a root constant, a table in the arena, the code base | The push-constant, vertex-fetch and code-base contracts on translated code | Met by `d3d12_translated_draw_test`: three draws, every pixel off the triangle's edge exact in a float target, validation clean on the AMD Radeon 880M and the RTX 5070 Laptop. An upload buffer behind a V# is not supported (section 3.3) |
| **M2** | Descriptor heap (heap slots 9/10 return in RAX), `CreateShaderResourceView`, sampler, root descriptor table, hash-table insert, bindless sampling | The full D3D12 descriptor path and the shader's T#/S# contract | A textured quad matches the synthetic 8×8 gradient or a known texture with error 0; the hash table has the expected entries. **Synthetic part met** by `d3d12_m2_textured_test`: through the proxy, D3D12 API only, a shader-visible CBV/SRV/UAV heap and a sampler heap (start handles from slots 9/10), two 8×8 textures uploaded with `Map` + `CopyTextureRegion`, `CreateShaderResourceView` and a POINT `CreateSampler`, a root signature of two descriptor tables, `SetDescriptorHeaps`, `SetGraphicsRootDescriptorTable` and `DrawIndexedInstanced` of a quad whose PS is translator output (`ps_m2.asm`: `s_load` of the T# and the S# from the tables, `image_sample`). Every one of the 4096 pixels matches the CPU-computed gradient (left half) and its point reflection (right half, second table) with error 0, and the hash table holds a distinct non-zero index for each of the two T#s and the S# and none for bytes nothing created; validation clean on the AMD Radeon 880M and the RTX 5070 Laptop. The layer change it needed: pipelines whose shader declares set-0 bindings (graphics and compute) were refused; their layout now holds the device's bindless set 0 and replay binds it at every draw and dispatch. Still open: shaders of the game itself (direct heap indexing, unbounded tables, static samplers, descriptors the game writes itself), no host D3D12/WARP byte comparison (the reference is computed on the CPU). Formats, mips and arrays through a PSO: `d3d12_m2_formats_test` samples an 8×8 BC1 and an 8×8 BC7 texture with 4 mips each and an R8G8B8A8 2D array of 3 slices through the same path (every subresource uploaded at its `GetCopyableFootprints` layout, POINT samplers with `MinLOD = MaxLOD` pinning each level, a view from mip 1, two array views read by `ps_m2_array.asm`); all 12 cases match the CPU with error 0, validation clean on the RTX 4070 Ti SUPER, with no layer change needed |
| **M3** | Depth/stencil, multiple render targets, resource barriers, render passes, occlusion queries, fences, 2–3 frames in flight; run the game's first frames | Real frame structure and the first game code | The game reaches its first rendered frame (loading screen/menu or whatever the executable does) or stops on a single unsupported call with a clean report. **Synthetic part met** by `d3d12_m3_frame_test`: through the proxy, D3D12 API only, six frames through a ring of three slots (three frames in flight; each slot has its own allocator, list, targets, depth buffer, query and fence, and a frame waits with `SetEventOnCompletion` + an event for the fence its slot signalled three frames earlier before reading back and recording the slot again). Pass 1 draws three rectangles from root constants with translator-output shaders (`vs_m3.asm`, `ps_m3.asm`, hand-written RDNA2) through `OMSetRenderTargets` into two render targets (R8G8B8A8_UNORM and R32_FLOAT) and a `D24_UNORM_S8_UINT` depth/stencil buffer (the layer substitutes D32_SFLOAT_S8_UINT on the Radeon; the RTX has it natively): depth LESS with write, stencil REPLACE with references 1 and 2, then a draw with no depth test whose stencil test is EQUAL; before the far rectangle, a single-output probe (`ps_m3_query.asm`) uses the same depth/stencil tests with color, depth and stencil writes disabled, inside `BeginQuery`/`EndQuery(OCCLUSION)`, and `ResolveQueryData` writes its sample count to a readback buffer. The actual far-rectangle draw still writes both render targets with depth/stencil writes enabled. Resource barriers move the first target RENDER_TARGET → PIXEL_SHADER_RESOURCE\|COPY_SOURCE and the second to COPY_SOURCE (and back at the next frame, from resources created in those states). Pass 2 samples the first target through an SRV in a descriptor table (the M2 path) with `ps_m3_post.asm` into a third target. Every pixel of all three targets (the float one bit for bit) matches the CPU reference with error 0 in each of the six frames, and the single-output probe's occlusion count equals the exact visible area of the far rectangle in each frame (3555 samples over the six frames). The original version was validation clean (including synchronization validation) on the AMD Radeon 880M and the RTX 5070 Laptop; current probe/isolation results are recorded below. @whattheflop's run on #1584 (GeForce RTX 3080 and a Ryzen 7 9800X3D Radeon iGPU) sampled the fences with nonblocking reads after each submit and saw 1, 2, 3, 1, 2, 3 slots still incomplete on NVIDIA and 1, 1, 1, 1, 1, 1 on AMD: up to three slots in flight on NVIDIA, one on AMD (sampled fence completion; GPU execution overlap was not measured). **Still open for M3:** render passes (`BeginRenderPass`, D3D-27) are not exercised, nor `BINARY_OCCLUSION`, occlusion during pipeline statistics (D3D-162), MSAA or depth/stencil copies and read-only DSV states in this test; the game's own first frames need the executable |
| **M4** | Compute PSOs, UAVs, storage images, indirect dispatch, copy queues, async streaming | Asset loading and GPU compute | The world's streaming path runs without device loss; compute results match a CPU/reference check. **Synthetic part met** by `d3d12_m4_compute_test`: through the proxy, D3D12 API only, with translator-output compute shaders (`cs_m4_fill.asm` and `cs_m4_indirect.asm`, hand-written RDNA2). A COPY queue streams an input buffer in (`CopyBufferRegion` from an UPLOAD buffer into a DEFAULT buffer created in COMMON), `Signal`; a COMPUTE queue `Wait`s on it and runs one list: a compute root signature (four root constants and a descriptor table of five UAVs: a structured buffer, three raw buffers and an `R32_UINT` `RWTexture2D`), a compute PSO and `Dispatch` (256 threads) that fills the structured buffer and the image from the input and writes the arguments of the next dispatch into a raw buffer; barriers (the argument buffer to `INDIRECT_ARGUMENT`, UAV barriers on the buffer and the image); `ExecuteIndirect` with a dispatch command signature running a second PSO for the 3 groups the first dispatch asked for, which reads the buffer and the image back (`image_load`) and writes a raw output; then everything to COMMON and `Signal`. A COPY queue `Wait`s on that fence, copies the five results (the image through `CopyTextureRegion`) to a readback buffer, and the CPU waits with `SetEventOnCompletion`. Each D3D12 queue has its own `VkQueue` where the family has two (both test GPUs); all 1,924 result words match the CPU reference with error 0; validation (with synchronization validation) clean on the AMD Radeon 880M and the RTX 5070 Laptop. Layer gaps this test found, closed with it: buffer UAVs (structured and raw) were not written (`CreateUnorderedAccessView` stubbed them, now a V# in the slot); `ExecuteIndirect` of a dispatch ran whatever pipeline and root arguments the command buffer had bound instead of the list's (now recorded with the command, as `Dispatch` does); an image created in `UNORDERED_ACCESS` and first used by a shader was zero-filled by the first barrier or copy that named it, losing the shader's writes (now moved to GENERAL, zeroed, by the first submission after its UAV view is created that is not on a copy queue and dispatches or draws; every other queue's next submission waits for that one); and a queue submitted before the queue whose fence it waits on, when a resource's layout is handed over between them (the layout tracker followed submission order, so a copy list was replayed against the layout from before the compute list ran and the run stopped on a validation error): such a batch is now held on the CPU until a Signal that satisfies its Wait has been submitted, and recorded in dependency order (section 4.4, "Wait before Signal across queues"; `d3d12_queue_handover_test`, every queue-type and submission order, exact against the host runtime). **Still open for M4:** the storage-image initialization is tied to the first shader-running submission, not to the list that uses the image, so a list that only reaches an image through a barrier or copy before any dispatch or draw is submitted still zero-fills it first (as before), and an image used by a shader only on a queue that never runs one of its own dispatches or draws first relies on another queue's submission; buffer SRVs and CBVs as V#s (typed buffer UAVs and UAV counters are done: `d3d12_buffer_uav_test`, section 3.4 "Buffer UAVs"), `ExecuteIndirect` signatures that change root arguments or use count buffers for dispatches, and async streaming itself and the world's streaming path, which need the game |
| **M5** | Mesh shaders, `ExecuteIndirect`, bindless-table pressure, multi-threaded command lists, pipeline cache | World rendering | A stable open-world frame at any frame rate; then profile and optimise |
| **M6** | Ray tracing/XDXR: standard AS builds or the shipped `XboxBvhBuilder` compute path; BVH node format; the builder's 52 console `FSSM` programs (all 52 translate and pass `spirv-val`: [embedded_shaders.md](embedded_shaders.md)) | RT shadows and any RT effects | RT output matches a reference; the builder's shaders run through the `FSSM` translator or a substitute; no unknown XDXR call remains |
| **M7** | HDR, VRS, `PresentX` planes (2-plane HDR + GameDVR compose, `ImmediateThresholdPercent`), resize, alt-tab, `SuspendX`/`ResumeX`, frame pacing and `GetFrameStatisticsX` | Presentation polish | HDR and resize work on the target display; no validation errors over a long run. **Partly met:** `ImmediateThresholdPercent` (late frame flipped at once through `VK_KHR/EXT_swapchain_maintenance1` IMMEDIATE/FIFO_RELAXED, section 4.5; checked on the fake-clock decision and on the Vulkan present mode, 0 VUIDs in our runs with `LEONIDA_VK_VALIDATION=1` on two GPUs, no tearing observed on a display; the console's real semantics are unmeasured). **Plane compose** (section 4.5): 1-2 planes scaled from a source rectangle to a destination placement, point or bilinear, plane 1 alpha-blended (premultiplied or straight), filtered and blended in linear light, SDR (sRGB or gamma 2.2) and HDR10 (PQ) planes converted to the swapchain, SDR white from Windows' setting when HDR10 is active (else 203 nits), checked against a CPU model on two GPUs with 0 VUIDs (exact for point filtering, within 1-2 levels for bilinear, alpha, tone map and PQ conversions); the layouts, flag values and alpha convention are ours, the console's plane semantics are unmeasured, and the public HDR sample's second plane is the GameDVR copy, not an overlay. HDR10 swapchain (ST.2084 surface): verified on an RTX 5080 with an HDR10 display by @JavierBlanco23 in #2069, not on AMD (our test displays list no ST.2084 surface); HDR10 on an SDR swapchain is tone mapped (identity below 0.8 of SDR white, Reinhard shoulder above, then clipped per channel). `SuspendX`/`ResumeX`, frame pacing and a minimal `GetFrameStatisticsX` exist; VRS, alt-tab, an AMD HDR10 display run are open |

**M3 occlusion portability (D3D-209).** Vulkan's [occlusion query specification](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#queries-occlusion)
permits a count multiplied by the number of written color outputs, even with `VK_QUERY_CONTROL_PRECISE_BIT`.
The original two-output query returned twice the CPU area on Intel UHD (Windows driver 31.0.101.4146),
while NVIDIA RTX 3050 6GB Laptop (617.14) returned the area itself. Synthetic direct Vulkan and native D3D12
controls also returned 8192 on Intel for a 64×64 quad with two outputs, versus 4096 with one;
NVIDIA and D3D12 WARP returned 4096 in both cases. This does not establish duplicated replay commands.
The test now measures a single-output, write-free probe, keeping its exact CPU oracle and the original
MRT rendering and pixel checks. It does not divide application query results or change the runtime backend.

`d3d12_m3_first_pass` runs the same executable with `--first-pass-only`: all six frames, MRT pixels,
depth/stencil, barriers, queries and slot fences are still checked, and the third target must stay clear.
It isolates post-pass pipeline creation and execution: it skips creating the post-process PSO and recording
the sampled draw. All five FSSM files and their shader-library entries, including `vs_m2` and `ps_m3_post`,
are still required at startup. The second-pass root signature and descriptor heaps are still created,
so missing post-pass shaders or failed descriptor setup still fail this mode. `d3d12_m3_frame` remains
the full test. The first-pass mode passes with validation on both GPUs above (3555 samples). The full test passes on NVIDIA;
on this older Intel driver it still fails validation because `ps_m3_post` declares `Int64` without
`shaderInt64` support. The full test continues to report that separate capability issue.

### 6.1 Minimum API set for the first frame

M0–M3's synthetic tests need only this subset. The game itself may need more as soon as its first frame does;
everything else can be absent or a clean failure:

- `D3D12XboxCreateDevice` and `CheckFeatureSupport`; the measured device overrides (heap start handles in RAX;
  slot 76 texture creation if the game uses it; slot 62, compute-PSO creation in the builder's ABI, is not needed
  for the first frame); one direct queue, one allocator, one command list, one fence;
- DXGI factory/adapter/output enumeration; frame events and `PresentX` (or `GetBuffer`/`Present` if the game creates a swapchain);
- a root signature with root constants and one descriptor table, masking `0x03000000` on a copy; one VS+PS PSO and
  an input layout;
- a shader-visible CBV/SRV/UAV heap and a sampler heap; `CreateShaderResourceView`, `CreateSampler`,
  `SetDescriptorHeaps`, `SetGraphicsRootDescriptorTable`;
- one committed vertex/index buffer, one texture, upload through `Map`/`Unmap` or `WriteToSubresource`,
  `CopyBufferToImage`;
- resource barriers, RTV/DSV, `OMSetRenderTargets`, `Clear*View`, `DrawIndexedInstanced`, `PresentX`/`Present`;
- the runtime side: descriptor hash table, bindless set 0, push constants (`s0..s31`, heap base, code base),
  vertex input from the PSO layout.

Not needed for the first frame: compute, mesh, geometry, ray tracing, VRS, PresentX plane compose, reserved/tiled resources,
heap aliasing, indirect draws, multiple queues, pipeline libraries. (`PresentX` itself is needed; only its plane compose, HDR and statistics are not.)

**Why this order.** M0–M2 isolate the layer from the game: they exercise every contract piece with known data.
M3 is the first point where the game's own root signatures, PSOs and descriptors appear; expect the trace to
rewrite parts of section 3.5. M4/M5 are ordered by dependency: assets must stream before the world can draw, and
the corpus's 9,282 mesh shaders (and, very likely, indirect draws) are world-rendering features. M6 is last
because the game ships its own BVH builder; the layer may only need to run its compute work, not implement a full
DXR path.

**First-boot minimum.** Strictly, the first boot needs M0–M3 plus whatever the game does before its first frame.
If the first frame is a loading screen with a simple UI draw, M3 may be enough; if it is the world, M5 is the real
minimum. The trace decides, not the milestone labels.

## 7. Prior art and measured Xbox ABI

A GPL-2.0-only fork of AnyPS5 (`americas-recomp`, branch `xbox-series`) recompiles the same GTA VI Xbox Series
shaders and has run the package's DLLs against a PC D3D12 device behind an Xbox-ABI proxy. Its measurements are
prior art for this design. The findings below are paraphrased; no code or long passages are copied. They were
verified there by running the package DLLs (`XS_TRACE=1`, per-slot hit counters) or by static import analysis, not
independently re-measured here. **Basis:** *static imports* means PE import-table analysis; *run* means the package
DLLs were executed against a PC D3D12 device through the Xbox-ABI proxy.

Other Xbox Series compatibility-layer efforts have since measured overlapping parts of the same surface — running
the package's own console DLLs and shader pack, retail titles, or a console-compiler output. Their findings are
folded into the table below with the measurement basis noted per row; nothing is re-derived from their code.

| Finding | Basis and link | Consequence for this design |
|---|---|---|
| Across the 16 package DLLs there are only 6 Xbox-specific static imports: `xmem.dll` → `XMemAlloc`, `XMemFree`, `XMemGetWorkingSetStatistics`, `XMemVirtualQuery`; `d3d12_xs.dll` → `D3D12SerializeVersionedRootSignature`, `D3D12CreateRootSignatureDeserializer`. Both `d3d12_xs` names exist with the same names in the PC `d3d12.dll`. The other 492 functions are standard Windows and 113 are shipped in the package. Argument shapes for the less common `xmem` calls come from caller disassembly; some attribute bits are inferred. | *Static imports* — [PLATFORM_SURFACE.md](https://github.com/realdody/americas-recomp/blob/xbox-series/docs/xbox/PLATFORM_SURFACE.md) | Confirms the middleware surface is tiny. The repo's `xmem` shim already covers all four; `d3d12_xs` still forwards only the two root-signature functions. The device/queue/command-list surface belongs to the executable. |
| `D3D12XboxCreateDevice(adapter, params, riid, out)` returns a PC `ID3D12Device` behind the Xbox-ABI proxy. The params struct is 24 words and is only logged; the signature comes from public documentation, not a header. | *Run* — [platform README](https://github.com/realdody/americas-recomp/blob/xbox-series/tools/xbox/platform/README.md), [PLATFORM_SURFACE.md](https://github.com/realdody/americas-recomp/blob/xbox-series/docs/xbox/PLATFORM_SURFACE.md) | Corrects "device entry point unknown": the entry point is named. The params/flag layout remains **Open**. Do not assume `D3D12CreateDevice`. |
| Xbox texture creation is device slot 76 (heap properties / heap flags / description / initial state / clear value / iid / out), not `CreateCommittedResource`. The description is 56 bytes plus extension words; its layout field is `0x101` (tiled) or `0x1` (row-major buffer), and the PC proxy forwards it to `CreateCommittedResource` slot 27 with a driver-chosen layout. The FidelityFX upscaler creates 10 default-heap resources this way. | *Run* — [PLATFORM_SURFACE.md](https://github.com/realdody/americas-recomp/blob/xbox-series/docs/xbox/PLATFORM_SURFACE.md) | Adds a required Xbox-specific device method and a non-public layout value. `0x101` must not be passed to the public layout enum; the tiled data layout/detile path remains **Open**. See section 4.2. |
| Device slot 14 is the public `CreateDescriptorHeap`; the returned heap is wrapped because `GetCPUDescriptorHandleForHeapStart` / `GetGPUDescriptorHandleForHeapStart` (heap slots 9/10) return the 8-byte handle in RAX on the Xbox build. The PC call returns it through a hidden pointer in RDX, so the proxy special-cases the return. | *Run* — [PLATFORM_SURFACE.md](https://github.com/realdody/americas-recomp/blob/xbox-series/docs/xbox/PLATFORM_SURFACE.md) | Adds a return-convention quirk to the descriptor-heap ABI. A generic IDL/sret thunk will corrupt the handle. See section 3.4. |
| Xbox root-signature descriptors carry flags `0x03000000`. The PC serializer rejects them with `E_INVALIDARG`; the proxy clears the high byte on a copy. The v1.0 `D3D12SerializeRootSignature` path keeps flags at +32, and `0x3000001` also fails on the PC runtime. | *Run* — [PLATFORM_SURFACE.md](https://github.com/realdody/americas-recomp/blob/xbox-series/docs/xbox/PLATFORM_SURFACE.md) | Narrows the root-signature reflection risk. Mask the high byte on a private copy; the meaning of the bits remains **Open**. See section 3.5. |
| The BVH builder's constructor creates 53 compute pipelines through device slot 62, called with a compute-PSO shape (a plain `D3D12_COMPUTE_PIPELINE_STATE_DESC`, two zero words, iid, out). On the PC interface the prior art forwards to, slot 62 is `CreateStateObject` (wrong arity), so the proxy overrides it. | *Run* — [PLATFORM_SURFACE.md](https://github.com/realdody/americas-recomp/blob/xbox-series/docs/xbox/PLATFORM_SURFACE.md) | Corrects the assumption that the whole device vtable is public order. Slot 62 must be compute-PSO creation for the builder, not a state object. Slots 1/15/18/19/27/38 did match the desktop order. |
| A reusable `xbox_device_proxy.h` forwards every slot to a PC object, counts hits per slot and logs first calls with `XS_TRACE=1`; the observed device slots include 1, 14, 15, 16, 18, 19, 27, 38, 62 and 76. Their `xgr` stand-in covers 48 of 398 methods; Xodus (LGPL-2.1) covers 152 of 396 via IDL-generated stubs. | *Run* — [platform README](https://github.com/realdody/americas-recomp/blob/xbox-series/tools/xbox/platform/README.md), [PLATFORM_SURFACE.md](https://github.com/realdody/americas-recomp/blob/xbox-series/docs/xbox/PLATFORM_SURFACE.md) | Confirms the trace-first approach and shows the long tail. We adopt the *method* (per-slot counters, first-call logging), not the code. |
| The BVH builder's 53 compute shaders are Xbox DXIL with `@xbox.*` intrinsics; 44 of 53 fail the PC D3D12 shader compiler, 22 have been lowered to standard DXIL and accepted as pipelines, and 31 use raw 64-bit GPU addresses. | *Run* — [PLATFORM_SURFACE.md](https://github.com/realdody/americas-recomp/blob/xbox-series/docs/xbox/PLATFORM_SURFACE.md) | A separate translation path from the FSSM RDNA2 corpus when measured on the **bitcode**. See section 4.8 and the paragraph below. Measured later on the console `XBCH` record of the same containers: 52 programs, 51 pass `spirv-val` through the FSSM translator ([embedded_shaders.md](embedded_shaders.md)). 52 programs, 42 pass `spirv-val` through the FSSM translator ([embedded_shaders.md](embedded_shaders.md)). |
| On the console's own shader compiler, compute user parameters go into user SGPRs `s4`… in root-signature order while each fits whole (root constant = 1 dword, root descriptor = 2, descriptor table = 1 dword holding the 32-bit address of its first descriptor); `s0`..`s3` are the driver's. From the first parameter that does not fit, it and all after it go into a packed array addressed by a 40-bit pointer in `s[0:1]` (constant 4 B, descriptor 8 B, no alignment). Thread-group ids follow the user SGPRs. | *Console-compiler output, sibling layer* | Narrows question 3 for the compute stage: the inline-from-`s4` model matches this repo's census (lowest CS pointer SGPR `s4`), and the spill target is named (the array at `s[0:1]`), not guessed. VS/PS/MS bases were not measured there; verify against the game's own signatures. |
| The command list's inline records: a `Dispatch` is `0x4002C400` followed by x, y, z (16 B); a 20-byte record `0x4003CC00` stands in for console slot 80; the pipeline record is a 28-byte copy of the PSO object's +0x10..+0x2B; a root parameter starts from the header word at list +0x344+4·i (the high half carries n−1 for a multi-constant write), then a descriptor is 12 B, one constant 8 B and n constants 4+4n B. | *Console-DLL run, sibling layer* | Supplies the record opcode for the "five-dword packet" this repo inferred at slot 80 and the header encoding for parameter groups; consistent with our own measurements of +0x18/+0x28/+0x30/+0x140/+0x344/+0x544. |
| Mesh (NGG) inputs measured across the corpus: `s3[27:24]` = wave index in the group, `v5` = group·128 + thread, `s16`–`s24` = user data. | *Corpus measurement, sibling layer* | Confirms the `s16` mesh base of §3.5's census and the `v5` group numbering already used for the mesh stage. |
| Console unified memory can be mapped GPU-side without copying: a region of at most 4,095 MB at a 4 GiB-aligned address, committed up front, opened through `ID3D12Device3::OpenExistingHeapFromAddress` (a partial commit and a full 4 GiB are rejected). The low 32 bits of a console address then index the buffer directly. | *Measured on PC D3D12, sibling layer* | A concrete recipe for §3.3's single address space: game addresses as offsets into one buffer that shaders reach through `PhysicalStorageBuffer64`. |
| The title physical-page calls use 64 KiB pages; mapping a run of consecutive pages returns a contiguous range inside the unified region, and scattered or explicitly addressed mappings are refused. | *Console-DLL run, sibling layer* | Sizes the page granularity the `xmem` shim should answer before the executable's real calls are known. |
| On a retail UE5 title the graphics command list takes console slot 83 for `CopyTextureRegion`, 84 for `CopyResource`, 26 for barriers, 127 for direct mesh dispatch, 59 for a second indirect/mesh dispatch path, and 96 for an async-compute budget call that accepts six zero words. | *Retail-title run, sibling layer* | Narrows question 2: mesh dispatch is not at the PC position (GCL6 slot 79) on the console, and the copy/barrier/budget slots add to the shifted groups already measured here. |
| The console `d3d12_xs.dll` also exports `D3D12XboxCreateSyntheticDevice` and `D3D12XboxSetProcessDebugFlags`; sibling console modules include `xg_xs.dll` (`XGCreateTexture2DComputer`, `XGComputeOptimalSwizzleMode`), `AcpHal.dll` (`ApuAlloc`, `ApuFree`, `ApuMapVirtualAddress`, `ApuIsVirtualAddressValid`, `AcpHalCreate`, `AcpHalAllocateShapeContexts`, `AcpHalReleaseShapeContexts`) and `dstorage_xs.dll` (console DirectStorage); `XAudio2_9.dll` ordinal 1 is `XAudio2Create` and `GameInput.dll` ordinal 1 is `GameInputCreate`. | *Export inventory, sibling layer* | Names the console-only modules the loader's shim registry should expect beyond `xgameruntime`/`d3d12_xs`/`xmem`/`gameinput`. |
| Console `xaudio2_9.dll` voices accept the console formats: XMA/XMA2 and an Opus voice format `0x704F`; a `CO_E_NOTINITIALIZED` from the system XAudio2 is retried after `CoInitializeEx` on the calling thread. | *Console-DLL run, sibling layer* | Forwarding console `XAudio2_9.dll` to the system XAudio2 2.9 works; the retry rule and the format remapping are the quirks to reproduce. |
| A retail title boots to its menu and lobby through the same shim-layer route (native x64 code, console-library stand-ins); its console-only library set adds `chad` (`XDsp*`), `xapu` (`XApu*`), `xcrdapi` and `*legacy` DLL variants to the list. | *Retail-title run, sibling layer* | Confirms the architecture end to end on retail code and extends the console-DLL checklist. |

### 7.1 What this confirms, corrects and narrows

**Confirms.**

- The 16 middleware DLLs need only four `xmem` functions and the two `d3d12_xs` root-signature functions. Our
  earlier statement that the bundled DLLs only import the two root-signature calls was right but incomplete; the
  four `xmem` imports complete the static surface. The repo's `xmem` shim already implements all four.
- Device slots 1 (AddRef), 15 (`GetDescriptorHandleIncrementSize`), 18 (`CreateShaderResourceView`),
  19 (`CreateUnorderedAccessView`), 27 (`CreateCommittedResource`) and 38 (`GetCopyableFootprints`) match the
  desktop order. Device slot 14 is the public `CreateDescriptorHeap`; only the returned heap's start-handle return
  is wrapped. The public chain is a usable base for those methods, not a guess.
- The BVH builder receives the device and command lists from the caller; prior art runs it exactly that way.

**Corrects.**

- **Device creation.** `D3D12XboxCreateDevice` is the entry point. The 24-word params struct is only logged, so
  the flag/layout meaning is still unknown; the name and call shape are no longer unknown. Prior art returns a PC
  D3D12 device behind the proxy; this design returns the Vulkan-backed `Device` behind the same call shape.
- **Vtable.** The Xbox device is not uniformly the public order. Slot 76 is an extra texture-creation method,
  slot 62 is called with a compute-PSO signature where the PC interface has a state-object method, and the
  descriptor-heap start handles use a different return convention. Implement the public chain plus explicit
  measured overrides; do not trust a single generic thunk for every slot.
- **Texture layout.** Xbox `0x101` tiled is not a public `D3D12_TEXTURE_LAYOUT` value, and slot 76 is the texture
  path. Our earlier `D3D12_RESOURCE_DESC::Layout` discussion covered only the public enum; it now includes the
  Xbox value and slot 76.
- **Root signatures.** The `0x03000000` flags must be masked on a copy before the host serializer/deserializer.
  Our earlier "forward the blob to the host" was too simple; the PC serializer rejects the Xbox bits.
- **BVH builder.** Its console programs are `FSSM`; the bitcode in the same container is the representation the
  PC compiler consumed. "Run the builder's dispatches" therefore takes the same FSSM → SPIR-V path as the game,
  with one rule still missing ([embedded_shaders.md](embedded_shaders.md)).

**Narrows.**

- Presentation slots and struct layouts (`PresentX` and plane parameters; call shapes are known from public samples, §4.5), command-list extensions, and queue/fence creation slots remain
  unknown; the prior art explicitly stops before them.
- Descriptor start-handle ABI is known; descriptor bytes and heap increments are still **Open**. Slot 15 is
  forwarded unchanged, but the actual increment values were not measured.
- Texture creation ABI and the `0x101` value are known; the tiled data layout, detile path and DCC remain
  **Open**.
- The device params (command-queue ring size, scratch memory, debug flags) are unused and their meanings are
  still unknown.
- The `xgr` 48/398 and Xodus 152/396 coverage numbers show how large the long tail is. They do not change the
  plan: implement what the game calls, in the order it calls it.

### 7.2 The BVH builder's shader path

The builder's 52 compute shaders are a second *corpus*, not a second shader front end. Each container carries an
LLVM bitcode blob and an `XDAT`/`XBCH` record; the bitcode is what the PC D3D12 compiler experiment consumed
(44 of 53 rejected, 22 lowered), while the console runtime executes the `XBCH` record. That record decompresses
to `FSSM`, the same container the game's own compute shaders use: 52/52 unpack, translate and pass
`spirv-val` with `tools/rdna2spirv.py`; [embedded_shaders.md](embedded_shaders.md) lists the five rules that
closed the last ten. The raw 64-bit addresses are the translator's existing raw-address path, so no
address-to-resource table is needed on top. A port that runs `XboxBvhBuilder` therefore needs the FSSM translator,
not a DXIL front end (a substitute builder remains the alternative).
Correctness is still unverified on a GPU and the call sites need the executable (RT-5).

### 7.3 Licence

`americas-recomp` / AnyPS5 is **GPL-2.0-only**. GPL-2.0-only is not compatible with this repository's
GPL-3.0-or-later, so no code, headers, generated stubs or other implementation text from that tree may be copied
into Leonida. This section records ABI/format facts and links only; the designs in this document are independent.
Xodus is LGPL-2.1 and is cited only for its coverage number, not as a code source. The same line holds for the
other compatibility-layer efforts whose measurements are folded in above: facts and numbers only, paraphrased —
nothing is copied from their trees.

## 8. Known vs unknown

### 8.1 Known

- The public D3D12 ABI and method order (Windows SDK `d3d12.h` 10.0.26100.0); the Xbox public reference lists the
  same base interfaces and method names under `d3d12_xs.h`, but the measured Xbox device ABI is only mostly the
  public order: slots 1/15/18/19/27/38 matched, while slots 62 and 76 and the descriptor-heap start-handle return
  did not (section 7).
- The translator/runtime shader contract, and that it works on a real GPU for the tested paths.
- The package evidence in section 2 and Appendix A: the BVH builder's Device11/CommandList4/6/8 signatures and
  XDXR types, the FSR IIDs, the other DLLs' lack of `d3d12_xs` imports, `gameos.xvd`'s encrypted payload, and the
  absent executable.
- The prior art's measured Xbox ABI (section 7): 6 Xbox-specific static imports across the 16 DLLs,
  `D3D12XboxCreateDevice`, device slots 62/76, descriptor-heap start handles in RAX, root-signature flags
  `0x03000000`, and the BVH builder's container format (a DXIL bitcode blob plus the console `XBCH` record).

### 8.2 Unknown / open questions

| # | Question | How to resolve |
|---|---|---|
| 1 | The device entry point is now named `D3D12XboxCreateDevice(adapter, params, riid, out)`. What do its 24-word params and flag bits mean? | Public documentation and the executable's call sites. Log the words and accept any flag bits; do not treat a wrong params layout as fatal until the fields are understood. |
| 2 | Which Xbox device/heap/queue/command-list slots differ? Prior art measured device slots 62 and 76 and descriptor-heap slots 9/10; slot 14 is the public `CreateDescriptorHeap`, with only the returned heap's start-handle return wrapped. **Partly measured** with the FSR DLL ([d3d12_command_objects.md §3.5a](d3d12_command_objects.md)): device slots 14, 15, 16, 18, 19, 27, 38, 62 and 76 at their expected shapes (24 in the code, not called), slot 67 a console-only pipeline call; command-list slots 14, 15, 25 and 26 at their PC positions, then shifted (+4 from `SetDescriptorHeaps` at 32, +2 at `ExecuteIndirect` at 61), with console-only methods at 85 and 141. The BVH builder's call sites add the rest of the +4 root group (37, 39, 43, 45) and console slots 71, 73, 80, 146 (the GCL7 `Barrier`) and 148 (an upload allocator whose pointer is also the GPU address) ([d3d12_command_objects.md §3.3, §3.3a](d3d12_command_objects.md)). A second, retail-title measurement adds console slots 83 (`CopyTextureRegion`), 84 (`CopyResource`), 26 (barriers), 127 (direct mesh dispatch — not the PC position, GCL6 slot 79), 59 (a second indirect/mesh dispatch path) and 96 (an async-compute budget call) (section 7). Presentation, queue and fence slots remain unknown. | Trace every `QueryInterface` and every method slot with per-slot hit counters. A wrong slot shows as an immediate crash or wrong argument; compare against the SDK vtable. |
| 3 | How does the console map a root signature to `s0..s31` (order, per-stage visibility, spills above 32 dwords, static samplers, direct-heap base)? | **Compute partly measured** by a sibling project's console-compiler output (section 7): parameters inline in user SGPRs from `s4` in root order while each fits whole; the first that does not fit, and all after it, go into a packed array addressed by a 40-bit pointer at `s[0:1]`; thread-group ids follow the user SGPRs. That matches this repo's CS census (lowest CS pointer SGPR `s4`). VS/PS/MS bases remain open. Static samplers: no sampling instruction was found reading one from a driver table (SHADER-735, section 3.5; 0.66% undetermined); which S# is a static sampler remains open. Dump the game's root-signature blobs with the host deserializer; mask the `0x03000000` flag byte on a copy first; match against the per-shader user-SGPR read sets. If a signature exceeds 32 dwords, reverse the spill pointer from the shader. |
| 4 | Are Xbox descriptor bytes GFX10 T#/S#, and what are the heap increments? Does the game use `Create*View` or build descriptors itself? | Dump a shader-visible heap after the first `CreateShaderResourceView` and after the first draw; compare the bytes the shader loads with the T# parser. The start-handle return ABI is known (RAX); the increment values still need measuring. |
| 5 | What is the buffer/descriptor/code VA range, and what arena capacity is needed? | Log every `GetGPUVirtualAddress` and every descriptor base; track the high-water mark. Choose a capacity; grow at a frame boundary if needed. |
| 6 | Which swapchain/present path is used: DXGI, `PresentX`, or both? | **Partly answered from public samples (§4.5):** GDKX samples use `PresentX` + frame events and no DXGI swapchain (DXGI only for `IDXGIDevice1`→adapter→output). Hypothesis for the game, not observed: same. Still open: vtable slots, `D3D12XBOX_*` layouts, and whether the game also creates a swapchain. Check imports and the first present call. Implement the `PresentX` + frame-event presenter first; keep DXGI swapchain as fallback. |
| 7 | Does the game build acceleration structures through the standard D3D12 calls or through `XboxBvhBuilder`'s own compute path? | Log `BuildRaytracingAccelerationStructure`, `CopyRaytracingAccelerationStructure`, `Emit...PostbuildInfo`, and the builder's command-list method calls on the first RT frame. The builder's container carries a DXIL bitcode blob and a console `XBCH` record; the record is `FSSM` and all 52 programs pass `spirv-val` ([embedded_shaders.md](embedded_shaders.md)). If the builder path is used, translate the `XBCH` record rather than the bitcode. |
| 8 | What are the layouts of `D3D12XBOX_XDXR_STREAM_DESC`, the batched build desc, and the offline statistics desc? | **Measured** from `XboxBvhBuilder.dll`'s reads ([xdxr.md](xdxr.md)): `BATCHED_DESC2` (`0x50`) and `XDXR_STREAM_DESC` (`0x10`) with every field's offset, and the stream records' offsets, types and defaults. Still open: the record names, and the offline statistics layout, which the DLL never writes (it rejects that postbuild type). |
| 9 | Xbox texture creation is device slot 76 with layout `0x101` (tiled) or `0x1` (row-major). What is the tiled data layout, and does the game upload console-tiled or DCC data? | Trace slot 76, `CopyTextureRegion` and the resource descs. If the game uploads console-tiled data, add a detile step or a matching layout. |
| 10 | Does the game use binning, DCC, VRS, wave MMA, stream output, tessellation, or amplification shaders? | **Shaders: answered** by `tools/feature_scan.py` over all 26,503 (1.0.2.1): no hull, domain or amplification shader (DXIL program kinds: 2,236 VS, 12,101 PS, 195 GS, 2,689 CS, 9,282 MS); no `WAVE_MMA` or `SHADING_RATE` in any SFI0, and no `v_mfma`/`v_wmma`/`v_smfmac` in the disassembly; no output-signature element on a stream other than 0 (all 195 GS write stream 0 only) and no GDS / `ds_ordered_count` / GWS instruction, so no stream-output path. Also from SFI0: 10 PS need ROVs (pixel interlock), 30 shaders `TILED_RESOURCES` (see #18), 685 `ATOMIC_INT64_ON_TYPED_RESOURCE`. Binning and DCC are not shader-visible; they stay open for the PSO/stream subobjects. |
| 11 | What should `CheckFeatureSupport` answer for UMA, resource binding tier, mesh tier, RT tier? | **Implemented** (D3D-11). Reports truthful host architecture (discrete desktop GPU: non-UMA), ResourceBindingTier 3 (descriptor indexing / bindless support), 48-bit GPU virtual address support matching the arena, and maps `D3D12_FEATURE_FORMAT_SUPPORT` to Vulkan format properties plus multisample render-target/resolve support and one quality level per supported sample count. |
| 12 | Does the game rely on async compute or copy queues for correctness? | Trace queue creation and cross-queue fences. If only performance, serialize on one queue for boot. |
| 13 | Is 144 bytes of push constants available on all target GPUs? | **Resolved / Measured** (D3D-14). Checked at device creation in `runtime/shims/d3d12_vulkan_device.cpp`. Measured across the test fleet (#166): NVIDIA (256 B), desktop AMD (256 B), and Apple M1/MoltenVK (4096 B) satisfy 144 B natively. A Radeon 680M on the Windows driver 22.40.84.06 and the earlier Intel Arc entries expose only the core minimum 128 B (`maxPushConstantsSize == 128`), but an Arc B580 on Windows 11 (driver 32.0.101.9175) reports 256 B (#166), so the 128 B figure for Arc depends on the OS or driver; an Arc result on Linux (Mesa ANV) would settle which side it is. `d3d12_vulkan_device_test` accepts either answer and checks that the fallback flag matches it. Below 144 B, the device logs a warning and marks `push_constants_need_fallback`, routing the 16-byte tail (dwords 32..35: heap base and code base) to a uniform-buffer fallback. |
| 14 | Are there PSOs with two code-base stages? | Count from the PSO/shader hash table and the index metadata. If yes, implement per-stage code-base slots (section 5.2). **Per stage, measured** (`tools/feature_scan.py`, `s_getpc_b64` kept for data rather than a far branch): 54 VS, 93 PS, 266 MS, 263 CS, 0 GS; the translator's `uses_code_base` gives the same counts (with LLVM 23 that needs #223, without which these 1,471 shaders are rejected). So VS+PS and MS+PS pipelines with two code-base stages are possible; whether any exists depends on which shaders a PSO pairs, which is in the executable, not in `shaders.rpf`. |
| 15 | Is `ExecuteIndirect` used with signatures Vulkan cannot express directly? | Trace `CreateCommandSignature` and `ExecuteIndirect`; implement simple signatures first, expand the complex ones with compute. The simple signatures are D3D-17 (#313). |
| 16 | Does the game use append/consume UAV counters or typed buffer formats the translator does not model? | **Shaders: answered** by `tools/feature_scan.py` over all 26,503 (1.0.2.1, disassembled with LLVM 21.1.8 for gfx1013; SHADER-65). **No append/consume counters:** none of the 445,394 PSV0 binding records is a structured UAV with a counter, and there is no `ds_append`/`ds_consume`/GDS instruction. Typed buffers are declared in 5,359 shaders as SRVs (17,328 records) and in 2,177 as UAVs (4,005). **MTBUF** (format in the instruction): 52,890 loads in 10,278 shaders, all in five formats the translator decodes (`16_16_SNORM` 24,881, `8_8_8_8_UNORM` 19,835, `8_8_8_8_UINT` 5,099, `2_10_10_10_UNORM` 2,648, `16_16_16_16_UINT` 427), and no MTBUF store. **MUBUF** `buffer_*_format_*` (format from V# word 3): 90,316 accesses (84,908 loads, 5,408 stores) in 11,591 shaders; word 3 comes from a scalar load in 88,924 of them, is computed in 1,390, and is a literal in 2 (FORMAT 0, INVALID, which reads 0 on the hardware and in the translator). So the shaders fix no MUBUF format: the descriptors the executable writes decide it. Still open: which formats those descriptors carry; the runtime can log a V# format outside `Gen.FORMATS` (those read 0 today) when the descriptor is created or copied. Trace UAV descs and shader `buffer_*` ops; extend the translator/runtime together. **Runtime side done, console side open:** the layer writes typed buffer V#s and a counter V# (a hypothesis for where the shader finds it, section 3.4 "Buffer UAVs"); whether the game uses either is for the trace. |
| 17 | Do upload/readback resources ever reach shaders (as vertex/index buffers or descriptors), and does the `-heap_base` bias work for them? | Trace `CreateCommittedResource` heap types and every VB/IB/descriptor that points at an upload resource; test the bias with a synthetic draw. |
| 18 | Does the game alias buffers/images in a heap, or use reserved/tiled resources? | Trace `CreatePlacedResource` ranges, aliasing barriers, `CreateReservedResource` and tile mappings. If yes, implement shared memory per heap range or fall back to linear allocations with a clear log. Shaders: 30 declare `TILED_RESOURCES` in SFI0 (`tools/feature_scan.py`), so reserved/tiled resources are likely used. Placed resources are D3D-16 and reserved resources D3D-20 (#313). |
| 19 | What do the `0x03000000` root-signature flag bits mean? | Masking them on a copy works around the PC serializer; the semantics remain unknown. **Origin measured** (section "Root-signature flag quirk"): in the package only `XboxBvhBuilder.dll` sets them, in its 52 compiled `RTS0` parts and unconditionally at `0xc30e` before serialising; the FSR DLL's 29 `RTS0` parts carry 0. Compare against the public flag enum and observe the game's behaviour with and without the mask; check whether the executable's root signatures carry them. |
| 20 | Which of the BVH builder's 52 `FSSM` programs does the game actually use, and do the translated programs match a reference? | Dump the slot-62 compute PSOs on the first RT frame and map each to its container by the `ILDN` name; 51/52 pass `spirv-val` today and the one missing rule is named in [embedded_shaders.md](embedded_shaders.md); compare against the builder's CPU validation (`CpuValidateResultX`) or a captured reference. 42/52 pass `spirv-val` today and the four missing rules are named in [embedded_shaders.md](embedded_shaders.md); compare against the builder's CPU validation (`CpuValidateResultX`) or a captured reference. |
| 21 | Which `Metadata` values and blob format does the console's PIX use for `SetMarker`/`BeginEvent`? The layer reads 0 and 1 as strings and labels 2 (the PC PIX3 blob) `PIX event` (section 4.10). | Log the `Metadata` values the game passes; if it is 2, decode the console blob from the executable's PIX calls rather than assuming the PC WinPixEventRuntime layout. |
| 22 | Does the game ask `CheckFeatureSupport(D3D12_OPTIONS5)` for `RaytracingTier` and switch ray tracing off when it reads `NOT_SUPPORTED`? The layer answers `NOT_SUPPORTED` because full DXR (`CreateStateObject`, `DispatchRays`) is not implemented, even though the XDXR acceleration-structure path the game's own traversal shaders use is ([bvh.md](bvh.md) §7a). On the console, ray tracing is always present, so the game may never ask. | The first-boot trace (`--trace-d3d12`) logs every `CheckFeatureSupport` call and its answer. If the game asks and gates its RT on the tier, decide with the maintainer whether to report `TIER_1_0`/`1_1` for this game while `DispatchRays` stays unimplemented; until then, keep the conservative answer. |

## 9. Risks and non-goals

**Risks.**

- **Xbox ABI deviations.** The device vtable is mostly public but not uniformly: slot 76 adds texture creation,
  slot 62 is compute-PSO creation for the BVH builder, descriptor-heap slots 9/10 return in RAX, and root signatures
  carry `0x03000000`. Use a per-slot proxy with counters; presentation, command-list, queue and fence slots remain
  unknown. A wrong return convention or slot mapping corrupts state rather than failing cleanly.
- **Root-signature mapping.** A wrong `s0..s31` packing produces silently wrong rendering, not a crash. The
  per-shader read-set metadata and a strict mismatch check are the mitigation. The `0x03000000` flag mask is a
  workaround, not an explanation.
- **The arena.** A single BDA arena is required by the shader contract for buffers, descriptor tables and code; the
  game's buffer working set may exceed `maxBufferSize` or PC VRAM. Host-visible arenas are slow; device-local
  arenas need staging and careful upload/readback handling. This is the main performance risk.
- **Hash-table cost.** Every image/sampler access hashes a T#/S# and probes a storage buffer. **Measured (D3D-56, section 5.1)** on an RTX 5080 (driver 621.24): in `DEVICE_LOCAL` VRAM the lookup itself stays fast up to 100k entries (wave-uniform full sweeps `3.3–12.8 us/1M`, divergent hits `24–72 us/1M`), whereas `HOST_VISIBLE | HOST_COHERENT` system memory slows uniform full sweeps 16x–36x and divergent lookups 210x–879x at 10k–100k entries; `bc8e604`'s bounded slot relocation keeps the 100k table at `20.00 MiB` (`19.07%` load, `max_probes=8`) instead of doubling. **The runtime now keeps that table in `DEVICE_LOCAL` memory** and uploads only the slots that changed, from a mapped staging copy, with the copy and its barrier in the command buffer that will read it (D3D-76; 46.2x on a divergent miss at 100k on an RTX 2060 SUPER). Improving slot placement within the 8-probe window is the open optimisation (SHADER-100); Intel, RTX 5070 Ti Laptop and AMD RDNA3 are measured in section 5.1; Apple numbers remain open via `vk_descriptor_bench`.
- **Wave64.** A native wave64 shader needs a 64-wide subgroup in its stage: AMD (Windows driver, RADV on GFX10+) can require
  it for compute, mesh and pixel shaders, NVIDIA cannot (32 only). There, compute and mesh shaders translated with
  `--wave64-emulate` are exact; wave64 pixel, vertex and geometry shaders have no emulation and keep the
  translator's 32-wide approximation (cross-half ops are approximate). This is a known fidelity gap, not specific to
  the layer.
- **Xbox tiling/DCC.** Slot 76 and layout `0x101` are known, but the tiled data layout, detile path and DCC remain
  unknown. If the game assumes console tiling or compression metadata, the layer needs a detile/repack step.
- **BVH builder shaders.** The container's DXIL bitcode is Xbox DXIL that PC drivers reject; its console `XBCH`
  record is `FSSM`, and all 52 programs pass `spirv-val` through the same translator as the game
  ([embedded_shaders.md](embedded_shaders.md)). Translation is not execution:
  there is no GPU reference for any of the 52 yet.
- **Ray tracing.** The game's RT work includes 64 compute shaders that traverse AMD-format BVH nodes, and it ships
  a BVH builder; a standard D3D12 AS path may also exist. If it is used, the layer must build AMD-format nodes, not
  a Vulkan AS.

**Non-goals.**

- Xbox One ESRAM or `d3d12_x` (the Xbox One flavour); this is a Series build.
- D3D11, D3D12 video, protected content, cross-process sharing, meta commands, DRED tooling.
- A general D3D12 compatibility layer for arbitrary applications.
- Any use of game data, keys or decryption.
- Copying code, headers or generated stubs from the GPL-2.0-only prior art; its ABI/format facts are cited with
  links only.
- Replacing the translator's shader contract. The layer adapts to it; changes to it (per-stage code base, extra
  metadata) are coordinated with `tools/rdna2spirv.py`.

## 10. References

Prior art (facts and links only; no code copied; GPL-2.0-only, see section 7.3):

- [americas-recomp, branch `xbox-series`](https://github.com/realdody/americas-recomp/tree/xbox-series)
- [PLATFORM_SURFACE.md: package import surface and measured device ABI](https://github.com/realdody/americas-recomp/blob/xbox-series/docs/xbox/PLATFORM_SURFACE.md)
- [platform tools README: proxy, `XS_TRACE`, xgr, first-run workflow](https://github.com/realdody/americas-recomp/blob/xbox-series/tools/xbox/platform/README.md)
- [GPL-2.0 license text](https://github.com/realdody/americas-recomp/blob/xbox-series/LICENSE)

Public D3D12:

- [D3D12 programming guide](https://learn.microsoft.com/en-us/windows/win32/direct3d12/directx-12-programming-guide)
- [Root signatures](https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signatures)
- [`D3D12_ROOT_SIGNATURE_FLAGS`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_root_signature_flags)
- [Resource binding in HLSL](https://learn.microsoft.com/en-us/windows/win32/direct3d12/resource-binding-in-hlsl)
- [Descriptor heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/descriptor-heaps)
- [Recording command lists and bundles](https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles)
- [Using resource barriers](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12)
- [Enhanced barriers (DirectX spec)](https://microsoft.github.io/DirectX-Specs/d3d/D3D12EnhancedBarriers.html)
- [Executing and synchronizing command lists](https://learn.microsoft.com/en-us/windows/win32/direct3d12/executing-and-synchronizing-command-lists)
- [Fence-based resource management](https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management)
- [Queries](https://learn.microsoft.com/en-us/windows/win32/direct3d12/queries)
- [Mesh shader (DirectX spec)](https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html)
- [Direct3D 12 raytracing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-12-raytracing)
- [`ID3D12Device10`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nn-d3d12-id3d12device10),
  [`ID3D12GraphicsCommandList7`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nn-d3d12-id3d12graphicscommandlist7)
  (the Win32 reference does not yet have pages for `Device11`/`CommandList8`; the Windows SDK header defines them)

Xbox public documentation:

- [Xbox graphics API reference](https://devdocs.xbox.com/reference/graphics/d3d12)
- [Xbox `ID3D12Device::CreateCommittedResource`](https://devdocs.xbox.com/reference/graphics/d3d12/interfaces/id3d12device/methods/id3d12device_createcommittedresource_public)
- [Xbox-GDK-Samples (MIT): SimpleDeviceAndSwapChain, FramePacing](https://github.com/microsoft/Xbox-GDK-Samples)
- [DirectXTK12 wiki: DeviceResources](https://github.com/Microsoft/DirectXTK12/wiki/DeviceResources); [directxtk12modelviewer](https://github.com/walbourn/directxtk12modelviewer) (MIT); SDL2 `SDL_render_d3d12_xbox.cpp` (zlib)
- [`PresentX` mentioned in `XGameStreamingSetResolution`](https://docs.xbox.com/reference/system/xgamestreaming/functions/xgamestreamingsetresolution)
- [GDK release notes mentioning `D3D12XBOX_RESOURCE_FLAG_FORCE_TEXTURE_COMPATIBILITY`](https://docs.xbox.com/announcements/gdk/2510/release-notes-2510)

Vulkan:

- [Descriptor indexing](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_descriptor_indexing.html)
- [Buffer device address](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_buffer_device_address.html)
- [Synchronization2](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_synchronization2.html)
- [Dynamic rendering](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_dynamic_rendering.html)
- [Timeline semaphores](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_timeline_semaphore.html)
- [Mesh shader](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_mesh_shader.html)
- [Fragment shader barycentric](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_fragment_shader_barycentric.html)
- [Fragment shader interlock](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_fragment_shader_interlock.html)
- [Subgroup size control](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_subgroup_size_control.html)
- [Maintenance4](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_maintenance4.html)

## Appendix A: evidence and reproduction

REA was used for the PE import/export and string inventory. The XVD header and entropy scan used local .NET,
because `gameos.xvd` is an XVD container rather than a PE. Nothing was decrypted or executed.

### A.1 `gameos.xvd`

- Size 348,069,888 bytes. `msft-xvd` magic at file offset **0x200**; the 512 bytes before it are a high-entropy
  block, consistent with a signature. Using the standard XVD header layout, the first two dwords after the magic
  are `volume_flags = 1` and `format_version = 3`.
- 64 KiB Shannon entropy sampled at offsets 0, 0x1000, 0x10000, 1 MiB, 16 MiB, 64 MiB, 128 MiB, 256 MiB:
  6.40, 6.69, 7.68, 7.68, 7.997, 7.998, 7.997, 7.997 bits/byte.
- A chunked ASCII scan (4 MiB chunks) of the whole file found no `d3d12_xs.dll`, `d3d12.dll`,
  `D3D12CreateDevice`, `ID3D12Device`, `gameos`, or `xboxgip` strings. A token spanning a chunk boundary could be
  missed, but the entropy result does not depend on the scan.
- Conclusion: the payload is encrypted or compressed. It cannot supply the D3D12 API surface without keys, which
  are out of scope. No key search or decryption was attempted.

Reproduce: read the first 4 KiB, locate `msft-xvd`; compute per-block entropy; scan for the ASCII tokens above.
The file is an XVD container, not a PE; PE-oriented import tools do not apply.

### A.2 `XboxBvhBuilder.dll`

- Size 884,224 bytes; PE32+ x86-64; export directory RVA 861,536, 12 named exports.
- The exported C++ signatures name:
  - `ID3D12Device11` (constructor, rehydration),
  - `ID3D12GraphicsCommandList4` (standard build, copy, CPU validation),
  - `ID3D12GraphicsCommandList6` (batched build),
  - `ID3D12GraphicsCommandList8` (batched rehydration),
  - `D3D12XBOX_XDXR_STREAM_DESC` and `D3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2`,
  - `D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC` and the standard build/copy/prebuild types.
- Imports: `d3d12_xs.dll!D3D12SerializeVersionedRootSignature` (ILT RVA 864,368, IAT RVA 148,048) and
  `xmem.dll!XMemVirtualQuery` (ILT RVA 864,384, IAT RVA 148,064). No other console graphics imports.
- GUID byte scan against the Windows SDK `d3d12.h` found `IID_ID3D12PipelineState` at file offset 0x0c9ed8 and
  `IID_ID3D12RootSignature` at 0x0c9ee8. Presence in a read-only data table is evidence of the interface version
  the DLL was compiled against, not proof that every method is called.

Reproduce: `rea.inspect_native_exports` and `rea.inspect_pe_imports` on the DLL, then a byte scan for the
little-endian GUIDs from `d3d12.h`.

Constructed by `runtime/test/bvh_builder_test.cpp` (1.0.2.1: `rt::Loader`, `Loader::initialise()` for TLS and
`DllMain`, `.pdata` registered by the loader (#176), then `XboxBvhBuilder(ID3D12Device11*, bool)`
on a PC device behind the Xbox-ABI proxy). Measured:

- 0 unimplemented imports; `D3D12SerializeVersionedRootSignature` and `XMemVirtualQuery` bind to shims. The constructor writes 24 bytes of the object (`sizeof(XboxBvhBuilder)` >= 24;
  the rest of its state is elsewhere). It prints nothing through `OutputDebugString` and never calls
  `TerminateProcess`. On failure it throws the failing `HRESULT` as a C++ `long`.
- **Device slot 62 is called as `(desc, 0, 0, &IID_ID3D12PipelineState, out)`**, with a plain
  `D3D12_COMPUTE_PIPELINE_STATE_DESC` (root signature set, node mask 0, flags 0). The proxy used to read
  `(desc, riid, out)`, so the first call returned `E_POINTER` and the constructor threw it.
- With slot 62 read that way, on **WARP** the constructor returns after 62 ms: 17 root signatures (slot 16) and 53
  compute PSOs (slot 62, 53 of 53 `S_OK`), CS bytecode 4–75 KB each. `~XboxBvhBuilder` returns and makes no device
  call. Created is not the same as correct: the container's DXIL bitcode is Xbox DXIL with `@xbox.*` intrinsics
  (section 7.2), its console `XBCH` record is `FSSM`, and nothing has been dispatched yet.
- **NVIDIA (RT-54).** On an RTX 3060 (616.64), an RTX 4090 (621.14) and an RTX 5070 Ti (621.24), all on Windows 11,
  the first 48 PSOs return `S_OK`, and the 49th (38,248 bytes) never does. Some runs return `E_OUTOFMEMORY` for it,
  which the constructor throws. Other runs end the process inside `nvwgf2umx.dll` with `0xC0000409`, after the
  driver's LLVM 7.0.1 bitcode reader prints `Invalid record` or `Never resolved value found in function`. That
  can't be caught. The D3D12 debug layer prints nothing first.
  - **Cause: the input, not Leonida.** The proxy passes the bytecode through unchanged, so nothing is wrong in what
    it forwards. The input is the problem: `LEONIDA_BVH_RECORD62=1 LEONIDA_BVH_DXIL=1` shows that every one of the
    53 PSO calls (52 distinct containers) is signed, has the same 9 parts, and is shader model 6.5 / DXIL 1.5. But dxil.dll (Windows SDK
    10.0.26100) rejects all 53 (`0x80aa0009`): the target triple is `dxil-ms-xb2` and the module carries
    `xbox.compiler.options` metadata. Most also have Xbox-only externals (44 of 53), `extractvalue` on non-DXIL types
    (16) or other disallowed instruction types (5). No PC driver has to accept this, and WARP happens to.
  - **What sets the 49th apart: one error kind.** It is the only shader of the 53 that the validator reports for a
    resource in a function signature (a non-entry function that takes a resource). Its other counts are 265
    `extractvalue`, 6 Xbox externals and 514 `i64` mentions; the 36th shader has the same 265 and 468 and is created.
  - **Workaround: the test uses WARP on every hardware vendor.** AMD doesn't take these shaders either: on an RX 7900
    XTX (32.0.32029.2010) the constructor faults with `0xC0000005` right after the first slot 62 call (RT-89, #166).
    So `bvh_builder_test` uses WARP whenever there is a hardware adapter, unless `LEONIDA_BVH_HARDWARE=1`, and it
    prints the reason with the adapter's PCI vendor id. WARP creates all 53 PSOs, and the constructor and destructor
    return.
  - **The runtime itself still shouldn't hand Xbox DXIL to a PC driver.** The same container carries the console
    `XBCH` record, which decompresses to `FSSM`; all 52 programs translate and pass `spirv-val` today
    ([embedded_shaders.md](embedded_shaders.md)). Translating that record is the path that avoids a DXIL lowering
    front end (section 7.2, M6); the bitcode is a second representation.
- **On the Vulkan device (D3D-557).** With `LEONIDA_BVH_DEVICE=vulkan` the constructor runs on the Vulkan-backed
  device, where slot 62 needs the translated module of each container's `FSSM` program. `make_game.py` translates
  the DLL's programs into `<shaders>/bvh/` and the shader library reads that index
  ([embedded_shaders.md](embedded_shaders.md) section 7). Measured on 1.0.2.1: before that step creation #1 failed
  (`E_FAIL` with no library, `E_INVALIDARG` with the game's corpus loaded). With it, on an RTX 3070 (NVIDIA 610.74)
  and on a Ryzen 7 9700X's integrated Radeon (AMD 32.0.21042.62), both with the `--wave64-emulate` translation, 49 of
  the first 50 creations return `S_OK` and the constructor throws `E_INVALIDARG` at #50, `Box16Rehydration`, the one
  program the translator had no rule for then. Since that rule, `buffer_store_short`, is on `main` (`8f208d09a`), all
  53 are created on the Radeon with the emulated modules and the constructor and destructor return
  ([embedded_shaders.md](embedded_shaders.md) section 7, `main` `8575d36e`; the RTX 3070 was not run again). Nothing
  has been dispatched.

### A.3 `amd_fidelityfx_upscaler_xbox.dll`

- Size 786,432 bytes; PE32+ x86-64.
- Import: `d3d12_xs.dll!D3D12CreateRootSignatureDeserializer` (IAT RVA 164,672).
- GUID byte scan found `IID_ID3D12Device` (0x0472f0), `IID_ID3D12Resource` (0x047310),
  `IID_ID3D12RootSignatureDeserializer` (0x047320), `IID_ID3D12PipelineState` (0x047330),
  `IID_ID3D12Heap` (0x047340), `IID_ID3D12RootSignature` (0x047350), `IID_ID3D12DescriptorHeap` (0x0472e0) and
  `IID_ID3D12CommandSignature` (0x047300). Again, presence ≠ use.
- Run through the public FidelityFX API by `runtime/test/fsr_dispatch_test.cpp` (1.0.2.1: `rt::Loader` and
  `Loader::initialise()`, with #93's static TLS, which the DLL's thread-safe statics need, and `.pdata` registered by
  the loader (#176); PC D3D12 device on an RTX 4090 behind the Xbox-ABI proxy). Measured:
  - It maps with 0 unimplemented imports: `D3D12CreateRootSignatureDeserializer` and `XMemAlloc` /
    `XMemFree` / `XMemGetWorkingSetStatistics` bind to the shims, 82 imports to host DLLs. Its TLS template is
    8 bytes with no callbacks. `DllMain(DLL_PROCESS_ATTACH)` returns TRUE.
  - `ffxConfigure(GLOBALDEBUG)` and the CPU-only upscale queries accept the **PC struct type ids**: the
    Performance ratio is 2.0 and 3840x2160 Quality renders at 2560x1440. `GET_VERSIONS` for the upscaler lists one
    provider, `0xf5a5ca1e01001001` "4.1.1".
  - `ffxCreateContext` reports, through the message callback, that only one configuration is built in:
    `maxUpscaleSize` 1920x1080 ("only the fixed-resolution Quality shader tables are included", so a 1280x720
    render size), with `HIGH_DYNAMIC_RANGE`, `DEPTH_INVERTED` and `AUTO_EXPOSURE` all required. It also requires an
    `ffxCreateContextDescUpscaleVersion` in the chain; 4.1.2 is rejected as invalid, 4.1.1 and 4.0.0 are accepted.
  - With all of that, `ffxCreateContext` returns `FFX_API_RETURN_ERROR` with no message, no XMem allocation and no
    call on the device proxy, for the PC DX12 backend descriptor (`0x2`), for the same `{header, device}` layout
    under `0x02000000`, `0x02000001` and `0x02000002`, and with no backend descriptor at all.
- **The Xbox backend descriptor, measured from the DLL** (Ghidra 12.2 through REA; RVAs, image base `0x180000000`):
  - `ffxCreateContext` (`0x11d0`) picks its one provider (`ffxProvider_FSR4_Int8` by RTTI) by finding a device in
    the chain: type 4 (`GET_VERSIONS`) at `+0x18`, types `0x10009` and **`0x01000002`** at `+0x10`.
  - The provider's create (`0xc9b0`) wants `CreateContextDescUpscale` (`0x10000`) first, checks the version
    descriptor (`0x1000b`, 4.0.0 to 4.1.1), then walks `pNext` for **type `0x01000002`: `{header, device}`, the device
    at `+0x10`**, the same layout as the PC DX12 descriptor. Two of them, or none, is `FFX_API_RETURN_ERROR`
    without a message: that is what the ids above got. An optional type `0x01000003` sets function pointers: when the
    one at `+0x20` is set (with `+0x28`), the backend calls it instead of `CreateHeap` (device slot 28, `0x7390`); what
    its other fields do is open.
  - The HRESULT checks at the calls seen here (`0x33d4`, `0x65b3`, `0x74a8`) go through one helper (`0x1c60`):
    `FormatMessageW` of the code, then `throw 1`.
- **Run with that descriptor** (`fsr_dispatch_test`, PC device on an RTX 2000 Ada, driver 597.06, behind the
  Xbox-ABI proxy): the DLL creates its backend through the proxy. Device slots called: 14 `CreateDescriptorHeap` (5),
  15 `GetDescriptorHandleIncrementSize`, 18 `CreateShaderResourceView`, 19 `CreateUnorderedAccessView` and **76** (4),
  plus heap slot **9** `GetCPUDescriptorHandleForHeapStart` (Xbox RAX form). No earlier test in this repository
  reached slot 76 or heap slot 9 with a real client; their measured shapes hold. Slot 76 is called with the 7
  arguments of `CreateCommittedResource`
  (heap properties, heap flags, resource desc, state, clear value, IID, out; call at `0x33cc`), the desc being the
  56-byte `D3D12_RESOURCE_DESC` with layout `0x101` and the word 3 at `+0x34`.
  - On that GPU it stops at its 4th slot-76 texture: 1920x1080 `DXGI_FORMAT_R9G9B9E5_SHAREDEXP` with
    `ALLOW_UNORDERED_ACCESS`, which the NVIDIA driver refuses (`E_INVALIDARG`); WARP creates it. The console build
    creates it, so the console takes that format as a UAV. On the same GPU Vulkan has no `STORAGE_IMAGE` support for
    `VK_FORMAT_E5B9G9R9_UFLOAT_PACK32` either (vulkaninfo, driver 597.06), so the layer needs an emulation (32-bit
    storage plus a pack/unpack) where a GPU lacks it. @Vonsek measured the same texture on more devices (#864):
    refused on an RTX 5080 (616.92), created on an AMD Radeon and on WARP, where typed UAV writes round-trip; and on
    both GPUs one allocation can serve both uses, a mutable `R32_UINT` Vulkan image with `E5B9G9R9_UFLOAT_PACK32` in
    its format list, written through an `R32_UINT` storage view and sampled through an `E5B9G9R9` view.
    **Implemented** in `d3d12_vulkan_device.cpp` and `src/uav_clear.h` (`d3d12_r9g9b9e5_uav_test`): on an RTX 3060 Ti
    (driver 621.01, `LEONIDA_FSR_DEVICE=vulkan LEONIDA_FSR_NO_STANDIN=1`, `LEONIDA_VK_VALIDATION=1`),
    `ffxCreateContext` and `ffxDestroyContext` both return OK without `standin_slot76` and with 0 validation errors.
    Open: a float store to that format rounds differently on PC devices (@Vonsek, #864: AMD Radeon truncates the
    mantissas, WARP rounds to nearest even; 609 of 794 test triples differ). Which rule the console FSR needs, and
    whether its shaders store floats to that texture at all (requiring shader-side packing into the `R32_UINT` storage
    view), waits on decoding its `FSPX` shaders.
  - With that one texture created as `R32_UINT` instead (the test's PC stand-in, `standin_slot76`), or as is on
    WARP, `ffxCreateContext` and `ffxDestroyContext` both return OK. Device slots called: 14 (5), 15 (26), 18 (10),
    19 (16), **27
    `CreateCommittedResource` (4)**, **38 `GetCopyableFootprints` (4)**, 76 (10); heap slot 9 (26). So the console
    device also serves the PC `CreateCommittedResource` at slot 27, and slots 14, 15, 18, 19, 27 and 38 keep their PC
    positions for this client. No compute pipeline or root signature is created at context creation.
  - On that context, one `ffxDispatch` recorded into a logging command list measures the **console command list**:
    slots 14, 15, 25 and 26 are at their PC positions, then `SetDescriptorHeaps` is at 32 (PC 28), the compute root
    setters four slots later (33, 35, 41; two of them inferred from their shapes), `ExecuteIndirect` two slots later
    (61), and there are console-only methods at 85 and 141. The list object also carries a write cursor and limits
    (`+0x18`, `+0x28`, `+0x30`) the DLL writes commands through while there is room. Table and evidence:
    [d3d12_command_objects.md §3.5a](d3d12_command_objects.md).
    Device slot 67 is a console-only compute-pipeline call, `DeserializeComputePipelineStateX` in the DLL's own
    messages, not the PC method at that index; the proxy routes it to `CreateComputePipelineState`. The dispatch stops where the PC runtime
    refuses the DLL's compute code at slot 62: the DLL's shaders carry an `ACCC` part instead of DXIL, an `FSPX`
    payload with an RDNA2 program ([fspx.md](fspx.md)). The Vulkan device takes both forms at
    `CreateComputePipelineState`: the `ACCC` container slot 62 gets, and the `FSPX` payload the proxy builds from the
    slot-67 description's four pairs. It finds the translated module by `fspx_code_sha256` and `fspx_state_sha256` in
    `<shaders>/fspx/index.json`, which `make_game.py` writes ([fspx.md](fspx.md), "Build and run-time lookup";
    `d3d12_fspx_compute_test`). The pipeline answers the console slot 11 the DLL calls afterwards with 0, so no
    compute scratch buffer is made ([d3d12_command_objects.md](d3d12_command_objects.md) §3.5a). Not run here: the
    DLL's own dispatch on this device (`fsr_dispatch_test` needs MSVC structured exception handling).
  - Its `throw 1` reaches the caller. Before #965 the throw carried an image base of 0, because `_CxxThrowException`
    did not know manually mapped images; with #965 it carries the DLL's base, and the DLL still does not catch it on
    that path (measured with `fsr_dispatch_test` and `LEONIDA_FSR_NO_STANDIN=1`).
  - `xmem.dll!XMemGetWorkingSetStatistics` takes two arguments, `(0x20000001, stats*)`, at all seven call sites;
    the one-argument shim wrote through the constant. Fixed in `runtime/shims/xmem.cpp`.

### A.4 Other DLLs and the executable

- `amd_fidelityfx_loader_xbox.dll`, `Bink2Scarlett.dll`, `libHttpClient.GDK.dll`, `XCurl.dll`: no `d3d12_xs`
  imports.
- The package layout XML, chunk 1001, lists `game_scarlett_master.exe`; the file is absent from the folder. One
  claim in this document comes from running the executable from a player's own copy: how it calls device slot 26
  (section 4.1; register roles and argument values only). No other claim depends on the executable's contents.
- The repository's existing import survey (`docs/design.md`, "Baseline already recorded") records that the 16
  bundled DLLs need only `xmem` and the two `d3d12_xs` root-signature calls; our scan of the six non-CRT DLLs is
  consistent.

## Appendix B: interface versions seen in shipped binaries

| Interface | Evidence | Public methods beyond the previous version |
|---|---|---|
| `ID3D12Device11` | `XboxBvhBuilder.dll` constructor and rehydration signatures | `CreateSampler2` (public SDK) |
| `ID3D12GraphicsCommandList4` | standard BVH build/copy/validation signatures | `BeginRenderPass`/`EndRenderPass`, `BuildRaytracingAccelerationStructure`, `EmitRaytracingAccelerationStructurePostbuildInfo`, `CopyRaytracingAccelerationStructure`, `DispatchRays`, `SetPipelineState1` |
| `ID3D12GraphicsCommandList6` | batched BVH build signature | `DispatchMesh` |
| `ID3D12GraphicsCommandList8` | batched rehydration signatures | `OMSetFrontAndBackStencilRef` |

The public Xbox graphics index retrieved for this document lists interfaces up to `ID3D12Device10` and
`ID3D12GraphicsCommandList7`; the shipped DLL names `Device11` and `CommandList8`, which the index does not cover.
This is why the layer targets the Windows SDK's newer public chain rather than assuming the Xbox public index is
complete.
