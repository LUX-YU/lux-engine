"""Narrow check of the generated new-product dependency closure, not a replacement layering framework."""
import json
import sys
from pathlib import Path

build = Path(sys.argv[1])
reply = build / '.cmake/api/v1/reply'
indices = sorted(reply.glob('index-*.json'), key=lambda p: p.stat().st_mtime)
assert indices, 'CMake File API response is required'
index = json.loads(indices[-1].read_text())
model = json.loads((reply / index['reply']['codemodel-v2']['jsonFile']).read_text())
for config in model['configurations']:
    targets = {t['id']: json.loads((reply / t['jsonFile']).read_text()) for t in config['targets']}
    names = {t['name']: key for key, t in targets.items()}
    for name in ('lux_editor_context', 'lux_editor_ui', 'lux_editor_app', 'lux_editor'):
        seen, pending = set(), [names[name]]
        while pending:
            key = pending.pop()
            if key in seen:
                continue
            seen.add(key)
            target = targets[key]
            assert not target['name'].endswith('_legacy'), (name, target['name'])
            facts = json.dumps(target).replace('\\\\', '/')
            assert 'editor_legacy/' not in facts, (name, target['name'], 'legacy provider')
            pending.extend(d['id'] for d in target.get('dependencies', []))
        closure = sorted(targets[k]['name'] for k in seen)
        if name == 'lux_editor_context':
            assert 'ui' not in closure and 'lux_editor_ui' not in closure
        print(name, 'closure:', ', '.join(closure))
print('PASS: new product dependency/include closure excludes legacy; Context excludes UI')
