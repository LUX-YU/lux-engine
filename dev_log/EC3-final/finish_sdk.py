"""Finish the restored SDK qualification without rewriting earlier command evidence."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys

w = Path(__file__).resolve().parent
c = json.loads((w / 'final-config.json').read_text())
s = Path(c['source'])
p = Path(c['prefix'])
base = s.parent / 'build/RelWithDebInfo' / ('ec3-sdk-' + c['implementation_sha'][:12])

def run(name, args):
    subprocess.run([sys.executable, str(w / 'run.py'), '--source', str(s), '--cwd', str(s),
                    '--runtime', str(p / 'bin'), name, *map(str, args)], check=True)

# The workspace consumer intentionally has an executable entry, not a CTest registration.
# The earlier empty CTest invocation remains recorded and is not counted as a passed test.
prior = next((r for r in json.loads((w / 'commands.json').read_text())
              if r['name'] == 'final-sdk-workspace-executable'), None)
if prior:
    assert prior['exit_code'] == 0 and prior['source_head'] == c['implementation_sha']
    assert hashlib.sha256((w / prior['log']).read_bytes()).hexdigest() == prior['sha256']
else:
    run('final-sdk-workspace-executable', [base / 'ec3-workspace/editor_ec3_workspace.exe',
                                          w / 'workspace-actual-files'])
control = next((r for r in json.loads((w / 'commands.json').read_text())
                if r['name'] == 'final-sdk-run-controls-v2-test'), None)
if control:
    assert control['exit_code'] == 0 and control['source_head'] == c['implementation_sha']
    assert hashlib.sha256((w / control['log']).read_bytes()).hexdigest() == control['sha256']
else:
    subprocess.run([sys.executable, str(w / 'run_controls_sdk.py')], check=True)

sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
manifest = (w / 'final-install-manifest.txt').read_text().splitlines()
installed = [Path(name) for name in manifest if name]
missing = [str(file) for file in installed if not file.is_file()]
assert not missing, missing
tracked = subprocess.check_output(['git', 'ls-files'], cwd=s, text=True).splitlines()
removed = ['editor/workbench/scene/codegen/inspector_codegen.py',
           'editor/application/extensions/src/BuiltinContributions.cpp']
assert all(name not in tracked for name in removed)
old_emitter = [str(file.relative_to(p)) for file in p.rglob('inspector_codegen.py')]
assert not old_emitter, old_emitter
installed_commands = p / 'include/lux/engine/editor/storage/ProjectCommands.hpp'
source_commands = s / 'editor/activities/project/include/lux/engine/editor/storage/ProjectCommands.hpp'
assert sha(installed_commands) == sha(source_commands)
units = []
for file in base.glob('ec3-*/compile_commands.json'):
    entries = json.loads(file.read_text())
    for item in entries:
        command = item['command'].replace('\\', '/').lower()
        assert '/pinclude/' not in command and '/sinclude/' not in command, item
        assert '/install/relwithdebinfo/include' not in command, item
    units.append({'group': file.parent.name, 'entries': entries})
(w / 'final-sdk-extra-compile-commands.json').write_text(json.dumps(units, indent=2) + '\n')
result = {
    'implementation_sha': c['implementation_sha'],
    'manifest_entries': len(installed), 'missing': missing,
    'scene_composition': {'bytes': (p / 'bin/lux_engine_scene_composition.dll').stat().st_size,
                          'sha256': sha(p / 'bin/lux_engine_scene_composition.dll')},
    'public_project_commands_sha256': sha(installed_commands),
    'old_emitter_install_hits': old_emitter, 'removed_production_paths': removed,
    'independent_compile_groups': [x['group'] for x in units],
    'note': 'Dependency toolchain includes are allowed; no source-private headers or earlier engine SDK prefix.',
}
(w / 'final-sdk-audit.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({k: v for k, v in result.items() if k != 'independent_compile_groups'}, indent=2))
