; v_exp_f32 flushes subnormal results to positive zero.
; Expected mrt0 bits: 0x01000000, 0x00800000, 0x00000000, 0x00000000
v_mov_b32_e32 v0, 0xc2fa0000
v_mov_b32_e32 v1, 0xc2fc0000
v_mov_b32_e32 v2, 0xc2fe0000
v_mov_b32_e32 v3, 0xc3150000
v_exp_f32_e32 v4, v0
v_exp_f32_e32 v5, v1
v_exp_f32_e32 v6, v2
v_exp_f32_e32 v7, v3
exp mrt0 v4, v5, v6, v7 done vm
s_endpgm
