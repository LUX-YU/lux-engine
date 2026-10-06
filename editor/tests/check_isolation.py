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
        if name == 'lux_editor_ui':
            for forbidden in ('scene_render', 'scene_runtime', 'ui_rendering', 'lux_editor_app'):
                assert forbidden not in closure, (name, forbidden)
        print(name, 'closure:', ', '.join(closure))
print('PASS: new product dependency/include closure excludes legacy; Context excludes UI')

source = Path(model['paths']['source'])
removed = (
    'modules/core/object/include/lux/engine/object/ObjectDispatcher.hpp',
    'modules/core/object/include/lux/engine/object/ObjectIdentity.hpp',
    'modules/core/object/include/lux/engine/object/ObjectDeleter.hpp',
    'editor/ui/include/lux/engine/editor/EditorUIRoot.hpp',
    'editor/context/include/lux/engine/editor/FrameworkError.hpp',
    'editor/ui/include/lux/engine/editor/EditorUiScene.hpp',
    'editor/ui/include/lux/engine/editor/WindowInput.hpp',
)
for path in removed:
    assert not (source / path).exists(), path
print('PASS: removed public providers have no compatibility headers')
