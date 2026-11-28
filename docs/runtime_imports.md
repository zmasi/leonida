# Runtime: measured imports of the shipped modules

`gta6rt --dry-run` maps an x86-64 image, applies base relocations, binds every import through the shim
registry or the host, and prints a report. It does **not** call `DllMain`, TLS callbacks or the entry point, so
this page measures mapping and binding only; it does not claim the modules work.

The game executable is not in the package. Every other PE that the package ships is present, and all of them are
Xbox Series builds. Running the dry-run over those images is the closest thing to a loader test that does not need
the executable; [The retail executable](#the-retail-executable-rt-5) below is the dry-run over the executable
itself, supplied separately.

## Reproduce

Build the runtime, then point it at the package:

```powershell
powershell -File scripts/build_runtime.ps1

$game = "D:\path\to\your\package"
runtime\build\gta6rt.exe --dry-run "$game\XboxBvhBuilder.dll" --game-dir $game
```

Each run prints one line per mapped module:

```
XboxBvhBuilder.dll  base=0000000180000000 size=909312 entry=+0x227cc  shim=4 host=53 module=0 unimplemented=0
    shim          d3d12_xs.dll!D3D12SerializeVersionedRootSignature
    shim          xmem.dll!XMemVirtualQuery
    shim          api-ms-win-crt-runtime-l1-1-0.dll!_configure_narrow_argv
    shim          api-ms-win-core-libraryloader-l1-2-0.dll!DisableThreadLibraryCalls
1 module(s) mapped, 0 unimplemented import(s)
unimplemented by return policy: hresult=0 other=0
```

`shim` counts imports served by the clean-room tables, `host` counts imports served by the desktop DLLs,
`module` counts imports served by another bundled image, and `unimplemented` counts imports with no binding.

## Measured result

All 16 DLLs in the package map at their preferred base `0x180000000` with no relocation, and every import binds. `gta6rt --init` on each one also runs its TLS callbacks and `DllMain` and exits 0 (counts re-measured with `--dry-run` and `--init` on `main` `aa2988cc`, Windows 11):

| Image | Size of image | Entry RVA | Shim | Host | Module | Unimplemented | `--init` |
|---|---:|---:|---:|---:|---:|---:|---|
| `Bink2Scarlett.dll` | 278,528 | `0x2ac1c` | 4 | 56 | 0 | 0 | exit 0 |
| `XboxBvhBuilder.dll` | 909,312 | `0x227cc` | 4 | 53 | 0 | 0 | exit 0 |
| `amd_fidelityfx_upscaler_xbox.dll` | 802,816 | `0x26780` | 6 | 80 | 0 | 0 | exit 0 |
| `amd_fidelityfx_loader_xbox.dll` | 32,768 | `0x18d0` | 5 | 17 | 0 | 0 | exit 0 |
| `libHttpClient.GDK.dll` | 999,424 | `0x11f0` | 26 | 172 | 0 | 0 | exit 0 |
| `XCurl.dll` | 188,416 | `0x17b0` | 24 | 116 | 0 | 0 | exit 0 |
| `concrt140.dll` | 303,104 | `0x29160` | 2 | 101 | 0 | 0 | exit 0 |
| `msvcp140.dll` | 577,536 | `0x525a0` | 13 | 174 | 0 | 0 | exit 0 |
| `msvcp140_1.dll` | 36,864 | `0x15c0` | 2 | 31 | 0 | 0 | exit 0 |
| `msvcp140_2.dll` | 192,512 | `0x1efa0` | 2 | 94 | 0 | 0 | exit 0 |
| `msvcp140_atomic_wait.dll` | 81,920 | `0x3c80` | 4 | 60 | 0 | 0 | exit 0 |
| `msvcp140_codecvt_ids.dll` | 28,672 | `0x1340` | 2 | 22 | 0 | 0 | exit 0 |
| `vccorlib140.dll` | 344,064 | `0x27e50` | 7 | 107 | 0 | 0 | exit 0 |
| `vcomp140.dll` | 86,016 | `0x6580` | 7 | 71 | 0 | 0 | exit 0 |
| `vcruntime140.dll` | 110,592 | `0xf890` | 3 | 38 | 0 | 0 | exit 0 |
| `vcruntime140_1.dll` | 49,152 | `0x3e90` | 1 | 33 | 0 | 0 | exit 0 |

The 12 Microsoft-signed DLLs in this table (the 10 VC++ runtime DLLs, `XCurl.dll` and `libHttpClient.GDK.dll`) report an Authenticode hash mismatch as shipped, because the Xbox packaging fills the load-config `CodeIntegrity` field of all 16 DLLs after signing; this is expected in every copy of the package and is not a sign of tampering ([jasontheminions's finding](https://github.com/ItsNotRexed/Leonida/issues/8504#issuecomment-7110985774)).

Since the library-loader shims (`runtime/shims/library_loader.cpp`), a module's `LoadLibrary*`, `GetModuleHandle*`, `GetProcAddress` and `FreeLibrary` imports count as shim, through `kernel32`, `kernelbase` or the `api-ms-win-core-libraryloader-*` set it names. That is why every image above has a few more shim and fewer host imports than before; the Xbox-specific imports below are unchanged. `DisableThreadLibraryCalls` is served the same way: a mapped module gets `TRUE` (the real function refuses a module the Windows loader did not map, which failed the `DllMain` of the four `msvcp140_*` satellite DLLs), and any other handle goes to the real function.

The process shim (`runtime/shims/process.cpp`) serves `ExitProcess` and `GetCommandLineA/W` the same way: through `kernel32`, `kernelbase`, and `api-ms-win-core-processthreads-l1-1-0` and `api-ms-win-core-processenvironment-l1-1-0`, the sets the API-set umbrella libraries (`OneCore_apiset.lib`, `mincore.lib`) bind them to. `vcomp140.dll` imports `ExitProcess` from the first of these. The UCRT's `_get_narrow_winmain_command_line` and `_get_wide_winmain_command_line` are served too, so a `WinMain` executable gets the game's command line as `lpCmdLine`. `abort()` raises `SIGABRT` when the guest installed a handler (the guest's `signal()` import binds to the host CRT, so its table is the one raised here); with the default or ignore disposition, or a handler that returns, the run still ends with code 3. `run_tiny_game_abort_exit_codes` compares that with a native run of the stand-in image. The table above is `gta6rt --dry-run` on each DLL at the commit that added this paragraph.

The lifecycle shim (`runtime/shims/lifecycle.cpp`) serves `RegisterAppStateChangeNotification` and
`UnregisterAppStateChangeNotification` under `api-ms-win-core-psm-appnotify-l1-1-0`, `-l1-1-1` and `kernelbase.dll`, and
`CreateWindowExW`/`CreateWindowExA` under `user32.dll` and the two `ext-ms-win-rtcore-ntuser-window-l1-1-*` sets, and
`CreateWindowExA` under `ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0`. The title's import table names
`RegisterAppStateChangeNotification` from `api-ms-win-core-psm-appnotify-l1-1-0` and `CreateWindowExA` from that ANSI
set (`tools/pe_inspect.py`). Only the title's first top-level window (no parent, not a child, not a tool window) is fitted to the work area and hooked: its
`SC_CLOSE` runs the registered suspend callbacks with `quiesced` TRUE under the registration lock, then the run ends
through the title's own `ExitProcess`, so the host finishes its cleanup. A 1 s deadline ends the run the same way if the
callbacks have not returned.

The shims added since account for the rest of the difference from the first version of the table. The UCRT's `_configure_narrow_argv`, `_configure_wide_argv`, `__p___argc`, `__p___wargv` and `abort` are served by `runtime/shims/process.cpp`, which is why almost every DLL gained one or two. The file-system calls listed in *Console paths and the write overlay* below (`CreateFile2`, `CopyFile2`, `CreateDirectoryW`, `FindFirstFileExW`, `SetFileAttributesW` and the rest) account for `msvcp140.dll`, `vcomp140.dll`, `Bink2Scarlett.dll`'s `CreateFile2` and part of the two HTTP clients. `winhttp.dll` is a module the shims own (*WinHTTP offline* below), so every `WinHttp*` import binds to it: 15 in `libHttpClient.GDK.dll` (all 16 but `WinHttpCrackUrl`) and 15 in `XCurl.dll` (all but `WinHttpQueryOption`). That is why the two HTTP clients moved the most: 11 to 26 and 9 to 24 shim imports.

The UCRT argv configuration imports honor the executable's startup mode. Each run starts with `argc == 0` and
null narrow/wide argument tables, while `GetCommandLineA/W` is already available to TLS and process-attach
callbacks. Default startup publishes unexpanded arguments for its chosen width. `setargv.obj` / `wsetargv.obj`
startup expands wildcard arguments against the host working directory, including quoted patterns; an unmatched
pattern stays literal. Narrow expansion searches the ACP-converted pattern and converts matching names back to
ACP, so its matches can differ from wide startup. The two widths retain independent tables, and the most recently
configured width sets the shared `argc`. `noarg.obj` leaves the initial tables null; later no-argument
configuration is a no-op that preserves existing pointers and counts. These are guest-owned tables, without
changing the host CRT's argument globals. This does not add console-path wildcard routing or change the legacy
`msvcrt!__getmainargs` wildcard policy. `run_argv_modes` compares the same six linker-selected MSVC fixtures
natively and through the loader; `run_startup_cmdline` also checks native preconfiguration and command-line
identity through attach and entry.

The six unique Xbox-specific imports (eight consumer/import pairs) are exactly the set recorded in
[`d3d12_vulkan.md` §7](d3d12_vulkan.md#7-prior-art-and-measured-xbox-abi):

| Consumer | Import | Binding |
|---|---|---|
| `Bink2Scarlett.dll` | `xmem.dll!XMemAlloc` | shim |
| `Bink2Scarlett.dll` | `xmem.dll!XMemFree` | shim |
| `XboxBvhBuilder.dll` | `xmem.dll!XMemVirtualQuery` | shim |
| `XboxBvhBuilder.dll` | `d3d12_xs.dll!D3D12SerializeVersionedRootSignature` | shim |
| `amd_fidelityfx_upscaler_xbox.dll` | `xmem.dll!XMemAlloc` | shim |
| `amd_fidelityfx_upscaler_xbox.dll` | `xmem.dll!XMemFree` | shim |
| `amd_fidelityfx_upscaler_xbox.dll` | `xmem.dll!XMemGetWorkingSetStatistics` | shim |
| `amd_fidelityfx_upscaler_xbox.dll` | `d3d12_xs.dll!D3D12CreateRootSignatureDeserializer` | shim |

The video path (`Bink2Scarlett.dll`), the BVH builder and the FidelityFX upscaler therefore all go through the
two shim DLLs that already exist. `libHttpClient.GDK.dll` and `XCurl.dll` import only desktop APIs.

`xmem.dll!XMemTransferMemory` is not imported by any of these DLLs, but a Series executable calls it during startup
(reported in [#5934](https://github.com/ItsNotRexed/Leonida/issues/5934#issuecomment-7101779516)). Its GDK reference
page is an NDA topic, but Microsoft's public [NetRumble](https://github.com/microsoft/XBOX-MonoGame-NetRumble/blob/203255e785f2ab2671a9d83b724eaa14d5b07414/console/NetRumble.Bootstrap/Main.cpp#L135-L145)
sample and FNA's [NativeAOT-Xbox](https://github.com/FNA-XNA/NativeAOT-Xbox/blob/b9dc547afd0e05f45764d670ec49e0ac7997a301/Bootstrap/Main.cpp#L117-L124)
bootstrap call it as `if (!XMemTransferMemory(XMemTransferTitleToSystem, size))` with a `size_t` in multiples of
2 MiB, to move budget from the title's memory partition to the system's. A PC has no such partition, so the shim
returns `TRUE` for every direction and size and allocates nothing; with `--trace` it records each distinct direction
and size as an `xmem_transfer` event, because the enum values are not public. `xmem_test` covers it.

Seven more `xmem.dll` imports follow argument shapes measured locally at a title's call sites (the comments in
`runtime/shims/xmem.cpp` give each shape):

- `XMemAllocDefault` and `XMemFreeDefault` take `XMemAlloc`'s and `XMemFree`'s arguments and share their heap.
- `XMemAllocatePhysicalPages(attributes, UINT64* count, UINT64* pages)` returns a `BOOL` and the count it allocated.
  Each call is one pagefile-backed section of `count` 64 KiB pages. A page number is a 4 KiB frame number: a
  non-zero multiple of 16 below 2^25, which is what the measured title assumes when it packs them. `0` means "unmap".
- `XMemMapPhysicalPages(address, UINT64 count, pages)` returns the address. A null page array unmaps.
  `XMemMapPhysicalPagesScatter(addresses, UINT32 count, pages)` returns a `BOOL`, and a `0` entry unmaps.
  `XMemFreePhysicalPages(UINT64 count, pages)` returns a `BOOL`. Every page is its own view, mapped with
  `MapViewOfFile3` into a 64 KiB placeholder, so a page keeps its contents across remaps and can be mapped twice.
- The windows are reserve-only 64 KiB-page `XMemVirtualAlloc` regions. The shim reserves them as placeholders and
  keeps their protection, which mapped pages get (AWE's rule). The measured title also reserves a placeholder with
  `MEM_RESERVE_PLACEHOLDER` and carves windows out of it with `MEM_REPLACE_PLACEHOLDER`, and the shim passes both
  through. **ASSUMPTION:** nothing commits into a window with `kernel32!VirtualAlloc`, which a placeholder refuses.
- `XMemVirtualAlloc` used to refuse 2 MiB pages, and five of the 23 measured calls commit them. They are now
  committed 2 MiB-aligned through `VirtualAlloc2`. The attribute word is recorded (`xmem_attributes`) and the
  console's `PAGE_GRAPHICS_*` bits are dropped from the protection, as before.
- `XMemIsToolsMemAvailable()` is `FALSE`: a PC has no development-kit tools memory. The measured title asks only
  on an out-of-memory path, after an allocation has already failed.

## The retail executable (RT-5)

`game_scarlett_master.exe` (120,181,760 bytes on disk, SHA-256
`e95020b2fa4a721db3df59fdecf75621730ac559d8835252f6c3b658d5a08993`) is a PE32+ x86-64 image with no resource
section. `gta6rt --dry-run` maps it at its preferred base together with the four package DLLs it imports, and
exits 0 with nothing on stderr:

```powershell
$game = "D:\path\to\your\package"
$scratch = Join-Path $env:TEMP 'leonida-overlay'
runtime\build\gta6rt.exe --dry-run "D:\path\to\game_scarlett_master.exe" --game-dir $game --overlay $scratch
```

`--overlay` points the write overlay away from `<game-dir>\overlay`, so a dry run writes nothing into the package.
Measured at `1cc6e3c1`, Windows 11, MSVC 14.44:

| Image | Base | Size of image | Entry RVA | Shim | Host | Module | Unimplemented |
|---|---|---:|---:|---:|---:|---:|---:|
| `game_scarlett_master.exe` | `0x140000000` | 184,995,840 | `0x56a1ec8` | 53 | 432 | 72 | **30** |
| `XboxBvhBuilder.dll` | `0x180000000` | 909,312 | `0x227cc` | 5 | 52 | 0 | 0 |
| `amd_fidelityfx_loader_xbox.dll` | relocated | 32,768 | `0x18d0` | 6 | 16 | 0 | 0 |
| `libHttpClient.GDK.dll` | relocated | 999,424 | `0x11f0` | 28 | 170 | 0 | 0 |
| `Bink2Scarlett.dll` | relocated | 278,528 | `0x2ac1c` | 5 | 55 | 0 | 0 |

The 72 module imports are served by those four DLLs. The 30 unimplemented imports all fall under the dry-run's
policy "other" (none is counted as returning an `HRESULT`), and they come from seven Xbox-only DLLs:

| DLL | Unimplemented imports | What the names suggest (not measured) |
|---|---:|---|
| `xmem.dll` | 7: `XMemAllocDefault`, `XMemAllocatePhysicalPages`, `XMemFreeDefault`, `XMemFreePhysicalPages`, `XMemIsToolsMemAvailable`, `XMemMapPhysicalPages`, `XMemMapPhysicalPagesScatter` | the console's physical page and default allocation routines; `XMemAlloc`, `XMemFree`, `XMemVirtualQuery`, `XMemGetWorkingSetStatistics` and `XMemTransferMemory` already bind |
| `d3d12_xs.dll` | 1: `D3D12ConfigureVirtualMemoryX` | GPU virtual memory setup |
| `xg_xs.dll` | 2: `XGCreateTextureComputer`, `XGSuggestSwizzleMode` | texture layout computation |
| `XCRDAPI.dll` | 1: `DStorageGetFactory` | DirectStorage |
| `chad.dll` | 5: `XDspActivate`, `XDspConnectWithMaximumStreamLimit`, `XDspDeactivate`, `XDspDisconnect`, `XDspSubmitCommand` | the audio DSP |
| `xapu.dll` | 5: `XApuConnect`, `XApuDequeueResult`, `XApuDisconnect`, `XApuEnqueueCommand`, `XApuGetResultCount` | the audio processing unit (Opus decode when `runtime/opus_decoder` is built) |
| `AcpHal.dll` | 9: `AcpHalAllocateShapeContexts`, `AcpHalCreate`, `AcpHalReleaseShapeContexts`, `ApuAlloc`, `ApuCreateHeap`, `ApuFree`, `ApuIsVirtualAddressValid`, `ApuMapApuAddress`, `ApuMapVirtualAddress` | the audio hardware layer |

The game executable imports two functions from `xg_xs.dll`, the console's texture layout library:
`XGSuggestSwizzleMode` (2 call sites, in the helper that fills a texture description) and `XGCreateTextureComputer`
(4 call sites, which use slots 1, 2, 8 and 10 of the returned object's vtable). `runtime/shims/xg_xs.cpp` answers them
from a model of the console's texture layouts, `runtime/src/console_texture_layout.h` (AMD addrlib's GFX10 rules for
the linear, 256 B, 4 KiB and 64 KiB modes: block shapes, mip tails, the micro tile patterns, a separate stencil plane),
ported with the computer's method table and layout structure from Marvisak/VIctory (GPL-3.0-or-later, `xg.rs` and `texture.rs`
at `d2a4bae1f`). `XGSuggestSwizzleMode` writes `0x100 | mode` (linear for 1D, `64KB_Z_X` for depth, `64KB_R_X` for
render targets, else `4KB_S`); the computer fills the layout (slot 2), gives an element's offset (slot 6), copies a
subresource into (slot 8) and out of (slot 9) tiled memory, and reports the 64 KiB tiling (slot 10). Not modelled:
compression metadata planes, and addressing or copies for the pipe/bank XOR modes and MSAA (`E_NOTIMPL`). A description
it cannot lay out still gets a computer in the out pointer with `E_INVALIDARG`, because two of the four callers ignore
the HRESULT and call through the object at once; that object answers every slot with `E_NOTIMPL` and writes nothing.
`xg_xs_test` covers the model (hand-checked addresses, mip tails, a bijection and round trip over every element) and the
shim's slots. The table above was measured before this shim and still lists these two as unimplemented; they are served
now.
Set `LEONIDA_XG_TRACE=1` to log the first 1024 calls per entry point; the source notes are in
[xg_xs_THIRD_PARTY.md](../runtime/shims/xg_xs_THIRD_PARTY.md).

Only two of these DLLs have a shim in the repository (`xmem.dll` and `d3d12_xs.dll`), and they are not complete:
they hold 8 of the 30 unimplemented imports. The other 22 are in five DLLs (`xg_xs.dll`, `XCRDAPI.dll`, `chad.dll`,
`xapu.dll`, `AcpHal.dll`) with no shim at all.

The seven `xmem.dll` imports are served now (see the `xmem.dll` notes above), so the same dry run lists 23.

`--init` on the same files also exits 0 with nothing on stderr. Its `--trace` file has 15 lines: the four package DLLs'
`DllMain(DLL_PROCESS_ATTACH)` each return 1, the executable's two TLS callbacks run at process attach (reason 1) and
again at detach (reason 0), and there is no `exception`, `error` or `unimplemented_call` event. So none of the 30
unimplemented imports is reached by the TLS callbacks or the `DllMain`s.

The executable's audio imports `xapu.dll` (`XApuConnect`, `XApuDequeueResult`, `XApuDisconnect`,
`XApuEnqueueCommand`, `XApuGetResultCount`) are served by `runtime/shims/xapu_queue.cpp`; `chad.dll`
(`XDspActivate`, `XDspConnectWithMaximumStreamLimit`, `XDspDeactivate`, `XDspDisconnect`, `XDspSubmitCommand`) by
`runtime/shims/xapu_chad.cpp`. The shapes were read statically from the executable's call sites: one each, four for
`XApuEnqueueCommand`. `XApuConnect` reads the title's 24-byte `ConnectInput` (process 1 = Opus at 48 kHz, stream
count, commands per stream, a shared buffer, options) and returns `{shared buffer, result event}`; it never writes the
input. Activate, process, deactivate and reset commands are copied from the title's stack command, and each queues one
result in order. A process command decodes its Opus packet through `runtime/opus_decoder` (Rust over the BSD-3-Clause
`opus-pure`) into the slot's output, honouring first frame, frame count, output length and the deinterleaved option;
without the crate the result carries the table-of-contents frame count and silence. The layouts follow Marvisak/VIctory
`crates/victory-hle/src/modules/apu.rs` (GPL-3.0-or-later). `XDspActivate` writes its two outputs as pointers to zeroed
memory, which the title reads as "command done" after each `XDspSubmitCommand`; the DSP stays silent. `xapu_chad_test`
covers the layouts, the queue and the silent path; `xapu_opus_test` the decoded path. With `--trace`, the first call of
each, and the first command of each type, is an `audio_queue` event.
The table above was measured before this shim and still lists these ten as unimplemented; they are served now. The call shapes agree with `docs/audio_hal_exe_calls.md`, which was measured with `tools/import_call_sites.py`.

What this does not show: `--run` has not been run, so the entry point has not been called and no import is known to be
reached on the way to the first frame. A bound import is not a working one (the binding only says a function with
that name exists), and an unimplemented import stops the run only if the title calls it
([When an unimplemented import is called](#when-an-unimplemented-import-is-called)). The executable imports 25
functions from `ws2_32.dll` (23 by ordinal, plus `getaddrinfo` and `freeaddrinfo`). The `ws2_offline` shim now serves the
outbound and name-resolution calls among them, loopback only (see the Winsock2 section below), and the remaining
imports bind to the host's. `winhttp.dll` is served by the offline shim, which opens none. Known gap: `dnsapi.dll`
(`DnsQuery*`) is not shimmed, so a title that calls it reaches the host's resolver.

## Audio hardware (`AcpHal.dll`)

The game executable imports nine functions from `AcpHal.dll`, the console's audio hardware layer.
`runtime/shims/acphal.cpp` serves them; the argument shapes come from a static read of the executable's call sites
(one thunk per import, 16 call sites in all), and the names of the parameters and object slots are hypotheses.
`ApuCreateHeap` reserves one block of ordinary memory for both sizes it is given, `ApuAlloc` and `ApuFree` manage it
first fit, and an APU address is a fixed 32-bit base plus the offset in it, so `ApuMapVirtualAddress` and
`ApuMapApuAddress` are arithmetic and `ApuIsVirtualAddressValid` checks a live block and the alignment.
`AcpHalAllocateShapeContexts` fills zeroed 64-byte contexts per count, and `AcpHalCreate` returns an object whose
calls succeed and do nothing: there is no audio processor, so the game gets no sound from this path. Its boot checks
fail hard, which is why every call succeeds. `acphal_test` covers the call shapes.
The table under [The retail executable](#the-retail-executable-rt-5) was measured before this shim and still lists these nine as unimplemented; they are served now.

## Audio output off the console (WASAPI)

Off the console the title's sound goes out through WASAPI, not through the `AcpHal` path above: it creates an
`MMDeviceEnumerator` with `CoCreateInstance` (one call, `CLSCTX_ALL`, `IID_IMMDeviceEnumerator`), asks it for the
default render endpoint and activates `ISpatialAudioClient` on it (measured: the first interface the title asks the
device for). A console always has an output. A host with none
answers `GetDefaultAudioEndpoint(eRender, ...)` with `0x80070490` (`ERROR_NOT_FOUND`), and the title's device start
then takes an error path that leaves its own output-device lock held by a job worker: the audio engine never mixes, a
main-thread command buffer fills up, and the title stops on an `int3` about 47 s into the boot (measured under Wine with
its audio drivers disabled; refusing the enumerator itself stops it the same way, and a host null sink removes the stop).

So `CoCreateInstance` of that class goes to `runtime/shims/silent_audio.cpp` (hooked in `shims/com_shim.cpp`). When
the host's enumerator has a default render endpoint it is returned unchanged. When it has none, or cannot be made, or
`LEONIDA_AUDIO=silent` is set, the title gets an enumerator whose render answers are a **silent output endpoint** of the
runtime's own: `GetDefaultAudioEndpoint(eRender)`, `EnumAudioEndpoints(eRender)` (active devices) and `GetDevice` with
its ID give an active `IMMDevice` whose property store has a friendly name and the device format; `Activate` gives
`ISpatialAudioClient`, `ISpatialAudioClient2`, `IAudioClient`, `IAudioClient2` or `IAudioClient3` (any other interface
is `E_NOINTERFACE`, logged once). The spatial client has a 7.1 bed of static objects and no dynamic objects, as an
endpoint with no spatial sound format turned on; its objects are mono 48 kHz float, and its
`ISpatialAudioObjectRenderStream` signals the stream's event every 10 ms pass (480 frames) while it runs and hands
each object a scratch buffer between `BeginUpdatingAudioObjects` and `EndUpdatingAudioObjects`. The client
takes any PCM or float format in shared mode (event-driven or polled), and `GetService` gives `IAudioRenderClient`
(a scratch buffer), `IAudioClock`, `ISimpleAudioVolume` and `IAudioStreamVolume`. A clock consumes the written frames
in real time, so `GetCurrentPadding` drains, and an event-driven stream's event is signalled every 10 ms period
while it runs, so a title that paces its mixer on the event keeps running. Capture questions keep the host's answer
(with `LEONIDA_AUDIO=silent` the host's enumerator is only made when one comes). Endpoint notification callbacks are
accepted and never called. Nothing opens or touches a host audio device, and every sample is discarded. The first
silent device handed out prints one `[rt] audio: serving the silent output endpoint (<reason>)` line, the reason being
the host's failed call or `LEONIDA_AUDIO=silent`.

The mix format is 48 kHz, 32-bit float, 7.1 (eight channels, mask `0x63F`). 48 kHz float is the shared-mode
format of the Windows audio engine and of the console's. 7.1 is the console's case, and a hypothesis, not measured
here: the console's system mixer takes a 7.1 bed whatever output is plugged in and downmixes it for stereo, so a
console title is built for eight channels, where stereo would be the usual answer of a PC with headphones. The stream
itself takes any channel count, so a title that asks for fewer is not refused. `silent_audio_test`
covers the enumerator over stand-in hosts with and without an endpoint and with none, a render stream run for a
few periods with its event and padding checked, a spatial stream run for ten passes, and the `CoCreateInstance` hook
with `LEONIDA_AUDIO=silent`.

## Console paths and the write overlay

A title's file calls name the console's drives, so `GAME:\data\x.sps` is a file inside the installed game folder.
[ivall/xcl](https://github.com/ivall/xcl) (GPL-2.0-only; facts only, no code) documents for Xbox Series GDK titles that
`GAME:` maps to the game folder and that writes go to a writable overlay instead of the package, which stays read-only.
`gta6rt` therefore shims `CreateFileA/W`, `FindFirstFileA/W`/`FindNextFileA/W`/`FindClose`, `GetFileAttributesA/W`,
`GetFileAttributesExA/W` and `DeleteFileA/W` (`runtime/shims/file_system.cpp`), under `kernel32.dll`, `kernelbase.dll`
and the `api-ms-win-core-file-*` sets a GDK title links against.

- A `GAME:` path (any case, `\` or `/`) maps under `--game-dir`. A path without the prefix is a host path and is
  passed to the host unchanged. `--game-dir` defaults to the image's folder. `gta6rt` makes it and `--overlay`
  absolute at startup, so a bare image name (`gta6rt --run game.exe` from the game folder) and a relative folder
  both keep naming the same place after the title changes its current directory (CTest `run_tiny_game_dir`).
  `G:\` is the same folder as `GAME:`, and `T:\` is a scratch folder that `GAME:` does not see (RT-218; the other
  console layers map those two drives this way, and only the executable can say which spelling GTA VI uses). Each
  process has its own, `<overlay>\.gta6rt-temp\<YYYYMMDD-HHMMSS-mmm>-pid<id>` (start time in UTC and process id,
  `rt::fs::scratch_dir()`, named on the startup line), as the console's temporary drive is empty at launch and
  shared with no other run: with one folder for all runs, a `CREATE_NEW` of a name an earlier run left failed with
  `ERROR_FILE_EXISTS` (old upstream issue 7913). `rt::fs::reserve_unique_dir` reserves it once per overlay with a
  create-only `CreateDirectoryW`; a name that already exists (a reused process id in the same millisecond) is skipped
  for `-2`, `-3`, ... up to 1000 names, after which `T:` paths fail with `ERROR_INVALID_NAME` and the reason is
  printed once. Earlier runs' folders are kept, never reused, and `T:\..` cannot reach them. A path under
  `--game-dir` or the overlay is always a host path, so a package that sits on a host drive `G:` keeps working, and
  with no folders configured `G:` and `T:` stay host drives.
  `T:\` is a drive root like `GAME:\`: its folder is made on the first `T:` call, so it is a folder before anything
  is written there, and removing or moving it fails with `ERROR_ACCESS_DENIED`.
- An open that can write, and `DeleteFile`, use the overlay: `--overlay DIR`, default `<game-dir>\overlay`. A
  write-open of a package file copies it into the overlay first, so `OPEN_EXISTING` keeps its bytes. The copy is
  written under `<overlay>\.gta6rt-copy` and renamed into place, one copy-up at a time, so a process killed mid-copy
  or two threads opening the same file never leave a truncated copy under its name; a delete of a
  package file leaves an empty marker under `<overlay>\.gta6rt-deleted`, which reads and listings treat as gone. The
  package is never written.
- `DeleteFileA/W` checks the visible file's `FILE_ATTRIBUTE_READONLY`: a read-only package file with no overlay is
  refused with `ERROR_ACCESS_DENIED` before writing a deletion marker. An overlay copy supplies its own attributes,
  so a writable overlay can still be deleted when the package file underneath is read-only. The native A/W controls
  in `file_system_test` cover these cases and mixed hidden, system and archive attributes.
- Console file shims support resolved Windows paths beyond `MAX_PATH`. A short `GAME:`, `G:` or `T:` name can
  reach a file under a deep package or overlay root, with the same overlay and listing rules.
- Reads prefer the overlay copy and then the package. `FindFirstFile` merges the two folders, the overlay winning on a
  name, and hides deleted names, the marker folder and the copy folder.
- A path that climbs above `GAME:` with `..`, or names another drive, fails with `ERROR_INVALID_NAME`, so a title
  cannot reach outside the package and the overlay.

- `CreateDirectory*` makes the folder in the overlay. A name the title can already see, including a folder only the
  package has, fails with `ERROR_ALREADY_EXISTS`; a missing parent with `ERROR_PATH_NOT_FOUND`.
- `RemoveDirectory*` needs the folder empty in the title's view (`ERROR_DIR_NOT_EMPTY` otherwise). An overlay folder is
  removed; a package folder is hidden by a `.gta6rt-dir` marker inside its marker folder, and stays on disk. A marker
  folder on its own (left by a file deleted below it) does not hide anything. A remove and a write-open or
  `CreateDirectory*` inside the folder on another thread are ordered as on the host: one goes first, and the create
  fails with `ERROR_PATH_NOT_FOUND` or the remove with `ERROR_DIR_NOT_EMPTY`, never both succeeding.
- `MoveFile*` writes the destination in the overlay. A package file is copied there and its old name deleted by a
  marker; an overlay file or an overlay-only folder is renamed. An existing destination needs
  `MOVEFILE_REPLACE_EXISTING` even when only the package has it. Moving a package folder fails with
  `ERROR_ACCESS_DENIED` (as deleting one does), a move between `GAME:` and a host path with `ERROR_NOT_SAME_DEVICE`, and
  `MOVEFILE_DELAY_UNTIL_REBOOT` with `ERROR_INVALID_PARAMETER`.
- `CopyFile`, `CopyFileEx` and `CopyFile2` read the source the way a read does and write a `GAME:` destination in the
  overlay; fail-if-exists counts a name only the package has. Either side can be a host path. `CopyFile2` checks its
  parameter block before preparing the destination: one shorter than `COPYFILE2_EXTENDED_PARAMETERS` is
  `E_INVALIDARG` and writes nothing, as Windows answers. Where the host has no `CopyFile2` (Wine 9, whose loader binds
  the import to a stub that aborts the process), the shim copies with `CopyFileExW` instead: the flags both calls
  define and the cancel flag are passed on (a flag already set cancels before anything is written), CopyFile2-only
  requests that leave the contents as `CopyFileExW` writes them (no offload, compressed traffic, sparse copy, ...)
  are dropped, and a `COPYFILE2` progress routine is not called. A call it can't run as asked (`SKIP_ALTERNATE_STREAMS`,
  `DIRECTORY`, `OPEN_AND_COPY_REPARSE_POINT`, `IGNORE_SOURCE_ENCRYPTION`, an unknown flag, or a longer V2 block) is
  `ERROR_NOT_SUPPORTED` and writes nothing. `rt::fs::force_copy_file2_fallback` makes the test take this path on
  Windows too. A cancel flag that is already set is answered before the host's `CopyFile2` is called as well, since
  Wine 10 and 11 export `CopyFile2` but ignore the flag; clearing it lets the same call be retried.
- `SetFileAttributes*` sets the attributes on the overlay copy, copying a package file up (or mirroring a package
  folder) first, so the package keeps its own.
- A folder has to exist in the title's view before it is used. An open that writes fails with `ERROR_PATH_NOT_FOUND`
  when its folder is missing, was removed, or is a file, and creates nothing (`T:` is the exception: a create there
  makes its parent folders); `FindFirstFile` fails the same way for a
  listing inside such a folder (a pattern that matches nothing in a folder that exists is still
  `ERROR_FILE_NOT_FOUND`, and a listing directly below a file is `ERROR_DIRECTORY`, as on the host). `DeleteFile*`,
  `RemoveDirectory*`, `SetFileAttributes*` and the source of `MoveFile*` answer a name inside such a folder with
  `ERROR_PATH_NOT_FOUND` too, and a missing name in a folder that exists with `ERROR_FILE_NOT_FOUND`, as on the host.
- `MoveFile*` onto the same name succeeds and changes nothing. A rename that changes only the case renames the overlay
  entry; a package file gets an overlay copy under the new spelling, and a package folder is refused like any other
  move of one.

`runtime/test/file_system_test.cpp` (ctest `file_system`) covers this over folders it makes in `%TEMP%`.

## Console identity in the registry

GTA VI does not only ask `XSystemGetDeviceType` which console it runs on. Code linked into the executable opens
`HKEY_LOCAL_MACHINE\Software\Microsoft\Windows NT\CurrentVersion\OEM`, reads the `DWORD` value `ConsoleMode`
(`0x64` Series X, `0x60` Series S) and closes the key. A missing value means a PC, and a PC takes the PC memory
budgets and resolution, so without an answer the title does not run as the console build. It decides the console
model, and with it the back buffer size (1920x1080 for Series S, 3840x2160 for Series X).

Measured with `tools/pe_inspect.py` on the retail 1.0.2.1 package:

| Module | Imports | From |
|---|---|---|
| `game_scarlett_master.exe` | `RegOpenKeyW`, `RegQueryValueExW`, `RegCloseKey` | `ADVAPI32.dll` |
| `libHttpClient.GDK.dll` | the same three and `RegOpenKeyExW` | `ADVAPI32.dll` |
| `XCurl.dll` | `RegOpenKeyExW`, `RegQueryValueExW`, `RegCloseKey`; `RegOpenKeyW` | `api-ms-win-core-registry-l1-1-0.dll`; `-l2-1-0.dll` |

Before `runtime/shims/console_registry.cpp` the loader bound all of them to the host's advapi32 (`pe_inspect` labels
them `shim` only because it ran on Linux, with no `System32` to find them in). Under Proton the lead wrote the value
into the Wine prefix; on a Windows PC the key does not exist and writing it needs administrator rights.

`console_registry.cpp` now serves the four functions under `advapi32.dll` and the registry API sets. It answers only
`HKLM\Software\Microsoft\Windows NT\CurrentVersion\OEM` / `ConsoleMode`, with a pseudo key; every other key,
value and handle goes to the host's advapi32 unchanged. It follows the `RegQueryValueExW` contract for the value:
`REG_DWORD` is reported when the type pointer is set, a null data pointer is a size query (4 bytes), a buffer under
4 bytes gives `ERROR_MORE_DATA` with the size, and a data pointer without a size is `ERROR_INVALID_PARAMETER`.

One setting drives both answers (`runtime/shims/console_identity.h`): `LEONIDA_DEVICE_TYPE` 6 gives `ConsoleMode`
`0x60`, 7 (also the default) gives `0x64`, 8 (devkit) gives `0x68`. For the types that have no value (0, 1, 2 to 5) the
key is not ours and the host registry answers, so a missing key stays missing, as on a PC. The first answer of each
value is logged once: `[rt] registry: ConsoleMode answered 0x64 (LEONIDA_DEVICE_TYPE=7)`. `advapi32.dll` is a host
module in `rt::shims::owns_dll`: `LoadLibrary("advapi32.dll")` still returns the real module.

Credit: the pseudo key design and the value numbers follow VIctory `crates/victory-hle/src/modules/xsystem.rs`
(`mod device_type`, GPL-3.0-or-later, commit 2849477); `0x68` for the devkit follows xbox-emulator
`crates/compat/xpal` and `compat-common/src/gdk/hook.rs` (GPL-3.0, commit 0284e83). The test is
`runtime/test/console_registry_test.cpp` (`ctest -R console_registry`).

## When an unimplemented import is called

The table above has no unimplemented imports, but the game executable will. A call to one used to return 0
silently, which for an `HRESULT` function is `S_OK` with its out parameters never written; the caller then carries on
with stale pointers and fails somewhere unrelated. `LEONIDA_UNIMPLEMENTED` chooses what the thunk does instead
(details in `runtime/README.md`):

- `log` (default): print the name once, return 0. Unchanged.
- `fail`: return `E_NOTIMPL` for imports known to return `HRESULT`, 0 otherwise.
- `stop`: print the import, the caller as `module+RVA` and rcx/rdx/r8/r9, then break (debugger) or exit with code 3.

The `--dry-run` report ends with `unimplemented by return policy: hresult=N other=N`, so the first run against the
executable shows how many of its missing imports `fail` would turn into errors.

The `HRESULT` list is by DLL and name: `D3D12Create*` and `D3D12Serialize*` (`d3d12.dll`, `d3d12_xs.dll`),
`D3D12GetDebugInterface`, `D3D12EnableExperimentalFeatures`, `CreateDXGIFactory*`, `DXGIGetDebugInterface1`,
`DXGIDeclareAdapterRemovalSupport` (`dxgi.dll`), and in `xgameruntime.dll` `XGameRuntimeInitialize`, `XGameEventWrite`
and the families `XAsync*`, `XTaskQueue*`, `XUser*`, `XStore*`, `XGameSave*`, `XPackage*`, `XGameUi*` except the 33
functions their public reference pages show returning `void`, `bool` or `int` (for example `XTaskQueueDispatch`,
`XUserCloseHandle`, `XStoreIsLicenseValid`). The mixed `XSystem*`, `XGameStreaming*`, `XThread*` and `XDisplay*`
families are not listed. The sources are the Microsoft Learn pages for the D3D12/DXGI functions and the GDK reference
(`gaming/gdk/docs/reference/system/<family>/functions/<name>`); `D3D12XboxCreateDevice` is matched by name from the
measurement in `d3d12_vulkan.md`, because its page could not be found.

### Delay imports and thread events: where the loader differs from Windows

- **Delay imports are bound eagerly.** `Loader::bind_imports` (`runtime/src/pe_loader.cpp`) binds the delay-import
  table at load time, like the import table, so `--dry-run` shows the whole import surface. A delay-loaded DLL or
  function that nothing serves becomes an unimplemented thunk, which follows `LEONIDA_UNIMPLEMENTED` at its first
  call (return 0 by default). On Windows the delay-load helper raises `0xC06D007E` (module not found) or
  `0xC06D007F` (procedure not found) at that call instead, and a title can catch it and take a fallback; that fallback
  never runs here. `--trace` writes a `delay_import_unimplemented` event (`module`, `import`) for each such import
  when the image is loaded.
- **Thread events reach TLS callbacks in TLS slot order.** A new thread's `DLL_THREAD_ATTACH` runs the mapped
  modules' TLS callbacks in the order of their TLS slots (`DLL_THREAD_DETACH` in reverse). A module takes the lowest
  free slot when it is mapped, before its imports are loaded, so the executable's callbacks run before those of the
  DLLs it imports; a slot freed by an unloaded module goes to the next one. Windows does not document its order, so a
  title that depends on DLL-first order would see a difference here. Mapped DLLs' `DllMain` gets no thread events
  (`runtime/README.md`, PE loader).

## Run-time loaded modules

Not every console module is reached through an import table. `libHttpClient.GDK.dll` and `XCurl.dll` both use the
GDK runtime, but `dumpbin /imports` shows **no import of `xgameruntime.dll`** in either. Both carry the wide string
`xgameruntime.dll` and the ASCII names `InitializeApiImpl`, `InitializeApiImplEx`, `InitializeApiImplEx2`,
`QueryApiImpl`, `UninitializeApiImpl` and `XErrorReport` (one block of strings in each DLL), next to a statically
linked `XGameRuntimeInitialize`. So `xgameruntime.lib`'s thunks `LoadLibraryW` the runtime and `GetProcAddress` those
six names at run time. A runtime without `XErrorReport` is refused before any initializer is called: with only the
five entry points served, the package's own libHttpClient `HCInitialize` returned `0x89240101`
(`E_GAMERUNTIME_DLL_NOT_FOUND`) and XCurl's `curl_global_init` returned `CURLE_FAILED_INIT`, both called through
`gta6rt --run` by a stand-in program that loads the two DLLs from the package. xodus-gaming/xgameruntime-docs
(GPL-3.0) documents the same mechanism for PC GDK titles: `InitializeApiImplEx2` is what
`XGameRuntimeInitialize[WithOptions]` calls, and `QueryApiImpl` hands out the runtime's COM classes, whose methods
mostly correspond one to one to the `xgameruntime.lib` functions. The executable is very likely linked the same way.

Bound to the host, those calls would load the PC's Gaming Services runtime, or fail, and never reach the offline
shims. `runtime/shims/library_loader.cpp` therefore answers `LoadLibrary*` and `GetModuleHandle*` for a module the
shim registry owns (`rt::shims::owns_dll`: `xgameruntime.dll`, `d3d12_xs.dll`, `xmem.dll`, `xg_xs.dll`, `gameinput.dll`, and the
DirectStorage modules `dstorage_xs.dll`, `xcrdapi.dll` and `dstorage.dll`, [directstorage.md](directstorage.md)) with a
pseudo-handle, and `GetProcAddress` on it resolves through the registry. A name the registry doesn't serve is `NULL`
with `ERROR_PROC_NOT_FOUND` and is logged once. Every other module goes to the host unchanged.

`runtime/shims/xgameruntime.cpp` serves the five entry points and `XErrorReport(HRESULT, const char*)`, the same
function as `IXErrorImpl` slot 3 (below), so a report reaches the callback `XErrorSetCallback` set either way. The
initializers succeed offline and log the GDK version and mode once. `QueryApiImpl` serves `XThreadingImpl`,
`XErrorImpl`, `XNetworkingImpl`, `XSystemImpl`, `XPersistentLocalStorageImpl`, `XPackageImpl`, `XGameImpl`,
`XGameRuntimeFeatureImpl` and `XSystemAnalyticsImpl` (below), and `XUserImpl`, `XGameSaveImpl`, `XStoreImpl`,
`XGameEventImpl`, `XGameUiImpl`, `XGameStreamingImpl`, `XDisplayImpl`, `XUserDeviceImpl`, `XAccessibilityImpl` and
`XLauncherImpl` from xgameruntime-docs' layout (below); for any other class it logs
the class and interface id and returns `0x80070032`, the documented error for a class the runtime doesn't provide, so
the first boot with the executable lists the classes to implement next. `xgameruntime_test` drives this path through
the api-set binding `XCurl.dll` uses, and calls the classes `xgameruntime.cpp` serves slot by slot.

`XCurl.dll` has a second way in. One of its functions reads `HKLM\Microsoft\GamingServices` value
`ServiceRetryTimeout`, then `LoadLibraryExW(L"gameplatformservices.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)` and
`GetProcAddress`es `GetApiVersionImpl` and `QueryApiImpl`. It calls the first through the CFG dispatch with two
pointers to 64-bit locals (the result is not read), compares the pair with the one it saw last and keeps it. On the
host that load would find a PC's Gaming Services runtime, which bypasses the offline classes, or nothing, in which
case XCurl returns `0x87E00196`. The shims own `gameplatformservices.dll` too: its `QueryApiImpl` is the offline
one, and `GetApiVersionImpl(uint64_t*, uint64_t*)` writes the same two values every time. `curl_global_init` also
asks that module for `InitializeApiImpl`, which is not served; the call succeeds without it.

### Class layouts, measured

Each `xgameruntime.lib` function the package's DLLs link is a thunk of the same shape: it calls a helper with the
class id (passed as both the class and the interface id), which initializes the runtime once and calls
`QueryApiImpl`; then it calls one vtable slot with `this` followed by the function's own arguments, and calls
`Release` (slot 2). The helper treats `0x80040154` (class not registered) from `QueryApiImpl` specially: it turns it
into `0x89240107` and calls an error hook; any other failure is returned as is.

The slots below were read from the thunks statically linked into `libHttpClient.GDK.dll` and `XCurl.dll`
(disassembled with `llvm-objdump`; no code was run, nothing else read). Every slot called matches, in argument
count and width, the function at that position of xgameruntime-docs' "Layout" list after IUnknown's three slots:

| Class | Slots called | Checked against the documented order |
|---|---|---|
| `XThreadingImpl` (`IXThreadingImpl`, 28 methods, slots 3-30) | 3-7 and 9-30 (`libHttpClient.GDK.dll`), 3, 5, 12, 16-18, 22, 29 (`XCurl.dll`) | e.g. 7 `XAsyncBegin` (5 args), 9 `XAsyncSchedule` (pointer, 32-bit), 10 `XAsyncComplete` (pointer, HRESULT, size), 12 `XTaskQueueCreate` (two 32-bit modes, pointer), 16 `XTaskQueueDispatch` (returns a byte), 19 `XTaskQueueSubmitDelayedCallback` (5 args), 22 `XTaskQueueTerminate` (pointer, bool, two pointers), 27 `XThreadSetTimeSensitive` (bool), 30 `XThreadIsTimeSensitive` (returns a byte). Slot 8 (`XAsyncBeginAlloc`, private) is never called; slot 28 (`XTaskQueueIsEmpty`, private) takes no arguments |
| `XErrorImpl` (`IXErrorImpl`, slots 3-5) | 3 (`XCurl.dll`) | slot 3, the docs' `_unknown_`, takes an HRESULT and a pointer: `XErrorReport(HRESULT, const char*)` |
| `XNetworkingImpl` (`IXNetworkingImpl2`, slots 3-20) | 11-17 (`XCurl.dll`), 3-20 (`libHttpClient.GDK.dll`, each in its documented argument shape, e.g. 18-20 (enum, u64 pointer / u64 / pointer)) | the Utf16 `XNetworkingQuerySecurityInformationForUrl*` trio, `XNetworkingVerifyServerCertificate`, `XNetworkingGetConnectivityHint`, `XNetworkingRegister/UnregisterConnectivityHintChanged` (pointer, bool). Served offline, asked for with `IXNetworkingImpl2`'s id |
| `XSystemImpl` (`IXSystemImpl5`, slots 3-8 known) | 3-8 (`libHttpClient.GDK.dll`) | 3-5 `XSystemGetConsoleId` / `GetXboxLiveSandboxId` / `GetAppSpecificDeviceId` (size, pointer, pointer each), 6 `XSystemHandleTrack` (two pointers, IXSystemImpl3), 7 `XSystemIsHandleValid` (pointer), 8 `XSystemAllowFullDownloadBandwidth` (bool, IXSystemImpl4); IXSystemImpl2 and 5 add no known method. Served offline |
| `XPersistentLocalStorageImpl` (`IXPersistentLocalStorageImpl3`, slots 3-8) | none called (the table of ids only) | slots 3-7 are `XPersistentLocalStorageGetPathSize` / `GetPath` / `GetSpaceInfo` / `PromptUserForSpaceAsync` / `PromptUserForSpaceResult`, slot 8 `XPersistentLocalStorageMountForPackage` (IXPersistentLocalStorageImpl3). Slots from the public xgameruntime-docs and Wine IDL, not measured in a caller. Served offline: the folder is `<overlay>\.gta6rt-persistent`, which `GAME:` does not see, the quota is the 2048 MB of `MicrosoftGame.Config`, a prompt for space the quota holds succeeds, and `MountForPackage` is `E_NOTIMPL` |
| `XPackageImpl` (`IXPackageImpl3`, `IXPackageImpl4`) | 3-19, 22, 24-27, 30, 32-34, 37-40 (`libHttpClient.GDK.dll`); 3-7, 9, 12, 14, 16-19, 25, 26, 34, 37-39 (the retail executable, as `IXPackageImpl4`) | slots 3-34 are IXPackageImpl's 32 entries, 35-38 IXPackageImpl2's 4, 39-40 IXPackageImpl3's 2, 41 IXPackageImpl4's `XPackageGetPackageKind`. Served offline: 3 identifier, 4 packaged process, 5-10 installation monitor (create, close, progress complete, no update, progress registration, unregister), 11 user locale, 12-19 chunks of the fully installed package (every selector Ready, declared languages enumerated, install order / install / async install complete at once with a monitor, 0-byte download estimate, uninstall accepted with nothing removed; ported from VIctory's xpackage.rs), 22 unregister installed, 23-26 mount / path size / path / close, 34 uninstall package, 37-38 mount with UI (a mapped content package mounts, any other completes with `E_GAMEPACKAGE_NO_PACKAGE_IDENTIFIER`), 39 `XPackageEnumeratePackages` (S_OK, the callback never runs: no content packages to list; VIctory answers the title's call the same way); the rest log once and return `E_NOTIMPL` |
| `XGameImpl` (`IXGameImpl3`, slots 3-5) | none in the package's DLLs (`libHttpClient.GDK.dll` holds its id only in the linked id table); the executable's requests are unmeasured | from the xgameruntime IDL: 3 `XGameGetXboxTitleId` (pointer), 4 `XLaunchNewGame` (two strings and a user handle, `void`), 5 `XLaunchRestartOnCrash` (string, u32). Served offline: the title id is `MicrosoftGame.Config`'s `<TitleId>` (`E_FAIL` without one), a new game launch is logged and ignored, a crash restart succeeds with nothing registered |

**Two layout sources, and where they differ.** Besides xgameruntime-docs (GPL; facts only), the LGPL
`include/*.idl` of xodus-gaming/xgameruntime (from the Wine project) declares each interface with its methods in
order. Compared class by class, the two agree exactly wherever the docs list no `_unknown_` slot: `XGameSaveImpl`'s 30
methods, `XGameImpl`'s 3, `XStoreImpl`'s 81 (to `IXStoreImpl6`). Where the docs list one, the IDL is not consistent:
for `XErrorImpl` it keeps the slot as `HRESULT __PADDING__()`, so the two still line up, but for `XUserImpl` (the
docs' slot 15) and `XDisplayImpl` (slot 3) it has no entry at all, and every later method sits one slot earlier than
the docs put it. Take a layout from the IDL only where it matches the docs' slot count, and read an IDL interface that
skips one of the docs' unknown slots as missing it, not as proof the slot doesn't exist. The measured slots in the
table above all follow the docs.

`XThreadingImpl` forwards each slot to `third_party/xtaskqueue`, the same implementation the named exports use;
`XThreadSetTimeSensitive`/`XThreadIsTimeSensitive` keep a per-thread flag and the two private slots log once and
answer 0. `XErrorImpl` logs each report and hands it to the callback `XErrorSetCallback` set.
`XNetworkingImpl` answers as a console without a network would: the connectivity hint is initialized with no
connectivity (level None), the port and URL security-information queries fail with
`HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE)` (the `*Async` ones complete with it through XAsync), the
certificate check fails the same way, the two change registrations send the one initial notification Microsoft
documents (on the queue's completion port, the process queue when none is given: the hint above, or port 0) and
none after, and the three `IXNetworkingImpl2` additions are `E_NOTIMPL`.

`XUserImpl` (`runtime/shims/xgameruntime_user.cpp`) is served from xgameruntime-docs' layout alone. No package DLL
calls an `XUser*` slot (`libHttpClient.GDK.dll` only holds the GUID table). The executable links thunks for
`IXUserImpl` slots 3-5, 7-14, 19-25 and 33-34 and `IXUserGamertagImpl` slot 3 (`tools/gdk_runtime_calls.py`, from the
file alone): none of the slots logged on first use below, and of the offline failures only the token-and-signature
trio (23-25). What the slots must answer is still **not measured**; that is the executable's first boot.
- **Interfaces:** the class id doubles as `IXUserImpl`'s; `IXUserImpl2`-`6` answer with the same 50-slot vtable;
  `IXUserGamertagImpl` (`XUserGetGamertag`) is a second interface.
- **The user:** one offline local user, always signed in. `XUserAddAsync` adds it with or without
  `AddDefaultUserSilently` (with `AllowGuests` as well, it's `E_INVALIDARG`, as documented). It has fixed non-zero user
  and local ids, is not a guest, is an adult, and has every privilege. Its gamertag comes from `LEONIDA_GAMERTAG` (15
  printable ASCII characters at most), `Player` otherwise.
- **Handles:** each one is its own allocation, closed once by `XUserCloseHandle`. `XUserCompare` is 0 for any two
  handles, since they all name the one user.
- **Not available offline:** token, gamer-picture, MSA-token and resolve-issue requests fail synchronously before
  starting (`E_GAMEUSER_FAILED_TO_GET_TOKEN`, `E_FAIL`, `E_GAMEUSER_FAILED_TO_RESOLVE`), so no `XAsyncBlock` waits.
  `XUserGetSignOutDeferral` is `E_GAMEUSER_DEFERRAL_NOT_AVAILABLE`, and change callbacks are only tracked as tokens,
  because the user never changes.
- **Logged on first use:** the docs' unknown slot 15 (answers `E_NOTIMPL`, so nothing reads outputs it didn't
  write), the PC-only `IXUserImpl5` slots and `IXUserImpl6`'s undocumented sign-out trio (`XUserIsSignOutPresent`, a
  bool, answers false).
- **Validity:** handles are looked up in a live set, never read, so a closed or foreign pointer is refused without
  touching its memory, and a double close does nothing. Unregistering a change callback is never pending, so it
  returns true for any token. `XUserIsStoreUser` is true, as documented for console.
- **First calls:** every served slot writes one `[rt] xgameruntime: XUser <name> first call: ...` line with its
  arguments and answer (`XUserGetGamertag` one per component, with the text it returned), so a boot log shows whether
  the title added its user and read the gamertag its player-select prompt shows.

`xgameruntime_user_test` calls every served slot with the documented arguments and reads back the first-call lines.

`XGameSaveImpl` (`runtime/shims/xgameruntime_gamesave.cpp`) is served from the same layout, also **not measured**:
- **Interfaces:** the class id doubles as `IXGameSaveImpl`'s; `IXGameSaveImpl2` (adds the three `XGameSaveFiles`
  slots, 30-32) and `IXGameSaveImpl3` (no methods of its own) answer with the same 33-slot vtable.
- **Storage:** `LEONIDA_SAVE_DIR\local-user\<container>\<blob>`, defaulting to `%LOCALAPPDATA%\Leonida\saves`,
  names %-encoded: the layout `tools/saves.py import` writes ([saves.md](saves.md#importing-into-the-runtime)).
  `XGameSaveFilesGetFolderWithUi*` gives `<root>\files\<configId>`.
- **Updates:** writes and deletes are buffered; `XGameSaveSubmitUpdate` writes each new blob beside its final name,
  then renames it over, so a failed write leaves the container as it was. One write or delete per blob per update;
  an update past 16 MB (`GS_MAX_BLOB_SIZE`) is `E_GS_UPDATE_TOO_BIG`.
- **Names:** container names follow `XGameSaveCreateContainer`'s documented rule (`E_GS_INVALID_CONTAINER_NAME`
  otherwise).
- **Reads:** `XGameSaveReadBlobData` fills the title's buffer with the `XGameSaveBlob` array, then each blob's bytes
  and name, which the array points into; a buffer too small is `E_GS_PROVIDED_BUFFER_TOO_SMALL`, a missing blob
  `E_GS_BLOB_NOT_FOUND`. The async read's result size (`XAsyncGetResultSize`) is the buffer it needs.
- **ASSUMPTION:** a fixed 1 GiB quota, blob names of at most 256 bytes, `blobNames = nullptr` reads every blob,
  and `XGameSaveGetContainerInfo` of a missing container returns `S_OK` without a callback. `syncOnDemand` is
  ignored: nothing syncs offline.

`LEONIDA_TRACE_XGAMESAVE=1` logs every call of these slots as one `[rt] xgamesave:` line on stderr, with the
milliseconds since the process started and the thread: the handles, container and blob names, sizes and
HRESULT, never the blob contents. An async call (`InitializeProvider`, `GetRemainingQuota`, `DeleteContainer`,
`ReadBlobData`, `SubmitUpdate`, `FilesGetFolderWithUi`) logs four more lines: `schedule` when `XAsyncSchedule` is
called, `run` when the work starts on the title's queue (`waited=` is the gap), `complete` with its HRESULT and
the `...Result` call that collects it. A job that waits behind a stalled queue, or a result the title never
collects, then shows in a run's log. Unset, nothing is logged beyond the one line when the provider opens.

`xgameruntime_gamesave_test` drives every slot against a temporary `LEONIDA_SAVE_DIR`. With the trace on, it
checks the lines of a write and a read.

`XStoreImpl` (`runtime/shims/xgameruntime_store.cpp`), from the same layout; the game-license struct is measured
on the executable, the rest is not:
- **Interfaces:** the class id doubles as `IXStoreImpl`'s; `IXStoreImpl2`-`6` answer with the same 84-slot vtable.
- **Offline answers:** the game license is active and not a trial; a package's license is acquirable and valid;
  there are no add-on licenses, no products in any query and no package updates. License-changed and license-lost
  registrations hand out tokens; the callbacks never run.
- **Not available offline:** purchases, consumables, license tokens, collections and purchase ids, downloads and
  every store UI fail before starting (`E_FAIL`, logged once). The docs' three unnamed slots and
  `XStoreIsAvailabilityPurchasable` (no documented signature) answer `E_NOTIMPL`.
- **Measured on the executable:** `XStoreGameLicense` is 104 bytes. The title's `XStoreQueryGameLicenseResult`
  call passes a 104-byte stack buffer and reads the SKU id as a C string at +0x00, the three booleans at
  +0x13..+0x15, the trial id as a C string at +0x1C and the expiration time at +0x60. That fixes
  `STORE_SKU_ID_SIZE` = 18 and the 104-byte total.
- **UNVERIFIED:** `SKU_ID_SIZE` = 5 and the exact `TRIAL_UNIQUE_ID_MAX_SIZE` = 64. The executable bounds the
  trial id to at most 68 bytes (the expiration time is at +0x60) but reads it as a C string, so it does not name
  the bound; 64 is the public source's value. It does not call either can-acquire slot (24-27), so
  `XStoreCanAcquireLicenseResult` (12 bytes) is unmeasured.

`xgameruntime_store_test` drives the license check, add-ons, updates, package licenses and product queries.

`XGameEventImpl`, `XGameUiImpl`, `XGameStreamingImpl`, `XDisplayImpl`, `XUserDeviceImpl`, `XAccessibilityImpl` and
`XLauncherImpl` (`runtime/shims/xgameruntime_offline.cpp`), from the same layout, also **not measured**:
- **`XGameEventWrite`** succeeds; the events are dropped. The first one's name is logged, and
  `LEONIDA_LOG_GAME_EVENTS=1` logs each with its dimensions and measurements.
- **Dialogs:** a message dialog's title, text and buttons go to the log (`[rt] game dialog:`), and it completes at
  once with its default button; an error dialog's HRESULT and context are logged and it is dismissed. So an error
  the game would show on screen at boot is readable in the log. Every other UI (profile cards, pickers, text entry,
  web authentication, invites, state share) fails before starting (`E_FAIL`); the docs' unnamed slots and the
  PC-hosted UI callbacks of `IXGameUiImpl4` answer `E_NOTIMPL`.
- **Streaming:** never streamed. `XGameStreamingInitialize` succeeds, `XGameStreamingIsStreaming` is false, there
  are no clients, and the connection-state registration hands out a token that never fires. Every per-client slot
  answers `E_GAMESTREAMING_NOT_INITIALIZED` (0x89245400), which a caller reading a bool sees as false (low byte
  0).
- **Not reached by the executable:** the parameter lists of `XGameUiShowMessageDialogAsync` and
  `XGameStreamingRegisterConnectionStateChanged` are the GDK reference's, and the executable has no caller to check
  them against. `tools/gdk_runtime_calls.py` finds no thunk for the message dialog and none for any
  `XGameStreamingImpl` or `XUserDeviceImpl` slot. Of `XGameUiImpl` it links slots 7-8 and 17-19 only: the profile
  card and text entry, which fail before starting.

- **Accessibility and speech** (`IXAccessibilityImpl2`, slots 3-21): `XHighContrastGetMode` (5) answers Off and
  `XClosedCaptionSetEnabled` (4) is accepted. One silent voice is installed:
  `XSpeechSynthesizerEnumerateInstalledVoices(context, callback)` (8) calls back once, with voice id `leonida-silent`
  and language `en-US` (the GDK reference's `XSpeechSynthesizerVoiceInformation` layout), and returns `S_OK`
  (`E_INVALIDARG` without a callback). `XSpeechSynthesizerCreate` (9) hands
  out a synthesizer handle that speaks with it (`E_POINTER` for a null out pointer). Close (10), set default voice (11)
  and set custom voice (12, only for that voice id) accept that handle and refuse any other with `E_INVALIDARG`.
  Streams from text (13) and SSML (21) are created from it and return a
  stream handle; its close (14) accepts it. Stream size (15) reports 44 bytes and stream data (16) copies them: a valid
  RIFF/WAVE header for 16 kHz, 16-bit mono PCM with zero data bytes, so a title that plays narration reads a silent
  stream (a buffer under 44 bytes gets `ERROR_INSUFFICIENT_BUFFER`). A foreign handle on 15 or 16 gets `E_INVALIDARG`,
  outputs zeroed. The closed-caption properties (3) and the speech-to-text display calls (6, 7, 17-20) fail with
  `E_FAIL`, writing nothing: their parameter lists are not in the docs' layout.
- **`XLauncherImpl`** (`IXLauncherImpl`, slots 3-5): `XLaunchUri` fails with `E_FAIL`; nothing is launched.
  `XDisplayAcquireTimeoutDeferral` (4) creates a Win32 `PowerRequestDisplayRequired` request and returns its handle (a
  failing `PowerCreateRequest` or `PowerSetRequest` returns the Win32 error as an HRESULT).
  `XDisplayCloseTimeoutDeferralHandle` (5) clears that request and closes the handle, so the display may sleep again.
  Side effect on the host: while the title holds a deferral, Windows keeps the PC's display on.
- **ASSUMPTION:** the parameter lists of the speech calls and `XHighContrastGetMode` (Off is 0), from the GDK
  reference; only the argument counts were measured, from `tools/gdk_runtime_calls.py` on the executable.
- **HDR:** `XDisplayTryEnableHdrMode` answers Disabled, because the host output is SDR. With `LEONIDA_HDR=1` it
  answers Enabled with the documented default tone-map luminances (0.01, 1000, 1000 nits).
- **Devices:** every device belongs to the offline user (`XUserFindForDevice` hands out an `XUserHandle` the title
  closes). `XUserRegisterForDeviceAssociationChanged` (slot 4) and
  `XUserRegisterForDefaultAudioEndpointUtf16Changed` (7) keep each registration (its queue, duplicated, its context
  and callback) under a token unique across both kinds and never 0; a null callback or token is `E_INVALIDARG`, and
  queue and context are optional. The matching unregister (5, 8) removes it at once, with or without `wait`, and
  returns true for every token, as `XUserUnregisterForChangeEvent` does: a token already removed, never handed out or
  of the other kind changes nothing. Offline the device association and the default audio endpoint never change, so
  no event is raised and no callback is ever queued or called. The user has no audio endpoint of its own
  (`ERROR_NOT_FOUND`). Finding a controller shows no UI and gives an all-zero device id, as
  documented for desktop. `APP_LOCAL_DEVICE_ID` is taken to be 32 bytes; no caller in the executable confirms it.

`xgameruntime_offline_test` drives the five classes; `xgameruntime_offline_hdr` runs it again with
`LEONIDA_HDR=1`.

Two more classes are served from `tools/gdk_runtime_calls.py`'s measurement of `libHttpClient.GDK.dll` (it finds the
thunks in the code without running it and prints, per class, the interface id asked for, the slots called and the
argument count each passes after `this`). Their thunks are in the DLL but nothing in it calls them; a title linked with
the same `xgameruntime.lib` gets the same thunks. Every slot is the function at that position of xgameruntime-docs'
layout with the documented argument count, and each behaves as Microsoft's reference page for that function says
(`runtime/shims/xgameruntime.cpp` cites both):

| Class | Interface asked for | Slots (args after `this`) | Offline behaviour |
|---|---|---|---|
| `XGameRuntimeFeatureImpl` | its class id | 3 `XGameRuntimeIsFeatureAvailable` (1, returns `bool`) | True when `QueryApiImpl` serves the class the feature's functions go through (whichever `xgameruntime*.cpp` serves it) and the feature is documented for Windows: today XAccessibility, XSpeechSynthesizer, XLauncher, XAsync, XAsyncProvider, XTaskQueue, XThread, XError, XGame, XGameSave, XGameUI, XNetworking, XPackage, XPersistentLocalStorage, XStore, XSystem, XUser, XGameEvent, XGameStreaming. False for XDisplay (served, but documented for the consoles only), for the classes not served (XAppCapture, XGameInvite) and for values past the enum |
| `XSystemAnalyticsImpl` | its class id | 3 `XSystemGetAnalyticsInfo` (1: the result's address, as MSVC passes a returned struct to a member function) | The named export's `XSystemAnalyticsInfo` |

Not served yet, though `libHttpClient.GDK.dll` has thunks for them: `XAppCaptureImpl` and `XGameInviteImpl`,
among others.

Which classes the package's GDK modules reference, from a byte search for the class and interface GUIDs that
xgameruntime-docs lists (16-byte in-memory form, every `.dll` of the package; counts only, nothing else read):

| Module | Runtime classes referenced (highest interface id present) |
|---|---|
| `XCurl.dll` | `XThreadingImpl`, `XErrorImpl`, `XNetworkingImpl` (`IXNetworkingImpl2`) |
| `libHttpClient.GDK.dll` | 23 of the 24 documented classes (not `XGameActivationImpl`), each GUID once: the ids of the `xgameruntime.lib` thunks the incremental link kept, 404 of them. `XThreadingImpl` (27 thunks), `XPackageImpl` (30), `XNetworkingImpl` (18) and `XSystemImpl` (6) are the ones read by hand above; `XErrorImpl` (3) and 18 more classes have thunks too. Only `XThreadingImpl`'s and `XErrorImpl`'s are called ([below](#measuring-which-runtime-classes-a-binary-calls)). Highest interfaces: `IXSystemImpl5`, `IXUserImpl5` (not 6), `IXGameImpl3`, `IXStoreImpl6`, `IXPackageImpl3` (not 4), `IXGameUiImpl4`, `IXGameSaveImpl3`, `IXGameStreamingImpl3`, `IXPersistentLocalStorageImpl3`, `IXUserDeviceImpl2`, `IXNetworkingImpl2` |
| every other `.dll` | none |

So `XCurl.dll` needs threading, error reporting and networking from the runtime, and the interface versions in
`libHttpClient.GDK.dll` date the GDK the package was built with: newer than the first `IXUserImpl5`, older than
`IXUserImpl6`.

### The retail executable's runtime classes (RT-5)

`python -I tools/gdk_runtime_calls.py game_scarlett_master.exe` (from the file alone, nothing executed; `main`
`1cc6e3c1`) finds one `QueryApiImpl` helper at RVA `0x55de048` and **161** `xgameruntime.lib` thunks that reach **17**
runtime classes. 158 of the thunks are referenced from the image; three are present and unreferenced (one each in
`XPackageImpl`, `XThreadingImpl` and `XAccessibilityImpl`).

The executable does not import `xgameruntime.dll`. Its import table has no such module, so the thunks are linked into
the image like `libHttpClient.GDK.dll`'s. The name occurs in the image as a UTF-16 string, which
`tools/pe_inspect.py` lists among the run-time load candidates (a string is where to look, not proof of a
`LoadLibrary`). The registry
therefore needs no module name beyond the `xgameruntime.dll` it already has, which settles the assumption at the top of
`runtime/shims/xgameruntime.cpp`.

| Class | Thunks | Referenced | Served |
|---|---:|---:|---|
| `XAccessibilityImpl` | 10 | 9 | no |
| `XAppCaptureImpl` | 1 | 1 | no |
| `XLauncherImpl` | 3 | 3 | no |
| `XDisplayImpl` | 1 | 1 | yes |
| `XGameEventImpl` | 1 | 1 | yes |
| `XGameImpl` | 2 | 2 | yes |
| `XGameRuntimeFeatureImpl` | 1 | 1 | yes |
| `XGameSaveImpl` | 20 | 20 | yes |
| `XGameUiImpl` | 5 | 5 | yes |
| `XNetworkingImpl` | 10 | 10 | yes |
| `XPackageImpl` | 18 | 17 | yes |
| `XPersistentLocalStorageImpl` | 2 | 2 | yes |
| `XStoreImpl` | 44 | 44 | yes |
| `XSystemAnalyticsImpl` | 1 | 1 | yes |
| `XSystemImpl` | 3 | 3 | yes |
| `XThreadingImpl` | 18 | 17 | yes |
| `XUserImpl` | 21 | 21 | yes |

"Served" is the tool's answer for the runtime sources at that commit. The three that are not: `XAccessibilityImpl`
(the speech-synthesizer slots 8-13, 15, 16 and 21 are referenced; slot 14 is not), `XAppCaptureImpl` (slot 5,
`XAppCaptureTakeScreenshot`) and `XLauncherImpl` (slots 3-5: `XLaunchUri` and the two display timeout-deferral calls).
Open pull requests serve them: #4235, #4307 and #4248. The slots of `XUserImpl`, `XStoreImpl` and `XGameSaveImpl` the
executable calls, and the argument counts the tool estimates from the registers, are in the report the tool writes.
`XErrorImpl`, which `XCurl.dll` and `libHttpClient.GDK.dll` call, is not among the executable's classes.

### Measuring which runtime classes a binary calls

`tools/gdk_runtime_calls.py` lists, from the file alone, the runtime classes, interface ids and vtable slots an image
calls through `xgameruntime.lib`'s thunks, so the executable's list is known before it first runs.
`scripts/first_look.ps1` runs it on the executable.

```powershell
python -I tools/gdk_runtime_calls.py "$game\XCurl.dll" "$game\libHttpClient.GDK.dll"   # text; --json for JSON
python -I tools/gdk_runtime_calls.py <exe> --out <dir>    # writes <exe>.gdk_calls.txt and .json, prints a summary
```

It finds the helper the thunks share (the call a known id's `lea` feeds, after which a vtable slot past IUnknown is
called), takes every call to it as a thunk, reads the class and interface ids loaded into `rcx` and `rdx` (an id the
table doesn't know is listed as unknown) and records each slot called up to `Release`, within the thunk's
`.pdata` function or a byte window without one. The slots are named from `tools/gdk_runtime_classes.json`: the ids,
inheritance and method order of xodus-gaming/xgameruntime-docs (GPL-3.0; facts only), made with
`python -I tools/gen_gdk_runtime_classes.py <docs clone>`. An interface's slot table is its chain's methods, base
first, from slot 3. For each class the report says whether `runtime/shims/xgameruntime.cpp` has its class id (a
`constexpr GUID k...Class`), and for each thunk whether anything in the image reaches it: a call, tail jump, address
load or pointer in data, directly or through an incremental-link `jmp` stub, a call from another thunk counting only
when that thunk is reached. The argument count it prints is an estimate from the registers and stack slots written
before the call.

Measured on the package's DLLs (counts and slots only):

| Image | Thunks (referenced) | Classes and slots |
|---|---:|---|
| `XCurl.dll` | 16 (16) | `XThreadingImpl` 3, 5, 12, 16-18, 22, 29; `XErrorImpl` 3; `XNetworkingImpl` as `IXNetworkingImpl2` 11-17, as in the table above. The estimated argument counts match the documented signatures for all 16 |
| `libHttpClient.GDK.dll` | 404 (15) | 23 classes through one helper. `XThreadingImpl` 3-7, 9-30, `XNetworkingImpl` as `IXNetworkingImpl2` 3-20, `XSystemImpl` as `IXSystemImpl5` 3-8 and `XPackageImpl` as `IXPackageImpl3` 3-19, 22, 24-27, 30, 32-34, 37-40 match the slots read by hand; the other 19 classes have thunks too. Referenced: `XThreadingImpl` 3, 5, 7, 9-15, 17, 19, 22, 29 and `XErrorImpl` 3 only |
| the other 14 DLLs | 0 | none |

`libHttpClient.GDK.dll` is incrementally linked (its entry point is a `jmp rel32` stub), and an incremental link
doesn't remove unreferenced functions (`/INCREMENTAL` turns `/OPT:REF` off): it carries 404 thunks and calls 15. Two
interface ids its thunks ask for are not in xgameruntime-docs: one of `XGameSaveImpl` (slots 33-49, after the 30 of
`IXGameSaveImpl3`'s chain) and one of `XGameStreamingImpl` (slots 3-6). The report prints them.

Limits:

- x86-64 only, and the thunk shape measured here: the class id loaded with a RIP-relative `lea` into `rcx` and the
  interface id into `rdx` (or the class id copied there), the vtable read with `mov r,[r]` and the slot with
  `mov r,[r+disp]` and a call (the CFG dispatch, or `call r`) or `call [r+disp]`, register copies followed. A
  `QueryApiImpl` call outside the library's thunks is found only when it has that shape; helper calls it can't
  follow to a slot are listed.
- Static: a referenced thunk may never run, and a reference through a computed address or from another module is
  not seen.
- "Served" means the shim has the class id. It doesn't say that each slot does more than log, nor that
  `QueryApiImpl` accepts the interface id the thunk passes; the report marks each request whose interface id is not
  the class id.
- Names are as complete as xgameruntime-docs: an *unknown* entry keeps its slot unnamed, and a slot past an interface
  without a documented layout is unknown.

A title that calls `LoadLibrary*`, `GetModuleHandle*`, `GetProcAddress` or `FreeLibrary` on one of the package's
bundled DLLs (`Bink2Scarlett.dll`, `XboxBvhBuilder.dll`, the FidelityFX DLLs) is routed through `rt::Loader` instead of
the host Windows loader (excluding data-file loads and host-preferred CRTs). The loader maps the DLL from `--game-dir`,
binds its console imports (`xmem.dll`, `d3d12_xs.dll`) through the shim registry, runs its `DllMain` and TLS callbacks,
and resolves direct, ordinal and forwarded exports from the mapped image. `run_tiny_bundled_load` exercises this path
end to end with a synthetic bundled DLL importing `xmem.dll!XMemAlloc` and `XMemFree`.

## WinHTTP offline

`XCurl.dll` and `libHttpClient.GDK.dll` do their HTTP through WinHTTP: between them they import 16 functions from
`winhttp.dll` (`WinHttpOpen`, `Connect`, `OpenRequest`, `SendRequest`, `ReceiveResponse`, `QueryHeaders`,
`QueryDataAvailable`, `ReadData`, `WriteData`, `AddRequestHeaders`, `SetOption`, `QueryOption`, `SetTimeouts`,
`SetStatusCallback`, `CloseHandle`, `CrackUrl`) and nothing from Winsock. Nothing here goes online, so `gta6rt` owns
`winhttp.dll` (`runtime/shims/winhttp_offline.cpp`) and answers all 16 in-process:

- Sessions, connections and requests are local handles: no name is resolved and no socket is opened.
- `WinHttpSendRequest` fails with `ERROR_WINHTTP_CANNOT_CONNECT`. On a `WINHTTP_FLAG_ASYNC` session it returns `TRUE`
  and the failure arrives as WinHTTP delivers one: `WINHTTP_CALLBACK_STATUS_REQUEST_ERROR` with `{API_SEND_REQUEST,
  ERROR_WINHTTP_CANNOT_CONNECT}`, on a thread-pool thread, with the request's context. A `dwContext` of 0 keeps the
  context set with `WINHTTP_OPTION_CONTEXT_VALUE`, as the host's `winhttp.dll` does. A request closed first gets no
  error callback.
- Reading a response and `WinHttpWriteData` fail with `ERROR_WINHTTP_INCORRECT_HANDLE_STATE`. Options, timeouts and
  headers are accepted; `WINHTTP_OPTION_CONTEXT_VALUE` and `WINHTTP_OPTION_HANDLE_TYPE` can be queried.
- A status callback is inherited by child handles, and so is the parent's `WINHTTP_OPTION_CONTEXT_VALUE` as it is
  when the child is made (measured on the host's `winhttp.dll`). Child handles report `HANDLE_CREATED`; every
  handle reports `HANDLE_CLOSING` once, last. `WinHttpCrackUrl` only parses, so it is the host's.
- Because the shims own the module, a title's `LoadLibrary("winhttp.dll")` gets a pseudo-handle too, and a WinHTTP
  function outside these 16 resolves to `NULL` (logged), never to the host.

Each refused server is logged once (`[rt] winhttp: offline, refused <host>:<port>`), which records what a title
tried to reach. `runtime/test/winhttp_offline_test.cpp` (ctest `winhttp_offline`) covers the synchronous and
asynchronous paths with `.invalid` names; it opens no connection.

## Winsock2 (`ws2_32.dll`) offline

`runtime/shims/ws2_offline.cpp` intercepts outbound Winsock2 entry points — both by name and by standard Windows
`ws2_32.dll` ordinal (`bind`/`#2`, `connect`/`#4`, `sendto`/`#20`, `setsockopt`/`#21`, `gethostbyaddr`/`#51`,
`gethostbyname`/`#52`,
`WSAAsyncGetHostByAddr`/`#102`, `WSAAsyncGetHostByName`/`#103`, `getaddrinfo`, `GetAddrInfoW`, `GetAddrInfoExA/W`,
`getnameinfo`, `GetNameInfoW`, `WSAConnect`, `WSAConnectByNameA/W`, `WSAConnectByList`, `WSASendTo`, `WSASendMsg`,
`WSALookupServiceBeginA/W`, `WSAJoinLeaf`, and `WSAIoctl` for `WSAID_CONNECTEX` and `WSAID_WSASENDMSG`). `wsock32.dll` maps its
`bind`, `connect`, `sendto`, `gethostbyaddr`, `gethostbyname` and `WSAAsyncGetHostBy*` exports (and their ordinals) to
the same wrappers, since its exports forward to `ws2_32.dll` natively; its `setsockopt` (`#21`) is its own export,
which takes the Winsock 1.1 option numbers, so it has its own wrapper. Like `ws2_32.dll` it stays host-owned
(`owns_dll`), so `LoadLibrary` returns the real module. The shim keeps `ws2_32.dll` partially overridden
so local socket bookkeeping (`WSAStartup`, `socket`, `closesocket`, `select`, `recv`, `send`, byte-order helpers) still
works for loopback traffic:

- `connect`, `WSAConnect`, `ConnectEx`, `sendto`, `WSASendTo`, `WSASendMsg`, `WSAConnectByNameA/W` and
  `WSAConnectByList` allow only IPv4 loopback (`127.0.0.0/8`), IPv6 loopback (`::1`) and IPv4-mapped loopback
  (`::ffff:127.0.0.0/104`); everything else fails with `WSAENETUNREACH`.
- `WSAConnectByNameA/W` pass `localhost` to the host as the literal `127.0.0.1`, so the host never reads its hosts
  file. Numeric loopback literals pass through unchanged.
- `ConnectEx` and `WSASendMsg` from `WSAIoctl` are the host's own pointers, fetched for the socket they are called on
  (each Winsock provider has its own), after the same loopback check.
- `bind` narrows wildcard binds (`0.0.0.0`, `::`) to loopback (`127.0.0.1`, `::1`) and refuses other non-loopback
  addresses with `WSAEADDRNOTAVAIL`.
- A send to a loopback address on an unbound socket (`sendto`, `WSASendTo`, `WSASendMsg`, `WSAJoinLeaf`) would bind it
  to the wildcard address, as Microsoft's `sendto` remarks describe, and its port would then take datagrams from other
  hosts. The shim first binds such a socket to `127.0.0.1`, `::1` or `::ffff:127.0.0.1` (matching the destination); if
  it stays unbound the send fails with `WSAEADDRNOTAVAIL`.
- Multicast group joins through `setsockopt` (`IP_ADD_MEMBERSHIP`, `IP_ADD_SOURCE_MEMBERSHIP`, `IPV6_ADD_MEMBERSHIP`,
  `MCAST_JOIN_GROUP`, `MCAST_JOIN_SOURCE_GROUP`, and `wsock32.dll`'s Winsock 1.1 `IP_ADD_MEMBERSHIP` = 5) fail with
  `WSAEADDRNOTAVAIL`; other options pass through. `WSAJoinLeaf` gets the same loopback check as `connect`, so a group
  address is refused with `WSAENETUNREACH`.
- Forward lookups refuse external names with `WSAHOST_NOT_FOUND` without sending DNS traffic. A dotted octet with a
  leading zero (`0127.0.0.1`) is refused too: Windows reads it as octal, so it is not the loopback address it looks
  like. `getnameinfo` / `GetNameInfoW` force `NI_NUMERICHOST` (or return `WSAHOST_NOT_FOUND` under `NI_NAMEREQD`) for
  non-loopback addresses.
- `WSALookupServiceBeginA/W` (name-service queries) always fail with `WSASERVICE_NOT_FOUND`.

`runtime/test/ws2_offline_test.cpp` (ctest `ws2_offline`) verifies name and ordinal bindings, the `wsock32.dll`
aliases, external refusals using RFC 5737 / RFC 3849 / `.invalid` targets, the `0127.0.0.1` refusal, the
`WSASERVICE_NOT_FOUND` refusal, wildcard-bind narrowing, a `ConnectEx` pointer from `WSAIoctl` connecting to a loopback
listener, a local loopback UDP round-trip, loopback binds for sends on unbound UDP sockets (with a control showing
the host's own `sendto` binds to `0.0.0.0`), and the multicast and `WSAJoinLeaf` refusals.

## What this proves and what it does not

- It proves the loader parses real Xbox Series PE files, maps their sections, applies no unnecessary relocation,
  walks their import tables, and classifies every thunk.
- The loader also registers each image's unwind table. `pe_loader_test` checks, for every DLL present in
  `LEONIDA_GAME_DIR`, that the whole exception directory is registered, that `RtlLookupFunctionEntry` returns
  each entry for its first byte, and that the entry point is covered (directly, or through the `jmp rel32` thunks
  an incrementally linked entry point goes through: a thunk touches no stack, so it has no entry of its own).
  Measured on all 16 DLLs: every table passes the loader's checks and every entry resolves to itself.

  | Image | Unwind entries | Entry point |
  |---|---:|---|
  | `Bink2Scarlett.dll` | 527 | covered |
  | `XboxBvhBuilder.dll` | 449 | covered |
  | `amd_fidelityfx_upscaler_xbox.dll` | 477 | covered |
  | `amd_fidelityfx_loader_xbox.dll` | 46 | covered |
  | `libHttpClient.GDK.dll` | 2,452 | `jmp rel32` thunk to a covered function |
  | `XCurl.dll` | 446 | covered |
  | `concrt140.dll` | 1,121 | covered |
  | `msvcp140.dll` | 1,228 | covered |
  | `msvcp140_1.dll` | 45 | covered |
  | `msvcp140_2.dll` | 469 | covered |
  | `msvcp140_atomic_wait.dll` | 86 | covered |
  | `msvcp140_codecvt_ids.dll` | 32 | covered |
  | `vccorlib140.dll` | 697 | covered |
  | `vcomp140.dll` | 195 | covered |
  | `vcruntime140.dll` | 236 | covered |
  | `vcruntime140_1.dll` | 79 | covered |
- It does **not** prove any imported function behaves correctly when called. That needs the executable, or a
  harness that calls a known entry point.
- The executable's own imports are unknown. `MicrosoftGame.Config` names `game_scarlett_master.exe`; the package
  here does not contain it, so its import table cannot be measured yet.
- The CRT images are present in the package, but `pe_loader.cpp` prefers the host copies for the `vcruntime`/
  `msvcp`/`concrt`/`vccorlib`/`vcomp` families. The table above maps the packaged images only to test the
  parser; a real run binds the host copies.
- No game data is stored in this repository. The counts are output of a local run against the user's package.
