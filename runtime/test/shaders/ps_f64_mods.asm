; regen_fixtures: this source has a translator fixture in tests/semantic/fixtures/disasm
; Output modifiers on v_add_f64 (VOP3_Profile<VOP_F64_F64_F64> carries clamp and omod). On RDNA2 the omod of this op
; does nothing while f64 denormals are kept, which is the MODE this port assumes, and clamp does apply: measured with
; tools/hw_oracle raw kernels on a gfx1036 iGPU in wave32 and wave64, mul:2/mul:4/div:2 apply only with MODE.IEEE off
; and f64 denormals flushed. So a + b is 1.25 with or without an omod, and clamp gives 1.0.
; a = 0.75, b = 0.5 as doubles (low word first); each export is the high word of one double result (low words are 0).
; mul:2 -> 1.25, clamp -> 1.0, clamp div:2 -> 1.0 (the omod did nothing), mul:4 on a - b -> 0.25.
; With the omod applied the words would have been 0x40040000 (2.5), 0x3fe40000 (0.625) and 0x3ff00000 (1.0).
; Needs shaderFloat64; vk_harness exits 77 (SKIP) on a device without it.
; Expected mrt0 bits: 0x3ff40000, 0x3ff00000, 0x3ff00000, 0x3fd00000
v_mov_b32 v0, 0
v_mov_b32 v1, 0x3fe80000
v_mov_b32 v2, 0
v_mov_b32 v3, 0x3fe00000
v_add_f64 v[4:5], v[0:1], v[2:3] mul:2
v_add_f64 v[6:7], v[0:1], v[2:3] clamp
v_add_f64 v[8:9], v[0:1], v[2:3] clamp div:2
v_add_f64 v[10:11], v[0:1], -v[2:3] mul:4
exp mrt0 v5, v7, v9, v11 done vm
s_endpgm
