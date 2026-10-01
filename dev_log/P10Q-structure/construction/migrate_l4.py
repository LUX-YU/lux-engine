from pathlib import Path
import json

repo=Path(r'E:/SyncForder/CodeRepos/lux-engine-p10q-structure')
work=Path(__file__).resolve().parent
plan=json.loads((work/'file-plan.json').read_text())
for item in plan:
    if not item['source'].endswith(('src/LaunchEditor.cpp','launcher/LaunchEditor.hpp')):continue
    a,b=repo/item['source'],repo/item['destination']
    assert a.resolve().is_relative_to(repo) and b.resolve().is_relative_to(repo) and not b.exists()
    b.parent.mkdir(parents=True,exist_ok=True);a.rename(b)

def write(name,s):
    p=repo/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(s,newline='\n')

write('editor/application/CMakeLists.txt','add_subdirectory(launch)\n')
write('editor/application/launch/CMakeLists.txt','''add_component(COMPONENT_NAME editor_launch NAMESPACE lux::engine::editor STATIC SOURCE_FILES src/LaunchEditor.cpp)
component_include_directories(editor_launch BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(editor_launch PUBLIC lux::engine::editor::editor_contracts
    PRIVATE lux::engine::platform::platform_process)
component_add_transitive_commands(editor_launch
    "find_package(lux-engine-editor-contracts REQUIRED COMPONENTS editor_contracts)"
    "find_package(lux-engine-platform REQUIRED COMPONENTS platform_process)")
lux_classify_target(TARGET editor_launch LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-launch VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_launch)
''')
p=repo/'editor/launcher/CMakeLists.txt';s=p.read_text().replace('src/ProjectCreationPane.cpp src/LaunchEditor.cpp','src/ProjectCreationPane.cpp')
s=s.replace('PRIVATE lux::engine::platform::platform_process lux::engine::scene::scene_asset',
            'PRIVATE lux::engine::editor::editor_launch lux::engine::scene::scene_asset')
s=s.replace('"find_package(lux-engine-platform REQUIRED COMPONENTS platform_process)"',
            '"find_package(lux-engine-editor-launch REQUIRED COMPONENTS editor_launch)"')
p.write_text(s,newline='\n')
p=repo/'editor/app/CMakeLists.txt';s=p.read_text().replace('lux::engine::editor::editor_settings lux::engine::editor::editor_launcher',
    'lux::engine::editor::editor_settings lux::engine::editor::editor_launcher lux::engine::editor::editor_launch')
p.write_text(s,newline='\n')
write('editor/CMakeLists.txt','''include(${PROJECT_SOURCE_DIR}/cmake/EditorTests.cmake)

# Formal providers, configured inside-out. Layers are source ownership, not aggregate libraries.
add_subdirectory(editing)
add_subdirectory(authoring)
add_subdirectory(activities)
add_subdirectory(workbench)
add_subdirectory(application)

# Existing product only: contribution protocols expire at P11, product conversion at P12.
# No formal provider above may depend on these implementations.
add_subdirectory(metadata)
add_subdirectory(plugins)
add_subdirectory(context)
add_subdirectory(ui)
add_subdirectory(tools)
add_subdirectory(launcher)
add_subdirectory(app)

# All cross-layer targets see their real providers before target_link_libraries.
add_subdirectory(assets) # Remaining P12 save-adapter tests.
add_subdirectory(tests/integration)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_subdirectory(tests/architecture)
    add_subdirectory(tests/persistence)
    add_subdirectory(tests/workspace)
endif()
''')
write('editor/application/README.md','''# Application

`editor_launch` owns the existing process launch function used by Launcher and product commands.
It contains no project creation UI or author model. Runtime/service assembly remains in the
formal integration harness; it is not installed as another product. The existing desktop product
still uses its explicitly retained app/context until the P12 switch.
''')
write('editor/application/launch/README.md','''# Editor process launch

`launchEditor(installation, project_file)` asks the platform process API to start the installed Editor.
Its existing signature, error payload and logical include remain unchanged. The caller owns scheduling;
this library does not create an executor, project, window or second application context.
''')
write('editor/README.md','''# Editor

The formal implementation is organized by responsibility, from inner to outer:

| Layer | Source responsibilities | Dependencies |
| --- | --- | --- |
| editing | identities, History, SessionStore, SessionState, shared values | pure foundation |
| authoring | Scene, Material, Flow, Project, Layout values and author models | editing and pure engine/modules |
| activities | save/publication, Run, projection, compilation/preview, project/workspace IO, tasks | formal inner contracts and engine capabilities |
| workbench | desktop, viewport, widgets, domain interaction and views | inner public contracts; no activity private state |
| application | existing process launch and concrete assembly boundaries | explicit providers |

Layers are directories, not five libraries. Targets retain actual CPU, Process, Toolchain and GPU
boundaries. Material/Flow/Scene interactions remain CPU libraries despite residing in workbench.
ProjectBuilder creates a ProjectBuildConfig; only project activities perform file IO.

History has one shared algorithm. SessionStore owns author sessions; each model owns its source and
SessionState. SaveService and WriteCoordinator keep accepted work and publication facts. SceneRuntime
owns runtime instances and retirement. RunStore owns run records; ViewHost alone owns top-level Panes.
Views retain explicit bindings, gestures and local presentation. Closing a View does not close a session,
cancel another owner's task or release its unfinished GPU responsibility.

Public includes/packages retain their logical names. Project-only support lives in sinclude; private
implementation headers are not SDK APIs. Inspector generation lives in workbench/scene; generic widgets
and viewport do not depend on author models. Tests spanning layers are configured last under tests.

## Existing product, pending P11/P12

The executable has not switched to the new product workflow. `app`, `context`, `ui`, legacy concrete
`tools`, `launcher`, `metadata`, `plugins`, `transition`, and the old editing/save adapters retain only
their registered consumers. Contribution protocol replacement belongs to P11; product and adapter
removal belongs to P12. Formal providers must never include or link those implementations.

The old ProjectCreationPane remains in launcher; its process launch implementation is shared from
application/launch. The integration harness assembles formal modules and is never installed as a second
product. Layout values do not open content; WorkspaceStore returns plans/manifests without applying Root.

See [continuous quality rules](../docs/editor-quality.md). Mutable construction state is only in
`.internal/editor-redesign/`; frozen dev_log snapshots describe their own implementation SHA.
''')
print('L4 real process-launch provider and explicit product boundary completed.')
