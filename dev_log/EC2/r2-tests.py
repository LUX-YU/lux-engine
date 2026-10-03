from pathlib import Path
import json
s = Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
w = Path(__file__).parent
name = 'editor/activities/scene/test/configuration.cpp'
(s / name).write_text((w / 'pending/configuration-test.cpp').read_text(), newline='\n')
p = s / 'editor/tests/integration/project/CMakeLists.txt'
t = p.read_text()
if 'editor_scene_configuration_native' not in t:
    t += '''
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_scene_configuration_native ${PROJECT_SOURCE_DIR}/editor/activities/scene/test/configuration.cpp)
    target_link_libraries(editor_scene_configuration_native PRIVATE scene_execution lux::engine::project::project_plugin_rendering)
    lux_editor_test_options(editor_scene_configuration_native)
    add_test(NAME editor.scene_configuration_native COMMAND editor_scene_configuration_native "${CMAKE_BINARY_DIR}")
    set_tests_properties(editor.scene_configuration_native PROPERTIES TIMEOUT 30 LABELS "editor;native;EC2")
endif()
'''
    p.write_text(t, newline='\n')
p = s / 'editor/tests/architecture/rules.json'
j = json.loads(p.read_text())
if 'scene_transform' not in j['scene_execution']['direct']:
    j['scene_execution']['direct'].append('scene_transform')
for key,value in j.items():
    if isinstance(value,dict) and 'closure' in value:
        if key == 'scene_execution' or 'scene_execution' in value['closure']:
            value['closure']['scene_transform'] = {'path':'engine/scene/builtin_systems/transform'}
            value['closure']['scene_transform_configuration_codegen_generate'] = {'path':'engine/scene/builtin_systems/transform'}
# Tests configure after all production providers, and never expand production header admission.
j['editor_layering']['files'][name] = ['editor_scene_configuration_native']
j['editor_layering']['targets']['editor_scene_configuration_native'] = {
    'layer':'TEST','role':'TEST','capabilities':['CPU'], 'path':'editor/tests/integration/project'
}
j['scene_execution'].setdefault('test_headers',{})[name] = [
    'lux/engine/project/PluginManager.hpp','lux/engine/project/PluginRendering.hpp','iostream'
]
p.write_text(json.dumps(j,indent=2) + '\n', newline='\n')
