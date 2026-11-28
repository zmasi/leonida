; Mesh shader (NGG, wave64, 128 threads = 2 waves): CORRECTNESS CONTROL for a trip count that comes from v_readlane_b32
; and a loop that is nothing but scalar integer arithmetic. Written as a positive control: it checks the value of
; that scalar arithmetic and the number of iterations, and nothing else.
;   Excluded shape: the loop body has NO lane exchange (no v_readlane, readfirstlane, DPP, bpermute, ballot or vote),
;   NO workgroup barrier, NO LDS (the program has no ds_ instruction) and NO memory access; EXEC is not changed in
;   it. The only branches are scalar and uniform. This file does not put a barrier, a lane exchange or a wave vote in
;   a loop whose count was read from a lane, and it is not a reproduction of anything that does.
;   Before the loop (straight-line): v2 = 3 * lane + 1 + 2 * wave, lane = 0..63 within the wave, wave = s3[27:24].
;   v_readlane_b32 s6, v2, 40 reads lane 40, EXEC all lanes: 121 in wave 0 and 123 in wave 1 (lane 40 is in the upper
;   half of the wave, so a 32-wide wrap would read lane 8 = 25 / 27). Both are odd; every odd lane gives an even
;   value, so reading lane 39 or 41 gives 118/124 (wave 0), 2 iterations and another result.
;   Count: s11 = min((s6 & 1) + 2, 3) = 3 in both waves (2 for an even value). s12 counts the iterations as well and
;   leaves the loop at 3 whatever s11 holds, so the loop cannot run more than 3 times.
;   Loop, in this order, while s11 != 0 and s12 < 3 (a do-while, uniform scalar branches only):
;       s14 = s14 * 5 + s6     (s14 starts at 7)    s12 += 1     s11 -= 1
;   Wave 0: s6 = 121: 156, 901, 4626.   Wave 1: s6 = 123: 158, 913, 4688.   (two iterations would give 901 / 913.)
;   After the loop each wave compares s14 with its own value (4626 / 4688), s12 with 3 and s11 with 0; blue = 0.5 if
;   all three hold, else 0. The values are not constants of the exports: without the loop s14 stays 7.
;   Every transfer and the exports come after the loop; there is no s_barrier in the program. Only wave 0 sends the
;   mesh allocation (1 primitive, 65 vertices: indices 0, 1 and 64 are used), as in ms_w64x_wave_loops.
; The triangle is ms_tri's, with its vertices from threads 0 and 1 (wave 0) and thread 64 (wave 1), as in
; ms_w64x_lds_wave0: the picture is ms_tri's only when wave 1 ran its loop with its own lane 40.
; Needs a DXBC with PSV0 (128 threads, 128/64 outputs, triangles; see scripts/run_gpu_tests.py):
;   VKH_MESH=1 vk_harness ms_w64x_ctl_readlane_alu.spv fs_passthru.spv out.raw
;   -> every pixel (all 4096) = ((x+0.5)/64, (y+0.5)/64, 0.5, 1), identical to ms_tri. A wrong lane, a wrong trip
;   count or a wave that did not run the loop gives blue 0 on its vertex, so the blue channel is not 0.5.
; mattr: +wavefrontsize64,-wavefrontsize32
v_mbcnt_lo_u32_b32 v0, -1, 0
v_mbcnt_hi_u32_b32 v0, -1, v0
v_mov_b32_e32 v11, v0
s_bfe_u32 s4, s3, 0x40018
v_lshl_add_u32 v0, s4, 6, v0
v_mad_u32_u24 v2, v11, 3, 1
s_lshl_b32 s5, s4, 1
v_add_nc_u32_e32 v2, s5, v2
v_readlane_b32 s6, v2, 40
s_and_b32 s11, s6, 1
s_add_u32 s11, s11, 2
s_min_u32 s11, s11, 3
s_mov_b32 s12, 0
s_mov_b32 s14, 7
s_mul_i32 s14, s14, 5
s_add_u32 s14, s14, s6
s_add_u32 s12, s12, 1
s_sub_u32 s11, s11, 1
s_cmp_ge_u32 s12, 3
s_cbranch_scc1 2
s_cmp_lg_u32 s11, 0
s_cbranch_scc1 65528
s_mov_b32 s15, 4688
s_cmp_eq_u32 s4, 0
s_cselect_b32 s15, 4626, s15
s_cmp_eq_u32 s14, s15
s_cselect_b32 s13, 1, 0
s_cmp_eq_u32 s12, 3
s_cselect_b32 s13, s13, 0
s_cmp_eq_u32 s11, 0
s_cselect_b32 s13, s13, 0
v_cvt_f32_u32_e32 v15, s13
v_mul_f32_e32 v15, 0.5, v15
v_cmp_eq_u32_e32 vcc, 1, v0
v_cndmask_b32_e64 v1, -1.0, 0x40400000, vcc
v_cndmask_b32_e64 v3, 0, 2.0, vcc
v_cmp_eq_u32_e32 vcc, 64, v0
v_cndmask_b32_e64 v2, -1.0, 0x40400000, vcc
v_cndmask_b32_e64 v4, 0, 2.0, vcc
v_mov_b32_e32 v6, 0
v_mov_b32_e32 v7, 1.0
v_mov_b32_e32 v8, v15
s_mov_b32 m0, 0x1041
s_cmp_lg_u32 s4, 0
s_cbranch_scc1 1
s_sendmsg sendmsg(MSG_GS_ALLOC_REQ)
v_cmp_gt_u32_e64 s[16:17], 2, v0
v_cmp_eq_u32_e64 s[18:19], 64, v0
s_or_b64 exec, s[16:17], s[18:19]
exp pos0 v1, v2, v6, v7 done
exp param0 v3, v4, v8, v7
s_mov_b64 exec, -1
v_cmpx_eq_u32_e32 0, v0
v_mov_b32_e32 v9, 0x4000400
exp prim v9, off, off, off done
s_endpgm
