// The slot 57 factory's blend fallback (shims/d3d12_xs_native_pso_fallback.h): only a pipeline that blends a
// B4G4R4A4 target is retried, with blending off on those targets alone. Any other E_NOTIMPL refusal keeps the blend.
#include "../shims/d3d12_xs_native_pso_fallback.h"

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC two_targets(DXGI_FORMAT first, DXGI_FORMAT second) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.NumRenderTargets = 2;
    desc.RTVFormats[0] = first;
    desc.RTVFormats[1] = second;
    for (auto& rt : desc.BlendState.RenderTarget) {
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    desc.BlendState.IndependentBlendEnable = FALSE;
    return desc;
}

}  // namespace

int main() {
    using rt::d3d12_xs::native_pipeline_detail::without_stand_in_blend;

    {
        auto desc = two_targets(DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM);
        expect(without_stand_in_blend(desc), "a blended B4G4R4A4 target is the stand-in's refusal");
        expect(!desc.BlendState.RenderTarget[0].BlendEnable, "blending goes off on the B4G4R4A4 target");
        expect(desc.BlendState.RenderTarget[1].BlendEnable, "the other target keeps its blend");
        expect(desc.BlendState.IndependentBlendEnable, "the targets now differ, so blending is per target");
    }
    {
        // No B4G4R4A4 target: an E_NOTIMPL here is some other refusal (a rect list, mesh limits, a dual-source output
        // that does not fit Location 0 Index 1), and the pipeline must not be rebuilt without its blend.
        const auto before = two_targets(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto desc = before;
        expect(!without_stand_in_blend(desc), "no B4G4R4A4 target, no retry");
        expect(std::memcmp(&desc, &before, sizeof(desc)) == 0, "the description is left as it was");
    }
    {
        auto before = two_targets(DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM);
        before.BlendState.RenderTarget[0].BlendEnable = FALSE;
        auto desc = before;
        expect(!without_stand_in_blend(desc), "a B4G4R4A4 target that does not blend, no retry");
        expect(std::memcmp(&desc, &before, sizeof(desc)) == 0, "the description is left as it was");
    }
    {
        auto desc = two_targets(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM);
        desc.NumRenderTargets = 1;  // the B4G4R4A4 format past the target count is not a target
        expect(!without_stand_in_blend(desc), "a format past NumRenderTargets is not looked at");
    }
    {
        // IndependentBlendEnable == FALSE with only RenderTarget[0] populated (normal D3D12 usage; RenderTarget[1..7]
        // zeroed): B4G4R4A4 in slot 0 must replicate RenderTarget[0] before setting IndependentBlendEnable = TRUE so
        // slot 1 keeps its blend state and RenderTargetWriteMask.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.NumRenderTargets = 2;
        desc.RTVFormats[0] = DXGI_FORMAT_B4G4R4A4_UNORM;
        desc.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BlendState.IndependentBlendEnable = FALSE;
        desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        expect(without_stand_in_blend(desc), "shared blend with B4G4R4A4 in slot 0 triggers fallback");
        expect(desc.BlendState.IndependentBlendEnable == TRUE, "fallback enables IndependentBlendEnable");
        expect(!desc.BlendState.RenderTarget[0].BlendEnable &&
                   desc.BlendState.RenderTarget[0].RenderTargetWriteMask == D3D12_COLOR_WRITE_ENABLE_ALL,
               "slot 0 disables blend while keeping its write mask");
        expect(desc.BlendState.RenderTarget[1].BlendEnable == TRUE &&
                   desc.BlendState.RenderTarget[1].SrcBlend == D3D12_BLEND_SRC_ALPHA &&
                   desc.BlendState.RenderTarget[1].RenderTargetWriteMask == D3D12_COLOR_WRITE_ENABLE_ALL,
               "slot 1 inherits RenderTarget[0]'s blend state and write mask instead of staying zeroed");
    }
    {
        // IndependentBlendEnable == FALSE with B4G4R4A4 in slot 1 and RenderTarget[1] zeroed: slot 1 is effectively
        // blended through RenderTarget[0], so the fallback must trigger and disable blend only on slot 1.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.NumRenderTargets = 2;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.RTVFormats[1] = DXGI_FORMAT_B4G4R4A4_UNORM;
        desc.BlendState.IndependentBlendEnable = FALSE;
        desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        expect(without_stand_in_blend(desc), "shared blend with B4G4R4A4 in slot 1 triggers fallback");
        expect(desc.BlendState.IndependentBlendEnable == TRUE, "slot 1 fallback enables IndependentBlendEnable");
        expect(desc.BlendState.RenderTarget[0].BlendEnable == TRUE, "slot 0 keeps its blend");
        expect(!desc.BlendState.RenderTarget[1].BlendEnable &&
                   desc.BlendState.RenderTarget[1].RenderTargetWriteMask == D3D12_COLOR_WRITE_ENABLE_ALL,
               "slot 1 disables blend and inherits RenderTarget[0]'s write mask");
    }
    {
        // IndependentBlendEnable == FALSE with RenderTarget[0].BlendEnable == FALSE and stale RenderTarget[1].BlendEnable
        // == TRUE: slot 1 is NOT effectively blended, so no fallback should trigger and desc must remain untouched.
        auto before = two_targets(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM);
        before.BlendState.IndependentBlendEnable = FALSE;
        before.BlendState.RenderTarget[0].BlendEnable = FALSE;
        before.BlendState.RenderTarget[1].BlendEnable = TRUE;  // ignored when IndependentBlendEnable == FALSE
        auto desc = before;
        expect(!without_stand_in_blend(desc), "stale RenderTarget[1].BlendEnable ignored when IndependentBlendEnable is FALSE");
        expect(std::memcmp(&desc, &before, sizeof(desc)) == 0, "desc unchanged when effective blend is off");
    }
    {
        // IndependentBlendEnable == TRUE preserves distinct per-target blend settings and write masks.
        auto desc = two_targets(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM);
        desc.BlendState.IndependentBlendEnable = TRUE;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0x3;
        desc.BlendState.RenderTarget[1].RenderTargetWriteMask = 0x5;
        expect(without_stand_in_blend(desc), "independent blend with B4G4R4A4 in slot 1 triggers fallback");
        expect(desc.BlendState.RenderTarget[0].BlendEnable == TRUE &&
                   desc.BlendState.RenderTarget[0].RenderTargetWriteMask == 0x3,
               "independent blend preserves slot 0");
        expect(!desc.BlendState.RenderTarget[1].BlendEnable &&
                   desc.BlendState.RenderTarget[1].RenderTargetWriteMask == 0x5,
               "independent blend disables slot 1 blend without overwriting its write mask");
    }
    if (failures) return 1;
    std::printf("native_pso_fallback: ok\n");
    return 0;
}
