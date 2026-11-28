"""tools/image_bvh_census.py on hand-written listings: forms, descriptor fields, word-read categories, lane rule.

    python -I tests/test_image_bvh_census.py -v

Every listing here is invented (registers and values included); no game data, no GPU.
"""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import image_bvh_census as census  # noqa: E402

ENC = " ; encoding: [0x00,0x00,0x00,0x00]"      # only the length matters: every instruction is 4 bytes here


def listing(*lines):
    """A listing in shader_disasm's layout. `name:` lines are labels; `@name` in a branch is its target."""
    body, labels, pc = [], {}, 0
    for ln in lines:
        if ln.endswith(":"):
            labels[ln[:-1]] = pc
        else:
            body.append((pc, ln))
            pc += 4
    out = []
    for at, ln in body:
        if "@" in ln:
            op, target = ln.split("@")
            ln = f"{op}{(labels[target] - at - 4) // 4}"
        out.append("\t" + ln + ENC)
    return "; cs_probe  code 0x0..0x%x (header), gfx1013, 0 invalid words\n" % pc + "\n".join(out) + "\n"


def analyse(*lines):
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "cs_probe.s")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(listing(*lines))
        _, insns = census.load_program(path)
    return census.analyse(insns)


def reads(site, reach=None):
    """{(word, category)} of a site's reads (optionally only those with the given reach)."""
    return {(e[1], e[2]) for e in site["reads"] if reach is None or e[3] == reach}


DESC_LITERALS = [                                 # an invented descriptor; s8 is copied from the user SGPR s2
    "s_mov_b32 s8, s2",
    "s_brev_b32 s9, 0x1fe",                       # 0x7f800000: box_grow 0xff, box_sort_en 0
    "s_movk_i32 s10, -2",                         # 0xfffffffe (sign-extended)
    "s_mov_b32 s11, 0x52000123",                  # size bits 0x123, triangle_return_mode 0, llc_noalloc 1, type 5
]
CALL32 = "image_bvh_intersect_ray v[4:7], v[0:10], s[8:11]"
TRIANGLE = [                                      # the words divided by word 1, and word 1's high half
    "v_rcp_f32_e32 v20, v5",
    "v_mul_f32_e32 v21, v4, v20",
    "v_mul_legacy_f32_e32 v22, v20, v6",
    "v_mov_b32_e32 v12, v7",                      # a plain copy is followed
    "v_mul_f32_e32 v23, v12, v20",
    "v_mul_i32_i24_sdwa v24, sext(v5), v30 dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:WORD_1 src1_sel:DWORD",
    "v_cmp_gt_i32_e32 vcc_lo, 0, v24",
]


class CategoryTests(unittest.TestCase):
    def test_triangle_words_and_literal_descriptor(self):
        (site,) = analyse(*DESC_LITERALS, CALL32, *TRIANGLE, "s_endpgm")
        self.assertEqual(site["form"], "image_bvh_intersect_ray")
        self.assertEqual(reads(site), {(0, "ratio"), (1, "ratio"), (2, "ratio"), (3, "ratio"), (1, "high_half")})
        self.assertEqual(reads(site, "some"), set())
        self.assertEqual([p[1:] for p in site["product_reads"]], [("v_cmp_gt_i32 with 0", "all")])
        self.assertIsNone(site["desc"][0])
        fields = census.decode_descriptor(site["desc"])
        self.assertEqual(fields, {"box_grow": 0xFF, "box_sort_en": 0, "size": 0x123FFFFFFFE,
                                  "triangle_return_mode": 0, "llc_noalloc": 1, "big_page": 0, "type": 5})

    def test_box_words_64bit_form_and_unknown_descriptor_words(self):
        (site,) = analyse(
            "s_mov_b32 s12, 0",                   # word 0 literal
            "s_load_dwordx2 s[13:14], s[0:1], 0x0",   # words 1 and 2 from memory: not literals
            "s_mov_b32 s15, 0x30000000",
            "image_bvh64_intersect_ray v[4:7], v[0:11], s[12:15]",
            "v_lshlrev_b32_e32 v10, 3, v4",       # integer pack / compare
            "v_and_or_b32 v11, v5, 7, v10",
            "v_cmp_eq_u32_e32 vcc_lo, v31, v6",
            "v_add_f32_e32 v9, v7, v8",           # float, not divided
            "global_store_dword v[30:31], v6, off",   # memory
            "s_endpgm")
        self.assertEqual(site["form"], "image_bvh64_intersect_ray")
        self.assertEqual(reads(site), {(0, "raw_integer"), (1, "raw_integer"), (2, "raw_integer"),
                                       (3, "raw_float"), (2, "raw_other")})
        self.assertEqual(site["desc"][0], 0)
        f = census.decode_descriptor(site["desc"])
        self.assertIsNone(f["box_sort_en"])
        self.assertIsNone(f["size"])
        self.assertEqual((f["type"], f["big_page"]), (3, 0))

    def test_select_modifier_and_next_call_operand(self):
        (first, second) = analyse(
            CALL32,
            "v_cndmask_b32_e32 v9, v4, v5, vcc_lo",   # a select is not followed
            "v_rcp_f32_e32 v20, v5",
            "v_mul_f32_e64 v21, |v6|, v20",       # an abs modifier: not the ratio
            "v_mov_b32_e32 v0, v4",               # word 0 becomes the next call's node id
            "image_bvh_intersect_ray v[4:7], [v0, v12, v13, v14, v15, v16, v17, v18, v19, v24, v25], s[8:11]",
            "s_endpgm")
        self.assertEqual(reads(first), {(0, "select"), (1, "select"), (1, "ratio"), (2, "raw_float"),
                                        (0, "raw_other")})
        self.assertEqual(reads(second), set())

    def test_next_call_and_overwrite_kill_the_words(self):
        (first, second) = analyse(
            CALL32,
            "v_mov_b32_e32 v6, 0",                # word 2 overwritten
            "v_add_f32_e32 v9, v6, v8",
            "image_bvh_intersect_ray v[4:7], v[12:22], s[8:11]",
            "v_add_f32_e32 v9, v4, v8",           # the second call's word 0
            "s_endpgm")
        self.assertEqual(reads(first), set())
        self.assertEqual(reads(second), {(0, "raw_float")})


class PathTests(unittest.TestCase):
    def test_overwrite_on_one_path_is_some_paths(self):
        (site,) = analyse(
            CALL32,
            "s_cbranch_scc1 @join",
            "v_mov_b32_e32 v5, 1.0",
            "join:",
            "v_rcp_f32_e32 v20, v5",
            "s_endpgm")
        self.assertEqual([e[1:4] for e in site["reads"]], [(1, "ratio", "some")])

    def test_else_half_of_a_saved_mask_keeps_the_word(self):
        (site,) = analyse(
            CALL32,
            "v_cmp_ne_u32_e32 vcc_lo, 4, v31",
            "s_and_saveexec_b32 s40, vcc_lo",     # then-half
            "v_min3_u32 v5, v1, v2, v3",          # overwrites word 1 in the then-lanes only
            "s_andn2_b32 exec_lo, s40, exec_lo",  # else-half: saved & ~then
            "v_rcp_f32_e32 v20, v5",              # these lanes never ran the overwrite
            "s_mov_b32 exec_lo, s40",
            "v_add_f32_e32 v21, v5, v8",          # all lanes again: both values
            "s_endpgm")
        self.assertEqual([e[1:4] for e in site["reads"]], [(1, "ratio", "all"), (1, "raw_float", "some")])

    def test_wrexec_else_half_and_restore(self):
        (site,) = analyse(
            CALL32,
            "s_mov_b32 s41, exec_lo",
            "v_cmpx_lt_u32_e32 2, v31",
            "v_mov_b32_e32 v6, 0",
            "s_andn2_wrexec_b32 s42, s41",        # EXEC = s41 & ~EXEC
            "v_cmp_eq_u32_e32 vcc_lo, v32, v6",
            "s_mov_b32 exec_lo, s41",
            "s_endpgm")
        self.assertEqual([e[1:4] for e in site["reads"]], [(2, "raw_integer", "all")])

    def test_unknown_exec_writer_gives_no_proof(self):
        (site,) = analyse(
            "s_load_dword s43, s[0:1], 0x0",
            CALL32,
            "v_cmp_ne_u32_e32 vcc_lo, 4, v31",
            "s_and_saveexec_b32 s40, vcc_lo",
            "v_min3_u32 v5, v1, v2, v3",
            "s_mov_b32 exec_lo, s43",             # a mask the walk cannot place
            "v_rcp_f32_e32 v20, v5",
            "s_endpgm")
        self.assertEqual([e[1:4] for e in site["reads"]], [(1, "ratio", "some")])

    def test_write_after_a_join_covers_reads_under_the_same_exec(self):
        (site,) = analyse(
            CALL32,
            "s_mov_b32 s41, exec_lo",
            "s_cbranch_scc1 @narrow",
            "s_branch @join",
            "narrow:",
            "v_cmpx_lt_u32_e32 2, v31",           # one path narrows, the other does not
            "join:",
            "v_mov_b32_e32 v6, 0",                # every lane the join runs overwrites word 2
            "v_cmp_eq_u32_e32 vcc_lo, v32, v6",   # so this reads no word
            "s_mov_b32 exec_lo, s41",             # the call's lanes again
            "v_cmp_eq_u32_e32 vcc_lo, v33, v6",   # lanes the narrowed path left out still hold word 2
            "s_endpgm")
        self.assertEqual([e[1:4] for e in site["reads"]], [(2, "raw_integer", "some")])

    def test_narrowing_in_a_loop_gives_no_proof(self):
        (site,) = analyse(
            CALL32,
            "s_mov_b32 s50, exec_lo",
            "loop:",
            "v_readfirstlane_b32 s4, v9",
            "v_cmpx_eq_u32_e32 s4, v9",           # a different lane on every pass
            "v_rcp_f32_e32 v20, v5",
            "v_mov_b32_e32 v5, 0",                # word 1 replaced in this pass's lanes
            "s_xor_b32 s50, s50, exec_lo",
            "s_mov_b32 exec_lo, s50",
            "s_cbranch_execnz @loop",
            "s_endpgm")
        self.assertEqual([e[1:4] for e in site["reads"]], [(1, "ratio", "some")])

    def test_inner_back_edge_drops_guards_and_saved_masks(self):
        node = ("n", 8, census.ROOT)
        st = census.SiteState({5: frozenset({(("w", 1), frozenset({("out", node)})),
                                              (census.OTHER, frozenset({("in", node)}))})}, {40: node}, node)
        out = census.strip(st)
        self.assertEqual(out.masks, {})
        self.assertEqual(out.vg[5], frozenset({(("w", 1), frozenset()), (census.OTHER, frozenset())}))
        self.assertEqual(st.masks, {40: node})            # the state on the other edges is untouched

    def test_loop_around_the_call_is_cut_and_counted(self):
        (site,) = analyse(
            "top:",
            CALL32,
            "v_rcp_f32_e32 v20, v5",
            "s_cbranch_scc1 @top",
            "s_endpgm")
        self.assertEqual([e[1:4] for e in site["reads"]], [(1, "ratio", "all")])
        self.assertTrue(site["loop_carried"])


class SummaryTests(unittest.TestCase):
    def directory(self, tmp, listings):
        for name, text in listings.items():
            with open(os.path.join(tmp, name + ".s"), "w", encoding="utf-8", newline="\n") as f:
                f.write(text)

    def test_summary_partition_and_privacy(self):
        listings = {
            "cs_tri": listing(*DESC_LITERALS, CALL32, *TRIANGLE, "s_endpgm"),
            "cs_box": listing("image_bvh64_intersect_ray v[4:7], v[0:11], s[12:15]",
                              "v_lshlrev_b32_e32 v10, 3, v4", "s_cbranch_scc1 @end", "v_mov_b32_e32 v4, 0",
                              "end:", "v_cmp_eq_u32_e32 vcc_lo, v31, v4", "s_endpgm"),
            "cs_none": listing("v_mov_b32_e32 v0, 0", "s_endpgm"),
            "cs_bad": listing(CALL32, "s_branch 0x40", "s_endpgm"),    # a branch out of the code
        }
        with tempfile.TemporaryDirectory() as src, tempfile.TemporaryDirectory() as outdir:
            self.directory(src, listings)
            out = os.path.join(outdir, "census.json")
            with contextlib.redirect_stdout(io.StringIO()) as printed:
                self.assertEqual(census.main([src, "--out", out]), 0)
            with open(out, "rb") as f:
                blob = f.read()
        self.assertNotIn(b"\r\n", blob)
        s = json.loads(blob)
        self.assertEqual(s["sites"], {"total": 2, "image_bvh_intersect_ray": 1, "image_bvh64_intersect_ray": 1})
        self.assertEqual(s["shaders"], {"cs": 2})
        self.assertEqual(s["not_analysed"], {"branch target outside code": 1})
        self.assertEqual(sum(v for v, _ in s["site_partition"].values()), s["sites"]["total"])
        # cs_box reads word 0 as an integer on every path (the shift) and on some paths (the compare): the
        # category counts as read on every path, the compare as one read on some paths only
        self.assertEqual(s["site_partition"], {"ratio+high_half": [1, 1], "raw_integer": [1, 1]})
        self.assertEqual(s["word_reads"]["sites"]["ratio"], [1, 0])
        self.assertEqual(s["word_reads"]["reads_some_paths"]["raw_integer"], 1)
        self.assertEqual(s["some_paths_position"]["raw_integer"], [1, 0])     # before any later call
        self.assertEqual(s["word_reads"]["per_word_sites"]["0"]["raw_integer"], [1, 0])
        self.assertEqual(s["descriptor"]["fields"]["type"], {"5": 1, "not a literal": 1})
        self.assertEqual(s["descriptor"]["word0"], {"not a literal": 2})
        self.assertEqual(s["high_half_product_readers"], {"v_cmp_gt_i32 with 0": [1, 0]})
        self.assertEqual(s["limits"]["sites with a category read on some paths only"], 0)
        for text in (blob.decode("utf-8"), printed.getvalue()):
            for name in listings:
                self.assertNotIn(name, text)

    def test_missing_source_is_an_error(self):
        with self.assertRaises(SystemExit) as cm, contextlib.redirect_stderr(io.StringIO()):
            census.main([os.path.join(tempfile.gettempdir(), "no_such_shaders_dir_xyz")])
        self.assertNotEqual(cm.exception.code, 0)

    def test_output_must_not_overwrite_a_listing(self):
        with tempfile.TemporaryDirectory() as src:
            self.directory(src, {"cs_tri": listing(CALL32, "s_endpgm")})
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                census.main([src, "--out", os.path.join(src, "cs_tri.s")])
            with open(os.path.join(src, "cs_tri.s"), encoding="utf-8") as f:
                self.assertIn("image_bvh", f.read())

    def test_v_cmpx_e32_does_not_clobber_sgpr_source_and_e64_product_reader(self):
        # v_cmpx_*_e32 reads a[0] (an SGPR source), never writes it: descriptor literals and saved EXEC masks survive.
        (site,) = analyse(
            *DESC_LITERALS,
            "v_cmpx_ne_u32_e32 s9, v0",
            CALL32,
            "s_mov_b32 s41, exec_lo",
            "v_cmpx_eq_u32_e32 s41, v31",
            "v_mov_b32_e32 v6, 0",
            "s_andn2_wrexec_b32 s42, s41",
            "v_cmp_eq_u32_e32 vcc_lo, v32, v6",
            "s_mov_b32 exec_lo, s41",
            "v_mul_i32_i24_sdwa v24, sext(v5), v30 dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:WORD_1 src1_sel:DWORD",
            "v_cmpx_gt_i32_e64 exec_lo, 0, v24",
            "s_endpgm",
        )
        self.assertEqual(site["desc"][1], 0x7F800000)
        self.assertIn((2, "raw_integer", "all"), [e[1:4] for e in site["reads"]])
        self.assertEqual([p[1:] for p in site["product_reads"]], [("v_cmpx_gt_i32 with 0", "all")])

    def test_modified_v_rcp_f32_does_not_tag_ratio_reciprocal(self):
        for rcp in (
            "v_rcp_f32_e64 v20, |v5|",
            "v_rcp_f32_e64 v20, -v5",
            "v_rcp_f32_e64 v20, v5 clamp",
        ):
            (site,) = analyse(CALL32, rcp, "v_mul_f32_e32 v21, v4, v20", "s_endpgm")
            self.assertEqual(reads(site), {(1, "raw_float"), (0, "raw_float")}, rcp)

    def test_high_half_operand_requires_vgpr_and_handles_both_operands(self):
        # SGPR or immediate in operand 1 must not read VGPR v5 or crash with KeyError.
        (site,) = analyse(
            CALL32,
            "v_mul_i32_i24_sdwa v24, sext(s5), v30 dst_sel:DWORD src0_sel:WORD_1 src1_sel:DWORD",
            "v_mul_i32_i24_sdwa v25, sext(1), v30 dst_sel:DWORD src0_sel:WORD_1 src1_sel:DWORD",
            "v_cmp_gt_i32_e32 vcc_lo, 0, v24",
            "v_cmp_gt_i32_e32 vcc_lo, 0, v25",
            "s_endpgm",
        )
        self.assertEqual(site["product_reads"], [])

        # Both operands high-half with word 1 in operand 2: product still tracks H cleanly on every path.
        (site2,) = analyse(
            CALL32,
            "v_mul_i32_i24_sdwa v24, sext(v30), sext(v5) dst_sel:DWORD src0_sel:WORD_1 src1_sel:WORD_1",
            "v_cmp_gt_i32_e32 vcc_lo, 0, v24",
            "s_endpgm",
        )
        self.assertEqual(reads(site2), {(1, "high_half")})
        self.assertEqual([p[1:] for p in site2["product_reads"]], [("v_cmp_gt_i32 with 0", "all")])

    def test_is_copy_and_imm32_edge_cases(self):
        self.assertFalse(census.is_copy({
            "op": "v_swap_b32",
            "args": [{"k": "vgpr", "r": 4, "n": 1, "neg": True}, {"k": "vgpr", "r": 5, "n": 1}],
            "mods": [],
        }))
        self.assertIsNone(census.imm32({"k": "fimm", "v": 1e40}))

    def test_blocks_of_non_contiguous_gap_empty_and_load_program_bad_hex(self):
        self.assertEqual(census.blocks_of([]), ([], []))
        self.assertEqual(census.analyse([]), [])
        # A 4-byte gap between 0x104 and 0x200 must start a new block with no fall-through across the gap.
        insns = [
            {"op": "v_mov_b32_e32", "args": [{"k": "vgpr", "r": 0, "n": 1}, {"k": "imm", "v": 0}],
             "mods": [], "enc": "00000000", "pc": 0x100},
            {"op": "s_endpgm", "args": [], "mods": [], "enc": "00000000", "pc": 0x200},
        ]
        blocks, succ = census.blocks_of(insns)
        self.assertEqual(([len(b) for b in blocks], succ), ([1, 1], [[], []]))

        # Malformed hex encoding in a listing with embedded data words must Skip instead of crashing with ValueError.
        with tempfile.TemporaryDirectory() as root:
            disasm = os.path.join(root, "disasm")
            fssm = os.path.join(root, "fssm")
            os.makedirs(disasm)
            os.makedirs(fssm)
            with open(os.path.join(fssm, "cs_badhex.fssm"), "wb") as f:
                f.write(bytes(0x40))
            bad_path = os.path.join(disasm, "cs_badhex.s")
            with open(bad_path, "w", encoding="utf-8", newline="\n") as f:
                f.write("; cs_badhex.fssm code 0x0..0x8\n"
                        "\t" + CALL32 + " ; encoding: [0xzz]\n"
                        "\t; invalid instruction encoding\n")
            with self.assertRaises(census.Skip):
                census.load_program(bad_path)

    def test_cli_overwrite_guards_atomic_write_trailing_lf_and_oserror(self):
        with tempfile.TemporaryDirectory() as root:
            src = os.path.join(root, "disasm")
            fssm = os.path.join(root, "fssm")
            os.makedirs(src)
            os.makedirs(fssm)
            self.directory(src, {"cs_tri": listing(*DESC_LITERALS, CALL32, *TRIANGLE, "s_endpgm")})
            idx_path = os.path.join(root, "index.json")
            with open(idx_path, "w", encoding="utf-8", newline="\n") as f:
                f.write("[]\n")
            fssm_path = os.path.join(fssm, "cs_tri.fssm")
            with open(fssm_path, "wb") as f:
                f.write(b"FSSM")
            hardlink = os.path.join(root, "linked.json")
            os.link(os.path.join(src, "cs_tri.s"), hardlink)

            for bad_out in (idx_path, fssm_path, hardlink):
                with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                    census.main([src, "--out", bad_out])
            with open(idx_path, "r", encoding="utf-8") as f:
                self.assertEqual(f.read(), "[]\n")
            with open(fssm_path, "rb") as f:
                self.assertEqual(f.read(), b"FSSM")

            # Normal write ends with a single LF.
            out_dir = os.path.join(root, "out")
            os.makedirs(out_dir)
            out_path = os.path.join(out_dir, "report.json")
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(census.main([src, "--out", out_path]), 0)
            with open(out_path, "rb") as f:
                data = f.read()
            self.assertTrue(data.endswith(b"\n"))

            # Atomic write failure must keep previous contents and clean up temp file.
            with open(out_path, "w", encoding="utf-8", newline="\n") as f:
                f.write('{"keep": 1}\n')
            orig_replace = census.replace_retry.replace

            def boom(src_p, dst_p, **kw):
                raise OSError("disk full")

            census.replace_retry.replace = boom
            try:
                err = io.StringIO()
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                    rc = census.main([src, "--out", out_path])
                self.assertEqual(rc, 2)
                self.assertIn("ERROR: cannot write", err.getvalue())
            finally:
                census.replace_retry.replace = orig_replace
            with open(out_path, "r", encoding="utf-8") as f:
                self.assertEqual(f.read(), '{"keep": 1}\n')
            self.assertEqual(os.listdir(out_dir), ["report.json"])

            # OSError during scan_directory must report ERROR and exit 2.
            orig_scan = census.scan_directory

            def bad_scan(_):
                raise OSError("unreadable")

            census.scan_directory = bad_scan
            try:
                err = io.StringIO()
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                    rc = census.main([src])
                self.assertEqual(rc, 2)
                self.assertIn("ERROR: cannot read", err.getvalue())
            finally:
                census.scan_directory = orig_scan


if __name__ == "__main__":
    unittest.main()
