; Each signed i16 lane of SRC0 saturates to an unsigned byte and packs into D[15:0]; the
; destination's high half is kept (the GFX10 16-bit partial write, as v_cvt_f16_f32).
; Expected MRT0 words: cafeff00 dead00ff 00008000 abcdffff.
	v_mov_b32 v1, 0x0100fed4
	v_mov_b32 v2, 0xcafebabe
	v_sat_pk_u8_i16 v2, v1
	v_mov_b32 v3, 0x80407fff
	v_mov_b32 v4, 0xdeadbeef
	v_sat_pk_u8_i16 v4, v3
	v_mov_b32 v5, 0x00000000
	v_mov_b32 v6, 0x0080ff00
	v_sat_pk_u8_i16_e64 v5, v6
	v_mov_b32 v7, 0xabcd0000
	v_sat_pk_u8_i16 v7, 0x00ff0100
	exp mrt0 v2, v4, v5, v7 done vm
	s_endpgm
