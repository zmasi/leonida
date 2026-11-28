# gta6rt

`gta6rt` is the native Windows runtime for the Xbox Series build. It is deliberately small: the game
executable is still missing from the package used here, so this directory contains the machinery that
can be built and tested without it — the PE loader, the measured import shims, and the GPU/Vulkan test
tools.

## Build

The normal path is the repository build script:

```powershell
powershell -File scripts/build_runtime.ps1
```

It uses CMake + Ninja from Visual Studio when available, otherwise `cl.exe`, and writes
`runtime\build\gta6rt.exe` plus `gta6rt_tls_host.dll` (the loader's TLS-index helper; it must stay next to
the executable). The direct CMake path works too:

```powershell
cmake -S runtime -B runtime/build
cmake --build runtime/build --config RelWithDebInfo
```

Requirements: Windows x64, Visual Studio 2022 with the C++ desktop workload. The GPU tools need the
Vulkan SDK; the runtime and CPU tests do not. The loader is x64-only and the PE mapper rejects any
other machine.

## Dry run

```powershell
gta6rt --dry-run <image.exe|image.dll> [--game-dir <package>] [--overlay <dir>]
```

`--overlay` is where a mapped title's writes go (default `<game-dir>\overlay`); `GAME:` paths read from
`--game-dir`. See [Console paths and the write overlay](../docs/runtime_imports.md#console-paths-and-the-write-overlay).

The dry run maps the image and the bundled modules it imports, applies base relocations, binds every
import through the shim registry, a host DLL, or another bundled module, parses exports, installs static
TLS for the calling thread, applies section protections, and
prints a per-module report. It does **not** call `DllMain`, TLS callbacks, or the entry point, so no
code from the mapped images runs. TLS reserves its module index through the runtime's own
`gta6rt_tls_host.dll`, and host imports load system DLLs, as they did before.

`gta6rt --init <image>` runs the same mapping and then `Loader::initialise()`: each module's TLS
callbacks and each DLL's `DllMain(DLL_PROCESS_ATTACH)`, dependencies first. An EXE's entry point is not
run. When the loader is destroyed, initialised modules receive TLS callbacks and DLL entry-point calls with
`DLL_PROCESS_DETACH`, in reverse initialisation order. All mapped images, unwind tables and static TLS stay
available until these notifications finish, so DLLs can release resources such as FLS callbacks safely. The reserved
argument is null (explicit unload). A map-only load receives no detach notifications.

`--dry-run`, `--init` and `--run` are separate modes: give exactly one. Two together (`--dry-run --run`)
print the usage and exit 2 before anything is mapped.

`--init` and `--run` first check that the host can run the game's AVX code (CPUID.1 AVX and OSXSAVE, XCR0 YMM
state; `src/host_cpu.h`). Without it they print one line naming what is missing and exit 4 before anything is
mapped. In a virtual machine, pass the host CPU through (QEMU `-cpu host`); under Rosetta on macOS, set
`ROSETTA_ADVERTISE_AVX=1`. `LEONIDA_SKIP_CPU_CHECK=1` skips the check; `--dry-run` never runs it.

The report line is:

```
<name>  base=<addr> size=<image size> entry=+<rva>  shim=N host=N module=N unimplemented=N
```

Shim and unimplemented imports are listed by name. A non-zero `unimplemented` count is the useful
signal: it names the next shim that is missing. The measured result for the 16 DLLs shipped in the
package is recorded in the runtime-imports documentation.

After the totals the report counts the unimplemented imports by return class, and marks the HRESULT ones in the list:

```
    UNIMPLEMENTED xgameruntime.dll!XUserAddAsync  [returns HRESULT]
1 module(s) mapped, 1 unimplemented import(s)
unimplemented by return policy: hresult=1 other=0
```

### Unimplemented imports at run time: `LEONIDA_UNIMPLEMENTED`

An import with no shim, host function or bundled module is bound to a thunk. What a call to it does is set by the
environment variable `LEONIDA_UNIMPLEMENTED`, read once per process:

| value | a call to the thunk |
|---|---|
| `log` (default) | prints `[rt] unimplemented: dll!name` once and returns 0 in rax and xmm0. This is what the runtime always did. |
| `fail` | the same, but returns `E_NOTIMPL` (`0x80004001`) for imports known to return an `HRESULT`, and 0 for all others. |
| `stop` | on the first call prints the import, the caller's return address as `module+RVA`, and rcx, rdx, r8 and r9 as the game passed them; then breaks if a debugger is attached (continuing returns the `fail` value), else ends the process with exit code 3. |

No mode writes to an out parameter: a function that fills one in on success leaves it as the caller had it. Under
`log` an `HRESULT` function therefore returns `S_OK` with its outputs unset, which is why `fail` exists; with `fail`
the caller sees an error and can take its own failure path. Any other value is reported and treated as `log`.

Whether an import returns an `HRESULT` is decided by DLL and name alone (`classify_import` in
`shims/registry.cpp`, where each entry names its source). The list is short on purpose and every entry was checked
against the public Microsoft documentation: `D3D12Create*`/`D3D12Serialize*` and the D3D12 debug/experimental
entries, `CreateDXGIFactory*` and two DXGI functions, `XGameRuntimeInitialize`, and the GDK families `XAsync*`,
`XTaskQueue*`, `XUser*`, `XStore*`, `XGameSave*`, `XPackage*`, `XGameUi*` minus the functions the same pages document
as `void`/`bool`/`int`. A function that is not on the list gets 0 under `fail`, exactly as under `log`.

The thunk keeps the Win64 argument registers and the return address intact (it is entered by the game's `call`), and
reaches a shared stub that saves them, so a floating-point caller still reads 0.0 from xmm0 and a debugger can unwind
through it.

### Compat modules: `LEONIDA_COMPAT_DIR`

Opt-in. When `LEONIDA_COMPAT_DIR` names a folder, an import whose DLL is a file in that folder (matched by file name,
any case) is served by that DLL: Windows loads it from the full path with `LoadLibraryExW`
(`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`, never the game folder), so its CRT, TLS and SEH
work as for any host DLL and it takes no slot of the mapper's static TLS. This step comes before the shims, the
bundled package DLLs and the host, and the DLL owns all of its module's imports: a symbol it does not export binds to
the unimplemented thunk, not to a shim, so one console DLL is never served by two implementations that do not share
state. The binding is reported as `host`, and `[rt] compat: <dll> -> <path>` is printed once per DLL. A file in the
folder that Windows refuses to load stops the run with the error code. Unset or empty: no change. The DLLs stay
loaded for the process lifetime. Only static imports go through this step; a run-time `LoadLibrary` of the same name
does not yet.

It exists for drop-in implementations of console DLLs that have no shim here, such as the `AcpHal`, `chad`, `xapu`
and `XCRDAPI` crates of [omonkeyz/xbox-emulator](https://github.com/omonkeyz/xbox-emulator) (GPL-3.0-or-later).
Measured on the 1.0.2.1 executable at `867d922`: without the folder, `--dry-run` leaves 20 imports unimplemented
(AcpHal 9, chad 5, xapu 5, XCRDAPI 1); with those four DLLs built from `f89e410`, it leaves 0, with the four
package DLLs still mapped (`module=72`). Those DLLs run static initializers when Windows loads them, and with the
defaults one of them hooks the process: the package DLLs mapped after it then fail to map. With
`TOMBOX_GDK_EARLY=0 TOMBOX_GDK_RUNTIME=0 TOMBOX_OSHOST=0 TOMBOX_PAD=0 TOMBOX_TIMER_1MS=0` they map; their network
guard stays on and denies all traffic. `loader_test` covers the step with a synthetic DLL (no game data).

### Repeatable random bytes: `LEONIDA_RNG_SEED`

With `LEONIDA_RNG_SEED=<number>` (decimal, or hexadecimal with `0x`; read once when the `bcrypt` shim starts),
`BCryptGenRandom` on the RNG algorithms answers from a seeded splitmix64 generator instead of the host's, so two
runs with the same seed get the same bytes. The shim prints `[rt] bcrypt: deterministic LEONIDA_RNG_SEED enabled`
with the seed on stderr; a seed of 0 uses a fixed non-zero state. Unset, the host's generator answers as before.

## PE loader

`src/pe_loader.cpp` maps x86-64 images at their preferred base, or elsewhere (with relocations) when
that base is taken. An image with a relocation table is moved and fixed up whatever `IMAGE_FILE_RELOCS_STRIPPED`
says; without one, the flag refuses the move, and an image with the flag clear is moved with nothing fixed up (the
loader logs that). The Windows 11 (build 26200) loader does the same in these three cases, as Zombins2 measured
with synthetic DLLs in a review of old pull request 8847 of the original repository, which is no longer readable.
Open question from the same measurement: Windows also refused flag-set DLLs without a relocation directory at a free
preferred base (error 487, linker-built `/FIXED` DLLs included), and why is not known; the loader still maps those
at their base, as before. It copies headers and sections, applies `IMAGE_REL_BASED_DIR64` and `HIGHLOW` relocations
(rejecting the other types, such as `HIGH`, `LOW` and `HIGHADJ`, which no x64 MSVC image contains), binds
imports by name or ordinal, parses the export table, sets section protections, and registers the
image's x64 unwind table (`.pdata`) with `RtlAddFunctionTable`, so exceptions raised in or passing
through a mapped image are dispatched and stack walks can step over its frames. The table is validated
first (entries in bounds, sorted and non-overlapping) and removed when the loader is destroyed. Modules
are deduped by basename, so an import graph that reaches the same DLL twice maps it once. The identity
key uses the filesystem's native path representation with ASCII case folded; it does not require a
Windows filename to fit the ANSI code page. A lossless legacy narrow filename alias retains the existing
bytewise case behavior for narrow API and import-name lookups. `Module::name`, mapped address labels,
and path diagnostics use UTF-8. The `LoadLibrary*`/`GetModuleHandle*` shims look a wide name up by its native
basename when it has a character outside ASCII (`Loader::find_loaded_module_w`, `load_runtime_module_w`), and
module names compare case-insensitively with the system's own table (`CompareStringOrdinal`), as Windows
compares them; a narrow name outside ASCII compares the same way through the ANSI code page.

The game's code is built for the console's AMD Zen 2 and uses a few of its instructions without asking CPUID: SSE4a
`insertq`/`extrq`, `clzero` and `monitorx`/`mwaitx` (no Intel CPU has them) and `clwb` (Intel since Ice Lake)
([docs/cpu_features.md](../docs/cpu_features.md)). A host without one raises `#UD` there, so `gta6rt` installs a
vectored handler before any image code runs (`src/cpu_emulation.h`): it recognises those encodings at the faulting
address, does the same work on the thread's context and continues after the instruction. It prints one `[rt] cpu:`
line at startup naming what the host lacks, and one the first time each instruction is emulated. On a host that has
them the CPU runs them and the handler never sees them. Each emulated instruction costs an exception round trip, so a
hot loop of them is slow; patching such a site in place is a later step. `cpu_emulation_test` runs every form for
real: natively on a Zen host, through the handler on an Intel one.

`module_names` checks native and mapped renamed DLLs, distinct Unicode identities, basename cache
reuse, existing ACP aliases, the wide and case-insensitive lookups, export execution, and path errors. On native Windows with Python available,
`run_module_names` compares original renamed EXEs through native execution and `--dry-run`, `--init`,
and `--run`, including a Unicode parent directory and an ACP-named imported DLL. It checks strict UTF-8
trace names and unchanged fixture bytes. Both tests use only locally built synthetic images.

MSVC C++ exceptions need one more piece. `_CxxThrowException` stores the throwing image's base in the
exception record, from `RtlPcToFileHeader`, and the catch side adds the throw's RVAs to it; Windows' own
`RtlPcToFileHeader` only knows images its loader mapped, so for a mapped image the base is 0 and no
`catch` matches. When the loader binds a host copy of the MSVC runtime (`vcruntime140.dll`,
`msvcp140.dll` and the rest of that list), it points that DLL's import of `RtlPcToFileHeader` at a
version that asks Windows first and then the modules a live `Loader` maps. The redirect stays for the
process lifetime and answers as Windows does when no `Loader` is alive.

`load()` also installs static TLS for the calling thread. `__declspec(thread)` reads `TEB->ThreadLocalStoragePointer[_tls_index]`, which is a
different index space from the `TlsAlloc` slots, and only the OS loader can allocate a module index.
The runtime therefore ships a minimal `gta6rt_tls_host.dll`; the loader reuses the index the OS assigns
it, writes that index into the image's `AddressOfIndex`, and copies the image's TLS template into the
slot. This reserves one index; every additional simultaneous TLS module gets its own by mapping a fresh
clone of the helper from `%TEMP%\leonida_tls_host_<pid>_<slot>.dll`, so up to 16 modules can be live at
once and a further one is rejected with an error instead of sharing a slot. Images without a TLS
directory (or with an all-zero one) load normally.

An executable (an image without `IMAGE_FILE_DLL`, the game's EXE under `--run`, `--init` and `--dry-run`) gets
module TLS index 0 instead, as Windows gives the process EXE. MSVC code built with `/GA` relies on that: it reads
`ThreadLocalStoragePointer[0]` directly and never loads `_tls_index`. A `thread_local int v = 42;` EXE built
`/O2 /GA /MD` printed `tls_index=0 value=42` natively but `tls_index=6 value=0` under `gta6rt --run` when it got a
helper's index. Index 0 belongs to `gta6rt.exe` itself (measured: Windows gives it to the process EXE, the trace event
below names it, and `gta6rt.exe` has a 1398-byte TLS template when measured and no TLS callbacks), and all of its own
TLS reads go through `_tls_index`. So `gta6rt.exe` moves off index 0 once, at start (`rt::reserve_tls_index0()`,
called from a CRT initializer that runs before any of gta6rt's own static initializers): it writes one more clone of the
helper, `leonida_tls_host_<pid>_home.dll`, whose TLS directory covers a copy of `gta6rt.exe`'s template (the
helper carries a 16 KiB array for it), swaps slots 0 and that clone's index on the calling thread and writes the
clone's index into `gta6rt.exe`'s `_tls_index`. Every thread the OS starts from then on has a fresh copy of
`gta6rt.exe`'s TLS at that index from its first instruction, so nothing has to run on a new thread first, including
code that runs in an earlier DLL's `DLL_THREAD_ATTACH`. Slot 0 then holds a block nothing reads, which the executable's
template copy replaces on the loading thread, on threads started later, and through `install_tls_for_thread()` on
older ones, exactly as a DLL's copy replaces a helper's; the OS frees both slots at thread exit. The move cannot be made
later: MSVC keeps the address of a TLS slot in a frame across calls (`zlib_inflate` in `shader_library.h` does), so
a thread that already runs gta6rt's code cannot be moved wherever it is stopped. The startup trace records the move at
the first executable (`tls_index0`). DLLs keep the helper indices and work next to the executable as before.

When the executable does not get index 0 (the reservation failed, or another executable holds it), its `/GA` code
still reaches its static TLS directly at index 0 (`mov r64, gs:[58h]`), but the loader can only get that image an
index from the OS, and it need not be 0. Before the image's sections
are protected and before any of its code runs, TLS callbacks included, the loader rewrites each such
slot-0 read in an executable section to the same instruction reading `TEB->TlsSlots[slot]` instead
(`patch_ga_tls_reads`), with one `TlsAlloc` slot per image; the rewrite is the same length, so nothing
else in the image moves. It applies only to an EXE whose own TLS index is not 0 and that has a TLS
directory; the loader's log names the image, the number of rewritten reads and the slot. Set
`LEONIDA_TLS_GA_PATCH=0` to turn the rewrite off, for measuring what the image does without it; any other
value, or leaving it unset, keeps it on.

Current limits, by design:

- `load()` runs no image code. Static TLS is data, not code. The TLS index helper is the runtime's own
  `gta6rt_tls_host.dll`; host imports still load system DLLs as before.
- `--dry-run` never runs image code. `initialise()` runs TLS callbacks and a DLL's `DllMain`;
  `run_tls_callbacks()` is the seam for callers that do not use `initialise()`. An EXE's entry point is
  the caller's to run.
- Every thread started after `load()` gets its own copy of the TLS template, from
  `gta6rt_tls_host.dll`'s `DLL_THREAD_ATTACH`, and gives it back at `DLL_THREAD_DETACH`, also when the
  thread outlives its `Loader`. From `initialise()` until `~Loader` starts the process detach, those
  events also run the module's TLS callbacks, with the thread's copy already in place. A thread that
  was already running when `load()` ran, such as a worker of the process thread pool that the task queue later runs a
  title's callback on, gets its copy from `load()` itself, the way Windows gives one to running threads for a DLL it
  loads: a block, no `DLL_THREAD_ATTACH`, `DLL_THREAD_DETACH` when it ends, and `~Loader` takes the block back.
  `install_tls_for_thread()` and `uninstall_tls_for_thread()` are no-ops on such a thread. Mapped DLLs' `DllMain` is not called with thread events. Destroy the `Loader` on the thread that
  called `load()`. As on Windows, a new thread gets every module's copy before any TLS callback runs, and the
  executable's callbacks come after the DLLs' ones, for attach and for detach.
- One executable holds index 0 at a time, and only after `reserve_tls_index0()` has run at process start. An
  executable mapped without it (the reservation failed, the `Loader` is used without it, or another executable
  holds index 0) gets a helper index and a `[rt] loader: ... does not get module TLS index 0` line on stderr: its
  `/GA` code would read and write the TLS in slot 0, which is `gta6rt.exe`'s own or the other executable's, so
  `--run` and `--init` refuse it with an error and exit code 1, and a `Loader` user must not run its code.
- Threads that ran gta6rt's code before the reservation keep nothing of their TLS: they find a fresh copy at the new
  index. In gta6rt only the main thread has run its code by then, and its TLS moves with it.
- The home clone stays loaded until the process ends (freeing it would free `gta6rt.exe`'s TLS blocks), and a mapped
  image's file cannot be deleted, so its file stays in `%TEMP%` until the next run's clean-up of dead processes'
  clones deletes it. The clones for mapped DLLs are deleted at exit as before. A `gta6rt.exe` whose TLS
  outgrows the helper's 16 KiB array is refused the move.
- The TOC of the game's RPF archives is encrypted and is not read here. The loader works on the PE
  files and on any images the caller passes it.

## Shims

`shims/registry.cpp` holds the static tables. The current shims are:

- `xmem.dll` — `XMemAlloc`, `XMemFree`, the measured two-argument `XMemGetWorkingSetStatistics(kind, out)`, and the
  measured three-argument
  `XMemVirtualQuery(process, address, out)`. Allocations are 64-byte aligned; the working-set and
  virtual-query outputs fill the observed 64-byte structures and return `FALSE`/`0` on a null or bad
  address. `XMemAllocDefault`/`XMemFreeDefault` share that heap. `XMemVirtualAlloc` accepts the attribute word,
  drops the console's `PAGE_GRAPHICS_*` protection bits, commits 2 MiB guest pages 2 MiB-aligned,
  and passes `MEM_RESERVE_PLACEHOLDER`/`MEM_REPLACE_PLACEHOLDER` through. Physical pages
  (`XMemAllocatePhysicalPages`, `XMemFreePhysicalPages`, `XMemMapPhysicalPages`, `XMemMapPhysicalPagesScatter`) are
  64 KiB pages of pagefile-backed sections, mapped one view per page into the placeholder that a reserve-only
  64 KiB-page `XMemVirtualAlloc` becomes, with that reservation's protection. `XMemIsToolsMemAvailable` is `FALSE`.
- The `kernel32.dll` file API — `CreateFileA/W`, `FindFirstFileA/W`/`FindNextFileA/W`/`FindClose`,
  `GetFileAttributesA/W`, `GetFileAttributesExA/W` and `DeleteFileA/W`: a `GAME:` path maps under `--game-dir`, an
  open that can write and a delete go to `--overlay DIR` (default `<game-dir>\overlay`), which reads prefer, and a path
  that climbs above `GAME:` is refused. The package is never modified. See
  [docs/runtime_imports.md](../docs/runtime_imports.md#console-paths-and-the-write-overlay).
- `d3d12_xs.dll` — `D3D12SerializeVersionedRootSignature` and
  `D3D12CreateRootSignatureDeserializer`, with the console root-signature high byte (`0x03000000`)
  cleared on a copy before forwarding to the PC `d3d12.dll`. `D3D12XboxCreateDevice` wraps a device in
  the measured-slot Xbox-ABI proxy: the host PC `ID3D12Device` by default, or the Vulkan-backed device
  (`shims/d3d12_vulkan_device.cpp`) when `LEONIDA_D3D12=vulkan` is set and the runtime was built with the
  Vulkan SDK and Python 3. The Vulkan device covers command submission, descriptor heaps, render-target and
  depth views, committed resources and copies (uploads, readbacks, BC textures), root signatures, graphics and
  compute pipeline states, draws, dispatches and `ExecuteIndirect`; see `docs/d3d12_vulkan.md` for what is
  missing. `LEONIDA_DEVICE_FAULT=1` enables a stderr report with supported Vulkan fault information and queue
  checkpoints on the first observed device loss. On NVIDIA, a build configured with
  `-DLEONIDA_AFTERMATH_SDK=<unpacked Nsight Aftermath SDK>` also takes `LEONIDA_AFTERMATH=1`: GPU crash dumps
  (`.nv-gpudmp`, plus `.nvdbg` shader debug info) in `LEONIDA_AFTERMATH_DIR`, and a decoded summary on stderr that
  names the faulting shader by its library name, the page fault and the active shaders (`src/vulkan_aftermath.h`).
  The SDK is not part of the repository; its DLL is loaded at run time (`LEONIDA_AFTERMATH_DLL` overrides the path).

- `dstorage_xs.dll` / `xcrdapi.dll` / `dstorage.dll` — `DStorageGetFactory`: the console DirectStorage API
  (`shims/dstorage.cpp`, declarations in `shims/dstorage_xs_abi.h`). Files open through the `GAME:` mapping and the
  overlay; each queue reads file or memory ranges into memory in order on its own worker thread, inflating zlib
  requests (RFC 1950, `src/shader_library.h`'s inflater), and reports through status arrays, `ID3D12Fence` signals
  and events. BCPack, swizzle and physical-page requests are not done yet.
  See [docs/directstorage.md](../docs/directstorage.md) for the ABI evidence and what is unverified.

- `gameinput.dll` / `xgameruntime.dll` — `GameInputCreate`: the v0 GameInput API for gamepads on SDL3
  (`shims/gameinput.cpp`), so any controller SDL supports works, with rumble, Xbox trigger rumble, force feedback
  and trigger haptics where SDL opens the pad as a haptic device, and on a DualSense adaptive-trigger effects and the
  lightbar. Optional and CMake-only: built when an installed SDL3 is found,
  or with `-DLEONIDA_SDL3_FETCH=ON` to download the pinned release; nothing is downloaded otherwise, and the
  `scripts/build_runtime.ps1` cl.exe fallback never builds it. See [docs/gameinput.md](../docs/gameinput.md) for the mapping, the `LEONIDA_DS_*` settings and what is
  unverified. A build without SDL3 links `shims/gameinput_no_sdl.cpp` instead: no devices, or with
  `LEONIDA_INPUT_SCRIPT` set one scripted pad driven by the script alone, so a headless or CI build can get past the
  title's first prompt ([docs/gameinput.md](../docs/gameinput.md#scripted-input-without-sdl3)).

### What `xgameruntime.dll` reports: environment overrides

The `xgameruntime` shim (`shims/xgameruntime.cpp`) answers the title's questions about the console and its package
with fixed values. Six environment variables change an answer, for trying another of the title's code paths. Each
is read on every call, and a value that is empty or too long for its buffer is ignored; the row marked *diagnostic* only adds log lines.
`tests/test_docs_env_vars.py` fails when the runtime or `make_game.py` reads a `LEONIDA_*` variable that no
document names.

| variable | default | what it changes |
|---|---|---|
| `LEONIDA_DEVICE_TYPE` | `7` | The `XSystemDeviceType` that `XSystemGetDeviceType` returns: `0` Unknown, `1` Pc, `2` XboxOne, `3` XboxOneS, `4` XboxOneX, `5` XboxOneXDevkit, `6` XboxScarlettLockhart (Series S), `7` XboxScarlettAnaconda (Series X), `8` XboxScarlettDevkit. A number outside 0..8 is ignored; text that is not a number counts as `0`. `6` also makes `XSystemGetAnalyticsInfo` report the form `Xbox Series S` instead of `Xbox Series X`. The title's registry read of `HKLM\Software\Microsoft\Windows NT\CurrentVersion\OEM` `ConsoleMode` is answered from the same setting (`6` → `0x60`, `7` → `0x64`, `8` → `0x68`; the other types have no value, so the host registry is asked), so no registry key has to be written into a Wine prefix or a Windows machine (`docs/runtime_imports.md`, "Console identity in the registry"). |
| `LEONIDA_ANALYTICS_FORM` | unset | The `form` string of `XSystemGetAnalyticsInfo`, in place of the one the device type gives (up to 63 characters). |
| `LEONIDA_PACKAGE_ID` | `LeonidaPackageIdentifier00000000` | The identifier `XPackageGetCurrentProcessPackageIdentifier` returns (up to 32 characters). It is also the one identifier `XPackageMount` refuses with `E_ACCESSDENIED`, as the game's own package. |
| `LEONIDA_CONTENT_PACKAGES` | unset | The content packages `XPackageMount` can mount: `id1=path1;id2=path2` (up to 1,023 characters). The mount path of a listed identifier is its path, as written; any other identifier is `ERROR_NOT_FOUND`, and a path longer than 259 characters is `ERROR_INSUFFICIENT_BUFFER`. |
| `LEONIDA_PACKAGED` | `1` | `0`, or text that is not a number, makes `XPackageIsPackagedProcess` return false. Any other number keeps it true. |
| `LEONIDA_TITLE_MEMORY_MB` | unset | Caps the title memory limit (in MiB, clamped to `2048..65536`) reported by both `XSystemGetAppMemoryUsageLimit` (`xgameruntime.cpp`) and `XMemGetWorkingSetStatistics` (`xmem.cpp`). Useful on 16 GB RAM / low-VRAM hosts to prevent oversized title pools; invalid or non-positive values are ignored and fall back to the host memory report. |
| `LEONIDA_TRACE_XGAMESAVE` | unset | *Diagnostic.* `1` logs one `[rt] xgamesave:` line to stderr per `XGameSave*` and `XGameSaveFiles*` call (`shims/xgameruntime_gamesave.cpp`): the call, its handles, container and blob names, sizes and HRESULT, after `t=<ms since the process started>ms tid=<thread>`. An async call also logs when its job is scheduled, when its work runs (with how long it waited on the queue), when it completes and when the title collects it with the `...Result` call. Blob contents are never logged. Read once per process; unset, a call costs one load. |
| `LEONIDA_STACKS` | unset | Comma-separated whole seconds after the title's entry point is called (e.g. `90,180`). At each time `--run` suspends every other thread in turn just long enough to copy its context and stack, then unwinds the copy (title frames through the unwind tables the loader registers) and prints to stderr `[rt] stacks t=<s> thread <tid> (<thread description, or start address>):` and up to 40 `#i module+0xRVA` frames, innermost first; a chain identical to an earlier thread's prints `same as thread <tid>`. |

### Switches of the D3D12 layer for a retail boot

Read once per process, by the Vulkan D3D12 layer (`shims/d3d12_vulkan_device.cpp`) unless the row names another file.
"on" means the behaviour is the default and `0` turns it off; "unset" means it is off until the variable is set. The
ones marked *diagnostic* only add log lines or exist to compare a run with and without a change; nothing needs them.

| variable | default | what it changes |
|---|---|---|
| `LEONIDA_LAZY_PIPELINES` | on | Graphics pipelines are built at their first draw. `0` builds each at `CreatePipelineState`, as before; the title creates far more pipelines at startup than it draws with. |
| `LEONIDA_LAZY_COMPUTE` | on | Compute pipelines are built at the first dispatch that names them, or earlier by the warm-up thread. `0` builds each at `CreateComputePipelineState`. |
| `LEONIDA_ASYNC_MIN_BYTES` | unset | `<n>`, with `LEONIDA_ASYNC_PIPELINES=1`: only a pipeline of n bytes of SPIR-V or more (compute: its module; graphics: all its stages) is built asynchronously, its draws or dispatches skipped until the compile worker has it; a smaller one is built where it is needed, as with asynchronous builds off, so small lighting and post programs are not skipped while the large ray-tracing ones compile. Unset or `0`: every pipeline is asynchronous. Text that is not a decimal number is ignored, with a log line. ([docs/d3d12_vulkan.md](../docs/d3d12_vulkan.md), "Only the large ones asynchronous") |
| `LEONIDA_ASYNC_MIN_BYTES_GRAPHICS` | unset | `<n>`, with `LEONIDA_ASYNC_PIPELINES=1`: the threshold of `LEONIDA_ASYNC_MIN_BYTES` for graphics and mesh pipelines only (all their stages), so a draw can wait for a different size than a dispatch. `0`: every graphics pipeline is asynchronous (its draws skipped until built) while compute keeps `LEONIDA_ASYNC_MIN_BYTES`. Unset: graphics take `LEONIDA_ASYNC_MIN_BYTES`, as before the variable. Text that is not a decimal number is ignored, with a log line. ([docs/d3d12_vulkan.md](../docs/d3d12_vulkan.md), "Only the large ones asynchronous") |
| `LEONIDA_COMPUTE_WARMUP_MAX_BYTES` | `1048576` | The largest SPIR-V module, in bytes, the background warm-up thread (`LEONIDA_COMPUTE_WARMUP`) builds ahead of its first dispatch. Larger modules wait for that dispatch. |
| `LEONIDA_PIPELINE_ORDER_LOG` | unset (off) | `1` appends, for each pipeline and variant (the unbound-slot mask of a graphics pipeline) that a dispatch or draw names for the first time, one line to `pipeline_order.log` beside `pipeline_cache.session.bin`: the PSO's description key, the variant, when the title created the PSO and when it first used it (milliseconds since the process started), the build time and whether the handle was ready already. A line of counts is printed at exit (`[rt] pipeline-order: requested N, already ready M, median lead X ms`). The hook never blocks and allocates nothing; it takes only a non-blocking `try_lock`, on the first request of a pipeline and of a variant. Record only: nothing builds, orders or skips differently, and no cache file is touched. At most 8192 lines per run; a file past 8 MiB is set aside as `pipeline_order.log.prev`. The file holds hashes and numbers, no shader or game data, and stays local. See `docs/d3d12_vulkan.md`. |
| `LEONIDA_PIPELINE_PREWARM_ORDER` | unset (off) | `<file>`: an order log written with `LEONIDA_PIPELINE_ORDER_LOG=1` in an earlier run; `1`: `pipeline_order.log` beside the session cache. The graphics and mesh pipelines that run asked for first are queued, when the title creates them, as warm-ups on the compile workers in the recorded order: after every demanded build, one at a time and none while a demanded build runs. The run read is this GPU's with the most records. With `LEONIDA_PIPELINE_COMPILE=on-demand`, a console stream in the plan is parsed by its first prewarm job rather than at its first bind. A missing, garbled or other-GPU file is ignored with one `[rt] pipeline-prewarm:` line. See `docs/d3d12_vulkan.md`. |
| `LEONIDA_PIPELINE_PREWARM_BUDGET_MS` | `300000` | With `LEONIDA_PIPELINE_PREWARM_ORDER`: records are prewarmed in order until their recorded build times add up to this many milliseconds (a record with no build time counts 1000). |
| `LEONIDA_PIPELINE_PREWARM_MAX_ENTRIES` | `4096` | With `LEONIDA_PIPELINE_PREWARM_ORDER`: at most this many pipeline variants are prewarmed. |
| `LEONIDA_SERIAL_BIG_COMPILES` | unset | Large pipelines (compute modules over 128 KiB of SPIR-V, graphics pipelines of 1 MiB or more) compile side by side while their estimates, 512 bytes of memory for each byte of SPIR-V, fit a budget (`LEONIDA_BIG_COMPILE_BUDGET_MB`): one always does, what a submission waits for goes before the warm-up, and the warm-up takes a quarter of the budget at most. `1` compiles the compute ones one at a time, and the graphics ones of a submission one after another, as before there was a budget. `0` lets all of them compile side by side, within the compile workers' heavy count (`LEONIDA_COMPILE_HEAVY_MAX`) for the asynchronous builds and the warm-up. |
| `LEONIDA_BIG_COMPILE_BUDGET_MB` | unset | The budget, in MiB: a fixed number (`0`: one large pipeline at a time) instead of three quarters of the memory that is free at the moment, which is the smaller of the free RAM and the room under the commit limit, read again for each pipeline. Text that is not a number is ignored. |
| `LEONIDA_CONSOLE_FORMATS` | on | Texture descriptions the console accepts and a PC GPU may not (an R9G9B9E5 render target, an sRGB RGBA8 or BGRA8 texture with unordered access) get a host format that can do the job. `0` refuses them, as a PC D3D12 runtime does. |
| `LEONIDA_CONSOLE_HEAP_SHADOW` | on | A console descriptor heap has a copy in the descriptor arena that the shaders read. `0` keeps its bytes in the title's memory only (the comparison for a run that misbehaves with the copy). |
| `LEONIDA_IMPORT_BELOW_ARENA` | unset | `1` keeps a host-memory import that the driver put below the GPU address arena. By default such an import is not used and the shadow buffer in the arena answers, because a V# cannot name an address below the arena. |
| `LEONIDA_HOST_IMAGE_COPY` | `auto` | Whether `WriteToSubresource`/`ReadFromSubresource` copy on the CPU with `VK_EXT_host_image_copy`. `auto` (unset, or any text other than `0` and `1`) uses it where the device has it, except on NVIDIA (vendor `0x10DE`), where a real boot lost the device; `1` uses it wherever the device has it; `0` never enables the extension, so every CPU texture copy takes the staging submit. Device creation logs one `[rt] d3d12: host image copy:` line with the choice and why (`docs/d3d12_vulkan.md`). |
| `LEONIDA_ORDER_TEXTURE_UPLOADS` | on | A title texture in a host-transfer image is filled through the ordered host copy (`write_host_texture`). `0` fills it with `WriteToSubresource`. |
| `LEONIDA_PLACED_IMAGE_KEYS` | on | A texture the title placed at its own address (device slot 133) and that the GPU writes keeps its own image address as its T# key. `0` re-keys it to the title address, like a texture the GPU does not write. |
| `LEONIDA_PLACED_TEXTURE_UAV` | on | Device slot 71 writes an unordered-access view of a texture in the title's placed memory. `0` writes a null descriptor. |
| `LEONIDA_REFRESH_MAPPED_TEXTURES` | on | A mapped slot-133 texture is refilled outside the fill budget, and its pages are committed before they are read. `0` treats it like any other title texture. |
| `LEONIDA_TEXTURE_PAGE_CHECK` | on | The pages of a title texture are checked for being readable before they are hashed or copied. `0` checks only a texture whose check is already pending. |
| `LEONIDA_TITLE_TEXTURE_PERIOD_MS` | `16` | The shortest time between two passes over the textures filled from the title's memory. |
| `LEONIDA_TITLE_TEXTURE_CHECK_KB` | `4096` | How many KiB of those textures one pass hashes to find changed ones. |
| `LEONIDA_TITLE_TEXTURE_FILL_MB` | `32` | How many MiB of changed textures one pass fills again; the rest waits for the next pass. |
| `LEONIDA_TEXTURE_HASH_BUDGET_US` | `400` | Microseconds a pass may spend hashing before it stops at a texture boundary. `0` leaves the byte budget alone to bound it. |
| `LEONIDA_TEXTURE_SWEEP_BUDGET_US` | `400` | Microseconds a pass may spend on residency checks and uploads between textures. |
| `LEONIDA_TITLE_BUFFERS_MB` | `512` | The most memory, in MiB, of buffers made over the title's own memory for vertex and index views that name it. |
| `LEONIDA_TITLE_BUFFER_COVERAGE` | on | Such a view counts as covered only when a known buffer spans all its bytes. `0` asks for the first byte only. |
| `LEONIDA_SHADOW_FULL_WALK` | unset | *Diagnostic.* `1` queries every page of the title-memory shadows at every refresh, as before unchanged generations were skipped. |
| `LEONIDA_STRICT_REPLAY` | unset | *Diagnostic.* `1` makes a draw or dispatch with an unresolvable resource fail its command list (`src/vulkan_replay.h`); by default it is dropped and logged. |
| `LEONIDA_VOLUME_MIPS` | on | The XG layout of a 3D texture with mips or a block format is computed (`shims/xg_xs_layout.h`). `0` refuses it. |
| `LEONIDA_TITLE_WATCHDOG` | unset (armed) | The title's own hang watchdog stays armed by default, as on the console: if the game's main loop does not report for 30 seconds, the title ends itself. `0` makes `gta6rt --run` keep it from firing while the game thread waits for the driver, for example on the first boot after a driver update (`src/main.cpp`). **Since RT-BOOT-COMBINED `0` also needs `LEONIDA_TITLE_WATCHDOG_KICK`**: the watchdog's addresses are no longer built in. With `0` and no `LEONIDA_TITLE_WATCHDOG_KICK`, the loader says once that the watchdog stays armed. |
| `LEONIDA_TITLE_WATCHDOG_KICK` | unset | `<trap RVA>,<kick RVA>` (hex, measured locally and not kept in the repository): where the watchdog's trap instruction is and the byte the game's main loop sets. Used only with `LEONIDA_TITLE_WATCHDOG=0`, and only when the watchdog's trap instruction is at the first RVA; set without `LEONIDA_TITLE_WATCHDOG=0`, the watchdog stays armed and the loader says so once. |
| `LEONIDA_POOL_DIAG_AT` | unset | *Diagnostic.* `<module>+0x<offset>` of the title's transient-pool assert, as the trace prints it (measured locally): the trace dumps the pool's resource list when that int3 is hit (`shims/startup_trace.h`). |
| `LEONIDA_PEEK` | unset | *Diagnostic.* `<rva>[:n],<rva>,...` (hex): a thread prints the 8-byte values at these RVAs of the title's image (`:n` = n consecutive values) every `LEONIDA_PEEK_SECONDS` (`src/main.cpp`). The RVAs are measured locally and not kept in the repository. `scripts/windows-boot/boot.ps1` no longer sets the game-state and session-state RVAs by default: it sets `LEONIDA_PEEK` only from `LEONIDA_GAME_STATE_RVA` and `LEONIDA_SESSION_STATE_RVA`, and without them its RESULT states read `?` ([scripts/windows-boot/README.md](../scripts/windows-boot/README.md)). |
| `LEONIDA_PEEK_SECONDS` | `10` | *Diagnostic.* The period of the `LEONIDA_PEEK` thread (`src/main.cpp`), which prints 8-byte values of the title's image. |
| `LEONIDA_INPUT_SCRIPT` | unset | Timed pad presses, `button@seconds` separated by commas (`a@70,a@85,start@120`), each held 300 ms or its own `button@seconds:hold`; the clock starts with the shim. Works with and without SDL3: with SDL3 it drives the keyboard pad (`LEONIDA_KEYBOARD_PAD=always`), without SDL3 one scripted pad that is the only device. Controllers and the keyboard pad still need SDL3. Format and buttons in [docs/gameinput.md](../docs/gameinput.md#environment-variables). |
| `LEONIDA_INPUT_LOG` | on | The GameInput shim logs every press in `LEONIDA_INPUT_SCRIPT`'s form (`name@seconds`), so a session played by hand can be replayed. `0` turns the lines off. |
| `LEONIDA_INPUT_TRACE` | unset | `1` (diagnostic, the GameInput shim built without SDL3) logs the title's reading calls on the scripted pad: the first 128, then each one with a button down and every 1000th. Details in [docs/gameinput.md](../docs/gameinput.md#scripted-input-without-sdl3). |
| `LEONIDA_LOG_ALLOCINFO` | unset | *Diagnostic.* `1` logs every device slot 25 call (`GetResourceAllocationInfo`) with its thread, description and sizes (`shims/d3d12_xs_device.cpp`). |
| `LEONIDA_LOG_EXE_PIPELINES` | unset | *Diagnostic.* When set, one line per distinct graphics pipeline that uses a program translated from the executable's inline containers. |
| `LEONIDA_MEM_PROBE` | unset | *Diagnostic.* `<seconds>`: every that many seconds the memory probe (`src/mem_probe.h`) logs `[rt] mem-probe t=` (live pipelines, shader modules and `VkDeviceMemory` per memory type), `[rt] mem-probe breakdown t=` (the same allocated bytes by consumer: the arena with its reserved, bound and in-use MiB, geometry copies, title windows and shadows, upload heaps, staging, texture blocks, sampled textures, render targets and depth, and `other`, which add up to the allocated total; `src/memory_breakdown.h`) and `[rt] mem-probe smaps` (the process's resident memory by kind). |
| `LEONIDA_MEM_PROBE_CACHE` | unset | *Diagnostic.* `1` adds the pipeline cache's size to the memory probe's report; the driver serializes the whole cache to answer. |
| `LEONIDA_TEXTURE_PROBE` | unset | *Diagnostic.* Logs a `[rt] texture-probe` line for every shader resource view of a texture in the title's memory. |
| `LEONIDA_LOCAL_DIAGNOSTICS` | unset | *Diagnostic.* `1` turns on the boot diagnostics that write to stderr from the draw, clear, present and descriptor paths (the lines tagged `present-probe`, `fsdraw`, `heap-probe`, `desc-probe` and `PresentX diag`; `src/local_diagnostics.h`). They follow a frame of the title from its draws to the window and cost a lock and a line per draw, so they are off by default. The full-screen draw targets that `LEONIDA_CAPTURE_SOURCE` reads back are only collected with it. |
| `LEONIDA_CAPTURE_SOURCE` | unset | *Diagnostic.* With `LEONIDA_CAPTURE`, `1` also reads back the presented source texture itself (`src/vulkan_present.h`). |
| `LEONIDA_XG_TRACE_FORMAT` | unset | *Diagnostic.* A DXGI format number: logs the first 256 XG layout computers of that format, whatever `LEONIDA_XG_TRACE`'s cap (`shims/xg_xs.cpp`). |
| `LEONIDA_FSDRAW_POKE_CB` | unset | *Diagnostic, changes what is drawn.* An experiment of the full-screen draw probe (`src/fsdraw_probe.h`): writes a known colour into the constant buffer a bound table names, to prove the path from the table to the render target. |
| `LEONIDA_PIPELINE_COMPILE` | unset | `on-demand` (the `--first-run-ui` profile sets it) prepares pipeline-state streams and sampled views at bind time: a placed 2D texture then waits for its memory until a command or a view first uses it. `cache-only` makes an eager build (`LEONIDA_LAZY_PIPELINES=0`) ask the driver's cache first and leaves what would have to be compiled to the first draw. `first-use` is accepted and means the default. When a pipeline is compiled is `LEONIDA_LAZY_PIPELINES`. |
| `LEONIDA_GRAPHICS_STAGE_LAYOUT` | on | Graphics root arguments are laid out as the console does for the vertex, pixel and geometry stages (from `s8`, a descriptor table in one dword). `0` keeps `rt::rootsig::pack()` (from `s0`, two dwords per address) for every graphics signature. |
| `LEONIDA_FRAME_PACING` | unset | `1` (the `--first-run-ui` profile sets it) hands device slots 78 to 81 (`SetFrameIntervalX`, `ScheduleFrameEventX`, `WaitFrameEventX`, `GetFrameStatisticsX`) to the device's own presentation once the first `PresentX` has built it, with the title's interval and origin event replayed to its pacer. Unset, the slots answer from the device's token counter with no wait. |
| `LEONIDA_GPU_LABELS` | unset | `1` records the labels of console list slots 71 and 72 and stores each into the title's memory when the GPU has finished the command buffer that carries it (`src/vulkan_gpu_labels.h`), for lists the list values (`LEONIDA_LIST_VALUES`, on by default) do not track: those land their slot 71 to 74 writes themselves, and a second writer could rewind a counter. Unset, the two slots of an untracked list do nothing. Both writers land a value at a GPU address arena offset in the arena and one on an xmem page the title sees as `PAGE_NOACCESS` through xmem's own mapping (`src/value_write_sink.h`). |
| `LEONIDA_LINEAR_MAP_FIRST` | unset | `Map` of a slot 133 texture leased as linear answers with the placement address and the title texture registry fills the image. `1` gives it the linear mapping instead: pinned pages, with a GPU-written texture read back into them, and a refusal when the pages are gone. |
| `LEONIDA_CONSOLE_D16S8` | by GPU | The console's `D16_UNORM_S8_UINT` family (DXGI 118 to 120) is made as the D24S8 family where the GPU has `D24_UNORM_S8_UINT` as a depth-stencil attachment and keeps its own D16/S8 path where it has none (AMD). `native` or `d24` chooses for every GPU. The device logs which it took. |
| `LEONIDA_RGBA4_COPY_REFUSE` | unset | `1` refuses a footprint copy to or from a `B4G4R4A4_UNORM` render target stored as RGBA8 (its texels are four bytes there, not two). Unset, the copy is made. |
| `LEONIDA_SLOT133_LAYOUT_WORD` | unset | The committed stand-in of device slot 133 (where no resource can be made on the title's pages) reads its fourth word as a resource state. `1` reads it as a `D3D12_BARRIER_LAYOUT`, as the placed-at-VA handler does: `CreateCommittedResource3` with that layout on a device that has it, else the legacy state of the layout. |
| `LEONIDA_DESC_MIRROR` | on | Descriptor writes to a title-backed heap whose real start differs from the title's are also stored at the title's address, and list slots 35 and 36 pull the title's descriptors into the bound table. `0` turns both off. |
| `LEONIDA_SLOT70_TITLE` | on | A small raw view of the title's own memory through device slot 70 becomes a raw buffer view of its GPU copy. `0` keeps the null view. |
| `LEONIDA_WINDOW` | unset | `WxH` resizes the title's window when the presenter attaches to it. |
| `LEONIDA_PRESENT_DUMP` | unset | *Diagnostic.* `DIR` or `DIR,N` writes every Nth presented frame (60 by default) as a PNG into `DIR` (`src/present_dump.h`). The files are pictures of the title: keep them out of commits, issues and pull requests. |
| `LEONIDA_LOCAL_SPEECH` | unset | `1` forwards the title's speech-synthesis calls to the host's installed `xgameruntime.dll` where there is one. Unset, and wherever that library is missing, the silent voice answers. No profile sets it. |
| `LEONIDA_CONSOLE_FIX_US` | `0` | Microseconds one submission may spend fixing the changed slots of a console descriptor heap's copy; `0` sets no limit. Past it, each further changed slot is written as a null descriptor and queued for `LEONIDA_CONSOLE_RETRY_US`, so a shader reads it as empty until its turn comes: a limit changes results, and can hang the GPU. |
| `LEONIDA_CONSOLE_RETRY_US` | `500` | Microseconds one submission may spend retrying, round robin, the console heap slots not settled yet (a T# whose texture does not exist yet, or one deferred by `LEONIDA_CONSOLE_FIX_US`). `0` retries all of them at every submission. |
| `LEONIDA_SHADOW_ALIAS_MS` | `0` | The shortest time, in milliseconds, between two copies of the title-memory pages that XMem maps at two addresses into the slot-133 shadows and title windows: pages a write reached since they were guarded at every address, or every time while they can't be (a page pinned for a kernel write). `0` copies them at every submission; a larger value lets a GPU read of them be up to that many milliseconds behind the title's writes. |
| `LEONIDA_SHADOW_KEEP` | on | The slot-133 shadows and title windows copy a page only in the 256-byte pieces where the title's bytes changed since the last copy, so what a shader stored in the copy stays, also on pages copied without a sign that the title wrote them (aliased while they can't be guarded at every address, unguarded, pinned). A title writing the same bytes again to reset such a value is not seen. `0` copies those pages whole, as before, overwriting the GPU's writes. |
| `LEONIDA_REFRESH_PER_FRAME` | `0` | Trades correctness for speed. `1`: the slot-133 shadows, title windows and geometry copies are brought up to date only by the first submission after each present, not by every submission; `2`: the console descriptor heaps too. A GPU read of them can then be a frame behind the title's writes (wrong pixels). `0` refreshes all of them at every submission. |
| `LEONIDA_GEOMETRY_ASYNC` | unset | Trades correctness for speed. `1` refreshes the geometry copies (`shims/d3d12_geometry_views.h`) on a worker thread of the layer, started at submission and not waited for, instead of on the title's submitting thread. A draw may read a copy the worker is still writing: wrong vertices for a frame. |
| `LEONIDA_GEOMETRY_WHOLE_VIEWS` | unset | A geometry copy that is already filled copies only the 64 KiB tiles of title memory written since the last refresh (`shims/d3d12_geometry_views.h`). Any value other than `0` copies every view over a written tile whole, as before. |
| `LEONIDA_GEOMETRY_HOST` | unset | Where the geometry copies (`shims/d3d12_geometry_views.h`, under `LEONIDA_GEOMETRY_MB`) live. Unset or `0`: device-local memory through the BAR while the driver gives it (several GB of VRAM in a world run). `1`: system memory outside VRAM, write-back where the device offers a cached host type; `2`: system memory, write-combined. The GPU then reads every vertex and index of a copy over the bus, which can cost frame time in geometry-heavy passes. A device whose host-visible memory is all device-local (an integrated GPU) keeps its types. The choice is logged once and in the `geometry buffers` line. |
| `LEONIDA_TITLE_FILL_ASYNC` | unset | Trades correctness for speed. `1` stops device slot 70 views from hashing and filling the texture they name on the title's thread: a texture never filled or known stale is queued for a worker thread that fills one texture at a time. Until then a shader samples the image as it is, empty or a frame behind. |
| `LEONIDA_TITLE_VIEW_REFRESH_MS` | `0` | The shortest time, in milliseconds, between two checks of a title texture's bytes when a device slot 70 view of it is written. `0` checks at every view. The periodic pass still checks the texture on its own period, and a texture known stale is checked at once. Has no effect with `LEONIDA_TITLE_FILL_ASYNC=1`. |
| `LEONIDA_TITLE_EVERY_BUDGET_US` | `0` | Microseconds one pass over the textures filled from the title's memory may spend on the textures it visits every pass (mapped by the title, or known stale), resuming where the last pass stopped; the texture that crosses the limit is finished. A mapped texture never filled yet is visited first whatever the limit. `0` visits all of them every pass. It limits the synchronous pass only: with `LEONIDA_TEXTURE_REFRESH_ASYNC=1` the pass runs on the refresh worker, and the limit does not apply. |
| `LEONIDA_TITLE_TEXTURE_FILL_MS` | `8` | Milliseconds one pass over the textures filled from the title's memory may spend filling; a texture already started is finished. `0` removes the time limit and leaves `LEONIDA_TITLE_TEXTURE_FILL_MB` to bound the pass. |
| `LEONIDA_TITLE_PARTIAL_MIPS` | unset | `1`: a 2D texture of one slice the title keeps in its own memory, whose larger mips are not readable, is filled from the first mip that is when its last slot-70 view starts there or later. Views that start above keep the fallback until a later fill has their mips. Unset, a texture waits for all its mips. |
| `LEONIDA_TITLE_PARTIAL_MIPS_MB` | `2048` | With `LEONIDA_TITLE_PARTIAL_MIPS=1`, the image memory the textures filled in part may hold, in MiB: each has memory for all its mips. A texture past it keeps the fallback. |
| `LEONIDA_MAP_READBACK` | unset | `1` writes the GPU's texels of a small render or unordered-access target the title placed into the title's pages when the title maps it, so a CPU read of the target sees what the GPU rendered (the title reads its exposure statistics that way). Only a single-row, single-mip, single-layer, single-sample 2D target of at most 4 KiB a row; the copy waits for the GPU's work already submitted, and a page that is not writable is left as it is. Unset, the title reads its own pages, which the GPU never writes. |
| `LEONIDA_SLOT133_IMPORT` | on | *Diagnostic.* `0` imports no title-placed buffer (device slot 133) as host memory: each takes the shadow-buffer fallback, so no GPU store can reach the title's memory through an import. |
| `LEONIDA_ARENA_NULL_MB` | `0` | `n` (MiB) keeps the first `n` MiB of the descriptor arena, at `heap_base + 0`, zeroed and never allocated, and moves the zero page and the quad indices above it. A translated shader whose address arithmetic starts from a null base then reads or writes there instead of in a title buffer or the shared zero page. Ignored when it would take half of the arena or more; the device logs the region it made. |
| `LEONIDA_COMPUTE_HEAP_RECORDS` | unset | *Diagnostic.* With the compute input probe (`LEONIDA_COMPUTE_INPUTS`, `LEONIDA_DEVICE_FAULT=1`; [docs/d3d12_vulkan.md](../docs/d3d12_vulkan.md)), up to eight comma-separated `<table>/<slot>:<stride>:<dword>:<heap>[:<first>:<count>]` specs (`shims/d3d12_compute_input_probe.h`). Each logs `compute-heap-record` lines for the records of `stride` bytes in the buffer at root `s<table>`, descriptor `<slot>`, whose dword `<dword>` holds a resource-heap index in its low 24 bits, with `s<heap>` holding the heap's 32-bit shader address. `<first>` and `<count>` name the root dwords that hold the dispatch's first record and record count; without them the first records of the buffer are read. |
| `LEONIDA_SLOT31_ASYNC` | unset | Trades correctness for speed. `1` makes queue slot 31 (`CopyPageMappingsBatchX`) signal the queue, hand a copy of its batches to one worker thread and return; the worker waits for that signal and maps them in call order (`shims/d3d12_xs_device.cpp`). A copy of the GPU-only window made before the worker gets there reads the old pages for a moment. Unset, the caller waits for the mapping. |
| `LEONIDA_SLOT31_RUNS` | unset | `1` makes queue slot 31 unmap a run of pages whose mapping changes once and map one view for each stretch of consecutive pages of one section, instead of page by page (`shims/xmem.cpp`). Off by default because it was measured slower. |
| `LEONIDA_SLOT31_CPU_TWIN` | unset | `1` repeats the page mappings queue slot 31 makes in the GPU view of the title's stream pool at the matching addresses of its CPU view (`shims/xmem.cpp`, read when the second of the pool's two reservations is noted). |
| `LEONIDA_QUEUE_PAGE_MAPPINGS` | on | Read by `rt::xmem::queue_page_mappings_enabled()` (`shims/xmem.cpp`), where `0` turns it off. Nothing in the runtime calls that function, so the variable has no effect at present, and what it was meant to switch is not clear from the code. |
| `LEONIDA_POOL_ZERO_PAGES` | unset | `1` answers a read by title code of an unmapped page of an XMem physical-page window by mapping a zeroed read-only page there (a "zero plant", logged), taken under the physical-mapping lock (`shims/xmem.cpp`). Unset, only a write into such a page is backed with a zeroed page. |
| `LEONIDA_TITLE_HEAP` | unset | `1` makes the title's `XMemAlloc` blocks come from a Win32 heap of their own instead of the CRT heap (`shims/xmem.cpp`), so a GPU store through a slot-133 import past a buffer's end, or into a freed block, cannot corrupt the runtime's own objects on the same page. |
| `LEONIDA_XMEM_SEC_COMMIT` | on under Wine | XMem's physical-page sections are committed whole under Wine; `0` reserves them and commits each page at its first touch, as on Windows, where the variable has no effect (`shims/xmem.cpp`). |
| `LEONIDA_HANG_EXIT_S` | `0` | `s` seconds with work pending on the queues and no submission retired counts as a GPU hang the driver may recover from without reporting the device lost: the stall is reported (with checkpoints under `LEONIDA_STALL_CHECKPOINTS=1`) and the process ends with exit code `0x47` (`src/vulkan_backend.h`), so a boot does not sit until its time limit. `0` or unset never ends the process. |
| `LEONIDA_DEVICE_MARKERS` | unset | *Diagnostic.* `1`, with `LEONIDA_DEVICE_FAULT=1` or `2`, on a device with `VK_AMD_buffer_marker` (RADV, AMD's drivers): every dispatch the device-fault report numbers writes its `seq` into a small host-visible buffer at top of pipe (started) and bottom of pipe (finished), one slot per queue (`src/vulkan_device_markers.h`). A loss or stall report then prints each queue's last finished and started `seq` and marks the oldest started, unfinished dispatch as the likely hang, the AMD counterpart of NVIDIA's checkpoints; the values are best-effort after a loss. Two marker writes per dispatch while on; nothing when unset. Without the extension it logs one line saying the markers are unavailable ([docs/d3d12_vulkan.md](../docs/d3d12_vulkan.md) section 4.11). |
| `LEONIDA_STALL_CHECKPOINTS` | unset | *Diagnostic.* `1` makes a device-stall report also collect the NVIDIA checkpoints of every queue (`src/vulkan_device_fault.h`), which name the programs in flight. The extension only promises them for a lost device, so outside that state the call is outside its contract. |
| `LEONIDA_PASS_TRACE` | unset | *Diagnostic.* `n`, with `LEONIDA_DEVICE_FAULT=1`: in the frame after every `n`-th present, each submission logs its compute programs in order, repeats folded (`src/vulkan_backend.h`), for the frame's pass order. |
| `LEONIDA_GPU_LATENCY` | unset | *Diagnostic.* `1` logs, every 10 s for each queue kind, how long the GPU took from a submission's `vkQueueSubmit` to it being seen done, and the share of the wall time the queue had work (`src/vulkan_backend.h`). |
| `LEONIDA_SUBMIT_PHASES_SECONDS` | `5` | *Diagnostic.* The period, in seconds, of the always-on line that says how many submissions there were and where their CPU time went, phase by phase (`src/submit_phases.h`). `0` turns the line off. |
| `LEONIDA_DEV_OVERLAY` | unset | *Diagnostic, Windows.* `1` draws what the runtime is doing over the top left corner of the title's window (`src/dev_overlay.h`): frame and submission rate, a submission's CPU time by phase, the phase running now, pipeline builds, indirect draws, cores, free RAM, and the private memory and GPU memory (`src/memory_figures.h`, shared with `LEONIDA_TITLE_HUD`; the GPU figure is the device's `VK_EXT_memory_budget`, `VRAM n/a` without it). |
| `LEONIDA_SAMPLE_MS` | unset | *Diagnostic.* `ms` starts the thread sampler (`src/thread_sampler.h`): every `ms` milliseconds each thread of the process that used CPU since the last look is suspended just long enough to copy its context, its stack is unwound afterwards, and a periodic report lists the busiest threads and their most frequent call chains (eight frames, innermost first). |
| `LEONIDA_SAMPLE_REPORT_S` | `10` | *Diagnostic.* Seconds between two reports of the thread sampler (`LEONIDA_SAMPLE_MS`). |
| `LEONIDA_SAMPLE_WAITS` | unset | *Diagnostic.* `1` makes the thread sampler sample waiting threads too, not only those that used CPU, and list each thread's most frequent chain, so a thread that waits shows where. |
| `LEONIDA_SAMPLE_DETAIL` | unset | *Diagnostic.* Comma-separated thread IDs whose samples the thread sampler also unwinds 16 frames deep, listing each such thread's ten most frequent chains. |
| `LEONIDA_SAMPLE_SHIMS` | unset | *Diagnostic.* `1` makes the thread sampler unwind each sample deep enough to tell whether a title thread is inside the runtime (a frame of `gta6rt` called from title code) and where, and report it. |
| `LEONIDA_AFTERMATH_SHADER_ERRORS` | unset | *Diagnostic.* With `LEONIDA_AFTERMATH=1`, `1` also sets `VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_ERROR_REPORTING_BIT_NV` (`src/vulkan_aftermath.cpp`): the driver reports more shader errors, so a run can fault where it otherwise does not. |
| `LEONIDA_AFTERMATH_KEEP_SPIRV` | unset | *Diagnostic.* With `LEONIDA_AFTERMATH=1`, `1` keeps a copy of every SPIR-V module created, for the crash-dump decoder. Off by default because the title creates gigabytes of them; the decoder then reads `<LEONIDA_SHADERS>/<name>.spv` back when its hash matches the module's. The runtime's own modules are always kept. |
| `LEONIDA_AUDIO` | unset | `silent` (any case) makes the title's `MMDeviceEnumerator` answer every render question with the runtime's silent output endpoint, even on a host that has an output (`shims/silent_audio.cpp`, read at each `CoCreateInstance`): no host audio device is opened and every sample is thrown away, for muted test runs. Unset, or any other value, the host's endpoint answers when there is one and the silent endpoint only when the host has none. Capture keeps the host's answer either way. See [docs/runtime_imports.md](../docs/runtime_imports.md#audio-output-off-the-console-wasapi). |
| `LEONIDA_TEXTURE_REFRESH_ASYNC` | unset | `1` moves the before-submission refresh of the title textures (`refresh_title_textures`) to a worker: a submission only wakes it, and fills by itself only the mapped textures that were never filled, so their first draw has their bytes. Persistent mappings are hashed whole on the worker every pass; the rest is swept 128 textures at a time. A texture changed since may be sampled a frame behind. |
| `LEONIDA_SUBMIT_ASYNC` | `0` | `<n>`: each queue's `ExecuteCommandLists`, `Signal`, `Wait` and markers run in call order on a worker of the queue's own (`src/vulkan_backend.h`), at most n batches queued; the caller of the next one waits for room. The replay, the before-submission refreshes and `vkQueueSubmit` then leave the title's thread. A batch that fails is reported as a held one is. `0`: on the caller's thread. |
| `LEONIDA_COMPUTE_WARMUP_LIVE_MAX_BYTES` | unset | `<n>`: a compute warm-up of a module over n bytes of SPIR-V is not started while the title submits (a submission in the last second); the pipeline is built at its first use instead (docs/d3d12_vulkan.md, "Pipelines still being built"). Unset or `0`: every warm-up runs, ordered behind a submission's compiles by the large-compile gate. |
| `LEONIDA_COMPILE_GATE` | `auto` (adaptive or classic, by calibration) | The compile gate (`SubmitMutex::CompileTurn`, `src/vulkan_device_fault.h`): a queue submission (or a present or device wait) does not enter the driver while a pipeline create is in it, and waits in the runtime instead, since on NVIDIA 630.20 a `vkQueueSubmit` that entered during a long `vkCreateComputePipelines` waited 10 s in the driver and lost the device. Creates pass together; one that arrives while a submission waits queues behind it. Unset or `auto`: the device measures the driver once, before the title has it (`src/compile_gate_calibration.h`, `docs/d3d12_vulkan.md`): a worker thread creates a synthetic compute pipeline that takes about a second to compile while the queue gets empty submissions, each waited for. Submissions back in under 50 ms with the create still in the driver give the adaptive gate; one held until the create leaves, a create too fast to tell, a submission slow for another reason or any failure give the classic gate. The verdict is kept per GPU and driver (vendor, device, driver version, pipeline cache UUID) in `compile_gate_calibration.txt` beside the pipeline cache, so the measurement runs once per driver (a failed or unclear calibration is not kept: the next launch measures again), and is logged once: `[rt] compile-gate: auto: adaptive (calibrated on NVIDIA 621.40 in 0.7 s: 115 submissions came back in <= 0.5 ms beside a 722 ms create; cached in ...)`, or `auto: classic (...)`, or `(cached calibration of ...)`. Only gta6rt's title process calibrates: `--precompile-pipelines`, `--compile-server` and tests keep the classic gate. `classic`: compute creates only, waited for however long they take (`LEONIDA_COMPILE_GATE_WAIT_MS` can bound it), no probes, the default before `auto`. `adaptive`: the adaptive gate whatever the calibration says; on a driver that holds submissions (630.20) its first probe of a kind enters the driver after the probe budget and loses the device if that create stays 10 s more. The adaptive gate's wait is bounded by `LEONIDA_COMPILE_GATE_WAIT_MS`. Each submission that enters the driver next to a create the gate went past (at that limit, or earlier at the probe budget, `LEONIDA_COMPILE_GATE_PROBE_MS`) is a probe; two that come back in under 250 ms, at least 1 s apart, with the create still in the driver turn the gate off for the rest of the run (NVIDIA 621.40: 0-11 ms), and one that stays 2 s or more keeps it on; both are logged once. The verdict is kept per kind of create, compute and graphics (mesh included; graphics creates pass the gate since then, only compute ones did before): on NVIDIA 621.40 submissions came back in 0-12 ms beside compute creates, but one sat 54 s in the driver beside a 56 s mesh create and the device was lost. A kind that is off is still timed, and a submission that stays 2 s beside one of its creates turns it back on. A submission never goes ahead, at the limit or the budget, beside a create of a kind whose verdict is that it holds submissions: it waits for that create up to `LEONIDA_COMPILE_GATE_HELD_WAIT_MS`. On a device loss the log names the creates in the driver (kind, program, size, how long) and each kind's gate. `1`: always gate, graphics creates too, with no limit unless `LEONIDA_COMPILE_GATE_WAIT_MS` sets one, never probe or turn it off, and a compute pipeline build also holds the submit lock for its driver calls (`compute_driver_turn`, `shims/d3d12_vulkan_device.cpp`). `0`: no gate. |
| `LEONIDA_COMPILE_GATE_WAIT_MS` | 0 (15000 with the adaptive gate) | `<ms>`: how long a submission waits at the compile gate before it goes ahead anyway; the creates in the driver then hold no later submission back (creates that enter after it are waited for again), and the log says how long the submission then took in the driver. The waiter holds the submit lock, so every queue stops behind it, and the title's hang watchdog ends the process after 30 s (one create stayed in the driver for over 54 s on NVIDIA 621.40). 15 s keeps the 630.20 protection for creates of up to 25 s. The same with the watchdog kept kicked: nothing ends the run then, but the stops still cost the frames (an RTX 4060 run that way had 459 waits, 155 s in all, by t=202 s). `0`: no limit, and with `LEONIDA_COMPILE_GATE_PROBE_MS` unset no probes either: the gate waits however long the creates take and never adapts. |
| `LEONIDA_COMPILE_GATE_HELD_WAIT_MS` | 60000 | `<ms>`: with the adaptive gate, how long a submission waits for creates of a kind the gate found to hold submissions (`docs/d3d12_vulkan.md`) before it goes ahead anyway; those creates are then not waited for again. `0`: until they leave. |
| `LEONIDA_COMPILE_GATE_PROBE_MS` | 2500 with the adaptive gate (0 with `LEONIDA_COMPILE_GATE_WAIT_MS=0`) | `<ms>`: the adaptive gate's probe budget. Until it knows whether the driver holds a submission behind a create, a submission also goes ahead once the submissions' waits at the gate in the last 10 s, its own included, add up to this; a single wait reaches it at 2.5 s. On NVIDIA 621.40 waits of 3-7 s, none near the 15 s limit, came back to back and kept the title's main loop from reporting for 30 s; on an RTX 4060 with the watchdog kept kicked, 459 waits of up to 2.9 s took 155 s by t=202 s. The cost on a driver that does hold submissions (630.20): the first probe enters the driver after 2.5 s of waiting and loses the device if the create stays 10 s more; the held verdict then stops the early probes. `0`: probes only at `LEONIDA_COMPILE_GATE_WAIT_MS`. Used only by the adaptive gate. |
| `LEONIDA_COMPILE_GATE_RECALIBRATE` | unset | `1`: with `LEONIDA_COMPILE_GATE` unset or `auto`, the device measures the driver again instead of reading the verdict kept in `compile_gate_calibration.txt`, and writes the new one there (after a driver update the key changes and it measures anyway). |
| `LEONIDA_FAST_PIPELINES` | unset | `graphics`, `compute` or `all` (`1` = all) creates those pipelines with `VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT` (`shims/d3d12_pipeline_compile.h`): slower shaders, much shorter builds. Part of the compute journal key, so optimized and fast builds keep separate files. |
| `LEONIDA_COMPILE_THREADS` | half the logical processors, 2-8 | `<n>`: the compile workers of the asynchronous builds (`LEONIDA_ASYNC_PIPELINES=1`), the compute warm-up and the cache checkpoint (`shims/d3d12_pipeline_compile.h`, `CompilePool`). A demanded build goes before warm-ups; large and speculative builds take at most n-1 workers, so one stays free for a small build a submission waits for; one build of a PSO at a time; warm-ups one at a time, and none while a demanded build is queued or running. `1`: one general worker plus the graphics-only one (`LEONIDA_GRAPHICS_COMPILE_WORKER`), the layout before the pool. |
| `LEONIDA_COMPILE_HEAVY_MAX` | threads-1 (`1` with `LEONIDA_SERIAL_BIG_COMPILES=1`) | `<n>`: at most n builds of large modules (compute over 128 KiB, graphics of 1 MiB of SPIR-V or more) on the compile workers at once, 1 to threads-1. They also keep within the memory budget of `LEONIDA_BIG_COMPILE_BUDGET_MB`. |
| `LEONIDA_GRAPHICS_COMPILE_WORKER` | on | With `LEONIDA_COMPILE_THREADS=1`: a second compile worker that takes graphics pipeline builds only, so a long compute build does not hold them up (`shims/d3d12_pipeline_compile.h`). `0`: the one general worker builds everything. With more workers it is not used. |
| `LEONIDA_CONSOLE_HEAP_GPU` | unset | `1`: the V#s of the console descriptor heaps are rebased by a compute pass at the start of each submission (`src/console_heap_gpu.h`) instead of on the CPU. The CPU still finds the changed slots and registers the T#s; a V# the pass cannot answer is resolved after its submission and its slot queued again. |
| `LEONIDA_CONSOLE_HEAP_ARENA` | unset | With `LEONIDA_CONSOLE_HEAP_GPU=1`, `1` puts a console heap's GPU copy and its guard in the arena (device-local where the device has ReBAR) instead of an import of host memory. No room in the arena: the import. |
| `LEONIDA_CONSOLE_GPU_TSHARPS` | on | With `LEONIDA_CONSOLE_HEAP_GPU=1`, the T#s of changed slots are registered by the resolver thread and the refresh only decodes them. `0` registers them on the submitting thread, under the fix budget. |
| `LEONIDA_CONSOLE_GPU_RETRY_MS` | `2000` | With `LEONIDA_CONSOLE_HEAP_GPU=1`, how often V# words that got no backing are asked for again, on a timer that bounds the cost per submission. |
| `LEONIDA_CONSOLE_GPU_FILTER` | unset | With `LEONIDA_CONSOLE_HEAP_GPU=1`, `1`: a released backing, an alias range that went or a word answered since asks for a rescan of only the slots it touches, not of every V# slot. Overflows and key resets still rescan everything. |
| `LEONIDA_SHADOW_GPU` | unset | `1`: aliased pages of the slot-133 shadows are copied on the GPU at the start of a submission, from an import of the mapped title section (`VK_EXT_external_memory_host`, per 2 MiB window, kept while xmem's map journal shows no change over it). Destinations outside the arena and pages that do not import are copied by the CPU as before. |
| `LEONIDA_SHADOW_GPU_MB` | `1024` | With `LEONIDA_SHADOW_GPU=1`, the MiB of title memory imported for those copies at most. |
| `LEONIDA_SHADOW_JOIN` | unset | `1`: pieces of one shadow copy job that continue where the one before ended, in the pages and in the copy, are one copy (one GPU region); one the GPU refuses is copied by the CPU whole. |
| `LEONIDA_STRAY_RO_HEAL` | unset | `1`: a title write into a read-only page of title memory that no guard of ours owns (a guard left behind) makes the page writable again and retries the write, so the title goes on; a shadow may then miss that page's next writes for a frame. Unset: the first 32 such pages are logged. |
| `LEONIDA_PRELUDE_QUEUE_WAIT` | on | The prelude pass of a submission (the console-heap pass) waits for the last submission of another queue that recorded one (`src/vulkan_backend.h`). `0` (an experiment): no wait, so the queues do not serialise on each other's submissions; a list may read what the other pass has not written yet. |
| `LEONIDA_DEPTH_BOUNDS_SLOT70` | on | Slot 70 of the console command list is forwarded to `OMSetDepthBounds` (`shims/d3d12_xs_device.cpp`). `0` leaves the slot to the default thunk. |
| `LEONIDA_DESCRIPTOR_RING` | unset | *Diagnostic.* `1` records the descriptor-table slots each list binds (`shims/d3d12_com.cpp`), as `LEONIDA_DEVICE_FAULT=1` does; it takes a global lock on almost every table bind. |
| `LEONIDA_DESC_PROBE` | unset | *Diagnostic.* `1` logs which console-heap slots the API wrote and which the title wrote itself, what the title's records decode to and whether the GPU can see them. It hashes every slot of every bound table at each submission. The log quotes title data: keep it local. |
| `LEONIDA_DRAW_DIAG` | on with the local diagnostics or `LEONIDA_FSDRAW_PROBE` | *Diagnostic.* Logs the first 8 draws and one in 1,200 after them, with whether each was skipped for having no viewport, its pipeline, render target, viewport and scissor (`shims/d3d12_com.cpp`). `0` turns it off, any other value on. |
| `LEONIDA_FSDRAW_PROBE` | unset | *Diagnostic.* `1` keeps records of T#s registered, CBVs made, pipelines created and tables submitted, and logs full-screen draws with them (`src/fsdraw_probe.h`); `LEONIDA_COMPUTE_INPUTS` turns them on too. They take a lock at each of those events. |
| `LEONIDA_FSDRAW_SLOTS` | `4` | *Diagnostic.* With `LEONIDA_FSDRAW_PROBE=1`, how many slots of a descriptor table are shown. |
| `LEONIDA_COMPUTE_INPUT_LIMIT` | unset | *Diagnostic.* With the compute input probe (`LEONIDA_COMPUTE_INPUTS`), the first n matching dispatches are logged in full, then one in 1,000 (`shims/d3d12_compute_input_probe.h`). Unset or `0`: every one. |
| `LEONIDA_LOCK_TRACE` | unset | *Diagnostic.* `1` logs the first 256 texture `Map` calls that pass a read range (the title's texture lock passes the subresource's byte range there). |
| `LEONIDA_TRACE_TEXTURE` | unset | *Diagnostic.* `<address>[,<address>...]` (hex, the base a T# names): logs the resource id of an image registered at that address, so a draw's T# can be followed to the passes that write it. |
| `LEONIDA_MIRROR_LAG` | unset | *Diagnostic.* `1` measures how late a console fence's mirror shows a value the GPU has reached (`shims/d3d12_xs_device.cpp`). |
| `LEONIDA_MIRROR_EXACT` | unset | `1`: the device's fence-wait thread writes a console fence's mirror the moment the timeline reaches the next value, so the title sees a completion as soon as the runtime does; the thread-pool wait stays as the backstop for rewinds (`shims/d3d12_xs_device.cpp`). |
| `LEONIDA_INPUT_STATS` | unset | *Diagnostic.* `1`: every 10 s, how the title asked for input readings (by method and device argument) and what it got (`shims/gameinput.cpp`). |
| `LEONIDA_SAMPLE_FIND` | unset | *Diagnostic.* `<rva>[,<rva>...]` (hex, title function starts): every report of the thread sampler (`LEONIDA_SAMPLE_MS`) says which threads have the function on their stack (48 frames deep), how often, and their most frequent chain under it (`src/thread_sampler.h`). |

The two serializer functions and `D3D12XboxCreateDevice` are the only `d3d12_xs` imports used by the
shipped DLLs (measured; see `docs/design.md` and `docs/d3d12_vulkan.md`). The game executable's own
imports are unknown until it is available.

## Tests

For a Visual Studio build, pass CTest the same configuration you built above.

```powershell
ctest --test-dir runtime/build -C RelWithDebInfo --output-on-failure
```

On Windows the build configures the CPU tests that do not need a GPU or game files, including the
root-signature forwarding test (`root_sig_test`) and the clear-command test. The Vulkan tools and
D3D12-over-Vulkan COM targets are built when Vulkan headers are found (via `VULKAN_SDK`, an installed
SDK, or `-DLEONIDA_VULKAN_HEADERS_FETCH=ON` to download the pinned Khronos Vulkan-Headers release):

- `vk_harness` — hand-written RDNA2 shader programs on a real Vulkan device.
- `vk_texture_test`, `vk_compute_test`, `vk_store_test` — texture, LDS/compute, and buffer-store paths.
- `vk_command_replay_test`, `d3d12_com_test`, `d3d12_vulkan_device_test` — the D3D12 command path on
  Vulkan: replay, the COM command objects, and the Vulkan device behind the Xbox-ABI proxy.
- GPU tests count only validation-type errors. Loader and layer messages, such as a third-party implicit layer
  that fails to load, are printed but don't fail the run; to silence one, set
  `VK_LOADER_LAYERS_DISABLE=<layer name pattern>`.
- `d3d12_draw_test` — milestone M1: a VS+PS pipeline found by FSSM hash, the input layout matched by semantic,
  and five draws plus a culled triangle, compared pixel by pixel. They also cover an unbound render target and
  DSV, a rebound root signature, instance step rate 0, an unbound vertex slot, and PSOs, buffers and heaps that
  must be refused. A second pass draws a full-screen triangle with no vertex input and an XOR logic op.
- `d3d12_translated_draw_test` — milestone M1.5: the same with translator output. `test/shaders/vs_m15.asm` (vertex
  fetch through a V#) and `ps_m15.asm` (constants read through the code base) are built by `test/shaders/build.py`
  and found by FSSM hash; three draws into a float target, compared bit for bit. Needs `llvm-mc` (`LLVM_MC`) and the
  Vulkan SDK's `spirv-as` and `spirv-val` at build time.
- `d3d12_compute_test` — the compute path: root signature, a compute PSO found by its FSSM hash in a
  translator-style index, root arguments through the GPU address arena, and two dispatches. A second program is
  found the same way in `<shaders>/bvh/index.json`, where the BVH builder's programs go (`docs/embedded_shaders.md`).
- `d3d12_fspx_compute_test` — compute PSOs from synthetic FSPX programs (`docs/fspx.md`): an `ACCC` container
  through slot 62 and a bare kind-6 program, found by `fspx_code_sha256` in `<shaders>/fspx/index.json`, one
  dispatch each that reads its kind-6 program as the code base; malformed directories and untranslated programs
  are refused with `E_INVALIDARG`. Also slot 67 with all four pairs (picked by kind 0), refused pairs, and the
  pipeline's console slot 11.
- `d3d12_set0_compute_test` — set 0 from the compute bind point: a compute PSO whose index entry lists set-0
  bindings, and four dispatches that sample two SRVs, bytes no view was made for and a clamping sampler through
  the bindless set the layer binds.
- `d3d12_present_test` — milestone M0: opens a window and presents 60 cleared frames through the proxy on
  Vulkan, resizes it and presents 10 more, and checks the colour in the presented images (the presenter's
  capture) and on screen where GDI can read it. A driver double then injects an
  out-of-date and a suboptimal present, a failed semaphore during a swapchain recreation and a failed blit
  submission, which the presenter must survive. Set
  `LEONIDA_VK_VALIDATION=1` to run them under the Khronos validation layer.
- `vk_pipeline_check <spirv dir> [cache.bin] [--jobs N] [--start I] [--count N] [--max-bytes N]` -
  creates a shader module and, for compute shaders, a pipeline for every entry in the translator's
  `index.json`; reports `MODULE FAIL` / `PIPELINE FAIL` per shader and a summary. `--max-bytes`
  names and skips modules larger than the limit, which is how the corpus run works around the
  driver's compile time on multi-megabyte ray-tracing shaders. The measured partial pass is in
  [docs/pipeline_check.md](../docs/pipeline_check.md).

`gameinput_test` (only with SDL3; otherwise CTest lists `gameinput` as not run) drives the GameInput shim on SDL virtual joysticks (no controller needed): enumeration, readings
and the button/axis mapping, rumble and trigger rumble, DualSense effect bytes, a dispatcher, and hot-plug.
`gameinput_no_sdl_test` (only without SDL3) checks the shim a build without SDL3 links: `gameinput_no_sdl` the
identity-only object, `gameinput_no_sdl_script` the scripted pad of `LEONIDA_INPUT_SCRIPT` on a fake clock (device
callbacks, readings at chosen times, the reading history).
`gameinput_probe [--seconds N] [--rumble] [--force-feedback]` is the manual check on a real pad: it prints live
readings, plays each motor and trigger, and holds a constant force on a pad that reports a force-feedback motor.

`unimplemented_policy_test` builds a small image whose two imports bind to nothing and checks `log`, `fail` and
`stop` on it: return values, untouched out parameter, xmm0 = 0, environment parsing, and (in a child process) the
`stop` message with the caller's module+RVA and the argument registers. It also checks that the stub keeps the stack
16-byte aligned at the call into the runtime, that rbx/rbp/rdi/rsi/r12-r15 and xmm6-xmm15 survive a call
(`test/fixtures/abi_probe.asm`, MASM; skipped without `ml64`), and that `RtlVirtualUnwind` from inside the call walks
back to the caller.

### PE-loader fuzzer: `pe_loader_fuzz`

`pe_loader_fuzz` (RT-388) is a deterministic mutation fuzzer for `Loader::load`. Each case builds three small
synthetic DLLs in the test itself (headers and four sections, base relocations, import and delay-import tables, exports
with forwarders and ordinal-only entries, a TLS directory with template, index and callbacks, and an exception
directory with `UNWIND_INFO`), mutates their fields and bytes, writes them to a scratch folder and loads the first one.
A case passes when `load()` returns or throws a `std::exception`, the `Loader` is destroyed again, and after a
successful load every export and unwind table lies inside its image. It fails on a crash, an AddressSanitizer report,
a non-`std::exception` throw, or a case that runs past `--timeout-ms` (30 s). No image code runs (it never calls
`initialise()`), and nothing is read from the game.

CTest runs the first 120 cases of the fixed seed. Case `N` of a seed is the same everywhere, so a failure reproduces
with `--case N` and shrinks with `--minimize N`, which retries in child processes with each mutation left out and
prints the ones the failure needs. `--dump N DIR` writes the case's images for a closer look. A process memory limit
(`--mem-mb`, 1024 by default) turns a mutated size of several gigabytes into a failed allocation. On a machine
with many threads, most of each case is the loader's snapshot of every thread in the system for a TLS module
(`install_on_existing_threads`); expect a few cases per second.

```powershell
pe_loader_fuzz --iters 20000                 # or --seconds 3600; LEONIDA_LOADER_FUZZ_ITERS/_SECONDS/_SEED
pe_loader_fuzz --seed 0x4C454F4E49444131 --case 1234 --verbose
pe_loader_fuzz --minimize 1234
pe_loader_fuzz --start 10000000 --seconds 3600   # a second process on another range of cases
```

For AddressSanitizer, configure a separate build with `-DLEONIDA_LOADER_FUZZ_ASAN=ON` and `RelWithDebInfo`
(`/fsanitize=address` does not combine with Debug's `/RTC1`). That instruments the fuzzer and the loader sources
compiled into it, and copies `clang_rt.asan_dynamic-x86_64.dll` beside the test. MSVC needs the Visual Studio
component "C++ AddressSanitizer". clang-cl from an official LLVM release works with the MSVC headers and libraries
(LLVM 23.1.5 with MSVC 14.44 was tested). Run it from an x64 Developer prompt:

```powershell
cmake -S runtime -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLEONIDA_LOADER_FUZZ_ASAN=ON `
  -DCMAKE_C_COMPILER=<llvm>/bin/clang-cl.exe -DCMAKE_CXX_COMPILER=<llvm>/bin/clang-cl.exe `
  -DCMAKE_LINKER=<llvm>/bin/lld-link.exe -DCMAKE_AR=<llvm>/bin/llvm-lib.exe -DCMAKE_RC_COMPILER=<llvm>/bin/llvm-rc.exe `
  -DCMAKE_MT=<llvm>/bin/llvm-mt.exe
cmake --build build-asan --target pe_loader_fuzz
build-asan/pe_loader_fuzz --seconds 3600
```

With clang-cl the target is built with `-fsanitize-address-use-after-return=never`: clang-cl 23's
stack-use-after-return frames make MSVC C++ exception dispatch fault in `_C_specific_handler_noexcept` at `/O2`, and
every rejected image is a thrown exception. STL container annotations are off (`_DISABLE_STRING_ANNOTATION`,
`_DISABLE_VECTOR_ANNOTATION`), because they need the component's `stl_asan.lib` and the libraries linked in are
built without ASan.

The PE-loader and Bink-decode tests that need the retail package are optional: when
`LEONIDA_GAME_DIR` (and, for Bink, `LEONIDA_MOVIE`) is unset they print `SKIP` and exit 0. They read
the files in place and never copy or commit game data.

`shader_library_corpus_test` runs the D3D12 layer's shader lookup (`src/shader_library.h`) over a work folder
that `make_game.py --stages shaders` built from your copy. With `LEONIDA_SHADERS=<work>/shaders/spirv` it takes
every container in `<work>/shaders/dxbc` (or `LEONIDA_SHADER_DXBC`), inflates its FSSM program and looks it up
under the stage its DXIL program header names. Each container has to find its own `index.json` entry and a
readable `.spv`, every entry has to be reached, and in containers with more than one 0x0D payload no earlier
payload may be the indexed program. Without `LEONIDA_SHADERS` it prints `SKIP` (CTest reports it as skipped).
It prints counts only; on the 1.0.2.1 corpus all 26,503 containers resolve.

## Current limits

`gta6rt --run <exe> [-- args]` runs an executable's entry point inside the runtime (`shims/process.cpp` gives it its
own command line and turns its `exit` into the end of the run); CTest's `run_tiny_game` does that with a stand-in game.
The game's `atexit` handlers and static destructors go to a table of the run's own (the CRT's process-wide one is
shared with gta6rt): `exit` and a return from `main` run them newest first before the run ends, `_cexit` runs them and
returns, and `_exit`, `quick_exit`, `ExitProcess` and `abort` skip them, as natively. `run_tiny_atexit` runs a stand-in
program natively and through `--run` for each of those paths (and for `_exit` from a second thread during `exit`) and
holds both to the Windows sequence (under Wine, whose CRT differs, a native difference is only reported). The
executable's thread-local destructor callback (`_register_thread_local_exe_atexit_callback`) is kept by the run too:
the host UCRT accepts one per process and fast-fails the second, and gta6rt's own startup registers the first. `exit`
and `_cexit` call it once, as `(nullptr, DLL_PROCESS_DETACH, nullptr)`, before the atexit handlers, as the UCRT does,
so the leaving thread's `thread_local` objects end before the static ones; `_exit`, `quick_exit`, `ExitProcess` and
`abort` do not. Other threads' `thread_local` objects end through the loader's thread-detach notification, which calls
the same function from the executable's TLS directory. `run_tiny_atexit` covers `exit` and `_cexit` with a
`thread_local` object. `at_quick_exit` handlers are not covered (`quick_exit` runs none).
`run_tiny_frame` does it with a stand-in that imports `d3d12_xs.dll!D3D12XboxCreateDevice` and, with
`LEONIDA_D3D12=vulkan`, clears a render target and reads it back on the Vulkan device: the loader, the game thread,
the Xbox-ABI proxies and the device in one process (`test/fixtures/tiny_frame.cpp`; `test/check_tiny_frame.py` checks
the verdict, the `shim` binding and the call order in the `--trace-d3d12` output, and skips without a Vulkan GPU).
The game does not boot yet. The missing executable blocks the import-driven work: the real device
entry point, command-list/queue/fence extensions, `PresentX`, and most of the `xgameruntime` surface (an offline
shim covers `XGameRuntimeInitialize`, device/console queries, `XTaskQueue` and `XAsync`; see `xgameruntime_test`). The
D3D12-to-Vulkan design and the measured console ABI are in
The game does not boot yet, and none of this has been measured against the real executable. The missing executable
blocks the import-driven work:

- The device entry point: `D3D12XboxCreateDevice` exists and wraps the PC or the Vulkan-backed device in the
  measured-slot proxy, but its parameter and flag layout is unmeasured, and the Xbox device vtable is only partly
  measured: the FSR DLL's calls confirm device slots 14, 15, 16, 18, 19, 27, 38, 62 and 76, slot 67 is a
  console-only pipeline call, and the rest is open (`docs/d3d12_vulkan.md`, question 2).
- The command-queue and fence extensions: the proxies forward the public slots. Of the console's own queue methods
  past them, `PresentX` is routed (slot 30, or 32 on a queue detected as the newer layout; `docs/d3d12_vulkan.md`)
  to the presentation the device proxy builds on the first call (`shims/xbox_present.*`, `xbox_present_test`);
  `SuspendX` and `ResumeX` have unmeasured slots and are routed only when their variables name one. The other slots
  past the PC table answer `S_OK` and do nothing.
- Most of the `xgameruntime` surface: an offline shim covers `XGameRuntimeInitialize`, device/console queries,
  `XTaskQueue` and `XAsync`, and `QueryApiImpl` serves `XThreadingImpl`, `XErrorImpl`, `XNetworkingImpl`,
  `XPersistentLocalStorageImpl` and `XSystemImpl` (see `xgameruntime_test`); any other class is refused with a log.

The D3D12-to-Vulkan design and the measured console ABI are in
[docs/d3d12_vulkan.md](../docs/d3d12_vulkan.md); the shader translator and its GPU checks are described
in [docs/design.md](../docs/design.md), and the differential-validation design is in the shader-validation
work.

No game data, keys, or extracted assets belong in this directory or in the repository.

### First-boot D3D12 trace

On a Vulkan-enabled build, select the Vulkan backend to trace device, command-list and queue calls handled
by Leonida. Put `--trace-d3d12` before the separator for game arguments.

```powershell
$env:LEONIDA_D3D12 = 'vulkan'
.\runtime\build\gta6rt.exe --run $gameExe --trace-d3d12 2>&1 |
    Select-String '^\[d3d12-trace\]'
```

Each trace line contains a sequence number, a fixed API name and an outcome. `called` marks a void API call
whose validation may be deferred. `ok`, `failed` and `unsupported` report known results. `ignored` marks a no-op.
Share only the lines prefixed with `[d3d12-trace]`. Other runtime diagnostics are separate.

### Startup trace: `--trace FILE`

`--trace FILE` (with `--dry-run`, `--init` or `--run`, before the `--` separator) writes one JSON object per line
to FILE, flushed per event so the file survives a crash ([shims/startup_trace.h](shims/startup_trace.h)). FILE must
not exist yet: the trace is created before the loader reads anything, so gta6rt refuses an existing path (exit code 2)
rather than risk truncating an input such as the image. The file is UTF-8; `OutputDebugStringA` text is converted
from the ANSI code page.

| `kind` | When | Fields |
|---|---|---|
| `trace_open` | first line | `pid` |
| `tls_index0` | the first executable is mapped with module TLS index 0, which gta6rt freed at start (once per process) | `owner` (the image that held index 0, `gta6rt.exe`), `moved_to` (the index its block moved to) |
| `map` | a module is mapped, bound and protected | `module`, `base`, `size`, `relocated` |
| `tls_callback` | before each TLS callback the Loader runs (process attach and detach; thread events run under the Windows loader lock and are not traced) | `module`, `at` (module+RVA), `reason` |
| `dllmain` | after `DllMain(DLL_PROCESS_ATTACH)` returns | `module`, `result` |
| `unimplemented_call` | first call of each unimplemented import | `import` (`dll!name`), `caller` (module+RVA) |
| `delay_import_unimplemented` | at load, for each delay import that binds to an unimplemented thunk (Windows would raise `0xC06D007E`/`0xC06D007F` at its first call; [runtime_imports.md](../docs/runtime_imports.md#delay-imports-and-thread-events-where-the-loader-differs-from-windows)) | `module`, `import` (`dll!name`) |
| `xmem_attributes` | the first `XMemAlloc`, `XMemFree`, `XMemAllocDefault`, `XMemFreeDefault` and `XMemVirtualAlloc` for each distinct raw attribute word (not every allocation) | `api`, `attributes`, `size` (not for the frees), `caller` (module+RVA) |
| `xmem_physical_pages` | the first `XMemAllocatePhysicalPages` for each distinct attribute word | `api`, `attributes`, `pages`, `caller` |
| `file_open_failed` | a guest `CreateFileW`/`CreateFile2` that fails (at most 256 events) | `api`, `path` (as the title passed it), `error` (Win32), `caller` (module+RVA) |
| `int3_skipped` | with `LEONIDA_SKIP_INT3` set when the trace opens: first int3 at each address, which is then stepped over (after 4096 distinct addresses, a new one is logged each time it is hit) | `at` (module+RVA) |
| `debug_string` | `OutputDebugStringA/W` with no debugger attached | `text` |
| `exception` | any exception, before handlers run | `code`, `at`, `access`/`address` for access violations, `cpp` for C++ throws, `stack` (up to 16 frames) |
| `exit` / `error` | `--run` returns, or the loader throws | `code` / `what` |

Every line also has `seq` and `t` (seconds since the trace opened). Addresses are `module+0xRVA` for mapped images,
`host:name.dll+0xRVA` for host modules. The exception handler only records: it returns `EXCEPTION_CONTINUE_SEARCH`,
so the process behaves the same with or without the trace (except under the diagnostic `LEONIDA_SKIP_INT3`, which
steps over int3). It works with any `LEONIDA_UNIMPLEMENTED` policy.

The file can quote what the game prints, so it stays on your machine. In issues and pull requests, share counts or
lines from synthetic fixtures only.

```powershell
.\runtime\build\gta6rt.exe --run $gameExe --trace startup.jsonl
Get-Content startup.jsonl | ConvertFrom-Json | Group-Object kind | Select-Object Count, Name
```

[tools/boot_trace.py](../tools/boot_trace.py) prints the first-boot summary of this file and of the
`--trace-d3d12` log (modules, unimplemented calls with their callers, DllMain/TLS results, exceptions with their
stack, the exit, and the D3D12 calls by outcome and first call per method):

```powershell
python -I tools\boot_trace.py startup.jsonl --d3d12 d3d12.log
```

[docs/boot.md](../docs/boot.md) is the runbook around it: the commands, the triage order and the open questions'
trace signals.

### Typed clear regression

`clear_color_test` checks numeric UINT conversion and unchanged float clears without a GPU.
`d3d12_clear_test` compares full/rectangle readbacks with Windows WARP across UNORM, SNORM, FLOAT, UINT and SINT
render-target formats, plus depth and stencil clears. Fixtures are synthetic; no shaders or game data are needed.
The GPU test runs explicitly, while the CPU test is registered in CTest.

```powershell
ctest --test-dir runtime/build -C RelWithDebInfo -R '^clear_color$' --output-on-failure
$env:LEONIDA_VK_VALIDATION = '1'
$env:VK_VALIDATION_VALIDATE_SYNC = '1'
.\runtime\build\RelWithDebInfo\d3d12_clear_test.exe
```

### RGBA8 typeless RTV clear regression (phase 1)

`typeless_rtv_test` checks typed-view recording and replay without a GPU. It is registered in CTest.
`d3d12_typeless_rtv_test` compares synthetic full and rectangular clears with Windows WARP through explicit
UNORM/SRGB/UINT RTVs on 2D RGBA8 typeless resources, including mip slices, array-layer ranges and 4x MSAA.
SINT/SNORM clears are compared where both Vulkan and WARP report render-target support. It checks descriptor
snapshots, alternating view formats, SRGB with UAV resource usage, and invalid view rejection.

```powershell
ctest --test-dir runtime/build -C RelWithDebInfo -R '^typeless_rtv$' --output-on-failure
$env:LEONIDA_VK_VALIDATION = '1'
$env:VK_VALIDATION_VALIDATE_SYNC = '1'
.\runtime\build\RelWithDebInfo\d3d12_typeless_rtv_test.exe
```

### 96-bit R32G32B32 textures (D3D-62)

`d3d12_rgb96_test` compares footprints, uploads, readbacks, boxes, texture-to-texture copies and CPU copies of
`R32G32B32_FLOAT/UINT/SINT/TYPELESS` textures (2D arrays with mips, and volumes) with Windows WARP, bit for bit,
checks that WARP and the hardware adapter report no resource tiling for all four formats, and prints how long an
upload and a readback take. `d3d12_descriptor_test` samples them (alpha reads as one). It
is registered in CTest (`d3d12_rgb96`).

```powershell
$env:LEONIDA_VK_VALIDATION = '1'
$env:VK_VALIDATION_VALIDATE_SYNC = '1'
.\runtime\build\RelWithDebInfo\d3d12_rgb96_test.exe
```

RGBA8 typeless RTV clears support explicit UNORM/SRGB/UINT/SINT/SNORM views of 2D mip slices and contiguous
array-layer ranges, and MSAA. Other typeless color families also accept same-family typed RTVs, subject to Vulkan
color-attachment support. Typed-resource RTV casts are limited to R8G8B8A8_UNORM resources whose castable-format
list allows the view. A 1D texture cannot be created as a render target or depth buffer (`CreateCommittedResource`
refuses `ALLOW_RENDER_TARGET`/`ALLOW_DEPTH_STENCIL` on it), and 3D typeless view reinterpretation remains refused.

### Resource descriptions

`d3d12_resource_desc_test` creates the same committed and reserved resources on the Vulkan-backed device and on
Windows D3D12 (the default adapter and WARP, whichever exist) and compares every field of `ID3D12Resource::GetDesc`:
the `Alignment` and `MipLevels` a description left at 0 must read back as the host fills them in. A description the
host refuses is skipped, and one only the layer refuses is listed and not compared. It needs a
Vulkan GPU and runs explicitly.

```powershell
$env:LEONIDA_VK_VALIDATION = '1'
.\runtime\build\RelWithDebInfo\d3d12_resource_desc_test.exe
```

`d3d12_placed_dimensions_test` places 1D and 3D textures (arrays, mips, BC1, UAV and render targets, plus a 2D
control) in heaps big enough for both sides' `GetResourceAllocationInfo`, at offset 0 and at 64 KiB, on the
Vulkan-backed device and on Windows D3D12 (the default adapter and WARP), and compares the answers and `GetDesc`. A
dimension outside the enum must be refused alike. It also copies a pattern through a buffer into every subresource of a placed
3D texture and a placed 1D array and back, and checks the bytes. It needs a Vulkan GPU.

```powershell
$env:LEONIDA_VK_VALIDATION = '1'
.\runtime\build\RelWithDebInfo\d3d12_placed_dimensions_test.exe
```

### 1D render targets and depth buffers

`d3d12_texture1d_target_test` creates `TEXTURE1D` textures with `ALLOW_RENDER_TARGET` (10 color formats) or
`ALLOW_DEPTH_STENCIL` (4 depth formats), one texture and a 6-layer array each, on the Vulkan-backed device, the
hardware adapter and WARP. Where every reference creates one and the Vulkan device can make that 1D attachment image,
the layer must create it and `GetResourceAllocationInfo` must answer a size. It then clears through `TEXTURE1D` and
`TEXTURE1DARRAY` RTVs and DSVs (whole and with rectangles, on mips and slice ranges) and through a 1D UAV with a
rectangle, and compares every subresource's bytes with WARP. It is registered in CTest (`d3d12_texture1d_target`).

```powershell
$env:LEONIDA_VK_VALIDATION = '1'
.\runtime\build\RelWithDebInfo\d3d12_texture1d_target_test.exe
```

### Depth and stencil resolves

`d3d12_depth_resolve_test` resolves one plane of a 4x D32 or D24S8 target into a 1x one with
`ResolveSubresourceRegion` (MIN and MAX of depth or stencil, AVERAGE of depth by its own format; whole and by
rectangle) on the Vulkan-backed device and on Windows D3D12 (the default adapter and WARP), and compares the
destinations read back. Calls the layer refuses must fail `Close` and leave the destination as cleared. Clears give
every sample of a pixel one value, so the modes over differing samples are not checked. It needs a Vulkan GPU.

```powershell
$env:LEONIDA_VK_VALIDATION = '1'
.\runtime\build\RelWithDebInfo\d3d12_depth_resolve_test.exe
```

### Predicate capture lifetime

`vk_predicate_capture_test` checks full-width predicate snapshots, offset rebinding and reset with 32 gated submissions, then reuses completed command-buffer slots. Run explicitly on a Vulkan GPU with conditional rendering; it uses only synthetic data.

### A synthetic guest draws its first offscreen frame

On a Windows Vulkan build, configure CMake with `-DLEONIDA_TEST_GUEST_DRAW=ON` and build the
`tiny_guest_draw` and `gta6rt` targets. With llvm-mc, spirv-as and spirv-val available at configure
time, run `ctest --test-dir runtime/build -C Release -R "^d3d12_translated_draw_guest$" -V`.
The loader maps the test executable. It imports the Xbox device factory, submits three draws using two
handwritten shaders, reads back a 64×64 offscreen texture and checks every pixel.

The guest also builds independently of the shader tools. To produce the same synthetic fixture on
another machine, run `python -I runtime/test/shaders/build.py m15_shaders vs_m15 ps_m15` from the
checkout with llvm-mc, spirv-as and spirv-val available. Copy the whole resulting folder to the test
machine. For Wine, make the runtime executable, its runtime DLLs, the guest executable and the fixture
accessible in the same Wine prefix. For example, from their containing directory with the guest and
fixture beside the loader:

```sh
fixture=$(winepath -w "$PWD/m15_shaders")
shaders=$(winepath -w "$PWD/m15_shaders/spirv")
guest=$(winepath -w "$PWD/tiny_guest_draw.exe")
LEONIDA_D3D12=vulkan LEONIDA_SHADERS="$shaders" wine ./gta6rt.exe --run "$guest" -- "$fixture"
```

A passing run exits zero and prints `M1.5 guest: PASS, 4064 of 4096 pixels exact, 32 edge pixels the clear colour`.
The 32 diagonal pixels must all match the clear colour exactly. The fixture does not create a window.

### A 600-frame headless guest scene

Configure a Windows Vulkan build with `-DLEONIDA_GUEST_SCENE=ON` and static SDL3
(`SDL3_DIR` or `-DLEONIDA_SDL3_FETCH=ON`). Build `gta6rt` and `tiny_guest_draw`.
With the shader tools and Python available at configure time, CTest runs the existing
one-frame test and the scene with `ctest -R "^(d3d12_translated_draw_guest|d3d12_guest_scene)$" -V`.
For an externally generated M1.5 shader fixture, use the runner directly:

```sh
python -I scripts/run_guest_scene.py --loader path/to/gta6rt.exe --guest path/to/tiny_guest_draw.exe \
    --shaders path/to/m15_shaders --out new-scene-run --negative-controls
```

For Wine, add `--wine path/to/wine`. With a Linux cross-build, CTest also registers
`d3d12_guest_scene` when CMake finds the shader tools and has a Wine command in
`CMAKE_CROSSCOMPILING_EMULATOR`. Build the `tiny_guest_draw` target, then run
`ctest --test-dir <build> -R '^d3d12_guest_scene$' --output-on-failure`.
On a headless host, prefix that CTest command with `xvfb-run -a` so the scene runner
keeps guest stdout and stderr separate while checking the frame-count marker.
The runner launches the mapped guest for an
uninterrupted 600-frame run, a 300-frame save, and a fresh process that restores the
checkpoint. This opt-in build links a fixture input source into the loader. Scripted keyboard changes feed the SDL virtual pad. GameInput readings
drive the position of one quad. The guest streams ten 16-byte synthetic palette pages from
`GAME:\scene.bin` and changes its shader constants. A software reference independently
predicts the GPU pixel digest for each frame and checks six captured images.

Each run writes frame records, a state file, selected PPM readbacks, and a checkpoint
where applicable. At the first A-button edge the guest generates a quarter-second PCM
WAV. Playback is off by default. `--play-audio` requests playback through host WinMM
and records its result. The runner uses a fresh output directory, or a temporary one
when `--out` is omitted. The negative controls independently corrupt a scene byte and
a saved-state byte and require the expected guest errors. This fixture renders offscreen.
