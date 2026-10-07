"""Inspect actual generated target/include closure; no layered framework or guessed directory ownership."""
import json
import sys
from pathlib import Path

reply = Path(sys.argv[1]) / '.cmake/api/v1/reply'
index = max(reply.glob('index-*.json'), key=lambda p: p.stat().st_mtime)
index = json.loads(index.read_text())
model = json.loads((reply / index['reply']['codemodel-v2']['jsonFile']).read_text())
source = Path(model['paths']['source']).resolve()
editor = source / 'editor'
legacy = source / 'editor_legacy'
ui = source / 'modules/function/ui'
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
            provider = (source / target['paths']['source']).resolve()
            assert not provider.is_relative_to(editor) and not provider.is_relative_to(legacy), 'BRIDGE_EDITOR_EDGE'
            assert not provider.is_relative_to(ui), 'BRIDGE_UI_EDGE'
            if name == 'object':
                assert target['name'] not in ('process_execution', 'object_execution'), 'OBJECT_REVERSE_PROCESS_EDGE'
            for group in target.get('compileGroups', []):
                for include in group.get('includes', []):
                    path = Path(include['path']).resolve()
                    assert not path.is_relative_to(editor) and not path.is_relative_to(legacy), 'BRIDGE_EDITOR_EDGE'
                    assert not path.is_relative_to(ui), 'BRIDGE_UI_EDGE'
                    if name == 'object':
                        assert not path.is_relative_to(source / 'engine/process'), 'OBJECT_REVERSE_PROCESS_INCLUDE'
            pending.extend(item['id'] for item in target.get('dependencies', []))
        print(name, 'actual target/include closure PASS:', ', '.join(sorted(targets[key]['name'] for key in seen)))
print('PASS: Object independent of Process; bridge independent of Editor/UI')
