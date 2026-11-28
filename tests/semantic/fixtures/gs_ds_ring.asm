; Three positions forming a full-screen triangle. The synthetic output signature
; spans 16 registers, so a 64-dword stride reaches the next emitted vertex.
v_mov_b32 v10, 256
v_mov_b32 v11, 512
ds_read_b32 v20, v11
ds_read_b32 v21, v10
s_waitcnt lgkmcnt(0)
v_add_f32 v20, -2.0, v20
v_mov_b32 v12, 0
v_mov_b32 v30, -1.0
v_mov_b32 v31, 0
v_mov_b32 v32, 1.0
ds_write_b32 v12, v30 offset:4
ds_write_b64 v12, v[31:32] offset:8
ds_write_b64 v12, v[30:31] offset:260
ds_write_b32 v12, v32 offset:268
ds_write_b32 v12, v30 offset:256
ds_write2_b32 v12, v20, v21 offset1:64
s_waitcnt lgkmcnt(0)
s_sendmsg sendmsg(MSG_GS, GS_OP_EMIT, 0)
s_sendmsg sendmsg(MSG_GS, GS_OP_EMIT, 0)
v_mov_b32 v40, -1.0
v_mov_b32 v41, 3.0
v_mov_b32 v42, 0
v_mov_b32 v43, 1.0
ds_write_b64 v12, v[40:41] offset:512
ds_write_b64 v12, v[42:43] offset:520
s_waitcnt lgkmcnt(0)
s_sendmsg sendmsg(MSG_GS, GS_OP_EMIT_CUT, 0)
s_endpgm
