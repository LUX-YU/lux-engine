"""P03 gate: validate archived evidence, using Git blobs for historical source identity."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
import zipfile


def check(root):
    here = root / 'dev_log/P03'
    def read(path): return json.loads(path.read_text(encoding='utf-8-sig'))
    def digest(data): return hashlib.sha256(data).hexdigest()
    def archived(name):
        path = PurePosixPath(name)
        assert not path.is_absolute() and '..' not in path.parts and '\\' not in name
        result = root.joinpath(*path.parts).resolve()
        assert result.is_relative_to(root)
        return result
    receipt = read(here / 'receipt.json')
    impl = receipt['implementation_sha']
    assert receipt['phase'] == 'P03' and receipt['status'] == 'PASS'
    assert receipt['stop_after'] == 'P03' and not receipt['continuation_authorized']
    assert receipt['input_sha'] == 'e0f440067792dc661e99a4f861a4116eeab4d304'
    for a, b in [(receipt['input_sha'], impl), (impl, 'HEAD')]:
        subprocess.run(['git', 'merge-base', '--is-ancestor', a, b], cwd=root, check=True)
    expected_predecessors = {'dev_log/' + p + '/receipt.json' for p in ['P00', 'P01', 'P01-R1', 'P02', 'P02-R1']}
    assert set(receipt['predecessor_receipts']) == expected_predecessors
    for name in receipt['predecessor_receipts']:
        predecessor = read(archived(name))
        assert predecessor['status'] == 'PASS'
        subprocess.run(['git', 'merge-base', '--is-ancestor', predecessor['implementation_sha'], receipt['input_sha']],
                       cwd=root, check=True)
    for item in read(here / 'artifacts.json'):
        assert digest(archived(item['archive_path']).read_bytes()) == item['sha256'], item
    spec = read(here / 'spec-input.json')
    assert digest(archived(spec['archive_path']).read_bytes()) == spec['sha256']
    original = (here / 'spec/P03_material_authoring.md').read_bytes()
    assert digest(original) == spec['original_reference_sha256']
    with zipfile.ZipFile(archived(spec['archive_path'])) as package:
        members = package.namelist()
        reference = next(n for n in members if n.endswith('reference/P03_material_authoring.md'))
        start = next(n for n in members if n.endswith('LUX_ENGINE_P03_START_AFTER_P02_R1_2026-09-28.md'))
        assert package.read(reference) == original
        assert package.read(start) == (here / 'spec/P03-start.md').read_bytes()
    for item in read(here / 'files.json'):
        revision = impl + ':' + item['path']
        if item['status'] == 'D':
            assert subprocess.run(['git', 'cat-file', '-e', revision], cwd=root, capture_output=True).returncode != 0
        else:
            assert digest(subprocess.check_output(['git', 'show', revision], cwd=root)) == item['git_content_sha256']
    commands = {PurePosixPath(c['archive_log']).stem: c for c in receipt['commands']}
    groups = ['editor-sessions-p01-consumer', 'ui-resources-editor-d2', 'ui-resources-external-feature',
              'ui-scene-pane-consumer', 'ui-views-gpu-consumer', 'scene-model-p02-consumer',
              'material-model-p03-consumer']
    scene = ['content', 'atomic', 'identity', 'changes', 'plugin'] + [f'mixed-{n}' for n in range(1, 5)] + [f'read-{n}' for n in range(5, 9)]
    material = ['content', 'failure', 'layout', 'lifetime', 'mixed-replace', 'mixed-recreate', 'reading']
    required = ['tracked-snapshot', 'configure', 'build', 'no-work', 'ctest', 'install', 'close-throw',
                'close-contract', 'sessions-detail', 'boundaries', 'model-boundaries', 'material-boundaries',
                'architecture', 'portable', 'historical-verifiers', 'package-audit', 'test-names',
                'model-imports', 'sessions-imports', 'material-imports', 'clone', 'clone-checkout',
                'clone-tracked', 'clone-configure', 'clone-architecture', 'C01', 'C03', 'C04']
    required += ['model-' + name for name in scene] + ['material-' + name for name in material]
    required += [group + '-' + action for group in groups for action in ['configure', 'build', 'no-work', 'ctest']]
    assert set(required) <= commands.keys(), set(required) - commands.keys()
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
    original = {x['name'] for x in read(root / 'dev_log/P02-R1/test-coverage.json')['final_tests']}
    assert original <= names
    assert {'editor.material_model.' + s for s in material} | {'editor.material_model_boundaries'} <= names
    for name in ['boundaries', 'model-boundaries', 'material-boundaries']:
        cases = read(here / 'evidence' / (name + '.json'))
        assert cases and all(x['passed'] and x.get('graph') and x['repaired_exit_code'] == 0 for x in cases)
    cases = {x['id']: x for x in read(here / 'evidence/material-boundaries.json')}
    for name in ['static-private-runtime', 'static-private-imported', 'compiler', 'storage', 'old-bridge-header']:
        assert cases[name]['exit_code'] != 0 and cases[name]['expected_rule']
    assert all(x['matched'] for x in read(here / 'logs/portability/results.json'))
    historical = read(here / 'logs/historical/results.json')
    assert {x['phase'] for x in historical} == {'P00', 'P01', 'P01-R1', 'P02', 'P02-R1'}
    assert all(x['exit_code'] == 0 for x in historical)
    coverage = read(here / 'test-coverage.json')
    assert coverage['current_phase'] == 'P03' and coverage['implementation_sha'] == impl
    results = {x['id']: x for x in receipt['test_results']}
    for name in [f'X03-{n:02}' for n in range(1, 5)] + ['P01', 'P01-R1', 'P02', 'P02-R1']:
        assert results[name]['status'] == 'PASS'
    for name in ['Q01', 'Q07', 'Q09', 'Q11', 'Q27']:
        assert results[name]['status'] == 'SCOPED_PASS' and results[name]['scope']
    ledger = read(here / 'migration-ledger.json')
    assert ledger['current_phase'] == 'P03' and ledger['implementation_sha'] == impl
    assert ledger['transition_bridges'] == read(root / 'dev_log/P02-R1/migration-ledger.json')['transition_bridges']
    assert all(x['delete_by'] == 'P12' for x in ledger['p03_retained_adapters'])
    for name, marker in [('C01', 'visible_before=0 visible_after=1'), ('C03', 'released_during_query=1'),
                         ('C04', 'create_succeeded=1 outcome_succeeded=0')]:
        assert marker in (here / 'logs' / (name + '.log')).read_text()
        assert receipt['known_failures'][name]['status'] == 'FAIL'
    imports = (here / 'logs/material-imports.log').read_text().lower()
    assert not any(x in imports for x in ['vulkan', 'editor_app.dll', 'editor_context.dll', 'editor_ui.dll',
                                          'project_storage.dll', 'scene_runtime.dll', 'scene_composition.dll', 'compiler'])
    for item in read(here / 'evidence/sdk-headers.json'):
        data = archived(item['archive_path']).read_bytes()
        assert digest(data) == item['sha256']
        assert item['matches_source']
        source = subprocess.check_output(['git', 'show', impl + ':' + item['source_path']], cwd=root)
        assert data.replace(b'\r\n', b'\n') == source.replace(b'\r\n', b'\n')
    protected = ['dev_log/P00', 'dev_log/P01', 'dev_log/P01-R1', 'dev_log/P02', 'dev_log/P02-R1',
                 'editor/app/test/baseline_failures.cpp', 'editor/app/src/EditorTestAccess.cpp', 'editor/transition',
                 'editor/editing/history', 'editor/editing/sessions', 'editor/tools/scene/model',
                 'editor/tools/material/test/material_protocol.cpp']
    assert not subprocess.check_output(['git', 'diff', receipt['input_sha'], impl, '--', *protected], cwd=root)
    print('PASS P03 gate: X03-01..04, scoped Q01/Q07/Q09/Q11/Q27, mixed batches and stable reads; '
          'P01/P02/R1 and old product regressions retained, actual dependency negatives, installed CPU consumer. '
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
