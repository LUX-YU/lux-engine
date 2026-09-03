# Lux Engine L5 Editor、Runtime Capability、Plugin Packaging 与 Product Composition 架构总览

Status: **Normative Architecture Baseline — v3 reconciled 2026-09-03**

Reviewed public repository checkpoint: `main@bc15a84252c5740e6e47f3e1094810d6dd4ab711`.

Canonical physical topology remains repository-owned; this document freezes semantic ownership and dependency direction. When current HEAD moves, coding agents may remap paths/targets but MUST NOT reinterpret an owner merely to finish a feature.

---

## 1. Architecture objective

Lux is organized around explicit owners and narrow capabilities rather than global managers.

The engine must support all of these without changing the underlying semantic model:

```text
normal project gameplay modules
reusable source Plugin Packages
headless/runtime-clean products
full Editor products
offline Toolchain products
multiple Script frontends/backends
third-party RenderFeatures
project-specific shipping executables
```

The key rule is:

> **Packaging does not redefine semantics.**

A Component remains a data/state contract whether it is first-party, project-owned, or distributed inside a Plugin Package. A System/Provider remains an implementation. A Script Ability/Event remains a contract. A RenderFeature remains a Render extension. Editor code remains Editor-only.

---

## 2. L0–L5 and Product composition

```text
L0 Reusable modules / foundations
    core/object/meta/math/serialization
    resource identity/VFS/descriptions
    function/ui
    function/graph
    function/script
    function/render foundations
    function/material / function/flowforge source vocabulary

L1 Domain
    ECS Registry / Components
    Simulation Systems
    authoritative synchronous gameplay/domain behavior
    ScriptSystem as Simulation scripting runtime owner

L2 Process
    ExecutionRuntime
    Cpu/Main/Blocking scheduling where authorized
    TaskScope
    Timer
    AssetRead / time-spanning process workflows

L3 Scene / integration
    Scene composition
    WorldObject/materialization integration
    render/simulation bridge
    Scene metadata

L4 Toolchain
    Material compiler/cooker
    FlowForge compiler
    asset import/cook
    build/package/codegen tools

L5 Editor
    EditorApplication composition leaf
    EditorContext / Toolset / Selection
    Inspector / AssetBrowser / SceneEditor
    MaterialEditor / FlowForgeEditor

Product/Application composition (not L6)
    Editor executable
    project-specific game/server/tool targets
```

Allowed dependency direction follows the above layering and explicit product classification. Lower runtime layers do not depend on Editor or Toolchain implementation.

---

## 3. Preserved foundation

The following are already foundation directions and are not redesign targets:

```text
B   L2 Execution
V1  AssetVfs read/control split
V2  VFS-backed AssetRead + blocking IO isolation
A   EditorApplication + non-owning EditorContext + Toolset
F   Shared Graph Source
G   Graph editing/render protocol separation
R0  foundation qualification/lifetime hardening
```

Preserve:

```text
bounded admission
structured task lifetime
explicit mutable VFS control plane + read snapshots
no VFS singleton
EditorApplication owns application lifetimes
EditorContext borrows/carries capabilities
Toolset mutates only during composition then freezes
GraphTopology/GraphLayout owned by modules/function/graph
GraphEditingSession separate from rendering/backend
```

---

## 4. Runtime Script architecture

The Script architecture is now a first-class runtime framework, not an Editor feature.

Unified ontology:

```text
Component
    state/data contract

System / integration object
    behavior/runtime owner

Script Ability
    script-to-engine callable contract

Capability Provider
    concrete runtime implementation of an Ability

HookPoint / EventPoint
    engine-to-script execution/event contract

Coroutine / Awaitable
    script control flow spanning time
```

The backend-neutral runtime relation is:

```text
ScriptArtifact
    ↓
ScriptInstance incarnation
    ↓
backend long-lived object state
    ↓
BoundScriptCall / BoundScriptStepCall
    ↓
ScriptSystem continuation / awaitable / stable-point runtime
```

FlowForge generated state machines, Lua coroutines, and future C++ coroutine handles remain backend-private representations.

See `11`, `12`, and `13`.

---

## 5. Gameplay Script object model

Runtime Script lifecycle follows ScriptInstance incarnation, not scene-global time and not persistent WorldObject identity.

```text
persistent WorldObject identity
    != runtime ECS Entity incarnation
    != ScriptInstance incarnation
```

Canonical gameplay lifetime:

```text
create backend object
    ↓
initialize instance/context/capabilities/methods
    ↓
BeginPlay
    ↓
ACTIVE gameplay
    ↓
RETIRING
    ↓
cancel/destroy gameplay continuations
    ↓
EndPlay(reason)
    ↓
release prepared methods / destroy backend object
```

Runtime spawn/materialization receives BeginPlay when admitted. Unmaterialization/retirement receives EndPlay. Rematerialization creates a new incarnation and therefore a new BeginPlay/EndPlay lifetime.

Temporary disable/throttling is activity policy, not object lifetime.

See `13-script-gameplay-object-lifecycle.md`.

---

## 6. Script Ability / Provider ownership

Scripts depend on contracts, never on concrete provider class identity.

Example:

```text
PhysicsQuery3D contract
        ↓ implemented by
JoltPhysicsSystem runtime object
```

The declaration belongs with the Physics semantic owner. Generated binder/thunks borrow an already-owned provider object; codegen never constructs or shared-owns the provider.

Dynamic path:

```text
composition publishes bound Ability
        ↓
Script mount resolves requirement once
        ↓
immutable prepared receiver/thunk
        ↓
call without string/service lookup
```

Shipping-known paths may later specialize the dispatch, but semantic identity remains `ContractId + schema`.

---

## 7. Async / stable-point model

All time-spanning Script work returns control to Simulation.

```text
start operation
    ↓
ScriptAwaitable PENDING/READY/FAILED/CANCELLED
    ↓
Script invocation SUSPENDED
    ↓
completion only marks readiness/enqueues
    ↓
explicit Simulation stable point
    ↓
validate ScriptInstance generation
    ↓
resume backend continuation
```

Never:

```text
worker -> lua_resume
physics callback -> FlowForge continue
Timer callback -> coroutine_handle.resume
GPU callback -> Script execution
```

Event.await shares the existing EventPoint source and ScriptSystem EventBucket; it is not a separate Event bus.

---

## 8. Lua portability

`LUA_SOURCE` is the canonical ScriptArtifact kind. The canonical asset stores source, not LuaJIT or Lua 5.4 bytecode.

Approved direction:

```text
same source / same ScriptArtifact semantics
        ↓
LuaJIT JIT ON
LuaJIT JIT OFF
Lua 5.4
```

JIT is optimization only. VM-specific C API differences remain in a narrow private Lua implementation seam. ScriptSystem and ScriptArtifact do not learn the VM kind.

The portable Lua profile must reject/avoid semantics that cannot be represented losslessly and consistently across the supported VMs.

---

## 9. Lux UI / Editor boundary

`modules/function/ui` is the public UI owner.

```text
Editor / generated UI / plugin Editor facet
        ↓
Lux UI public API
        ↓
private backend
        ↓
Dear ImGui / imgui-node-editor
```

Long-lived semantic objects may be object-oriented (`Pane`, `UISession`, `ViewportElement`, Editor windows), while leaf controls remain immediate-mode.

Do not create a retained WidgetManager/tree or speculative `IUiBackend` abstraction.

Legacy is only a selected visual/interaction reference.

---

## 10. EditorApplication / EditorContext / Toolset

Physical ownership remains:

```text
EditorApplication owns
    ExecutionRuntime
    root TaskScope
    mutable AssetVfs
    production AssetRead endpoint/port
    Toolset
    EditorSelection
    UISession
    SceneMetaManager
    configured RenderRuntime/platform state
```

`EditorContext` is non-owning and only carries/references the corresponding capabilities.

Toolset is a bounded exception to the “no registry” rule because it has one specific semantic purpose: long-lived L4 Editor tool capabilities.

```text
composition phase: install built-ins and Editor plugin tools
freeze
runtime: get/find only
shutdown: requestStop/destroy after users are gone
```

It is not a generic DI container.

---

## 11. Project Module vs Plugin Package

A project developer normally extends Lux directly with project modules. That does not require a plugin abstraction.

```text
Project Module
    normal gameplay/domain/render/tool/editor code owned by a project

Plugin Package
    optional reusable/source-distributed packaging around one or more modules
```

A Plugin Package may contain several targets/facets:

```text
domain/runtime
render
scene integration
toolchain
editor
```

These facets remain independently classified and only depend in legal directions.

Plugin is therefore orthogonal to architecture layers.

See `14-plugin-package-and-extension-composition.md`.

---

## 12. RenderFeature is already the Render extension contract

Do not introduce `RenderPlugin` as a second semantic layer.

`RenderFeature` already owns a RenderScene-installed renderer capability with:

```text
FeatureTypeId / descriptor
attach/detach lifecycle
per-view state
frame/render-graph contribution
configuration/registration
external-facing Render context/scene facades
```

A package may contain a RenderFeature target, but that target remains low-level Render code. If the same package also has `TerrainSystem` or Components, a higher Scene/integration target bridges Domain data to the RenderFeature.

Wrong:

```text
TerrainRenderFeature -> TerrainSystem / ECS / Editor
```

Correct:

```text
terrain_domain       terrain_render
       \               /
        \             /
         terrain_scene_integration
```

The packaging boundary never grants upward dependencies.

---

## 13. Toolchain and durable authoring

Material/FlowForge compiler instances remain long-lived immutable/reentrant tools; each invocation owns mutable compilation state and snapshots.

In-memory Editor integration is allowed after UI/NodeCanvas prerequisites.

Durable workflows remain STOP gates until approved source identity/codecs exist:

```text
MaterialGraph codec/document identity
FlowGraph codec + ScriptSymbol source identity/package contract
```

Do not invent a file format merely because a Save button needs one.

---

## 14. Product model

`PLAYER` is a runtime-clean qualification profile, not the final shipping architecture.

Final target:

```text
Project configuration
    + selected engine modules
    + selected Systems / Scene integrations
    + project native/generated code
    + source Plugin Package selections
    + selected Render backend/features
    + cooked content
        ↓
project-specific executable/server/tool target
```

Product Track P remains STOP until a project target-generation/manifest contract is approved.

Do not create a separate Plugin manifest first; plugin/package selections belong to the same Product composition model.

---

## 15. Current implementation direction

The unique current execution DAG is maintained in `07`.

High level:

```text
S4-P
 ↓
S5 Event.await + real domain Abilities
 ↓
S6 C++ coroutine/static specialization
 ↓
R1 whole-engine requalification
 ↓
U/C/D/E/U2
 ↓
H0/I0
 ↓
FC Engine Framework Closure
 ↓
H1/I1 persistence
 ↓
P product closure
```

No speculative S7 is planned.

---

## 16. Framework closure

Engine Framework v1 may be called closed only when FC in `07` passes:

```text
Runtime foundations stable
Script S1–S6 frozen
Lux UI boundary stable
Inspector / AssetBrowser / SceneEditor / Viewport working
NodeCanvas backend isolation complete
Material/FlowForge in-memory Editor integrations working
product/profile/install boundaries clean
no known architecture STOP condition outstanding
```

After FC, new systems/features should normally be ordinary feature production, not a root architecture migration.

---

## 17. Global prohibitions

```text
no generic global ServiceRegistry/Manager root
no hidden lifetime extension used to mask invalid ownership
no lower runtime layer depending on Editor/Toolchain
no Engine-specific Scene/System ontology pushed into reusable modules merely for convenience
no runtime provider identity persisted in ScriptArtifact
no per-call service/string lookup for Script Ability
no worker direct Script resume
no borrowed value across suspension
no public ImGui/node-editor dependency outside Lux UI backend
no universal graph/value/property framework
no persistence format invented from UI feature work
no PluginManager/IPlugin replacing owner-specific registration
no RenderPlugin duplicating RenderFeature
no plugin hot-unload ABI designed merely to support source sharing
no separate plugin manifest before Product P
```
