from pathlib import Path
import hashlib, json, re, subprocess

w = Path(__file__).resolve().parent
r = Path('E:/SyncForder/CodeRepos/lux-engine-p10q-structure')
original = w.parents[2]
sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=r, text=True).strip()
base = 'f7c27f9375cbf8dd8af37b30a6027a460de26213'
build = r.parent / 'build/RelWithDebInfo' / ('p10q-structure-' + sha[:12])
out = w / 'audit'
def write(name, data):
    (out / name).write_text(json.dumps(data, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')

units = json.loads((build / 'compile_commands.json').read_text())
editor = [x for x in units if '/editor/' in x['file'].replace('\\', '/')]
assert editor and all(re.search(r'[-/]std:c\+\+20\b', x['command']) for x in editor)
changed = subprocess.check_output(['git', 'diff', '--name-only', base, sha], cwd=r, text=True).splitlines()
headers = [p for p in changed if p.startswith('modules/') and '/include/' in p]
assert not headers
write('standards.json', {'implementation_sha': sha, 'editor_tus': len(editor), 'all_cpp20': True,
                       'android_build': 'NOT_RUN', 'modules_public_header_changes': headers})
graph = json.loads((build / 'editor-architecture/targets.json').read_text())
names = ['edit_history', 'edit_sessions', 'editor_tasks', 'tasks_ui', 'editor_launch', 'editor_metadata', 'editor_launcher']
actual = [{k: x[k] for k in ['name', 'TYPE', 'SOURCE_DIR', 'edges']} for x in graph if x['name'] in names]
assert len(actual) == len(names)
write('binary-boundaries.json', {'implementation_sha': sha, 'actual': actual,
    'reason': 'History/session identity and metadata shared state retain SHARED. TaskMonitor and launch are new real STATIC providers. Existing tasks_ui and editor_launcher retain actual implementation, not forwarding aliases.'})

protected = original / 'editor/project/src/ProjectBuilder.cpp'
raw = hashlib.sha256(protected.read_bytes()).hexdigest()
assert raw == 'ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c'
assert protected.read_bytes() == (w / 'protected/ProjectBuilder.cpp').read_bytes()
def git(repo, *args):
    return subprocess.check_output(['git', *args], cwd=repo, text=True).strip()
assert git(original, 'rev-parse', 'HEAD') == base
assert git(original, 'rev-parse', 'main') == '2bb33ff1a1f11025cf404e074c0e9b259239d8a4'
assert git(original, 'diff', '--name-only') == 'editor/project/src/ProjectBuilder.cpp'
assert not git(original, 'diff', '--cached', '--name-only')
assert not git(r, 'diff', '--name-only', base, sha, '--', 'dev_log')
subprocess.run(['git', 'apply', '--check', str(w / 'protected/ProjectBuilder-relocated.patch')], cwd=r, check=True)
write('protected-final.json', {'implementation_sha': sha, 'original_head': base,
    'original_branch': git(original, 'branch', '--show-current'), 'original_status': git(original, 'status', '--short'),
    'main': git(original, 'rev-parse', 'main'), 'project_builder_sha256': raw,
    'tracked_migration': 'editor/project/src/ProjectBuilder.cpp -> editor/authoring/project/src/ProjectBuilder.cpp',
    'relocated_patch_check': 'PASS (not applied)', 'old_dev_log_diff': []})
print('Final standards, binary boundaries and protected workspace verified', sha)
