import json
from pathlib import Path
s = Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p = s / 'editor/tests/architecture/rules.json'
j = json.loads(p.read_text())
for file in ['include/lux/engine/editor/scene/SceneConfigurationDraft.hpp', 'src/SceneConfigurationDraft.cpp']:
    j['editor_layering']['files']['editor/authoring/scene/' + file] = ['scene_model']
    if 'editor/authoring/scene/' + file not in j['scene_model']['files']:
        j['scene_model']['files'].append('editor/authoring/scene/' + file)
for file in ['include/lux/engine/editor/scene/SceneConfigurationPreparation.hpp', 'src/SceneConfigurationPreparation.cpp']:
    j['editor_layering']['files']['editor/activities/scene/' + file] = ['scene_execution']
    if 'editor/activities/scene/' + file not in j['scene_execution']['files']:
        j['scene_execution']['files'].append('editor/activities/scene/' + file)
for target, headers in {
    'scene_model': ['lux/engine/editor/scene/SceneConfigurationDraft.hpp', 'lux/engine/simulation/SimulationExecutionSpec.hpp'],
    'scene_execution': ['cmath', 'variant', 'lux/engine/serialization/PortableValueCodec.hpp'],
    'scene_ui': ['lux/engine/editor/scene/SceneConfigurationPreparation.hpp'],
}.items():
    for header in headers:
        if header not in j[target]['headers']:
            j[target]['headers'].append(header)
p.write_text(json.dumps(j,indent=2) + '\n',newline='\n')
l = Path('E:/SyncForder/CodeRepos/lux-engine/.internal/editor-redesign/migration-ledger.json')
j = json.loads(l.read_text(encoding='utf-8'))
j['ec2']['status'] = 'R2_IN_PROGRESS'
j['ec2']['batches']['R1'] = {
    'status':'IMPLEMENTED_DEVELOPMENT_VALIDATED',
    'sha':'6715621f23c06ccf5585b154fcf0163234b6208e',
    'evidence':['r1-before-mutable-plan','r1-before-project','r1-build-complete','r1-build-no-work',
      'r1-install','r1-sdk-positive','r1-sdk-manifest','r1-sdk-bytes','r1-sdk-open','r1-owner-regressions'],
    'note':'Final EC2 clean-SHA full matrix pending. Save As catalog path included after real full-build failure.'
}
j['ec2']['batches']['R2'] = 'IN_PROGRESS'
l.write_text(json.dumps(j,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
