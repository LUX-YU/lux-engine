"""Generator semantics only; editor.modules separately compiles actual reflected declarations."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

GENERATOR, TEMPLATES = sys.argv[1:3]
del sys.argv[1:3]


class ModuleGeneration(unittest.TestCase):
    def generate(self, declarations, *, logical='Fixture.hpp', name='selectedModules', success=True, empty=False):
        with tempfile.TemporaryDirectory(prefix='Modules 中文 ') as directory:
            root = Path(directory)
            ir = root/'input.json'
            ir.write_text(json.dumps({'declarations': declarations}), encoding='utf-8')
            config = root/'config.json'
            config.write_text(json.dumps({'name': name, 'namespace': 'test::product', 'inputs': [] if empty else [
                {'logical_path': logical, 'ir': ir.as_posix()}]}), encoding='utf-8')
            before = ir.read_bytes()
            result = subprocess.run([GENERATOR, str(config), TEMPLATES, str(root/'stage')], capture_output=True)
            self.assertEqual(ir.read_bytes(), before)
            if not success:
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertFalse((root/'stage').exists())
                return result.stderr.decode('utf-8')
            self.assertEqual(result.returncode, 0, result.stderr.decode('utf-8'))
            return (root/'stage/selectedModules.modules.cpp').read_text(), json.loads((root/'stage/semantics.json').read_text())

    @staticmethod
    def function(name='tool::module'):
        return {'id': name, 'fq_name': name+'()', 'invoke_name': name, 'params': [],
                '__kind': 'FunctionDecl', 'attributes': ['luxmodule']}

    def test_strong_references_not_factory_invocations(self):
        code, model = self.generate([self.function()])
        self.assertIn('&tool::module', code)
        self.assertNotIn('tool::module()', code)
        self.assertIn('std::is_same_v<decltype(&tool::module), GetModule*>', code)
        self.assertEqual(model['modules'], ['tool::module'])

    def test_selection_preserves_order_and_ignores_unmarked(self):
        unmarked = self.function('not::selected')
        unmarked['attributes'] = []
        _, model = self.generate([self.function('b::module'), unmarked, self.function('a::module')])
        self.assertEqual(model['modules'], ['b::module', 'a::module'])

    def test_duplicates_and_overloads_rejected(self):
        self.assertIn('duplicate', self.generate([self.function(), self.function()], success=False))

    def test_non_function_and_missing_marker_rejected(self):
        wrong = self.function()
        wrong['__kind'] = 'CXXRecordDecl'
        self.generate([wrong], success=False)
        self.generate([], success=False)

    def test_no_code_or_path_injection(self):
        for logical in ('../Foo.hpp', 'X".hpp', '/tmp/X.hpp', 'a/../X.hpp', 'C:/X.hpp', 'C:X.hpp'):
            self.generate([self.function()], logical=logical, success=False)
        for name in ('a();', '../outside', 'a::b'):
            self.generate([self.function()], name=name, success=False)
        self.generate([self.function('a();')], success=False)

    def test_zero_selected_modules(self):
        code, model = self.generate([], empty=True)
        self.assertEqual(model['modules'], [])
        self.assertIn('std::array<GetModule*, 0>', code)


unittest.main()
