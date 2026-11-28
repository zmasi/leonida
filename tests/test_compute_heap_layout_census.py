"""Compute heap layout census on synthetic shader containers, listings and dispatch logs."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_heap_index_census import Work, SFI0_RES, SFI0_SMP
from test_rootsig_census import container, rts0
import compute_heap_layout_census as c


class ComputeHeapLayout(unittest.TestCase):
    def test_compare_programs(self):
        work = Work()
        self.addCleanup(work.close)
        root = Path(work.root)
        both = c.RESOURCE | c.SAMPLER

        def program(name, res, smp, constants=3, flags=both, used=SFI0_RES | SFI0_SMP, embedded=True):
            lines = [(f"s_mov_b32 s20, s{res}", 4), ("s_mov_b32 s21, 0", 4),
                     ("s_load_dwordx8 s[32:39], s[20:21], s4", 8),
                     (f"s_mov_b32 s20, s{smp}", 4), ("s_load_dwordx4 s[40:43], s[20:21], s4", 8),
                     ("image_sample v[0:3], v[4:5], s[32:39], s[40:43] dmask:0xf dim:SQ_RSRC_IMG_2D", 8),
                     ("s_endpgm", 4)]
            work.add(name, "cs", used, lines, rsrc2=12 << 1)
            if embedded:
                payload = rts0(params=[("constants", 0, constants)], flags=flags)
                (root / (name + ".dxbc")).write_bytes(container([(b"SFI0", struct.pack("<Q", used)), (b"RTS0", payload)]))

        program("both", 7, 8)
        program("swapped", 8, 7)
        program("full", 7, 8, constants=28)
        program("sampler_only", 10, 7, flags=c.SAMPLER, used=SFI0_SMP)
        program("absent", 7, 8, embedded=False)
        program("captured", 7, 8, embedded=False)
        program("conflict", 7, 8, embedded=False)
        program("other_process", 7, 8, embedded=False)
        first, second = root / "first.log", root / "second.log"
        layout = "[rt] compute-layout: pso=A flags=c00 bound-heap=0x0 shader-heap=0x0 (s4 type=0 const=3 slots=0)\n"
        dispatch = "[rt] compute-input: execute=1 list=0 cmd=0 name=cs/captured pso=A direct groups=1,1,1\n"
        first.write_text(layout + dispatch + layout.replace("pso=A", "pso=B") +
                         dispatch.replace("pso=A", "pso=B").replace("captured", "conflict") +
                         layout.replace("pso=A", "pso=B").replace("const=3", "const=4"), encoding="utf-8")
        second.write_text(dispatch.replace("captured", "other_process"), encoding="utf-8")
        counts = c.census(Path(work.disasm), c.captured_layouts([first, second], "cs"))
        self.assertEqual((counts["compute"], counts["direct"]), (8, 8))
        self.assertEqual((counts["match"], counts["resource matches"], counts["sampler matches"]), (3, 2, 3))
        self.assertEqual((counts["mismatch"], counts["no room"], counts["missing layout"],
                          counts["ambiguous or invalid layout"]), (1, 1, 2, 1))

    def test_ambiguous_and_explicit_table_reads(self):
        def dynamic(n):
            return {"roles": {"V#"}, "offset": ("dynamic", None), "base": f"user s{n}", "ptr": "ptr32"}

        layout = (c.RESOURCE, 6, (4,))
        self.assertEqual(c.compare(c.RESOURCE, layout, [dynamic(4)]), "inconclusive")
        self.assertEqual(c.compare(c.RESOURCE, layout, [dynamic(4), dynamic(6)]), "match")
        self.assertEqual(c.compare(c.RESOURCE, layout, [dynamic(6), dynamic(8)]), "inconclusive")
        self.assertEqual(c.compare(c.SAMPLER, layout, []), "incompatible flags")


class FspxFallbackBomAndCliTests(unittest.TestCase):
    def _setup_shader(self, name="fspx_prog", with_rts0=True):
        work = Work()
        self.addCleanup(work.close)
        root = Path(work.root)
        lines = [
            ("s_mov_b32 s20, s7", 4), ("s_mov_b32 s21, 0", 4),
            ("s_load_dwordx8 s[32:39], s[20:21], s4", 8),
            ("s_mov_b32 s20, s8", 4), ("s_load_dwordx4 s[40:43], s[20:21], s4", 8),
            ("image_sample v[0:3], v[4:5], s[32:39], s[40:43] dmask:0xf dim:SQ_RSRC_IMG_2D", 8),
            ("s_endpgm", 4),
        ]
        used = SFI0_RES | SFI0_SMP
        work.add(name, "cs", used, lines)
        if with_rts0:
            payload = rts0(params=[("constants", 0, 3)], flags=c.RESOURCE | c.SAMPLER)
            (root / (name + ".dxbc")).write_bytes(
                container([(b"SFI0", struct.pack("<Q", used)), (b"RTS0", payload)])
            )
        return work, root

    def test_fspx_bom_json_and_directory_at_launch_path(self):
        work, root = self._setup_shader("fspx_bom")
        fspx_dir = root / "fspx"
        fspx_dir.mkdir()
        bom_json = b"\xef\xbb\xbf" + b'{"user_sgprs": 12}'
        (fspx_dir / "fspx_bom.json").write_bytes(bom_json)
        counts = c.census(Path(work.disasm), {})
        self.assertEqual(counts["match"], 1)

        # A directory at fspx/<name>.json must be ignored cleanly rather than raising.
        work_dir, root_dir = self._setup_shader("fspx_isdir")
        (root_dir / "fspx" / "fspx_isdir.json").mkdir(parents=True)
        self.assertEqual(c.program_loads(Path(work_dir.disasm), "fspx_isdir"), [])

    def test_invalid_fspx_json_raises_and_main_exits_2(self):
        import contextlib
        import io

        bad_payloads = [
            ("bad_utf8", b"\xff\xfe\xfd"),
            ("non_dict", b"[]"),
            ("bool_val", b'{"user_sgprs": true}'),
            ("str_val", b'{"user_sgprs": "8"}'),
            ("neg_val", b'{"user_sgprs": -1}'),
        ]
        for label, payload in bad_payloads:
            with self.subTest(case=label):
                work, root = self._setup_shader("bad_fspx")
                fspx_dir = root / "fspx"
                fspx_dir.mkdir()
                bad_file = fspx_dir / "bad_fspx.json"
                bad_file.write_bytes(payload)
                with self.assertRaises(ValueError) as ctx:
                    c.program_loads(Path(work.disasm), "bad_fspx")
                self.assertIn(str(bad_file), str(ctx.exception))

                out, err = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                    rc = c.main([str(work.disasm)])
                self.assertEqual(rc, 2)
                self.assertEqual(out.getvalue(), "")
                self.assertIn("ERROR:", err.getvalue())
                self.assertIn(str(bad_file), err.getvalue())

    def test_utf8_bom_dispatch_log_and_main_success(self):
        import contextlib
        import io

        work, root = self._setup_shader("bom_log_prog", with_rts0=False)
        fspx_dir = root / "fspx"
        fspx_dir.mkdir()
        (fspx_dir / "bom_log_prog.json").write_text('{"user_sgprs": 12}', encoding="utf-8", newline="\n")
        log_path = root / "dispatch.log"
        log_text = (
            "[rt] compute-layout: pso=P1 flags=c00 bound-heap=0x0 shader-heap=0x0 (s4 type=0 const=3 slots=0)\n"
            "[rt] compute-input: execute=1 list=0 cmd=0 name=cs/bom_log_prog pso=P1 direct groups=1,1,1\n"
        )
        log_path.write_bytes(b"\xef\xbb\xbf" + log_text.encode("utf-8"))
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = c.main([str(work.disasm), "--dispatch-log", str(log_path), "--group", "cs"])
        self.assertEqual(rc, 0)
        self.assertEqual(err.getvalue(), "")
        self.assertIn("match: 1\n", out.getvalue())


if __name__ == "__main__":
    unittest.main()
