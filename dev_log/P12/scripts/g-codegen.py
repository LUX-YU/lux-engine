from pathlib import Path
r=Path(r'E:/SyncForder/CodeRepos/lux-engine-p12')
p=r/'editor/workbench/scene/codegen/inspector_codegen.py';s=p.read_text();a=s.index("        if self.config.get('session_fields'):");b=s.index('        return suffix, text',a)
# Generate directly against the formal author contract. Run controls share the same field-generation algorithm.
run=s[s.index("            run = text.replace",a):b];run='\n'.join(line[4:] if line.startswith('    ') else line for line in run.splitlines())+'\n';s=s[:a]+run+s[b:]
a=s.index('def generate(config, data):');b=s.index('\n\ndef main():',a)
s=s[:a]+'''def generate(config, data):
    if len(set(config["components"])) != len(config["components"]):
        raise ValueError("duplicate component implementation")
    outputs, declarations, bindings = {}, [], []
    for component in config["components"]:
        suffix, text = Generator(data, config).component(component)
        outputs[suffix + ".inspector.generated.cpp"] = text
        declarations.append(f"InspectorComponent binding_{suffix}();")
        bindings.append(f"binding_{suffix}()")
    header = (
        '#pragma once\\n#include <lux/engine/editor/scene/InspectorView.hpp>\\n#include <array>\\n'
        f'#include <{config["logical_path"]}>\\nnamespace lux::editor::scene::generated\\n{{\\n' +
        '\\n'.join(declarations) + f'\\ninline auto {config["name"]}Bindings()\\n{{\\n'
        '    return std::array{' + ', '.join(bindings) + '};\\n}\\n}\\n')
    run = header.replace('lux/engine/editor/scene/InspectorView.hpp', 'lux/engine/editor/scene/RunInspectorView.hpp')
    run = run.replace('namespace lux::editor::scene::generated', 'namespace lux::editor::scene::run_generated')
    run = run.replace('InspectorComponent', 'RunInspectorComponent')
    outputs[config['name'] + '.inspector.generated.hpp'] = header + '\\n' + run
    return outputs
'''+s[b:]
s=s.replace('InspectorInteraction','InspectorFields').replace('scene::SceneEditing&','InspectorFields&').replace('lux::simulation::ecs::Entity target','SceneObjectRef target').replace('EditorResult<void>','SceneEditResult<void>').replace('ComponentEditorRegistration','InspectorComponent').replace('namespace lux::editor::ui::generated','namespace lux::editor::scene::generated')
s=s.replace('using namespace generated_support;', 'using namespace lux::editor::ui;\nusing namespace lux::editor::ui::generated_support;')
s=s.replace('    InspectorFields& editing, SceneObjectRef target, InspectorFields& interaction) noexcept','    InspectorFields& interaction) noexcept')
s=s.replace('new Element_{suffix}(parent, std::move(id), editing, target, interaction, status)','new Element_{suffix}(parent, std::move(id), interaction, interaction.target(), interaction, status)')
s=s.replace('lux::cxx::unexpected(EditorFailure{{EEditorError::FRONTEND_FAILURE, "inspector.create"}})', 'lux::cxx::unexpected(InspectorFields::constructionFailure().error())')
p.write_text(s)
p=r/'editor/workbench/scene/cmake/engine_editor_imgui_inspector_codegen.cmake';s=p.read_text().replace('cmake_parse_arguments(ARG "SESSION_FIELDS"','cmake_parse_arguments(ARG ""');s=s.replace('    if(ARG_SESSION_FIELDS)\n        string(APPEND json ",\\"session_fields\\":true")\n    endif()\n','');p.write_text(s)
for f in ['editor/workbench/scene/CMakeLists.txt','cmake/installed-consumers/desktop-views/CMakeLists.txt']:
 p=r/f;p.write_text(p.read_text().replace(' SESSION_FIELDS',''))
# Unchanged GPU qualification now links its actual providers.
p=r/'editor/tests/integration/scene_views/CMakeLists.txt';s=p.read_text();s+='''
if(LUX_EDITOR_TEST_DESKTOP_GPU)
    add_executable(editor_ui_presentation unified_ui.cpp)
    target_link_libraries(editor_ui_presentation PRIVATE desktop_shell editor_viewport lux::engine::platform::window
        lux::engine::function::render_features lux::engine::scene::scene_transform)
    lux_editor_test_options(editor_ui_presentation)
    foreach(mode offscreen scene-panes)
        add_test(NAME editor.presentation_${mode} COMMAND editor_ui_presentation --${mode})
        set_tests_properties(editor.presentation_${mode} PROPERTIES LABELS "editor;desktop;gpu" RUN_SERIAL TRUE TIMEOUT 120)
    endforeach()
endif()
''';p.write_text(s)
p=r/'editor/workbench/scene/CMakeLists.txt';s=p.read_text();s+='''
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME editor.inspector_codegen COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/inspector_codegen.py")
    set_tests_properties(editor.inspector_codegen PROPERTIES LABELS "editor;native")
endif()
''';p.write_text(s)
p=r/'cmake/installed-consumers/scene-ui/CMakeLists.txt';s=p.read_text().replace('find_package(lux-engine-editor-ui REQUIRED COMPONENTS editor_ui)','find_package(lux-engine-editor-desktop REQUIRED COMPONENTS desktop_shell)\n    find_package(lux-engine-editor-viewport REQUIRED COMPONENTS editor_viewport)').replace('editor/ui/test/unified_ui.cpp','editor/tests/integration/scene_views/unified_ui.cpp').replace('        lux::engine::editor::editor_ui lux::engine::platform::window','        lux::engine::editor::desktop_shell lux::engine::editor::editor_viewport lux::engine::platform::window');p.write_text(s)
