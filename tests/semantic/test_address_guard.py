"""rdna2spirv.py --address-guard: every heap_base + VA access is checked against the runtime's page table.

Standard library only; no GPU, no LLVM, no game files.

docs/d3d12_vulkan.md, "Address guard": an access whose first and last byte are below the header's limit (the arena)
passes on a compare; any other passes only when the page table (tools/address_guard.py, runtime/src/address_guard.h)
backs the 4 KiB pages of both. A refused load reads the zero area, a refused store or atomic writes the dump area
and an atomic returns 0; each refusal adds 1 to the header's count and writes its offset. Without the header's magic
(the runtime's guard off) every access passes. Without --address-guard the module is what it was.

cs_address_guard (runtime/test/shaders/cs_address_guard.asm, header: the expected words) runs here on the heap
scripts/run_address_guard_gpu_test.py gives it on the GPU, through spirv_mini_interp, with the guard on and off.
"""
import os
import re
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)  # python -I leaves the script's folder off sys.path

import support  # noqa: E402  (support puts tools/ on sys.path, honouring LEONIDA_TOOLS_DIR)
import spirv_mini_interp as si  # noqa: E402
import rdna2spirv  # noqa: E402
import address_guard as ag  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
SCRIPTS = os.path.join(ROOT, "scripts")
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)
import run_address_guard_gpu_test as gpu  # noqa: E402  (the heap and the expected words, shared with the GPU run)

# build.py's compute header for "; group: 1,1,1" and "; rsrc2: 0x8090": 8 user SGPRs, one thread.
FSSM = bytearray(0x148)
struct.pack_into("<4I", FSSM, 0x138, 0x8090, 1, 1, 1)
FSSM = bytes(FSSM)
VSHARP = [0, 0, 0x100, 0x31016FAC]          # out[]: base 0, 0x100 bytes, raw bounds


def translate(name="cs_address_guard", guard=True, lines=None):
    saved = rdna2spirv.ADDRESS_GUARD
    rdna2spirv.ADDRESS_GUARD = guard
    try:
        if lines is not None:
            return support.translate_lines(name, lines, fssm=FSSM)
        return support.translate_fixture(name, fssm=FSSM)
    finally:
        rdna2spirv.ADDRESS_GUARD = saved


def run(spv, heap):
    """Run one invocation on a copy of `heap` (heap_base 0, so a host address is the heap offset)."""
    mem = {i * 4: w for i, w in enumerate(struct.unpack(f"<{len(heap) // 4}I", heap)) if w}
    si.run_workgroup(spv, 1, 32, inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0), "%sg_lane": i},
                     push_constants=VSHARP + [0] * 32, memory=mem)
    return mem


class GuardedFixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.spv = translate()

    def test_guard_on(self):
        mem = run(self.spv, gpu.heap(True))
        self.assertEqual([mem.get(4 * k, 0) for k in range(8)], gpu.EXPECTED_ON)
        for at, want in gpu.MEMORY_ON.items():
            with self.subTest(at=hex(at)):
                self.assertEqual(mem.get(at, 0), want)

    def test_guard_off_at_run_time(self):
        """The same module on a heap without the magic: every access goes where it points."""
        mem = run(self.spv, gpu.heap(False))
        self.assertEqual([mem.get(4 * k, 0) for k in range(8)], gpu.EXPECTED_OFF)
        for at, want in gpu.MEMORY_OFF.items():
            with self.subTest(at=hex(at)):
                self.assertEqual(mem.get(at, 0), want)

    def test_unguarded_module(self):
        """Without --address-guard nothing reads the header, and the program behaves as with the guard off."""
        spv = translate(guard=False)
        self.assertNotIn("guard", spv)
        mem = run(spv, gpu.heap(True))
        self.assertEqual([mem.get(4 * k, 0) for k in range(8)], gpu.EXPECTED_OFF)

    def test_every_access_is_guarded(self):
        """Each PhysicalStorageBuffer pointer of %main comes from a %guard_fn call, except the header read itself."""
        main = re.search(r"%main = OpFunction .*?OpFunctionEnd", self.spv, re.S).group(0)
        calls = set(re.findall(r"(%\S+) = OpFunctionCall %ulong %guard_fn", main))
        sources = re.findall(r"= OpConvertUToPtr %_ptr_PSB_\w+ (%\S+)", main)
        self.assertEqual(sources.count("%gd_hp"), 1)
        others = [s for s in sources if s != "%gd_hp"]
        # 2 global loads, the store, the atomic, the x2 load, the buffer load, 2 s_loads and the 8 stores to out[]
        self.assertEqual(len(others), 16)
        self.assertTrue(set(others) <= calls, set(others) - calls)


class GuardForms(unittest.TestCase):
    """The kind and span each instruction family passes, read off the %guard_fn calls (sink, last-byte delta)."""

    def calls(self, lines):
        spv = translate("cs_guard_forms", lines=lines + ["s_endpgm"])
        consts = {m.group(1): int(m.group(2)) for m in re.finditer(r"(%\S+) = OpConstant %ulong (\d+)", spv)}
        out = []
        for m in re.finditer(r"OpFunctionCall %ulong %guard_fn %heap_base %gd_on %gd_limit %gd_table \S+ (\S+) (\S+)",
                             spv):
            out.append((consts.get(m.group(1), m.group(1)), consts[m.group(2)]))
        return out

    def test_loads_read_the_zero_area(self):
        self.assertEqual(self.calls(["global_load_dwordx4 v[2:5], v[0:1], off"]), [(15, ag.ZERO)])
        self.assertEqual(self.calls(["s_load_dwordx8 s[8:15], s[0:1], 0x0"]), [(31, ag.ZERO)])
        self.assertEqual(self.calls(["global_load_ubyte v2, v[0:1], off"]), [(3, ag.ZERO)])

    def test_stores_and_atomics_write_the_dump_area(self):
        self.assertEqual(self.calls(["global_store_dwordx3 v[0:1], v[2:4], off"]), [(11, ag.DUMP)])
        self.assertEqual(self.calls(["global_atomic_or_x2 v[0:1], v[2:3], off"]), [(7, ag.DUMP)])
        self.assertEqual(self.calls(["buffer_atomic_add v2, v1, s[0:3], 0 offen glc"]), [(3, ag.DUMP)])
        # a byte store merges into its dword with atomic and/or: one guarded dword
        self.assertEqual(self.calls(["global_store_byte v[0:1], v2, off"]), [(3, ag.DUMP)])

    def test_atomic_result_is_zero_when_refused(self):
        spv = translate("cs_guard_atomic", lines=["global_atomic_add v5, v[0:1], v5, off glc", "s_endpgm"])
        call = re.search(r"(%\S+) = OpFunctionCall %ulong %guard_fn", spv).group(1)
        ok = re.search(rf"(%\S+) = OpIEqual %bool {re.escape(call)} ", spv)
        self.assertIsNotNone(ok)
        self.assertRegex(spv, rf"= OpSelect %uint {re.escape(ok.group(1))} %\S+ %c0\b")


class TableModel(unittest.TestCase):
    """tools/address_guard.py, the Python reference of the table the runtime writes."""

    def test_pages_and_chunks(self):
        words = ag.table_words([(0x90000, 0x90001), (0x5_0000_1000, 0x5_0000_3000), (-0x2000 & ag.MASK48, 0)])
        self.assertTrue(ag.passes(words, 0x80000, 0x90000, 4))
        self.assertTrue(ag.passes(words, 0x80000, 0x90ffc, 4))
        self.assertFalse(ag.passes(words, 0x80000, 0x90ffc, 8))        # the last byte is on the next page
        self.assertTrue(ag.passes(words, 0x80000, 0x5_0000_2ff0, 16))
        self.assertFalse(ag.passes(words, 0x80000, 0x5_0000_3000, 4))
        self.assertFalse(ag.passes(words, 0x80000, 0x6_0000_0000, 4))  # another chunk, nothing backed
        self.assertTrue(ag.passes(words, 0x80000, 0x7fffc, 4))         # the arena, no lookup
        self.assertFalse(ag.passes(words, 0x80000, 0x7fffc, 8))        # past the arena, page 0x80 unbacked
        self.assertTrue(ag.passes(words, 0x80000, -0x2000, 4))         # third span: ends at 0 (wraps 2^48)
        self.assertTrue(ag.passes(words, 0x80000, -0x1000, 4))
        self.assertFalse(ag.passes(words, 0x80000, -0x3000, 4))

    def test_negative_offsets(self):
        """A buffer below the arena (LEONIDA_IMPORT_BELOW_ARENA=1): its 48-bit offset, sign-extended."""
        words = ag.table_words([((-0x2000) & ag.MASK48, (-0x1000) & ag.MASK48)])
        self.assertTrue(ag.passes(words, 0x80000, (-0x2000) & ag.MASK48, 4))
        self.assertFalse(ag.passes(words, 0x80000, (-0x1000) & ag.MASK48, 4))
        self.assertFalse(ag.passes(words, 0x80000, (-4) & ag.MASK48, 8))  # wraps through 0: not the arena

    def test_wrapping_spans_match_runtime(self):
        """Spans crossing 0 (negative signed or 48-bit unsigned) back both sides, matching AddressGuardTable::assign."""
        for span in (((-0x1000) & ag.MASK48, 0x1000), (-0x1000, 0x1000)):
            with self.subTest(span=span):
                words = ag.table_words([span])
                self.assertTrue(ag.page_backed(words, -0x1000))
                self.assertTrue(ag.page_backed(words, 0))
                self.assertFalse(ag.page_backed(words, -0x2000))
                self.assertFalse(ag.page_backed(words, 0x1000))
                self.assertTrue(ag.passes(words, 0x80000, -4, 8))
                self.assertFalse(ag.passes(words, 0x80000, -0x1004, 8))

    def test_full_block_when_out_of_room(self):
        words = ag.table_words([(1 << 32, (1 << 32) + 1), (2 << 32, (2 << 32) + 1)], max_blocks=1)
        self.assertTrue(ag.passes(words, 0x80000, 1 << 32, 4))
        self.assertFalse(ag.passes(words, 0x80000, (1 << 32) + 0x1000, 4))
        self.assertTrue(ag.passes(words, 0x80000, (2 << 32) + 0x5000, 4))   # the full block: all of chunk 2

    def test_constants_match_the_translator_and_the_runtime(self):
        self.assertEqual((rdna2spirv.GUARD_HEADER, rdna2spirv.GUARD_ZERO, rdna2spirv.GUARD_DUMP, rdna2spirv.GUARD_MAGIC,
                          rdna2spirv.GUARD_L1, rdna2spirv.GUARD_BLOCK_WORDS),
                         (ag.HEADER, ag.ZERO, ag.DUMP, ag.MAGIC, ag.L1, ag.BLOCK_WORDS))
        with open(os.path.join(ROOT, "runtime", "src", "address_guard.h"), encoding="utf-8") as f:
            text = f.read()
        for name, value in (("kGuardHeader", ag.HEADER), ("kGuardZero", ag.ZERO), ("kGuardDump", ag.DUMP),
                            ("kGuardEnd", ag.RESERVED_END), ("kGuardLevel1", ag.L1),
                            ("kGuardBlockWords", ag.BLOCK_WORDS)):
            with self.subTest(name=name):
                m = re.search(rf"{name} = (1u << \d+|0x[0-9a-fA-F]+|\d+)", text)
                self.assertIsNotNone(m)
                v = m.group(1)
                self.assertEqual(1 << int(v.split("<<")[1]) if "<<" in v else int(v, 0), value)
        self.assertIn('"LGUARD01"', text)


if __name__ == "__main__":
    unittest.main()
