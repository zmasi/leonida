; v_div_fixup_f32 without output modifiers: D = fixup(S0=v1 quotient, S1=v2 denominator, S2=v3 numerator).
; No buffer is touched, so the emitted SPIR-V is the lowering and nothing else: this is the fixture
; tests/semantic/test_valu_div_fixup.py pins the integer op order, constants and NaN priority with.
v_mov_b32 v1, 0x3f800000
v_mov_b32 v2, 0x40000000
v_mov_b32 v3, 0x40400000
v_div_fixup_f32 v4, v1, v2, v3
exp mrt0 v4, v4, v4, v4 done vm
s_endpgm
