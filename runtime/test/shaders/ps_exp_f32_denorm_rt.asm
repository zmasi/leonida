; Runtime-dependent v_exp_f32 inputs from interpolated pixel coordinates.
; x = floor(u * 64), exponent = -125 - x.
v_interp_p1_f32_e32 v2, v0, attr0.x
v_interp_p2_f32_e32 v2, v1, attr0.x
v_mul_f32_e32 v3, 0x42800000, v2
v_floor_f32_e32 v3, v3
v_sub_f32_e32 v4, 0xc2fa0000, v3
v_exp_f32_e32 v5, v4
exp mrt0 v5, v4, v3, v5 done vm
s_endpgm
