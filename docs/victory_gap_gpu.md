# VIctory vs Leonida: the GPU side

What VIctory (`victory/`, GPL-3.0-or-later) implements on the console D3D12 surface, set against what Leonida does,
and which gaps were ported (branch `port/gap-gpu`). VIctory's slice read: `victory-hle/src/modules/d3d12x.rs`,
`display.rs`, `fidelityfx.rs`, `dstorage.rs`, and `victory-gpu/src/` (`device.rs`, `list.rs`, `queue.rs`,
`fence.rs`, `frame.rs`, `dxgi.rs`, `descriptor.rs`, `resource.rs`, `signature.rs`, `query.rs`, `root.rs`,
`indirect.rs`). Leonida's side: `runtime/shims/d3d12_xs*.{cpp,h}` (slot tables in `d3d12_xs_slots.h`),
`dxgi_*`, `xbox_display.*`, `dstorage_xs.*`, `xgameruntime_offline.cpp` and `d3d12_vulkan_device.cpp`.

Legend: **same** = Leonida already does what VIctory does (often ported earlier, citing VIctory); **ported** = gap
closed on this branch; **left** = gap kept, with the reason. "V" is `victory/crates/`.

## Gaps, by how likely the title is to hit them early

| # | What VIctory does | Leonida before | Status | Where / test |
|---|---|---|---|---|
| 1 | Device object: +0x10 null-CB address 0, +0x18 out-of-line block with byte 0 = 1; the title tests byte 0x18 before inlining a device call (V `victory-gpu/src/device.rs:103-119, 479`) | +0x10 was the PC vtable pointer, +0x18 the PC slot count (non-zero low byte by chance) | ported | `d3d12_xs_device.cpp` `DeviceProxy`; `xs_proxy_mock` `check_console_headers` |
| 2 | Queue, allocator, list: command list type right after the header, +0x10 ("the title reads the list type right after the header", V `queue.rs:31-35, 1048`; `list.rs:90-100`) | +0x10 was the `DeviceProxy*` | ported | `ChildProxy::value_word`, `ListProxy::list_type`; `check_console_headers` |
| 3 | Fence value at +0x10, which slot 12 hands out (V `fence.rs:24-29, 136-139`) | value word elsewhere in the proxy (slot 12 already worked) | ported (layout) | `ChildProxy::value_word`; `check_console_headers` |
| 4 | Descriptor heap: description at +0x10, first descriptor at +0x20 (V `descriptor.rs:49-54`) | device pointer / PC vtable there | ported (cheap; VIctory does not say the title reads them) | `HeapProxy`; `check_console_headers` |
| 5 | CheckFeatureSupport OPTIONS in the console's 16-word layout, console-only word at index 11 (V `device.rs:240-273`) | slot 13 forwarded: the PC runtime refuses size 64 with E_INVALIDARG | ported (console layout only) | `console_check_feature_support`; `check_console_features` |
| 6 | FEATURE_LEVELS up to 12_2 (V `device.rs:97-98, 320-347`) | Vulkan device caps at 12_1 | ported (console layout only) | same |
| 7 | `D3D12ConfigureVirtualMemoryX` writes the GPU address range (user space) at +0x10/+0x18 and refuses null (V `device.rs:184-226`) | accepted, wrote nothing | ported | `d3d12_xs.cpp`; `d3d12_xs_exports` |
| 8 | `IID_ID3D12Device` bound as a data import (V `victory-hle/.../d3d12x.rs:80-84`) | no entry: an unimplemented-import thunk, whose code bytes a reader would take for the GUID | ported | `d3d12_xs.cpp` SHIM_TABLE; `d3d12_xs_exports` |
| 9 | Display timeout deferral = a display-required power request; close clears it (V `display.rs:79-123`) | E_FAIL, null handle | ported | `xgameruntime_offline.cpp` XLauncherImpl 4/5; `xgameruntime_offline` |
| 10 | CheckFeatureSupport FORMAT_SUPPORT answers every use of every format; ARCHITECTURE answers UMA and cache-coherent UMA (V `device.rs:296-306, 286-294`) | the Vulkan device answers what it honours, and the host GPU's architecture | left: Leonida's answers are measured and its paths refuse what it reports unsupported; a blanket "yes" would move failures later | - |
| 11 | ResolveQueryData writes stand-ins: occlusion 2^20 samples, binary visible, timestamps from the CPU clock (V `list.rs:1330-1376`) | real Vulkan queries | left: VIctory's are stand-ins for running no queries; Leonida runs them. If dropped draws make the title cull by occlusion, an opt-in "everything visible" resolve is the next step (needs a GPU-side write in `vulkan_replay.h`) | - |
| 12 | Queue GetDesc returns the console description a queue was created with (V `queue.rs:464-495, 966-971`) | the PC description (same Type at +0) | left: only the type is known to be read, and it is at the same offset | - |
| 13 | App capture provider (IID `22e672d7-...`), screenshot slot 0x28 answers NOT_SUPPORTED (V `display.rs:40-55, 129-137`) | class not served: QueryApiImpl fails, XAppCapture reported unavailable | left: same outcome for the title (no screenshot), and serving the class would flip IsFeatureAvailable | - |
| 14 | FidelityFX upscaler served by the host's PC FSR DLL on host D3D12 (`VICTORY_FIDELITYFX_DIR`, V `fidelityfx.rs`) | the title's own console FSR DLL runs (device slot 67, list slot 141) | left: needs a host D3D12 device; Leonida's is Vulkan. Off by default in VIctory too | - |
| 15 | Host GPU failure ends the process with DXGI_ERROR_DEVICE_REMOVED (V `d3d12x.rs:27-37`) | the Vulkan layer's own device-lost handling | left: different backend, nothing to port | - |
| 16 | Resource memory from the title's XMemAlloc, page mapper (V `d3d12x.rs:19-24`) | Leonida's title memory (TitleMemory / SparseTitleMemory) | left: out of scope (brief), different design | - |

## Already the same

| Area | VIctory | Leonida |
|---|---|---|
| Device slots 0-158 | `device.rs:402-458` | `d3d12_xs_slots.h` names every slot VIctory implements: 7-29, 36, 38, 39, 41, 48, 57 (refused, output nulled), 61, 62, 67 (Leonida implements it; V does not), 69, 70, 71, 73-76, 78-81, 86, 108, 127, 133 |
| Device levels, ID3D12Device11 layout switch | `device.rs:30-168` | `Layout::console`, same rule |
| Frame clock 78-81 | `frame.rs` | `frame_set_interval` / `frame_wait` / `frame_statistics`, ported line for line |
| Queue slots (both layouts' shift at 14) | `queue.rs:973-1017` | `kQueueEntries`; Suspend/Resume accepted (V: E_NOTIMPL) |
| Queue waits, cut lists, held submissions | `queue.rs:37-430` | value waits held by the device or checked at ExecuteX (`wait_for_values`) |
| PresentX, CopyPageMappingsBatchX, Signal2X, ExecuteX | `queue.rs:588-962` | present handler, page mapper, `signal`, `execute` |
| List slots, older/newer layouts | `list.rs:2055-2173` | `kListEntries` (every V slot, plus 49, 53-55, 141) |
| Write32/64 (GPU and system timestamps), WaitValue, CopyMemoryToMemoryX, AllocateUploadMemory, DispatchMeshX, enhanced barrier groups | `list.rs` | `list_title_method`, `write_value` / `write_gpu_timestamp` in the Vulkan device |
| Allocator slot 9 callback | `list.rs:1378-1387` | `kAllocatorCallbackSlot` |
| Console descriptor heaps with title memory, CPU = GPU start | `descriptor.rs:37-120` | `create_console_descriptor_heap` |
| Indirect argument type 105 (single-pass dispatch) | `list.rs:1669-1703` | `CreateCommandSignature` in the Vulkan device |
| Struct returns 25/26/108 return-buffer first | `resource.rs:802-849, 2583-2593` | `device_struct_return` |
| Console resource descriptions (layout 0x101, Xbox flag bits) at 76/127/133 | `resource.rs` | `make_pc_desc`, placed-at-VA handler |
| Root signatures (console flag byte cleared, host serializer and deserializer) | `root.rs` | `d3d12_xs.cpp` |
| DXGI device, adapter, factory, output, console mode list (slot 21) | `dxgi.rs` | `xbox_display.*`, device proxy's IDXGIDevice1 |
| HDR: disabled (SDR) | `display.rs:60-68` | `display_try_enable_hdr` (LEONIDA_HDR=1 opts in) |
| LaunchUri: nothing launched | `display.rs:70-76` | E_FAIL |
| DirectStorage: factory, files, status arrays, queues, scheduler, zlib and BCPack | `dstorage.rs` | `dstorage_xs.*`, ported from it |

## Tests (no game data)

- `xs_proxy_mock` (`runtime/test/xs_proxy_mock_test.cpp`): `check_console_headers`, `check_console_features`, and
  slot 13 in the slot-table check.
- `d3d12_xs_exports` (`runtime/test/d3d12_xs_exports_test.cpp`): the IID data import and the virtual memory range.
- `xgameruntime_offline`: the deferral is an open power-request handle; null output refused.
