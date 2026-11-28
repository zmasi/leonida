#include "d3d12_xs_pso_stream.h"
#include "d3d12_trace.h"

#include "../src/shader_library.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace rt::d3d12_xs {
namespace {

// Tag 5 and the DEPTH_STENCIL offsets of its two stencil mask bytes (StencilReadMask, StencilWriteMask).
constexpr std::uint32_t kStencilMaskTag = 5;
constexpr std::size_t kStencilReadMaskOffset = 0x10;
constexpr std::size_t kStencilWriteMaskOffset = 0x11;

// Streams beyond this are refused: a title's PSO stream is a few kilobytes.
constexpr std::size_t kMaxStreamBytes = 64 * 1024;

// Private-data GUIDs (random, fixed for this shim). The record pointer is found by GetPrivateData; the owner is an
// IUnknown the object releases when it is destroyed, which frees the record with it.
constexpr GUID kRecordPointerGuid{0x3c1f9a2e, 0x5b7d, 0x4e08, {0x8f, 0x12, 0x6a, 0x4d, 0x9c, 0x2b, 0x71, 0xe3}};
constexpr GUID kRecordOwnerGuid{0x9e4b2d17, 0x0a6c, 0x4f35, {0xb1, 0x8d, 0x23, 0x7e, 0x5f, 0xc9, 0x40, 0x6a}};

struct Layout {
    std::size_t size;
    std::size_t align;
};

// Size and alignment of each subobject payload this module walks (the SDK's D3D12_PIPELINE_STATE_SUBOBJECT_TYPE).
// Unknown types are not modelled: a stream with one is not captured.
bool layout_of(unsigned type, Layout& out) {
    switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: out = {8, 8}; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
            out = {sizeof(D3D12_SHADER_BYTECODE), 8};
            return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: out = {sizeof(D3D12_STREAM_OUTPUT_DESC), 8}; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: out = {sizeof(D3D12_BLEND_DESC), 4}; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: out = {4, 4}; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: out = {sizeof(D3D12_RASTERIZER_DESC), 4}; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: out = {sizeof(D3D12_DEPTH_STENCIL_DESC), 4}; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: out = {sizeof(D3D12_INPUT_LAYOUT_DESC), 8}; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
            out = {4, 4};
            return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
            out = {sizeof(D3D12_RT_FORMAT_ARRAY), 4};
            return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: out = {sizeof(DXGI_SAMPLE_DESC), 4}; return true;
        // Modelled so that a stream the creation parser accepts is also captured. That parser refuses a non-zero
        // `ViewInstanceCount`, so the payload's `pViewInstanceLocations` is never read; only the entry's own size and
        // alignment are needed to walk past it.
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
            out = {sizeof(D3D12_VIEW_INSTANCING_DESC), 8};
            return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: out = {sizeof(D3D12_CACHED_PIPELINE_STATE), 8}; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: out = {sizeof(D3D12_DEPTH_STENCIL_DESC1), 4}; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: out = {sizeof(D3D12_DEPTH_STENCIL_DESC2), 4}; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: out = {sizeof(D3D12_RASTERIZER_DESC1), 4}; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: out = {sizeof(D3D12_RASTERIZER_DESC2), 4}; return true;
        default: return false;
    }
}

std::size_t align_up(std::size_t value, std::size_t align) { return (value + align - 1) / align * align; }

struct Subobject {
    unsigned type;
    std::size_t tag_offset;
    std::size_t payload;  // offset of the payload in the stream
    std::size_t size;
};

// Walks a stream laid out as the console does: u32 type, payload at its natural alignment, next entry 8-aligned
// (the measured console offsets: BLEND payload +0x8c, DEPTH_STENCIL1 entry at +0x210). False when a type is unknown
// or the entries do not end exactly at `size`.
bool walk(const unsigned char* stream, std::size_t size, std::vector<Subobject>& out, bool console = false) {
    out.clear();
    std::size_t off = 0;
    while (off < size) {
        if (size - off < 4) return false;
        unsigned type;
        std::memcpy(&type, stream + off, 4);
        type = pc_stream_type(type, console);
        Layout layout{};
        if (!layout_of(type, layout)) return false;
        const std::size_t payload = align_up(off + 4, layout.align);
        if (payload > size || layout.size > size - payload) return false;
        out.push_back({type, off, payload, layout.size});
        off = align_up(payload + layout.size, 8);
    }
    return off == size;
}

// Copies `count` bytes into a new owned blob and returns its address (stable: the blob's buffer does not move).
const unsigned char* own_blob(std::vector<std::vector<unsigned char>>& blobs, const void* bytes, std::size_t count) {
    const auto* src = static_cast<const unsigned char*>(bytes);
    blobs.emplace_back(src, src + count);
    return blobs.back().data();
}

// Holds a PsoStreamRecord as an IUnknown: the object keeps one reference, the creation code drops its own.
class RecordOwner final : public IUnknown {
public:
    explicit RecordOwner(std::unique_ptr<PsoStreamRecord> record) : record_(std::move(record)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }
    PsoStreamRecord* record() { return record_.get(); }

private:
    ~RecordOwner() = default;
    std::atomic<ULONG> refs_{1};
    std::unique_ptr<PsoStreamRecord> record_;
};

// A record's target: the subobject it patches, a fallback subobject type (0 for none), and the payload bytes it carries.
struct Target {
    unsigned primary_type;
    unsigned fallback_type;
    std::size_t bytes;
};

// Tags 0 (depth-stencil) and 1 (rasterizer) are applied by overlay_depth_stencil and overlay_rasterizer, which also
// convert to the later subobject versions; these tags are copied as they are.
bool target_of_tag(std::uint32_t tag, Target& out) {
    switch (tag) {
        case 2:  // D3D12_BLEND_DESC
            out = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, 0, sizeof(D3D12_BLEND_DESC)};
            return true;
        case 3:  // sample mask (UINT)
            out = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, 0, sizeof(UINT)};
            return true;
        default: return false;
    }
}

// The base's subobject of the first of `types` it holds (a stream holds one version of each), or null.
const Subobject* find_first_of(const std::vector<Subobject>& subs, std::initializer_list<unsigned> types) {
    for (const unsigned type : types)
        for (const Subobject& sub : subs)
            if (sub.type == type) return &sub;
    return nullptr;
}

template <class T>
T read_payload(const unsigned char* bytes) {
    T value;
    std::memcpy(&value, bytes, sizeof(T));
    return value;
}

template <class T>
void write_payload(std::vector<unsigned char>& out, const Subobject& sub, const T& value) {
    std::memcpy(out.data() + sub.payload, &value, sizeof(T));
}

const Subobject* depth_stencil_of(const std::vector<Subobject>& subs) {
    return find_first_of(subs, {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL,
                                D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1,
                                D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2});
}

// Tag 0: a D3D12_DEPTH_STENCIL_DESC. It replaces DEPTH_STENCIL, the prefix of DEPTH_STENCIL1 (whose depth-bounds
// field stays the base's), or the matching fields of DEPTH_STENCIL2, whose faces each take the one pair of masks.
// False when the base has no depth-stencil subobject.
bool overlay_depth_stencil(const std::vector<Subobject>& subs, const unsigned char* record,
                           std::vector<unsigned char>& out) {
    const Subobject* sub = depth_stencil_of(subs);
    if (!sub) return false;
    const auto d = read_payload<D3D12_DEPTH_STENCIL_DESC>(record + kDerivedRecordPayload);
    if (sub->type != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2) {
        write_payload(out, *sub, d);
        return true;
    }
    auto d2 = read_payload<D3D12_DEPTH_STENCIL_DESC2>(out.data() + sub->payload);
    d2.DepthEnable = d.DepthEnable;
    d2.DepthWriteMask = d.DepthWriteMask;
    d2.DepthFunc = d.DepthFunc;
    d2.StencilEnable = d.StencilEnable;
    const auto face = [&](const D3D12_DEPTH_STENCILOP_DESC& f) {
        return D3D12_DEPTH_STENCILOP_DESC1{f.StencilFailOp, f.StencilDepthFailOp, f.StencilPassOp, f.StencilFunc,
                                           d.StencilReadMask, d.StencilWriteMask};
    };
    d2.FrontFace = face(d.FrontFace);
    d2.BackFace = face(d.BackFace);
    write_payload(out, *sub, d2);
    return true;
}

// Tag 5: the two stencil mask bytes of the record overwrite the base's depth-stencil masks (both faces' in
// DEPTH_STENCIL2); the rest of the record's zeros are not applied. False when the base has no depth-stencil subobject.
bool overlay_stencil_masks(const std::vector<Subobject>& subs, const unsigned char* record,
                           std::vector<unsigned char>& out) {
    const Subobject* sub = depth_stencil_of(subs);
    if (!sub) return false;
    const unsigned char read = record[kDerivedRecordPayload + kStencilReadMaskOffset];
    const unsigned char write = record[kDerivedRecordPayload + kStencilWriteMaskOffset];
    if (sub->type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2) {
        auto d2 = read_payload<D3D12_DEPTH_STENCIL_DESC2>(out.data() + sub->payload);
        d2.FrontFace.StencilReadMask = d2.BackFace.StencilReadMask = read;
        d2.FrontFace.StencilWriteMask = d2.BackFace.StencilWriteMask = write;
        write_payload(out, *sub, d2);
        return true;
    }
    out[sub->payload + kStencilReadMaskOffset] = read;
    out[sub->payload + kStencilWriteMaskOffset] = write;
    return true;
}

// Tag 1: a D3D12_RASTERIZER_DESC. It replaces RASTERIZER, or RASTERIZER1/2 converted: DepthBias as a float, and for
// RASTERIZER2 the line mode from MultisampleEnable and AntialiasedLineEnable (as D3D12 maps them). False when the base
// has no rasterizer subobject.
bool overlay_rasterizer(const std::vector<Subobject>& subs, const unsigned char* record,
                        std::vector<unsigned char>& out) {
    const Subobject* sub = find_first_of(subs, {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,
                                                D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1,
                                                D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2});
    if (!sub) return false;
    const auto r = read_payload<D3D12_RASTERIZER_DESC>(record + kDerivedRecordPayload);
    if (sub->type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER) {
        write_payload(out, *sub, r);
    } else if (sub->type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1) {
        const D3D12_RASTERIZER_DESC1 r1{r.FillMode, r.CullMode, r.FrontCounterClockwise, FLOAT(r.DepthBias),
                                        r.DepthBiasClamp, r.SlopeScaledDepthBias, r.DepthClipEnable,
                                        r.MultisampleEnable, r.AntialiasedLineEnable, r.ForcedSampleCount,
                                        r.ConservativeRaster};
        write_payload(out, *sub, r1);
    } else {
        const D3D12_LINE_RASTERIZATION_MODE line = r.MultisampleEnable       ? D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_WIDE
                                                  : r.AntialiasedLineEnable ? D3D12_LINE_RASTERIZATION_MODE_ALPHA_ANTIALIASED
                                                                            : D3D12_LINE_RASTERIZATION_MODE_ALIASED;
        const D3D12_RASTERIZER_DESC2 r2{r.FillMode, r.CullMode, r.FrontCounterClockwise, FLOAT(r.DepthBias),
                                        r.DepthBiasClamp, r.SlopeScaledDepthBias, r.DepthClipEnable, line,
                                        r.ForcedSampleCount, r.ConservativeRaster};
        write_payload(out, *sub, r2);
    }
    return true;
}

const Subobject* find_subobject(const std::vector<Subobject>& subs, const Target& target);

const Subobject* find_subobject(const std::vector<Subobject>& subs, const Target& target) {
    for (const Subobject& sub : subs)
        if (sub.type == target.primary_type) return &sub;
    if (target.fallback_type)
        for (const Subobject& sub : subs)
            if (sub.type == target.fallback_type) return &sub;
    return nullptr;
}

D3D12_SHADER_BYTECODE bytecode(const unsigned char* src, const Subobject& sub) {
    D3D12_SHADER_BYTECODE code{};
    std::memcpy(&code, src + sub.payload, sizeof(code));
    return code;
}

int dxil_kind(std::span<const std::uint8_t> code) {
    const auto dxil = leonida::dxbc_part(code, "DXIL");
    return dxil.size() >= 4 ? dxil[2] | (dxil[3] << 8) : -1;
}

}  // namespace

void audit_pso_stream(const D3D12_PIPELINE_STATE_STREAM_DESC& desc, HRESULT hr, bool normalized) {
    const auto* src = static_cast<const unsigned char*>(desc.pPipelineStateSubobjectStream);
    std::vector<Subobject> subs;
    if (!src || !desc.SizeInBytes || desc.SizeInBytes > kMaxStreamBytes ||
        !walk(src, desc.SizeInBytes, subs, normalized)) {
        std::fprintf(stderr, "[rt] pso-stream call=%llu normalized=%d hr=%08lx types=unwalked reason=%s\n",
                     d3d12_trace::last_sequence, int(normalized), static_cast<unsigned long>(hr),
                     d3d12_trace::stream_refusal ? d3d12_trace::stream_refusal : "-");
        return;
    }
    bool depth_bounds = false;
    std::string types, shaders;
    for (const auto& sub : subs) {
        unsigned raw;
        std::memcpy(&raw, src + sub.tag_offset, sizeof(raw));
        if (!types.empty()) types += ',';
        types += std::to_string(raw);
        if (sub.type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1 ||
            sub.type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2) {
            BOOL enabled;
            const auto offset = sub.type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1
                ? offsetof(D3D12_DEPTH_STENCIL_DESC1, DepthBoundsTestEnable)
                : offsetof(D3D12_DEPTH_STENCIL_DESC2, DepthBoundsTestEnable);
            std::memcpy(&enabled, src + sub.payload + offset, sizeof(enabled));
            depth_bounds |= enabled != FALSE;
        }
        if (!(sub.type >= 1 && sub.type <= 6) && sub.type != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS &&
            sub.type != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS) continue;
        const auto code = bytecode(src, sub);
        if (!code.pShaderBytecode || !code.BytecodeLength) continue;
        const std::span bytes{static_cast<const std::uint8_t*>(code.pShaderBytecode), code.BytecodeLength};
        const auto program = leonida::compute_program(bytes);
        shaders += " " + std::to_string(raw) + "/" + std::to_string(dxil_kind(bytes)) + "=" +
                   (program.bytes.empty() ? "unknown" : leonida::sha256_hex(program.bytes));
        if (FAILED(hr) && hr != E_NOTIMPL)
            shaders += " container=" + leonida::sha256_hex(bytes) + " bytes=" + std::to_string(bytes.size());
    }
    std::fprintf(stderr, "[rt] pso-stream call=%llu normalized=%d hr=%08lx bounds=%d types=%s%s reason=%s\n",
                 d3d12_trace::last_sequence, int(normalized), static_cast<unsigned long>(hr), int(depth_bounds),
                 types.c_str(), shaders.c_str(), d3d12_trace::stream_refusal ? d3d12_trace::stream_refusal : "-");
}

PsoStreamRecord::Payloads::~Payloads() {
    for (ID3D12RootSignature* root : roots)
        if (root) root->Release();
}

std::unique_ptr<PsoStreamRecord> PsoStreamRecord::capture(const D3D12_PIPELINE_STATE_STREAM_DESC& desc, bool console) {
    return capture(desc, std::span<const ShaderData>{}, console);
}

std::unique_ptr<PsoStreamRecord> PsoStreamRecord::capture(const D3D12_PIPELINE_STATE_STREAM_DESC& desc,
                                                          std::span<const ShaderData> shaders, bool console) {
    if (!desc.pPipelineStateSubobjectStream || desc.SizeInBytes == 0 || desc.SizeInBytes > kMaxStreamBytes)
        return nullptr;
    const auto* source = static_cast<const unsigned char*>(desc.pPipelineStateSubobjectStream);
    std::vector<Subobject> subs;
    if (!walk(source, desc.SizeInBytes, subs, console)) return nullptr;

    std::unique_ptr<PsoStreamRecord> record(new PsoStreamRecord());
    record->payloads_ = std::make_shared<Payloads>();
    for (const auto& shader : shaders) if (shader) record->payloads_->shaders.push_back(shader);
    record->bytes_.assign(source, source + desc.SizeInBytes);
    for (const Subobject& sub : subs) {
        if (console) std::memcpy(record->bytes_.data() + sub.tag_offset, &sub.type, sizeof(sub.type));
        if (!record->copy_pointed_data(sub.type, sub.payload)) return nullptr;
    }
    return record;
}

// The derived overrides touch value subobjects only. Shader bytes, semantic names and root
// signature references have the lifetime of the shared payload even after releasing the base PSO.
std::unique_ptr<PsoStreamRecord> PsoStreamRecord::derived(const PsoStreamRecord& base, std::vector<unsigned char> stream) {
    std::unique_ptr<PsoStreamRecord> record(new PsoStreamRecord());
    record->bytes_ = std::move(stream);
    record->payloads_ = base.payloads_;
    return record;
}

// Replaces the pointers in one subobject's payload with copies of what they point to.
bool PsoStreamRecord::copy_pointed_data(std::size_t type, std::size_t payload_offset) {
    unsigned char* payload = bytes_.data() + payload_offset;
    switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: {
            ID3D12RootSignature* root = nullptr;
            std::memcpy(&root, payload, sizeof(root));
            if (root) {
                payloads_->roots.push_back(root);
                root->AddRef();
            }
            return true;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: {
            D3D12_SHADER_BYTECODE code{};
            std::memcpy(&code, payload, sizeof(code));
            if (code.pShaderBytecode && code.BytecodeLength) {
                for (const auto& shader : payloads_->shaders)
                    if (shader->data() == code.pShaderBytecode && shader->size() == code.BytecodeLength) return true;
                code.pShaderBytecode = own_blob(payloads_->blobs, code.pShaderBytecode, code.BytecodeLength);
                std::memcpy(payload, &code, sizeof(code));
            }
            return true;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: {
            D3D12_STREAM_OUTPUT_DESC so{};
            std::memcpy(&so, payload, sizeof(so));
            if (so.pSODeclaration && so.NumEntries)
                so.pSODeclaration = reinterpret_cast<const D3D12_SO_DECLARATION_ENTRY*>(
                    own_blob(payloads_->blobs, so.pSODeclaration, so.NumEntries * sizeof(D3D12_SO_DECLARATION_ENTRY)));
            if (so.pBufferStrides && so.NumStrides)
                so.pBufferStrides = reinterpret_cast<const UINT*>(
                    own_blob(payloads_->blobs, so.pBufferStrides, so.NumStrides * sizeof(UINT)));
            std::memcpy(payload, &so, sizeof(so));
            return true;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: {
            D3D12_INPUT_LAYOUT_DESC layout{};
            std::memcpy(&layout, payload, sizeof(layout));
            if (!layout.pInputElementDescs || !layout.NumElements) return true;
            const std::size_t bytes = layout.NumElements * sizeof(D3D12_INPUT_ELEMENT_DESC);
            auto* elements = reinterpret_cast<D3D12_INPUT_ELEMENT_DESC*>(
                const_cast<unsigned char*>(own_blob(payloads_->blobs, layout.pInputElementDescs, bytes)));
            for (UINT i = 0; i < layout.NumElements; ++i) {
                if (!elements[i].SemanticName) continue;
                const std::size_t len = std::strlen(elements[i].SemanticName) + 1;
                elements[i].SemanticName =
                    reinterpret_cast<const char*>(own_blob(payloads_->blobs, elements[i].SemanticName, len));
            }
            layout.pInputElementDescs = elements;
            std::memcpy(payload, &layout, sizeof(layout));
            return true;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: {
            D3D12_CACHED_PIPELINE_STATE cached{};
            std::memcpy(&cached, payload, sizeof(cached));
            if (cached.pCachedBlob && cached.CachedBlobSizeInBytes) {
                cached.pCachedBlob = own_blob(payloads_->blobs, cached.pCachedBlob, cached.CachedBlobSizeInBytes);
                std::memcpy(payload, &cached, sizeof(cached));
            }
            return true;
        }
        default: return true;  // no pointers in this payload
    }
}

DeriveStatus derive_stream(const PsoStreamRecord& base, const unsigned char* records, unsigned count,
                           std::vector<unsigned char>& out) {
    if (!records || count == 0 || count > kDerivedMaxRecords) return DeriveStatus::malformed;
    out = base.bytes();
    std::vector<Subobject> subs;
    if (!walk(out.data(), out.size(), subs)) return DeriveStatus::malformed;

    for (unsigned i = 0; i < count; ++i) {
        const unsigned char* record = records + i * kDerivedRecordSize;
        std::uint32_t tag;
        std::memcpy(&tag, record, sizeof(tag));
        if (tag == kStencilMaskTag) {
            if (!overlay_stencil_masks(subs, record, out)) return DeriveStatus::missing_state;
            continue;
        }
        if (tag == 0 || tag == 1) {
            const bool applied = tag == 0 ? overlay_depth_stencil(subs, record, out) : overlay_rasterizer(subs, record, out);
            if (!applied) return DeriveStatus::missing_state;
            continue;
        }
        Target target{};
        if (!target_of_tag(tag, target)) return DeriveStatus::unknown_tag;
        const Subobject* sub = find_subobject(subs, target);
        if (!sub) return DeriveStatus::missing_state;
        std::memcpy(out.data() + sub->payload, record + kDerivedRecordPayload, target.bytes);
    }
    return DeriveStatus::ok;
}

bool attach_pso_record(ID3D12Object* object, std::unique_ptr<PsoStreamRecord> record) {
    if (!object || !record) return false;
    RecordOwner* owner = new RecordOwner(std::move(record));  // one reference: ours
    if (FAILED(object->SetPrivateDataInterface(kRecordOwnerGuid, owner))) {
        owner->Release();
        return false;
    }
    // The object holds its own reference now; drop ours. The pointer below is for lookups only.
    owner->Release();
    RecordOwner* pointer = owner;
    return SUCCEEDED(object->SetPrivateData(kRecordPointerGuid, sizeof(pointer), &pointer));
}

bool share_pso_record(ID3D12Object* source, ID3D12Object* target) {
    UINT size=sizeof(RecordOwner*);
    RecordOwner* owner=nullptr;
    if(!source || !target || FAILED(source->GetPrivateData(kRecordPointerGuid,&size,&owner)) ||
       size!=sizeof(owner) || !owner) return false;
    if(FAILED(target->SetPrivateDataInterface(kRecordOwnerGuid,owner))) return false;
    return SUCCEEDED(target->SetPrivateData(kRecordPointerGuid,sizeof(owner),&owner));
}

const PsoStreamRecord* find_pso_record(ID3D12Object* object) {
    if (!object) return nullptr;
    UINT size = sizeof(RecordOwner*);
    RecordOwner* owner = nullptr;
    if (FAILED(object->GetPrivateData(kRecordPointerGuid, &size, &owner)) || size != sizeof(RecordOwner*) || !owner)
        return nullptr;
    return owner->record();
}

}  // namespace rt::d3d12_xs
