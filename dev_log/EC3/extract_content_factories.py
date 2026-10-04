from pathlib import Path
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p=s/'editor/activities/sessions/include/lux/engine/editor/sessions/SessionCommands.hpp';a=p.read_text().replace('    using HistoryActionLookup', '''    class SessionPreparation;
    using SessionCreation = cxx::move_only_function<
        commands::CommandResult<commands::DispatchReceipt>(SessionPreparation)
    >;
    using HistoryActionLookup''');p.write_text(a)
for folder,ns,stem,name in [('material','material','Material','material'),('flow','flowforge','Flow','flow')]:
 p=s/f'editor/activities/{folder}/include/lux/engine/editor/{ns}/{stem}SessionFactory.hpp';a=p.read_text().replace('#include <lux/engine/editor/sessions/SessionFactory.hpp>','#include <lux/engine/editor/sessions/SessionFactory.hpp>\n#include <lux/engine/editor/sessions/SessionCommands.hpp>');last=a.rfind('}');signature=f'''    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeNew{stem}Command(
        commands::CommandEntry::Query, sessions::SessionCreation{', lux::flowforge::FlowSourceEnvironment' if folder=='flow' else ''}
    );
''';a=a[:last]+signature+a[last:];p.write_text(a)
 p=s/f'editor/activities/{folder}/src/SessionFactory.cpp';a=p.read_text();a='#include <random>\n'+a;source='lux::material::MaterialSource source{id, "Untitled Material", {}};' if folder=='material' else 'lux::flowforge::FlowSource source;\n                source.id = id;\n                source.name = "Untitled Flow";'
 a+=f'''
namespace lux::editor::{ns}
{{
    namespace
    {{
        constexpr commands::CommandDescriptor kNewCommand{{
            commands::CommandIdView{{"lux.editor.new.{name}"}}, "New {stem}", "File"
        }};
    }}
    std::shared_ptr<commands::CommandEntry> makeNew{stem}Command(
        commands::CommandEntry::Query query, sessions::SessionCreation receiver{', lux::flowforge::FlowSourceEnvironment environment' if folder=='flow' else ''}
    )
    {{
        return commands::CommandEntry::bind<kNewCommand>(
            contracts::CodeLease::builtin(), std::move(query),
            [create = std::move(receiver){', environment = std::move(environment)' if folder=='flow' else ''}](const commands::CommandInvocation&) {{
                std::mt19937 random{{std::random_device{{}}()}};
                const asset::AssetId id{{uuids::uuid_random_generator{{random}}()}};
                {source}
                return create(prepare{stem}Session({{std::move(source)}}, {{}}, {{}}{', environment' if folder=='flow' else ''}));
            }}
        );
    }}
}}
''';p.write_text(a)
# Scene creation: same preparation and error policy, now owned by Scene workbench.
p=s/'editor/application/extensions/src/BuiltinContributions.cpp';a=p.read_text();start=a.index('    std::shared_ptr<views::ViewFactoryEntry> builtinSceneCreationFactory(');end=a.index('    std::vector<std::shared_ptr<sessions::SessionFactoryEntry>>',start);body=a[start:end]
body=body.replace('builtinSceneCreationFactory','makeSceneCreationViewFactory').replace('ContentCreation','sessions::SessionCreation').replace('return views::ViewFactoryEntry::create(\n            contracts::CodeLease::builtin(),\n            views::ViewFactoryDescriptor{\n                views::ViewTypeIdView{"lux.editor.scene.creation"}, "New Scene", cxx::typeToken<std::monostate>()\n            },','return views::ViewFactoryEntry::bind<kCreationDescriptor>(\n            contracts::CodeLease::builtin(),').replace('                        auto package = lux::scene::createScenePackage(\n                            newAssetId(),','''                        std::mt19937 random{std::random_device{}()};
                        const asset::AssetId id{uuids::uuid_random_generator{random}()};
                        auto package = lux::scene::createScenePackage(
                            id,''').replace('viewFailure(', 'workbench::detail::viewFailure(')
p=s/'editor/workbench/scene/src/SceneCreationView.cpp';a=p.read_text();a='#include <lux/engine/editor/workbench/ViewFactorySupport.hpp>\n#include <lux/engine/editor/scene/SceneSessionFactory.hpp>\n#include <random>\n'+a;a+='''
namespace lux::editor::scene
{
    namespace
    {
        constexpr views::ViewFactoryDescriptor kCreationDescriptor{
            views::ViewTypeIdView{"lux.editor.scene.creation"}, "New Scene", cxx::typeToken<std::monostate>()
        };
    }
'''+body+'}\n';p.write_text(a)
p=s/'editor/workbench/scene/include/lux/engine/editor/scene/SceneCreationView.hpp';a=p.read_text().replace('#include <lux/engine/editor/views/IViewHost.hpp>','#include <lux/engine/editor/views/IViewHost.hpp>\n#include <lux/engine/editor/sessions/SessionCommands.hpp>\n\nnamespace lux::editor::views { class ViewFactoryEntry; }');i=a.rfind('}');a=a[:i]+'''    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> makeSceneCreationViewFactory(
        SceneConfigurationInputs, sessions::SessionCreation
    );
'''+a[i:];p.write_text(a)
p=s/'editor/workbench/scene/CMakeLists.txt';a=p.read_text().replace('PUBLIC lux::engine::editor::scene_control_api','PUBLIC lux::engine::editor::session_factories lux::engine::editor::scene_control_api').replace('PRIVATE lux::engine::scene::scene_transform','PRIVATE lux::engine::editor::scene_persistence lux::engine::scene::scene_transform').replace('component_add_transitive_commands(scene_ui','component_add_transitive_commands(scene_ui\n    "find_package(lux-engine-editor-session-factories REQUIRED COMPONENTS session_factories)"\n    "find_package(lux-engine-editor-scene-persistence REQUIRED COMPONENTS scene_persistence)"');p.write_text(a)
# App composes concrete provider calls; no helper registry or replacement command catalogue.
p=s/'editor/application/src/EditorContent.cpp';a=p.read_text().replace('        auto commands = extensions::builtinContentCommands(','        auto creation_available =').replace('            },\n            contentCreation(), flow_environment_\n        );\n        draft.commands.insert(draft.commands.end(), commands.begin(), commands.end());','''            };
        draft.commands.push_back(material::makeNewMaterialCommand(creation_available, contentCreation()));
        draft.commands.push_back(flowforge::makeNewFlowCommand(creation_available, contentCreation(), flow_environment_));''').replace('extensions::ContentCreation','sessions::SessionCreation');p.write_text(a)
p=s/'editor/application/src/EditorSceneTools.cpp';a=p.read_text().replace('extensions::builtinSceneCreationFactory','scene::makeSceneCreationViewFactory');p.write_text(a)
p=s/'editor/application/pinclude/lux/engine/editor/application/EditorApplicationImpl.hpp';a=p.read_text().replace('#include <lux/engine/editor/extensions/BuiltinContributions.hpp>','''#include <lux/engine/editor/scene/SceneSessionFactory.hpp>
#include <lux/engine/editor/material/MaterialSessionFactory.hpp>
#include <lux/engine/editor/flowforge/FlowSessionFactory.hpp>''').replace('extensions::ContentCreation','sessions::SessionCreation');p.write_text(a)
p=s/'editor/application/src/EditorCommands.cpp';a=p.read_text().replace('        auto builtins = extensions::builtinSessionFactories(registrations_.components, flow_environment_);\n        append(draft.sessions, builtins);','''        draft.sessions.push_back(scene::makeSceneSessionFactory(registrations_.components));
        draft.sessions.push_back(material::makeMaterialSessionFactory());
        draft.sessions.push_back(flowforge::makeFlowSessionFactory(flow_environment_));''');p.write_text(a)
# Test composition remains explicit, exercising all three providers and the original assertions.
p=s/'editor/tests/integration/session_factories/installation.cpp';a=p.read_text().replace('#include <lux/engine/editor/extensions/BuiltinContributions.hpp>','#include <lux/engine/editor/extensions/Contributions.hpp>');a=a.replace('extensions::builtinSessionFactories(schemas, {})','std::vector{scene::makeSceneSessionFactory(schemas), material::makeMaterialSessionFactory(),\n                flowforge::makeFlowSessionFactory({})}');p.write_text(a)
p=s/'editor/tests/integration/scene_views/views.cpp';a=p.read_text().replace('#include <lux/engine/editor/extensions/BuiltinContributions.hpp>','#include <lux/engine/editor/views/ViewFactory.hpp>');p.write_text(a)
# Delete the central production definition and header, now without a consumer.
(s/'editor/application/extensions/src/BuiltinContributions.cpp').unlink()
(s/'editor/application/extensions/include/lux/engine/editor/extensions/BuiltinContributions.hpp').unlink()
