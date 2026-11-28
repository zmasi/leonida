# Where the retail executable stops itself on an import's result (RT-5)

A title treats some results as impossible. It tests the value an import returned and, when it is the failing one,
branches to `int3`: MSVC's `__debugbreak`, and the marker it places after a call that does not return. A shim that
returns that value at such a site ends the boot there, with no message. This page lists those sites for
`game_scarlett_master.exe`, per import: how many there are and which value takes them. It shows which results each shim
must get right before a run reaches them.

Nothing was run. `tools/import_fail_fast.py` reads the file: it finds each import's call sites (through its thunk with
`tools/import_call_sites.py`, and as `call [rip+slot]` for the `api-ms-win-*` imports). It then decodes the instructions
after each call up to the test of the result and follows both sides of the branch for a few instructions. The method
and its limits are in the tool's docstring. No code or data of the executable is quoted here, only names, counts and
conditions. Measured on SHA-256 `e95020b2fa4a721db3df59fdecf75621730ac559d8835252f6c3b658d5a08993`. The bindings below
are those of `main` `d1dff7949`.

```powershell
python -I tools/import_fail_fast.py game_scarlett_master.exe
python -I tools/import_fail_fast.py game_scarlett_master.exe --dll xmem.dll --all --json
```

Result: 515 of 587 imports called directly, 27 with a fatal result check, 78 fatal sites. An independent pass with Capstone found the
same sites, apart from two `XMemVirtualAlloc` sites that the tool finds and that pass missed. At one, the result is copied
to another register before the test. At the other, the failing side reaches `int3` through a `jmp`.

## The sites, by how `main` binds the import

"Stops when" is the result that takes the fatal side. It is written with the register at the width the test reads:
`eax < 0` is a failed `HRESULT`, `rax == -1` is `INVALID_HANDLE_VALUE`.

All 13 non-host imports with a fatal result check are bound by shims on `main` `d1dff7949`, including the 6 that
previously had no binding (the four physical-page / tools-memory `xmem.dll` exports — 8 sites that failed on a default
return of 0 — plus `AcpHal.dll!ApuCreateHeap` and `GameInput.dll!#1`), along with two shimmed `kernel32` imports
(`VirtualFree` and `ReadFile`), for **15** shimmed imports and **12** host/CRT imports. Zero non-host fatal-checked
imports remain unbound.

### Shims: results to keep

| Import | Calls | Fatal | Stops when | Note |
|---|---:|---:|---|---|
| `xmem.dll!XMemVirtualAlloc` | 23 | 14 | `rax == 0` | `runtime/shims/xmem.cpp` takes the attribute word and drops the `PAGE_GRAPHICS_*` bits. It handles the 2 MiB page mode (`allocation_type` `0x20003000`), which 4 of these 14 sites pass, reserving it 2 MiB-aligned through `VirtualAlloc2`; the other 10 reach `VirtualAlloc`. A host without the placeholder API (`VirtualAlloc2`, `MapViewOfFile3`) still gets `ERROR_NOT_SUPPORTED` for those 4. The 6 calls with `0x4000` (`MEM_REPLACE_PLACEHOLDER`) or `0x40000` (`MEM_RESERVE_PLACEHOLDER`) that `docs/xmem_exe_calls.md` lists as refused are placeholder windows the shim now makes too; they are not among the 14 |
| `VirtualFree` | 23 | 12 | `eax == 0` | `runtime/shims/xmem.cpp` (`TitleVirtualFree`, bound on `kernel32.dll`, `kernelbase.dll`, and `api-ms-win-core-memory-l1-1-0..6.dll`): locks GPU label memory, removes tracked windows/placeholders on `MEM_RELEASE` under the xmem/fault locks, calls host `VirtualFree`, and invalidates `title_memory_map`, write-watch, and geometry-write state |
| `xmem.dll!XMemMapPhysicalPages` | 12 | 3 | `rax == 0` | `runtime/shims/xmem.cpp` maps 64 KiB views via `MapViewOfFile3` into a reserved window and returns `address` on success (or unmaps when `page_array` is null); returns `nullptr` on invalid arguments, a missing window, or hosts without `VirtualAlloc2`/`MapViewOfFile3` (`ERROR_NOT_SUPPORTED`) |
| `xmem.dll!XMemAllocatePhysicalPages` | 4 | 2 | `eax != 1` (1), `eax == 0` (1) | `runtime/shims/xmem.cpp` backs the requested 64 KiB pages with a `SEC_RESERVE` pagefile mapping (committed on touch by the vectored fault handler) and returns `TRUE` (1), or `FALSE` (0) on invalid parameters, slot exhaustion, or mapping failure |
| `xmem.dll!XMemMapPhysicalPagesScatter` | 2 | 2 | `eax == 0` | `runtime/shims/xmem.cpp` maps or unmaps one 64 KiB page per address and returns `TRUE` (1), or `FALSE` (0) on invalid address/window or hosts without `VirtualAlloc2`/`MapViewOfFile3` |
| `xmem.dll!XMemIsToolsMemAvailable` | 1 | 1 | `eax == 0` | `runtime/shims/xmem.cpp` deliberately returns `FALSE` (0) because a PC has no devkit tools memory; the function holding the site has 6 callers, each reaching it only after an allocation failed, so the title's own breakpoint marks out-of-memory |
| `XMemGetWorkingSetStatistics` | 3 | 2 | `eax == 0` | `runtime/shims/xmem.cpp` returns `FALSE` only for a null output or a failed `GetProcessMemoryInfo`; both sites pass a stack buffer |
| `XMemTransferMemory` | 1 | 1 | `eax == 0` | `runtime/shims/xmem.cpp` returns `TRUE` |
| `CreateFileW` | 13 | 1 | `rax == -1` | One open that has no failure path: the file must exist where `runtime/shims/file_system.cpp` maps it |
| `ReadFile` | 2 | 1 | `eax == 0` | `runtime/shims/file_system.cpp` (`read_file`, bound on `kernel32.dll`, `kernelbase.dll`, and `api-ms-win-core-file-*`): pins and opens guarded shadow destination pages via `leonida::kernel_writes::read_file`, and on `ERROR_NOACCESS` commits uncommitted physical-page destinations from user mode and retries once |
| `GetModuleHandleW` | 3 | 1 | `rax == 0` | `runtime/shims/library_loader.cpp`: two calls in a row, for `api-ms-win-core-synch-l1-2-0.dll` and then `kernel32.dll`; the title stops when both give `NULL` |
| `_configure_narrow_argv` | 1 | 1 | `eax != 0` | `runtime/shims/process.cpp`: the CRT's start-up; the shim returns 0 |
| `d3d12_xs.dll!D3D12ConfigureVirtualMemoryX` | 1 | 1 | `eax < 0` | `runtime/shims/d3d12_xs.cpp` returns `S_OK` |
| `AcpHal.dll!ApuCreateHeap` | 1 | 1 | `eax < 0` | `runtime/shims/acphal.cpp` allocates the cached/non-cached APU heaps via `VirtualAlloc` and returns `S_OK` (0); returns `E_FAIL` if already created, `E_INVALIDARG` on address-space overflow, or `E_OUTOFMEMORY` if allocation fails |
| `GameInput.dll!#1` | 1 | 1 | `eax < 0` | `runtime/shims/gameinput.cpp` binds both `GameInputCreate` and ordinal `#1`, creating the process-wide `IGameInput` shim instance and returning `S_OK` (0) (`E_POINTER` on null output) |

### Host and CRT: not failures a shim causes

| Import | Calls | Fatal | Stops when | What it is |
|---|---:|---:|---|---|
| `MSVCP140.dll!_Mtx_lock` | 21 | 19 | `eax != 0` | `std::mutex::lock`'s error path |
| `IsProcessorFeaturePresent` | 3 | 3 | `eax != 0` | `PF_FASTFAIL_AVAILABLE` (`0x17`) before `int 29h`: the CRT's abort path, after the decision to stop |
| `GetLogicalProcessorInformation` | 2 | 2 | `eax != 0` (1), `eax == 0` (1) | The size query must fail, then the real call must succeed |
| `GetLastError` | 36 | 1 | `eax != 0x7a` | Between those two: the size query's error must be `ERROR_INSUFFICIENT_BUFFER` |
| `strcmp` | 464 | 2 | `eax == 0` | The title's own string checks |
| `_stricmp` | 160 | 1 | `eax != 0` | as above |
| `strrchr` | 32 | 1 | `rax == 0` | as above |
| `MSVCP140.dll!_Getcat` (`codecvt<char>`, `ctype<wchar_t>`) | 2 | 2 | `rax == -1` | Locale facets; `msvcp140.dll` binds to the host copy |
| `CreateEventW` | 1 | 1 | `rax == 0` | Host |
| `CreateSemaphoreExW` | 2 | 1 | `rax == 0` | Host |
| `WS2_32.dll!#115` (`WSAStartup`) | 1 | 1 | `eax != 0` | Host; `runtime/shims/ws2_offline.cpp` keeps `ws2_32.dll` host-owned with partial overrides (outbound/bind/DNS only), so `WSAStartup` runs natively on loopback sockets and returns 0 |

## What this does not show

A site found here is code that can stop the title, not proof that the boot reaches it: nothing ran. Only
straight-line code after each call is read. A result stored and tested later, or a failure path that logs before it
stops, is not seen, and a stray call byte pair inside another instruction could add a false site (rare). Each count
is a heuristic count that can miss sites or include false matches. [docs/retail_boot_chain.md](retail_boot_chain.md)
records the stops reached during real runs, in order.
