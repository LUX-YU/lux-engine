"""Inspect actual generated target/include closure; no layered framework or guessed directory ownership."""
import json
import sys
from pathlib import Path

reply = Path(sys.argv[1]) / '.cmake/api/v1/reply'
index = max(reply.glob('index-*.json'), key=lambda p: p.stat().st_mtime)
index = json.loads(index.read_text())
model = json.loads((reply / index['reply']['codemodel-v2']['jsonFile']).read_text())
for config in model['configurations']:
    targets = {item['id']: json.loads((reply / item['jsonFile']).read_text()) for item in config['targets']}
    names = {item['name']: key for key, item in targets.items()}
    for name in ('object', 'object_execution'):
        pending, seen = [names[name]], set()
        while pending:
            key = pending.pop()
            if key in seen:
                continue
            seen.add(key)
            target = targets[key]
            if name == 'object':
                assert target['name'] not in ('process_execution', 'object_execution'), 'OBJECT_REVERSE_PROCESS_EDGE'
            for group in target.get('compileGroups', []):
                for include in group.get('includes', []):
                    path = include['path'].replace('\\', '/')
                    assert '/editor/' not in path and '/editor_legacy/' not in path, 'BRIDGE_EDITOR_EDGE'
                    assert '/function/ui/' not in path, 'BRIDGE_UI_EDGE'
                    if name == 'object':
                        assert '/engine/process/' not in path, 'OBJECT_REVERSE_PROCESS_INCLUDE'
            pending.extend(item['id'] for item in target.get('dependencies', []))
        print(name, 'actual target/include closure PASS:', ', '.join(sorted(targets[key]['name'] for key in seen)))
print('PASS: Object independent of Process; bridge independent of Editor/UI')
