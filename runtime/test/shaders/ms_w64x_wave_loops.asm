; Mesh shader (NGG, wave64, 128 threads = 2 waves): each wave runs a loop whose every iteration reads a wave-wide
; count, and the two waves leave it after a different number of iterations.
;   limit of a lane = (lane & 3) + 1 in wave 0, (lane & 7) + 1 in wave 1.
;   each iteration counts the lanes whose limit is above the iteration number (s_bcnt1 of a 64-lane compare), adds
;   the count to a sum and goes round again while the count is not 0.
;   wave 0: counts 64, 48, 32, 16, 0: 5 iterations, sum 160.  wave 1: 64, 56, ..., 8, 0: 9 iterations, sum 288.
; A wave whose sum is right sets the blue channel of param0 to 0.5 in the vertices it exports, else 0.
; The triangle is ms_tri's, with its vertices from threads 0 and 1 (wave 0) and thread 64 (wave 1), as in
; ms_w64x_wave_votes: the picture is ms_tri's only when both waves ran their own number of iterations with their
; own lanes. Under --wave64-emulate every count is an exchange with a workgroup barrier inside the loop, so the
; waves reach different numbers of barriers; with --mesh-serial-waves the two loops run one after the other.
; Only wave 0 sends the mesh allocation (1 primitive, 65 vertices: indices 0, 1 and 64 are used).
; Needs a DXBC with PSV0 (128 threads, 128/64 outputs, triangles; see scripts/run_gpu_tests.py):
;   VKH_MESH=1 vk_harness ms_w64x_wave_loops.spv fs_passthru.spv out.raw
;   -> every pixel = ((x+0.5)/64, (y+0.5)/64, 0.5, 1), identical to ms_tri.
; mattr: +wavefrontsize64,-wavefrontsize32
v_mbcnt_lo_u32_b32 v0, -1, 0
v_mbcnt_hi_u32_b32 v0, -1, v0
v_mov_b32_e32 v11, v0
s_bfe_u32 s4, s3, 0x40018
v_lshl_add_u32 v0, s4, 6, v0
s_mov_b32 s14, 288
s_cmp_eq_u32 s4, 0
s_cselect_b32 s5, 3, 7
s_cselect_b32 s14, 160, s14
v_and_b32_e32 v13, s5, v11
v_add_nc_u32_e32 v13, 1, v13
s_mov_b32 s20, 0
s_mov_b32 s22, 0
v_mov_b32_e32 v20, s20
v_cmp_lt_u32_e64 s[6:7], v20, v13
s_bcnt1_i32_b64 s10, s[6:7]
s_add_u32 s22, s22, s10
s_add_u32 s20, s20, 1
s_cmp_lg_u32 s10, 0
s_cbranch_scc1 65528
s_cmp_eq_u32 s22, s14
s_cselect_b32 s13, 1, 0
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
