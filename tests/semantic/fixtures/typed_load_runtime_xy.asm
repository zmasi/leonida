; buffer_load_format_xy with the V# in s[0:3] (push constants): its FORMAT and DST_SEL are known only at run time.
; The registers the load does not write keep their sentinels.
v_mov_b32 v2, 0xA5A5A5A5
v_mov_b32 v3, 0x5A5A5A5A
v_mov_b32 v4, 0x13579BDF
v_mov_b32 v5, 0x2468ACE0
v_mov_b32 v1, 0
buffer_load_format_xy v[2:3], v1, s[0:3], 0 offen
s_waitcnt vmcnt(0)
exp mrt0 v2, v3, v4, v5 done vm
s_endpgm
