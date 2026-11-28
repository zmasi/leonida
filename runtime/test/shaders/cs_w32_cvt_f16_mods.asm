; f16 conversion and DX10 clamp regressions. Mix VGPR inputs with the local lane ID so the shader
; converts runtime values. Lane 0 stores the eight words for vk_compute_test's dump mode.
;   out[0] = 0xffc4a000: DPP fetches lane 1's 0x7e25, widens the NaN and negates its sign
;   out[1] = 0x33800000: widen BYTE_1 of 0x00000100 (2^-24)
;   out[2] = 0x36800000: widen BYTE_1 of inline f16 2.0 (0x4000 -> 0x0040)
;   out[3] = 0xaaaa0001: narrow and clamp 2^-24, keeping the destination's high half
;   out[4] = 0xaaaa03ff: narrow and clamp the largest positive f16 subnormal
;   out[5] = 0xaaaa0000: div:2 flushes the half result of 2^-14 before clamp
;   out[6] = 0xaaaa0000: narrow and clamp -0.0
;   out[7] = 0xaaaa0001: clamp the subnormal quotient of a normal division fixup
; group: 32,1,1
; mattr: +wavefrontsize32,-wavefrontsize64
v_xor_b32_e32 v1, 0x7e24, v0
v_cvt_f32_f16_dpp v8, -v1 quad_perm:[1,0,3,2] row_mask:0xf bank_mask:0xf bound_ctrl:1
v_xor_b32_e32 v1, 0x0100, v0
v_cvt_f32_f16_sdwa v9, v1 dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1
v_cvt_f32_f16_sdwa v10, 2.0 dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1
v_xor_b32_e32 v2, 0x33800000, v0
v_xor_b32_e32 v3, 0x387fc000, v0
v_xor_b32_e32 v4, 0x38800000, v0
v_xor_b32_e32 v5, 0x80000000, v0
v_mov_b32_e32 v11, 0xaaaa5555
v_mov_b32_e32 v12, v11
v_mov_b32_e32 v13, v11
v_mov_b32_e32 v14, v11
v_mov_b32_e32 v15, v11
v_cvt_f16_f32_e64 v11, v2 clamp
v_cvt_f16_f32_e64 v12, v3 clamp
v_cvt_f16_f32_e64 v13, v4 clamp div:2
v_cvt_f16_f32_e64 v14, v5 clamp
v_xor_b32_e32 v6, 1, v0
v_mov_b32_e32 v7, 0x3c00
v_div_fixup_f16 v15, v6, v7, v7 clamp
v_cmpx_eq_u32_e32 0, v0
v_mov_b32_e32 v16, 0
buffer_store_dword v8, v16, s[0:3], 0 offen offset:0
buffer_store_dword v9, v16, s[0:3], 0 offen offset:4
buffer_store_dword v10, v16, s[0:3], 0 offen offset:8
buffer_store_dword v11, v16, s[0:3], 0 offen offset:12
buffer_store_dword v12, v16, s[0:3], 0 offen offset:16
buffer_store_dword v13, v16, s[0:3], 0 offen offset:20
buffer_store_dword v14, v16, s[0:3], 0 offen offset:24
buffer_store_dword v15, v16, s[0:3], 0 offen offset:28
s_endpgm
