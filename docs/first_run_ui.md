# Offline first-run UI on Windows

This integration combines the retail frame-loop runtime with the shader and rendering changes needed to display the first-run controller prompts. The visual milestone is a solid background with legible prompts at the lower right. Gameplay and the 3D world are outside the verified result.

## Build

Use an x64 Developer PowerShell with MSVC, CMake and Ninja. Install the Vulkan SDK and put the SPIR-V tools and an AMDGPU-capable `llvm-mc` on `PATH`. The shader tool setup is described in [testing.md](testing.md). Supply an extracted Xbox 1.0.2.1 package and its matching executable from your own files.

Run the commands from the checkout. Set `$game` to the local package and `$scratch` to a writable directory outside both the package and the checkout. These commands support a fresh shader workspace. SDL3 is fetched during configuration if there is no installed static SDL3 package.

```powershell
$game = Read-Host 'Path to the extracted Xbox 1.0.2.1 package'
$scratch = Read-Host 'Path for the build, shader workspace and port'
$game = (Resolve-Path -LiteralPath $game).Path
$build = Join-Path $scratch 'build'
$work = Join-Path $scratch 'work'
$port = Join-Path $scratch 'port'
cmake -S runtime -B $build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLEONIDA_REQUIRE_VULKAN=ON -DLEONIDA_D3D12_SHIM_TESTS=ON -DLEONIDA_SDL3_FETCH=ON
cmake --build $build --target gta6rt
python -I make_game.py --game $game --work $work --out $port --runtime-build $build --stages check,shaders,runtime
```

Keep the generated shader workspace. The port reads the shader directory recorded in `shader_path.txt`. Native graphics pipelines also use the source containers in the sibling `dxbc` directory. The build indexes the base, library, FidelityFX, BVH and executable-embedded programs.

## Launch

Run the title offline. The Winsock shim permits loopback and the WinHTTP shim refuses outbound requests. The hardware verification used a Windows AppContainer with zero network capabilities. [Boot runbook](boot.md)

```powershell
& (Join-Path $port 'gta6rt.exe') --first-run-ui (Join-Path $game 'game_scarlett_master.exe')
```

The profile selects Vulkan, a 2 GiB graphics address arena, on-demand pipelines, console root-argument layout, frame pacing and the keyboard gamepad in its focused mode (`LEONIDA_KEYBOARD_PAD=1`: keys count only while the title window has focus, so typing into another program presses nothing on a consent page; scripted presses still run). GPU completion labels (`LEONIDA_GPU_LABELS`) are not part of it: the list values land the title's list writes, and two writers of one address could rewind a counter. Explicit environment settings take precedence; a variable that is set but empty does not count. Speech synthesis through the host's own runtime is not part of the profile: `LEONIDA_LOCAL_SPEECH=1` turns it on, and without it the silent voice answers. The switches are listed in [`runtime/README.md`](../runtime/README.md). The package directory is the working directory. Guest output goes under `first-run-ui/overlay` beside the runtime, and sparse physical-memory backing uses `first-run-ui/xmem` on that volume. Use `--overlay DIR` or the absolute directory in `LEONIDA_XMEM_BACKING_DIR` to choose other writable locations.

## Through the first-run pages with the scripted pad

Set `LEONIDA_TRACE_PIPELINE_COMPILE=1` and capture stderr with `--trace-d3d12` to time Vulkan pipeline creation.
Pass that log to `tools/boot_trace.py --d3d12`. The summary reports the longest compilation and wait and counts
builds without a recorded completion.

Measured on 25 November 2026 on native Windows 11 with `scripts/windows-boot/boot.ps1`, an AMD Radeon RX 6800 (driver 32.0.21030.2001) and the shaders of an existing work folder, using no keyboard or controller, only `LEONIDA_INPUT_SCRIPT` (`-InputScript`). After the landing prompt takes A, the title shows these pages in order. None of them reaches the network: the WinHTTP shim refuses outbound requests.

1. **Navigation experience** (first-run setup): A on the focused option moves on.
2. **Brightness**: A moves on.
3. **Legal notices**: a list of entries. A on an entry opens it instead of moving on. A repeating `a`, `down`, `down` cycle (one cycle every 10 s) got through.
4. **Legal terms and policies**: two check boxes (privacy policy, terms of service) and a Confirm button. Confirm stays greyed out and ignores A until both boxes are ticked. Focus starts on the first box. `down` stops at Confirm, so a script that only presses `a` and `down` gets stuck there with one box unticked. The page looks stuck but is waiting for the box. The steps that worked are `up` to the unticked box, `a` to tick it, `down` to Confirm, then `a`. While a box is focused, the right stick scrolls the text beside it (`rsdown`). The run that ticked the privacy box had scrolled to the end first; whether the box needs that is not measured.

Confirm shows one black frame and then a dark loading screen with a spinner. The spinner was still turning when the 400-second run ended, about 100 s after Confirm. The frame rate dropped to about 5 presents a second, the session state left its landing value, and there were no device faults. (The game and session states in a `boot.ps1` RESULT now need `LEONIDA_GAME_STATE_RVA` and `LEONIDA_SESSION_STATE_RVA`, measured locally: the script no longer has them built in and reports `?` without them; [scripts/windows-boot/README.md](../scripts/windows-boot/README.md).)

Timings move by 10 to 20 s between runs on the same machine, because the pipeline cache warms up and the landing prompt arrives earlier. A fixed timeline can therefore press A on the wrong item. A later block that goes back with `up` and retries the legal-terms page makes the script more robust. The run above used the cycle up to about 230 s, then `up,up`, fifteen `rsdown`, `a`, `down,down`, `a`, and from 290 s `up,a,down,a`. The second block is the one that confirmed.

## Contributions

The frame-loop stack comes from [#321](https://github.com/ubiquitarianism/leonida/pull/321). The archive and shader catalog, shader translation, depth and texture support, presentation window and rectangle-list changes retain their upstream commits. Native and derived pipeline support incorporates the work of enx, TomBox, LooperSalty and KkelebekK. Four patches of the published [graphics runtime patches](https://github.com/Sigtryggur-Mani-Gudmundsson/graphics-runtime-patches/tree/d9a0bc1b070d8a36c81b182bc67edd7d24b41bf2) (GPL-3.0, author smg, committed as "Codex") are in this integration almost as published, about 390 lines: `graphics-root-layout` (the switch between the two packings of graphics root arguments, `runtime/shims/d3d12_com.h`), `descriptor-table-refresh` (the title's descriptors pulled into a bound table, and the table scan in the submitting device, `runtime/shims/d3d12_xs_device.cpp` and `runtime/src/vulkan_backend.h`), `descriptor-write-through` (descriptors the layer writes also stored at the title's address, `runtime/shims/d3d12_xs_device.cpp` and `.h`) and `sampler-table-refresh` (title-backed sampler tables, `runtime/shims/d3d12_vulkan_device.cpp`), with their tests in `runtime/test/d3d12_title_va_test.cpp` and `runtime/test/d3d12_m2_textured_test.cpp`. Each region names its patch in a comment.
