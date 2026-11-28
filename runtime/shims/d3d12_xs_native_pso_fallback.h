#pragma once

// The one blend fallback of the slot 57 native graphics factory (RT-SLOT57-BLEND-STATE). Where B4G4R4A4_UNORM is not
// renderable, the Vulkan layer draws it into an RGBA8 stand-in and refuses blending there (E_NOTIMPL): the stand-in
// quantizes the shader output before the blend. A pipeline that blends such a target is retried with blending off on
// those targets alone. E_NOTIMPL also stands for refusals that have nothing to do with blending, so a pipeline with no
// blended B4G4R4A4 target is never retried.
#include <d3d12.h>

namespace rt::d3d12_xs::native_pipeline_detail {

// Turns blending off on the B4G4R4A4 targets that blend, and only there. False, with `desc` unchanged, when there is
// no such target: the refusal was not the stand-in's.
inline bool without_stand_in_blend(D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
    const unsigned count = desc.NumRenderTargets < 8 ? desc.NumRenderTargets : 8;
    const bool independent = desc.BlendState.IndependentBlendEnable != FALSE;
    bool changed = false;
    for (unsigned i = 0; i < count; ++i) {
        const auto& effective = desc.BlendState.RenderTarget[independent ? i : 0];
        if (desc.RTVFormats[i] == DXGI_FORMAT_B4G4R4A4_UNORM && effective.BlendEnable) {
            changed = true;
            break;
        }
    }
    if (!changed) return false;
    // With IndependentBlendEnable == FALSE, D3D12 applies RenderTarget[0] to every target and ignores
    // RenderTarget[1..7]. Replicate RenderTarget[0] before switching to per-target blend state so the
    // non-B4G4R4A4 targets keep their blend state and RenderTargetWriteMask.
    if (!independent) {
        for (unsigned i = 1; i < 8; ++i) desc.BlendState.RenderTarget[i] = desc.BlendState.RenderTarget[0];
        desc.BlendState.IndependentBlendEnable = TRUE;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (desc.RTVFormats[i] == DXGI_FORMAT_B4G4R4A4_UNORM && desc.BlendState.RenderTarget[i].BlendEnable)
            desc.BlendState.RenderTarget[i].BlendEnable = FALSE;
    }
    return true;
}

}  // namespace rt::d3d12_xs::native_pipeline_detail
