"""tools/boot_matrix.py on a throwaway git repository and a stand-in for gta6rt (no network, no game data).

    python -I tests/test_boot_matrix.py

The merge cases build a base and pull request branches, one of them already merged into the base and one that
conflicts with an earlier PR in the set, and check the statuses, the branch and that the working tree is untouched.
The run cases boot a small Python script that writes synthetic startup and D3D12 traces the way gta6rt does: an exit,
a crash after an access violation, and a run that is still going when the timeout stops it. They also check that a
tree which is not the set's head is refused, and that a report leaves out addresses, paths and call arguments.
"""
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import textwrap
import unittest
import zipfile
from contextlib import redirect_stdout
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import boot_matrix as bm  # noqa: E402

MAKE_GAME = 'TRANSLATION_SOURCES = ("rdna2spirv.py", "shader_ir.py")\n'

FAKE_GTA6RT = textwrap.dedent('''\
    import json, sys, time
    args = sys.argv[1:]
    mode = args[0]
    trace = args[args.index("--trace") + 1]
    events = [{"kind": "trace_open", "pid": 1}, {"kind": "map", "module": "title.exe", "base": "0x140000000"},
              {"kind": "unimplemented_call", "import": "a.dll!F", "caller": "title.exe+0x10"}]
    if mode == "crash":
        events.append({"kind": "exception", "code": "0xc0000005", "at": "title.exe+0x4fb0aec",
                       "access": "read", "address": "0xffffffffffffffff"})
    if mode == "crash-unmapped":   # an instruction pointer outside every known module: a bare address
        events.append({"kind": "exception", "code": "0xc0000005", "at": "0x7ff6a0b0c0d0",
                       "access": "execute", "address": "0x7ff6a0b0c0d0"})
    if mode == "crash-odd":   # fields a runtime of a PR could fill with anything
        events.append({"kind": "exception", "code": "C:\\\\Users\\\\someone\\\\code", "at": "title.exe+0x10",
                       "access": "someone", "address": "0x10"})
    with open(trace, "w", encoding="utf-8") as f:
        for e in events:
            f.write(json.dumps(e) + "\\n")
        f.flush()
        calls = ["CreateCommandQueue", "CreateFence", "ExecuteCommandLists count=1 queue=0x7ff6a0b0"]
        for seq, call in enumerate(calls, 1):
            sys.stderr.write(f"[d3d12-trace] #{seq} {call} ok\\n")
        if mode == "crash-odd":
            sys.stderr.write("[d3d12-trace] #4 LoadCache C:\\\\Users\\\\someone\\\\cache.bin\\n")
        sys.stderr.write("[rt] d3d12: a diagnostic line\\n")
        sys.stderr.flush()
        if mode == "exit":
            f.write(json.dumps({"kind": "exit", "code": 0}) + "\\n")
        elif mode == "hang":
            f.flush()
            time.sleep(60)
    sys.exit(3 if mode.startswith("crash") else 0)
    ''')


def clean_git_env():
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.update({"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"})
    return env


class RepoCase(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory(prefix="leonida boot_matrix ")
        self.addCleanup(td.cleanup)
        self.tmp = td.name
        self.repo = os.path.join(self.tmp, "repo")
        os.makedirs(self.repo)
        patcher = mock.patch.dict(os.environ, clean_git_env(), clear=True)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.git("init", "-q", "-b", "main")
        self.write("make_game.py", MAKE_GAME)
        self.write("tools/rdna2spirv.py", "# translator v1\n")
        self.write("tools/shader_ir.py", "# ir\n")
        self.write("runtime/slots.txt", "slot 10: list proxy\nslot 35: queue\nslot 108: allocation info\n")
        self.commit("start")

    def git(self, *args):
        r = subprocess.run(["git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                            "-c", "commit.gpgsign=false", "-C", self.repo, *args],
                           capture_output=True, encoding="utf-8")
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.strip()

    def write(self, path, text):
        full = os.path.join(self.repo, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    def commit(self, message):
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)
        return self.git("rev-parse", "HEAD")

    def pr(self, number, path, text, base="main"):
        """A PR branch from base that writes path; its head is refs/boot-matrix/pr/N, as --fetch leaves it."""
        self.git("checkout", "-q", "-b", f"pr{number}", base)
        self.write(path, text)
        sha = self.commit(f"PR {number}")
        self.git("checkout", "-q", "main")
        self.git("update-ref", bm.PR_REF.format(number), sha)
        return sha

    def merge(self, *sets, expect=0):
        out = os.path.join(self.tmp, "matrix")
        args = ["merge", "--repo", self.repo, "--base", "main", "--out", out]
        for s in sets:
            args += ["--set", s]
        buf = io.StringIO()
        with redirect_stdout(buf):
            self.assertEqual(bm.main(args), expect)
        with open(os.path.join(out, bm.MATRIX), encoding="utf-8") as f:
            return out, json.load(f)


class ParseTest(unittest.TestCase):
    def test_set_spec(self):
        name, prs = bm.parse_set("rt5=7286,#8517:refs/heads/x")
        self.assertEqual(name, "rt5")
        self.assertEqual(prs, [{"pr": 7286, "ref": "refs/boot-matrix/pr/7286"}, {"pr": 8517, "ref": "refs/heads/x"}])

    def test_base_alone(self):
        self.assertEqual(bm.parse_set("main="), ("main", []))

    def test_bad_specs(self):
        for text in ("rt5", "=1", "a b=1", "rt5=x", "rt5=1,1", "rt5=1,", "a..b=1", "x.lock=1", "trail.=1",
                     "con=1", "NUL.x=1", "com3=1"):
            with self.subTest(text=text), self.assertRaises(Exception):
                bm.parse_set(text)

    def test_translation_sources_read_without_running(self):
        text = 'import os\nTRANSLATION_SOURCES = ("a.py",\n    "b.json")\nraise SystemExit("must not run")\n'
        self.assertEqual(bm.translation_sources(text), ("a.py", "b.json"))
        with self.assertRaises(bm.MatrixError):
            bm.translation_sources("X = 1\n")


class DigestTest(unittest.TestCase):
    def test_matches_make_game(self):
        """The digest over this checkout equals the one make_game.py writes beside the shader library."""
        sys.path.insert(0, ROOT)
        import make_game  # noqa: E402
        self.assertEqual(bm.digest_of_tree(ROOT), make_game.translation_sources_digest())


class MergeTest(RepoCase):
    def test_statuses_branch_and_untouched_worktree(self):
        base = self.git("rev-parse", "main")
        a = self.pr(1, "runtime/slot10.txt", "unwrap list proxies\n")
        self.pr(2, "runtime/slots.txt", "slot 10: bypass\nslot 35: queue\nslot 108: allocation info\n")
        self.pr(3, "runtime/slots.txt", "slot 10: unwrap\nslot 35: queue\nslot 108: allocation info\n")
        fixed = self.pr(4, "runtime/slot108.txt", "return buffer first\n")
        self.git("merge", "-q", "--no-ff", "-m", "Merge pull request #4 from someone/slot108", fixed)
        base = self.git("rev-parse", "main")
        status_before = self.git("status", "--porcelain")

        out, matrix = self.merge("stack=1,2,3,4", "small=1", expect=1)

        self.assertEqual(matrix["base"]["sha"], base)
        stack = matrix["sets"][0]
        self.assertEqual([(p["pr"], p["status"]) for p in stack["prs"]],
                         [(1, "merged"), (2, "merged"), (3, "conflict"), (4, "already-merged")])
        self.assertEqual(stack["prs"][2]["conflict_files"], ["runtime/slots.txt"])
        self.assertEqual(self.git("rev-parse", "refs/heads/boot-matrix/stack"), stack["head"])
        # Two merge commits on the base: first parents walk back to it, second parents are the PRs.
        parents = self.git("log", "--first-parent", "--format=%P", f"{base}..{stack['head']}").splitlines()
        self.assertEqual(len(parents), 2)
        self.assertEqual(parents[-1].split(), [base, a])
        self.assertEqual(self.git("show", f"{stack['head']}:runtime/slots.txt").splitlines()[0], "slot 10: bypass")
        self.assertEqual(self.git("rev-parse", "HEAD"), base)
        self.assertEqual(self.git("status", "--porcelain"), status_before)
        self.assertEqual(matrix["sets"][1]["prs"][0]["status"], "merged")
        # #3 merges cleanly onto the base alone; it is skipped because of #2 before it.
        self.assertEqual(bm.skip_reason(stack["prs"][2]), "conflicts with an earlier PR")
        self.assertEqual(bm.skip_reason(stack["prs"][3]), "already-merged")

    def test_conflict_with_base_and_base_alone(self):
        self.pr(1, "runtime/slots.txt", "slot 10: bypass\n")
        self.write("runtime/slots.txt", "slot 10: moved on main\n")
        base = self.commit("main moves on")
        _, matrix = self.merge("control=", "s=1", expect=1)
        self.assertEqual((matrix["sets"][0]["head"], matrix["sets"][0]["prs"]), (base, []))
        self.assertEqual(self.git("rev-parse", "refs/heads/boot-matrix/control"), base)
        self.assertEqual(bm.skip_reason(matrix["sets"][1]["prs"][0]), "conflicts with base")

    def test_same_inputs_same_commit(self):
        self.pr(1, "runtime/slot10.txt", "unwrap\n")
        _, first = self.merge("s=1")
        _, second = self.merge("s=1")
        self.assertEqual(first["sets"][0]["head"], second["sets"][0]["head"])
        author = self.git("show", "-s", "--format=%an <%ae>", first["sets"][0]["head"])
        self.assertEqual(author, "boot_matrix <boot-matrix@invalid>")

    def test_translator_change_changes_digest(self):
        self.pr(1, "tools/rdna2spirv.py", "# translator v2\n")
        self.pr(2, "runtime/slot10.txt", "unwrap\n")
        _, matrix = self.merge("translator=1", "runtime=2")
        base_digest = bm.digest_of_commit(self.repo, matrix["base"]["sha"])
        self.assertNotEqual(matrix["sets"][0]["translation_digest"], base_digest)
        self.assertEqual(matrix["sets"][1]["translation_digest"], base_digest)
        self.assertEqual(bm.digest_of_tree(self.repo), base_digest)

    def test_one_ref_per_pr_and_names_without_case(self):
        self.pr(1, "runtime/a.txt", "a\n")
        self.git("update-ref", "refs/heads/other", "main")
        for sets in (["a=1", "b=1:refs/heads/other"], ["rt5=1", "RT5=1"]):
            with self.subTest(sets=sets), mock.patch("sys.stderr", io.StringIO()):
                args = ["merge", "--repo", self.repo, "--base", "main", "--out", os.path.join(self.tmp, "m")]
                for s in sets:
                    args += ["--set", s]
                self.assertEqual(bm.main(args), 2)

    def test_a_checked_out_set_branch_stops_before_anything_is_written(self):
        self.pr(1, "runtime/a.txt", "a\n")
        self.merge("s=1")
        self.git("checkout", "-q", "boot-matrix/s")
        before = self.git("rev-parse", "boot-matrix/s")
        self.pr(2, "runtime/b.txt", "b\n", base="boot-matrix/s")
        self.git("checkout", "-q", "boot-matrix/s")
        err = io.StringIO()
        with mock.patch("sys.stderr", err):
            code = bm.main(["merge", "--repo", self.repo, "--base", "main", "--out",
                            os.path.join(self.tmp, "m2"), "--set", "t=1", "--set", "s=1,2"])
        self.assertEqual(code, 2)
        self.assertIn("checked out", err.getvalue())
        self.assertEqual(self.git("rev-parse", "boot-matrix/s"), before)
        other = subprocess.run(["git", "-C", self.repo, "rev-parse", "-q", "--verify", "refs/heads/boot-matrix/t"],
                               capture_output=True)
        self.assertNotEqual(other.returncode, 0, "the set before it must not have been written either")
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "m2")))

    def test_missing_ref_is_a_usage_error(self):
        err = io.StringIO()
        with mock.patch("sys.stderr", err):
            code = bm.main(["merge", "--repo", self.repo, "--base", "main", "--out",
                            os.path.join(self.tmp, "m"), "--set", "s=99"])
        self.assertEqual(code, 2)
        self.assertIn("--fetch", err.getvalue())


class RunTest(RepoCase):
    def setUp(self):
        super().setUp()
        self.pr(1, "runtime/slot10.txt", "unwrap\n")
        self.out, self.matrix = self.merge("s=1")
        self.fake = os.path.join(self.tmp, "fake_gta6rt.py")
        with open(self.fake, "w", encoding="utf-8", newline="\n") as f:
            f.write(FAKE_GTA6RT)
        self.shaders = os.path.join(self.tmp, "spirv")
        os.makedirs(self.shaders)
        self.git("checkout", "-q", "boot-matrix/s")   # the tree a run builds holds the set's head

    def record_shaders(self, digest):
        with open(os.path.join(self.shaders, "translation_sources.sha256"), "w", encoding="utf-8") as f:
            f.write(digest + "\n")

    def run_set(self, mode, timeout=30, expect=0, matrix=None, tree=None):
        real_boot = bm.boot
        seen = {}

        def fake_boot(command, run_dir, env, timeout_s):
            seen["env"] = env
            seen["command"] = command
            return real_boot([sys.executable, "-I", self.fake, mode], run_dir, env, timeout_s)

        gta6rt = os.path.join(self.tmp, "gta6rt.exe")
        open(gta6rt, "wb").close()
        args = ["run", matrix or self.out, "--set", "s", "--tree", tree or self.repo, "--exe", "title.exe",
                "--game-dir", "pkg", "--shaders", self.shaders, "--gta6rt", gta6rt, "--timeout", str(timeout),
                "--env", "LEONIDA_SKIP_INT3=1"]
        err = io.StringIO()
        with mock.patch.object(bm, "boot", fake_boot), redirect_stdout(io.StringIO()), mock.patch("sys.stderr", err):
            self.assertEqual(bm.main(args), expect, err.getvalue())
        seen["stderr"] = err.getvalue()
        return bm.latest_result(self.out, "s", self.matrix["sets"][0]["head"]), seen

    def export(self):
        """The set's head as `git archive` writes it, the way a tree for another user or machine is made."""
        tree = os.path.join(self.tmp, "export")
        os.makedirs(tree)
        archive = os.path.join(self.tmp, "head.zip")
        self.git("archive", "--format=zip", "-o", archive, self.matrix["sets"][0]["head"])
        with zipfile.ZipFile(archive) as z:
            z.extractall(tree)
        return tree

    def assert_refused(self, tree, *names):
        _, seen = self.run_set("exit", expect=2, tree=tree)
        for name in names:
            self.assertIn(name, seen["stderr"])
        self.assertFalse(os.path.exists(os.path.join(self.out, "s", "run1")), "a refused tree must not start a run")

    def test_checkout_at_another_commit_is_refused(self):
        self.git("checkout", "-q", "main")
        self.assert_refused(self.repo, "runtime/slot10.txt (missing)", "boot-matrix/s")

    def test_changed_file_is_refused(self):
        self.write("runtime/slot10.txt", "unwrap, edited locally\n")
        self.assert_refused(self.repo, "runtime/slot10.txt (differs)")

    def test_export_with_crlf_and_a_build_folder_runs(self):
        tree = self.export()
        path = os.path.join(tree, "runtime", "slots.txt")
        with open(path, "rb") as f:
            data = f.read()
        with open(path, "wb") as f:
            f.write(data.replace(b"\n", b"\r\n"))
        os.makedirs(os.path.join(tree, "runtime", "build"))
        open(os.path.join(tree, "runtime", "build", "gta6rt.exe"), "wb").close()
        result, _ = self.run_set("exit", tree=tree)
        self.assertEqual(result["stop"]["kind"], "exit")

    def test_export_missing_a_file_is_refused(self):
        tree = self.export()
        os.remove(os.path.join(tree, "make_game.py"))
        self.assert_refused(tree, "make_game.py (missing)")

    def test_matrix_without_file_list_asks_for_a_new_merge(self):
        os.remove(os.path.join(self.out, "s", bm.FILES))
        self.assert_refused(self.repo, "merge again")

    def test_exit(self):
        self.record_shaders(bm.digest_of_tree(self.repo))
        result, seen = self.run_set("exit")
        self.assertEqual(result["stop"]["kind"], "exit")
        self.assertEqual(result["stop"]["code"], 0)
        self.assertEqual(result["d3d12_calls"], 3)
        self.assertEqual(result["last_d3d12_call"], "#3 ExecuteCommandLists count=1 queue=0x7ff6a0b0 ok")
        self.assertEqual(result["last_d3d12_method"], "#3 ExecuteCommandLists ok")
        self.assertEqual(result["build"], bm.PREBUILT)
        self.assertEqual(result["shaders"], "current")
        self.assertEqual(seen["env"]["LEONIDA_D3D12"], "vulkan")
        self.assertEqual(seen["env"]["LEONIDA_SKIP_INT3"], "1")
        run_dir = os.path.join(self.out, "s", "run1")
        self.assertEqual(seen["command"][1:], ["--run", os.path.abspath("title.exe"), "--game-dir",
                                               os.path.abspath("pkg"), "--overlay", os.path.join(run_dir, "overlay")])
        self.assertEqual(seen["env"]["LEONIDA_SAVE_DIR"], os.path.join(run_dir, "save"))
        self.assertTrue(os.path.isdir(os.path.join(run_dir, "overlay")))

    def test_crash_names_the_last_exception(self):
        self.record_shaders("0" * 64)
        result, _ = self.run_set("crash")
        stop = result["stop"]
        self.assertEqual((stop["kind"], stop["code"], stop["at"]), ("last exception", "0xc0000005",
                                                                     "title.exe+0x4fb0aec"))
        self.assertEqual(stop["returncode"], 3)
        self.assertEqual(stop["address"], "0xffffffffffffffff")   # kept in result.json, left out of reports
        self.assertEqual(result["shaders"], "stale")
        self.assertEqual(bm.format_stop(stop), "last exception 0xc0000005 at title.exe+0x4fb0aec (read)")

    def test_timeout_stops_the_run(self):
        result, _ = self.run_set("hang", timeout=3)
        self.assertEqual(result["stop"]["kind"], "running at timeout")
        self.assertLess(result["seconds"], 30)
        self.assertEqual(result["shaders"], "unknown")

    def test_runs_are_numbered_and_report_uses_the_newest(self):
        self.run_set("crash")
        self.run_set("exit")
        self.assertTrue(os.path.isdir(os.path.join(self.out, "s", "run2")))
        matrix, rows = bm.report_rows(self.out)
        text = bm.format_markdown(matrix, rows)
        self.assertIn("| s | `" + self.matrix["sets"][0]["head"][:12] + "` | #1 | - | prebuilt (unverified) | "
                      "unknown | 3 | `#3 ExecuteCommandLists ok` | exit 0 |", text)

    def test_report_leaves_out_addresses_and_call_arguments(self):
        self.run_set("crash")
        matrix, rows = bm.report_rows(self.out)
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(bm.main(["report", self.out, "--json"]), 0)
        for text in (bm.format_markdown(matrix, rows), out.getvalue()):
            with self.subTest(text=text[:40]):
                self.assertIn("ExecuteCommandLists", text)
                self.assertIn("title.exe+0x4fb0aec", text)
                self.assertNotIn("0xffffffffffffffff", text)
                self.assertNotIn("queue=0x7ff6a0b0", text)

    def test_report_names_a_location_outside_every_module_unmapped(self):
        result, _ = self.run_set("crash-unmapped")
        self.assertEqual(result["stop"]["at"], "0x7ff6a0b0c0d0")   # kept in result.json
        matrix, rows = bm.report_rows(self.out)
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(bm.main(["report", self.out, "--json"]), 0)
        for text in (bm.format_markdown(matrix, rows), out.getvalue()):
            with self.subTest(text=text[:40]):
                self.assertNotIn("7ff6a0b0c0d0", text)
                self.assertIn("unmapped", text)

    def test_report_checks_the_form_of_codes_accesses_and_outcomes(self):
        result, _ = self.run_set("crash-odd")
        self.assertIn("someone", result["stop"]["code"])   # result.json keeps what the trace said
        matrix, rows = bm.report_rows(self.out)
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(bm.main(["report", self.out, "--json"]), 0)
        for text in (bm.format_markdown(matrix, rows), out.getvalue()):
            with self.subTest(text=text[:40]):
                self.assertNotIn("someone", text)
                self.assertNotIn("cache.bin", text)
        stop = rows[0]["result"]["stop"]
        self.assertEqual((stop["code"], stop["access"]), ("other", "other"))
        self.assertEqual(rows[0]["result"]["last_d3d12_method"], "#4 LoadCache other")

    def test_run_options_are_recorded_and_shared_by_name(self):
        result, _ = self.run_set("exit")
        self.assertEqual(result["options"]["env"], ["LEONIDA_SKIP_INT3"])
        self.assertEqual(result["options"]["overlay"], "fresh")
        _, rows = bm.report_rows(self.out)
        self.assertEqual(rows[0]["result"]["options"], {"env": ["LEONIDA_SKIP_INT3"], "timeout": 30.0,
                                                        "overlay": "fresh"})
        self.assertEqual(bm.shareable({"options": {"env": ["LEONIDA_X", "MY_NAME"], "overlay": "given"}})["options"]
                         ["env"], ["LEONIDA_X", "(1 other)"])

    def test_build_goes_to_a_folder_of_the_head(self):
        seen = {}

        def fake_build(tree, build, log_path, config, cmake_args):
            seen["build"] = build
            return False, None
        args = ["run", self.out, "--set", "s", "--tree", self.repo, "--exe", "title.exe", "--game-dir", "pkg",
                "--shaders", self.shaders, "--build"]
        with mock.patch.object(bm, "build_runtime", fake_build), redirect_stdout(io.StringIO()):
            self.assertEqual(bm.main(args), 1)
        head = self.matrix["sets"][0]["head"]
        self.assertEqual(seen["build"], os.path.join(self.repo, "runtime", f"build-boot-matrix-{head[:12]}"))
        result = bm.read_json(os.path.join(self.out, "s", "run1", bm.RESULT))
        self.assertEqual(result["build"], "failed")

    def test_relative_matrix_folder(self):
        """gta6rt runs in the run folder, so a relative matrix path must not reach it as a relative trace path."""
        cwd = os.getcwd()
        os.chdir(self.tmp)
        self.addCleanup(os.chdir, cwd)
        result, _ = self.run_set("exit", matrix=os.path.relpath(self.out, self.tmp))
        self.assertEqual(result["stop"]["kind"], "exit")

    def test_report_ignores_runs_of_an_older_head(self):
        self.run_set("exit")
        path = os.path.join(self.out, bm.MATRIX)
        self.matrix["sets"][0]["head"] = "f" * 40   # merged again, the set moved on
        bm.write_json(path, self.matrix)
        matrix, rows = bm.report_rows(self.out)
        self.assertIsNone(rows[0]["result"])
        self.assertIn("| not run |", bm.format_markdown(matrix, rows))

    def test_no_trace(self):
        stop = bm.classify(None, False, 1)
        self.assertEqual(bm.format_stop(stop), "no trace (process exit 1)")


class ClassifyTest(unittest.TestCase):
    @staticmethod
    def startup(**fields):
        trace = {"exception_count": 0, "exceptions": [], "unimplemented": [], "exit": None, "error": None}
        trace.update(fields)
        return trace

    def test_loader_error_wins_over_an_earlier_handled_exception(self):
        exc = {"code": "0x406d1388", "at": "title.exe+0x10"}
        stop = bm.classify(self.startup(exception_count=1, exceptions=[exc], error="bad image"), False, 1)
        self.assertEqual(stop["kind"], "error")

    def test_shared_calls_keep_real_labels_whole(self):
        for call, want in (("ID3D12Device::CreateCommittedResource", "ID3D12Device::CreateCommittedResource"),
                           ("XboxIDXGIDevice::GetAdapter", "XboxIDXGIDevice::GetAdapter"),
                           ("Xbox CreatePlacedResourceX (device slot 133)",
                            "Xbox CreatePlacedResourceX (device slot 133)"),
                           ("ExecuteCommandLists count=1 queue=0x7ff6a0b0", "ExecuteCommandLists"),
                           (r"CreateFoo C:\x\y", "CreateFoo")):
            with self.subTest(call=call):
                self.assertEqual(bm.shared_call(call), want)

    def test_last_call_of_a_slot_label(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "d3d12.log")
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write("[d3d12-trace] #1 ID3D12Device::CreateFence ok\n"
                        "[d3d12-trace] #2 Xbox CreatePlacedResourceX (device slot 133) failed\n")
            self.assertEqual(bm.last_d3d12_call(path),
                             ("#2 Xbox CreatePlacedResourceX (device slot 133) failed",
                              "#2 Xbox CreatePlacedResourceX (device slot 133) failed"))

    def test_shared_sites(self):
        for at, want in (("title.exe+0x4fb0aec", "title.exe+0x4fb0aec"),
                         ("host:ntdll.dll+0x9f3a2", "host:ntdll.dll+0x9f3a2"),
                         ("0x7ff6a0b0c0d0", "unmapped"), ("", "unmapped"), (None, "unmapped"),
                         (r"C:\x\title.exe+0x10", "unmapped"), ("dir/title.exe+0x10", "unmapped")):
            with self.subTest(at=at):
                self.assertEqual(bm.shared_site(at), want)

    def test_loader_message_stays_out_of_the_report(self):
        path = "C:\\Users\\someone\\games\\pkg\\title.exe"
        stop = {"kind": "error", "what": f"cannot map | {path}\nsecond line", "returncode": 1}
        result = {"run": "run1", "build": "ok", "shaders": "current", "stop": stop, "last_d3d12_call": None}
        shared = bm.shareable(result)
        self.assertNotIn("what", shared["stop"])
        self.assertNotIn("last_d3d12_call", shared)
        row = {"set": "s", "head": "1" * 40, "prs": [], "result": shared}
        line = bm.format_markdown({"base": {"sha": "0" * 40, "ref": "main"}}, [row]).splitlines()[-1]
        self.assertNotIn("someone", line)
        self.assertIn("error (the loader's message is in result.json)", line)
        self.assertEqual(len(re.findall(r"(?<!\\)\|", line)), 10)   # nine cells, nothing split


class ReportTest(RepoCase):
    def test_refs_and_conflicted_files_stay_out_of_the_report(self):
        self.git("update-ref", "refs/heads/someones-private-branch", "main")
        self.pr(1, "runtime/slots.txt", "a\n")
        sha = self.pr(2, "runtime/slots.txt", "b\n")
        self.git("update-ref", "refs/heads/someones-pr-branch", sha)
        out = os.path.join(self.tmp, "matrix")
        args = ["merge", "--repo", self.repo, "--base", "someones-private-branch", "--out", out,
                "--set", "s=1,2:refs/heads/someones-pr-branch"]
        with redirect_stdout(io.StringIO()):
            self.assertEqual(bm.main(args), 1)
        matrix, rows = bm.report_rows(out)
        js = io.StringIO()
        with redirect_stdout(js):
            self.assertEqual(bm.main(["report", out, "--json"]), 0)
        for text in (bm.format_markdown(matrix, rows), js.getvalue()):
            with self.subTest(text=text[:40]):
                self.assertNotIn("someones", text)
                self.assertNotIn("slots.txt", text)
                self.assertIn("conflicts with an earlier PR", text)
        self.assertEqual(rows[0]["prs"], [{"pr": 1, "status": "merged"},
                                          {"pr": 2, "status": "conflict", "skipped": "conflicts with an earlier PR"}])

    def test_not_run_and_skipped(self):
        self.pr(1, "runtime/slots.txt", "a\n")
        self.pr(2, "runtime/slots.txt", "b\n")
        out, _ = self.merge("s=1,2", expect=1)
        matrix, rows = bm.report_rows(out)
        text = bm.format_markdown(matrix, rows)
        self.assertIn("| #1 | #2 (conflicts with an earlier PR) | not run | - | - | - | - |", text)


if __name__ == "__main__":
    unittest.main()
