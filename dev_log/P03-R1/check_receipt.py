"""Validate the P03 R1 archive and fixed Git objects, never producer-machine paths."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import zipfile


def check(root):
    here = root / 'dev_log/P03-R1'
    def read(path): return json.loads(path.read_text(encoding='utf-8-sig'))
    def digest(data): return hashlib.sha256(data).hexdigest()
    def archived(name):
        path = PurePosixPath(name)
        assert not path.is_absolute() and '..' not in path.parts and '\\' not in name and ':' not in name
        result = root.joinpath(*path.parts).resolve()
        assert result.is_relative_to(root)
        return result
    receipt = read(here / 'receipt.json')
    base = receipt['input_sha']
    impl = receipt['implementation_sha']
    assert base == '7ead91c5f111c5edfd7d9f156e23e3f00af4a8eb'
    assert receipt['phase'] == 'P03-R1' and receipt['migration_stage'] == 'P03' and receipt['status'] == 'PASS'
    assert receipt['stop_after'] == 'P03' and not receipt['continuation_authorized']
    for a, b in [(base, impl), (impl, 'HEAD')]:
        subprocess.run(['git', 'merge-base', '--is-ancestor', a, b], cwd=root, check=True)
    for item in read(here / 'artifacts.json'):
        assert digest(archived(item['archive_path']).read_bytes()) == item['sha256'], item
    expected = {'editor/tools/material/model/' + p for p in
                ['CMakeLists.txt', 'src/PreparedMaterialReload.hpp', 'test/material_session.cpp']}
    changed = subprocess.check_output(['git', 'diff', '--name-only', base, impl], cwd=root, text=True).splitlines()
    assert set(changed) == expected, changed
    for item in read(here / 'files.json'):
        assert item['path'] in expected
        assert digest(subprocess.check_output(['git', 'show', impl + ':' + item['path']], cwd=root)) == item['git_content_sha256']
    before = read(here / 'before/input.json')
    assert before['base_sha'] == base
    original = subprocess.check_output(['git', 'show', base + ':' + before['production_path']], cwd=root)
    assert digest(original) == before['production_sha256']
    assert original == (here / 'before/production-before.hpp').read_bytes()
    patch_paths = ['editor/tools/material/model/test/material_session.cpp', 'editor/tools/material/model/CMakeLists.txt']
    patch = subprocess.check_output(['git', 'diff', base, impl, '--', *patch_paths], cwd=root)
    def normalize_patch(data): return re.sub(rb'^index [^\n]+\n', b'', data.replace(b'\r\n', b'\n'), flags=re.M)
    assert normalize_patch(patch) == normalize_patch((here / 'before/regression.patch').read_bytes())
    # Preserve every original assertion expression in the existing real-model suite.
    def assertions(data):
        text = data.decode()
        result = []
        for match in re.finditer(r'\bassert\s*\(', text):
            start = match.end()
            end, depth = start, 1
            while depth:
                if text[end] == '(': depth += 1
                elif text[end] == ')': depth -= 1
                end += 1
            result.append(re.sub(r'\s+', '', text[start:end - 1]))
        return Counter(result)
    test = patch_paths[0]
    old_asserts = assertions(subprocess.check_output(['git', 'show', base + ':' + test], cwd=root))
    new_asserts = assertions(subprocess.check_output(['git', 'show', impl + ':' + test], cwd=root))
    assert old_asserts <= new_asserts
    commands = {PurePosixPath(c['archive_log']).stem: c for c in receipt['commands']}
    groups = ['editor-sessions-p01-consumer', 'ui-resources-editor-d2', 'ui-resources-external-feature',
              'ui-scene-pane-consumer', 'ui-views-gpu-consumer', 'scene-model-p02-consumer', 'material-model-p03-consumer']
    new_cases = ['reload-unbound', 'reload-unbound-lease', 'reload-closing-lease', 'reload-reading-lease', 'reload-outcomes']
    old_commands = {PurePosixPath(c['archive_log']).stem for c in read(root / 'dev_log/P03/receipt.json')['commands']}
    required = old_commands | {'original-p03-gate'} | {'material-' + n for n in new_cases}
    assert required <= commands.keys(), required - commands.keys()
    for name, command in commands.items():
        assert command['implementation_sha'] == impl
        assert command['exit_code'] == (1 if name in ['C01', 'C03', 'C04'] else 0), name
        assert digest(archived(command['archive_log']).read_bytes()) == command['sha256'], name
    for name in ['configure', 'clone-configure']:
        assert '-DLUX_EDITOR_MIGRATION_STAGE=P03' in commands[name]['argv']
    for name in ['architecture', 'clone-architecture', 'package-audit']:
        args = commands[name]['argv']
        assert args[args.index('--stage') + 1] == 'P03'
        assert not read(archived(commands[name]['archive_log']))['findings']
    for name in ['no-work'] + [g + '-no-work' for g in groups]:
        assert 'ninja: no work to do' in archived(commands[name]['archive_log']).read_text()
    for name in ['ctest'] + [g + '-ctest' for g in groups]:
        assert '100% tests passed, 0 tests failed' in archived(commands[name]['archive_log']).read_text()
    names = {x['name'] for x in read(here / 'logs/test-names.log')['tests']}
    old_names = {x['name'] for x in read(root / 'dev_log/P03/test-coverage.json')['final_tests']}
    assert old_names <= names and {'editor.material_model.' + n for n in new_cases} <= names
    before_results = {r['scenario']: r for r in read(here / 'before/results.json')}
    assert set(before_results) == set(new_cases)
    for name, result in before_results.items():
        data = archived(result['archive_log']).read_bytes()
        assert digest(data) == result['sha256']
        if name in ['reload-unbound', 'reload-unbound-lease']:
            assert result['exit_code'] != 0
            for marker in ['nested_edit=1', 'source_unchanged=0', 'history_unchanged=0', 'observed_unchanged=0']:
                assert marker.encode() in data
        else:
            assert result['exit_code'] == 0
    for name in new_cases:
        data = (here / 'logs' / ('material-' + name + '.log')).read_text()
        for marker in ['source_unchanged=1', 'history_unchanged=1', 'observed_unchanged=1',
                       'dirty_unchanged=1', 'binding_unchanged=1', 'released_early=0', 'released_with_nodes=0']:
            assert marker in data, (name, marker)
        if name != 'reload-outcomes':
            assert 'nested_edit=0' in data and 'normal_public_edit_after_gate_release=1' in data
        if name in ['reload-closing-lease', 'reload-reading-lease']:
            assert 'gate_unchanged=1' in data
    for name in ['boundaries', 'model-boundaries', 'material-boundaries']:
        cases = read(here / 'evidence' / (name + '.json'))
        assert cases and all(x['passed'] and x.get('graph') and x['repaired_exit_code'] == 0 for x in cases)
    cases = {x['id']: x for x in read(here / 'evidence/material-boundaries.json')}
    for name in ['static-private-runtime', 'static-private-imported', 'compiler', 'storage', 'old-bridge-header']:
        assert cases[name]['exit_code'] != 0
    for name, marker in [('C01', 'visible_before=0 visible_after=1'), ('C03', 'released_during_query=1'),
                         ('C04', 'create_succeeded=1 outcome_succeeded=0')]:
        assert marker in (here / 'logs' / (name + '.log')).read_text()
        assert receipt['known_failures'][name]['status'] == 'FAIL'
    ledger = read(here / 'migration-ledger.json')
    old_ledger = read(root / 'dev_log/P03/migration-ledger.json')
    for field in ['transition_bridges', 'p03_extractions', 'p03_retained_adapters']:
        assert ledger[field] == old_ledger[field]
    assert ledger['p03_r1']['implementation_sha'] == impl and ledger['p03_r1']['status'] == 'PASS'
    results = {x['id']: x for x in receipt['test_results']}
    assert all(results['R03-' + str(n).zfill(2)]['status'] == 'PASS' for n in range(1, 5))
    assert all(x['matched'] for x in read(here / 'logs/portability/results.json'))
    assert all(x['exit_code'] == 0 for x in read(here / 'logs/historical/results.json'))
    assert 'PASS P03 gate' in (here / 'logs/original-p03-gate.log').read_text()
    spec = read(here / 'spec-input.json')
    package = archived(spec['archive_path'])
    assert digest(package.read_bytes()) == before['package_sha256'] == spec['sha256']
    with zipfile.ZipFile(package) as contents:
        member = next(n for n in contents.namelist() if n.endswith('/REVIEW_AND_INSTRUCTIONS.md'))
        assert contents.read(member) == (here / 'spec/REVIEW_AND_INSTRUCTIONS.md').read_bytes()
    print('PASS P03 R1 gate: real-model before/after R03-01..04, input owner/destruction/read scope; '
          'original 59 tests and assertions preserved, full regressions, installed consumers, explicit P03. '
          'C01/C03/C04 remain FAIL. Stop at P03.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence-root', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    try:
        check(args.evidence_root.resolve())
    except FileNotFoundError as error:
        print('EVIDENCE_INCOMPLETE: ' + str(error.filename), file=sys.stderr)
        raise SystemExit(1)
    except (AssertionError, KeyError, ValueError, OSError, subprocess.CalledProcessError) as error:
        print(str(error) or 'EVIDENCE_INVALID', file=sys.stderr)
        raise SystemExit(1)
