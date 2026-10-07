# Editor Framework v2

`editor/` is the new framework. The previous product is isolated in `editor_legacy/`.
The default build installs one `lux_editor`; it displays a Welcome Pane and does not load authoring tools.

## Owners and boundaries

| Owner | Responsibility |
|---|---|
| `LuxEngine` | Native window, EngineContext, UI Scene transport, optional project Context, one frame loop |
| `EditorWindow` | GLFW lifetime entry, one Input sample, one ui::Root |
| `ui::Root` | Sole generational sparse owner of Panes, content traversal, focus/capture and docking |
| `EditorContext` | Project values, independent AssetVfs, three frozen registrars; explicit EngineContext borrow |
| `EditorServiceRegistrar` | Lazy unique service instances; reverse successful-construction destruction |
| `EditorUiRegistrar` | Detached Pane factories; invocation lives in the UI library, ownership passes to Root |
| `SceneToolRegistrar` | Typed factories selected by WorldDescription predicates; no cached tool instances |
| `EditorUiScene` | DrawData slots, immediate resource pinning, Scene input publication and existing Runtime retirement |

Context is owner-thread only. `EditorContext::create` completes assembly and privately freezes the registrars;
callers cannot freeze early or replace factories afterwards.
Service requests return borrowed references, valid until Context destruction. Factories may request other registered
services; recursive creation fails, and failed creation is not cached as success. There is no dynamic plugin/scope protocol.

UI factories use canonical string `PaneDescription.type`; `name` is the stable layout identity,
and `title` is visible text. Factories receive values and Context, returning a complete detached `unique_ptr<ui::Pane>`. They must not start work
that depends on a not-yet-published project. Root prepares the entire batch before transferring any owner. Removal first
revokes routing, then removes ownership lookup, then notifies, and destroys the Pane after callbacks return. Structural
calls from drawing, event delivery or attachment callbacks return BUSY. Existing Root route indices are not owners.

`openProject()` accepts **in-memory** `ProjectDescription` and `EditorLayout`, plus a borrowed synchronous assembly
function. Pure validation failure preserves the current project. After teardown starts, a factory failure destroys all
candidate Panes before their Context and leaves no project. EngineContext, window and ImGui Context survive project
switches. UI dies before project services; Engine/GPU retirement completes before the native window is destroyed.

The loop collects platform/Process/object facts, samples Input once, lets Root adopt queued UI structure, draws when a slot is
available, pins the captured data immediately, evaluates ActionMapper with UI capture, publishes the pending UI frame and calls
`SceneRuntime::driveFrame()` once. Backpressure does not redraw pending frames. A paused UI Simulation still maintains
its SceneSystems. The existing Runtime performs final resource retirement; there is no new business close coordinator.

## Build and install

The production targets are `lux_editor_context`, `lux_editor_ui`, `lux_editor_app` (STATIC) and `lux_editor`.
The SDK package is `lux-engine-editor-framework`, with those three library component names under
`lux::engine::editor::`. Root and lux_editor_ui expose no Renderer or SceneRuntime dependency. UI Scene transport lives in the app private
include/source area; WindowInput is UI-private. Context registers factories but cannot invoke them. The public
`createPane` function and its declaration belong entirely to lux_editor_ui. `EditorLayout.hpp` owns layout values.

Use the EDITOR profile; legacy has no active build path. Build `all -j 4 -- -k 0`; CMake changes require a second
no-work build. `LUX_EDITOR_BUILD_NATIVE_TESTS` enables framework CPU tests; GPU and desktop lifecycle tests require their
explicit switches. Desktop lifecycle tests do not qualify system IME or interactive native input. Windows native output
is currently implemented; Linux native output returns an explicit unsupported error.

Framework v2 freezes `editor_legacy` as reference source. It has no active build or installation entry; requesting
`LUX_BUILD_EDITOR_LEGACY=ON` is rejected. To reproduce old results, use the corresponding historical Git revision.
Historical evidence was verified and archived outside the source tree; no old result qualifies the new framework.
New tests inspect the actual CMake dependency closure for legacy contamination.

Local build configuration uses `LUX_CMAKE_TOOLCHAIN`, `LUX_CMAKE_PREFIX_PATH` and `LUX_FRAMEWORK_SDK` environment
variables. Machine-specific paths belong to the local environment, not the tracked editor settings.

## Scope

Future tools belong in `tools/scene`, `tools/material`, `tools/flowforge`. Their services will own sessions; SceneSession
will own its SceneToolSet. SceneToolRegistrar selection is provisional and must be reviewed with that first real slice. No empty libraries or placeholder business classes are created in this stage. Real project file
loading, asset browsing, plugins, persistence, history and play mode remain outside this rewrite qualification.

The original ProjectBuilder user patch is preserved separately and **not applied**. Its mapped location is
`editor_legacy/authoring/project/src/ProjectBuilder.cpp`. Historical EC4 remains incomplete; this framework does not
retroactively qualify EC4 or previously deferred input/Linux/IME/performance results.

`LuxEngine::frame` returns EFrameStatus (RUNNING or EXIT_REQUESTED). Embedders access Root through
`window().uiRoot()`; no duplicate root accessor or public frame-count test probe is provided.

## Host cadence and measurements

The frame producer is work-conserving: no 16 ms deadline or idle polling timeout. The renderer owns VSync and
backpressure; Process/Object/native/Scene timer progress wakes the existing native wait. `frame()` remains a single
non-waiting host iteration; `exec()` also performs event-driven waiting. The product owns its menu/shortcut bindings.
`LuxEngine::statistics()` reports accumulated phases/counters and latest UI/scene sizes without retaining samples.
See `docs/editor-framework-v2.md` for phase definitions and the Event/Signal and cross-Pane boundaries.

`framework.pacing` is explicit automated desktop/GPU qualification (including VSync on/off and minimized wake), not
interactive native-input or IME certification. `ui.ocp_statistics` uses five warmups and thirty samples for 1/3 Panes
and 100/1000 retained labels per Pane; it makes no general zero-allocation or absolute FPS claim.
