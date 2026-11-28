// The depth, stencil and raster state of the slot 57 record's state blob A (RT-SLOT57-DEPTH-RASTER): the register
// reads at their vertex and mesh offsets and the RDNA2 -> D3D12 mapping. Synthetic blobs only: no game data, no D3D12.
// The register words are forms docs/fspx.md "State blob A" counts in 1.0.2.1.
#include "../src/native_pso.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace np = leonida::native_pso;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

void put(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) { std::memcpy(b.data() + at, &v, 4); }

// D3D12 values (d3d12.h).
enum : std::uint8_t { KEEP = 1, ZERO = 2, REPLACE = 3, INVERT = 6, INCR = 7, LESS = 2, EQUAL = 3, GEQUAL = 7,
                      ALWAYS = 8, CULL_NONE = 1, CULL_FRONT = 2, CULL_BACK = 3 };

// The forms the library uses most: D3D clip space with z clip on, back-face culling with counter-clockwise front.
constexpr std::uint32_t kClip = 0x00080000, kClipOff = 0x0c080000, kCullBack = 0x00201a42, kCullNone = 0x00201a40;

np::DepthRasterRegisters regs(std::uint32_t depth, std::uint32_t stencil = 0, std::uint32_t mask = 0x0100ff00,
                              std::uint32_t mode = kCullBack, std::uint32_t clip = kClip) {
    np::DepthRasterRegisters r;
    r.clip_cntl = clip;
    r.mode_cntl = mode;
    r.depth_control = depth;
    r.stencil_control = stencil;
    r.stencil_ref_mask = r.stencil_ref_mask_bf = mask;
    return r;
}

void decode() {
    for (std::size_t size : {388u, 480u}) {
        std::vector<std::uint8_t> b(size, 0);
        const std::size_t shift = size > 420 ? np::kStateAMeshShift : 0;
        put(b, 0, 0x4005d810);
        put(b, 0x9c + shift, kClip);
        put(b, 0xa0 + shift, kCullBack);
        put(b, 0xb8 + shift, 0x007007e2);
        put(b, 0xbc + shift, 0x00030030);
        put(b, 0xc0 + shift, 0x01ffff00);
        put(b, 0xc4 + shift, 0x01ffff01);
        const auto r = np::decode_state_a_depth_raster(b);
        check(r && r->clip_cntl == kClip && r->mode_cntl == kCullBack && r->depth_control == 0x007007e2 &&
                  r->stencil_control == 0x00030030 && r->stencil_ref_mask == 0x01ffff00 &&
                  r->stencil_ref_mask_bf == 0x01ffff01,
              shift ? "mesh family: the registers are 84 bytes further on"
                    : "vertex family: PA_CL_CLIP_CNTL +0x9c ... DB_STENCILREFMASK_BF +0xc4");
    }
    check(!np::decode_state_a_depth_raster(std::vector<std::uint8_t>(430, 0)), "another shape is not decoded");
}

void depth() {
    auto s = np::d3d12_depth_raster(regs(0x007007f0));
    check(s && !s->depth_enable && !s->depth_write && s->depth_func == ALWAYS && !s->stencil_enable,
          "0x007007f0: no depth test, ALWAYS");
    s = np::d3d12_depth_raster(regs(0x007007e2));
    check(s && s->depth_enable && !s->depth_write && s->depth_func == GEQUAL, "0x007007e2: GEQUAL test, no writes");
    s = np::d3d12_depth_raster(regs(0x00700796));
    check(s && s->depth_enable && s->depth_write && s->depth_func == LESS, "0x00700796: LESS with writes");
    s = np::d3d12_depth_raster(regs(0x007007a2));
    check(s && s->depth_func == EQUAL, "ZFUNC 2 is EQUAL");
    check(!np::d3d12_depth_raster(regs(0x007007fa)), "depth bounds are not built");
    check(!np::d3d12_depth_raster(regs(0x407007e2)), "the colour-write-on-depth flags are not built");
}

void stencil() {
    // Stencil on, REPLACE on depth pass for both faces, masks 0xff, operand 1.
    auto s = np::d3d12_depth_raster(regs(0x007007e7, 0x00030030, 0x01ffff00));
    check(s && s->stencil_enable && s->read_mask == 0xff && s->write_mask == 0xff && s->front.pass == REPLACE &&
              s->front.fail == KEEP && s->front.depth_fail == KEEP && s->front.func == ALWAYS &&
              s->back.pass == REPLACE,
          "0x007007e7 / 0x00030030: stencil REPLACE on pass, both faces");
    // A back-face function of its own (STENCILFUNC_BF) and ADD_WRAP with operand 1 as INCR.
    s = np::d3d12_depth_raster(regs(0x002005e3 | 1, 0x00080080, 0x0100ff00));
    check(s && s->front.func == 6 && s->back.func == EQUAL + 0 && s->front.pass == INCR && s->back.pass == INCR &&
              s->write_mask == 0,
          "STENCILFUNC 5 (NOTEQUAL) in front, STENCILFUNC_BF 2 (EQUAL) behind, ADD_WRAP as INCR");
    s = np::d3d12_depth_raster(regs(0x007007e3, 0x00001071, 0x01ffff00));
    check(s && s->front.fail == ZERO && s->front.pass == INVERT && s->back.fail == ZERO + 0 && s->back.fail != KEEP,
          "ZERO and INVERT");
    check(!np::d3d12_depth_raster(regs(0x007007e3, 0x00000080, 0x02ffff00)), "ADD_WRAP with operand 2 is not built");
    check(!np::d3d12_depth_raster(regs(0x007007e3, 0x0000000a, 0x01ffff00)), "a logic stencil op is not built");
    auto r = regs(0x007007e3, 0, 0x01ffff00);
    r.stencil_ref_mask_bf = 0x01f0ff00;
    check(!np::d3d12_depth_raster(r), "front and back write masks that differ are not built");
    // Single-sided stencil (BACKFACE_ENABLE [7] clear): DB_STENCILREFMASK_BF is ignored, so whatever it holds the
    // state is built, with the front masks and ops on both faces (RT-STATE-DEPTH-RASTER-BACKFACE-MASK).
    r = regs(0x00700763, 0x00030030, 0x01ffff00);
    r.stencil_ref_mask_bf = 0;
    s = np::d3d12_depth_raster(r);
    check(s && s->stencil_enable && s->depth_enable && !s->depth_write && s->depth_func == GEQUAL &&
              s->cull == CULL_BACK && s->read_mask == 0xff && s->write_mask == 0xff && s->front.pass == REPLACE &&
              s->back.pass == REPLACE && s->back.func == s->front.func,
          "single-sided stencil: the back-face mask register is ignored, depth and raster state kept");
    r.stencil_ref_mask_bf = 0x01f0ff00;
    s = np::d3d12_depth_raster(r);
    check(s && s->write_mask == 0xff, "single-sided stencil: a differing back write mask is ignored");
    r = regs(0x007007e2, 0x0000000a, 0x01ffff00);
    check(np::d3d12_depth_raster(r).has_value(), "stencil registers are not read while the stencil is off");
}

void raster() {
    auto s = np::d3d12_depth_raster(regs(0x007007f0, 0, 0x0100ff00, kCullBack));
    check(s && s->cull == CULL_BACK && s->front_ccw && s->depth_clip, "0x00201a42: cull back, counter-clockwise front");
    s = np::d3d12_depth_raster(regs(0x007007f0, 0, 0x0100ff00, kCullNone));
    check(s && s->cull == CULL_NONE, "0x00201a40: no culling");
    s = np::d3d12_depth_raster(regs(0x007007f0, 0, 0x0100ff00, 0x00201a41));
    check(s && s->cull == CULL_FRONT, "0x00201a41: cull front");
    s = np::d3d12_depth_raster(regs(0x007007f0, 0, 0x0100ff00, 0x00201a46));
    check(s && s->cull == CULL_BACK && !s->front_ccw, "FACE set: clockwise front");
    check(!np::d3d12_depth_raster(regs(0x007007f0, 0, 0x0100ff00, 0x00201a43)), "culling both faces is not built");
    check(!np::d3d12_depth_raster(regs(0x007007f0, 0, 0x0100ff00, 0x00201a48)), "a polygon mode is not built");
    s = np::d3d12_depth_raster(regs(0x007007f0, 0, 0x0100ff00, kCullBack, kClipOff));
    check(s && !s->depth_clip, "0x0c080000: both z clips off is DepthClipEnable = FALSE");
    check(!np::d3d12_depth_raster(regs(0x007007f0, 0, 0x0100ff00, kCullBack, 0x04080000)),
          "only the near z clip off is not built");
    check(!np::d3d12_depth_raster(regs(0x007007f0, 0, 0x0100ff00, kCullBack, 0)), "GL clip space is not built");
}

void plan() {
    std::vector<std::uint8_t> a(388, 0), b(np::kStateBHeader, 0);
    put(a, 0, 0x4005d800);
    put(a, 0xb8, 0x007007e2);
    np::GraphicsDesc desc{};
    desc.state_a = {a.data(), a.size()};
    desc.state_b = {b.data(), b.size()};
    np::HashMap map;
    const auto p = np::plan(desc, map);
    check(p.state.depth_raster && p.state.depth_raster->depth_control == 0x007007e2,
          "plan() decodes blob A into FixedState::depth_raster");
}

}  // namespace

int main() {
    decode();
    depth();
    stencil();
    raster();
    plan();
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
