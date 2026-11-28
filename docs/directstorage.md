# DirectStorage

The console title reads its streamed data through Xbox DirectStorage: `DStorageGetFactory` hands out a factory, the
factory opens files and creates queues, and a queue reads file or memory ranges into memory and reports completion
through status arrays, `ID3D12Fence` signals and events. `runtime/shims/dstorage.cpp` serves that API on the PC:
files open through the runtime's `GAME:` mapping and write overlay, and a worker thread per queue does the reads
and inflates zlib requests.
This page records which DLL and ABI the console title is measured to use, how sure the rest is, and what the shim does.

## Which DLL and which ABI: the evidence

The executable is not in the package, but with a copy supplied its import table was measured
([runtime_imports.md](runtime_imports.md), "The retail executable (RT-5)"): it imports `DStorageGetFactory` from
`XCRDAPI.dll`, one import with one call site. [Below](#measured-factory-the-iid-and-the-vtable-slot) gives the call
site, the interface id it passes and the vtable slot it then calls. What the package itself shows:

| Evidence | Source | What it says |
|---|---|---|
| The executable imports `DStorageGetFactory` from `XCRDAPI.dll`, exactly once | [runtime_imports.md](runtime_imports.md) (RT-5 table), [audio_hal_exe_calls.md](audio_hal_exe_calls.md) | The serving module is measured, not only the name of a module. Neither `dstorage.dll` nor `dstorage_xs.dll` is imported by the executable |
| The single call site asks for `{62959D11-D45F-4884-A0A3-F7862EA243AE}` and then calls **slot 8** of the returned object with `0x02000000` | [Measured factory](#measured-factory-the-iid-and-the-vtable-slot), this page | The console factory adds one console-only method (`SetCpuAffinity`) alongside the shared `SetDebugFlags` at slots 6–7 between `CreateStatusArray` (5) and `SetStagingBufferSize` (8); PC `dstorage.h` puts `SetDebugFlags` at 6 and `SetStagingBufferSize` at 7 |
| None of the 16 shipped DLLs imports DirectStorage | [runtime_imports.md](runtime_imports.md) (6 Xbox-specific imports, all `xmem`/`d3d12_xs`), [textures.md](textures.md) "The shipped binaries" (no `DStorage` import, export or string) | DirectStorage, if used, is called by the executable itself |
| The texture chunks carry the `DSTORAGE_BCPACK_MODE` values 1-7 and mix zlib and BCPack chunks | [textures.md](textures.md) "What BCPack is" | The data is laid out for console DirectStorage's decompression (DEFLATE, then BCPack, then swizzle) |
| A sibling console layer's export inventory names `dstorage_xs.dll` as console DirectStorage | [d3d12_vulkan.md §7](d3d12_vulkan.md#7-prior-art-and-measured-xbox-abi) | The console module name, measured elsewhere, not here |
| A retail title run by a sibling layer has `xcrdapi` in its console-only library set | [d3d12_vulkan.md §7](d3d12_vulkan.md#7-prior-art-and-measured-xbox-abi) | The name in a library list; the executable below narrows it to this one export |
| The GDK reference: `DStorageGetFactory` is in header `dstorage_xs.h`, library `dstorage_xs.lib`; the interface pages give library `dstorage.lib`; all say "Xbox Series consoles" | [GDK DStorage reference](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/dstorage_members) | The console API has its own names (`IDStorageFactoryX`, `IDStorageQueueX`, `IDStorageFileX`, `IDStorageStatusArrayX`) and structures that differ from the PC API |
| `registry.cpp` classified `dstorage.dll`/`dstorage_xs.dll` `DStorageGetFactory` as returning an HRESULT | `runtime/shims/registry.cpp` | It was an unimplemented import; `xcrdapi.dll` was not named. The module is measured now and the registry serves all three names |

**Chosen ABI: the console one** (the GDK's `IDStorageFactoryX2`/`IDStorageQueueX2`/`IDStorageFileX`/
`IDStorageStatusArrayX` and its `DSTORAGE_REQUEST`), served as `DStorageGetFactory` under all three names:

| Module | Confidence | Why |
|---|---|---|
| `dstorage_xs.dll` | medium | The reference's `dstorage_xs.lib` and the sibling inventory agree. No shipped module and not the executable import it, so nothing in this title needs it; the name is served for a title that would |
| `xcrdapi.dll` | **measured** | The executable's only DirectStorage import is `DStorageGetFactory` from this DLL and it is the only name it imports from it ([RT-5](runtime_imports.md), [the call site below](#measured-factory-the-iid-and-the-vtable-slot)). `XCRDAPI.dll` is not in the package, so the console OS supplies it. Owning the module also means a title's `LoadLibrary("xcrdapi.dll")` gets the shims' pseudo-handle, and `GetProcAddress` for any other name returns `NULL` (logged). Static imports of other `xcrdapi` names are unchanged |
| `dstorage.dll` | low | The interface pages name `dstorage.lib`; the executable does not import it. Every caller of `gta6rt`'s import table is a console build, so this name serves the console ABI too, not the PC one |

The PC DirectStorage ABI (`dstorage.h`: `IDStorageFactory` with `DSTORAGE_REQUEST` source and destination unions,
`ID3D12Resource` destinations) is **not served**. A caller that asks for a PC interface id gets `E_NOINTERFACE` and a
log line, never an object of the wrong shape. The console factory id is not one of those: it is the measured
`{62959D11-D45F-4884-A0A3-F7862EA243AE}` and the factory accepts it (the shim also takes an id it has not seen and logs
it). The other exception is `IDStorageStatusArray`, whose two methods are the console interface's in the same order
(console D3D12 binaries reuse the PC SDK's ids), so a status array accepts its id.

Owning a module has a run-time side: `LoadLibrary` of `xcrdapi.dll` or `dstorage.dll` (or `dstorage_xs.dll`) gets
the shims' pseudo-handle even when the package ships its own copy of that DLL, so the shim shadows a packaged copy
at run time (`library_loader.cpp` asks the registry before the bundled loader). None of the 16 shipped DLLs is one
of these, so nothing is shadowed today.

### What is not public, and the choice made

| Unknown | Choice | Confidence | Where to change it |
|---|---|---|---|
| Vtable order of the console interfaces (the reference lists methods alphabetically) | The PC header's order for the methods both APIs have; console-only methods (`SetCpuAffinity`, `GetStats`, `CreateQueue1`, `GetHandle`, `Query1`) where the overview lists them: factory `CreateQueue, OpenFile, CreateStatusArray, SetCpuAffinity, SetDebugFlags, SetStagingBufferSize`, then `GetStats` (X1), `CreateQueue1` (X2); queue `EnqueueRequest, EnqueueStatus, EnqueueSignal, Submit, CancelRequestsWithTag, Close, GetErrorEvent, RetrieveErrorRecord, Query`, then `EnqueueSetEvent` (X1), `Query1` (X2); status array `IsComplete, GetHResult`; file `Close, GetHandle`. The factory order is now anchored: the one call site reaches `SetStagingBufferSize` at slot 8 (measured) where PC `dstorage.h` puts `SetDebugFlags` at 6 and `SetStagingBufferSize` at 7, confirming one console-only method (`SetCpuAffinity`) sits alongside `SetDebugFlags` at slots 6 and 7 before it. Which of `SetCpuAffinity` and `SetDebugFlags` is 6 and which is 7 is still unmeasured, as are the X1/X2 slots | queue, status array: medium; file: medium; factory: medium (slot 8 and the 6/7 pair count measured; the relative order of `SetCpuAffinity` and `SetDebugFlags` a guess) | `runtime/shims/dstorage_xs_abi.h`, one declaration per interface; `IDStorageFactoryX::SetStagingBufferSize` must keep slot 8 |
| Interface ids of the X interfaces | The factory's id is measured: `{62959D11-D45F-4884-A0A3-F7862EA243AE}`, passed at the only `DStorageGetFactory` site ([below](#measured-factory-the-iid-and-the-vtable-slot)); bytes sit at `0x62FC7D8`/VA `0x1462FC7D8`. The others are not: a creating call (`DStorageGetFactory`, `CreateQueue*`, `OpenFile`, `CreateStatusArray`) accepts an id it has not seen for the kind of object it hands out and logs it once (`[rt] dstorage: ... asked for {...}: serving the ... interface`), so the first boot records them. It refuses an id seen for another kind, a PC DirectStorage id, and the system COM interfaces (the `{xxxxxxxx-0000-0000-C000-000000000046}` range with `IMarshal`, `IClientSecurity`, ..., and `IAgileObject`, `INoMarshal`, `IInspectable` and a few more). `QueryInterface` follows [d3d12_vulkan.md](d3d12_vulkan.md)'s rule (an unknown id is `E_NOINTERFACE`, logged): it accepts only `IUnknown`, the declared `IDStorageStatusArray` id for a status array, and the ids a creating call accepted for that kind of object | factory id: measured; the policy: n/a (a policy, not a fact) | `IidPolicy` in `runtime/shims/dstorage.cpp`; the factory id belongs beside `kStatusArrayIds` in `dstorage_xs_abi.h` (`dsx::kFactoryId`) and should be the first id `DStorageGetFactory` accepts |
| `DSTORAGE_MIN_QUEUE_CAPACITY`, `DSTORAGE_MAX_QUEUE_CAPACITY`, `DSTORAGE_REQUEST_MAX_NAME` | The PC header's values: `0x80`, `0x2000`, 64 | medium | `dstorage_xs_abi.h` |
| `E_DSTORAGE_*` values | The PC `dstorageerr.h` values (`FACILITY_GAME`, `0x8924xxxx`). That header also defines the console-only failures (zlib, BCPack, swizzle, XVD), so the space is shared. `E_DSTORAGE_DECOMPRESSION_ERROR` (`0x89240030`) is on the public constants page; `E_DSTORAGE_ZLIB_BAD_HEADER`, `_ZLIB_BAD_DATA` and `_ZLIB_PARITY_FAIL` (`0x89240031`-`33`) have no public description, and their names and values come from the header-generated bindings of the `direct-storage` crate (Sources). Which failure the console reports with each zlib code is inferred from the names | medium-high (values); medium (zlib codes' meaning) | `dstorage_xs_abi.h` |
| Struct layouts | The compiler's x64 layout of the reference's declarations, pinned with `static_assert` (`DSTORAGE_REQUEST` 72 bytes, `DSTORAGE_QUEUE_DESC` 24, `DSTORAGE_QUEUE_DESC1` 32); `DSTORAGE_REQUEST_OPTIONS` is decoded from `AsUINT64` by explicit bit positions (zlib bit 0, BCPack 1-4, swizzle 5-20, destination pages 21, source type 22, source pages 23, reserved 24-63) | high for member order and types, medium for the bit allocation (MSVC allocates bit-fields from bit 0 up) | `dstorage_xs_abi.h` |

The first boot with the executable still settles what the file does not: whether the boot reaches these calls at all (the
dry run and a trace measured elsewhere stop earlier, in `D3D12ConfigureVirtualMemoryX`,
[audio_hal_exe_calls.md](audio_hal_exe_calls.md)). The module and the factory's id no longer need a boot to find; the
queue, file and status-array ids, and the relative order of `SetCpuAffinity` and `SetDebugFlags` at factory slots 6 and 7,
still do.

## Measured factory: the IID and the vtable slot

Reproduce the call site count and the thunk from the file alone:

```powershell
python -I tools/import_call_sites.py game_scarlett_master.exe --import XCRDAPI.dll!DStorageGetFactory --json
```

It reports one `call` and one thunk, at RVA `0x1D8E4F0` / VA `0x141D8E4F0` and RVA `0x5F3FF80` / VA `0x145F3FF80`. The
thunk is the ordinary `jmp qword [rip+…]` through the import's address-table slot (RVA `0x6793FB8`), and the slot holds
the hint 0, name `DStorageGetFactory`. Capstone around the call site (`0x141D8E4DB`–`0x141D8E51C`), which is the one-time
initializer of a factory singleton (a `-1`-sentinel one-time wrapper at `0x141D8E470`, itself called from six places,
reaches it):

```
lea  rcx, [rip+0x456E2F6]      ; VA 0x1462FC7D8: the interface id, by address
lea  rdx, [rip+0x5B6798F]      ; VA 0x1478F5E78: the out pointer
mov  [rip+0x5B67990], rax      ; VA 0x1478F5E80: adjacent global stored before the call
call 0x145F3FF80               ; DStorageGetFactory(REFIID, void**)
mov  rcx, [rip+0x5B6797C]      ; VA 0x1478F5E78: the object the call wrote, tested for null
test rcx, rcx                  ; je -> int3, then test eax, eax / je -> reload + vtable call (else int3)
mov  rcx, [rip+0x5B6796B]      ; VA 0x1478F5E78: reload after the S_OK check
mov  rax, [rcx]                ; its vtable
mov  edx, 0x02000000           ; 32 MiB, the single argument
call [rax+0x40]                ; slot 8
test eax, eax                  ; jns, else int3
```

The id at `0x1462FC7D8` is 16 bytes, `11 9D 95 62 5F D4 84 48 A0 A3 F7 86 2E A2 43 AE`, read as Data1 little-endian
`u32`, Data2/Data3 little-endian `u16`, then the eight Data4 bytes:

**`{62959D11-D45F-4884-A0A3-F7862EA243AE}`**

Two things follow.

- **Slot 8.** `0x40 / 8 = 8`. `IUnknown` takes 0–2 and PC `dstorage.h` continues `CreateQueue` 3, `OpenFile` 4,
  `CreateStatusArray` 5, `SetDebugFlags` 6, `SetStagingBufferSize` 7, so `SetStagingBufferSize` at 8 confirms one
  console-only method (`SetCpuAffinity`) sits alongside the shared `SetDebugFlags` at slots 6 and 7 before it, matching
  `runtime/shims/dstorage_xs_abi.h` (`SetStagingBufferSize` at slot 8). Both `SetDebugFlags(UINT32)` (`void`) and
  `SetStagingBufferSize(UINT32)` (`HRESULT`) take one 32-bit argument; the `0x02000000` argument (32 MiB, the largest
  staging size the shim accepts) and the `HRESULT` sign check (`test eax, eax` / `jns`) after `call [rax+0x40]` identify
  `SetStagingBufferSize`. The call happens once, on first use, and the object is created before any queue or file exists,
  which is the state the shim's `E_DSTORAGE_STAGING_BUFFER_LOCKED` rule expects.
- **The id.** It is the console factory's, not a PC one (it is in none of `kPcInterfaceIds`), so a policy that learns the
  id at boot would log it on the first call and serve the factory under the PC-open descriptor it guesses from. The shim
  should accept it as the factory's declared id instead: `QueryInterface` of the factory for it succeeds, and the other
  kinds refuse it. The queue, file and status-array ids are still unmeasured.

What this does not show: which of `SetCpuAffinity` and `SetDebugFlags` sits at 6 vs 7, which of the factory's other slots
the title reads (the object is one global, so it reads some), and whether `SetStagingBufferSize` needs anything of the
shim beyond the size check it already does. The last one is behaviour the first run will show.

## What the shim does

- **Factory**: one object for the process, never freed (its reference count does not drop below one).
  `SetDebugFlags` and `SetCpuAffinity` are stored only (a console core mask means nothing on the host).
  `SetStagingBufferSize` accepts 0, 16, 20, 24, 28 and 32 MiB (the title asks for 32 MiB once, at slot 8), fails with
  `E_DSTORAGE_INVALID_STAGING_BUFFER_SIZE` otherwise and with `E_DSTORAGE_STAGING_BUFFER_LOCKED` while a queue or file
  exists. This records the console setting. Host transform scratch is allocated on demand.
  `GetStats` reports the counts the shim keeps (queues per priority, requests, bytes, files, errors, zlib
  inflates: `TotalDecompressionRequestCount` grows when a zlib request's inflate starts, which is after its checks
  passed and its source was read, `PendingDecompressionRequestCount` is the inflates running, and
  `DecompressionErrors` the streams that failed). As
  `DSTORAGE_PRIORITY_STATS` defines them, `TotalRequestCount` and the byte totals grow when a request is submitted
  (not when it is enqueued), and `PendingRequestCount` counts the requests the worker is processing; status and fence
  entries are counted when they are enqueued. A bad
  argument (a null pointer, an invalid queue desc or staging size) is logged once per method and kind, so a wrong
  vtable guess shows up in the log as arguments that make no sense for the method.
- **Files**: `OpenFile` goes through the registry's `kernel32.dll!CreateFileW`, the shim in `file_system.cpp`, so a
  `GAME:` path reads the overlay copy first, then the package, and a path that climbs out is refused (a host path is
  passed through). Read access, `FILE_SHARE_READ`, as the overview says; a failed open returns the Win32 error as an
  HRESULT. `GetHandle` returns a handle of its own (`ReOpenFile`, read access, `FILE_SHARE_READ`) that the caller
  closes; it has its own file position, which the shim's reads do not move. `Close` waits for the submitted requests
  that read the file; a request processed after it fails with `E_DSTORAGE_FILE_NOT_OPEN`. A request holds a reference
  on its file, so the last `Release` cannot close a file a request still reads.
- **Queues**: capacity `0x80`..`0x2000`, priority `LOW`..`REALTIME`, a memory-sourced queue must be `REALTIME`
  (`E_DSTORAGE_INVALID_QUEUE_CAPACITY`, `_INVALID_QUEUE_PRIORITY`, `_INVALID_MEMORY_QUEUE_PRIORITY`). A queue
  holds `capacity - 1` entries; `Query` reports the empty slots and the entries left before the automatic submit,
  which happens when the entries enqueued since the last submit reach half the capacity (unless
  `DSTORAGE_QUEUE_OPTIONS::AutoSubmitDisabled`). An enqueue into a full queue waits for space, as the reference
  says; with auto-submit disabled only a `Submit` from another thread frees it. One worker thread per queue runs
  the submitted entries in order, so every notification follows all the entries before it.
- **Reads into physical pages wait for `Submit`**: an automatic submit stops at the first request whose
  destination is a physical page list; that request and every entry behind it wait for the title's own `Submit`.
  A title may fill the page-number table such a list points into only in its submit path, so an early read finds
  page 0 (the entry fails) or the number the slot held before (the data lands in a page that now belongs to
  something else, with no error). A full queue submits the held entries too, since the title cannot call `Submit`
  while it waits in an enqueue, and so does `Close`. The idea is from VIctory (`Marvisak/VIctory` `eddde73`, also
  in `fxceless/victory-fork` `79cfdd9`); the queue here keeps its order, so entries behind a held read wait with
  it. `LEONIDA_DSTORAGE_HOLD_PAGES=0` restores the automatic submit of everything. A hold ends after
  `LEONIDA_DSTORAGE_HOLD_MS` milliseconds without a `Submit` (2000 unless set): the held entries are submitted with
  their page lists as they are, as a timed release, so a path that never calls `Submit` cannot stall for good. `0`
  is no time limit (a hold of five seconds is then logged once). The `[rt] ds-hold` line counts automatic submits,
  reads held, full-queue and timed releases, and destination lists that named page 0 when written (`event=`, at
  each power of two of the counter that moved).
- **Requests**: copied at enqueue. A file request reads `SourceSize` bytes at `FileOffset` into `Destination`; a
  memory request copies `Source` to `Destination` (`memmove`, so an overlapping pair is safe); a zlib request
  inflates those `SourceSize` bytes into `Destination` (below). Checked in this order when the worker reaches the
  request:
  1. cancelled: a success, whatever else is wrong with it;
  2. found at enqueue: a null request pointer (`E_INVALIDARG`) or a file object the shim did not make
     (`E_DSTORAGE_INVALID_FILE_HANDLE`);
  3. reserved option bits (`E_DSTORAGE_RESERVED_FIELDS`), then the source type against the queue's
     (`E_DSTORAGE_INVALID_SOURCE_TYPE`);
  4. a BCPack mode above 7 or swizzle mode above 4. BC2 is reserved here for a future decoder (`E_NOTIMPL`);
  5. a nonzero `IntermediateSize` is required only for combined zlib and BCPack. Compressed
     requests require a larger output than source, complete BC blocks and complete swizzle groups.
     Raw requests require equal source and destination sizes. File offsets used for decompression
     must be 16-byte aligned. The total memory bound is 1 GiB, with ordinary buffers limited to
     32 MiB apiece. Page lists can exceed the 32 MiB bound;
  6. a null destination, a null source (memory) or a null file (`E_INVALIDARG`), a closed file (`_FILE_NOT_OPEN`);
  7. a zero size: a success that touches no memory (an assumption: the reference does not say);
  8. a range past the end of the file (`E_DSTORAGE_END_OF_FILE`), then a destination or source the process cannot
     access (`E_DSTORAGE_ACCESS_VIOLATION`; the worker survives it). This needs asynchronous exceptions under
     clang-cl, so `runtime/CMakeLists.txt` builds `dstorage.cpp` with `/EHa` there; MSVC always catches it.

  Step 7 does not apply to a zlib request (its `DestinationSize` is never 0 after step 5); its stream fails as
  described next.
- **zlib** (`ZlibDecompress` alone): the source, `SourceSize` bytes of the file or of memory, is read into a staging
  buffer and inflated there by the runtime's RFC 1950 inflater (`leonida::zlib_inflate` in
  `runtime/src/shader_library.h`, the one the shader library uses for XBCH blobs). The result is copied to
  `Destination` only when it is exactly `DestinationSize` bytes, so a stream that fails leaves the destination as it
  was, and a memory source inside its own destination (in-place loading) inflates correctly. For ordinary destinations, step 5 also means a stream of stored blocks only, which is larger than what it holds, is
  refused with `_INVALID_DESTINATION_SIZE` before it is read, as the documented rule implies. The stream is a zlib
  stream: the two header bytes, the deflate blocks (stored, fixed and
  dynamic Huffman), the Adler-32 of the result. Bytes after the Adler-32 are not read, so a `SourceSize` rounded up
  over padding works (an assumption: the reference does not say). The game's own data agrees with RFC 1950: the
  texture pools' codec-8 chunks, laid out for this decompression ([textures.md](textures.md)), inflate with
  Python's `zlib.decompress`, which checks the header and the Adler-32 (`tools/ttpf_mips.py`). A failure:

  | Stream | HRESULT |
  |---|---|
  | header not deflate, a window over 32 KiB, a wrong check value, or a preset dictionary | `E_DSTORAGE_ZLIB_BAD_HEADER` (`0x89240031`) |
  | a reserved block type, an invalid code, a distance before the start, or a truncated stream | `E_DSTORAGE_ZLIB_BAD_DATA` (`0x89240032`) |
  | Adler-32 mismatch | `E_DSTORAGE_ZLIB_PARITY_FAIL` (`0x89240033`) |
  | inflates to more or fewer bytes than `DestinationSize` | `E_DSTORAGE_DECOMPRESSION_ERROR` (`0x89240030`; no code names a size mismatch, so the generic one) |

  The inflater decodes one bit at a time, on the queue's worker: correct, not fast. A faster decoder (table lookup)
  can replace it behind `zlib_inflate` without touching the request path.
- **Cancel**: `CancelRequestsWithTag(mask, value)` marks every request enqueued before the call and not yet started
  whose `(CancellationTag & mask) == value`. A cancelled request completes as a success without reading and is not
  counted as a failure, as the overview and the reference say, even when it also had an error found at enqueue.
- **Notifications**: `EnqueueStatus` sets the entry to `E_PENDING` at once and, when reached, to the first failure
  since the previous status or signal (else `S_OK`); `IsComplete` is `GetHResult != E_PENDING`; an index past the
  array is `E_DSTORAGE_INDEX_BOUND`, a foreign or null array `E_DSTORAGE_INVALID_STATUS_ARRAY`. `EnqueueSignal` calls
  `ID3D12Fence::Signal` (PC slot 10, which the `d3d12_xs` fence proxies forward) on the worker thread; a null fence is
  `E_DSTORAGE_INVALID_FENCE`. `EnqueueSetEvent` duplicates the handle at once and sets the duplicate, then closes it,
  so an event the title closes in the meantime (and a handle value reused for another object) is never the one set; a
  null or invalid handle is recorded as a failure. Each notification holds a reference on its array or fence until it
  has run.
- **Errors**: a failure goes into the status entry, the queue's error record (failure count and the first failure
  since the last `RetrieveErrorRecord`, with the file name the title opened) and sets the error event.
  `GetErrorEvent` returns a duplicate handle (the reference says the caller closes it). `RetrieveErrorRecord`
  clears the record and resets the event; with nothing recorded it returns `DSTORAGE_COMMAND_TYPE_NONE`. A failed
  `SetEvent` is recorded with command type 3 (the PC API's `EVENT`; the console enum stops at 2).
- **Close and release**: `Close` submits what is enqueued, waits until the worker has run it all and stops the
  worker; later enqueues are dropped and recorded as `E_DSTORAGE_QUEUE_CLOSED`. The last `Release` closes the same
  way, so a queue released with requests in flight finishes them first.

## Streamed texture requests

- Two adjacent file-backed transforms can read and decode concurrently into private buffers. A queue has
  one ordered publisher. Mutable memory inputs, page-list resolution, status slots, events, fences, and
  overlapping destinations retain their enqueue order. At most two helpers run across all queues and each
  assisted request has a 16 MiB eligibility limit. A shared 64 MiB window covers private source,
  intermediate, output and page-number buffers across queues. Each transform releases its previous input
  and each request releases the final buffer at retirement. A valid request larger than the window runs
  alone, bounded by the ABI maximum of two 1 GiB payloads. Raw buffer reads use no private staging.
  With tracing enabled, `ds-staging` reports the process peak reservation and return to zero. The figure
  excludes title destinations, allocator overhead and fixed decoder stacks. `LEONIDA_DSTORAGE_WORKERS=1`
  restores serial execution.
  Inflate expands LZ matches in bulk and batches Adler-32 reductions. `LEONIDA_DSTORAGE_FAST_INFLATE=0`
  selects the original bytewise path. BCPack indexes the entropy model by the high slot byte.
  `LEONIDA_DSTORAGE_FAST_BCPACK=0` selects the scalar bucket search.
- With `LEONIDA_DSTORAGE_TRACE=1`, `tools/dstorage_census.py <trace>` totals retirements, errors and bytes
  by shape, plus microseconds spent reading, inflating, decoding BC blocks, unshuffling, publishing and
  notifying. `--from-file story000` starts the measurement at the first story archive request and counts
  every subsequent retirement across queues. Stage times can overlap. Each failure reports its process-wide count
  for that HRESULT even with request tracing disabled.
- The worker inflates zlib to `IntermediateSize` when BCPack follows, rebuilds standard BC blocks, then
  applies the optional DirectStorage macro-unshuffle. Each stage validates its entire output before a destination
  write. Typed zlib errors propagate through the combined path.
- `runtime/src/bcpack.cpp` ports the interleaved rANS and strict block rebuilders from `tools/bcpack.py` for
  BC1, BC3, BC4, BC5, BC6H and BC7. The predictor window holds 256 blocks. The build generates the lookup
  tables from the existing codec modules. Damaged streams and missing predictors or inconsistent blocks
  exceeding the tolerance give `E_DSTORAGE_DECOMPRESSION_ERROR`. BC2 has no reference decoder.
  In fitted mode with a profile, a BC6H/BC7 request may have up to `max(2, blocks / 32)` **lost** blocks:
  blocks whose endpoint or index predictor chain starts at a history entry the profile does not know. They
  are rebuilt from the bits the profile has, so their colours can be wrong, and each such request is logged
  (`[rt] dstorage: BC7 request approximated N of M blocks ...`; the first eight, then at powers of two).
  `LEONIDA_DSTORAGE_LOST_DIVISOR=N` sets the divisor; 1024 is the reference decoder's limit
  (`_loose_limit` in `tools/bcpack.py`), which refused such requests outright and left the whole destination
  untouched. On the 1.0.2.1 package two BC7 requests in `data/ttdb/props/octa_imposters.ttpf` failed at every
  boot this way (one unknown endpoint entry each, 5 of 1,536 and 15 of 768 blocks lost with a locally built
  profile); with the default they are published. Inconsistent blocks (anchor or p-bit) keep the 1-in-1,024
  limit, and strict mode (`LEONIDA_DSTORAGE_FITTED=0`) refuses any lost block as before.
  The runtime accepts the reference's fitted BC1/3/4/5 predictors for streamed art.
  `LEONIDA_DSTORAGE_FITTED=0` requires confirmed entries. Fitted blocks can differ from console output.
  The runtime stage of `make_game.py` generates `<work>/shaders/bcpack/history.bcph` locally.
  The loader finds it relative to the shader folder selected by `LEONIDA_SHADERS` or the packaged
  `shader_path.txt`. `LEONIDA_DSTORAGE_HISTORY` overrides the path; a value of `0` disables external history.
  The load diagnostic reports BC6H row coverage and BC7 endpoint/index field coverage. Standalone generation:

  ```sh
  python3 -I tools/build_dstorage_history.py <game-root> --out <work>/shaders/bcpack/history.bcph
  ```

  The fitter follows the physical block offset of packed 2D mips, including those that start past the first
  256 blocks of a stream. It combines adjacent mips from the same layer with full-stream index anchor
  and shared-p-bit constraints. A shared p-bit can relate low red endpoint bits even when its sibling
  mip supplies only the leading bits. These local fits preserve the recorded precision and exact reference
  bits. A packed tail whose larger mips are BCPack-coded too has no raw or zlib sibling, so a history field that
  only such tails use stayed unknown: in the game runs of 2026-11-25 two BC7 tail streams of one pool failed
  with `missing-endpoint` (alpha fields of two history entries). For the streams that still start a chain at a
  missing field, the fitter now decodes the nearest BCPack sibling with the profile so far and fits against it,
  repeating while a round adds fields. The sibling may sit in the same refused BC7 stream: every block whose
  chains start elsewhere is still determined, and the undetermined ones are left out of the reference. Those
  fields are fitted, never confirmed, and fill only what the other fits left empty. On the 1.0.2.1 package this
  raised the BC7 endpoint fields from 4,228 to 4,406 of 6,144; the runtime decoder then refuses 8 of 5,167 BC7
  streams instead of 60 (both failing requests decode), and the 268 BC6H streams are unchanged (10 refused; a
  refused BC6H sibling is not used as a reference yet). A profile can still lack endpoint values or exceed the reference consistency tolerance. Such a request reports a decompression error before publication.
- The four swizzle modes interleave stripes from request-wide component planes into 16 KiB (mode 1) or
  32 KiB (modes 2–4) groups. The enum names give the successive stripe widths. The shipped BC4 sample
  separates its endpoint planes from indices across the inflated source. The title's compute shader does
  the within-group shuffle and the stored blocks already follow the texture layout.
  [Microsoft texture shuffling sample](https://learn.microsoft.com/en-us/samples/microsoft/xbox-gdk-samples/directstoragetextureshuffling/)
- Physical destinations and memory sources use 64 KiB pages with byte offsets in the first page.
  The worker reads the page list after submit, resolves and commits every page before copying, and holds
  xmem's map lock through the transfer. A successful scatter notifies the texture refill tracker at
  each mapped VA. Ordinary GPU alias destinations use the paired CPU view. Raw file writes into title
  memory go through the shared kernel-write helper, which pins each kernel write, commits first-touch pages
  on an access error and retries once. The superseded `LEONIDA_DSTORAGE_PIN_FILE_READS` switch is retired.
- `LEONIDA_DSTORAGE_EXTENDED=0` restores the old refusal for page lists, BCPack and nonzero swizzle.
  `LEONIDA_DSTORAGE_TRACE=1` logs each retired request, worker timestamp, options, source file/range, sizes, destination,
  cancellation and HRESULT. Queue pointers connect those lines to status slots, fences and events.
  Failures always log the full request. An access violation also records the CPU mapping and protection.
  With startup tracing enabled, `dstorage_failure` events share its clock with title exceptions.
  Pool fill lines report whether a successful texture upload used any nonzero detiled bytes.

## Tests

`runtime/test/dstorage_test.cpp` (ctest `dstorage`) runs on synthetic files in `%TEMP%` through the registry entries
under all three module names: `GAME:` reads with an overlay copy winning, offsets, zero size, a range past the end,
cancellation, auto-submit, reads into physical pages held for `Submit` (a page list changed or filled before it,
a full queue, the time limit, the switch), a full queue, status arrays, fences (a CPU fence object that checks the
data is in place when it is signalled), events, error records, file and queue close with requests in flight, a destination the
process cannot write, four threads enqueueing at once, the interface-id policy (unknown and system ids refused by
`QueryInterface`, learned by creating calls), a `GetHandle` handle with its own file position, an event closed (and
its handle value reused) before it is set, and cancelled requests that also had an enqueue error. zlib requests use
synthetic streams only: two made with Python's `zlib` (level 9: one dynamic-Huffman block; level 1: one
fixed-Huffman block) from text the test rebuilds, and stored-block streams the test builds, one of them a stored
block followed by the dynamic one. They are read from a file and from memory (also in place); a bad header, a
preset dictionary, a reserved block type, a truncated stream, an Adler-32 mismatch, a stream shorter and one longer
than `DestinationSize`, a stored stream larger than its result, an unaligned offset, an invalid `IntermediateSize`, an unreadable source and an unwritable destination each get their code, a failed
stream leaves its destination untouched, and the decompression statistics count them.

The extended tests encode hand-built lane symbols with `tests/test_bcpack.py`. All six modes are compared
byte-for-byte with the Python decoder through the queue, both bare and zlib-wrapped. Additional cases cover
truncation, all four swizzle patterns across three groups, an invalid second page, late page-list updates,
a decode crossing two reversed pages, both CPU refill ranges, gather, and event/status ordering.

## Sources and licences

| Source | Facts used | Licence / use |
|---|---|---|
| [GDK DStorage reference](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/dstorage_members) and its function, interface, structure and enumeration pages | Names, method signatures, structure members, enum values, requirements rows | [Microsoft Learn Terms of Use](https://learn.microsoft.com/en-us/legal/termsofuse); facts paraphrased, no text copied |
| [DirectStorage overview (GDK)](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/console/storage/directstorage/directstorage-overview) | Queue, auto-submit, cancellation, notification order, file sharing, decode order | Same terms; paraphrased |
| `dstorage.h`, `dstorageerr.h` in the public `Microsoft.Direct3D.DirectStorage` 1.3.0 NuGet package | PC method order, interface ids (to refuse them), capacity and name-length constants, `E_DSTORAGE_*` values | Read for facts only; not vendored. `runtime/shims/dstorage_xs_abi.h` is Leonida's own declaration |
| [DirectStorage constants](https://learn.microsoft.com/en-us/windows/win32/dstorage/dstorage-constants) (Win32 reference) | `E_DSTORAGE_DECOMPRESSION_ERROR` = `0x89240030`; `E_DSTORAGE_INVALID_DESTINATION_SIZE`: equal to the length without decompression, larger with it; `E_DSTORAGE_INVALID_FILE_OFFSET` is the offset invalid for a decompression mode | [Microsoft Learn Terms of Use](https://learn.microsoft.com/en-us/legal/termsofuse); paraphrased |
| [`direct-storage` crate](https://docs.rs/direct-storage/0.5.1/src/direct_storage/bindings.rs.html) 0.5.1 (bindings generated by `windows-bindgen` from the DirectStorage headers; 0.2.0's `errors.rs` has the same values) | `E_DSTORAGE_ZLIB_BAD_HEADER` `0x89240031`, `E_DSTORAGE_ZLIB_BAD_DATA` `0x89240032`, `E_DSTORAGE_ZLIB_PARITY_FAIL` `0x89240033` (names and values only) | MIT/Apache-2.0; two constants' names and values, no code copied |
| [RFC 1950](https://www.rfc-editor.org/rfc/rfc1950) and [RFC 1951](https://www.rfc-editor.org/rfc/rfc1951) | The zlib stream and DEFLATE formats `zlib_inflate` decodes | Public specifications |
| `game_scarlett_master.exe` (RT-5, supplied separately, not vendored) | The single `DStorageGetFactory` import, thunk, call site, the id it passes and the slot-8 call with 32 MiB; measured with `tools/import_call_sites.py` and Capstone 5.0.7 | Facts only, no bytes committed |
