#pragma once

#include "../src/gfx10_tiling.h"

// AMD addrlib equations and license: see xg_xs_THIRD_PARTY.md.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <dxgiformat.h>
#include <utility>
#include <vector>

namespace rt::xg_xs {

// D3D12_RESOURCE_DESC ABI, with the Xbox layout encoding in the layout field.
struct ResourceDesc {
    uint32_t dimension;
    uint64_t alignment, width;
    uint32_t height;
    uint16_t depth_or_array, mips;
    uint32_t format, samples, quality, layout, flags;
};
static_assert(sizeof(ResourceDesc) == 0x38 && offsetof(ResourceDesc, layout) == 0x2c);

constexpr uint32_t kSwizzled = 0x100;
constexpr uint32_t kLinear = 0, k4kbS = 5, k64kbS = 9, k64kbD = 10, k64kbZX = 24, k64kbRX = 27;
constexpr uint32_t kTexture1D = 2, kTexture2D = 3, kTexture3D = 4;
constexpr uint32_t kTextureDisplay = 0x08000000;
constexpr uint32_t kRenderTarget = 1, kDepthStencil = 2;
constexpr unsigned kMaxMips = 15;
constexpr uint32_t kTileBytes = 65536;

inline uint32_t select_mode(const ResourceDesc& desc) {
    if (desc.layout == 1) return kLinear;
    if ((desc.layout & ~0x1fu) == kSwizzled) return desc.layout & 0x1f;
    if (desc.flags & kDepthStencil) return k64kbZX;
    if (desc.flags & kTextureDisplay) return k64kbD;
    if (desc.flags & kRenderTarget) return k64kbRX;
    return desc.dimension >= kTexture2D ? k4kbS : kLinear;
}

struct Format {
    uint32_t bytes = 0;
    uint32_t side = 1;
};

inline Format format_info(uint32_t f) {
    // DXGI's typed and typeless variants occupy consecutive values within a storage family.
    if (f >= DXGI_FORMAT_R32G32B32A32_TYPELESS && f <= DXGI_FORMAT_R32G32B32A32_SINT) return {16, 1};
    if (f >= DXGI_FORMAT_R16G16B16A16_TYPELESS && f <= DXGI_FORMAT_R32G32_SINT) return {8, 1};
    if (f >= DXGI_FORMAT_R10G10B10A2_TYPELESS && f <= DXGI_FORMAT_R32_SINT) return {4, 1};
    if (f >= DXGI_FORMAT_R8G8_TYPELESS && f <= DXGI_FORMAT_R16_SINT) return {2, 1};
    if (f >= DXGI_FORMAT_R8_TYPELESS && f <= DXGI_FORMAT_A8_UNORM) return {1, 1};
    if (f == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) return {4, 1};
    if (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC1_UNORM_SRGB) return {8, 4};
    if (f >= DXGI_FORMAT_BC2_TYPELESS && f <= DXGI_FORMAT_BC3_UNORM_SRGB) return {16, 4};
    if (f >= DXGI_FORMAT_BC4_TYPELESS && f <= DXGI_FORMAT_BC4_SNORM) return {8, 4};
    if (f >= DXGI_FORMAT_BC5_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) return {16, 4};
    if (f == DXGI_FORMAT_B5G6R5_UNORM || f == DXGI_FORMAT_B5G5R5A1_UNORM ||
        f == DXGI_FORMAT_B4G4R4A4_UNORM) return {2, 1};
    if (f >= DXGI_FORMAT_B8G8R8A8_UNORM && f <= DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) return {4, 1};
    if (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB) return {16, 4};
    return {};
}

// Xbox Series slot 2 output. The unused planes are zero-filled.
struct MipLayout {
    uint64_t size, offset, slice_bytes;
    uint32_t pitch_texels, pitch_bytes, alignment;
    uint32_t padded_width, padded_height, padded_depth;
    uint32_t width_elements, height_elements, width_texels, height_texels;
    uint32_t depth, samples, swizzle, reserved0;
    uint64_t bank_rotation[2];
    uint32_t tile_depth, in_tail, tail_position, reserved1;
};
struct PlaneLayout {
    uint32_t usage, reserved0;
    uint64_t size, slice_bytes, offset, alignment;
    uint32_t element_bytes, standard_mips, tail_mips, reserved1;
    std::array<MipLayout, kMaxMips> mips;
};
template <size_t PlaneCount>
struct TextureLayout {
    uint64_t size, alignment;
    uint32_t mips, planes;
    std::array<PlaneLayout, PlaneCount> plane;
    uint32_t dimension, reserved;
};
using ResourceLayout = TextureLayout<4>;  // Slot 2 output ABI.
static_assert(sizeof(MipLayout) == 0x70 && offsetof(MipLayout, bank_rotation) == 0x50);
static_assert(sizeof(PlaneLayout) == 0x6c8 && offsetof(PlaneLayout, mips) == 0x38);
static_assert(sizeof(ResourceLayout) == 0x1b40 && offsetof(ResourceLayout, dimension) == 0x1b38);

struct Texture {
    // build_texture supports single-plane formats. Expand to the ABI layout only in get_layout.
    TextureLayout<1> layout{};
    // GFX10 thick blocks interleave mip chains by slabs of tile_depth slices.
    struct VolumeMip { uint64_t macro_offset = 0; uint32_t tail_x = 0, tail_y = 0; };
    std::array<VolumeMip, kMaxMips> volume_mips{};
    uint64_t slab_bytes = 0;
    Format format;
    uint32_t mode = 0, tile_width = 0, tile_height = 0, tile_depth = 1, array = 0;
};

inline uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Thin surface allocation from AMD addrlib's GFX10 rules. The logical size
// contracts by floor division, while physical pitch/height use ShiftCeil on
// the base extent in elements. A slice stores the tail block, then its
// unpacked mips from smallest to largest.
//
// A render-target or depth-stencil flag only matters when the layout word
// leaves the mode to XG (select_mode then picks a Z/R mode this model lacks);
// with an explicit mode the layout is that mode's, as on the console.
//
// A linear 3D texture is thin (addrlib's HwlComputeSurfaceInfoLinear): every
// depth slice holds the whole mip chain, rows aligned to 256 bytes, so the
// slices are plane.slice_bytes apart and a mip's size spans all its slices.
inline bool build_texture(const ResourceDesc& desc, Texture& texture) {
    // Exposure readback uses the explicit linear Series layout, including the
    // console flag set by its render-target descriptor builder.
    const bool linear_readback_target = (desc.flags & ~0x300000u) == kRenderTarget &&
        desc.dimension == kTexture2D && desc.format == DXGI_FORMAT_R32G32B32A32_FLOAT &&
        desc.layout == (kSwizzled | kLinear) && desc.width == 1 && desc.height == 1 &&
        desc.depth_or_array == 1 && desc.mips == 1;
    if (desc.dimension < kTexture1D || desc.dimension > kTexture3D ||
        desc.width == 0 || desc.width > 16384 || desc.height == 0 || desc.height > 16384 ||
        (desc.dimension == kTexture1D && desc.height != 1) ||
        desc.depth_or_array == 0 || desc.depth_or_array > 2048 || desc.samples != 1 || desc.quality ||
        ((desc.flags & (kRenderTarget | kDepthStencil)) && !linear_readback_target) ||
        (desc.layout > 1 && (desc.layout & ~0x1fu) != kSwizzled))
        return false;

    const Format format = format_info(desc.format);
    if (!format.bytes || (desc.dimension == kTexture1D && format.side != 1)) return false;
    const unsigned full_mips = std::bit_width(std::max({uint32_t(desc.width), desc.height,
        desc.dimension == kTexture3D ? unsigned(desc.depth_or_array) : 1u}));
    const unsigned mips = desc.mips ? desc.mips : full_mips;
    if (mips > full_mips) return false;

    const uint32_t mode = select_mode(desc);
    unsigned tile_bits;
    switch (mode) {
    case kLinear: tile_bits = 8; break;
    case k4kbS: tile_bits = 12; break;
    case k64kbS: case k64kbD: tile_bits = 16; break;
    default: return false;
    }
    const bool volume = desc.dimension == kTexture3D;
    if (volume && mode != k4kbS && mode != kLinear) return false;
    // Linear 3D is measured with one mip only (256-byte rows, slice after slice). Whether a linear 3D chain is laid
    // out slice-major or mip-major is not measured, and the two put mips 1+ at different offsets: refused, as before.
    if (volume && mode == kLinear && mips > 1) return false;
    const bool thick = volume && mode != kLinear;  // GFX10 3D S modes: blocks span tile_depth slices
    if (thick && (mips != 1 || format.side != 1)) {
        static const bool enabled = [] {
            const char* value = std::getenv("LEONIDA_VOLUME_MIPS");
            return !value || std::strcmp(value, "0") != 0;
        }();
        if (!enabled) return false;
    }
    if (mode != kLinear && desc.dimension == kTexture1D) return false;

    const uint32_t alignment = 1u << tile_bits;
    const unsigned element_bits = std::countr_zero(format.bytes);
    const unsigned tile_elements = tile_bits - element_bits;
    uint32_t tw = mode == kLinear ? alignment / format.bytes : 1u << ((tile_elements + 1) / 2);
    uint32_t th = mode == kLinear ? 1 : 1u << (tile_elements / 2);
    const uint32_t base_width = align_up(uint32_t(desc.width), format.side) / format.side;
    const uint32_t base_height = align_up(desc.height, format.side) / format.side;

    texture = {};
    texture.format = format;
    texture.mode = mode;
    texture.tile_width = tw;
    texture.tile_height = th;
    texture.array = volume ? 1u : desc.depth_or_array;
    if (thick) {
        // Dimensions in elements (4x4 blocks for BC formats).
        constexpr uint32_t blocks[5][3] = {{16, 16, 16}, {8, 16, 16}, {8, 16, 8}, {8, 8, 8}, {4, 8, 8}};
        texture.tile_width = tw = blocks[element_bits][0];
        texture.tile_height = th = blocks[element_bits][1];
        texture.tile_depth = blocks[element_bits][2];
    }

    auto& layout = texture.layout;
    layout.alignment = alignment;
    layout.dimension = desc.dimension;
    layout.mips = mips;
    layout.planes = 1;
    auto& plane = layout.plane[0];
    plane.usage = 1; // data plane
    plane.alignment = alignment;
    plane.element_bytes = format.bytes;
    plane.standard_mips = mips;
    // AMD addrlib Gfx10Lib::ComputeSurfaceInfoMacroTiled and Lib::GetMipTailDim.
    // A 4 KiB thick block has five tail levels and halves height. BC extents are in blocks.
    const unsigned max_tail = thick ? 5u : tile_bits - 4;
    uint32_t tail_w = thick ? tw : tw / 2, tail_h = thick ? th / 2 : th;
    constexpr uint32_t micro3d[5][3] = {{8,4,8}, {4,4,8}, {4,4,4}, {4,2,4}, {2,2,4}};
    const uint32_t micro_w = thick ? micro3d[element_bits][0] : 1u << ((8 - element_bits + 1) / 2);
    const uint32_t micro_h = thick ? micro3d[element_bits][1] : 256 / (format.bytes * micro_w);
    uint32_t tail_depth = 0;

    uint64_t chain_bytes = 0;
    for (unsigned i = 0; i < mips; ++i) {
        auto& mip = plane.mips[i];
        mip.width_texels = std::max(1u, uint32_t(desc.width) >> i);
        mip.height_texels = std::max(1u, desc.height >> i);
        mip.width_elements = align_up(mip.width_texels, format.side) / format.side;
        mip.height_elements = align_up(mip.height_texels, format.side) / format.side;
        const uint32_t round = (1u << i) - 1;
        const uint32_t physical_w = (base_width + round) >> i;
        const uint32_t physical_h = (base_height + round) >> i;
        if (mode != kLinear && mips > 1 && plane.standard_mips == mips &&
            mips - i <= max_tail && physical_w <= tail_w && physical_h <= tail_h) {
            plane.standard_mips = i;
            plane.tail_mips = mips - i;
            chain_bytes = alignment;
            tail_depth = (uint32_t(desc.depth_or_array) + round) >> i;
        }

        mip.depth = volume ? std::max(1u, uint32_t(desc.depth_or_array) >> i) : desc.depth_or_array;
        mip.padded_depth = volume ? align_up((uint32_t(desc.depth_or_array) + round) >> i, texture.tile_depth) : mip.depth;
        mip.samples = 1;
        mip.tile_depth = texture.tile_depth;
        mip.in_tail = i >= plane.standard_mips;
        if (mip.in_tail) {
            const unsigned index = max_tail - 1 - (i - plane.standard_mips);
            mip.offset = index > 6 ? 16u << index : index * 256u;
            if (thick) {
                texture.volume_mips[i].tail_x = ((index >> 1) & 1u) * micro_w;
                texture.volume_mips[i].tail_y = ((index & 1u) | ((index >> 1) & 2u)) * micro_h;
                mip.offset *= align_up(tail_depth, micro3d[element_bits][2]) / micro3d[element_bits][2];
            }
            mip.alignment = 256;
            mip.swizzle = mode == k64kbD ? 2 : 1; // 256 B microtile of the same family
            mip.padded_width = tail_w;
            mip.padded_height = tail_h;
            tail_w = std::max(tail_w / 2, micro_w);
            tail_h = std::max(tail_h / 2, micro_h);
        } else {
            mip.alignment = alignment;
            mip.swizzle = mode;
            mip.padded_width = align_up(physical_w, tw);
            mip.padded_height = align_up(physical_h, th);
        }
        mip.pitch_texels = mip.padded_width * format.side;
        mip.pitch_bytes = mip.padded_width * format.bytes;
        mip.slice_bytes = uint64_t(mip.pitch_bytes) * mip.padded_height;
        mip.size = mip.slice_bytes * (volume ? mip.padded_depth : 1u);
    }
    if (thick) {
        uint64_t macro = chain_bytes;
        chain_bytes *= align_up(tail_depth, texture.tile_depth) / texture.tile_depth;
        for (unsigned i = plane.standard_mips; i > 0; --i) {
            auto& mip = plane.mips[i - 1];
            texture.volume_mips[i - 1].macro_offset = macro;
            mip.offset = chain_bytes;
            macro += mip.slice_bytes * texture.tile_depth;
            chain_bytes += mip.size;
        }
        texture.slab_bytes = macro;
        plane.size = layout.size = macro * (align_up(desc.depth_or_array, texture.tile_depth) / texture.tile_depth);
        plane.slice_bytes = mips == 1 ? plane.size : macro / texture.tile_depth;
        return true;
    }
    for (unsigned i = plane.standard_mips; i > 0; --i) {
        auto& mip = plane.mips[i - 1];
        mip.offset = chain_bytes;
        chain_bytes += mip.slice_bytes;  // one slice's share: a linear 3D mip's size spans every depth slice
    }
    plane.slice_bytes = chain_bytes;
    plane.size = layout.size = chain_bytes * (volume ? desc.depth_or_array : texture.array);
    return true;
}

// The bytes from the texture's base that hold the mips from `first` on (the whole texture for 0). Only a 2D texture of
// one slice stores its chain from the smallest mips to the largest, so only there is a part of it a prefix.
inline uint64_t mip_chain_bytes(const Texture& t, unsigned first) {
    const auto& plane = t.layout.plane[0];
    if (!first || first >= t.layout.mips || t.layout.dimension != kTexture2D || t.array != 1) return t.layout.size;
    if (first >= plane.standard_mips) return t.layout.alignment;  // the tail is one block
    return plane.mips[first].offset + plane.mips[first].size;
}

// The first mip that is held, with every smaller one, by the `bytes` readable bytes from the texture's base; the mip
// count when not even the smallest is.
inline unsigned first_mip_within(const Texture& t, uint64_t bytes) {
    for (unsigned m = 0; m < t.layout.mips; ++m)
        if (mip_chain_bytes(t, m) <= bytes) return m;
    return t.layout.mips;
}

// These RB+ equation rows come directly from AMD addrlib. The other two
// non-XOR modes already have equations in Leonida's GFX10 detiler.
inline constexpr int k64kbStandard[5][16] = {
    {1, 2, 3, 4, -1, -2, -3, -4, -5, 5, -6, 6, -7, 7, -8, 8},
    {0, 1, 2, 3, -1, -2, -3, 4, -4, 5, -5, 6, -6, 7, -7, 8},
    {0, 0, 1, 2, -1, -2, -3, 3, -4, 4, -5, 5, -6, 6, -7, 7},
    {0, 0, 0, 1, -1, -2, 2, 3, -3, 4, -4, 5, -5, 6, -6, 7},
    {0, 0, 0, 0, -1, -2, 1, 2, -3, 3, -4, 4, -5, 5, -6, 6},
};
inline constexpr int k4kbStandard3D[5][12] = {
    {1, 2, 17, -1, 18, -2, 3, 19, -3, 4, 20, -4},
    {0, 1, 17, -1, 18, -2, 2, 19, -3, 3, 20, -4},
    {0, 0, 1, -1, 17, -2, 2, 18, -3, 3, 19, -4},
    {0, 0, 0, 1, 17, -1, 2, 18, -2, 3, 19, -3},
    {0, 0, 0, 0, 17, -1, 1, 18, -2, 2, 19, -3},
};

struct ElementAddress {
    explicit ElementAddress(const Texture& texture) : texture(texture) {
        const unsigned i = std::countr_zero(texture.format.bytes);
        switch (texture.mode) {
        case k4kbS:
            pattern = texture.tile_depth == 1 ? leonida::gfx10::tiling_detail::patterns_4k[i] : k4kbStandard3D[i];
            break;
        case k64kbS: pattern = k64kbStandard[i]; break;
        case k64kbD: pattern = leonida::gfx10::tiling_detail::patterns_64k[i]; break;
        default: return;
        }
        bits = std::countr_zero(texture.layout.alignment);
        const uint32_t dims[] = {texture.tile_width, texture.tile_height, texture.tile_depth};
        for (unsigned b = 0; b < bits; ++b) {
            if (!pattern[b]) continue;
            const auto [axis, shift] = coordinate(pattern[b]);
            for (uint32_t c = 0; c < dims[axis]; ++c)
                axis_offset[axis][c] |= ((c >> shift) & 1) << b;
        }
    }

    std::array<uint32_t, 3> tail_origin(const MipLayout& mip) const {
        std::array<uint32_t, 3> coords{};
        if (mip.in_tail && texture.slab_bytes) {
            const auto& sub = texture.volume_mips[&mip - texture.layout.plane[0].mips.data()];
            return {sub.tail_x, sub.tail_y, 0};
        }
        if (mip.in_tail && pattern) {
            for (unsigned b = 0; b < bits; ++b) {
                if (!pattern[b] || !(mip.offset & (1u << b))) continue;
                const auto [axis, shift] = coordinate(pattern[b]);
                coords[axis] |= 1u << shift;
            }
        }
        return coords;
    }

    uint64_t offset(const MipLayout& mip, const std::array<uint32_t, 3>& tail,
                    uint32_t x, uint32_t y, uint32_t z) const {
        if (texture.slab_bytes) return volume_row(mip, tail, y, z) + column(mip, tail, x);
        const uint64_t slice = z * texture.layout.plane[0].slice_bytes;
        if (texture.mode == kLinear) return slice + mip.offset + uint64_t(y) * mip.pitch_bytes + x * texture.format.bytes;

        const uint32_t w = texture.tile_width, h = texture.tile_height;
        if (mip.in_tail) {
            return slice + axis_offset[0][x + tail[0]] + axis_offset[1][y + tail[1]];
        }

        // Whole blocks are row-major, then the equation selects a byte inside it.
        const uint64_t block = uint64_t(y / h) * (mip.padded_width / w) + x / w;
        return slice + mip.offset + (block << bits) + axis_offset[0][x & (w - 1)] + axis_offset[1][y & (h - 1)];
    }

    // offset() split into a part that depends only on the row (y, z) and one that depends only on x, so a whole
    // row is addressed with one table lookup per element: offset(mip, tail, x, y, z) == row(mip, tail, y, z) +
    // column(mip, tail, x).
    uint64_t row(const MipLayout& mip, const std::array<uint32_t, 3>& tail, uint32_t y, uint32_t z) const {
        if (texture.slab_bytes) return volume_row(mip, tail, y, z);
        const uint64_t slice = z * texture.layout.plane[0].slice_bytes;
        if (texture.mode == kLinear) return slice + mip.offset + uint64_t(y) * mip.pitch_bytes;
        if (mip.in_tail) return slice + axis_offset[1][y + tail[1]];
        const uint32_t w = texture.tile_width, h = texture.tile_height;
        const uint64_t blocks = uint64_t(y / h) * (mip.padded_width / w);
        return slice + mip.offset + (blocks << bits) + axis_offset[1][y & (h - 1)];
    }
    uint64_t column(const MipLayout& mip, const std::array<uint32_t, 3>& tail, uint32_t x) const {
        if (texture.mode == kLinear) return uint64_t(x) * texture.format.bytes;
        if (mip.in_tail) return axis_offset[0][x + tail[0]];
        const uint32_t w = texture.tile_width;
        return (uint64_t(x / w) << bits) + axis_offset[0][x & (w - 1)];
    }

private:
    uint64_t volume_row(const MipLayout& mip, const std::array<uint32_t, 3>& tail, uint32_t y, uint32_t z) const {
        const auto& sub = texture.volume_mips[&mip - texture.layout.plane[0].mips.data()];
        const uint64_t block_row = uint64_t(y / texture.tile_height) * (mip.padded_width / texture.tile_width);
        return (z / texture.tile_depth) * texture.slab_bytes + sub.macro_offset + (block_row << bits) +
               axis_offset[1][(y + tail[1]) % texture.tile_height] + axis_offset[2][z % texture.tile_depth];
    }

    static std::pair<unsigned, unsigned> coordinate(int bit) {
        // Positive 1..16 are X, negative are Y, 17 and above are Z.
        if (bit < 0) return {1, unsigned(-bit - 1)};
        if (bit >= 17) return {2, unsigned(bit - 17)};
        return {0, unsigned(bit - 1)};
    }

    const Texture& texture;
    const int* pattern = nullptr;
    unsigned bits = 0;
    uint32_t axis_offset[3][256]{};
};

// The element copy behind XG's CopyIntoSubresource / CopyFromSubresource (xg_xs.cpp) and the D3D12 layer's fill of
// textures the title keeps in its own memory (d3d12_vulkan_device.cpp, TitleTextures): one mip of one array slice
// between the console layout (`tiled`, from the texture's base) and linear rows of elements `row_pitch` bytes apart,
// 3D slices `slice_pitch` apart. Into: linear -> tiled. The callers check the arguments and the buffer sizes
// (`tiled` holds layout.size bytes; linear holds the mip's rows). `columns` is scratch.
template<bool Into, class Byte>
inline void copy_elements(const Texture& t, uint32_t level, uint32_t slice, const Byte* source, Byte* destination,
                          uint32_t row_pitch, uint32_t slice_pitch, std::vector<uint64_t>& columns) {
    const auto& mip = t.layout.plane[0].mips[level];
    const uint32_t bytes = t.format.bytes;
    const uint32_t row_bytes = mip.width_elements * bytes;
    const uint32_t depth = t.layout.dimension == kTexture3D ? mip.depth : 1;
    const ElementAddress address(t);
    const auto tail = address.tail_origin(mip);
    columns.resize(mip.width_elements);
    for (uint32_t x = 0; x < mip.width_elements; ++x) columns[x] = address.column(mip, tail, x);
    for (uint32_t z = 0; z < depth; ++z) {
        for (uint32_t y = 0; y < mip.height_elements; ++y) {
            const uint64_t linear = uint64_t(z) * slice_pitch + uint64_t(y) * row_pitch;
            const uint64_t tiled = address.row(mip, tail, y, slice + z);
            if (t.mode == kLinear) {
                std::memcpy(destination + (Into ? tiled : linear), source + (Into ? linear : tiled), row_bytes);
                continue;
            }
            for (uint32_t x = 0; x < mip.width_elements; ++x) {
                const uint64_t at = tiled + columns[x];
                const uint64_t to = linear + uint64_t(x) * bytes;
                std::memcpy(destination + (Into ? at : to), source + (Into ? to : at), bytes);
            }
        }
    }
}

}  // namespace rt::xg_xs
