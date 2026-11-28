"""Synthetic JUnit comparisons, failures, privacy and large-report checks."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import junit_triage as jt


class JunitTriageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def report(self, name, xml):
        path = self.root / name
        path.write_text(xml, encoding="utf-8", newline="\n")
        return str(path)

    def test_ctest_disabled_status_is_skipped(self):
        for previous in ("failed", "passed"):
            for status in ("disabled", "  DISABLED  "):
                with self.subTest(previous=previous, status=status):
                    body = '<failure/>' if previous == "failed" else ''
                    before = self.report("before.xml", f'<testsuite><testcase name="sample" classname="ctest" time="0">{body}</testcase></testsuite>')
                    after = self.report("after.xml", f'<testsuite><testcase name="sample" classname="ctest" time="0" status="{status}"/></testsuite>')
                    result = jt.compare(jt.read_report(before), jt.read_report(after))
                    self.assertEqual(result["fixed"], 0)
                    self.assertEqual(result["newly_skipped"], 1)
                    self.assertEqual(result["after"]["skipped"], 1)
                    self.assertEqual(result["after"]["passed"], 0)

    def test_disabled_status_takes_precedence_over_children(self):
        report = self.report("disabled.xml", '<testsuite><testcase name="sample" classname="ctest" time="0" status="disabled"><failure/></testcase></testsuite>')
        self.assertEqual(jt.read_report(report)[("ctest", "sample")], "skipped")

    def test_transitions_and_privacy(self):
        before = self.report("before.xml", '<testsuite><testcase classname="private" name="a"/>'
            '<testcase name="b"><failure>secret</failure></testcase><testcase name="c"><skipped/></testcase>'
            '<testcase name="gone"/></testsuite>')
        after = self.report("after.xml", '<testsuites><testsuite><testcase classname="private" name="a"><error/></testcase>'
            '<testcase name="b"/><testcase name="c"/><testcase name="added"><skipped/></testcase></testsuite></testsuites>')
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = jt.main([before, after, "--json"])
        result = json.loads(out.getvalue())
        self.assertEqual(code, 1)
        self.assertEqual((result["matched"], result["regressions"], result["fixed"], result["resumed"],
                          result["missing"], result["added"]), (3, 1, 1, 1, 1, 1))
        for secret in ("private", "secret", before, after):
            self.assertNotIn(secret, out.getvalue() + err.getvalue())

    def test_bad_reports_and_missing_file(self):
        docs = ('<testsuite><testcase name="x"/><testcase name="x"/></testsuite>',
                '<testsuite><testcase/></testsuite>', '<testsuite><testcase name="x" time="NaN"/></testsuite>',
                '<testsuite><testcase name="x" time="-1"/></testsuite>', '<testsuite>', '<other/>',
                '<!DOCTYPE testsuite [<!ENTITY a "x">]><testsuite/>')
        good = self.report("good.xml", '<testsuite/>')
        for xml in docs:
            with self.subTest(xml=xml):
                bad = self.report("bad.xml", xml)
                out, err = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                    self.assertEqual(jt.main([good, bad, "--json"]), 2)
                self.assertEqual(out.getvalue(), "")
                self.assertNotIn(bad, err.getvalue())
        with self.assertRaises(jt.ReportError):
            jt.read_report(str(self.root / "missing"))
        path = self.root / "utf16.xml"
        path.write_bytes('<testsuite/>'.encode('utf-16'))
        with self.assertRaises(jt.ReportError):
            jt.read_report(str(path))

    def test_bounded_input_and_case_count(self):
        path = self.report("big.xml", '<testsuite>' + ''.join(f'<testcase name="t{i}"/>' for i in range(10000)) + '</testsuite>')
        self.assertEqual(len(jt.read_report(path)), 10000)
        with mock.patch.object(jt, "MAX_CASES", 9999), self.assertRaises(jt.ReportError):
            jt.read_report(path)
        with mock.patch.object(jt, "MAX_REPORT_BYTES", 100), self.assertRaises(jt.ReportError):
            jt.read_report(path)

    def test_clean_run_and_duplicate_names_in_different_classes(self):
        path = self.report("good.xml", '<testsuite><testcase classname="a" name="same"/>'
                           '<testcase classname="b" name="same"><skipped/></testcase></testsuite>')
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(jt.main([path, path]), 0)
        self.assertIn("Matched: 2", out.getvalue())
        self.assertIn("regressions: 0", out.getvalue())


if __name__ == "__main__":
    unittest.main()
