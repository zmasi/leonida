# GPU test results

This page keeps a local record of GPU test runs. The project's public tracker is
[#166](https://github.com/ItsNotRexed/Leonida/issues/166) (one comment per GPU and driver);
post counts and pass/fail lines there, never game shaders, logs or private paths.

| GPU | Driver, OS | Commit | build.py | check.py | run_gpu_tests.py | Failed | Entry |
|---|---|---|---|---|---|---|---|
| NVIDIA GeForce RTX 3090 (Ampere / GA102), subgroup 32..32 | NVIDIA 621.40, Vulkan 1.4.357, Windows 11 26320 | `db1a22d` | 274/274 translated and valid (run_gpu_tests.py) | run via run_gpu_tests.py | 209/264 passed, 54 skipped, 1 failed (native); 243/264 passed, 20 skipped, 1 failed (`LEONIDA_W64_EMULATE=1`); ctest 452/453 | `cs_imgatomic_ext` (fixed by #1518) | [local note](../gpu-test-results/RTX3090-62140-Windows.txt) |
| NVIDIA GeForce RTX 5070 Ti (Blackwell), subgroup 32 | NVIDIA 621.01, Windows 11 26300.9457 | `e0b08c7` | not run; pre-disassembled subset | run via run_gpu_tests.py | **Subset:** 82/98 passed, 16 skipped (native); 98/98 passed (`LEONIDA_W64_EMULATE=1`); Python assembler, no spirv-val. ctest 280/280 (MSVC) | none in the subset | [local note](../gpu-test-results/RTX5070Ti-62101.txt) |
| NVIDIA GeForce RTX 3080 Ti (Ampere), subgroup 32..32 | NVIDIA 616.92, Windows 11 26300.9974 | `d478a92` | not run; pre-disassembled subset | run via run_gpu_tests.py | **Subset:** 81/97 passed, 16 skipped (native); 97/97 passed (`LEONIDA_W64_EMULATE=1`); Python assembler, no spirv-val | none in the subset | [local note](../gpu-test-results/RTX3080Ti-61692.txt) |
| AMD Radeon RX 9070 GRE (RDNA4, GFX1201), subgroup 32..64 | RADV, Mesa 26.3.1-arch3.5, Vulkan 1.4.363, CachyOS | `05961f0` | 264/264 translated and valid | 203/206 pass, 3 skipped | 218/235 passed, 16 skipped (native and emulated) | `ms_wave_info_s3`, flaky (see below) | [local note](../gpu-test-results/RX9070GRE.txt) |
| AMD Radeon RX 9070 XT (RDNA4, GFX1201), subgroup 32..64 | AMD 26.11.2 (LLPC), Vulkan 1.4.359, Windows 11 26200.9974 | `58be9cd` | exit 0 (native and emulated) | 219/222 pass, 3 skipped (mesh fixtures build.py does not build) | 236/252 passed, 16 skipped, 0 failed (native and `LEONIDA_W64_EMULATE=1`) | none | [local note](../gpu-test-results/RX9070XT.txt) |
| AMD Radeon RX 9070 XT (RDNA4, GFX1201), subgroup 32..64 | RADV, Mesa 26.2.6-arch3.1, Vulkan 1.4.354, CachyOS | `5e95fa2` | stops at `cs_dpp_cmp` on main; 297/297 translated and valid with #1444 | 227/231 pass, 4 skipped (with #1444) | 243/263 passed, 19 skipped, 1 failed (native and emulated); 244/263, 0 failed with #1518 | `cs_imgatomic_ext` (fixture's old volume T#, fixed by #1518) | [local note](../gpu-test-results/RX9070XT-RADV.txt) |
| AMD Radeon RX 9070 (RDNA4, GFX1201), subgroup 32..64 | AMD 26.11.2 (LLPC), 32.0.32029.2010, Vulkan 1.4.359, Windows 11 26200.9974 | `d0da7da` | not run | not run | 231/249 passed, 16 skipped, 2 failed (native and `LEONIDA_W64_EMULATE=1` alike) | `cs_imgload_tfe`, `cs_imgload_d16_tfe`: the fixtures' expectation (#98); `ms_wave_info_s3` passes | [local note](../gpu-test-results/RX9070.txt) |
| AMD Radeon RX 9060 XT (RDNA4, GFX1200), subgroup 32..64 | AMD 26.11.2 (LLPC), 32.0.32029.2010, Vulkan 1.4.359, Windows 11 26300.9974 | `73516a2` | stops at `cs_dpp_cmp` on main; exit 0 with #1444 (native and emulated) | 230/234 pass, 3 skipped, 1 failed with #1444 and #1518 (`cs_texture_uav`, #2261) | 247/268 passed, 19 skipped, 2 failed (native and `LEONIDA_W64_EMULATE=1` alike) | `cs_imgatomic_ext` (#1518) and `cs_texture_uav` (#2261), both pass with those applied | [local note](../gpu-test-results/RX9060XT.txt) |
| AMD Radeon Graphics, Ryzen 5 7600X iGPU (RDNA2, gfx1036), subgroup 32..64 | AMD 26.11.2, 32.0.21045.29003, Vulkan 1.4.315, Windows 11 26200.9974 | `d0da7da` | not run | not run | 224/249 passed, 23 skipped, 2 failed (native and `LEONIDA_W64_EMULATE=1` alike) | `cs_imgload_tfe`, `cs_imgload_d16_tfe`: the fixtures' expectation (#98) | [local note](../gpu-test-results/Raphael-7600X-Win.txt) |
| AMD Radeon RX 7900 GRE (RDNA3, gfx1100), subgroup 32..64 | AMD proprietary 26.11.2 (LLPC), Vulkan 1.4.359, Windows 11 x64 | `5d74c27` | 247/247 translated and valid | run via run_gpu_tests.py | 225/241 passed, 16 skipped | none | [#166](https://github.com/ItsNotRexed/Leonida/issues/166#issuecomment-7115041436) |
| Intel Graphics (Meteor Lake, Xe-LPG), subgroup 8..32 | ANV, Mesa 26.2.7, Vulkan 1.4.354, Fedora 45 | `84347bb` | 279/279 translated and valid | 177/219 pass, 40 skipped, 2 failed | 194/248 passed, 52 skipped (native); 226/248 passed, 20 skipped (`LEONIDA_W64_EMULATE=1`) | `cs_imgload_tfe`, `cs_imgload_d16_tfe` (#31; pass with #98) | [local note](../gpu-test-results/IntelMTL.txt) |
| NVIDIA GeForce RTX 5070 Ti (Blackwell), subgroup 32..32 | NVIDIA 617.42, Vulkan 1.4.351, Windows 11 Pro 26300 | `21625bc` | 257/257 translated and valid (run_gpu_tests.py) | run via run_gpu_tests.py | 197/248 passed, 49 skipped, 2 failed (native); 229/248 passed, 17 skipped, 2 failed (`LEONIDA_W64_EMULATE=1`) | `cs_imgload_tfe`, `cs_imgload_d16_tfe` (#31) | [local note](../gpu-test-results/RTX5070Ti.txt) |
| Apple M5 Pro (16-core GPU, Metal 4), subgroup 4..32 | MoltenVK 1.4.2 (Homebrew), Vulkan 1.4.357, macOS 26.7.3 (25G342) | `d42773d07` | 280/280 translated and valid | 175/219 pass, 39 skipped, 5 failed | 183/248 passed, 61 skipped, 4 failed (native); 214/248 passed, 30 skipped, 4 failed (`LEONIDA_W64_EMULATE=1`) | `cs_fma_fused` (known row), `cs_imgload_tfe`, `cs_imgload_d16_tfe` (pass with #291), `ps_imgatomic` flake (#543) | [local note](../gpu-test-results/AppleM5Pro.txt) |
| Apple M3 (10-core GPU), subgroup 4..32 | MoltenVK 1.4.2 (Homebrew), Vulkan 1.4.357, macOS 26.5.1 (25F80) | `a73a57e13` | stops at `cs_dpp_cmp` on `main`; 301/301 translated and valid with #1444 | 185/234 pass, 44 skipped, 5 failed (with #1444) | 195/268 passed, 70 skipped, 3 failed (native); 229/268 passed, 36 skipped, 3 failed (`LEONIDA_W64_EMULATE=1`) | `cs_fma_fused` (#1724), `ps_ssharp` (#1774), `cs_imgatomic_ext` (#1518); check.py also `ps_ij_computed`, `ps_interp_f16_high` (#2176); validation clean but for the three documented messages; KosmicKrisp 190/268 (`cs_bvh`, `cs_bvh32`, `ps_cndmask_sign_mods`, `ps_valu16_mad_sel`: its `(NAN)` constants, #2229; `cs_imgatomic_ext`); llvmpipe 177/268 (`cs_imgatomic_ext`) | [local note](../gpu-test-results/AppleM3.txt) |
| NVIDIA GeForce RTX 3080 (Ampere, GA102, 10 GB GDDR6X), subgroup 32..32 | NVIDIA 580.178.04, Vulkan 1.4.312, Ubuntu 26.04 LTS | `d2cd6c0` | not run | not run | 197/248 passed, 49 skipped, 2 failed (native); 229/248 passed, 17 skipped, 2 failed (`LEONIDA_W64_EMULATE=1`) | `cs_imgload_tfe`, `cs_imgload_d16_tfe` (#31; see #280) | [local note](../gpu-test-results/RTX3080.txt) |
| NVIDIA GeForce RTX 5060 Ti (Blackwell), subgroup 32..32 | NVIDIA 610.88, Windows 11 | `5d74c27` | 270/270 translated and valid | 176/211 pass, 35 skipped | 192/241 passed, 48 skipped (native); 25/26 (`LEONIDA_W64_EMULATE=1 --filter w64`) | `ms_wave_info_s3` (known #8608) | [#166](https://github.com/ItsNotRexed/Leonida/issues/166) |

## AMD Radeon RX 9070 GRE — 2026-11-23

```
GPU:        AMD Radeon RX 9070 GRE (RDNA4 / GFX1201, Navi 48, PCI 1002:7550), subgroup 32..64
Driver:     RADV (Mesa 26.3.1-arch3.5), Vulkan 1.4.363
Subgroup:   32..64 (minSubgroupSize 32, maxSubgroupSize 64; default 64)
OS:         CachyOS, kernel 7.3.5-2-cachyos
Commit:     05961f0eeb24486ce25ddc7dfb453ca87db2be5f (main, 2026-11-23)
Tools:      llvm-mc 23.1.5 (native), SPIRV-Tools v2026.4, CMake 4.4.5 + Ninja 1.13.2, GCC 16.2.1, Python 3.14.9
build.py:   264 shaders: 264 translated, 264 pass spirv-val (6 mesh fixtures not built: ms_clip, ms_f16_pack, ms_tri, ms_v5_workgroup, ms_w64x_ballot, ms_wave_info_s3)
check.py:   203 of 206 pass, 3 skipped (the three mesh fixtures built only by run_gpu_tests.py)
run_gpu_tests.py: 218/235 passed, 16 skipped, 1 failed (native and LEONIDA_W64_EMULATE=1 alike)
Failed:     ms_wave_info_s3: at (0,0) got (-1,-1,-1,-1), want (0.0078,0.0078,0.5,1.0). Flaky on this RADV GPU: 1 pass and 2 fails in three isolated --filter reruns; same signature as the known round-44 mesh regression (#6172, fixed by #5741/#5855). Not a new failure.
Skipped:    16 skips are the documented "run by d3d12_*_test / w64_graphics.py" cases.
```

Setup: `cmake -S runtime -B runtime/build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DLEONIDA_VULKAN_HEADERS_FETCH=ON`;
implicit Vulkan layers disabled (`VK_LOADER_LAYERS_DISABLE='~implicit~'`) because MangoHud/Steam layers are installed.
Inside `run_gpu_tests.py`, 241/241 shaders translated and passed `spirv-val` in both the native and emulated runs.
Every pixel test other than the flaky mesh fixture was exact (4096/4096).

AI-assisted (Hermes Agent): built `main` at `05961f0`, ran `build.py`, `check.py` and
`run_gpu_tests.py` (native and emulated) on this GPU, and verified the isolated reruns of the one failure.

## AMD Radeon RX 9070 XT (RADV) — 2026-11-25

```
GPU:        AMD Radeon RX 9070 XT (RDNA4 / GFX1201, Navi 48, PCI 1002:7550), subgroup 32..64
Driver:     RADV (Mesa 26.2.6-arch3.1), Vulkan 1.4.354
Subgroup:   32..64 (minSubgroupSize 32, maxSubgroupSize 64; default 64)
OS:         CachyOS, kernel 7.3.4-1-cachyos
Commit:     5e95fa2f46d5df1a8397d5ee871add369031d8b6 (main, 2026-11-25)
Tools:      llvm-mc 23.1.5 (native), SPIRV-Tools v2026.4, CMake 4.5.1 + Ninja 1.13.2, GCC 16.2.1, Python 3.14.10
run_gpu_tests.py: 273 shaders: 273 translated, 273 pass spirv-val
            native:                 243/263 passed, 19 skipped, 1 failed
            LEONIDA_W64_EMULATE=1:  243/263 passed, 19 skipped, 1 failed
            with #1518 applied:     244/263 passed, 19 skipped, 0 failed (native and emulated)
Failed:     cs_imgatomic_ext: 6 of 64 sint words differ; first word 32: got 0x0badf00d, expected 0x00000101.
            Native and emulated, and 3 of 3 isolated --filter reruns. Not a driver fault: the fixture still
            builds the old 0x55 volume T# words that vk_store_test no longer keys on (#1518). With #1518
            applied, cs_imgatomic, cs_imgatomic3d and cs_imgatomic_ext all pass (64 words exact), native and
            emulated.
build.py:   on main, stops at cs_dpp_cmp.asm ("dpp variant of this instruction is not supported"), so check.py
            has no modules and reports 0 of 230 pass. With #1444 applied (and #1518): 297 shaders: 297
            translated, 297 pass spirv-val, native and emulated (6 mesh fixtures not built, as documented)
check.py:   with #1444 and #1518 applied: 227 of 231 pass, 4 skipped, native and LEONIDA_W64_EMULATE=1 alike
            (skips: ms_w64x_ballot, ms_wave_info_s3, ms_v5_workgroup built only by run_gpu_tests.py;
            cs_texture_uav needs the Windows D3D12 runtime)
Skipped:    run_gpu_tests.py's 19 skips are the documented "run by d3d12_*_test / w64_graphics.py" and Windows
            D3D12 runtime cases: cs_buf_counter, cs_buf_typed, cs_m4_fill, cs_m4_indirect, cs_texture_uav,
            ps_live_width, ps_m15, ps_m2, ps_m2_array, ps_m2_literal, ps_m2_uoff, ps_m3, ps_m3_post,
            ps_m3_query, ps_quad_flat, vs_m15, vs_m2, vs_m3, vs_quad_inst.
```

Setup: `cmake -S runtime -B <build> -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLEONIDA_VULKAN_HEADERS_FETCH=ON`;
ctest 93/93 passed (two skips). `ms_wave_info_s3`, which fails on the AMD proprietary driver in the entry below,
passed in every run here. On `main`, `build.py` stops at `cs_dpp_cmp.asm`, so `check.py` has nothing to run;
the `build.py` and `check.py` figures are with #1444 (and #1518) applied.

AI-assisted (Claude Code): built `main` at `5e95fa2`, ran `run_gpu_tests.py`, `build.py` and `check.py` (native and
emulated) on this GPU, reran the one failure in isolation, and reran with #1518 and #1444 applied.

## AMD Radeon RX 9070 XT — 2026-11-24

```
GPU:        AMD Radeon RX 9070 XT (RDNA4 / GFX1201, Navi 48, PCI 1002:7550), subgroup 32..64
Driver:     AMD proprietary 26.11.2 (LLPC), Vulkan 1.4.359
Subgroup:   32..64 (minSubgroupSize 32, maxSubgroupSize 64; default 64, subgroupSizeControl)
OS:         Windows 11, 10.0.26200.9974
Commit:     58be9cdf8df9a31bc7f6c068991f1aed381f7f4d (main, 2026-11-24)
Tools:      llvm-mc LLVM 23.1.6 (conda-forge, AMDGPU target), SPIRV-Tools v2026.4,
            CMake 4.4.4, Visual Studio 17 2022 generator, MSVC 14.44.35207, Python 3.12.10
run_gpu_tests.py: 261 shaders: 261 translated, 261 pass spirv-val
            native:                 236/252 passed, 16 skipped, 0 failed
            LEONIDA_W64_EMULATE=1:  236/252 passed, 16 skipped, 0 failed
build.py:   exit 0, native and emulated (6 mesh fixtures need a DXBC container and are not built)
check.py:   219 of 222 pass, 3 skipped (those mesh fixtures), native and emulated
Failed:     none
Skipped:    16 skips are the documented "run by d3d12_*_test / w64_graphics.py" cases.
```

Setup: `cmake -S runtime -B runtime/build` (default Visual Studio 17 2022 generator) and
`cmake --build runtime/build --config RelWithDebInfo`; then `run_gpu_tests.py`, and `build.py <out>` with
`check.py <out> runtime/build/RelWithDebInfo`, each once without and once with `LEONIDA_W64_EMULATE=1`. The suites
ran on the AMD LLPC driver with the project's own `vk_harness`, `vk_compute_test` and `vk_store_test` binaries; no
implicit layer beyond the Vulkan SDK's.

This replaces the 2026-11-23 record (`5d74c27`), which had `ms_wave_info_s3` failing on every native run and
`build.py` / `check.py` not run. On `58be9cd` the same driver passes `ms_wave_info_s3` with 4096/4096 exact pixels in
both full runs and in 3 of 3 isolated `--filter` reruns each way, and the D3D12-side columns are measured for the
first time on this card. The two `cs_imgload*_tfe` failures of earlier runs are gone with the fixture fix (#892).

AI-assisted: built `main` at `58be9cd` with MSVC, ran the three scripts native and under `LEONIDA_W64_EMULATE=1`,
and reran `ms_wave_info_s3` in isolation three times each way.

## AMD Radeon RX 7900 GRE — 2026-11-23

```
GPU:        AMD Radeon RX 7900 GRE (RDNA3 / gfx1100, PCI 1002:744C), subgroup 32..64
Driver:     AMD proprietary 26.11.2 (LLPC), Vulkan 1.4.359
Subgroup:   32..64 (minSubgroupSize 32, maxSubgroupSize 64; default 64)
OS:         Windows 11 x64
Commit:     5d74c2723 (main, 2026-11-23)
Tools:      llvm-mc 23.1.5 (MSYS2 UCRT64), Vulkan SDK 1.4.363 / SPIRV-Tools v2026.4, MSVC 14.51 (VS 2026 Community), Python 3.12.1
build.py:   247 shaders: 247 translated, 247 pass spirv-val
            wave64 VS/PS/GS on 32-wide subgroups: tier A: 1, tier C: 1
check.py:   not run separately (run_gpu_tests.py below)
run_gpu_tests.py: 225/241 passed, 16 skipped
Failed:     none
Skipped:    16 skips are the documented "run by d3d12_*_test / w64_graphics.py" cases.
```

Setup: `scripts\build_runtime.ps1` from a plain PowerShell. Without Ninja on `PATH` it took the cl.exe fallback, which
builds every Vulkan program the GPU tests use. `llvm-mc` came from the MSYS2 package `mingw-w64-ucrt-x86_64-llvm`
(23.1.5, AMDGPU target included) with `C:\msys64\ucrt64\bin` on `PATH`; on an old MSYS2 install, run `pacman -Syu`
until it reports nothing to do before installing it. Loader warnings from installed Medal/OBS Vulkan layers did not
affect the run. This is the first RDNA3 entry: wave64 and mesh shader tests ran natively (all `ms_*` pass).

AI-assisted (Claude): helped set up the tools and write this entry; the build and test run were done on this GPU.

## Intel Graphics (Meteor Lake) — 2026-11-24

```
GPU:        Intel Graphics (Meteor Lake-P, Xe-LPG, PCI 8086:7d45, integrated), Core Ultra 5 135U
Driver:     ANV (Mesa 26.2.7), Vulkan 1.4.354
Subgroup:   8..32 (minSubgroupSize 8, maxSubgroupSize 32; subgroupSizeControl, computeFullSubgroups)
OS:         Fedora Linux 45, kernel 7.3.4-300.fc45.x86_64
Commit:     84347bbb6 (main, 2026-11-24, after #24)
build.py:   279 shaders: 279 translated, 279 pass spirv-val
check.py:   177 of 219 pass, 40 skipped, 2 failed
run_gpu_tests.py: native 194/248 passed, 52 skipped, 2 failed
            LEONIDA_W64_EMULATE=1: 226/248 passed, 20 skipped, 2 failed
Failed:     cs_imgload_tfe, cs_imgload_d16_tfe (#31); both pass with #98 on the same main
```

The device has no 64-wide subgroup. Natively, the 32 tests that need one skip; under `LEONIDA_W64_EMULATE=1` all 32
run and pass. Besides the 16 documented "run by d3d12_*_test / w64_graphics.py" cases, four more skip in both modes
because this device cannot run them as written: `ps_bitmask_perm`, `ps_vcmpx_forms`, `ps_w64x_readlane` and
`ps_ij_computed`. The seven texture fixtures of the TEST-399 row in [testing.md](testing.md) pass with this Mesa
26.2.7. That row records 26.2.7 crashing on Iris Xe, so the crash may depend on the GPU generation or on the
distribution's build. The full record, with every skip by name, is in
[`gpu-test-results/IntelMTL.txt`](../gpu-test-results/IntelMTL.txt).

AI-assisted (Claude Code): built `main` at `84347bb`, ran `build.py`, `check.py` and `run_gpu_tests.py` native and
emulated on this GPU, and reran the tfe pair with #98 applied.

## NVIDIA GeForce RTX 5060 Ti — 2026-11-24

```
GPU:        NVIDIA GeForce RTX 5060 Ti (Blackwell), subgroup 32..32
Driver:     NVIDIA 610.88
Subgroup:   32..32 (native 32-wide)
OS:         Windows 11
Commit:     5d74c272363f2703265d44e6aaab1a1df8ddfaf3 (main, 2026-11-24)
Tools:      MSVC 19.52 (Visual Studio 2026) + Windows SDK 10.0.26100.0, Vulkan SDK 1.4.363.0, llvm-mc 21.1.8 (native), CMake 4.4.4, Python 3.14.4
build.py:   270 shaders: 270 translated, 270 pass spirv-val (6 mesh fixtures not built by build.py)
check.py:   176 of 211 pass, 35 skipped (0 failures)
run_gpu_tests.py (native): 192/241 passed, 48 skipped, 1 failed
run_gpu_tests.py (LEONIDA_W64_EMULATE=1 --filter w64): 25/26 passed, 1 skipped (0 failures)
ctest:      263/265 passed (2 known failures: d3d12_vulkan_device, d3d12_depth_resolve); 8/8 D3D12 shader tests passed after rebuilding with llvm-mc
Failed:     ms_wave_info_s3: at (0,0) got (-1.0000,-1.0000,-1.0000,-1.0000), want (0.0078,0.0078,0.5,1.0) — known issue, PR #8608.
Skipped:    35/48 skips are 64-wide subgroup / mesh / D3D12-delegated tests.
```

Setup: `cmake -S runtime -B runtime/build -DLLVM_MC=<path-to-llvm-mc.exe>`; `cmake --build runtime/build --config RelWithDebInfo --parallel`.
All 8 D3D12 shader tests (`d3d12_translated_draw`, `d3d12_m2_textured`, `d3d12_m2_formats`, `d3d12_m3_frame`, `d3d12_m3_first_pass`, `d3d12_m4_compute`, `d3d12_buffer_uav`, `d3d12_subgroup`) passed 8/8.

AI-assisted: built `main` at `5d74c2723`, ran `ctest`, `build.py`, `check.py`, and `run_gpu_tests.py` on this GPU with synthetic inputs only (no game files).
