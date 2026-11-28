// Pipeline state stream copies and the derived-state patch (device slot 55) with synthetic streams only: no D3D12
// device, no game data. The layout rule is the one measured on the console's slot 86 stream (see the offsets check).
#include <d3d12.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "d3d12_xs_pso_stream.h"

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

std::size_t align_up(std::size_t v, std::size_t a) { return (v + a - 1) / a * a; }

// Builds a stream the way the console lays it out: u32 type, payload at its natural alignment, next entry 8-aligned.
struct StreamBuilder {
    std::vector<unsigned char> bytes;
    void entry(unsigned type, const void* payload, std::size_t size, std::size_t payload_align) {
        const std::size_t at = bytes.size();
        bytes.resize(at + 4);
        std::memcpy(bytes.data() + at, &type, 4);
        const std::size_t pay = align_up(at + 4, payload_align);
        bytes.resize(pay, 0);
        const auto* p = static_cast<const unsigned char*>(payload);
        bytes.insert(bytes.end(), p, p + size);
        bytes.resize(align_up(bytes.size(), 8), 0);
    }
};

// The test's own copy of the layout rule: payload offset of the first subobject of `wanted`, or npos.
std::size_t payload_of_type(const std::vector<unsigned char>& s, unsigned wanted) {
    std::size_t off = 0;
    while (off + 4 <= s.size()) {
        unsigned type;
        std::memcpy(&type, s.data() + off, 4);
        std::size_t size = sizeof(D3D12_SHADER_BYTECODE), align = 8;
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND) { size = sizeof(D3D12_BLEND_DESC); align = 4; }
        else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK) { size = 4; align = 4; }
        else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER) { size = sizeof(D3D12_RASTERIZER_DESC); align = 4; }
        else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1) { size = sizeof(D3D12_DEPTH_STENCIL_DESC1); align = 4; }
        else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) { size = 8; align = 8; }
        else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT) { size = sizeof(D3D12_INPUT_LAYOUT_DESC); align = 8; }
        const std::size_t pay = align_up(off + 4, align);
        if (type == wanted) return pay;
        off = align_up(pay + size, 8);
    }
    return static_cast<std::size_t>(-1);
}

const unsigned char kVsCode[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                   17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
const char kSemantic[] = "POSITION";

D3D12_INPUT_ELEMENT_DESC make_element(const char* name) {
    D3D12_INPUT_ELEMENT_DESC e{};
    e.SemanticName = name;
    e.Format = DXGI_FORMAT_R32G32B32_FLOAT;
    return e;
}

// Root signature (null), VS, BLEND, SAMPLE_MASK, RASTERIZER, DEPTH_STENCIL1, INPUT_LAYOUT.
std::vector<unsigned char> base_stream(const D3D12_INPUT_ELEMENT_DESC* elements, unsigned element_count) {
    StreamBuilder s;
    ID3D12RootSignature* root = nullptr;
    s.entry(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, &root, 8, 8);
    D3D12_SHADER_BYTECODE vs{kVsCode, sizeof(kVsCode)};
    s.entry(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, &vs, sizeof(vs), 8);
    D3D12_BLEND_DESC blend{};
    s.entry(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, &blend, sizeof(blend), 4);
    UINT mask = 0xffffffffu;
    s.entry(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, &mask, 4, 4);
    D3D12_RASTERIZER_DESC raster{};
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    s.entry(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, &raster, sizeof(raster), 4);
    D3D12_DEPTH_STENCIL_DESC1 ds1{};
    ds1.DepthEnable = TRUE;
    s.entry(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, &ds1, sizeof(ds1), 4);
    D3D12_INPUT_LAYOUT_DESC layout{elements, element_count};
    s.entry(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, &layout, sizeof(layout), 8);
    return s.bytes;
}

// One derived record: tag at +0, payload at +4.
std::vector<unsigned char> record(unsigned tag, const void* payload, std::size_t size) {
    std::vector<unsigned char> r(rt::d3d12_xs::kDerivedRecordSize, 0);
    std::memcpy(r.data(), &tag, 4);
    if (size) std::memcpy(r.data() + 4, payload, size);
    return r;
}

void test_layout_matches_console_offsets() {
    // Console slot 86 stream: types 0..5 then BLEND at +0x88 (payload +0x8c), SAMPLE_MASK at +0x1d8, RASTERIZER at
    // +0x1e0, DEPTH_STENCIL1 at +0x210 (measured in the notes, mesure sur l'exe).
    StreamBuilder s;
    ID3D12RootSignature* root = nullptr;
    s.entry(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, &root, 8, 8);
    D3D12_SHADER_BYTECODE sb{};
    for (unsigned i = 0; i < 5; ++i) s.entry(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS + i, &sb, sizeof(sb), 8);
    check(s.bytes.size() == 0x88, "six entries (types 0..5) end at +0x88");
    D3D12_BLEND_DESC blend{};
    s.entry(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, &blend, sizeof(blend), 4);
    check(payload_of_type(s.bytes, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND) == 0x8c,
          "BLEND payload at +0x8c");
    check(s.bytes.size() == 0x1d8, "BLEND entry ends at +0x1d8");
    UINT mask = 1;
    s.entry(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, &mask, 4, 4);
    check(s.bytes.size() == 0x1e0, "SAMPLE_MASK entry is 8 bytes, next entry at +0x1e0");
}

void test_capture_and_deep_copy() {
    D3D12_INPUT_ELEMENT_DESC elements[1] = {make_element(kSemantic)};
    std::vector<unsigned char> base = base_stream(elements, 1);
    D3D12_PIPELINE_STATE_STREAM_DESC desc{base.size(), base.data()};
    auto rec = rt::d3d12_xs::PsoStreamRecord::capture(desc);
    check(rec != nullptr, "a synthetic base stream is captured");
    if (!rec) return;
    check(rec->bytes().size() == base.size(), "the copy has the stream's size");

    // Scribble over the source and its pointed-to data: the copy must still hold the original content.
    std::memset(base.data(), 0xee, base.size());
    elements[0].SemanticName = "XXXXXXXX";
    D3D12_SHADER_BYTECODE vs_copy{};
    std::memcpy(&vs_copy, rec->bytes().data() + payload_of_type(rec->bytes(), D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS),
                sizeof(vs_copy));
    check(vs_copy.pShaderBytecode != kVsCode && vs_copy.BytecodeLength == sizeof(kVsCode) &&
              std::memcmp(vs_copy.pShaderBytecode, kVsCode, sizeof(kVsCode)) == 0,
          "shader bytecode is copied, not referenced");
    D3D12_INPUT_LAYOUT_DESC il{};
    std::memcpy(&il, rec->bytes().data() + payload_of_type(rec->bytes(), D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT),
                sizeof(il));
    check(il.NumElements == 1 && std::strcmp(il.pInputElementDescs[0].SemanticName, kSemantic) == 0,
          "input layout semantic names are copied");
}

void test_derive_single_and_all_tags() {
    D3D12_INPUT_ELEMENT_DESC elements[1] = {make_element(kSemantic)};
    std::vector<unsigned char> base = base_stream(elements, 1);
    auto rec = rt::d3d12_xs::PsoStreamRecord::capture({base.size(), base.data()});
    if (!rec) { check(false, "base captured for derive tests"); return; }

    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = 0x5;
    auto r2 = record(2, &blend, sizeof(blend));
    std::vector<unsigned char> out;
    check(rt::d3d12_xs::derive_stream(*rec, r2.data(), 1, out) == rt::d3d12_xs::DeriveStatus::ok,
          "tag 2 (blend) derives");
    const std::size_t bp = payload_of_type(out, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND);
    check(std::memcmp(out.data() + bp, &blend, sizeof(blend)) == 0, "tag 2 payload lands in the BLEND subobject");
    check(out.size() == rec->bytes().size(), "derived stream keeps the base size");

    D3D12_RASTERIZER_DESC raster{};
    raster.FillMode = D3D12_FILL_MODE_WIREFRAME;
    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = FALSE;
    UINT mask = 0x3;
    std::vector<unsigned char> three;
    auto r0 = record(0, &ds, sizeof(ds));
    auto r1 = record(1, &raster, sizeof(raster));
    auto r3 = record(3, &mask, 4);
    three.insert(three.end(), r0.begin(), r0.end());
    three.insert(three.end(), r1.begin(), r1.end());
    three.insert(three.end(), r3.begin(), r3.end());
    check(rt::d3d12_xs::derive_stream(*rec, three.data(), 3, out) == rt::d3d12_xs::DeriveStatus::ok,
          "three records (tags 0, 1, 3) derive");
    const std::size_t rp = payload_of_type(out, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER);
    const std::size_t mp = payload_of_type(out, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK);
    const std::size_t dp = payload_of_type(out, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1);
    check(std::memcmp(out.data() + rp, &raster, sizeof(raster)) == 0, "tag 1 payload lands in RASTERIZER");
    check(std::memcmp(out.data() + mp, &mask, 4) == 0, "tag 3 payload lands in SAMPLE_MASK");
    check(std::memcmp(out.data() + dp, &ds, sizeof(ds)) == 0, "tag 0 patches the DS1 prefix");
    const std::size_t tail = dp + sizeof(ds);
    check(out[tail] == 0 && out[tail + 1] == 0 && out[tail + 2] == 0 && out[tail + 3] == 0,
          "tag 0 keeps the DS1 depth-bounds field of the base");
}

void test_stencil_mask_record() {
    // Tag 5 carries the two stencil mask bytes at the DEPTH_STENCIL offsets 0x10/0x11 and zeros elsewhere (read from the
    // title's writes). It overlays only those two bytes of the base's DEPTH_STENCIL (or DEPTH_STENCIL1) payload.
    D3D12_INPUT_ELEMENT_DESC elements[1] = {make_element(kSemantic)};
    std::vector<unsigned char> base = base_stream(elements, 1);
    auto rec = rt::d3d12_xs::PsoStreamRecord::capture({base.size(), base.data()});
    if (!rec) { check(false, "base captured for the tag 5 tests"); return; }
    const std::size_t dp = payload_of_type(rec->bytes(), D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1);

    unsigned char mask_payload[0x16] = {};
    mask_payload[0x10] = 0x0f;  // read mask
    mask_payload[0x11] = 0x3c;  // write mask
    auto r5 = record(5, mask_payload, sizeof(mask_payload));
    std::vector<unsigned char> out;
    check(rt::d3d12_xs::derive_stream(*rec, r5.data(), 1, out) == rt::d3d12_xs::DeriveStatus::ok,
          "tag 5 alone derives on a base with DEPTH_STENCIL1");
    check(out[dp + 0x10] == 0x0f && out[dp + 0x11] == 0x3c, "tag 5 overlays the two stencil mask bytes");
    bool rest_same = true;
    for (std::size_t i = 0; i < 0x38; ++i)
        if (i != 0x10 && i != 0x11 && out[dp + i] != rec->bytes()[dp + i]) rest_same = false;
    check(rest_same, "tag 5 leaves the other DEPTH_STENCIL1 bytes unchanged");

    // Order: records apply in the order they are given. Tag 0 writes its own masks, tag 5 overrides them.
    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.StencilReadMask = 0x01;
    ds.StencilWriteMask = 0x02;
    auto r0 = record(0, &ds, sizeof(ds));
    std::vector<unsigned char> seq;
    seq.insert(seq.end(), r0.begin(), r0.end());
    seq.insert(seq.end(), r5.begin(), r5.end());
    check(rt::d3d12_xs::derive_stream(*rec, seq.data(), 2, out) == rt::d3d12_xs::DeriveStatus::ok,
          "tag 0 then tag 5 derive together");
    check(out[dp + 0x10] == 0x0f && out[dp + 0x11] == 0x3c, "tag 5 after tag 0 wins (record order)");

    seq.clear();
    seq.insert(seq.end(), r5.begin(), r5.end());
    seq.insert(seq.end(), r0.begin(), r0.end());
    check(rt::d3d12_xs::derive_stream(*rec, seq.data(), 2, out) == rt::d3d12_xs::DeriveStatus::ok,
          "tag 5 then tag 0 derive together");
    check(out[dp + 0x10] == 0x01 && out[dp + 0x11] == 0x02, "tag 0 after tag 5 wins (record order)");
}

void test_view_instancing_is_captured() {
    // The creation parser accepts a VIEW_INSTANCING subobject whose count is 0 (it refuses only a non-zero count).
    // Such a stream must also be captured: otherwise the PSO carries no kept stream and every derived PSO on it is
    // refused with "base PSO has no kept stream".
    D3D12_INPUT_ELEMENT_DESC elements[1] = {make_element(kSemantic)};
    std::vector<unsigned char> base = base_stream(elements, 1);
    const std::size_t off = base.size();
    const unsigned type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING;
    const std::size_t payload = align_up(off + 4, 8);
    base.resize(payload + sizeof(D3D12_VIEW_INSTANCING_DESC));
    std::memcpy(base.data() + off, &type, sizeof(type));
    D3D12_VIEW_INSTANCING_DESC vi{};
    vi.ViewInstanceCount = 0;
    vi.Flags = D3D12_VIEW_INSTANCING_FLAG_NONE;
    std::memcpy(base.data() + payload, &vi, sizeof(vi));

    auto rec = rt::d3d12_xs::PsoStreamRecord::capture({base.size(), base.data()});
    check(rec != nullptr, "a stream with a zero-count VIEW_INSTANCING subobject is captured");
    if (!rec) return;
    check(rec->bytes().size() == base.size(), "the copy has the stream's size");
    check(payload_of_type(rec->bytes(), type) == payload, "the VIEW_INSTANCING payload keeps its offset");

    // And a derived record still resolves against it, which is what the PSO needs.
    D3D12_RASTERIZER_DESC raster{};
    raster.FillMode = D3D12_FILL_MODE_WIREFRAME;
    auto r1 = record(1, &raster, sizeof(raster));
    std::vector<unsigned char> out;
    check(rt::d3d12_xs::derive_stream(*rec, r1.data(), 1, out) == rt::d3d12_xs::DeriveStatus::ok,
          "a derived stream still derives on a base with VIEW_INSTANCING");
}

void test_refusals() {
    D3D12_INPUT_ELEMENT_DESC elements[1] = {make_element(kSemantic)};
    std::vector<unsigned char> base = base_stream(elements, 1);
    auto rec = rt::d3d12_xs::PsoStreamRecord::capture({base.size(), base.data()});
    if (!rec) { check(false, "base captured for refusal tests"); return; }
    std::vector<unsigned char> out;

    auto r6 = record(6, nullptr, 0);
    check(rt::d3d12_xs::derive_stream(*rec, r6.data(), 1, out) == rt::d3d12_xs::DeriveStatus::unknown_tag,
          "tag 6 is refused (meaning not known)");

    // A base without a rasterizer subobject cannot take a tag-1 record.
    StreamBuilder s;
    ID3D12RootSignature* root = nullptr;
    s.entry(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, &root, 8, 8);
    D3D12_BLEND_DESC blend{};
    s.entry(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, &blend, sizeof(blend), 4);
    auto norast = rt::d3d12_xs::PsoStreamRecord::capture({s.bytes.size(), s.bytes.data()});
    if (!norast) { check(false, "stream without rasterizer captured"); return; }
    D3D12_RASTERIZER_DESC raster{};
    auto r1 = record(1, &raster, sizeof(raster));
    check(rt::d3d12_xs::derive_stream(*norast, r1.data(), 1, out) == rt::d3d12_xs::DeriveStatus::missing_state,
          "tag 1 on a base without RASTERIZER is refused");

    // A subobject type this module does not walk: VIEW_INSTANCING (payload with a pointer, not modelled).
    StreamBuilder v;
    unsigned char junk[32] = {};
    v.entry(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, junk, sizeof(junk), 8);
    check(rt::d3d12_xs::PsoStreamRecord::capture({v.bytes.size(), v.bytes.data()}) == nullptr,
          "a stream with an unmodelled subobject type is not captured");
}

}  // namespace

int main() {
    std::printf("pso_stream_test: derived pipeline state streams\n");
    test_layout_matches_console_offsets();
    test_capture_and_deep_copy();
    test_derive_single_and_all_tags();
    test_stencil_mask_record();
    test_view_instancing_is_captured();
    test_refusals();
    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
