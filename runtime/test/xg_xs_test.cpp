// Checks the registered xg_xs.dll shim (runtime/shims/xg_xs.cpp) and the two texture layout models behind it
// (runtime/shims/xg_xs_layout.h and runtime/src/console_texture_layout.h). Synthetic textures only; no game files.
//
// Both exports are resolved through rt::shims::find, so this is the table the runtime installs. The cases of the
// second model and their expected numbers are those of the project it is ported from (Marvisak/VIctory,
// GPL-3.0-or-later, translated from Rust to C++ and changed for Leonida,
// crates/victory-formats/src/texture.rs and crates/victory-hle/src/modules/xg.rs tests at d2a4bae1f): hand-checked
// addresses, mip tail placement, a two-plane depth-stencil, and a bijection plus round trip over every element.
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <cstring>
#include <vector>

#include "../src/console_texture_layout.h"
#include "shims.h"
#include "xg_xs_layout.h"
#include "../src/title_texture_memory.h"
#include "../src/title_texture_refresh.h"

namespace {

using namespace rt::xg_xs;
namespace ct = leonida::console_texture;
using CreateFn = HRESULT(__stdcall*)(const void*, void**);
using SuggestFn = HRESULT(__stdcall*)(const void*, uint32_t, uint32_t*);
using RefFn = ULONG(__stdcall*)(void*);
using LayoutFn = HRESULT(__stdcall*)(void*, ResourceLayout*);
using SizeFn = uint64_t(__stdcall*)(void*);
using OffsetFn = uint64_t(__stdcall*)(void*, uint32_t, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t*);
using IntoFn = HRESULT(__stdcall*)(void*, void*, uint32_t, uint32_t, const void*, uint32_t, uint32_t);
using FromFn = HRESULT(__stdcall*)(void*, void*, uint32_t, uint32_t, uint32_t, const void*, uint32_t, uint32_t);
struct Tiling { uint32_t width; uint16_t height, depth; uint32_t start; };
using TilingFn = HRESULT(__stdcall*)(void*, uint32_t*, uint32_t*, uint32_t*, uint32_t*, uint32_t, Tiling*);

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

template<class Fn>
Fn slot(void* computer, unsigned index) {
    return reinterpret_cast<Fn>((*static_cast<void* const* const*>(computer))[index]);
}

bool make(CreateFn create, const ResourceDesc& d, void*& computer, const char* what) {
    const bool success = create(&d, &computer) == S_OK && computer;
    check(success, what);
    return success;
}

void check_addresses(const Texture& t) {
    const ElementAddress address(t);
    std::vector<bool> seen(t.layout.size / t.format.bytes);
    for (uint32_t slice = 0; slice < t.array; ++slice) {
        for (uint32_t level = 0; level < t.layout.mips; ++level) {
            const auto& mip = t.layout.plane[0].mips[level];
            const auto tail = address.tail_origin(mip);
            for (uint32_t y = 0; y < mip.height_elements; ++y) {
                for (uint32_t x = 0; x < mip.width_elements; ++x) {
                    const auto off = address.offset(mip, tail, x, y, slice);
                    if (off % t.format.bytes || off / t.format.bytes >= seen.size() || seen[off / t.format.bytes]) {
                        check(false, "every mip/array element has a distinct aligned address inside the resource");
                        return;
                    }
                    seen[off / t.format.bytes] = true;
                }
            }
        }
    }
    check(true, "every mip/array element has a distinct aligned address inside the resource");
}

// A fill of the smaller mips only (d3d12_vulkan_device.cpp, partial_mips) reads mip_chain_bytes(t, first) bytes from the
// base: those hold every element of the mips from `first` on, and the next larger mip is not among them.
void check_mip_prefix(uint32_t format, uint32_t layout, uint64_t width, uint32_t height, uint16_t mips) {
    ResourceDesc d{};
    d.dimension = kTexture2D;
    d.width = width;
    d.height = height;
    d.depth_or_array = 1;
    d.mips = mips;
    d.format = format;
    d.samples = 1;
    d.layout = layout;
    Texture t;
    check(build_texture(d, t), "mip prefix case lays out");
    const auto& plane = t.layout.plane[0];
    std::vector<uint8_t> whole(size_t(t.layout.size));
    for (size_t i = 0; i < whole.size(); ++i) whole[i] = uint8_t(i * 31 + (i >> 8) * 7);
    std::vector<uint64_t> columns;
    bool same = true, ordered = true, found = true;
    for (unsigned first = 0; first < t.layout.mips; ++first) {
        const uint64_t bytes = mip_chain_bytes(t, first);
        // Exactly the prefix, so that a read past it is a heap error under a checked allocator and a difference here.
        std::vector<uint8_t> part(whole.size(), 0xee);
        std::copy_n(whole.begin(), size_t(bytes), part.begin());
        for (unsigned level = first; level < t.layout.mips; ++level) {
            const auto& mip = plane.mips[level];
            const uint32_t pitch = mip.width_elements * t.format.bytes;
            std::vector<uint8_t> a(size_t(pitch) * mip.height_elements), b(a.size());
            copy_elements<false>(t, level, 0, whole.data(), a.data(), pitch, pitch * mip.height_elements, columns);
            copy_elements<false>(t, level, 0, part.data(), b.data(), pitch, pitch * mip.height_elements, columns);
            same &= a == b;
        }
        const bool in_tail = first > plane.standard_mips;
        found &= first_mip_within(t, bytes) == (in_tail ? plane.standard_mips : first);
        found &= first_mip_within(t, bytes - 1) == (first >= plane.standard_mips ? t.layout.mips
                                                    : first + 1 < t.layout.mips  ? first + 1
                                                                                 : t.layout.mips);
        if (first && first <= plane.standard_mips) ordered &= bytes < mip_chain_bytes(t, first - 1);
    }
    check(same, "the prefix of a mip holds it and every smaller one");
    check(ordered && mip_chain_bytes(t, 0) == t.layout.size, "a larger mip needs a longer prefix, mip 0 the whole texture");
    check(found, "the first mip within a readable prefix is the largest one it holds");
    check(first_mip_within(t, 0) == t.layout.mips, "nothing readable: no mip");
}

// copy_elements (row + column addressing) reads every element where offset() says it is.
void check_detile(const Texture& t) {
    const ElementAddress address(t);
    std::vector<uint8_t> tiled(t.layout.size);
    for (size_t i = 0; i < tiled.size(); ++i) tiled[i] = uint8_t(i * 2654435761u >> 13);
    std::vector<uint64_t> columns;
    for (uint32_t slice = 0; slice < t.array; ++slice) {
        for (uint32_t level = 0; level < t.layout.mips; ++level) {
            const auto& mip = t.layout.plane[0].mips[level];
            const uint32_t pitch = mip.width_elements * t.format.bytes + 4;
            std::vector<uint8_t> linear(size_t(pitch) * mip.height_elements);
            copy_elements<false>(t, level, slice, tiled.data(), linear.data(), pitch, 0, columns);
            const auto tail = address.tail_origin(mip);
            for (uint32_t y = 0; y < mip.height_elements; ++y) {
                for (uint32_t x = 0; x < mip.width_elements; ++x) {
                    const uint64_t at = address.offset(mip, tail, x, y, slice);
                    if (std::memcmp(linear.data() + size_t(y) * pitch + size_t(x) * t.format.bytes, tiled.data() + at,
                                    t.format.bytes)) {
                        check(false, "copy_elements reads each element at offset()");
                        return;
                    }
                }
            }
        }
    }
    check(true, "copy_elements reads each element at offset()");
}

bool all_bytes(const uint8_t* p, size_t n, uint8_t value) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] != value) return false;
    return true;
}

ct::Desc desc2d(uint32_t w, uint32_t h, uint32_t mips, uint32_t bpe, uint32_t swizzle) {
    ct::Desc d;
    d.width = w;
    d.height = h;
    d.mips = mips;
    d.element_bytes = bpe;
    d.swizzle = swizzle;
    return d;
}

uint64_t offset_of(const ct::Layout& l, uint32_t mip, uint32_t x, uint32_t y, uint32_t z) {
    uint64_t at = ~uint64_t{0};
    return l.offset(mip, x, y, z, at) == ct::Error::ok ? at : ~uint64_t{0};
}

void test_layout_model() {
    // Sizes past the hardware's 16384 texels per side are refused instead of overflowing.
    for (uint32_t s : {ct::kLinear, ct::kSw64kbDX}) {
        const auto largest = ct::Layout::make(desc2d(ct::kMaxExtent, ct::kMaxExtent, 15, 16, s));
        check(largest && largest->size >= uint64_t{ct::kMaxExtent} * ct::kMaxExtent * 16, "largest texture lays out");
        check(!ct::Layout::make(desc2d(ct::kMaxExtent + 1, 1, 1, 16, s)), "width past 16384 is refused");
        check(!ct::Layout::make(desc2d(1, 0xffffffffu, 1, 16, s)), "height past 16384 is refused");
        auto deep = desc2d(1, 1, 1, 16, s);
        deep.depth = 0xffffffffu;
        check(!ct::Layout::make(deep), "depth past 16384 is refused");
    }
    // Blocks hold their size in elements, and the pattern covers exactly the block's coordinates.
    for (uint32_t log2 : {8u, 12u, 16u}) {
        const uint32_t s = log2 == 8 ? ct::kSw256bS : log2 == 12 ? ct::kSw4kbS : ct::kSw64kbD;
        for (uint32_t e = 0; e < 5; ++e) {
            const auto l = ct::Layout::make(desc2d(1, 1, 1, 1u << e, s));
            check(l && (uint64_t{l->block[0]} * l->block[1] * l->block[2] << e) == (uint64_t{1} << log2),
                  "block holds its size in elements");
            check(l && l->pattern()[0].size() == l->block[0] && l->pattern()[1].size() == l->block[1],
                  "pattern covers the block");
        }
    }
    {
        const auto l = ct::Layout::make(desc2d(64, 64, 1, 16, ct::kSw64kbD));
        check(l && l->block[0] == 64 && l->block[1] == 64 && l->block[2] == 1, "64 KiB, 16-byte elements: 64x64");
        check(l && l->size == 65536 && l->alignment == 65536, "64 KiB, 16-byte elements: one block");
    }
    // Hand-checked: in a 64 KiB D block of 4-byte elements, x bits go to address bits 2, 3, 6, 9, 11, 13, 15 and y
    // bits to 4, 5, 7, 8, 10, 12, 14; a 4 KiB S block of 16-byte elements is 16x16 with y first.
    {
        const auto l = ct::Layout::make(desc2d(128, 128, 1, 4, ct::kSw64kbD));
        check(l && offset_of(*l, 0, 1, 0, 0) == (1u << 2), "64KB_D x=1");
        check(l && offset_of(*l, 0, 4, 0, 0) == (1u << 6), "64KB_D x=4");
        check(l && offset_of(*l, 0, 0, 4, 0) == (1u << 7), "64KB_D y=4");
        check(l && offset_of(*l, 0, 0, 8, 0) == (1u << 8), "64KB_D y=8");
        check(l && offset_of(*l, 0, 64, 64, 0) == ((1u << 15) | (1u << 14)), "64KB_D (64,64)");
        const auto s = ct::Layout::make(desc2d(32, 32, 1, 16, ct::kSw4kbS));
        check(s && s->block[0] == 16 && s->block[1] == 16, "4KB_S 16-byte block is 16x16");
        check(s && offset_of(*s, 0, 0, 1, 0) == (1u << 4), "4KB_S y=1");
        check(s && offset_of(*s, 0, 1, 0, 0) == (1u << 6), "4KB_S x=1");
        check(s && offset_of(*s, 0, 16, 0, 0) == 4096, "4KB_S second block");
        check(s && offset_of(*s, 0, 0, 16, 0) == 8192, "4KB_S third block");
    }
    // A 256x256 BC7 chain in 64 KiB D: one tail block holding mips 1-8, then mip 0; mip 1 sits at x = 32 elements,
    // 32 KiB into the tail. 4 KiB blocks hold fewer tail mips.
    {
        const auto l = ct::Layout::make(desc2d(64, 64, 9, 16, ct::kSw64kbD));
        check(l && l->first_tail_mip == 1 && l->size == 2 * 65536 && l->mips[0].offset == 65536, "tail comes first");
        check(l && l->mips[1].tail && l->mips[1].tail->offset == 32768 && l->mips[1].tail->x == 32 &&
                  l->mips[1].tail->y == 0,
              "mip 1 tail position");
        check(l && offset_of(*l, 1, 0, 0, 0) == 32768, "mip 1 element 0 address");
        const auto s = ct::Layout::make(desc2d(64, 64, 9, 16, ct::kSw4kbS));
        check(s && s->first_tail_mip == 3 && s->mips[2].offset == 4096 && s->mips[1].offset == 8192 &&
                  s->mips[0].offset == 8192 + 16384,
              "4 KiB tail holds fewer mips");
    }
    // Depth-stencil is two planes: 4-byte depth in 128x128 blocks, 1-byte stencil in 256x256 blocks.
    {
        ct::TextureDesc t;
        t.width = 1920;
        t.height = 1080;
        t.format = 20;
        t.swizzle = ct::kSw64kbZX;
        const auto tex = ct::Texture::make(t);
        check(tex && tex->planes.size() == 2, "D32_FLOAT_S8X24 has two planes");
        check(tex && tex->planes[0].layout.size == 1920ull * 1152 * 4, "depth plane size");
        check(tex && tex->planes[1].offset == tex->planes[0].layout.size, "stencil follows depth");
        check(tex && tex->planes[1].layout.size == 2048ull * 1280, "stencil plane size");
        check(tex && !tex->planes[0].layout.addressable(), "XOR modes are not addressed");
    }
    // Bad descriptions and buffers are refused.
    {
        check(!ct::Layout::make(desc2d(0, 4, 1, 4, ct::kSw4kbS)), "zero width is refused");
        check(!ct::Layout::make(desc2d(4, 4, 1, 12, ct::kSw4kbS)), "12-byte elements are refused");
        check(!ct::Layout::make(desc2d(4, 4, 1, 4, 3)), "mode 3 is refused");
        const auto l = ct::Layout::make(desc2d(64, 64, 1, 4, ct::kSw4kbS));
        std::vector<uint8_t> out(64 * 64 * 4), small(100);
        check(l && l->detile(small.data(), small.size(), 0, 0, out.data(), out.size(), 256, 0) == ct::Error::too_small,
              "a short tiled buffer is too small");
        uint64_t at = 0;
        check(l && l->offset(0, 64, 0, 0, at) == ct::Error::invalid, "an element outside is invalid");
        const auto lin = ct::Layout::make(desc2d(4, 2, 1, 4, ct::kLinear));
        std::vector<uint8_t> tiled(lin ? lin->size : 0), linear(32);
        check(lin && lin->detile(tiled.data(), tiled.size(), 0, 0, linear.data(), linear.size(), SIZE_MAX, 0) ==
                         ct::Error::too_small,
              "an overflowing row pitch is refused");
        auto vol = desc2d(4, 2, 1, 4, ct::kLinear);
        vol.dimension = ct::Dimension::tex3d;
        vol.depth = 3;
        const auto v = ct::Layout::make(vol);
        std::vector<uint8_t> vt(v ? v->size : 0), vl(96);
        check(v && v->detile(vt.data(), vt.size(), 0, 0, vl.data(), vl.size(), 16, SIZE_MAX) == ct::Error::too_small,
              "an overflowing depth pitch is refused");
    }
}

// Every element of every mip and slice lands on its own address inside the texture, and tiling then detiling gives
// the rows back without touching the padding around them.
void test_bijection_and_round_trip() {
    std::vector<ct::Desc> cases = {
        desc2d(64, 64, 7, 16, ct::kSw64kbD), desc2d(100, 60, 7, 4, ct::kSw64kbD), desc2d(250, 175, 8, 8, ct::kSw4kbS),
        desc2d(33, 17, 6, 1, ct::kSw4kbD),   desc2d(40, 9, 3, 2, ct::kSw64kbS),   desc2d(30, 20, 5, 4, ct::kLinear),
        desc2d(83, 31, 6, 1, ct::kSw256bS),
    };
    auto bc = desc2d(109, 71, 6, 16, ct::kSw256bD);
    bc.block = 4;
    cases.push_back(bc);
    auto array = desc2d(70, 70, 4, 16, ct::kSw4kbS);
    array.depth = 3;
    cases.push_back(array);
    auto volume = desc2d(64, 32, 7, 4, ct::kSw4kbS);
    volume.dimension = ct::Dimension::tex3d;
    volume.depth = 16;
    cases.push_back(volume);
    for (const ct::Desc& d : cases) {
        const auto l = ct::Layout::make(d);
        if (!l) {
            check(false, "round-trip case lays out");
            continue;
        }
        std::vector<bool> seen(static_cast<size_t>(l->size));
        std::vector<uint8_t> tiled(static_cast<size_t>(l->size));
        const size_t n = d.element_bytes;
        const uint32_t slices = d.dimension == ct::Dimension::tex3d ? 1 : d.depth;
        bool ok = true;
        for (uint32_t slice = 0; slice < slices; ++slice) {
            for (uint32_t mip = 0; mip < d.mips; ++mip) {
                const ct::Mip& m = l->mips[mip];
                const size_t active = size_t{m.width} * n, row = active + 13, plane = row * m.height + 11;
                std::vector<uint8_t> linear(plane * m.depth);
                for (size_t i = 0; i < linear.size(); ++i) linear[i] = static_cast<uint8_t>(i * 7 + mip * 13 + slice);
                ok &= l->tile(tiled.data(), tiled.size(), mip, slice, linear.data(), linear.size(), row, plane) ==
                      ct::Error::ok;
                std::vector<uint8_t> back(linear.size(), 0xcc);
                ok &= l->detile(tiled.data(), tiled.size(), mip, slice, back.data(), back.size(), row, plane) ==
                      ct::Error::ok;
                for (uint32_t z = 0; z < m.depth; ++z) {
                    const size_t base = z * plane;
                    for (uint32_t y = 0; y < m.height; ++y) {
                        const size_t start = base + y * row;
                        ok &= std::memcmp(&back[start], &linear[start], active) == 0;
                        ok &= all_bytes(&back[start + active], row - active, 0xcc);
                        for (uint32_t x = 0; x < m.width; ++x) {
                            uint64_t at64 = 0;
                            if (l->offset(mip, x, y, slice + z, at64) != ct::Error::ok) {
                                ok = false;
                                continue;
                            }
                            const size_t at = static_cast<size_t>(at64);
                            const size_t source = start + x * n;
                            if (at + n > seen.size() || at % n != 0 || seen[at]) {
                                ok = false;
                                continue;
                            }
                            ok &= std::memcmp(&tiled[at], &linear[source], n) == 0;
                            seen[at] = true;
                        }
                    }
                    ok &= all_bytes(&back[base + plane - 11], 11, 0xcc);
                }
            }
        }
        char what[96];
        std::snprintf(what, sizeof(what), "bijection and round trip: %ux%ux%u mips %u bpe %u mode %u", d.width,
                      d.height, d.depth, d.mips, d.element_bytes, d.swizzle);
        check(ok, what);
    }
}

// Two models lay the console's textures out in this tree: xg_xs_layout.h, which the XG computer answers the title
// with and the D3D12 layer's title-texture registry reads back with, and console_texture_layout.h, which the linear
// and tiled uploads read with. A texture both lay out must have one size and one address per element, or one side
// reads at offsets the other did not write. So for each such texture: the sizes are equal, each mip has the same
// extent in elements, and what the first model tiles the second detiles to the same rows, for every mip and slice.
void test_models_agree() {
    struct Case {
        uint32_t width, height, array, mips;
        DXGI_FORMAT format;
        uint32_t mode;
    };
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                   DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC3_UNORM};
    const uint32_t modes[] = {kLinear, k4kbS, k64kbS, k64kbD};
    struct Shape {
        uint32_t width, height, array, mips;
    };
    const Shape shapes[] = {{1, 1, 1, 1},    {4, 4, 1, 3},      {16, 16, 1, 1},  {64, 64, 1, 7},   {100, 60, 1, 7},
                            {256, 128, 1, 9}, {250, 175, 3, 8}, {512, 512, 1, 10}, {1024, 64, 2, 1}, {33, 1000, 1, 4}};
    unsigned compared = 0, only_one = 0;
    std::vector<uint8_t> tiled, source, back;
    std::vector<uint64_t> columns;
    for (const DXGI_FORMAT format : formats) {
        for (const uint32_t mode : modes) {
            for (const Shape& s : shapes) {
                const Format f = format_info(uint32_t(format));
                ResourceDesc d{kTexture2D, 0, s.width, s.height, uint16_t(s.array), uint16_t(s.mips),
                               uint32_t(format), 1, 0, mode == kLinear ? 1u : (kSwizzled | mode), 0};
                Texture t;
                const bool first = build_texture(d, t);
                ct::Desc p;
                p.width = s.width;
                p.height = s.height;
                p.depth = s.array;
                p.mips = s.mips;
                p.element_bytes = f.bytes;
                p.block = f.side;
                p.swizzle = mode;
                const auto l = ct::Layout::make(p);
                const bool second = l && l->addressable();
                char what[160];
                std::snprintf(what, sizeof(what), "both models agree: %ux%u array %u mips %u format %u mode %u",
                              s.width, s.height, s.array, s.mips, unsigned(format), mode);
                if (!first || !second) {
                    only_one += first != second;
                    continue;
                }
                ++compared;
                const char* differs = nullptr;
                if (t.layout.size != l->size) differs = "size";
                if (!differs && t.layout.mips != l->mips.size()) differs = "mip count";
                if (!differs) tiled.assign(size_t(t.layout.size), uint8_t{0});
                for (uint32_t level = 0; !differs && level < s.mips; ++level) {
                    const auto& a = t.layout.plane[0].mips[level];
                    const auto& b = l->mips[level];
                    if (a.width_elements != b.width || a.height_elements != b.height) {
                        differs = "mip extent";
                        break;
                    }
                    const uint32_t pitch = a.width_elements * f.bytes;
                    for (uint32_t slice = 0; !differs && slice < s.array; ++slice) {
                        source.resize(size_t(pitch) * a.height_elements);
                        for (size_t i = 0; i < source.size(); ++i)
                            source[i] = uint8_t(i * 131 + (i >> 8) * 7 + level * 17 + slice * 29 + 1);
                        copy_elements<true>(t, level, slice, source.data(), tiled.data(), pitch, 0, columns);
                        back.assign(source.size(), uint8_t{0});
                        const auto copied =
                            l->detile(tiled.data(), tiled.size(), level, slice, back.data(), back.size(), pitch, 0);
                        if (copied != ct::Error::ok) differs = "the second model refuses the copy";
                        else if (back != source) differs = "element addresses";
                    }
                }
                if (differs) {
                    std::printf("     %s: %s differ (sizes %llu and %llu)\n", what, differs,
                                static_cast<unsigned long long>(t.layout.size),
                                static_cast<unsigned long long>(l->size));
                }
                check(!differs, what);
            }
        }
    }
    std::printf("     %u textures laid out by both models, %u by one of them only\n", compared, only_one);
    check(compared >= 200, "both models lay out most of the list, so the comparison is not empty");
}

}  // namespace

int main() {
    check_mip_prefix(DXGI_FORMAT_BC7_UNORM, 0x109, 1024, 1024, 11);   // 64 KiB blocks and a tail
    check_mip_prefix(DXGI_FORMAT_BC1_UNORM, 0x105, 512, 256, 10);     // 4 KiB blocks and a tail
    check_mip_prefix(DXGI_FORMAT_BC7_UNORM, 0x109, 300, 700, 10);     // extents that are no power of two
    check_mip_prefix(DXGI_FORMAT_R8G8B8A8_UNORM, 0x10a, 256, 256, 1);  // one mip
    check_mip_prefix(DXGI_FORMAT_R8G8B8A8_UNORM, 0x105, 512, 512, 2);  // a chain too short for a tail
    check_mip_prefix(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 256, 128, 9);     // linear
    {
        ResourceDesc d{kTexture2D, 0, 256, 256, 6, 9, DXGI_FORMAT_BC7_UNORM, 1, 0, 0x109, 0};
        Texture cube;
        check(build_texture(d, cube) && mip_chain_bytes(cube, 4) == cube.layout.size &&
                  first_mip_within(cube, cube.layout.size - 1) == cube.layout.mips,
              "an array is readable as a whole or not at all");
    }
    {
        struct Entry { bool mapped, stale; int id; };
        Entry first{false, false, 1}, mapped{true, false, 2}, stale{false, true, 3};
        Entry last{false, false, 4}, added{false, false, 5};
        std::map<std::uint64_t, Entry*> entries{{10, &first}, {20, &mapped}, {30, &stale}, {40, &last}};
        std::uint64_t cursor = 0;
        std::vector<int> polled;
        const auto visit = [&](Entry& t) { polled.push_back(t.id); };
        const auto poll = [&](auto exhausted) {
            const auto snapshot = leonida::title_texture_snapshot(entries);
            leonida::poll_title_textures(snapshot, cursor, visit, exhausted);
        };
        poll([&] { return !polled.empty(); });
        check(polled == std::vector<int>{1}, "budget stops after a background texture");
        entries.erase(10);
        entries[50] = &added;
        poll([&] { return polled.size() == 2; });
        check(polled == std::vector<int>{1, 4}, "poll resumes after deletion and skips priority textures");
        polled.clear();
        poll([] { return false; });
        check(polled == std::vector<int>{5, 4}, "unlimited poll wraps once and includes new textures");
        cursor = 20;
        polled.clear();
        for (unsigned pass = 0; pass < 3; ++pass) poll([] { return true; });
        check(polled == std::vector<int>{4}, "an expired wall budget advances past priority entries to the next texture");

        // LEONIDA_TITLE_EVERY_BUDGET_US: the marked textures (mapped or stale) are visited from a cursor of their own
        // until the budget runs out; the unmarked ones are the round robin's and are skipped here.
        {
            Entry m1{true, false, 11}, u1{false, false, 12}, s1{false, true, 13}, m2{true, false, 14};
            Entry u2{false, false, 15}, s2{false, true, 16};
            std::map<std::uint64_t, Entry*> marked{{100, &m1}, {200, &u1}, {300, &s1}, {400, &m2}, {500, &u2}};
            std::uint64_t every = 0;
            std::vector<int> seen;
            std::vector<int> urgent_ids;  // stands for "mapped and never filled"
            const auto is_urgent = [&](const Entry& t) {
                return std::find(urgent_ids.begin(), urgent_ids.end(), t.id) != urgent_ids.end();
            };
            const auto visit_marked = [&](auto exhausted) {
                const auto snapshot = leonida::title_texture_snapshot(marked);
                leonida::visit_marked_title_textures(
                    snapshot, every,
                    [&](Entry& t) {
                        seen.push_back(t.id);
                        urgent_ids.erase(std::remove(urgent_ids.begin(), urgent_ids.end(), t.id), urgent_ids.end());
                    },
                    is_urgent, exhausted);
            };
            visit_marked([] { return false; });
            check(seen == std::vector<int>{11, 13, 14}, "without a limit every marked texture is visited once, in order");
            seen.clear();
            for (unsigned pass = 0; pass < 4; ++pass) visit_marked([] { return true; });
            check(seen == std::vector<int>{11, 13, 14, 11},
                  "a budget spent after each visit moves one marked texture a pass and wraps");
            seen.clear();
            marked.erase(300);
            marked[350] = &s2;
            visit_marked([&] { return seen.size() == 2; });
            check(seen == std::vector<int>{16, 14}, "the cursor survives an erased entry and reaches a new stale one");
            seen.clear();
            visit_marked([] { return true; });
            check(seen == std::vector<int>{11}, "the pass after resumes past the last visited entry");
            // The cursor now points past 100 (m1). Mark m2 (at 400) urgent, as a mapped texture never filled: it is
            // visited first whatever the budget, and once filled (no longer urgent) it is not visited a second time
            // in the same pass's rotation.
            seen.clear();
            urgent_ids = {14};
            visit_marked([] { return true; });
            check(seen == std::vector<int>{14, 16}, "an urgent texture is visited first, then one rotation step");
            seen.clear();
            urgent_ids = {14};
            visit_marked([] { return false; });
            check(seen == std::vector<int>{14, 11, 16},
                  "with no limit an urgent texture is still visited once, not again in the rotation");
            seen.clear();
            const std::vector<std::pair<std::uint64_t, Entry*>> none;
            leonida::visit_marked_title_textures(
                none, every, [&](Entry& t) { seen.push_back(t.id); }, [](const Entry&) { return true; },
                [] { return false; });
            check(seen.empty(), "an empty registry visits nothing");
        }

        // A fill made during a pass submits, and that submission destroys a retired image whose destructor erases its
        // registry entry; the registry mutex is recursive for its owner on Windows, so the erase lands inside the pass.
        // A walk over a snapshot keeps every entry of the pass alive and in order, which a walk over the registry
        // itself cannot do: its iterator steps into a freed node.
        std::map<std::uint64_t, std::shared_ptr<Entry>> registry;
        for (std::uint64_t address = 1; address <= 32; ++address)
            registry[address * 16] = std::make_shared<Entry>(Entry{false, false, int(address)});
        std::vector<int> walked;
        const auto pass = leonida::title_texture_snapshot(registry);
        for (const auto& entry : pass) {
            walked.push_back(entry.second->id);
            registry.erase(entry.first);                 // a retired image whose entry leaves mid-pass
            registry[entry.first + 1] = entry.second;    // and an image re-registered at the next address
        }
        std::vector<int> every(32);
        for (int i = 0; i < 32; ++i) every[i] = i + 1;
        check(walked == every, "a pass walks every entry once even when a visit erases and re-registers");
        check(registry.size() == 32, "the registry kept the re-registrations made during the pass");
    }

    {
        leonida::TitleTextureRangeIndex index;
        std::map<std::uint64_t, std::pair<unsigned, unsigned>> ranges;
        for (unsigned id = 1; id <= 60; ++id) {
            const auto begin = (id * 17) % 31;
            ranges[id] = {begin, begin + id};
            index.set(id, begin, id);
        }
        index.erase(12);
        ranges.erase(12);
        index.set(13, 5, 9);
        ranges[13] = {5, 14};
        index.set(2, 0, 0);
        ranges.erase(2);
        bool same = true;
        for (unsigned from = 0; from < 100; ++from) {
            for (unsigned to = from; to <= 100; ++to) {
                std::vector<std::uint64_t> got, want;
                index.overlapping(from, to, [&](auto key) { got.push_back(key); });
                std::sort(got.begin(), got.end());
                for (const auto& [key, range] : ranges)
                    if (from < to && range.first < to && from < range.second) want.push_back(key);
                same &= got == want;
            }
        }
        check(same, "indexed invalidation matches all CPU overlaps after alias, replacement and removal");
        index.set(99, (UINT64_MAX >> 1) - 2, 16);
        unsigned hits = 0;
        index.overlapping(UINT64_MAX >> 1, (UINT64_MAX >> 1) + 1, [&](auto id) { hits += id == 99; });
        check(hits == 1, "an interval across bit 63 is indexed without a word-width shift");
        const auto slice = leonida::title_texture_snapshot(ranges, 12, 4);
        check(slice.size() == 4 && slice.front().first == 13 && slice.back().first == 16,
              "bounded refresh resumes at the next live address");
        check(leonida::title_texture_snapshot(ranges, 61, 4).empty(), "end of sweep permits wrap");
    }

    {
        leonida::TitleWriteRanges writes{{20, 30}, {10, 20}, {15, 17}, {60, 75}, {60, 67}, {90, 90}};
        const auto original = writes;
        leonida::merge_title_writes(writes);
        check(writes == leonida::TitleWriteRanges{{10, 30}, {60, 75}}, "coalesce overlapping and adjacent writes");
        bool same = true;
        for (unsigned begin = 0; begin < 100; ++begin) {
            for (unsigned end = begin; end < 100; ++end) {
                const bool overlap = std::any_of(original.begin(), original.end(), [&](auto r) {
                    return begin < end && r.first < r.second && r.first < end && r.second > begin;
                });
                same &= leonida::title_write_overlaps(writes, begin, end) == overlap;
            }
        }
        check(same, "indexed writes find the same overlaps as scanning each original notification");
    }

    SYSTEM_INFO sys{};
    GetSystemInfo(&sys);
    const auto page = sys.dwPageSize;
    leonida::TextureReadability residency;
    unsigned probes = 0;
    bool committed = false;
    const auto probe = [&] { ++probes; return committed; };
    for (unsigned pass = 0; pass < 100; ++pass) residency.check(8, true, probe);
    check(probes == 1 && residency.pending(), "unchanged reserved texture is probed only once in 100 passes");
    check(!residency.check(9, true, probe) && probes == 2, "a new map generation retries the failed footprint");
    committed = true;
    residency.retry();  // overlapping DirectStorage or XG write
    check(residency.check(9, true, probe) && probes == 3, "write notification retries at the same generation");
    committed = false;
    check(!residency.check(9, true, probe) && probes == 4, "previous success is rechecked before the next read");
    check(!residency.check(9, false, probe) && probes == 5, "rollback disables the failed-probe cache");
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE, 0, 65536, nullptr);
    if (!section) return 1;
    void* view = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 65536);
    if (!view) return 1;
    const auto placed = reinterpret_cast<std::uintptr_t>(view);
    const auto first_touch = [](const void* at, std::size_t bytes) {
        return VirtualAlloc(const_cast<void*>(at), bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
    };
    check(!leonida::texture_memory_readable(placed, 65536), "background polling leaves mapped first-touch pages reserved");
    check(!leonida::texture_memory_readable(placed, 65536, [](const void*, std::size_t) { return true; }),
          "a section outside xmem stays deferred when the commit hook leaves it reserved");
    check(leonida::texture_memory_readable(placed, 65536, first_touch), "persistent Map can populate a physical section before checking it");
    UnmapViewOfFile(view);
    CloseHandle(section);
    void* reservation = VirtualAlloc(nullptr, 2 * page, MEM_RESERVE, PAGE_NOACCESS);
    if (!reservation) return 1;
    const auto address = reinterpret_cast<std::uintptr_t>(reservation);
    check(!leonida::texture_memory_readable(address, page), "untouched reserved texture is deferred");
    check(!leonida::texture_memory_readable(address, page, first_touch), "a persistent Map cannot commit a bare reservation");
    leonida::TextureReadability persistent_map;
    const auto probe_mapped = [&] { return leonida::texture_memory_readable(address, page); };
    check(!persistent_map.check(11, false, probe_mapped), "persistent Map defers a reserved texture before decoder writes");
    if (!VirtualAlloc(reservation, page, MEM_COMMIT, PAGE_READWRITE)) return 1;
    auto* pixels = static_cast<std::uint8_t*>(reservation);
    pixels[page - 1] = 93;
    check(persistent_map.check(11, false, probe_mapped) && pixels[page - 1] == 93,
          "persistent Map sees decoder writes after a failed probe at the same generation");
    check(leonida::texture_memory_readable(address + 3, page - 3) &&
          !leonida::texture_memory_readable(address + 3, page), "range crossing into a reserved mip is deferred");
    if (!VirtualAlloc(reinterpret_cast<void*>(address + page), page, MEM_COMMIT, PAGE_READONLY)) return 1;
    check(leonida::texture_memory_readable(address, 2 * page), "later stream commit completes the footprint");
    DWORD previous = 0;
    if (!VirtualProtect(reservation, page, PAGE_READWRITE | PAGE_GUARD, &previous)) return 1;
    check(!leonida::texture_memory_readable(address, 2 * page), "guarded texture page remains untouched");
    check(leonida::texture_memory_readable_bytes(address, 2 * page) == 0, "the readable prefix ends at a guarded first page");
    VirtualFree(reservation, page, MEM_DECOMMIT);
    check(!leonida::texture_memory_readable(address, page), "previously readable page is rechecked after decommit");
    VirtualFree(reservation, 0, MEM_RELEASE);

    auto create = reinterpret_cast<CreateFn>(rt::shims::find("xg_xs.dll", "XGCreateTextureComputer"));
    auto suggest = reinterpret_cast<SuggestFn>(rt::shims::find("xg_xs.dll", "XGSuggestSwizzleMode"));
    check(create && suggest && rt::shims::owns_dll("xg_xs.dll"), "XG exports are registered");
    if (!create || !suggest) return 1;

    test_layout_model();
    test_bijection_and_round_trip();
    test_models_agree();
    ResourceDesc exposure{3, 0, 1, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, 0x100, 0x100001};
    void* target = nullptr;
    if (!make(create, exposure, target, "create exposure readback render target")) return 1;
    ResourceLayout exposure_layout{};
    check(slot<LayoutFn>(target, 2)(target, &exposure_layout) == S_OK, "exposure slot 2 returns layout");
    const auto& plane = exposure_layout.plane[0];
    const auto& mip = plane.mips[0];
    check(exposure_layout.planes == 1 && exposure_layout.mips == 1 && plane.offset == 0 && mip.offset == 0,
          "first plane and mip start at the returned Map pointer");
    check(exposure_layout.size == 256 && mip.pitch_bytes == 256 && mip.slice_bytes == 256 && mip.size == 256,
          "one RGBA32F texel occupies one aligned linear row");
    slot<RefFn>(target, 1)(target);
    for (uint32_t flags : {kRenderTarget, kRenderTarget | 0x300000u}) {
        auto variant = exposure;
        variant.flags = flags;
        if (!make(create, variant, target, "linear render target with console flag variants")) return 1;
        ResourceLayout layout{};
        check(slot<LayoutFn>(target, 2)(target, &layout) == S_OK &&
              !std::memcmp(&layout, &exposure_layout, sizeof(layout)), "console flag variants have identical footprints");
        slot<RefFn>(target, 1)(target);
    }
    for (unsigned i = 0; i < 5; ++i) {
        auto other = exposure;
        switch (i) {
        case 0: other.width = 2; break;
        case 1: other.layout = 0x11b; break;
        case 2: other.flags |= kDepthStencil; break;
        case 3: other.samples = 4; break;
        case 4: other.format = DXGI_FORMAT_R8_UNORM; break;
        }
        Texture primary{};
        check(!build_texture(other, primary), "the linear readback shortcut excludes other target layouts");
        target = nullptr;
        check(create(&other, &target) == S_OK && target, "the other render targets still use the general layout model");
        if (target) slot<RefFn>(target, 1)(target);
    }

    ResourceDesc d{3, 0, 256, 128, 1, 1, DXGI_FORMAT_R8_UNORM, 1, 0, 0, 0};
    check(suggest(&d, 0, &d.layout) == S_OK && d.layout == 0x105, "2D SRV selects SW_4KB_S with aliased output");
    uint32_t mode = 0xdeadbeef;
    check(suggest(&d, 1, &mode) == E_NOTIMPL && mode == 0xdeadbeef, "unknown selector leaves output unchanged");
    check(create(&d, nullptr) == E_POINTER, "null computer output is rejected");
    void* computer = nullptr;
    if (!make(create, d, computer, "create 2D SW_4KB_S computer")) return 1;

    ResourceLayout l;
    std::memset(&l, 0xa5, sizeof(l));
    auto get_layout = slot<LayoutFn>(computer, 2);
    auto get_offset = slot<OffsetFn>(computer, 6);
    auto copy_into = slot<IntoFn>(computer, 8);
    check(get_layout(computer, &l) == S_OK, "slot 2 fills the Series layout");
    check(l.size == 32768 && l.alignment == 4096 && l.planes == 1 && l.mips == 1 && l.dimension == 3,
          "256x128 R8 uses eight 64x64 SW_4KB_S blocks");
    check(l.plane[0].mips[0].pitch_bytes == 256 && l.plane[0].mips[0].offset == 0,
          "slot 2 reports pitch and offset");
    const std::array<PlaneLayout, 3> unused{};
    check(std::memcmp(&l.plane[1], unused.data(), sizeof(unused)) == 0 && !l.reserved,
          "slot 2 clears all three unused planes and the trailing reserved word");
    check(slot<SizeFn>(computer, 3)(computer) == 32768 && slot<SizeFn>(computer, 4)(computer) == 4096,
          "size and alignment agree with slot 2");
    uint32_t bit = 99;
    check(get_offset(computer, 0, 0, 0, 1, 0, 0, &bit) == 16 && bit == 0 &&
          get_offset(computer, 0, 0, 64, 0, 0, 0, nullptr) == 4096,
          "SW_4KB_S addresses Y0 at bit 4 and X6 in the second tile");
    std::vector<uint8_t> tiled(l.size, 0xcd);
    constexpr unsigned pitch = 260;
    std::vector<uint8_t> linear(pitch * 128, 0);
    linear[64] = 17;
    linear[pitch] = 29;
    check(copy_into(computer, tiled.data(), 0, 0, linear.data(), pitch, 0) == S_OK &&
          tiled[4096] == 17 && tiled[16] == 29, "slot 8 writes expected tiled offsets from padded linear rows");
    check(copy_into(computer, tiled.data(), 0, 1, linear.data(), pitch, 0) == E_INVALIDARG,
          "slot 8 rejects an out-of-range subresource");
    check(copy_into(computer, tiled.data(), 0, 0, linear.data(), 255, 0) == E_INVALIDARG,
          "slot 8 rejects a short row pitch");
    check(slot<TilingFn>(computer, 10)(computer, nullptr, nullptr, nullptr, nullptr, 0, nullptr) == E_FAIL,
          "slot 10 requires a 64 KiB mode");
    check(slot<RefFn>(computer, 0)(computer) == 2 && slot<RefFn>(computer, 1)(computer) == 1 &&
          slot<RefFn>(computer, 1)(computer) == 0, "release deletes the computer at refcount zero");

    // The VIctory 256x256 BC7 SW_64KB_D fixture has one 64 KiB standard
    // mip and a tail block before it. Exercise it as a two-element array.
    d = {3, 0, 256, 256, 2, 0, DXGI_FORMAT_BC7_UNORM, 1, 0, 0x10a, 0};
    if (!make(create, d, computer, "create BC7 mipmapped array computer")) return 1;
    check(slot<LayoutFn>(computer, 2)(computer, &l) == S_OK, "get BC7 layout");
    const auto& p = l.plane[0];
    check(l.size == 262144 && l.mips == 9 && p.slice_bytes == 131072 &&
          p.standard_mips == 1 && p.tail_mips == 8 && p.element_bytes == 16,
          "BC7 array uses a standard block and a tail block per slice");
    check(p.mips[0].offset == 65536 && p.mips[0].pitch_bytes == 1024 &&
          p.mips[1].offset == 32768 && p.mips[1].in_tail,
          "BC7 standard mip and first tail offsets match fixture");
    uint32_t tiles = 0, packed[3]{}, shape[3]{}, count = 18;
    std::array<Tiling, 18> tilings{};
    tilings[1].start = 0xdeadbeef;
    check(slot<TilingFn>(computer, 10)(computer, &tiles, packed, shape, &count, 0, tilings.data()) == S_OK &&
          tiles == 4 && packed[0] == 0x0801 && packed[1] == 1 && packed[2] == 0 &&
          shape[0] == 256 && shape[1] == 256 && shape[2] == 1 && count == 2 &&
          tilings[0].start == 1 && tilings[9].start == 3 && tilings[1].start == 0xdeadbeef,
          "slot 10 reports standard tiles, texture-unit shape and packed tail");
    tiled.assign(l.size, 0);
    std::array<uint8_t, 16> block{}, back{};
    block[0] = 77;
    const unsigned last_mip_in_slice_1 = 17;
    check(slot<IntoFn>(computer, 8)(computer, tiled.data(), 0, last_mip_in_slice_1, block.data(), 16, 0) == S_OK &&
          tiled[p.slice_bytes + p.mips[8].offset] == 77 &&
          slot<FromFn>(computer, 9)(computer, back.data(), 0, last_mip_in_slice_1, 0, tiled.data(), 16, 0) == S_OK &&
          back == block, "slots 8 and 9 copy a BC block in the second slice's tail");
    slot<RefFn>(computer, 1)(computer);

    // The addrlib padded mip sizes use ceil division even for odd extents.
    Texture texture;
    d = {3, 0, 30, 20, 1, 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0, 1, 0};
    check(build_texture(d, texture) && texture.layout.size == 10240 &&
          texture.layout.plane[0].mips[0].offset == 5120 && texture.layout.plane[0].mips[0].pitch_bytes == 256,
          "row-major chain aligns each physical pitch to 256 B");

    for (const uint32_t swizzle : {0x105u, 0x109u, 0x10au}) {
        for (DXGI_FORMAT format : {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC7_UNORM}) {
            d = {3, 0, 259, 175, 2, 0, uint32_t(format), 1, 0, swizzle, 0};
            if (build_texture(d, texture)) check_addresses(texture), check_detile(texture);
            else check(false, "create odd-sized tiled mip chain");
        }
    }

    d = {4, 0, 9, 2, 2, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0, 0x105, 0};
    if (!make(create, d, computer, "create single-mip 3D SW_4KB_S computer")) return 1;
    check(slot<LayoutFn>(computer, 2)(computer, &l) == S_OK &&
          l.size == 8192 && l.plane[0].mips[0].padded_depth == 8 && l.plane[0].mips[0].tile_depth == 8,
          "3D 4KB_S blocks for RGBA8 are 8x16x8");
    check(slot<OffsetFn>(computer, 6)(computer, 0, 0, 0, 0, 1, 0, nullptr) == 16 &&
          slot<OffsetFn>(computer, 6)(computer, 0, 0, 8, 0, 0, 0, nullptr) == 4096,
          "3D 4KB_S selects Z0 at bit 4 and X3 in the second tile");
    slot<RefFn>(computer, 1)(computer);

    // GFX10 thick 4 KiB blocks: each 16-deep slab has one tail block, one
    // level-1 block and four level-0 blocks. Crossing z=16 skips the whole slab.
    ResourceDesc volume{4, 0, 32, 32, 32, 6, DXGI_FORMAT_R8_UNORM, 1, 0, 0x105, 0};
    if (!build_texture(volume, texture)) return 1;
    const ElementAddress thick(texture);
    const auto& levels = texture.layout.plane[0].mips;
    check(texture.layout.size == 49152 && levels[2].in_tail && levels[2].depth == 8 &&
          thick.offset(levels[0], {}, 0, 0, 0) == 8192 &&
          thick.offset(levels[0], {}, 0, 0, 16) == 32768 &&
          thick.offset(levels[2], thick.tail_origin(levels[2]), 0, 0, 0) == 2048,
          "3D mip chain uses per-slab macro offsets and a thick tail origin");
    for (DXGI_FORMAT format : {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                               DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                               DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC5_UNORM}) {
        volume = {4, 0, 61, 35, 67, 7, uint32_t(format), 1, 0, 0x105, 0};
        if (!build_texture(volume, texture)) return 1;
        const ElementAddress addr(texture);
        std::vector<uint8_t> used(texture.layout.size);
        bool distinct = true, matches = true;
        for (unsigned level = 0; level < texture.layout.mips; ++level) {
            const auto& mip = texture.layout.plane[0].mips[level];
            const auto tail = addr.tail_origin(mip);
            const unsigned pitch = mip.width_elements * texture.format.bytes + 8;
            const unsigned slice_pitch = pitch * mip.height_elements + 16;
            std::vector<uint8_t> source(slice_pitch * mip.depth), back(source.size()), tiled(texture.layout.size);
            std::vector<uint64_t> columns;
            for (unsigned z = 0; z < mip.depth; ++z)
                for (unsigned y = 0; y < mip.height_elements; ++y)
                    for (unsigned x = 0; x < mip.width_elements; ++x) {
                        const auto at = addr.offset(mip, tail, x, y, z);
                        if (at % texture.format.bytes || at > used.size() - texture.format.bytes || used[at]) {
                            distinct = false;
                            continue;
                        }
                        used[at] = 1;
                        source[z * slice_pitch + y * pitch + x * texture.format.bytes] = uint8_t(1 + (x+y+z)%251);
                    }
            if (!distinct) break;
            copy_elements<true>(texture, level, 0, source.data(), tiled.data(), pitch, slice_pitch, columns);
            copy_elements<false>(texture, level, 0, tiled.data(), back.data(), pitch, slice_pitch, columns);
            matches &= back == source;
        }
        check(distinct && matches, "odd volume mips and z slices are disjoint and preserve padded linear pitches");
    }

    d.format = DXGI_FORMAT_UNKNOWN;
    check(create(&d, &computer) == E_NOTIMPL && computer, "unsupported texture keeps a safe fallback object");
    check(slot<LayoutFn>(computer, 2)(computer, &l) == E_NOTIMPL && l.size == 8192 &&
          slot<RefFn>(computer, 1)(computer) == 1, "fallback leaves caller outputs untouched and is safe to release");

    // A linear 3D texture (layout 0x100) is thin (addrlib HwlComputeSurfaceInfoLinear): rows of 256 bytes, every
    // depth slice a whole mip chain. The title's texture lock reads mip 0's offset and size from this
    // computer and, when creation fails, its own uninitialised stack instead: measured with the 32x32x36 R32_UINT
    // 3D texture it clears 0x48000 bytes at that pointer, which is this layout's size exactly.
    d = {4, 0, 32, 32, 36, 1, DXGI_FORMAT_R32_UINT, 1, 0, 0x100, 0x300004};
    if (!make(create, d, computer, "create linear 3D computer")) return 1;
    std::memset(&l, 0xa5, sizeof(l));
    check(slot<LayoutFn>(computer, 2)(computer, &l) == S_OK && l.size == 0x48000 && l.alignment == 256 &&
              l.mips == 1 && l.dimension == 4 && l.plane[0].slice_bytes == 8192 &&
              l.plane[0].mips[0].offset == 0 && l.plane[0].mips[0].size == 0x48000 &&
              l.plane[0].mips[0].slice_bytes == 8192 && l.plane[0].mips[0].pitch_bytes == 256,
          "linear 3D 32x32x36 R32 is 36 slices of 32 rows of 256 bytes");
    check(slot<SizeFn>(computer, 3)(computer) == 0x48000 && slot<SizeFn>(computer, 4)(computer) == 256,
          "linear 3D size and alignment agree with slot 2");
    check(slot<OffsetFn>(computer, 6)(computer, 0, 0, 3, 2, 5, 0, nullptr) == 5 * 8192 + 2 * 256 + 3 * 4,
          "linear 3D element offset is slice, row, column");
    {
        // A mip-0 copy through slots 8 and 9: padded source rows and slices land at 256-byte rows, 8 KiB slices.
        constexpr unsigned row = 32 * 4 + 12, slice = row * 32 + 20;
        std::vector<uint8_t> source(slice * 36), read_back(source.size(), 0), memory(l.size, 0);
        for (size_t i = 0; i < source.size(); ++i) source[i] = uint8_t(i * 7 + 3);
        check(slot<IntoFn>(computer, 8)(computer, memory.data(), 0, 0, source.data(), row, slice) == S_OK &&
                  std::memcmp(&memory[35 * 8192 + 31 * 256], &source[35 * slice + 31 * row], 32 * 4) == 0 &&
                  slot<FromFn>(computer, 9)(computer, read_back.data(), 0, 0, 0, memory.data(), row, slice) == S_OK &&
                  std::memcmp(&read_back[35 * slice + 31 * row], &source[35 * slice + 31 * row], 32 * 4) == 0,
              "slots 8 and 9 copy a linear 3D mip slice by slice");
        check(slot<IntoFn>(computer, 8)(computer, memory.data(), 0, 1, source.data(), row, slice) == E_INVALIDARG,
              "a 3D texture has no second subresource");
    }
    slot<RefFn>(computer, 1)(computer);

    // A linear 3D texture with more than one mip: its chain layout (slice-major or mip-major) is not measured, so the
    // description is refused rather than given offsets that may be wrong for mips 1+.
    d = {4, 0, 32, 16, 8, 3, DXGI_FORMAT_R8_UNORM, 1, 0, 0x100, 0};
    check(!build_texture(d, texture), "linear 3D with mips is refused until a title texture measures it");

    // A render-target or depth flag with an explicit mode takes that mode's layout (measured: 1x1 RGBA32F
    // descriptions with layout 0x100 and the render-target bit reach the title's CPU upload).
    d = {3, 0, 1, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, 0x100, 0x300001};
    if (!make(create, d, computer, "create linear render-target computer")) return 1;
    check(slot<LayoutFn>(computer, 2)(computer, &l) == S_OK && l.size == 256 && l.alignment == 256,
          "explicit linear render target is one 256-byte row");
    slot<RefFn>(computer, 1)(computer);
    // Without a mode, the render target is not this model's (it leaves the mode to XG's Z/R choice): the general layout
    // model lays it out, as it does the other render targets above.
    void* general = nullptr;
    d.layout = 0;
    Texture own{};
    check(!build_texture(d, own), "a render target without a mode is not the explicit-mode model's");
    check(create(&d, &general) == S_OK && general, "a render target without a mode uses the general layout model");
    if (general) slot<RefFn>(general, 1)(general);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
