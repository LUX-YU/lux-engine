"""Regression cases for the Render signature gate. Run with the checker's Python dependencies."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


CHECKER = Path(__file__).with_name("CheckSignatureArity.py").resolve()
SOURCE = Path("modules/function/render/test/fixture.hpp")
BAD = "void six(int a, int b, int c, int d, int e, int f);\n"


class SignatureGateTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        self.src = self.repo / SOURCE
        self.src.parent.mkdir(parents=True)
        self.report = self.repo / "report.json"
        self.git("init", "--quiet")
        self.git("config", "user.email", "tests@example.invalid")
        self.git("config", "user.name", "Signature gate test")
        self.src.write_text(BAD, encoding="utf-8")
        self.commit()

    def git(self, *args):
        subprocess.run(["git", *args], cwd=self.repo, check=True, capture_output=True)

    def commit(self):
        self.git("add", "--all")
        self.git("commit", "--quiet", "-m", "fixture")

    def scan(self, expected, *args):
        result = subprocess.run([sys.executable, str(CHECKER), "--report", str(self.report), *args],
                                cwd=self.repo, capture_output=True, encoding="utf-8")
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return json.loads(self.report.read_text(encoding="utf-8"))

    def test_full_is_independent_of_worktree_diff(self):
        self.assertEqual(self.scan(0)["files"], [])
        self.src.unlink()
        self.assertEqual(self.scan(0)["over_limit_occurrences"], [])
        report = self.scan(1, "--full", "--revision", "HEAD")
        self.assertEqual(report["over_limit_occurrences"][0]["parameters"], 6)

    def test_callable_kinds_and_non_callable_expressions(self):
        self.src.write_text("""
            struct C { C(int, int, int, int, int); void ok(int, int, int, int); };
            using Callback = void(*)(int, int, int, int, int);
            void (*cb)(int, int, int, int, int);
            template<class T> void with_default(T, int, int, int, int n = 0) {}
            auto l = [](int, int, int, int, int){};
            template<class T> concept HasF = requires(T a,T b,T c,T d,T e) { f(a,b,c,d,e); };
            void caller() { with_default(1,2,3,4,5); }
        """, encoding="utf-8")
        report = self.scan(1, "--full")
        counts = [r["parameters"] for r in report["over_limit_occurrences"]]
        self.assertEqual(counts, [5] * 5)
        self.assertCountEqual([r["kind"] for r in report["over_limit_occurrences"]],
                              ["declaration", "function_type", "function_type", "definition", "lambda"])

    def test_diff_multiplicity_edits_moves_and_revision_scope(self):
        self.src.write_text(BAD.replace(", ", ",") + BAD, encoding="utf-8")
        report = self.scan(1, "--base", "HEAD")
        self.assertEqual([r["line"] for r in report["over_limit_occurrences"]], [2])
        self.src.write_text(BAD.replace("int f", "int changed"), encoding="utf-8")
        self.assertEqual(len(self.scan(1)["over_limit_occurrences"]), 1)
        self.src.rename(self.src.with_name("relocated.hpp"))
        self.report.unlink()
        self.commit()
        self.src.write_text(BAD, encoding="utf-8")
        report = self.scan(1, "--base", "HEAD^", "--revision", "HEAD")
        self.assertEqual(len(report["files"]), 1)
        self.assertTrue(report["files"][0].endswith("relocated.hpp"))

    def test_parse_failure_is_incomplete_with_locations(self):
        for source, kind in [("void f(int x DEFAULT, int b);", "ERROR"), ("struct C { int n; }", "missing"),
                             ("#define PARAMS int a,int b,int c,int d,int e\nvoid f(PARAMS);",
                              "unexpanded parameter type"),
                             ("#define DECL(T) void T(int,int,int,int,int)\nDECL(f);", "unexpanded declaration")]:
            with self.subTest(kind=kind):
                self.src.write_text(source, encoding="utf-8")
                report = self.scan(2, "--full")
                self.assertEqual(report["status"], "INCOMPLETE")
                self.assertTrue(any(g["kind"] == kind and g["line"] >= 1 for g in report["parse_gaps"]))

    def test_manifest_covers_external_and_untracked_consumers(self):
        path = Path("outside/current/consumer.ixx")
        final = Path("outside/final/consumer.hpp")
        for source in (path, final):
            dest = self.repo / source
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(BAD, encoding="utf-8")
        manifest = self.repo / "file_actions.json"
        manifest.write_text(json.dumps({"files": [{"path": "outside/old.hpp", "adaptation_paths": [path.as_posix()],
                                                   "final_paths": [final.as_posix()]}]}), encoding="utf-8")
        report = self.scan(1, "--manifest", str(manifest))
        self.assertEqual(report["files"], [path.as_posix(), final.as_posix()])


if __name__ == "__main__":
    unittest.main()
