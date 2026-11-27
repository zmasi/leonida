; Over-limit mesh for d3d12_mesh_compute_test (no draw, creation must fail).
; ms_tri's triangle (NGG, wave64, 128 threads, threads 0-2 export, thread 0 the
; primitive) with 32 vec4 params (param0..param31, all (u, v, 0.5, 1)): the
; generated vertex shader exports position plus 32 locations, 4 + 32*4 = 132
; components, over the 128-component vertex-output/fragment-input limits on
; Vega 20 and most RX (queried at runtime; the test branches, never spoofs).
; Translated plainly with --mesh-compute (no f16/bary flags); the pair seals
; normally, then CreatePipelineState refuses E_NOTIMPL with a nulled pointer.
v_mbcnt_lo_u32_b32 v0, -1, 0
v_mbcnt_hi_u32_b32 v0, -1, v0
s_bfe_u32 s4, s3, 0x40018
v_lshl_add_u32 v0, s4, 6, v0
v_cmp_eq_u32_e32 vcc_lo, 1, v0
v_cndmask_b32_e64 v1, -1.0, 0x40400000, vcc_lo
v_cndmask_b32_e64 v3, 0, 2.0, vcc_lo
v_cmp_eq_u32_e32 vcc_lo, 2, v0
v_cndmask_b32_e64 v2, -1.0, 0x40400000, vcc_lo
v_cndmask_b32_e64 v4, 0, 2.0, vcc_lo
v_mov_b32_e32 v6, 0
v_mov_b32_e32 v7, 1.0
v_mov_b32_e32 v8, 0.5
s_mov_b32 m0, 0x1003
s_sendmsg sendmsg(MSG_GS_ALLOC_REQ)
s_barrier
v_cmpx_gt_u32_e32 3, v0
exp pos0 v1, v2, v6, v7 done
exp param0 v3, v4, v8, v7
exp param1 v3, v4, v8, v7
exp param2 v3, v4, v8, v7
exp param3 v3, v4, v8, v7
exp param4 v3, v4, v8, v7
exp param5 v3, v4, v8, v7
exp param6 v3, v4, v8, v7
exp param7 v3, v4, v8, v7
exp param8 v3, v4, v8, v7
exp param9 v3, v4, v8, v7
exp param10 v3, v4, v8, v7
exp param11 v3, v4, v8, v7
exp param12 v3, v4, v8, v7
exp param13 v3, v4, v8, v7
exp param14 v3, v4, v8, v7
exp param15 v3, v4, v8, v7
exp param16 v3, v4, v8, v7
exp param17 v3, v4, v8, v7
exp param18 v3, v4, v8, v7
exp param19 v3, v4, v8, v7
exp param20 v3, v4, v8, v7
exp param21 v3, v4, v8, v7
exp param22 v3, v4, v8, v7
exp param23 v3, v4, v8, v7
exp param24 v3, v4, v8, v7
exp param25 v3, v4, v8, v7
exp param26 v3, v4, v8, v7
exp param27 v3, v4, v8, v7
exp param28 v3, v4, v8, v7
exp param29 v3, v4, v8, v7
exp param30 v3, v4, v8, v7
exp param31 v3, v4, v8, v7
v_cmpx_eq_u32_e32 0, v0
v_mov_b32_e32 v9, 0x200400
exp prim v9, off, off, off done
s_endpgm
