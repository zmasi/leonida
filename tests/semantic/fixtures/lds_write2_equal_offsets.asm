; Distinct DATA0 and DATA1 let each equal-offset write expose which element wins.
; Read back with single-address instructions so paired reads cannot mask a store error.
v_cmpx_eq_u32_e32 0, v0
v_mov_b32_e32 v1, 0
v_mov_b32_e32 v4, 0x11111111
v_mov_b32_e32 v5, 0x22222222
v_mov_b32_e32 v6, 0x33333333
v_mov_b32_e32 v7, 0x44444444

ds_write2_b32 v1, v4, v6 offset0:3 offset1:3
ds_write2st64_b32 v1, v4, v6 offset0:2 offset1:2
ds_write2_b64 v1, v[4:5], v[6:7] offset0:3 offset1:3
ds_write2st64_b64 v1, v[4:5], v[6:7] offset0:2 offset1:2
ds_write2_b64 v1, v[4:5], v[6:7]
s_waitcnt lgkmcnt(0)

ds_read_b32 v8, v1 offset:12
ds_read_b32 v9, v1 offset:512
ds_read_b64 v[10:11], v1 offset:24
ds_read_b64 v[12:13], v1 offset:1024
ds_read_b64 v[14:15], v1
s_waitcnt lgkmcnt(0)

buffer_store_dword v8, v1, s[0:3], 0 offen
buffer_store_dword v9, v1, s[0:3], 0 offen offset:4
buffer_store_dword v10, v1, s[0:3], 0 offen offset:8
buffer_store_dword v11, v1, s[0:3], 0 offen offset:12
buffer_store_dword v12, v1, s[0:3], 0 offen offset:16
buffer_store_dword v13, v1, s[0:3], 0 offen offset:20
buffer_store_dword v14, v1, s[0:3], 0 offen offset:24
buffer_store_dword v15, v1, s[0:3], 0 offen offset:28
s_endpgm
