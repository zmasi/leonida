#pragma once

// Source: adapted from enx/leonida, commit 5c175db901f8486bcdefd03746b578b657e3151f on the original author's git
// mirror (https://git.notrexed.dev/enx/leonida), a fork of this project. Licence: GPL-3.0-or-later as a fork of this
// project. That server could not be reached on 2026-11-25, when this file was merged, so the source and its licence
// could not be read there; the maintainer accepted the file because the source is a fork of this same
// GPL-3.0-or-later project (the README links the server as the original author's mirror) and because
// tools/native_pso_hashes.py is adapted from the same commit. Changed, as the pull request that brought it describes
// it (the source could not be compared): the parsing is standard-library code of its own, apart from the D3D12 part
// (runtime/shims/d3d12_xs_native_pso.h), and a stage is taken from the translator's index entry of the container
// its records name, not from the position of its block.
//
// The description Xbox device slot 57 receives (native graphics pipelines) and how its stages find the game's own
// shader containers. Standard library only, so the parsing is tested on every platform.
//
// Layout measured by LooperSalty on 1.0.2.1 and implemented by enx in 5c175db90: 0x108 bytes, the root
// signature at +0x00, pairs at +0x08 and +0x18, then seven blocks {code, size, records, size} at +0x28 + k * 0x20.
// Measured by KkelebekK: pair 0 is state blob A (380-520 bytes: blend, decode_state_a_blend; raster and
// depth, not decoded), pair 1 is state blob B (80-188 bytes: formats and input layout, decode_state_b), and each
// stage's records blob carries the source container's HASH digest after its 0x43 word (docs/fspx.md, kind 7).
// The block index is not trusted as the stage: the stage comes from the translator's index entry of the container.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace leonida::native_pso {

struct Pair {
    const void* data;
    std::uint64_t size;
};

struct StageBlock {
    Pair code;     // +0x00: the stage's native program
    Pair records;  // +0x10: kind-7 style records holding the container's HASH digest
};

inline constexpr std::size_t kStageBlocks = 7;

struct GraphicsDesc {
    const void* root_signature;  // +0x00
    Pair state_a;                // +0x08
    Pair state_b;                // +0x18
    StageBlock stages[kStageBlocks];  // +0x28
};

static_assert(sizeof(Pair) == 0x10 && sizeof(StageBlock) == 0x20, "pairs are {pointer, u64 size}");
static_assert(offsetof(GraphicsDesc, state_a) == 0x08 && offsetof(GraphicsDesc, state_b) == 0x18);
static_assert(offsetof(GraphicsDesc, stages) == 0x28 && sizeof(GraphicsDesc) == 0x108,
              "the slot-57 description has a 0x108-byte prefix");

// Far above the largest blob measured (a few KiB), and small enough for 32-bit offsets inside one.
inline constexpr std::uint64_t kPairLimit = 16u << 20;

// The size, not the pointer, determines whether a pair is absent.
enum class PairState { absent, present, malformed };

// A table entry with size 0 is absent whatever its pointer (the title passes base + offset, measured on 1.0.2.1).
inline PairState pair_state(const Pair& p) {
    if (!p.size) return PairState::absent;
    if (!p.data || p.size > kPairLimit) return PairState::malformed;
    return PairState::present;
}

inline std::span<const std::uint8_t> bytes(const Pair& p) {
    if (pair_state(p) != PairState::present) return {};
    return {static_cast<const std::uint8_t*>(p.data), static_cast<std::size_t>(p.size)};
}

inline std::uint32_t read_u32(std::span<const std::uint8_t> b, std::size_t at) {
    std::uint32_t v = 0;
    if (at + 4 <= b.size()) std::memcpy(&v, b.data() + at, 4);
    return v;
}

struct InputElement {
    std::string name;
    std::uint32_t semantic_index = 0;
    std::uint32_t format = 0;
    std::uint32_t input_slot = 0;
    std::uint32_t aligned_byte_offset = 0;
    std::uint32_t classification = 0;
    std::uint32_t step_rate = 0;
};

// The colour-target registers of state blob A (RT-SLOT57-BLEND-STATE, docs/fspx.md "State blob A"): CB_TARGET_MASK
// and CB_BLEND0..7_CONTROL as the console's driver serialized them, in the RDNA2 register layout.
struct BlendRegisters {
    std::uint32_t target_mask = 0;          // CB_TARGET_MASK: four component bits (R, G, B, A) per target
    std::array<std::uint32_t, 8> blend{};   // CB_BLENDn_CONTROL
};

// The depth, stencil and raster registers of state blob A (RT-SLOT57-DEPTH-RASTER, docs/fspx.md "State blob A").
struct DepthRasterRegisters {
    std::uint32_t clip_cntl = 0;            // PA_CL_CLIP_CNTL
    std::uint32_t mode_cntl = 0;            // PA_SU_SC_MODE_CNTL
    std::uint32_t depth_control = 0;        // DB_DEPTH_CONTROL
    std::uint32_t stencil_control = 0;      // DB_STENCIL_CONTROL
    std::uint32_t stencil_ref_mask = 0;     // DB_STENCILREFMASK
    std::uint32_t stencil_ref_mask_bf = 0;  // DB_STENCILREFMASK_BF
};

// State blob B. The u32 field widths after each name are a hypothesis that fits the
// measured sizes: 0x50 bytes with no element, and 188 bytes with POSITION, COLOR and TEXCOORD.
struct FixedState {
    std::optional<BlendRegisters> blend;        // from blob A; empty when blob A is absent or not the measured shape
    std::optional<DepthRasterRegisters> depth_raster;  // from blob A, likewise
    std::array<std::uint8_t, 8> rtv_formats{};  // +0x10, DXGI_FORMAT values
    std::uint32_t sample_mask = 0;              // +0x1c
    std::uint8_t dsv_format = 0;                // +0x24
    std::uint8_t rt_count = 0;                  // +0x25
    std::uint8_t topology_type = 0;             // +0x26, low nibble
    std::uint8_t sample_count = 1;              // +0x28
    std::vector<InputElement> elements;         // count at +0x4c, packed from +0x50
};

inline constexpr std::size_t kStateBHeader = 0x50;
inline constexpr std::uint32_t kMaxInputElements = 32;  // D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT
inline constexpr std::uint32_t kMaxSemanticName = 256;

// Empty for a blob that is too short, names more than 8 targets, a topology type past PATCH (4), or an element
// list that runs past the blob or has a malformed name. Bytes after the last element are allowed.
inline std::optional<FixedState> decode_state_b(std::span<const std::uint8_t> b, const char** why = nullptr) {
    const auto fail = [&](const char* reason) {
        if (why) *why = reason;
        return std::nullopt;
    };
    if (b.size() < kStateBHeader) return fail("blob B shorter than its header");
    FixedState s;
    std::memcpy(s.rtv_formats.data(), b.data() + 0x10, 8);
    s.sample_mask = read_u32(b, 0x1c);
    s.dsv_format = b[0x24];
    s.rt_count = b[0x25];
    s.topology_type = b[0x26] & 0x0f;
    s.sample_count = b[0x28] ? b[0x28] : 1;
    if (s.rt_count > 8) return fail("blob B render target count over 8");
    if (s.topology_type > 4) return fail("blob B topology type over 4");
    const std::uint32_t count = read_u32(b, 0x4c);
    if (count > kMaxInputElements) return fail("blob B input element count over 32");
    std::size_t at = kStateBHeader;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (at + 4 > b.size()) return fail("blob B input element past the end");
        const std::uint32_t length = read_u32(b, at);
        at += 4;
        if (length < 2 || length > kMaxSemanticName || at + length + 24 > b.size())
            return fail("blob B semantic name length out of range");
        const auto* name = reinterpret_cast<const char*>(b.data() + at);
        if (name[length - 1] != '\0' || std::memchr(name, '\0', length - 1))
            return fail("blob B semantic name not one NUL-terminated string");
        InputElement e;
        e.name.assign(name, length - 1);
        at += length;
        e.semantic_index = read_u32(b, at);
        e.format = read_u32(b, at + 4);
        e.input_slot = read_u32(b, at + 8);
        e.aligned_byte_offset = read_u32(b, at + 12);
        e.classification = read_u32(b, at + 16);
        e.step_rate = read_u32(b, at + 20);
        at += 24;
        s.elements.push_back(std::move(e));
    }
    return s;
}

// State blob A, measured on 1.0.2.1 over all 5,680 distinct blobs (25,471 graphics records, docs/fspx.md "State blob
// A"): 380 + 8k bytes (k = 0..5) for vertex-shader pipelines and 464 + 8k (k = 0..7) for mesh pipelines, whose stage
// block is 84 bytes longer, so every offset after +0x28 moves by 84 there. Word 0 is 0x4005d800 with bits 3 and 4
// free. CB_TARGET_MASK is at +0x78 and CB_BLEND0..7_CONTROL at +0xd0..+0xec (vertex-pipeline offsets). Any other
// shape is not decoded: the caller keeps its defaults.
inline constexpr std::uint32_t kStateATag = 0x4005d800;
inline constexpr std::size_t kStateATargetMask = 0x78, kStateABlend = 0xd0, kStateAMeshShift = 84;
inline constexpr std::size_t kStateAClip = 0x9c, kStateAMode = 0xa0, kStateADepth = 0xb8, kStateAStencil = 0xbc,
                             kStateARefMask = 0xc0, kStateARefMaskBf = 0xc4;

// The offset shift of a measured blob A shape (0 for vertex pipelines, 84 for mesh ones), or empty.
inline std::optional<std::size_t> state_a_shift(std::span<const std::uint8_t> a, const char** why) {
    const auto fail = [&](const char* reason) {
        if (why) *why = reason;
        return std::nullopt;
    };
    const std::size_t n = a.size();
    std::size_t shift = 0;
    if (n >= 380 && n <= 420 && (n - 380) % 8 == 0) shift = 0;
    else if (n >= 464 && n <= 520 && (n - 464) % 8 == 0) shift = kStateAMeshShift;
    else return fail("blob A size not one of the measured shapes");
    if ((read_u32(a, 0) & ~0x18u) != kStateATag) return fail("blob A tag not 0x4005d800");
    return shift;
}

inline std::optional<BlendRegisters> decode_state_a_blend(std::span<const std::uint8_t> a, const char** why = nullptr) {
    const auto shift = state_a_shift(a, why);
    if (!shift) return std::nullopt;
    BlendRegisters r;
    r.target_mask = read_u32(a, kStateATargetMask + *shift);
    for (std::size_t i = 0; i < r.blend.size(); ++i) r.blend[i] = read_u32(a, kStateABlend + *shift + 4 * i);
    return r;
}

// PA_CL_CLIP_CNTL at +0x9c, PA_SU_SC_MODE_CNTL at +0xa0, DB_DEPTH_CONTROL at +0xb8, DB_STENCIL_CONTROL at +0xbc and
// DB_STENCILREFMASK(_BF) at +0xc0/+0xc4 (vertex-pipeline offsets), measured as for the blend registers.
inline std::optional<DepthRasterRegisters> decode_state_a_depth_raster(std::span<const std::uint8_t> a,
                                                                       const char** why = nullptr) {
    const auto shift = state_a_shift(a, why);
    if (!shift) return std::nullopt;
    DepthRasterRegisters r;
    r.clip_cntl = read_u32(a, kStateAClip + *shift);
    r.mode_cntl = read_u32(a, kStateAMode + *shift);
    r.depth_control = read_u32(a, kStateADepth + *shift);
    r.stencil_control = read_u32(a, kStateAStencil + *shift);
    r.stencil_ref_mask = read_u32(a, kStateARefMask + *shift);
    r.stencil_ref_mask_bf = read_u32(a, kStateARefMaskBf + *shift);
    return r;
}

// One target's blend state in D3D12 terms, with D3D12_BLEND and D3D12_BLEND_OP values (d3d12.h) so this header
// stays standard-library only.
struct TargetBlend {
    bool enable = false;
    std::uint8_t src = 2, dst = 1, op = 1;              // D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD
    std::uint8_t src_alpha = 2, dst_alpha = 1, op_alpha = 1;
    std::uint8_t write_mask = 0xf;                      // D3D12_COLOR_WRITE_ENABLE_*: R 1, G 2, B 4, A 8
};

// An RDNA2 blend factor (V_028780_BLEND_*, the gfx10 register header) as a D3D12_BLEND, or 0 for one this path does
// not take: BOTH_(INV_)SRC_ALPHA (11, 12; not on gfx10). The dual-source SRC1 factors (15 to 18) read the pixel
// shader's SV_Target1, which the Vulkan layer moves to Location 0, Index 1 (RT-DUAL-SOURCE-BLEND). For an alpha
// factor, a colour factor names its alpha.
inline std::uint8_t d3d12_blend_factor(std::uint32_t f, bool alpha) {
    static constexpr std::uint8_t colour[21] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, 0, 14, 15, 16, 17, 18, 19, 20, 21};
    if (f >= std::size(colour)) return 0;
    if (alpha) {
        switch (f) {
            case 2: return 5;   // SRC_COLOR -> SRC_ALPHA
            case 3: return 6;   // ONE_MINUS_SRC_COLOR -> INV_SRC_ALPHA
            case 8: return 7;   // DST_COLOR -> DEST_ALPHA
            case 9: return 8;   // ONE_MINUS_DST_COLOR -> INV_DEST_ALPHA
            case 15: return 18; // SRC1_COLOR -> SRC1_ALPHA
            case 16: return 19; // INV_SRC1_COLOR -> INV_SRC1_ALPHA
            default: break;
        }
    }
    return colour[f];
}

// An RDNA2 COMB_FCN as a D3D12_BLEND_OP, or 0: DST_PLUS_SRC ADD, SRC_MINUS_DST SUBTRACT, MIN, MAX, DST_MINUS_SRC
// REV_SUBTRACT.
inline std::uint8_t d3d12_blend_op(std::uint32_t c) {
    static constexpr std::uint8_t op[5] = {1, 2, 4, 5, 3};
    return c < std::size(op) ? op[c] : 0;
}

// CB_BLENDn_CONTROL: COLOR_SRCBLEND [4:0], COLOR_COMB_FCN [7:5], COLOR_DESTBLEND [12:8], ALPHA_SRCBLEND [20:16],
// ALPHA_COMB_FCN [23:21], ALPHA_DESTBLEND [28:24], SEPARATE_ALPHA_BLEND [29], ENABLE [30], DISABLE_ROP3 [31]. Without
// SEPARATE_ALPHA_BLEND the alpha channel takes the colour fields. Empty for an enabled target this path cannot express.
inline std::optional<TargetBlend> d3d12_target_blend(std::uint32_t cb_blend, std::uint32_t write_mask) {
    TargetBlend t;
    t.write_mask = static_cast<std::uint8_t>(write_mask & 0xf);
    if (!(cb_blend >> 30 & 1)) return t;
    const bool separate = cb_blend >> 29 & 1;
    const std::uint32_t cs = cb_blend & 31, cf = cb_blend >> 5 & 7, cd = cb_blend >> 8 & 31;
    const std::uint32_t as = separate ? cb_blend >> 16 & 31 : cs, af = separate ? cb_blend >> 21 & 7 : cf,
                        ad = separate ? cb_blend >> 24 & 31 : cd;
    t.enable = true;
    t.src = d3d12_blend_factor(cs, false);
    t.dst = d3d12_blend_factor(cd, false);
    t.op = d3d12_blend_op(cf);
    t.src_alpha = d3d12_blend_factor(as, true);
    t.dst_alpha = d3d12_blend_factor(ad, true);
    t.op_alpha = d3d12_blend_op(af);
    if (!t.src || !t.dst || !t.op || !t.src_alpha || !t.dst_alpha || !t.op_alpha) return std::nullopt;
    return t;
}

// A DXGI_FORMAT with UINT or SINT channels. The colour block does not blend those (RDNA2 bypasses blending for integer
// targets), and D3D12 refuses a PSO that enables blending on one.
inline bool dxgi_integer_format(std::uint32_t f) {
    switch (f) {
        case 3: case 4: case 7: case 8: case 12: case 14: case 17: case 18: case 25: case 30: case 32: case 36:
        case 38: case 42: case 43: case 50: case 52: case 57: case 59: case 62: case 64:
            return true;
        default:
            return false;
    }
}

// The targets 0..count-1 of `r` in D3D12 terms, or empty when one of them cannot be expressed: the pipeline then keeps
// the defaults (blending off, every component written) for all of its targets, as before blob A was read. A target
// with an integer format (`formats`, DXGI values) keeps its write mask and has blending off, as on the console.
inline std::optional<std::array<TargetBlend, 8>> d3d12_blend(const BlendRegisters& r, unsigned count,
                                                             const std::array<std::uint8_t, 8>& formats = {}) {
    std::array<TargetBlend, 8> out{};
    for (unsigned i = 0; i < count && i < out.size(); ++i) {
        const std::uint32_t mask = r.target_mask >> (4 * i);
        const auto t = dxgi_integer_format(formats[i]) ? d3d12_target_blend(r.blend[i] & ~(1u << 30), mask)
                                                       : d3d12_target_blend(r.blend[i], mask);
        if (!t) return std::nullopt;
        out[i] = *t;
    }
    return out;
}

// The depth, stencil and raster state in D3D12 terms, with D3D12_COMPARISON_FUNC, D3D12_STENCIL_OP and D3D12_CULL_MODE
// values (d3d12.h). The defaults are the factory's state before blob A was read.
struct StencilFace {
    std::uint8_t fail = 1, depth_fail = 1, pass = 1;  // D3D12_STENCIL_OP_KEEP
    std::uint8_t func = 8;                            // D3D12_COMPARISON_FUNC_ALWAYS
};
struct DepthRaster {
    bool depth_enable = true;     // the factory also turns it off without a DSV
    bool depth_write = true;      // D3D12_DEPTH_WRITE_MASK_ALL
    std::uint8_t depth_func = 8;  // D3D12_COMPARISON_FUNC_ALWAYS
    bool stencil_enable = false;
    std::uint8_t read_mask = 0xff, write_mask = 0xff;
    StencilFace front, back;
    std::uint8_t cull = 1;        // D3D12_CULL_MODE_NONE
    bool front_ccw = false;
    bool depth_clip = true;
};

// An RDNA2 stencil op (V_02842C_STENCIL_*) as a D3D12_STENCIL_OP, or 0. The adding and subtracting ops take
// STENCILOPVAL as their operand, so they are INCR/DECR only with an operand of 1 (all 6,213 stencil records measured);
// REPLACE_TEST writes the reference value as D3D12's REPLACE does. ONES, REPLACE_OP and the logic ops have no D3D12 op.
inline std::uint8_t d3d12_stencil_op(std::uint32_t op, std::uint32_t operand) {
    switch (op) {
        case 0: return 1;                    // KEEP
        case 1: return 2;                    // ZERO
        case 3: return 3;                    // REPLACE_TEST -> REPLACE
        case 5: return operand == 1 ? 4 : 0; // ADD_CLAMP -> INCR_SAT
        case 6: return operand == 1 ? 5 : 0; // SUB_CLAMP -> DECR_SAT
        case 7: return 6;                    // INVERT
        case 8: return operand == 1 ? 7 : 0; // ADD_WRAP -> INCR
        case 9: return operand == 1 ? 8 : 0; // SUB_WRAP -> DECR
        default: return 0;
    }
}

// Empty for a state D3D12 cannot express here: clip space other than D3D's, only one of the near/far z clips off,
// a polygon mode, culling both faces, depth bounds, the colour-write-on-depth flags, a stencil op without a D3D12 op,
// or two-sided stencil (BACKFACE_ENABLE) with front and back masks that differ (D3D12_DEPTH_STENCIL_DESC has one
// pair).
//   PA_CL_CLIP_CNTL: DX_CLIP_SPACE_DEF [19], ZCLIP_NEAR_DISABLE [26], ZCLIP_FAR_DISABLE [27].
//   PA_SU_SC_MODE_CNTL: CULL_FRONT [0], CULL_BACK [1], FACE [2] (1: clockwise is front), POLY_MODE [4:3].
//   DB_DEPTH_CONTROL: STENCIL_ENABLE [0], Z_ENABLE [1], Z_WRITE_ENABLE [2], DEPTH_BOUNDS_ENABLE [3], ZFUNC [6:4],
//     BACKFACE_ENABLE [7], STENCILFUNC [10:8], STENCILFUNC_BF [22:20], bits 30 and 31 the colour-write flags.
//   DB_STENCIL_CONTROL: STENCILFAIL [3:0], STENCILZPASS [7:4], STENCILZFAIL [11:8], and the _BF ones at +12.
//   DB_STENCILREFMASK: STENCILMASK [15:8], STENCILWRITEMASK [23:16], STENCILOPVAL [31:24].
// RDNA2 compare functions run NEVER..ALWAYS as 0..7, D3D12's as 1..8.
inline std::optional<DepthRaster> d3d12_depth_raster(const DepthRasterRegisters& r) {
    DepthRaster s;
    const std::uint32_t clip = r.clip_cntl, mode = r.mode_cntl, d = r.depth_control;
    if (!(clip >> 19 & 1) || (clip >> 26 & 1) != (clip >> 27 & 1)) return std::nullopt;
    s.depth_clip = !(clip >> 26 & 1);
    if (mode >> 3 & 3 || (mode & 3) == 3) return std::nullopt;
    s.cull = (mode & 1) ? 2 : (mode & 2) ? 3 : 1;  // FRONT, BACK, NONE
    s.front_ccw = !(mode >> 2 & 1);
    if (d >> 3 & 1 || d >> 30) return std::nullopt;
    s.depth_enable = d >> 1 & 1;
    s.depth_write = d >> 2 & 1;
    s.depth_func = static_cast<std::uint8_t>((d >> 4 & 7) + 1);
    s.stencil_enable = d & 1;
    if (!s.stencil_enable) return s;
    // Without BACKFACE_ENABLE the back face uses the front registers and DB_STENCILREFMASK_BF is ignored, as the
    // back face's ops and function are below.
    const bool two_sided = d >> 7 & 1;
    if (two_sided && (r.stencil_ref_mask & 0x00ffff00) != (r.stencil_ref_mask_bf & 0x00ffff00)) return std::nullopt;
    s.read_mask = static_cast<std::uint8_t>(r.stencil_ref_mask >> 8);
    s.write_mask = static_cast<std::uint8_t>(r.stencil_ref_mask >> 16);
    const auto face = [&](std::uint32_t ops, std::uint32_t func, std::uint32_t operand) -> std::optional<StencilFace> {
        StencilFace f;
        f.fail = d3d12_stencil_op(ops & 15, operand);
        f.pass = d3d12_stencil_op(ops >> 4 & 15, operand);
        f.depth_fail = d3d12_stencil_op(ops >> 8 & 15, operand);
        f.func = static_cast<std::uint8_t>(func + 1);
        if (!f.fail || !f.pass || !f.depth_fail) return std::nullopt;
        return f;
    };
    const auto front = face(r.stencil_control, d >> 8 & 7, r.stencil_ref_mask >> 24);
    const auto back = two_sided ? face(r.stencil_control >> 12, d >> 20 & 7, r.stencil_ref_mask_bf >> 24) : front;
    if (!front || !back) return std::nullopt;
    s.front = *front;
    s.back = *back;
    return s;
}

using Digest = std::array<std::uint8_t, 16>;

// Every 16 bytes that follow a 4-aligned 0x43 word, without duplicates. A register number can also read 0x43, so
// all candidates are returned and the map decides which one names a container.
inline std::vector<Digest> record_digests(std::span<const std::uint8_t> records) {
    std::vector<Digest> out;
    std::set<Digest> seen;
    for (std::size_t at = 0; at + 4 + 16 <= records.size(); at += 4) {
        if (read_u32(records, at) != 0x43) continue;
        Digest d;
        std::memcpy(d.data(), records.data() + at + 4, 16);
        if (seen.insert(d).second) out.push_back(d);
    }
    return out;
}

inline std::string hex(const Digest& d) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    for (const std::uint8_t b : d) {
        s += kHex[b >> 4];
        s += kHex[b & 15];
    }
    return s;
}

inline std::optional<Digest> parse_digest(std::string_view text) {
    if (text.size() != 32) return std::nullopt;
    Digest d{};
    for (std::size_t i = 0; i < 32; ++i) {
        const char c = text[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return std::nullopt;
        d[i / 2] = static_cast<std::uint8_t>(d[i / 2] << 4 | v);
    }
    return d;
}

struct ShaderRef {
    Digest digest;
    std::string stage;  // the translator's stage: "vs", "ps", "gs", "ms", "cs"
    std::string path;   // the container, relative to the map file's folder, '/' separated
};

// native_hashes.txt, written by tools/native_pso_hashes.py next to the translator's index.json: a first line
// "# native_pso_hashes 1", then one "<32 hex digest>\t<stage>\t<relative container path>" line per digest.
class HashMap {
public:
    static constexpr std::string_view kHeader = "# native_pso_hashes 1";

    bool parse(std::string_view text, std::string* error = nullptr) {
        const auto fail = [&](std::string why) {
            if (error) *error = std::move(why);
            map_.clear();
            return false;
        };
        map_.clear();
        std::size_t line_no = 0;
        bool header = false;
        while (!text.empty()) {
            const std::size_t end = text.find('\n');
            std::string_view line = text.substr(0, end);
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
            ++line_no;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line_no == 1) {
                if (line != kHeader) return fail("native_hashes.txt: unknown header");
                header = true;
                continue;
            }
            if (line.empty() || line.front() == '#') continue;
            const std::size_t t1 = line.find('\t');
            const std::size_t t2 = t1 == std::string_view::npos ? t1 : line.find('\t', t1 + 1);
            if (t2 == std::string_view::npos || t2 + 1 >= line.size())
                return fail("native_hashes.txt line " + std::to_string(line_no) + ": expected three fields");
            const auto digest = parse_digest(line.substr(0, t1));
            const std::string_view stage = line.substr(t1 + 1, t2 - t1 - 1);
            if (!digest || stage.empty())
                return fail("native_hashes.txt line " + std::to_string(line_no) + ": bad digest or stage");
            const std::string_view path = line.substr(t2 + 1);
            const auto [it, inserted] = map_.try_emplace(*digest, ShaderRef{*digest, std::string(stage), std::string(path)});
            if (!inserted && (it->second.stage != stage || it->second.path != path))
                return fail("native_hashes.txt line " + std::to_string(line_no) + ": conflicting digest");
        }
        if (!header) return fail("native_hashes.txt is empty");
        return true;
    }

    const ShaderRef* find(const Digest& d) const {
        const auto it = map_.find(d);
        return it == map_.end() ? nullptr : &it->second;
    }

    std::size_t size() const { return map_.size(); }

private:
    std::map<Digest, ShaderRef> map_;
};

struct PlannedStage {
    std::size_t block = 0;
    const ShaderRef* shader = nullptr;
};

enum class PlanStatus {
    ok,
    malformed,    // a pair is malformed or blob B does not decode
    missing,      // a stage has no records, or its digest names no translated container
    ambiguous,    // a stage's records name two different containers
    unsupported,  // a stage kind this path does not build (hs, ds, cs), two of one kind, or no VS/MS
};

struct Plan {
    PlanStatus status = PlanStatus::malformed;
    FixedState state;
    std::vector<PlannedStage> stages;  // in block order
    std::size_t failed_block = 0;      // for missing/ambiguous/unsupported/malformed blocks
    const char* reason = "";           // why a description is malformed
    bool mesh = false;                 // an MS stage: the pipeline goes through a pipeline state stream

    const ShaderRef* stage(std::string_view name) const {
        for (const PlannedStage& s : stages)
            if (s.shader->stage == name) return s.shader;
        return nullptr;
    }
};

inline Plan plan(const GraphicsDesc& desc, const HashMap& map) {
    Plan p;
    if (pair_state(desc.state_a) == PairState::malformed) {
        p.reason = "blob A pair malformed";
        return p;
    }
    if (pair_state(desc.state_b) != PairState::present) {
        p.reason = "blob B pair absent or malformed";
        return p;
    }
    auto state = decode_state_b(bytes(desc.state_b), &p.reason);
    if (!state) return p;
    p.state = std::move(*state);
    if (pair_state(desc.state_a) == PairState::present) {
        p.state.blend = decode_state_a_blend(bytes(desc.state_a));
        p.state.depth_raster = decode_state_a_depth_raster(bytes(desc.state_a));
    }
    for (std::size_t k = 0; k < kStageBlocks; ++k) {
        const StageBlock& block = desc.stages[k];
        const PairState code = pair_state(block.code), records = pair_state(block.records);
        p.failed_block = k;
        if (code == PairState::malformed || records == PairState::malformed) {
            p.reason = "a stage pair malformed";
            return p;
        }
        if (code == PairState::absent && records == PairState::absent) continue;
        if (records != PairState::present) {  // code without records: nothing names its container
            p.status = PlanStatus::missing;
            return p;
        }
        const ShaderRef* found = nullptr;
        for (const Digest& d : record_digests(bytes(block.records))) {
            const ShaderRef* ref = map.find(d);
            if (!ref) continue;
            if (found && found != ref) {
                p.status = PlanStatus::ambiguous;
                p.failed_block = k;
                return p;
            }
            found = ref;
        }
        p.failed_block = k;
        if (!found) {
            p.status = PlanStatus::missing;
            return p;
        }
        const std::string& s = found->stage;
        if (!(s == "vs" || s == "ps" || s == "gs" || s == "ms") || p.stage(s)) {
            p.status = PlanStatus::unsupported;
            return p;
        }
        p.stages.push_back({k, found});
    }
    const bool vs = p.stage("vs"), ms = p.stage("ms");
    if (vs == ms || (ms && p.stage("gs"))) {
        p.status = PlanStatus::unsupported;
        return p;
    }
    p.mesh = ms;
    p.status = PlanStatus::ok;
    return p;
}

}  // namespace leonida::native_pso
