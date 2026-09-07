# L5 EditorContext、Toolset、Editor Extension Contribution 与生命周期设计

Status: **Normative L5 Foundation Design — v3 reconciled 2026-09-03**

Parent: `00-L5-architecture-overview.md`

General Project Module / source Plugin Package / RenderFeature composition is owned by `14-plugin-package-and-extension-composition.md`.

> 本文件只定义 EditorApplication / EditorContext / Toolset 以及 **Editor-owned extension points**。这里的 “contribution” 不是全 Engine `ContributionRegistry`，也不创建统一 Plugin runtime。

---

## 1. Design problem

Editor is a long-lived application. Windows may open/close repeatedly while shared application/tool capabilities remain alive.

Shared lifetimes include:

```text
product-wide VFS / AssetRead access
ExecutionRuntime / root TaskScope
long-lived L4 tools
UI session / commands
Selection
Scene metadata
configured Render presentation/runtime capability
Editor extensions contributed during composition
```

These lifetimes must not be owned by an individual `MaterialEditor`, `AssetBrowser`, `SceneEditor`, or plugin-provided Editor window.

---

## 2. Physical ownership

`EditorApplication` remains the physical composition owner.

```text
EditorApplication owns
    process::ExecutionRuntime
    process::TaskScope root_tasks
    mutable AssetVfs control plane
    concrete AssetRead endpoint/port
    Toolset
    EditorSelection
    UISession
    SceneMetaManager
    RenderRuntime/platform state when configured
```

`EditorContext` owns none of those application lifetimes.

It carries/references narrow capabilities:

```text
Toolset&
AssetVfsView
AssetReadPort
ExecutionRuntime&
TaskScope&
EditorSelection&
UISession&
const SceneMetaManager&
```

Context is explicit dependency convenience, not a hidden global service root.

---

## 3. Forbidden Context alternatives

Do not introduce:

```text
EditorContext::instance()
EditorServices
ServiceRegistry
EditorServiceProvider
getAnything<T>()
resolve(string)
EditorContext value-owning ExecutionRuntime/VFS
per-window VFS
```

Adding a new EditorContext capability requires a real Editor-wide lifetime/consumer case; arbitrary project/plugin-specific Systems do not belong in EditorContext.

---

## 4. Shutdown ordering invariants

Exact member order may follow implementation constraints, but these semantic invariants hold:

```text
stop admitting new Editor work
requestStop root TaskScope
close/wait root TaskScope until owned work is done
remove/destroy windows/listeners using EditorContext
reset/destroy EditorContext
requestStop/destroy Toolset after its users are gone
destroy Selection/UISession as dependency order permits
close AssetRead admission and settle outstanding reads
unmount/destroy mutable VFS
stop/join ExecutionRuntime after work using it is gone
tear down SceneMeta/Render/platform in dependency-safe order
```

Never destroy a capability while a live Context/window/task may still call it.

---

## 5. Toolset semantics

`Toolset` is the bounded long-lived L4 Editor tool capability set.

It is not a generic DI/service/plugin registry.

Composition model:

```text
COMPOSING
    install built-in tools
    install project/source-plugin Editor tools
    validate required tools
    freeze

RUNNING
    get/find only

STOPPING
    requestStop tools as defined
    no new install
```

Typed identity is used; string service keys are forbidden.

A missing required Tool fails closed. Toolset does not auto-create a dummy/no-op tool.

---

## 6. Tool lifetime and concurrency

A long-lived Tool owns stable facility/configuration state, not per-invocation mutable job state.

For a compiler Tool:

Allowed long-lived state:

```text
immutable compiler environment
immutable search/include configuration
copyable scheduler capabilities
read-only shared config
```

Per invocation owns:

```text
source snapshot
IR/MLIR context
mutable diagnostics
temporary files
receiver/stop state
```

The same Tool instance should be re-entrant/parallel when the underlying domain contract allows it. Do not put a whole-compiler mutex around all invocations merely for convenience.

---

## 7. Editor windows consume Toolset, they do not own it

Example relation:

```text
MaterialEditor
    holds EditorContext&
    may cache MaterialGraphCompiler& from Toolset
```

Closing the window destroys only window-local UI/document interaction state. It does not stop/destroy Toolset, VFS, ExecutionRuntime, root TaskScope, or already-admitted application-owned background work.

---

## 8. Editor Extension vs general Plugin Package

A source Plugin Package is defined in `14` and may have Domain/Render/Scene/Toolchain/Editor targets.

This file only defines the **Editor facet**.

An Editor facet may depend downward on its package/project public runtime/tool contracts and contribute Editor-only behavior.

Examples:

```text
TerrainEditor
Material custom editor
special Inspector binding
custom graph presentation
asset workflow
project-specific debug/visualization pane
```

The PLAYER/game product excludes the Editor target by dependency/product classification. No runtime permission system is required to keep Editor implementation out of the game.

---

## 9. Editor-owned contribution kinds

EditorApplication/composition may accept explicitly typed Editor contributions such as:

```text
Tool factories/descriptors
Editor window/Pane factories
commands
Component Inspector bindings targeting Lux UI
graph presentation/edit bindings targeting Lux UI/NodeCanvas
asset/editor workflow integration
```

A concrete aggregate descriptor is allowed if it is clearly Editor-owned, for example conceptually:

```text
EditorContribution
    tools
    windows
    component_editors
    graph_presentations
    commands
```

This descriptor must not grow into an all-engine `Contribution` universe.

Simulation, Render, Script and Toolchain keep their own owner-specific registration/composition contracts.

---

## 10. Contributed code lifetime

Tool factories, TypeTokens, generated binding function pointers, window factories and other static code references require their code to remain loaded while objects/registrations referencing them remain alive.

Source-linked Plugin v1 naturally satisfies this.

This document does not approve hot-unload binary plugins.

If a future binary extension model is introduced, its module lease/lifetime rules must independently prove that no Tool/window/generated binding survives unloaded code.

---

## 11. Concrete System access from Editor extension

It is legal for a specialized Editor tool to know a concrete project/plugin System's public type/configuration when it edits that exact domain.

It is not legal to globally discover it through singleton/service locator.

Use an explicit current Scene/Simulation/editor-session capability or canonical mutation/query path.

Allowed conceptually:

```text
TerrainEditor
    -> current Scene/Simulation session
    -> TerrainSystem public control/query or canonical Terrain mutation
```

Forbidden:

```text
TerrainSystem::instance()
PluginServices.get<TerrainSystem>()
EditorContext::getAnything<TerrainSystem>()
```

Do not add TerrainSystem to EditorContext simply because TerrainEditor exists.

---

## 12. UISession / CommandRouter / Lux UI

Editor windows and Editor extension code draw through Lux UI public APIs.

```text
Editor extension
    ↓
ui::Frame / Pane / Lux UI primitives
    ↓
private Lux UI backend
```

No direct Dear ImGui/imgui-node-editor types in plugin/editor public/generated code.

Commands are registered/routed through the Editor-owned command/session mechanisms, not by direct window-to-window calls.

Detailed UI contract: `10`.

---

## 13. Generated Inspector / graph contributions

First-party, project, and source-plugin generated UI bindings use the same public generator/Lux UI surface.

There is no runtime reflection fallback merely because a binding came from a plugin package.

Generated code:

```text
uses stable Component/graph semantic identity
calls Lux UI
calls canonical typed mutation/editing APIs
contains no ImGui types
```

---

## 14. Editor facet + Toolchain facet

A source package may provide both:

```text
terrain_toolchain
terrain_editor
```

The Editor facet may consume the Toolchain facet and install a long-lived Tool into Toolset during composition.

The runtime/domain facet must not depend upward on either.

---

## 15. Project switching / VFS

EditorApplication controls mutable `/Game`/project/plugin VFS mounts.

Windows receive `AssetVfsView` and should re-resolve stable asset identity after project generation changes where required.

Do not let each plugin/editor window create its own resource database or VFS.

---

## 16. Contribution freeze

Editor extension topology becomes stable before normal Editor runtime interaction.

```text
load/compose source-linked code
install tools
register windows/commands/generated bindings
validate conflicts/requirements
freeze Toolset/editor contribution set
start normal Editor work
```

Runtime add/remove/replace of arbitrary Editor contributions is not required for source Plugin v1.

---

## 17. STOP conditions

STOP for architecture review if Editor extension work appears to require:

```text
EditorServices / ServiceRegistry
EditorContext generic getAnything/resolve
Toolset expanded into arbitrary runtime service container
runtime-install/remove/replace of Tools after freeze
per-window ownership of application capabilities
plugin-specific System stored globally in EditorContext
all-engine ContributionRegistry
IPlugin startup/shutdown god interface
binary hot-unload semantics merely for source Plugin sharing
direct ImGui/node-editor use outside private Lux UI backend
```
