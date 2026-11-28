# Comparing test reports

`python -I tools/junit_triage.py before.xml after.xml` compares two UTF-8 JUnit XML reports, such as files generated
by `ctest --output-junit`. Add `--json` for machine-readable counts. The tool reads local reports and prints only
aggregate counts, never test names, paths, failure messages or captured output. Keep source reports private when
they may contain sensitive data.

Tests are matched by `(classname, name)`. Duplicate identities are rejected because there is no reliable way to
match them across runs. A regression means a matched test passed before and failed or errored afterwards. Added
failures, skipped-to-failed transitions, missing tests and newly skipped tests are separate counts; inspect those
when assessing coverage. Exit codes: 0 if there are no matched regressions, 1 if there are, 2 for an unreadable,
invalid, oversized or ambiguous report. Exit 0 alone does not mean that every test passed.

The parser accepts `testsuite` or `testsuites` roots, rejects DTDs and non-UTF-8 inputs, limits reports to 32 MiB and
200,000 cases, and validates finite, nonnegative testcase durations when supplied. The standard-library test
`python -I tests/test_junit_triage.py` builds synthetic reports including 10,000 cases; no external data or GPU is
needed.
