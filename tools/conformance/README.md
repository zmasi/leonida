# Conformance harness: does the Vulkan-backed D3D12 device render what D3D12 renders?

The shim in `runtime/` reimplements D3D12 on Vulkan. Its own CTest suite in `runtime/test/`
already covers draws on SPIR-V shaders (including `d3d12_draw` and `d3d12_first_light`),
while this directory defines a small set of deterministic scenes designed to **diff the
shim's rendered pixels directly against native D3D12 on the same machine.**

This harness renders those scenes with native D3D12 (`native/`), verifies every pixel each
scene claims, and provides `compare.py` to diff two renderer manifests once a Vulkan-shim
driver is wired into `tools/conformance/`.

## Layout

```
tools/conformance/
  spec/scenes.h                  the contract: scene kinds, geometry, state, expected pixels
  spec/shaders/*.hlsl            the two shader pairs, compiled by dxc
  native/                        native D3D12 reference renderer (Windows, D3D12 + WIC)
  compare.py                     check a render against the contract, diff two renders
  COMPARE.md                     compare.py's modes, exit codes and limits
```

## The scenes

Six scenes, each chosen to make one failure visible at an exact coordinate:

| scene | proves |
|---|---|
| `clear` | render target creation, clear value, readback |
| `fullscreen` | vertex shader runs, pipeline binds, one triangle covers the target |
| `depth_order` | depth test rejects a later draw that is behind (green survives over red only because of it) |
| `scissor` | scissor clips at `{48,32,176,96}`, right/bottom exclusive |
| `blend` | 50% source over the clear colour, `SRC_ALPHA`/`INV_SRC_ALPHA` |
| `indexed` | the index buffer is read; vertices are stored out of draw order |

The contract states the baseline pipeline state every scene inherits (cull NONE,
`TRIANGLELIST`, sample mask, viewport, clip-space z in 0..1 with `w = 1`, row 0 is the top).
Anything an implementer would otherwise have to guess is written down, because two guesses
that disagree produce two different images and neither is wrong on its own terms.

## Run it

```powershell
# 1. build and render the reference images
powershell -NoProfile -ExecutionPolicy Bypass -File tools\conformance\native\scripts\build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\conformance\native\scripts\run.ps1

# 2. check every declared pixel
python tools\conformance\compare.py expect --manifest tools\conformance\native\out\run1\manifest.json

# 3. prove the scenes are not vacuous: corrupt three pieces of state and require three failures
powershell -NoProfile -ExecutionPolicy Bypass -File tools\conformance\native\scripts\negative-control.ps1
```

Requirements: Windows with the D3D12 runtime, Visual Studio 2022 (C++ x64), Windows SDK
10.0.26100 (`dxc.exe`), Python 3.10+ for `compare.py` (standard library only; no pip
installs). The renderer needs a D3D12 device with feature level 11_0 — it runs on the
Microsoft Basic Render Driver as well as on hardware.

## Status today

The Vulkan-backed D3D12 shim in `runtime/` has moved past the early `2e5c0f2` baseline where
`ID3D12Device::CreateRootSignature` returned `E_NOTIMPL` and blocked the five drawn scenes:

- **Implemented in `runtime/`:** `ID3D12Device::CreateRootSignature` and
  `ID3D12Device::CreateGraphicsPipelineState` (`runtime/shims/d3d12_vulkan_device.cpp`), along
  with `SetPipelineState`, `SetGraphicsRootSignature` and root-argument setters,
  `RSSetViewports`, `RSSetScissorRects`, `IASetVertexBuffers`, `IASetIndexBuffer`,
  `OMSetRenderTargets`, `DrawInstanced`, and `DrawIndexedInstanced`
  (`runtime/shims/d3d12_com.cpp`).
- **Covered by runtime CTest today:** `runtime/CMakeLists.txt` registers graphics draw tests
  such as `d3d12_draw` (`runtime/test/d3d12_draw_test.cpp`, synthetic GLSL shaders compiled to
  SPIR-V covering indexed draws, root constants/CBVs, culling, and alpha blending) and
  `d3d12_first_light` (`runtime/test/d3d12_first_light_test.cpp`, package-gated via
  `LEONIDA_FIRST_LIGHT_DIR`).
- **What `tools/conformance/` ships today:** the six-scene contract (`spec/scenes.h` and
  `spec/shaders/*.hlsl`), the native D3D12 reference renderer (`native/`), and the manifest
  checker / image diff tool (`compare.py`, documented in `COMPARE.md`). No Vulkan-shim
  conformance driver or SPIR-V compilation step for `spec/shaders/*.hlsl` is wired into
  `tools/conformance/` yet.

## The shader format question

The reference renderer compiles HLSL to **DXIL** because native D3D12 requires it. The
shim's documented contract is **SPIR-V**, because the game's shaders reach it from the RDNA2
translator. A PSO implementation that understands only one of them cannot serve both.

The cheapest resolution when wiring a shim driver into `tools/conformance/`: compile each
conformance shader twice (`dxc -T vs_6_6` and `dxc -spirv -T vs_6_6`) and let each renderer
take the blob it wants. One workload, one set of expected pixels, no DXIL translator in the
shim.

## Why the scenes are trustworthy

Three independent checks, all reproducible:

1. **The renderer verifies itself.** It reads every declared pixel back from the GPU *and*
   from the PNG decoded off disk, prints one line per check, and exits non-zero on any
   mismatch. 18 checks, 18 passes, on both the NVIDIA and AMD adapters.
2. **The negative control asserts its own result.** It disables the depth test, the blend
   state and the scissor rect in a throwaway copy, then requires exactly `depth_order`,
   `blend` and `scissor` to fail — each at the pixel that names the state. If a
   `String.Replace` silently stops matching, the script throws instead of reporting success.
3. **Determinism is measured**, not assumed: two runs produce byte-identical PNGs.

## Extending it

Add a `Scene` to `spec/scenes.h` with its expected pixels. The renderer picks it up through
`SceneKind`; `/we4062` makes an unhandled enumerator a build error, so a new kind cannot
silently render as a clear-only scene. Once a shim driver is added to `tools/conformance/`,
add a matching row to its switch as well.
