; TEST-FMA-DENORM-PRODUCT: the four f32 FMA-family ops on an exact product below 2^-126 and a normal addend.
; x = s0, y = s1, z = s2 come from push constants, so a driver cannot fold them. The literals are fmamk's factor
; K = 2^-63 and fmaak's addend 0x00ffffff.
v_mov_b32_e32 v4, s0
v_mov_b32_e32 v5, s1
v_mov_b32_e32 v6, s2
; v0 = x * 2^-63 + z
v_fmamk_f32 v0, v4, 0x20000000, v6
; v1 = x * y + 0x00ffffff
v_fmaak_f32 v1, v4, v5, 0x00ffffff
; v2 = x * y + z
v_fma_f32 v2, v4, v5, v6
; v3 = x * y + z, accumulated into z
v_mov_b32_e32 v3, v6
v_fmac_f32_e32 v3, v4, v5
exp mrt0 v0, v1, v2, v3 done vm
s_endpgm
