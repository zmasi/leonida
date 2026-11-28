// LEONIDA_STUB_PIPELINES (diagnostic): a pipeline whose translated shader is missing is created as a stub instead of
// failing. Without the switch, a graphics PSO with no shader library is E_FAIL, as before; with it, the call succeeds
// and the PSO is a registered graphics pipeline built from the stages in src/stub_pipeline.h (MVP-GPU-04; its draws
// are d3d12_stub_draw_test's). Synthetic only: no shader library, no game data.
#include "../shims/d3d12_com.h"

#include <cstdio>
#include <cstdlib>

namespace {

int g_failed = 0;
#define CHECK(x)                                                                    \
    do {                                                                            \
        if (!(x)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);       \
            ++g_failed;                                                             \
        }                                                                           \
    } while (0)

template <class T>
struct Ref {
    T* p = nullptr;
    ~Ref() {
        if (p) p->Release();
    }
};

D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_desc() {
    static const unsigned char kBytes[16] = {'D', 'X', 'B', 'C'};  // never parsed: the library lookup comes first
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.VS = {kBytes, sizeof kBytes};
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    return desc;
}

}  // namespace

int main() {
    // No shader library, whatever the environment says.
    SetEnvironmentVariableW(L"LEONIDA_SHADERS", nullptr);
    _putenv_s("LEONIDA_STUB_PIPELINES", "");

    Ref<ID3D12Device> device;
    if (FAILED(rt::d3d12::create_vulkan_device_standalone(IID_PPV_ARGS(&device.p))) || !device.p) {
        std::printf("SKIP: no Vulkan device\n");
        return 77;
    }
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = graphics_desc();

    {  // Off: refused as before, and no object.
        Ref<ID3D12PipelineState> pso;
        const HRESULT hr = device.p->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso.p));
        if (hr == E_NOTIMPL) {  // a device without the GPU address arena or graphics entry points refuses earlier
            std::printf("SKIP: the device cannot create graphics pipelines (E_NOTIMPL)\n");
            return 77;
        }
        CHECK(hr == E_FAIL);
        CHECK(!pso.p);
    }
    {  // "0" is off too.
        _putenv_s("LEONIDA_STUB_PIPELINES", "0");
        Ref<ID3D12PipelineState> pso;
        CHECK(device.p->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso.p)) == E_FAIL && !pso.p);
    }
    {  // On: a registered graphics pipeline from the built-in stages, so SetPipelineState binds it.
        _putenv_s("LEONIDA_STUB_PIPELINES", "1");
        Ref<ID3D12PipelineState> pso;
        CHECK(device.p->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso.p)) == S_OK);
        CHECK(pso.p != nullptr);
        CHECK(rt::d3d12::graphics_pipeline_id(pso.p) != 0);
        CHECK(rt::d3d12::compute_pipeline_id(pso.p) == 0);
    }
    {  // Real refusals are not stubbed: a bad description is still E_INVALIDARG.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC bad = desc;
        bad.VS = {};
        Ref<ID3D12PipelineState> pso;
        CHECK(device.p->CreateGraphicsPipelineState(&bad, IID_PPV_ARGS(&pso.p)) == E_INVALIDARG && !pso.p);
    }
    _putenv_s("LEONIDA_STUB_PIPELINES", "");

    std::printf("%s\n", g_failed ? "FAILED" : "PASS");
    return g_failed ? 1 : 0;
}
