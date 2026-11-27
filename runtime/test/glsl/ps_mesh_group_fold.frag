#version 460
// Group-index fold for d3d12_mesh_compute_test grid A (ms_v5_workgroup as compute).
// The mesh writes param0 = (32g, 32g+1, 32g+31, 1) with g the linear workgroup
// index x + y*nx + z*nx*ny, the same for all three vertices, so every covered
// pixel of every group interpolates its own g exactly. All groups draw the same
// full-screen triangle with no depth test, so the last group wins: grid 2x3x4
// must show g = 23 on every pixel. A replay that dropped Y or Z (the console
// clamp forces them to 1) would show g = 5 or 19 instead, each a different
// colour here. The root constant k (push dword 8, s8) proves graphics roots
// reach the consumer: B carries (floor(g/4) + k) / 8 with k = 1.

layout(location = 0) in vec4 color;
layout(location = 0) out vec4 target;

layout(push_constant) uniform UserData {
    uint s[32];
    uvec2 heap_base;
    uvec2 code_base;
} pc;

void main() {
    float g = floor(color.r / 32.0 + 0.5);
    float r = mod(g, 2.0);
    float gg = mod(floor(g / 2.0), 2.0);
    float b = (floor(g / 4.0) + float(pc.s[8])) / 8.0;
    target = vec4(r, gg, b, 1.0);
}
