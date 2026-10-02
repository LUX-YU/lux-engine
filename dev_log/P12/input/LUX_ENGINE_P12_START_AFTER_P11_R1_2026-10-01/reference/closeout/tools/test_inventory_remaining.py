#!/usr/bin/env python3
"""Synthetic tests only: does not compile or test lux-engine."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('inventory_remaining', HERE / 'inventory_remaining.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class InventorySafetyTests(unittest.TestCase):
    def test_committed_only_and_no_modification(self):
        with tempfile.TemporaryDirectory(prefix='closeout-synthetic-') as directory:
            root = Path(directory)
            def run(*args):
                return subprocess.run(['git', '-C', str(root), *args], check=True,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout
            run('init', '-q')
            run('config', 'user.email', 'synthetic@example.invalid')
            run('config', 'user.name', 'Synthetic test')
            data = {
                'editor/app/src/Editor.cpp': 'class EditorContext {};\n',
                'editor/activities/project/CMakeLists.txt': 'add_library(editor_assets STATIC AssetImporter.cpp)\n',
                'editor/workbench/Bad.hpp': 'class PaneManager;\n',
                'editor/authoring/project/src/ProjectBuilder.cpp': '// tracked original\n',
                'dev_log/old/README.md': 'EditorContext FAIL\n',
                'editor/tests/negative.cpp': 'const char* expected = "EditorContext";\n',
            }
            for path, value in data.items():
                dest = root / path
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_text(value, encoding='utf-8')
            run('add', '.')
            run('commit', '-qm', 'synthetic baseline')
            protected = root / 'editor/authoring/project/src/ProjectBuilder.cpp'
            protected.write_text('// uncommitted user bytes\n', encoding='utf-8')
            user = root / 'untracked-user.txt'
            user.write_text('never touch\n', encoding='utf-8')
            before = run('status', '--porcelain=v1', '-z')
            report = module.collect(root, 'HEAD')
            after = run('status', '--porcelain=v1', '-z')
            self.assertEqual(before, after)
            self.assertEqual(protected.read_text(), '// uncommitted user bytes\n')
            self.assertEqual(user.read_text(), 'never touch\n')
            self.assertFalse(report['worktree_used_as_source'])
            self.assertEqual([f['path'] for f in report['legacy_root_files']], ['editor/app/src/Editor.cpp'])
            self.assertTrue(any(h['path'] == 'editor/workbench/Bad.hpp' for h in report['symbol_candidates']))
            self.assertTrue(any(h['path'] == 'editor/tests/negative.cpp' for h in report['symbol_candidates']))
            self.assertFalse(any('editor_assets' in h.get('symbols', []) for h in report['symbol_candidates']))
            self.assertFalse(any(h['path'].startswith('dev_log/') for h in report['symbol_candidates']))

    def test_invalid_ref_rejected(self):
        for ref in ('', '-bad', 'HEAD\nmain'):
            with self.assertRaises(ValueError):
                module.collect(Path('.'), ref)

    def test_output_never_overwritten(self):
        with tempfile.TemporaryDirectory(prefix='closeout-output-') as directory:
            root = Path(directory) / 'repo'
            root.mkdir()
            subprocess.run(['git', '-C', str(root), 'init', '-q'], check=True)
            out = Path(directory) / 'keep.json'
            out.write_text('original', encoding='utf-8')
            result = subprocess.run([sys.executable, str(HERE / 'inventory_remaining.py'), '--repo', str(root),
                                     '--output', str(out)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(out.read_text(), 'original')

    def test_repo_output_refused(self):
        with tempfile.TemporaryDirectory(prefix='closeout-repo-output-') as directory:
            root = Path(directory)
            subprocess.run(['git', '-C', str(root), 'init', '-q'], check=True)
            out = root / 'should-not-exist.json'
            result = subprocess.run([sys.executable, str(HERE / 'inventory_remaining.py'), '--repo', str(root),
                                     '--output', str(out)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(result.returncode, 2)
            self.assertFalse(out.exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
