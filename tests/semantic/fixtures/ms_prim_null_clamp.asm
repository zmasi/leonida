; Mesh shader (NGG, wave64), PSV0: 128 threads, 128/128 outputs, triangles. Two waves of 64 lanes.
; Thread t = s3[27:24] * 64 + mbcnt. Both waves send GS_ALLOC_REQ with m0 = M0 (the test puts
; 128 prims << 12 | V there, V the vertex count it checks).
; Every thread exports vertex t, pos0 = (t, 0, 0, 1), and primitive t, chosen by t & 3:
;   0: indices (t & 7, (t & 7) + 1, (t & 7) + 2)
;   1: null primitive (bit 31) whose fields are 300, 511 and 0x3FF (bit 9 set), all >= V
;   2: null primitive whose fields are (V, 0, 1): only field 0 is out of range, at the count itself
;   3: as 0, with the three edge flags (bits 9, 19, 29)
; The test replaces the tokens M0 and NULL_AT_COUNT (the whole word of case 2) with literals.
v_mbcnt_lo_u32_b32 v0, -1, 0
v_mbcnt_hi_u32_b32 v0, -1, v0
s_bfe_u32 s4, s3, 0x40018
v_lshl_add_u32 v0, s4, 6, v0
s_mov_b32 m0, M0
s_sendmsg sendmsg(MSG_GS_ALLOC_REQ)
v_cvt_f32_u32_e32 v1, v0
v_mov_b32_e32 v2, 0
v_mov_b32_e32 v3, 1.0
exp pos0 v1, v2, v2, v3 done
v_and_b32_e32 v4, 7, v0
v_add_nc_u32_e32 v5, 1, v4
v_add_nc_u32_e32 v6, 2, v4
v_lshlrev_b32_e32 v5, 10, v5
v_lshlrev_b32_e32 v6, 20, v6
v_or3_b32 v7, v4, v5, v6
v_and_b32_e32 v8, 3, v0
v_cmp_eq_u32_e32 vcc, 3, v8
v_cndmask_b32_e64 v9, 0, 0x20080200, vcc
v_or_b32_e32 v7, v7, v9
v_mov_b32_e32 v10, 0xbff7fd2c
v_cmp_eq_u32_e32 vcc, 1, v8
v_cndmask_b32_e32 v7, v7, v10, vcc
v_mov_b32_e32 v11, NULL_AT_COUNT
v_cmp_eq_u32_e32 vcc, 2, v8
v_cndmask_b32_e32 v7, v7, v11, vcc
exp prim v7, off, off, off done
s_endpgm
