from pathlib import Path
import subprocess,re
r=Path(r'E:/SyncForder/CodeRepos/lux-engine-p12');d=r/'cmake/installed-consumers/editor-d2'
# Narrow SDK remains two DLLs (component schema and generated controls), with formal SceneSession access.
p=d/'Gui.cpp';s=p.read_text().replace('lux::editor::ComponentEditorRegistration','lux::editor::scene::InspectorComponent').replace('lux::editor::ui::generated','lux::editor::scene::generated');s=s.replace('            lux::editor::scene::SceneEditing& editing,\n            lux::simulation::ecs::Entity target,\n            lux::editor::ui::InspectorInteraction& interaction','            lux::editor::scene::InspectorFields& fields');s=s.replace('create(*result, lux::ui::ElementId{"generated"}, editing, target, interaction)','create(*result, lux::ui::ElementId{"generated"}, fields)').replace('        result.provider = {"consumer.editor", 1};\n','');p.write_text(s)
p=d/'include/consumer/Gui.hpp';s=p.read_text().replace('#include <functional>\n','').replace('#include <lux/engine/editor/ui/ComponentEditors.hpp>','#include <lux/engine/editor/scene/InspectorView.hpp>');s=s.replace('lux::editor::ComponentEditorRegistration','lux::editor::scene::InspectorComponent');a=s.index('    CONSUMER_GUI_PUBLIC void\n    checkUndrawnInspector');s=s[:a]+'    CONSUMER_GUI_PUBLIC void checkInspector();\n} // namespace consumer\n';p.write_text(s)
# Retain pure codec/Pak assertions verbatim; only replace the deleted old UI call chain.
p=d/'main.cpp';s=p.read_text();a=s.index('int sceneWorkflow(');b=s.index('void pakRoundTrip',a);s=s[:a]+s[b:];a=s.index('void sharedInspector()');b=s.index('int main(',a);s=s[:a]+s[b:];s=s.replace('    sharedInspector();','    consumer::checkInspector();');a=s.index('#if defined(LUX_TOOL_INTERNAL_TESTS)');s=s[:a]+'    return 0;\n}\n';p.write_text(s)
# Preserve existing real scalar/container/lifetime checks against the formal author Session.
old=subprocess.check_output(['git','show','HEAD:cmake/installed-consumers/editor-d2/GuiGesture.cpp'],cwd=r,text=True)
setup=(r/'cmake/installed-consumers/desktop-views/Inspector.cpp').read_text();a=setup.index('int main()');setup=setup[:a]+setup[a:].replace('int main()','void consumer::checkInspector()',1);setup=setup.replace('#include <consumer/Component.ecs_schema.hpp>\n#include <desktop_consumer.inspector.generated.hpp>','#include <consumer/Domain.hpp>\n#include <consumer/Gui.hpp>');setup=setup.replace('#include "ControlsTestAccess.hpp"','#include "../common/ControlsTestAccess.hpp"');setup=setup.replace('simulation::ecs::generated::DesktopConsumerComponentSchemas()','consumer::schemas()');setup=setup.replace('author::generated::desktop_consumerBindings()','std::array{consumer::binding()}');setup=setup[:setup.index('    const auto find =')]
setup+='''    const auto& schema = schemas.schemas().front();
    const auto component = [&]() {
        auto source = take(session->read());
        return take(source.withRead([&](const auto& read) -> author::SceneEditResult<consumer::Component> {
            auto encoded = take(read.component(target, schema.id));
            auto decoded = schema.decode_value(encoded.version, encoded.bytes, {}, {});
            assert(decoded);
            simulation::ecs::Registry registry;
            const auto entity = registry.create();
            std::move(*decoded).installInto(registry, entity);
            return registry.get<consumer::Component>(entity);
        }));
    };
    const auto setField = [&](std::string path, auto value) {
        author::SceneEditBatch batch{session->describe().current, "SDK field", {}};
        batch.edits.push_back(author::SceneSetField::make<consumer::Component>(target, schema.id, std::move(path), std::move(value)));
        return session->apply(std::move(batch));
    };
    const auto findControl = [&](auto&& self, object::LuxObject& node) -> ui::NumericEdit* {
        if (auto* control = dynamic_cast<ui::NumericEdit*>(&node)) return control;
        for (auto* child = node.firstChild(); child; child = child->nextSibling())
            if (auto* found = self(self, *child)) return found;
        return nullptr;
    };
    auto& pane = *inspector;
    auto* control = findControl(findControl, pane);
    assert(control);
    const auto read = [&] { return component().settings.gain; };
    const auto original = read();
    assert(std::get<double>(control->value()) == original);
    const auto before = session->historyView()->snapshot;
    control->setValue(original + 1.0);
    assert(read() == original);
    static_cast<void>(ui::ControlsTestAccess::edited(*control, {true, true, false, false}));
    frame();
    assert(read() == original && interaction.overlay());
    assert(session->historyView()->snapshot.current == before.current);
    assert(pane.finishEditing()); // No ImGui deactivation is required.
    assert(!interaction.overlay() && read() == original + 1.0);
    assert(session->historyView()->snapshot.cursor == before.cursor + 1);
    assert(session->undo() && read() == original);
    frame();
    assert(session->redo() && read() == original + 1.0);
    assert(session->undo());
    frame();
    assert(root->requestFocus(*control));
    control->setValue(original + 2.0);
    static_cast<void>(ui::ControlsTestAccess::edited(*control, {true, true, false, false}));
    frame();
    control->setValue(original);
    static_cast<void>(ui::ControlsTestAccess::edited(*control, {true, false, false, true}));
    frame();
    assert(!interaction.overlay() && read() == original);
    assert(session->historyView()->snapshot.current == before.current);
    assert(pane.clearTarget());
    assert(!root->focusedElement() && !findControl(findControl, pane));
    frame();
    assert(pane.rebind({key, &interaction}, target));
    frame();
    assert(findControl(findControl, pane));
'''
a=old.index('        const auto find =');b=old.index('        pane.requestClose();',a);body=old[a:b]
body=body.replace('(*root)->','root->').replace('history.view()','session->historyView()').replace('history.undo()','session->undo()').replace('history.redo()','session->redo()').replace('assert(root->update({}, nullptr));','frame();')
body=body.replace('        assert(component().pairs[0][1] == 27 && session->undo());','        frame();\n        assert(component().pairs[0][1] == 27 && session->undo());')
body=re.sub(r'        const auto writable = editing.writeTarget\(target\);[\s\S]+?        \)\);', '        assert(setField("sequence", large));',body,count=1)
body=re.sub(r'        const auto list_target = editing.writeTarget\(target\);[\s\S]+?        \)\);', '        assert(setField("list", long_list));',body,count=1)
body=re.sub(r'        const auto replacement_target = editing.writeTarget\(target\);[\s\S]+?        \)\);', '        assert(setField("list", long_list));',body,count=1)
setup+=body
setup+='''    assert(host.close(id) && host.drain());
    assert(!host.describe(id) && store.describe(key.id()));
    // A binding record retains code until the generated object destructor has returned.
    bool destroyed{}, released{};
    struct Probe final : ui::Element {
        bool& destroyed;
        bool& released;
        Probe(ui::Element& parent, ui::ElementId id, bool& d, bool& r)
            : Element(parent, std::move(id)), destroyed(d), released(r) {}
        ~Probe() noexcept override { assert(!released); destroyed = true; }
        void draw() noexcept override {}
    };
    struct State { bool* destroyed; bool* released; };
    static State state;
    state = {&destroyed, &released};
    auto code = std::shared_ptr<const void>(new int{1}, [&](const void* value) {
        assert(destroyed); released = true; delete static_cast<const int*>(value);
    });
    auto registration = consumer::binding();
    registration.code = code;
    registration.create = +[](ui::Element& parent, ui::ElementId id, author::InspectorFields&) noexcept
        -> author::InspectorComponent::CreateResult {
        return std::make_unique<Probe>(parent, std::move(id), *state.destroyed, *state.released);
    };
    auto candidate = take(author::makeInspectorView(queue.dispatcherRef(), ui::PaneId{"code-owner"},
        store.access<author::SceneSession>(), {key, &interaction}, target, schemas, {registration}));
    const auto probe = take(host.adopt(candidate, views::ViewRestoreKey{"code-owner"})).id;
    frame();
    registration = {};
    code.reset();
    assert(!released && !destroyed);
    assert(host.close(probe) && host.drain());
    assert(destroyed && released);
    std::puts("PASS generated Inspector: scalar preview/commit/cancel; nested/sequence/variant/optional/map/list; bounded rows; explicit finish; code lifetime");
}
'''
(d/'GuiGesture.cpp').write_text(setup)
p=d/'Targets.cmake';s=p.read_text().replace('lux::engine::editor::editor_ui','lux::engine::editor::scene_ui');a=s.index('if(TARGET editor_app)');s=s[:a];p.write_text(s)
p=d/'CMakeLists.txt';s=p.read_text().replace('find_package(lux-engine-editor-ui REQUIRED COMPONENTS editor_ui)','find_package(lux-engine-editor-scene-ui REQUIRED COMPONENTS scene_ui)').replace('include_component_cmake_scripts(lux::engine::editor::editor_ui)','include_component_cmake_scripts(lux::engine::editor::scene_ui)');p.write_text(s)
# Build the same public SDK component and control sources in the normal regression, without old app adapters.
sub=r/'editor/tests/integration/components';sub.mkdir(exist_ok=True)
(sub/'CMakeLists.txt').write_text('''if(LUX_EDITOR_TEST_INSTALLED_UI)
    include_component_cmake_scripts(meta)
    include_component_cmake_scripts(schema)
    include(${PROJECT_SOURCE_DIR}/editor/workbench/scene/cmake/engine_editor_imgui_inspector_codegen.cmake)
    set(D2_CONSUMER_SOURCE_DIR "${PROJECT_SOURCE_DIR}/cmake/installed-consumers/editor-d2")
    include("${D2_CONSUMER_SOURCE_DIR}/Targets.cmake")
    add_test(NAME editor.component_elements COMMAND consumer_protocol "${CMAKE_CURRENT_BINARY_DIR}/component-project")
    set_tests_properties(editor.component_elements PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 LABELS "editor;desktop;installed")
endif()
''')
p=r/'editor/tests/integration/CMakeLists.txt';p.write_text(p.read_text()+'\nadd_subdirectory(components)\n')
