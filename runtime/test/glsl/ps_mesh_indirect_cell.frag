#version 460
// Pixel shader of d3d12_mesh_compute_indirect_test. Every group of the mesh-as-compute producer (ms_v5_workgroup
// and its wide twin) draws the same full-screen triangle and writes param0 = (32g, 32g+1, 32g+31, 1), g being the
// logical linear group index of its record (x + y * nx + z * nx * ny, restarting at 0 for every record and
// continuing across the tiles a large record is split into). This shader keeps one fragment per group: the one at
// pixel (g mod cells) of a target `width` pixels wide, and discards the rest. That one fragment
//   - adds 1.0 to render target 0 (additive blend), so a cell counts the groups that landed on it, from every
//     record and every tile: a skipped or doubled record or tile moves the count;
//   - stores g / 2^22 as depth (depth test ALWAYS, writes on), so the cell shows the last group that reached
//     it. 2^22 is the D3D12 maximum group count, so g / 2^22 is exact in f32 and a tile that restarted its group
//     numbering shows a different value.
// Root constants (the console graphics layout starts them at s8): s8 = cells, s9 = width.
layout(location = 0) in vec4 color;
layout(location = 0) out vec4 target;

layout(push_constant) uniform UserData {
    uint s[32];
    uvec2 heap_base;
    uvec2 code_base;
} pc;

void main() {
    const uint g = uint(floor(color.r / 32.0 + 0.5));
    const uvec2 pixel = uvec2(gl_FragCoord.xy);
    if (pixel.y * pc.s[9] + pixel.x != g % pc.s[8]) discard;
    target = vec4(1.0, 0.0, 0.0, 0.0);
    gl_FragDepth = float(g) / 4194304.0;
}
