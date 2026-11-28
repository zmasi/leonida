# First boot: the runbook and the two traces

The Windows integration has a dedicated [offline first-run UI profile](first_run_ui.md). The original dry-run
and initialization measurements used a separately supplied executable and identified 30 unimplemented imports.
The Winsock shim restricts connections to loopback and the WinHTTP shim refuses outbound requests. Keep retail runs
offline and traces on the player's machine. [Original import measurements](runtime_imports.md#the-retail-executable-rt-5)

## 1. What the first boot produces

`gta6rt` has three passes over the executable (`runtime/README.md`, "Dry run" and "Current limits"):

| Pass | What it does | What a clean result shows |
|---|---|---|
| `--dry-run` | maps the image, binds its imports, applies TLS | every module in `map`, no exception |
| `--init` | runs TLS callbacks and `DllMain(PROCESS_ATTACH)` | `tls_callback` and `dllmain` events, no exception |
| `--run` | calls the entry point on the game thread and returns its exit code | `exit` (or a deliberate `error`) |

Two traces are written independently:

* **startup trace** (`--trace FILE`): one JSON object per line, flushed per event, so it survives a crash. Kinds
  and fields are in [`runtime/README.md`](../runtime/README.md) ("Startup trace").
* **D3D12 trace** (`--trace-d3d12`): one `[d3d12-trace] #<seq> <method> <outcome>` line per call, on stderr. The
  ordinary `[rt]` diagnostics stay separate. A present is one `XboxPresentation::PresentX` line (`ok` or `failed`),
  so the presents of a run are a count of those lines.

`tools/boot_trace.py` reads both and prints the triage units: modules, unimplemented calls and callers,
DllMain/TLS results, exceptions with their stack frames, the exit, and the D3D12 calls by outcome and first call
per method. When `XboxPresentation` logs its teardown `[rt] present: pacing summary ...` line (BL-6), the D3D12
summary also reports the frame count, interval count, target interval, mean and drift against target, `p50`/`p95`/`p99`
frame times, and `p50`/`p95`/`p99` origin-to-present latencies (and includes the numeric `frame_pacing` object in
`--json` and `--counts-only`). The remaining `[rt]` lines in the same stderr log carry the device parameters, feature
ids and warnings; the summary leaves them as a count, so grep `^\[rt\]` for those.

For `xmem_physical_pages`, the shim records the first request for each distinct attribute word. The text and
JSON summaries list the API, raw attribute word, requested page count and caller for the first `--limit`
records in trace order. Use `--limit 0` to keep the total with no records. Text shows how many records were
omitted, and JSON keeps the full count in `startup.kinds.xmem_physical_pages`.

## 2. Commands

Windows (from the repository root):

```powershell
$env:LEONIDA_D3D12 = 'vulkan'
$exe = "C:\path\to\game_scarlett_master.exe"
$pkg = "C:\path\to\the\package"
$trace = Join-Path $env:TEMP boot.jsonl
$log = Join-Path $env:TEMP d3d12.log
& { $ErrorActionPreference = 'Continue'
    .\runtime\build\gta6rt.exe --run $exe --game-dir $pkg --trace $trace --trace-d3d12 2>&1 } |
    ForEach-Object { $_.ToString() } |
    Out-File -LiteralPath $log -Width 4096 -Encoding utf8
python -I tools\boot_trace.py $trace --d3d12 $log
```

The capture pipeline writes each message as text to avoid PowerShell 5.1 wrapping the first stderr record at a
narrow output width. The tool also reads existing UTF-16LE logs saved with `2>` in PowerShell 5.1 and recognizes
the `program.exe : ` prefix on the first stderr line.

The `& { ... }` block matters when the session (or a profile or script) has `$ErrorActionPreference = 'Stop'`:
Windows PowerShell 5.1 then turns the first stderr line into a terminating error, the run stops there, and the log
holds no `[d3d12-trace]` line, so it reads as a run with no D3D12 call. The block sets `Continue` for the run only
(`scripts/first_look.ps1` captures its dry run the same way). Measured on 5.1.26100 with a stand-in that prints three
trace lines to stderr and exits 37: without the block, all three with `Continue` and none with `Stop`; with it, all
three with either, `$LASTEXITCODE` 37, and the session's preference unchanged.

Linux under Wine (a cross build made with xwin, or the `runtime-windows` artifact; the backend must be Vulkan):

```sh
export LEONIDA_D3D12=vulkan
wine runtime/build/windows/gta6rt.exe --run /path/to/game_scarlett_master.exe \
     --game-dir "Z:\\path\\to\\the\\package" \
     --trace "Z:\\tmp\\boot.jsonl" --trace-d3d12 2> /tmp/d3d12.log
python3 -I tools/boot_trace.py /tmp/boot.jsonl --d3d12 /tmp/d3d12.log
```

`runtime\build\gta6rt.exe` is where `scripts\build_runtime.ps1` (Ninja) puts it; the direct CMake path in
[runtime/README.md](../runtime/README.md) uses the multi-config Visual Studio generator, which writes
`runtime\build\RelWithDebInfo\gta6rt.exe` instead.

`--trace FILE` **refuses an existing file** (exit 2): it is created before the loader reads anything, so use a
fresh name or delete the old one. `LEONIDA_D3D12=vulkan` selects the Vulkan-backed device, the only backend that
traces command lists, resources and pipelines; without it the layer uses the Windows D3D12 device, and the
`[d3d12-trace]` lines are only what the Xbox-ABI proxy forwards (device creation, queue, fence and allocator
calls), which `boot_trace.py` summarizes without complaint. A run on the Vulkan device prints
`[rt] d3d12: Vulkan device: <name>`; check the log for it (a runtime built without the Vulkan headers accepts the
variable and stays on the Windows device). Keep the traces out of the repository: they can quote whatever the game prints.

Useful switches:

* `LEONIDA_UNIMPLEMENTED=log` (default) logs every unimplemented import once and returns 0; `fail` returns
  `E_NOTIMPL` for the `HRESULT` imports; `stop` ends the process with exit code 3 on the first such call, after
  printing the caller's `module+RVA` and the argument registers (with a debugger attached it breaks instead). Start
  with `log`, then use `stop` to pin the first blocker and `fail` to see how the game's own failure path reacts.
  This switch covers imports only; it does not affect the D3D12 outcomes.
* `LEONIDA_SKIP_CPU_CHECK=1` skips the AVX preflight of `--init` and `--run`, which otherwise exits 4 with one line
  naming the missing feature on a host without AVX (a QEMU `qemu64` guest; under Rosetta, set
  `ROSETTA_ADVERTISE_AVX=1` instead). The game itself still needs AVX, so this is for the #UD emulation work (see
  [cpu_features.md](cpu_features.md#hosts-without-avx) for what works on a host without AVX).
* `LEONIDA_SKIP_INT3=1` (with `--trace`) steps over each int3 in guest code and logs it once per address as an
  `int3_skipped` event, so a boot can be followed past soft asserts (an int3 followed by code that carries on). It is
  read when the trace opens.
  Diagnostic only: a correct run reaches none, so each skipped address is a finding, not a fix. A failed guest file
  open is always traced as `file_open_failed` (path, error, caller) when `--trace` is on.
* `LEONIDA_STUB_PIPELINES=1` creates a pipeline whose translated shader is missing (no shader library, or no entry
  for its program) as a stub instead of failing `CreateGraphicsPipelineState` / `CreateComputePipelineState`.
  A graphics pipeline with a vertex stage (MVP-GPU-04) is a real Vulkan pipeline built from the built-in stages in
  `runtime/src/stub_pipeline.h` and registered like any other, so its draws run through replay (render targets,
  layouts, push constants) and the validation layer. Its vertex stage puts every vertex outside the clip volume, so
  the draw changes no pixel. `LEONIDA_STUB_PIPELINES=color` makes every three vertices a triangle over the viewport
  and writes magenta (1 to an integer target) to each render target, so the passes that are stubbed show on screen.
  The cover needs a triangle topology (a point or line draw marks only its points or lines) and stays inside the
  scissor rectangle; the title's culling, wireframe fill, sample mask, depth and stencil tests, blending and logic op
  are off for it, and its write mask is kept. A compute or mesh stub is never registered: `SetPipelineState` binds no
  executable pipeline and its dispatches and mesh draws are not recorded (D3D-482). A mesh draw with another device's
  stub, and a draw with another implementation's PSO, are still recording errors. Diagnostic only, like `LEONIDA_SKIP_INT3`: it follows a boot past every pipeline the title
  creates without translating or running a game shader. Each stub is logged with a power-of-two count.
  `d3d12_stub_pipelines_test` and `d3d12_stub_draw_test` cover it.
* During `--run` on Vulkan, the runtime paints the package shell splash when the title shows its window. The first
  direct queue presents it on the window's swapchain. Startup clears keep the splash visible until the first
  non-black output replaces it. The runtime reads `ShellVisuals/SplashScreenImage` from
  `MicrosoftGame.Config` or `SplashScreen/Image` from `appxmanifest.xml`, preserves the image's aspect ratio and
  uses the same swapchain for PresentX. A surface without readback hands off at the first title present.
  Set `LEONIDA_PACKAGE_SPLASH=0` to disable the splash.
* `LEONIDA_VK_VALIDATION=1` enables the Khronos validation layer with the Vulkan backend (startup overhead; it is a
  diagnostic).
* `LEONIDA_DEVICE_FAULT=1` enables Vulkan device-loss diagnostics and checkpoints where available. The first
  observed loss prints a `[rt] device-fault:` report to stderr.
* `LEONIDA_TITLE_HUD=1` shows live counters in the title bar of the window the title presents in
  (`runtime/shims/title_hud.h`). It updates twice a second with:
  - the frame rate and the average and worst frame time from PresentX, or, when no frame came in that half
    second, how long since the last one (a hang reads "0 fps, no frame for 12.5 s");
  - the process's private memory (its commit charge; Wine fills only `PagefileUsage`, which is the same figure)
    and working set;
  - its GPU memory: in use and the budget over the device-local heaps, and in use in the other heaps, read from
    the Vulkan device through `VK_EXT_memory_budget`. Without that extension it falls back to DXGI's
    `QueryVideoMemoryInfo`, which reads 0 under Wine; with neither the field reads `VRAM n/a`, not a false 0.
    Both memory figures and their text come from `runtime/src/memory_figures.h`, which the dev overlay shows too;
  - the graphics and compute pipelines created, and the seconds spent in `vkCreate*Pipelines` in the last half
    second. A pipeline compiled in the frame shows up there as a hitch. This needs `LEONIDA_MEM_PROBE`; without it
    the field reads `PSO -`.
  - the time since the first present.

  A thread of its own sets the text with `SendMessageTimeout`, so a busy window thread never holds a present.
* `LEONIDA_DEV_OVERLAY=1` (Windows) shows what the runtime is doing in a small click-through window over the top left
  corner of the title's window, while that window is in front (`runtime/src/dev_overlay.h`): frames and submissions a
  second, where a submission's CPU time goes, the phase that is running now and for how long, pipeline builds in
  flight, indirect draws emitted and skipped, cores, free RAM, and the same private memory and GPU memory figures as
  `LEONIDA_TITLE_HUD` (`VRAM n/a` without `VK_EXT_memory_budget`). The device publishes its GPU memory figure, and
  the overlay and the HUD read it at most once per half second. The rates cover the last two seconds. It has its own
  thread, so it also counts while the title presents nothing, and it is in no capture. `LEONIDA_TITLE_HUD` shows frame
  times and compile times in the title bar instead; both can be on.
* `DISABLE_RTSS_LAYER=1` and `DISABLE_VULKAN_OBS_CAPTURE=1` when MSI Afterburner or OBS is installed; their
  implicit layers add swapchain usage that shows up as validation errors ([testing.md](testing.md), "Known
  problems").
* `--overlay DIR` sends the guest's writes to DIR instead of the default `<game-dir>\overlay` (useful when the
  package drive is read-only); the package itself is never written. Set `LEONIDA_WRITE_DIR` to override that folder
  for one run (it wins over `--overlay`). For a fresh first-run profile, also set `LEONIDA_SAVE_DIR` to a separate
  empty directory for the local game-save provider. The title's `T:` drive (its temporary drive,
  where it writes its crash dump `T:\<guid>.dmp` and crash log) is a folder of each process's own inside the overlay,
  `<overlay>\.gta6rt-temp\<YYYYMMDD-HHMMSS-mmm>-pid<id>` (the process's start time in UTC and its id).
  The folder is reserved with a create-only `CreateDirectoryW` when `gta6rt` starts the title (or on the first `T:`
  call): when that name already exists, as after a reused process id in the same millisecond, it takes `-2`, `-3`,
  ... (up to 1000 names), so a run never writes into a folder that existed before it. The startup line names it
  (`console paths: GAME: -> ..., writes -> ..., T: -> ...`).
  Folders of earlier runs are kept, so a run's crash files can be read after it stops; delete them by hand. With
  one folder for every run, a `CREATE_NEW` of a name an earlier run had left failed with `ERROR_FILE_EXISTS` (80)
  and no report landed (old upstream issue 7913).

### Under Wine on Linux

The prefix has to supply a `d3d12.dll` that exports `D3D12SerializeVersionedRootSignature`, because the executable
imports it. Wine's own `d3d12.dll` does not answer that export for the runtime's shim, and the failure is quiet:
the D3D12 trace ends at about 1,658 calls, the startup trace ends in an access violation inside the executable,
and the one line that names the cause is
`[rt] d3d12_xs: host d3d12.dll has no D3D12SerializeVersionedRootSignature`. The run never reaches
`CreateRootSignature`.

Copying the DLLs a Proton prefix installs is not enough on its own: Wine keeps preferring its own copy next to
them. Name them in the overrides.

```bash
GE=$HOME/GE-Proton11-13-x86_64/files/lib/wine
cp "$GE/vkd3d-proton/x86_64-windows/d3d12.dll" \
   "$GE/vkd3d-proton/x86_64-windows/d3d12core.dll" \
   "$GE/dxvk/x86_64-windows/dxgi.dll" "$WINEPREFIX/drive_c/windows/system32/"
export WINEDLLOVERRIDES="d3d12,d3d12core,dxgi=n,b"
export LEONIDA_D3D12=vulkan
export LEONIDA_SHADERS='Z:\path\to\shaders\spirv'
wine ./gta6rt.exe --run 'Z:\path\to\game_scarlett_master.exe' --game-dir 'Z:\path\to\the\package' \
     --trace /tmp/boot.jsonl --trace-d3d12
```

With those three DLLs and the override, the same prefix reaches the frame loop: about 1.4 million D3D12 calls in
90 s, no exception, `PresentX ready`, against 1,658 calls and an access violation without them, measured on Wine
11.13 (GE-Proton 11-13) with an RDNA2 iGPU. A prefix that Proton itself created already carries these files and
needs no override.

## 3. Triage order

Read the startup trace first; the first event that is not the expected one is the run's first fact.

1. **`map`** — is the executable at its preferred base (`relocated: false`), and are the expected package DLLs
   mapped? A missing module is an import-table or search-path problem.
2. **`unimplemented_call`** — the first `dll!name` and its `caller` (module+RVA). Add a shim, a host binding or a
   deliberate failure in `runtime/shims/registry.cpp`; rerun with `LEONIDA_UNIMPLEMENTED=stop` to halt at the first
   remaining blocker.
3. **`tls_callback` / `dllmain`** — a failed DllMain, or a TLS callback that faults, usually points at the CRT or
   TLS index. The summary shows the address and phase of the last observed TLS callback
   (`last_tls_callback` in JSON). The loader records it before invoking the callback, so the record gives no
   completion result. The loader's own TLS is checked by `run_tiny_game_tls`.
   An access violation in CRT startup before the first D3D12 call can be the TLS index instead: code built with
   `/GA` reads its thread-local data at module index 0 without `_tls_index`, and index 0 belongs to `gta6rt.exe`
   (stop 4 of [retail_boot_chain.md](retail_boot_chain.md)). `load()` rewrites those reads in a mapped executable
   so that they reach the image's own block, and every other mapped module with a TLS directory has an index of
   its own ([`runtime/README.md`](../runtime/README.md), "PE loader"). The stderr log has one
   `[rt] loader: <image>: <N> index-0 TLS reads` line for the executable; `N` 0 on an image that faults there
   means its reads have a shape the rewrite does not know. `LEONIDA_TLS_GA_PATCH=0` turns the rewrite off, to
   see the stop without it. `loader_tls` checks both ways with a synthetic `/GA` executable mapped beside a DLL
   that has TLS of its own, on the loading thread and on a new one, and `run_tiny_game_ga_tls` runs a `/GA`
   build of the stand-in game through `gta6rt --run`. Not confirmed here on the retail executable: that needs
   game files.
4. **`exception`** — code, `at`, access/address and the stack frames. The loader's vectored handler only records,
   so the process behaviour is the same with or without the trace. When the report reaches `--limit`, it reserves
   the final slot for the last observed exception. The trace also includes handled exceptions, so the last
   observation alone does not establish what caused the run to stop.
5. **`exit` / `error`** — a clean exit is not a correct frame; continue to the D3D12 trace.
6. **D3D12 trace** — the `failed`, `unsupported` and `refused` lines name the method. A `failed`
   `ExecuteCommandLists` submitted nothing from its lists. (`boot_trace.py` also lists a `partial` outcome, a
   submission that ran only the supported commands; the runtime does not write it yet.)
   Compare the method's position with the public SDK vtable. An unknown slot is expected until the executable names it
   (Q2, Q6). The `[rt]` lines in the same log carry the 24 device words, feature IDs and warnings.
   The summary expects `#1` first, then one more than the last observed number. It counts each mismatch once and
   shows up to `--limit` expected/observed pairs. A break can reflect an excerpt, restart, duplicate or reordered
   line; it is not a count of missing calls.
7. **First frame** — after the device and queues, the first `CreateRootSignature`, PSO, `Draw*` and `PresentX` (or
   DXGI) calls are the frame's critical path. The layer's M0–M4 tests already exercise the offscreen parts; the
   present slots are the part that cannot be measured from the package alone.

The trace tool prints the first call per method and the `unsupported`/`failed`/`refused`/`partial` lists. The `--json`
option gives the same summary for a script. Share only the summary's **counts and error classes**.

If startup stops before the first D3D12 call, the combined command still prints the startup report. A missing
or invalid requested trace is reported on stderr and sets exit code 2. In a partial JSON report that section
is `null`; if neither input yields a report, stdout is empty.

## 4. The open questions, and the trace signal for each

The questions are in [`d3d12_vulkan.md`](d3d12_vulkan.md) §8.2; this table takes the same 1–22 in
order. It maps each question to the event to look for; it does not answer them.

| # | Question | Signal in the first trace |
|---|---|---|
| 1 | `D3D12XboxCreateDevice` 24-word params and flags | first `D3D12XboxCreateDevice` in the D3D12 trace; the shim prints the 24 words as an `[rt]` line in the same log, with or without `--trace-d3d12` (grep `^\[rt\]`); accept unknown flags |
| 2 | Xbox device/heap/queue/command-list slots | queue, fence and allocator method names in the D3D12 trace; device slots past the PC vtable print an `[rt] d3d12_xs: device slot …` line instead and record nothing; compare with the SDK vtable |
| 3 | Root signature → `s0..s31` | `CreateRootSignature`, `SetGraphicsRootSignature`, then the shader's user-SGPR reads; mask `0x03000000` on a copy |
| 4 | Descriptor bytes and heap increments | first `CreateShaderResourceView`/`CreateSampler` and the first draw; dump the heap bytes and compare with the T#/S# parser |
| 5 | VA range and arena capacity | `GetGPUVirtualAddress` and every descriptor base; high-water mark |
| 6 | DXGI, `PresentX`, or both | first present/queue call; `PresentX` is a queue method past the public chain, so an unknown slot is the expected first finding |
| 7 | Standard AS build vs `XboxBvhBuilder` | first `BuildRaytracingAccelerationStructure`/`Copy...` vs slot-62 PSOs; see [embedded_shaders.md](embedded_shaders.md) |
| 8 | XDXR desc layouts | measured in [xdxr.md](xdxr.md); the live signal is a non-empty stream desc at slot 62 |
| 9 | Tiled textures / DCC | slot 76, `CopyTextureRegion`, resource descs; a `0x101` layout or tiled address |
| 10 | Binning, DCC, VRS, wave MMA, stream output | shader-side answered by `tools/feature_scan.py`; the trace adds PSO/stream subobjects |
| 11 | `CheckFeatureSupport` answers | the call's `ok`/`failed` and the `[rt]` feature-id line; the answer itself is not logged yet |
| 12 | Async compute / copy queues | queue creation and cross-queue fences |
| 13 | 144-byte push constants | device creation `[rt]` line; `push_constants_need_fallback` when the device reports fewer |
| 14 | Two code-base stages in one PSO | PSO creation log; the index metadata (`uses_code_base`) |
| 15 | `ExecuteIndirect` signatures | `CreateCommandSignature` and `ExecuteIndirect` calls |
| 16 | UAV counters and typed buffer formats | UAV descs and V# format logs; formats outside `Gen.FORMATS` read 0 |
| 17 | Upload/readback resources reaching shaders | `CreateCommittedResource` heap types and the VB/IB/descriptor that points at one |
| 18 | Placed/reserved/tiled resources | `CreatePlacedResource`, aliasing barriers, tile mappings |
| 19 | `0x03000000` root-signature bits | the root-signature blobs; the mask is a workaround, not the meaning |
| 20 | Which BVH builder programs the game uses | slot-62 PSOs on the first RT frame; map each to its `ILDN` container |
| 21 | PIX `Metadata` blob | metadata values; 0 and 1 are decoded silently, so an `unsupported` line means an unexpected value; if it is 2, decode the console blob from the executable's PIX calls, not the PC WinPixEventRuntime layout |
| 22 | `CheckFeatureSupport(OPTIONS5)` RaytracingTier | the `[rt]` feature id; the tier answer itself is not logged yet, and `DispatchRays` stays unimplemented |

## 5. What a good first run looks like

* `map` lists the executable at its base, no `relocated`, and the package DLLs the imports need.
* `dllmain` is all `1`; no `exception` before the entry point.
* The D3D12 trace starts with the device, queue, allocator, list and fence, then the first resource/PSO/draw.
* The first `unsupported` is a named method, not a crash; it is the next D3D12 call to fix. `LEONIDA_UNIMPLEMENTED`
  does not affect these outcomes.
* `exit` is the game's own code, not the loader's. A crash after the first frame is the next layer question.

## 6. Which pull requests a stop depends on

A stop is only comparable with someone else's when both name the same base, the same pull requests in the same
order, and a shader library made by the same translator. `tools/boot_matrix.py` makes that one row per named set:

```powershell
python -I tools\boot_matrix.py merge --base origin/main --fetch origin --out $matrix `
    --set main= --set rt5=445 --set rt5-plus=445,565
# check out or export boot-matrix/rt5 where you build, then, per set:
python -I tools\boot_matrix.py run $matrix --set rt5 --tree $tree --exe $exe --game-dir $pkg `
    --shaders $work\shaders\spirv --build
python -I tools\boot_matrix.py report $matrix
```

* `merge` only runs git (`merge-tree --write-tree`, `commit-tree`): no checkout, nothing from a pull request runs.
  A PR that `scripts/merge_check.py` finds already on the base is skipped (also one merged and then reverted there:
  git merges an ancestor as a no-op, so revert the revert on a branch and give it as `N:REF`); one that conflicts
  with an earlier PR in the set is skipped too (its files are listed in `matrix.json`, and whether it conflicts with
  the base itself or only with the PRs before it). Each set becomes the branch `boot-matrix/<set>`; an empty set
  (`main=`) is the base alone, the row to compare the others with. A set's branch that is checked out in the
  repository stops the merge before anything is written.
* `run` first checks that the tree holds the set's head: `merge` writes the blob id of every file of that head to
  `<matrix>/<set>/files.json`, and a tree that lacks one of them or holds other content stops the run (exit 2) before
  anything is built. CRLF line ends count as the same file; files the head does not have (a build folder) are not
  looked at. It then builds `runtime/` in the tree, in a build folder of the head's own
  (`runtime/build-boot-matrix-<head>`, so an earlier set's binary is never reused), or takes `--gta6rt`: such a run
  is recorded as built `prebuilt (unverified)`, since nothing shows which source the binary came from. It runs
  `--run --trace --trace-d3d12` with `LEONIDA_D3D12=vulkan` and stops the process tree at `--timeout` (600 s). Each
  run gets a fresh `LEONIDA_SAVE_DIR`, and a fresh overlay unless `--overlay` names one, so what an earlier run wrote
  cannot change it. The traces stay in `<matrix>/<set>/runN/`; `result.json` has the stop, the D3D12 call count, the
  last call and the run's options (`--env` names, timeout, overlay, configuration).
* The `Shaders` column compares the library's `translation_sources.sha256` (written by `make_game.py`) with the
  same digest over the tree: `stale` means the library came from another translator, a common reason a stop does
  not reproduce.
* `report` prints the Markdown table (or `--json`), with the newest run of each set's current head. Both pass on
  only a fixed list of fields, each checked for the form it should have: the base and the heads as commits, set
  names, each PR's number, status and why it was skipped, the build and shader state, the stop's kind, exit or
  exception code, module+RVA and access kind, the counts, the run's options (`LEONIDA_*` names, others counted) and
  the last D3D12 call as number, label and outcome. A label of the shape the runtime writes
  (`ID3D12Device::CreateFence`, `Xbox CreatePlacedResourceX (device slot 133)`) is shown whole, one with free text
  after its name is cut to the name, anything else is `(other)`; an outcome, exception code or access kind of
  another form is `other`, and a location outside every known module is `unmapped`. The refs given for the base and
  the PRs, merge_check's verdicts, conflicted file names, the access address and the loader's error message (it can
  name a path) stay in `matrix.json` and `result.json` on your machine. A `last exception` row is the last exception
  the trace recorded before the process ended; exceptions are logged before handlers run, so it is where to look
  first, not proof of the cause.

## 7. Limits

The retail executable is not in the package, so this page is untested against it; the tool and the trace formats
are tested on synthetic fixtures and on `tiny_game` (which has no D3D12 calls). The present slots (Q6) and the
Xbox device parameters (Q1) remain the first things the executable itself has to name. The D3D12 trace needs the
Vulkan backend; a build without it runs the game's calls through the PC device and records only the proxy's
device, queue, fence and allocator calls. The runbook
does not make the game boot; it makes the first real trace readable.
