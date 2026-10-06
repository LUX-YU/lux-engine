# Editor Framework v1

`editor/` is the new framework. The previous product is isolated in `editor_legacy/`.
The default build installs one `lux_editor`; it displays a Welcome Pane and does not load authoring tools.

## Owners and boundaries

| Owner | Responsibility |
|---|---|
| `LuxEngine` | Object queue, native window, EngineContext, UI Scene transport, optional project Context, one frame loop |
| `EditorWindow` | GLFW lifetime entry, one Input sample, one EditorUIRoot |
| `EditorUIRoot` | SparseSet of unique top-level Panes; EXTERNAL parent links and existing PaneHandle routing |
| `EditorContext` | Project values, independent AssetVfs, three frozen registrars; explicit EngineContext borrow |
| `EditorServiceRegistrar` | Lazy unique service instances; reverse successful-construction destruction |
| `EditorUiRegistrar` | Detached Pane factories; invocation lives in the UI library, ownership passes to Root |
| `SceneToolRegistrar` | Typed factories selected by WorldDescription predicates; no cached tool instances |
| `EditorUiScene` | DrawData slots, immediate resource pinning, Scene input publication and existing Runtime retirement |

Context is owner-thread only. Registration finishes before `freeze()`; factories cannot be replaced afterwards.
Service requests return borrowed references, valid until Context destruction. Factories may request other registered
services; recursive creation fails, and failed creation is not cached as success. There is no dynamic plugin/scope protocol.

UI factories receive values and Context, returning a complete detached `unique_ptr<ui::Pane>`. They must not start work
that depends on a not-yet-published project. Root prepares the entire batch before transferring any owner. Removal first
revokes routing, then removes ownership lookup, then notifies, and destroys the Pane after callbacks return. Structural
calls from drawing, event delivery or attachment callbacks return BUSY. Existing Root route indices are not owners.

`openProject()` accepts **in-memory** `ProjectDescription` and `EditorLayout`, plus a borrowed synchronous assembly
function. Pure validation failure preserves the current project. After teardown starts, a factory failure destroys all
candidate Panes before their Context and leaves no project. EngineContext, window and ImGui Context survive project
switches. UI dies before project services; Engine/GPU retirement completes before the native window is destroyed.

The loop collects platform/Process/object facts, applies queued UI structure, samples Input once, draws when a slot is
available, pins the captured data immediately, evaluates ActionMapper with UI capture, publishes UI Scene input and calls
`SceneRuntime::driveFrame()` once. Backpressure does not redraw pending frames. A paused UI Simulation still maintains
its SceneSystems. The existing Runtime performs final resource retirement; there is no new business close coordinator.

## Build and install

The production targets are `lux_editor_context`, `lux_editor_ui`, `lux_editor_app` (STATIC) and `lux_editor`.
The SDK package is `lux-engine-editor-framework`, with those three library component names under
`lux::engine::editor::`. `EditorUIRoot.hpp` exposes no Renderer or SceneRuntime dependency; the UI Scene transport has
its own header. Context has no UI library dependency; `EditorUiRegistrar::create()` is implemented by `lux_editor_ui`.

Use EDITOR profile, `LUX_BUILD_EDITOR_LEGACY=OFF` (default). Build `all -j 4 -- -k 0`; CMake changes require a second
no-work build. `LUX_EDITOR_BUILD_NATIVE_TESTS` enables framework CPU tests; GPU and desktop lifecycle tests require their
explicit switches. Desktop lifecycle tests do not qualify system IME or interactive native input. Windows native output
is currently implemented; Linux native output returns an explicit unsupported error.

Legacy can be configured with `LUX_BUILD_EDITOR_LEGACY=ON`, then built explicitly as `lux_editor_legacy` and
`lux_launcher_legacy`. Its directory is excluded from default build and root installation. Existing test assertions and
historical snapshots remain available. Legacy migration architecture tests are a separate opt-in and are not the new
framework's dependency policy. New tests inspect the actual CMake dependency closure for legacy contamination.

## Scope

Future tools belong in `tools/scene`, `tools/material`, `tools/flowforge`. Their services will own sessions; SceneSession
will own its SceneToolSet. No empty libraries or placeholder business classes are created in this stage. Real project file
loading, asset browsing, plugins, persistence, history and play mode remain outside this rewrite qualification.

The original ProjectBuilder user patch is preserved separately and **not applied**. Its mapped location is
`editor_legacy/authoring/project/src/ProjectBuilder.cpp`. Historical EC4 remains incomplete; this framework does not
retroactively qualify EC4 or previously deferred input/Linux/IME/performance results.
