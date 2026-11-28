#version 450

layout(location = 0) flat in int tag;
#ifndef TEST_MSAA_DEPTH
layout(location = 0) out vec4 color;
#endif
void main() {
    gl_SampleMask[0] = 1 << (tag % 4);
#ifdef TEST_MSAA_DEPTH
    const int x = int(gl_FragCoord.x);
    const int y = int(gl_FragCoord.y);
    gl_FragDepth = float(16 * tag + 4 * y + x) / 255.0;
#else
    color = vec4(floor(gl_FragCoord.xy), tag + 1, 255) / 255.0;
#endif
}
