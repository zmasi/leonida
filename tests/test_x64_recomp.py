"""Unit tests for tools/x64_recomp.py: the x86-64 -> arm64 static recompiler groundwork.

Two layers are covered:

  * decoder: hand-assembled x86-64 bytes -> Insn fields,
  * end-to-end: a hand-assembled x86-64 function is translated to arm64, packed into a
    flat kernel image and executed under qemu-system-aarch64 with semihosting. The
    runtime prints "R=xxxxxxxx" (the guest edi) which the test compares against an
    expected value.

Every fixture is built byte by byte here; no game files, no third-party packages.
The qemu tests skip cleanly when qemu-system-aarch64 is not installed.

    python -I tests/test_x64_recomp.py
"""
import os
import shutil
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
TOOLS_DIR = os.path.join(ROOT, "tools")
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import x64_recomp as x  # noqa: E402

QEMU = os.environ.get(
    "QEMU_AARCH64",
    "/home/nolan/.nix-profile/bin/qemu-system-aarch64"
    if os.path.exists("/home/nolan/.nix-profile/bin/qemu-system-aarch64")
    else shutil.which("qemu-system-aarch64") or "")
HAS_QEMU = bool(QEMU) and os.path.exists(QEMU)

GUEST_VA = 0x46000000
SCRATCH = 0x47100000  # inside the kernel image's RW RAM window, away from ctx/stack


def asm(*hexes):
    return bytes.fromhex("".join(h.replace(" ", "") for h in hexes))


class DecodeTests(unittest.TestCase):
    def one(self, code, base=0x1000):
        return x.decode_one(code, 0, len(code), base)

    def test_basic_forms(self):
        ins = self.one(asm("31 c0"))            # xor eax,eax
        self.assertEqual(ins.mnem, "xor")
        self.assertEqual(ins.ops, (("r", 0, 4), ("r", 0, 4, False)))
        self.assertEqual(ins.size, 2)

        ins = self.one(asm("48 89 e5"))         # mov rbp,rsp
        self.assertEqual(ins.mnem, "mov")
        self.assertEqual(ins.ops, (("r", 5, 8), ("r", 4, 8, False)))

        ins = self.one(asm("b8 05 00 00 00"))   # mov eax,5
        self.assertEqual(ins.ops, (("r", 0, 4, False), ("i", 5, 4)))

        ins = self.one(asm("83 c0 03"))         # add eax,3
        self.assertEqual(ins.mnem, "add")
        self.assertEqual(ins.ops, (("r", 0, 4), ("i", 3, 1)))

        ins = self.one(asm("48 b8 88 77 66 55 44 33 22 11"))  # mov rax,imm64
        self.assertEqual(ins.size, 10)
        self.assertEqual(ins.ops, (("r", 0, 8, False), ("i", 0x1122334455667788, 8)))

        ins = self.one(asm("b4 ff"))            # mov ah,-1 (no REX: O-field 4..7 = hi8)
        self.assertEqual(ins.ops, (("r", 0, 1, True), ("i", -1, 1)))

    def test_rex_and_extended_regs(self):
        ins = self.one(asm("4c 89 f8"))         # mov rax,r15
        self.assertEqual(ins.ops, (("r", 0, 8), ("r", 15, 8, False)))
        ins = self.one(asm("41 54"))            # push r12
        self.assertEqual(ins.ops, (("r", 12, 8, False),))

    def test_opcode_90_nop_pause_and_rex_xchg(self):
        ins = self.one(asm("90"))
        self.assertEqual((ins.mnem, ins.ops, ins.size, ins.unknown),
                         ("nop", (), 1, False))

        ins = self.one(asm("f3 90"))
        self.assertEqual((ins.mnem, ins.ops, ins.size, ins.unknown),
                         ("pause", (), 2, False))

        ins = self.one(asm("41 90"))
        self.assertEqual((ins.mnem, ins.ops, ins.size, ins.unknown),
                         ("xchg", (("r", 8, 8, False), ("r", 0, 8, False)), 2, False))

    def test_modrm_mem(self):
        ins = self.one(asm("48 8b 44 24 08"))   # mov rax,[rsp+8]
        m = ins.ops[1][1]
        self.assertEqual(m["base"], 4)
        self.assertIsNone(m["index"])
        self.assertEqual(m["disp"], 8)

        ins = self.one(asm("48 8b 04 b7"))      # mov rax,[rdi+rsi*4]
        m = ins.ops[1][1]
        self.assertEqual(m["base"], 7)
        self.assertEqual(m["index"], 6)
        self.assertEqual(m["scale"], 2)

        ins = self.one(asm("48 8b 05 10 00 00 00"))  # mov rax,[rip+0x10]
        m = ins.ops[1][1]
        self.assertTrue(m["rip"])
        self.assertEqual(m["abs"], 0x1000 + 7 + 0x10)

    def test_branches_and_immediates(self):
        ins = self.one(asm("74 05"))            # jz +5
        self.assertEqual(ins.kind, "branch")
        self.assertEqual(ins.cc, "e")
        self.assertEqual(ins.ops[0], ("a", 0x1007, 0))

        ins = self.one(asm("e9 78 56 34 12"))   # jmp rel32
        self.assertEqual(ins.ops[0], ("a", 0x1000 + 5 + 0x12345678, 0))

        ins = self.one(asm("e8 fc ff ff ff"))   # call -4
        self.assertEqual(ins.kind, "call")
        self.assertEqual(ins.ops[0], ("a", 0x1000 + 5 - 4, 0))

        ins = self.one(asm("c3"))
        self.assertEqual(ins.kind, "ret")

    def test_cmov_setcc_shifts(self):
        ins = self.one(asm("0f 44 c1"))         # cmove eax,ecx
        self.assertEqual(ins.mnem, "cmov")
        self.assertEqual(ins.cc, "e")
        ins = self.one(asm("0f 95 c0"))         # setnz al
        self.assertEqual(ins.mnem, "setcc")
        self.assertEqual(ins.cc, "ne")
        ins = self.one(asm("d3 e0"))            # shl eax,cl
        self.assertEqual(ins.mnem, "shl")
        ins = self.one(asm("c1 e8 04"))         # shr eax,4
        self.assertEqual(ins.mnem, "shr")
        self.assertEqual(ins.ops[1], ("i", 4, 1))

    def test_shift_zero_count_flags(self):
        # Shifts and rotates with immediate count 0 (or masked to 0) affect no flags in x86
        for hexcode in ("c1 e0 00", "c1 e8 00", "c1 f8 00", "c1 c0 00", "c1 c8 00"):
            ins = self.one(asm(hexcode))
            self.assertEqual(ins.flags_w, "")
            self.assertEqual(ins.flags_r, "")

        # 32-bit shift masked with 0x1f: count 32 -> 0
        ins = self.one(asm("c1 e0 20"))
        self.assertEqual(ins.flags_w, "")

        # 64-bit shift masked with 0x3f: count 64 -> 0
        ins = self.one(asm("48 c1 e0 40"))
        self.assertEqual(ins.flags_w, "")

        # Non-zero counts write flags
        ins = self.one(asm("c1 e0 01"))
        self.assertEqual(ins.flags_w, "NZCP")
        ins = self.one(asm("d3 e0"))
        self.assertEqual(ins.flags_w, "NZCP")

    def test_prefixes(self):
        ins = self.one(asm("66 b8 34 12"))      # mov ax,0x1234
        self.assertEqual(ins.ops, (("r", 0, 2, False), ("i", 0x1234, 2)))
        ins = self.one(asm("f3 a4"))            # rep movsb
        self.assertEqual(ins.mnem, "movs")
        self.assertEqual(ins.rep, "f3")
        ins = self.one(asm("f2 ae"))            # repne scasb
        self.assertEqual(ins.mnem, "scas")
        self.assertEqual(ins.rep, "f2")
        ins = self.one(asm("f3 0f 1e fa"))      # endbr64 (decoded, no crash)
        self.assertEqual(ins.size, 4)

    def test_group7_0f01_carries_modrm(self):
        # 0F 01 /r is the system-instruction group. Every form has a ModRM byte, so
        # reading only the two opcode bytes stops the walk one byte early.
        for code, mnem in (("0f 01 fc", "clzero"), ("0f 01 d0", "xgetbv"),
                           ("0f 01 fa", "monitorx"), ("0f 01 fb", "mwaitx"),
                           ("0f 01 c8", "monitor"), ("0f 01 f9", "rdtscp"),
                           ("0f 01 f8", "swapgs"), ("0f 01 e0", "smsw")):
            ins = self.one(asm(code))
            self.assertEqual(ins.size, 3, code)
            self.assertEqual(ins.mnem, mnem, code)
            self.assertEqual(ins.kind, "sys", code)

        # the memory forms keep their ModRM/SIB/displacement length
        ins = self.one(asm("0f 01 00"))              # sgdt [rax]
        self.assertEqual((ins.mnem, ins.size), ("sgdt", 3))
        ins = self.one(asm("0f 01 44 24 10"))        # sgdt [rsp+0x10]
        self.assertEqual((ins.mnem, ins.size), ("sgdt", 5))
        ins = self.one(asm("0f 01 7c 24 10"))        # invlpg [rsp+0x10]
        self.assertEqual((ins.mnem, ins.size), ("invlpg", 5))


class ShiftCarryAnalysisTests(unittest.TestCase):
    def test_imm_shift_zero_preserves_reaching_producer(self):
        # cmp r8d, 2; shl eax, 0; setc dl; ret
        code = asm("41 83 f8 02", "c1 e0 00", "0f 92 c2", "c3")
        insns = list(x.disasm(code, 0x1000))
        tr = x.FunctionTranslator(insns)
        tr.translate()
        setc_ins = next(i for i in insns if i.mnem == "setcc")
        self.assertEqual(tr.arm_path[setc_ins.addr].mnem, "cmp")

    def test_var_shift_consumer_reads_ctx_and_the_writer_before_stores(self):
        # cmp r8d, 2; mov cl, 0; shl eax, cl; setc dl; ret
        code = asm("41 83 f8 02", "b1 00", "d3 e0", "0f 92 c2", "c3")
        insns = list(x.disasm(code, 0x1000))
        tr = x.FunctionTranslator(insns)
        tr.translate()
        setc_ins = next(i for i in insns if i.mnem == "setcc")
        cmp_ins = next(i for i in insns if i.mnem == "cmp")
        shl_ins = next(i for i in insns if i.mnem == "shl")
        self.assertIsNone(tr.arm_path[setc_ins.addr])
        self.assertIn(cmp_ins.addr, tr.mat)
        self.assertIn(shl_ins.addr, tr.mat)

    def test_var_shift_keeps_the_carry_live_into_its_block(self):
        # shl eax, cl; setc dl; ret: the carry may come from before the block
        code = asm("d3 e0", "0f 92 c2", "c3")
        insns = list(x.disasm(code, 0x1000))
        tr = x.FunctionTranslator(insns)
        tr.translate()
        self.assertIn("C", tr.live_in[insns[0].addr])

    def test_var_shift_has_the_writer_in_the_block_before_store(self):
        # cmp r8d, 2; jmp +0; mov cl, 0; shl eax, cl; setc dl; ret
        code = asm("41 83 f8 02", "eb 00", "b1 00", "d3 e0", "0f 92 c2", "c3")
        insns = list(x.disasm(code, 0x1000))
        tr = x.FunctionTranslator(insns)
        tr.translate()
        cmp_ins = next(i for i in insns if i.mnem == "cmp")
        self.assertIn(cmp_ins.addr, tr.mat)


def run(code):
    return x.run_result(code, GUEST_VA)


@unittest.skipUnless(HAS_QEMU, "qemu-system-aarch64 not installed")
class ExecTests(unittest.TestCase):
    def test_mov_add_ret(self):
        # mov eax,40; add eax,2; mov edi,eax; ret -> 42
        r, _ = run(asm("b8 28 00 00 00", "83 c0 02", "89 c7", "c3"))
        self.assertEqual(r, 42)

    def test_sub_cmp_jcc_loop(self):
        # sum = 0; ecx = 5; do { sum += ecx } while (--ecx != 0) -> 15
        # mov edi,0; mov ecx,5; L: add edi,ecx; dec ecx; jnz L; ret
        r, _ = run(asm(
            "bf 00 00 00 00",       # mov edi,0
            "b9 05 00 00 00",       # mov ecx,5
            "01 cf",                # L: add edi,ecx
            "ff c9",                # dec ecx
            "75 fa",                # jnz -6
            "c3"))
        self.assertEqual(r, 15)

    def test_unsigned_branch(self):
        # cmp below/above: eax=3 vs 7 -> jb taken -> edi=1
        r, _ = run(asm(
            "b8 03 00 00 00",       # mov eax,3
            "83 f8 07",             # cmp eax,7
            "72 06",                # jb +6
            "bf 02 00 00 00",       # mov edi,2
            "c3",
            "bf 01 00 00 00",       # mov edi,1
            "c3"))
        self.assertEqual(r, 1)

    def test_signed_branch(self):
        # -1 < 7 signed -> jl taken
        r, _ = run(asm(
            "b8 ff ff ff ff",       # mov eax,-1
            "83 f8 07",             # cmp eax,7
            "7c 06",                # jl +6
            "bf 02 00 00 00",       # mov edi,2
            "c3",
            "bf 01 00 00 00",       # mov edi,1
            "c3"))
        self.assertEqual(r, 1)

    def test_call_and_ret(self):
        # 00 call ->0x0c (callee sets edi=9, ret pops the pushed return ->0x05)
        # 05 mov edi,7 ; 0a jmp ->0x12 ; 0c mov edi,9 ; 11 ret ; 12 ret
        r, _ = run(asm(
            "e8 07 00 00 00",       # call +7 -> 0x0c
            "bf 07 00 00 00",       # mov edi,7   (return lands here)
            "eb 06",                # jmp +6 -> 0x12
            "bf 09 00 00 00",       # callee: mov edi,9
            "c3",                   # ret -> guest pc 0x05
            "c3"))                  # end: ret -> sentinel -> exit
        self.assertEqual(r, 7)

    def test_setcc_after_cmp(self):
        # setg al when 9 > 4 -> edi=1
        r, _ = run(asm(
            "b8 09 00 00 00",       # mov eax,9
            "83 f8 04",             # cmp eax,4
            "0f 9f c7",             # setg dil
            "0f b6 ff",             # movzx edi,dil
            "c3"))
        self.assertEqual(r, 1)

    def test_cmov(self):
        # cmovne: equal -> keep old; not-equal -> take new
        r, _ = run(asm(
            "bf 05 00 00 00",       # mov edi,5
            "b8 01 00 00 00",       # mov eax,1
            "83 f8 02",             # cmp eax,2 (ne)
            "bb 2a 00 00 00",       # mov ebx,42
            "0f 45 fb",             # cmovne edi,ebx
            "c3"))
        self.assertEqual(r, 42)

    def test_logic_clears_carry(self):
        # stc then xor (clears CF) -> jae taken -> edi=1
        r, _ = run(asm(
            "f9",                   # stc
            "31 c0",                # xor eax,eax
            "73 06",                # jae +6
            "bf 02 00 00 00",
            "c3",
            "bf 01 00 00 00",
            "c3"))
        self.assertEqual(r, 1)

    def test_adc_chain(self):
        # 64-bit add via adc: lo=0xFFFFFFFF + 1 -> lo 0, carry into hi
        r, _ = run(asm(
            "b8 ff ff ff ff",       # mov eax,-1
            "31 d2",                # xor edx,edx
            "83 c0 01",             # add eax,1   (CF=1)
            "83 d2 00",             # adc edx,0   (edx=1)
            "89 d7",                # mov edi,edx
            "c3"))
        self.assertEqual(r, 1)

    def test_push_pop(self):
        r, _ = run(asm(
            "68 34 12 00 00",       # push 0x1234
            "5f",                   # pop rdi
            "c3"))
        self.assertEqual(r, 0x1234)

    def test_imul(self):
        r, _ = run(asm(
            "b8 06 00 00 00",       # mov eax,6
            "6b c0 07",             # imul eax,eax,7
            "89 c7",                # mov edi,eax
            "c3"))
        self.assertEqual(r, 42)

    def test_shifts(self):
        r, _ = run(asm(
            "b8 01 00 00 00",       # mov eax,1
            "c1 e0 05",             # shl eax,5 -> 32
            "c1 e8 02",             # shr eax,2 -> 8
            "89 c7",
            "c3"))
        self.assertEqual(r, 8)

    def test_shift_sets_carry(self):
        # shr sets CF=last bit out; jb reflects it
        r, _ = run(asm(
            "b8 03 00 00 00",       # mov eax,3
            "d1 e8",                # shr eax,1 -> CF=1
            "72 06",                # jb +6
            "bf 02 00 00 00",
            "c3",
            "bf 01 00 00 00",
            "c3"))
        self.assertEqual(r, 1)

    def test_memory(self):
        # mov [scratch],eax; mov edi,[scratch]
        s = SCRATCH
        r, _ = run(asm(
            "b8 2a 00 00 00",                    # mov eax,42
            "a3 " + s.to_bytes(8, "little").hex(),  # mov [scratch],eax (moffs)
            "a1 " + s.to_bytes(8, "little").hex(),  # mov eax,[scratch]
            "89 c7",
            "c3"))
        self.assertEqual(r, 42)

    def test_lea_and_indexed_mem(self):
        s = SCRATCH
        r, _ = run(asm(
            "bf " + s.to_bytes(4, "little").hex(),   # mov edi,scratch
            "c7 07 63 00 00 00",                      # mov dword [rdi],99
            "be " + (s + 4).to_bytes(4, "little").hex(),  # mov esi,scratch+4
            "c7 06 58 02 00 00",                      # mov dword [rsi],600
            "8b 07",                                   # mov eax,[rdi]   =99
            "03 06",                                   # add eax,[rsi]   +600
            "89 c7",
            "c3"))
        self.assertEqual(r, 699)

    def test_movsx_movzx(self):
        r, _ = run(asm(
            "b1 ff",                # mov cl,-1
            "0f be f9",             # movsx edi,cl -> -1
            "c3"))
        self.assertEqual(r, 0xFFFFFFFF)

    def test_neg_not_inc_dec(self):
        r, _ = run(asm(
            "b8 05 00 00 00",       # mov eax,5
            "f7 d8",                # neg eax -> -5
            "ff c0",                # inc eax -> -4
            "f7 d0",                # not eax -> ~-4 = 3
            "ff c8",                # dec eax -> 2
            "89 c7",
            "c3"))
        self.assertEqual(r, 2)

    def test_div(self):
        r, _ = run(asm(
            "31 d2",                # xor edx,edx
            "b8 e7 03 00 00",       # mov eax,999
            "b9 0a 00 00 00",       # mov ecx,10
            "f7 f1",                # div ecx -> eax=99 edx=9
            "89 c7",
            "c3"))
        self.assertEqual(r, 99)

    def test_rep_movsb(self):
        s = SCRATCH
        dst = SCRATCH + 0x100
        r, _ = run(asm(
            "be " + s.to_bytes(4, "little").hex(),          # mov esi,src
            "c7 06 41 42 43 44",                            # src = 'ABCD'
            "bf " + dst.to_bytes(4, "little").hex(),        # mov edi,dst
            "b9 04 00 00 00",                               # mov ecx,4
            "f3 a4",                                        # rep movsb -> rdi=dst+4
            "8b 7f fc",                                     # mov edi,[rdi-4] = the 4 copied bytes
            "c3"))
        self.assertEqual(r, 0x44434241)

    def test_repne_scasb(self):
        s = SCRATCH
        r, _ = run(asm(
            "bf " + s.to_bytes(4, "little").hex(),          # mov edi,buf
            "c7 07 58 58 51 00",                            # buf = 'XXQ\0'
            "b0 51",                                        # mov al,'Q'
            "b9 05 00 00 00",                               # mov ecx,5
            "f2 ae",                                        # repne scasb
            # found -> rdi = buf+3, ZF=1 -> sete
            "0f 94 c0",                                     # sete al
            "0f b6 f8",                                     # movzx edi,al
            "c3"))
        self.assertEqual(r, 1)

    def test_indirect_jump_via_table(self):
        # jmp rax where rax = target inside the function -> dispatch table lookup
        r, _ = run(asm(
            "48 b8 " + (GUEST_VA + 18).to_bytes(8, "little").hex(),  # mov rax,target (0x12)
            "ff e0",                                                 # jmp rax
            "bf 01 00 00 00",                                        # (skipped)
            "c3",
            "bf 2b 00 00 00",                                        # target: mov edi,43
            "c3"))
        self.assertEqual(r, 43)

    def test_mov_imm64(self):
        # mov r64,imm64 is the only insn with a full 8-byte immediate
        r, _ = run(asm(
            "48 b8 " + (GUEST_VA + 13).to_bytes(8, "little").hex(),  # mov rax,V+13
            "48 8d 78 00",                                           # lea rdi,[rax]
            "48 81 ef " + GUEST_VA.to_bytes(4, "little").hex(),      # sub rdi,GUEST_VA
            "c3"))
        self.assertEqual(r, 13)

    def test_high_byte_regs(self):
        # b4 = mov ah,imm8 (no REX -> reg field 4..7 are the high bytes of regs 0..3)
        r, _ = run(asm(
            "b4 ff",                # mov ah,0xff
            "0f b6 fc",             # movzx edi,ah -> 0xff
            "c3"))
        self.assertEqual(r, 0xFF)

    def test_pushf_popf(self):
        # stc; pushf; clc; popf -> CF restored to 1 -> jb taken
        r, _ = run(asm(
            "f9",                   # stc
            "9c",                   # pushf
            "f8",                   # clc
            "9d",                   # popf
            "72 06",                # jb +6
            "bf 02 00 00 00",
            "c3",
            "bf 01 00 00 00",
            "c3"))
        self.assertEqual(r, 1)

    def test_clc_clears_only_carry(self):
        # stc; clc -> CF=0 -> jb not taken -> edi=2
        r, _ = run(asm(
            "f9",                   # stc
            "f8",                   # clc
            "72 06",                # jb +6
            "bf 02 00 00 00",
            "c3",
            "bf 01 00 00 00",
            "c3"))
        self.assertEqual(r, 2)

    def test_leave_and_frame(self):
        r, _ = run(asm(
            "55",                   # push rbp
            "48 89 e5",             # mov rbp,rsp
            "48 83 ec 10",          # sub rsp,16
            "c7 45 fc 2a 00 00 00", # mov dword [rbp-4],42
            "c9",                   # leave
            "5d",                   # pop rbp
            "bf 2a 00 00 00",       # mov edi,42
            "c3"))
        self.assertEqual(r, 42)

    def test_shift_zero_count_carry_cmp_imm(self):
        # cmp r8d, 2; shl eax, 0; setc dl; movzx edi, dl; ret
        # r8d = 1 < 2 -> borrow (CF=1) -> dl=1
        r1, _ = run(asm("41 b8 01 00 00 00", "41 83 f8 02", "c1 e0 00", "0f 92 c2", "0f b6 fa", "c3"))
        self.assertEqual(r1, 1)

        # r8d = 3 >= 2 -> no borrow (CF=0) -> dl=0
        r0, _ = run(asm("41 b8 03 00 00 00", "41 83 f8 02", "c1 e0 00", "0f 92 c2", "0f b6 fa", "c3"))
        self.assertEqual(r0, 0)

    def test_shift_zero_count_carry_cmp_cl(self):
        # cmp r8d, 2; mov cl, 0; shl eax, cl; setc dl; movzx edi, dl; ret
        # r8d = 1 < 2 -> borrow (CF=1) -> dl=1
        r1, _ = run(asm("41 b8 01 00 00 00", "41 83 f8 02", "b1 00", "d3 e0", "0f 92 c2", "0f b6 fa", "c3"))
        self.assertEqual(r1, 1)

        # r8d = 3 >= 2 -> no borrow (CF=0) -> dl=0
        r0, _ = run(asm("41 b8 03 00 00 00", "41 83 f8 02", "b1 00", "d3 e0", "0f 92 c2", "0f b6 fa", "c3"))
        self.assertEqual(r0, 0)

    def test_shift_nonzero_count_carry_cl(self):
        # mov eax, 0x80000000; mov cl, 1; shl eax, cl; setc dl; movzx edi, dl; ret -> CF=1
        r, _ = run(asm("b8 00 00 00 80", "b1 01", "d3 e0", "0f 92 c2", "0f b6 fa", "c3"))
        self.assertEqual(r, 1)

    def test_shift_zero_count_cl_keeps_carry_from_an_earlier_block(self):
        # cmp r8d, 2; jmp +0 (a block boundary); mov cl, 0; shl eax, cl; setc dl; movzx edi, dl; ret
        # The carry comes from the cmp in the block before: a shift by cl = 0 must leave it alone.
        for r8d, want in ((1, 1), (3, 0)):
            r, _ = run(asm(f"41 b8 {r8d:02x} 00 00 00", "41 83 f8 02", "eb 00", "b1 00", "d3 e0", "0f 92 c2",
                           "0f b6 fa", "c3"))
            self.assertEqual(r, want, f"r8d={r8d}")

    def test_shift_nonzero_count_cl_after_an_earlier_block_takes_its_own_carry(self):
        # mov eax, 0x80000000; cmp r8d, 2 (CF=0); jmp +0; mov cl, 1; shl eax, cl (CF=1); setc dl; movzx edi, dl; ret
        r, _ = run(asm("b8 00 00 00 80", "41 b8 03 00 00 00", "41 83 f8 02", "eb 00", "b1 01", "d3 e0", "0f 92 c2",
                       "0f b6 fa", "c3"))
        self.assertEqual(r, 1)

    def test_shift_zero_count_cl_keeps_the_carry_a_logic_op_cleared(self):
        # stc; test eax, eax (CF=0); mov cl, 0; shl eax, cl; setc dl; movzx edi, dl; ret -> 0
        r, _ = run(asm("f9", "85 c0", "b1 00", "d3 e0", "0f 92 c2", "0f b6 fa", "c3"))
        self.assertEqual(r, 0)


if __name__ == "__main__":
    unittest.main()
