#!/usr/bin/env python3
"""Tests only the document-pack audit helper, NOT lux-engine."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
AUDIT = HERE / 'audit_migration.py'

class AuditTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='lux-audit-test-')
        self.repo = Path(self.temp.name)
        subprocess.run(['git', 'init', '-q', str(self.repo)], check=True)

    def tearDown(self):
        self.temp.cleanup()

    def put(self, path, content):
        p = self.repo / path
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content, encoding='utf-8')
        return p

    def run_audit(self, stage):
        result = subprocess.run([sys.executable, str(AUDIT), '--repo', str(self.repo), '--stage', stage],
                                capture_output=True, text=True)
        return result, json.loads(result.stdout or result.stderr)

    def test_clean_source(self):
        self.put('editor/tools/scene/model/src/SceneSession.cpp','struct GoodValue {};\n')
        r, report = self.run_audit('P13')
        self.assertEqual(r.returncode, 0)
        self.assertEqual(report['inspected_source_files'], 1)

    def test_expired_path(self):
        self.put('editor/context/include/lux/engine/editor/EditorContext.hpp','// old path\n')
        r, report = self.run_audit('P12')
        self.assertEqual(r.returncode, 1)
        self.assertTrue(any(x['rule'].startswith('F') for x in report['findings']))

    def test_renamed_file_old_class(self):
        self.put('editor/odd/Leftover.hpp','class LUX_PUBLIC SceneEditor final {};\n')
        r, report = self.run_audit('P12')
        self.assertEqual(r.returncode, 1)
        self.assertTrue(any(x['rule'] == 'OLDDEF_SceneEditor' for x in report['findings']))

    def test_new_scope_old_include(self):
        self.put('editor/tools/scene/model/src/A.cpp','#include <lux/engine/editor/EditorContext.hpp>\n')
        r, report = self.run_audit('P02')
        self.assertEqual(r.returncode, 1)
        self.assertTrue(any(x['rule'] == 'NEW_DEPENDS_ON_OLD' for x in report['findings']))

    def test_bridge_expiry(self):
        self.put('editor/transition/LegacyPersistenceState.hpp','struct LegacyPersistenceState {};\n')
        self.assertEqual(self.run_audit('P01')[0].returncode, 0)
        self.assertEqual(self.run_audit('P12')[0].returncode, 1)

    def test_unregistered_transition(self):
        self.put('editor/transition/Forever.hpp','struct Placeholder {};\n')
        r, report = self.run_audit('P03')
        self.assertEqual(r.returncode, 1)
        self.assertTrue(any(x['rule'] == 'TRANSITION_UNREGISTERED' for x in report['findings']))

    def test_history_and_runtime_deadlines(self):
        self.put('editor/history/src/X.cpp','void EditHistory::beginSave() {}\n')
        self.assertEqual(self.run_audit('P00')[0].returncode, 0)
        self.assertEqual(self.run_audit('P01')[0].returncode, 1)
        (self.repo/'editor/history/src/X.cpp').unlink()
        self.put('engine/scene/src/Y.cpp','void SceneRuntime::invalid() {}\n')
        self.assertEqual(self.run_audit('P05')[0].returncode, 0)
        self.assertEqual(self.run_audit('P06')[0].returncode, 1)

    def test_bad_stage_is_error_not_pass(self):
        r, report = self.run_audit('P99')
        self.assertEqual(r.returncode, 2)
        self.assertEqual(report['status'], 'AUDIT_ERROR')

    def test_no_source_is_modified(self):
        p=self.put('editor/odd/Test.cpp','struct PaneManager {};\n')
        before=p.read_bytes()
        self.run_audit('P12')
        self.assertEqual(before,p.read_bytes())

if __name__ == '__main__':
    unittest.main(verbosity=2)
