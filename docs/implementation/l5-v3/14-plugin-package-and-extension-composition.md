# Project Module、Source Plugin Package、Editor Extension 与 RenderFeature Composition

Status: **Normative Source Plugin / Extension Composition Contract — 2026-09-03**

Parents:

- `00-L5-architecture-overview.md`
- `07-implementation-roadmap-and-gates.md`
- `08-normative-execution-contract.md`

Related:

- `01-editor-context-toolset-and-plugins.md`
- `10-lux-ui-foundation-and-legacy-visual-parity.md`
- `11/12` Script Ability/codegen contracts

> 本文件定义 source-shared/build-time-composed Plugin v1。它 supersede 任何把 Plugin 当成新的 Engine layer、用一个 `IPlugin`/`PluginManager` 横跨所有 owner、或为 RenderFeature 再造 `RenderPlugin` 语义层的旧建议。它不批准 binary hot-load/hot-unload ABI。

---

## 1. The four distinct concepts

Lux distinguishes:

```text
Project Module
    normal project/game/engine extension code unit

Plugin Package
    optional reusable/distributed source package containing one or more modules/targets

Editor Extension
    Editor-only facet/target supplied by a project or Plugin Package

RenderFeature
    Render subsystem's own low-level renderer extension contract
```

They are not subclasses of one `Plugin` base class.

---

## 2. Normal game development does not require Plugin

A game developer may directly create:

```text
Components
Systems
Script Abilities
Events
Scene integration
Render-facing data
project tools
```

This is normal project code.

Example:

```text
PoisonComponent
PoisonSystem
PoisonAbility
```

or a concept Lux does not know at all:

```text
VoxelWorld
ColonyEconomy
RailwayNetwork
CardBattle
ProceduralPlanet
```

No Plugin abstraction is required for those concepts to be first-class Lux gameplay.

Component/API are contracts; System/Provider is an implementation. Project composition selects concrete implementations.

---

## 3. Plugin Package is a distribution/build boundary

When a developer wants to share a coherent set of project modules with other projects, that set may be packaged/distributed as a Plugin Package.

A Plugin Package answers:

```text
what source targets are distributed together?
what package dependencies exist?
which classified facets may a product select?
```

It does **not** answer:

```text
how all Engine concepts call each other through one universal interface
```

Packaging never changes semantic ownership.

---

## 4. Plugin is orthogonal to architecture layers

A package may contain several independently classified targets/facets:

```text
Domain/runtime facet
    Components
    Simulation Systems/Providers
    Script Abilities / Events

Render facet
    RenderFeature
    shaders/render resources

Scene/integration facet
    bridges Domain/Scene facts to lower render/runtime capabilities

Toolchain facet
    importer/cooker/compiler/build/codegen support

Editor facet
    Editor Tool/Pane/window
    commands
    Inspector binding
    graph presentation/edit integration
    asset/editor workflow
```

Each target keeps the ordinary dependency DAG.

Plugin Package is not L6 and is not a cross-layer bypass.

---

## 5. Target graph, not a giant plugin target

Do not build one monolithic target such as:

```text
AcmeTerrainPlugin
    directly depends on Runtime + Render internals + Toolchain + Editor
```

Preferred composition:

```text
acme::terrain_domain
acme::terrain_render
acme::terrain_scene
acme::terrain_toolchain
acme::terrain_editor
```

Exact names are package-defined. Classification/ownership matters more than naming convention.

This allows product-specific closure:

```text
Game
    terrain_domain
    terrain_scene
    terrain_render (when rendered)

Dedicated Server
    terrain_domain
    optional non-render scene integration only
    NO terrain_render
    NO terrain_editor
    NO terrain_toolchain

Editor
    runtime facets required by edited project
    terrain_toolchain
    terrain_editor
```

---

## 6. Domain/runtime facet

A gameplay/domain package uses the same Engine extension contracts as ordinary project code.

Typical sources:

```text
Component declarations/meta
Simulation System implementation
SimulationSystemRegistration
Script Ability/Event declarations
Ability reflection/codegen
provider publication during composition
```

Do not create:

```text
PluginComponentId
PluginSystemId
PluginAbilityId
PluginSystemRegistry
PluginAbilityRegistry
```

The normal Component/System/Ability identities remain canonical.

---

## 7. Script Ability in source packages

A source Plugin Package declaring an Ability uses the same public `lux_script_abilities()`/codegen contract as an external game project.

```text
plugin domain target
    owns Ability declaration
    owns or references the real Provider implementation
        ↓
canonical generated metadata/binder
        ↓
optional higher consumer targets
    Lua schema/packaging
    FlowForge catalog
    Editor/tooling support
```

Domain target does not depend directly on Lua/FlowForge/Python runtime implementation.

No separate Plugin Script semantic registry.

---

## 8. RenderFeature is already the Render extension contract

Do not invent `RenderPlugin`.

`RenderFeature` already represents one capability installed into a RenderScene and has stable render-owned lifecycle/identity/registration mechanisms.

A third-party/public RenderFeature can use the Render public SDK/facades and register a factory/type without naming Engine Editor/System implementation.

Therefore a Plugin Package with rendering support simply carries a Render facet:

```text
plugin render target
    implements RenderFeature
    exports/uses ordinary RenderFeature registration/factory contract
```

Packaging does not wrap it in a second semantic object.

---

## 9. Low-level Render dependency direction

RenderFeature is lower-level than Simulation/Scene/Editor semantics.

It MUST NOT depend on a gameplay System merely because both are distributed in one package.

Wrong:

```text
TerrainRenderFeature
    -> TerrainSystem
    -> ECS/Simulation
```

Correct:

```text
terrain_domain       terrain_render
       \               /
        \             /
         terrain_scene
```

The higher Scene/integration target may know both the Terrain domain contract and the RenderFeature-facing registration/sync contract.

It observes/extracts/synchronizes state in the authorized direction.

---

## 10. RenderFeature + Component integration

When a RenderFeature consumes data represented by Components, do not pull ECS into the RenderFeature.

Use a Scene/integration binding:

```text
Component/System state
    ↓
Scene/integration observation/extraction
    ↓
render-safe data/control path
    ↓
RenderFeature
```

Existing structures such as RenderFeature metadata, configuration registration, sync-stage creation and component observation are examples of the correct higher-layer binding shape.

The low Render target remains usable by a client that only understands the Render SDK where its feature semantics permit.

---

## 11. Render-only Plugin Package

A valid source package may contain only a Render facet.

Examples:

```text
custom atmospheric renderer
volume renderer
postprocess feature
CUDA/graphics interop overlay
special scientific visualization feature
```

It does not need to fabricate Components/Systems/Editor code to be a “real plugin”.

Likewise a dialogue/gameplay package may have no RenderFeature at all.

Therefore Plugin Packages are **multi-facet**, not mutually exclusive `GameplayPlugin`/`RenderPlugin`/`EditorPlugin` classes.

---

## 12. Editor facet

Editor-only code lives in an Editor-classified target and may depend downward on the public contracts of the plugin's runtime/tool/render facets as appropriate.

An Editor facet may contribute, through Editor-owned composition seams:

```text
long-lived Tool facilities
Editor windows/Panes
commands
Component Inspector bindings generated against Lux UI
graph presentation/edit bindings against Lux UI/NodeCanvas
asset/import/editor integrations
special visualization/manipulation tools
```

Example:

```text
TerrainEditor
    may know TerrainComponent/Terrain config/System public control contract
    may use terrain Toolchain
    may use Lux UI/Viewport/Selection
```

But the Game/PLAYER target does not link `terrain_editor`, so game runtime cannot access Editor implementation.

This is a product dependency boundary, not a runtime permission check.

---

## 13. Editor access to a concrete System

It is legal for an Editor extension to know a concrete project/plugin System type when that Editor tool specifically edits that System's semantics.

It is not legal to discover it through a global singleton/service locator.

Allowed patterns depend on the current Scene/Simulation composition API, for example:

```text
Editor owns/uses current scene session capability
    ↓
explicit scene/simulation query/control path
    ↓
concrete TerrainSystem or canonical mutation
```

Forbidden:

```text
TerrainSystem::instance()
PluginServices.get<TerrainSystem>()
EditorContext::getAnything<TerrainSystem>()
```

Do not put every possible plugin System into EditorContext.

---

## 14. Toolset and Editor contribution freeze

`Toolset` remains the long-lived container for L4 Editor tool capabilities, not a general plugin registry.

Composition:

```text
create EditorApplication
install built-in Tools
install project/source-plugin Editor Tools
validate required tools
freeze Toolset
start Editor
```

After freeze:

```text
get/find only
no install/remove/replace
```

Contributed code/function pointers/TypeToken static data must remain loaded until all contributed objects are destroyed.

Source plugins naturally satisfy this because their code is linked into the product/build closure.

---

## 15. Editor contribution descriptors are owner-local

An Editor composition descriptor may aggregate Editor-specific facts such as:

```text
Tool contributions
Window contributions
Inspector bindings
Graph UI bindings
Commands
```

This is an **Editor extension point**, not a universal Engine Contribution framework.

Simulation owns System registration.
Render owns RenderFeature registration.
Script owns Ability/Event/codegen contracts.
Toolchain owns concrete tools/importers.
Editor owns Editor UI/tool contributions.

Do not merge all of these into one `ContributionRegistry`.

---

## 16. Toolchain facet

A Plugin Package may distribute:

```text
asset importer
cooker
compiler
codegen integration
build helper
```

Those targets remain Toolchain products and do not enter PLAYER/runtime closure unless a lower runtime contract truly requires separate data/code.

Editor facet may consume a Toolchain facet through Toolset/application composition.

Do not make the runtime domain target depend upward on its own cooker merely because they ship in one repository.

---

## 17. Assets / VFS

A source package may include cooked/runtime assets or source assets consumed by its Toolchain facet.

Runtime assets enter the same product-wide VFS/pak model as first-party/project assets.

The Plugin Package does not create a private AssetManager/VFS.

Potential mount roots such as `/PluginName` are a Product/VFS composition decision, not a reason to create Plugin-owned filesystem state.

---

## 18. Package identity

A Plugin Package may have a stable distribution/package identity for dependency and product selection purposes.

This identity is NOT a replacement for runtime semantic identities:

```text
Component identity
SystemTypeId
ScriptApiContractId
FeatureTypeId
Tool TypeToken
Command identity
```

Do not prefix every runtime identity with a new `Plugin*Id` layer.

---

## 19. v1 acquisition is intentionally non-normative

A source package may be obtained through:

```text
git submodule
FetchContent
package manager
installed find_package package
add_subdirectory
vendor/source copy under an approved project policy
```

Acquisition mechanism is not Engine runtime architecture.

What matters is that the resulting targets are explicit, classified, and obey dependency/product rules.

---

## 20. CMake target graph is v1 composition truth

Before Product Track P freezes a project/package manifest:

```text
CMake target graph
    = authoritative source-plugin build composition
```

Do not invent:

```text
plugin.toml
luxplugin.json
PluginManifest.rdesc
PluginActivationGraph
```

merely to begin sharing source modules.

When P is designed, Plugin Package identity/dependencies/selectable facets should become part of the **same** project target-generation model rather than a parallel manifest universe.

---

## 21. Product Track P integration

Future project target-generation needs to be able to select plugin facets according to product:

```text
Game executable
Dedicated server
Editor
Toolchain/cook host
other project-specific tools
```

The manifest/spec must distinguish product closure, not merely “plugin enabled yes/no”.

Example:

```text
AcmeTerrain package selected
    runtime target needs terrain_domain + terrain_scene + terrain_render
    server needs terrain_domain only/approved subset
    editor additionally needs terrain_toolchain + terrain_editor
```

Exact manifest syntax remains STOP until Product P review.

---

## 22. Binary Plugin ABI is a separate future problem

Source Plugin v1 does not require dynamic modules.

Do not pre-pay for:

```text
hot load
hot unload
ABI negotiation
marketplace runtime installation
live code replacement
DLL lifetime tracking across every owner
```

If a real product later requires binary plugins, design a separate contract.

A plausible first constrained binary model may be:

```text
same toolchain / fingerprint validation
load at startup/composition
no runtime unload
code remains loaded until all contributions are destroyed
```

But this document does not approve or require that ABI.

Older dynamic extension/ModuleLease architecture must not be resurrected merely because the word “plugin” is used.

---

## 23. Terrain example

A complete shared Terrain source package may be:

```text
AcmeTerrain

acme::terrain_domain
    TerrainComponent
    TerrainSystem
    TerrainQuery Ability
    TerrainChanged Event

acme::terrain_render
    TerrainRenderFeature
    TerrainGpuResources
    terrain shaders

acme::terrain_scene
    observe/extract Terrain state
    drive render-safe TerrainFeature state

acme::terrain_toolchain
    heightmap importer
    terrain cooker

acme::terrain_editor
    TerrainEditor
    brush Tools
    Inspector bindings
    viewport visualization/manipulation
```

Game:

```text
MyGame
    -> terrain_domain
    -> terrain_scene
    -> terrain_render
```

Editor:

```text
Editor<MyGame>
    -> project runtime closure
    -> terrain_toolchain
    -> terrain_editor
```

Dedicated server:

```text
MyServer
    -> terrain_domain
    -> only the non-render integrations actually required
```

No universal Plugin runtime object is necessary.

---

## 24. Qualification expectations for a reusable source package

At least one installed/external-package qualification should prove relevant facets using only public/installed seams.

Depending on package contents:

```text
Domain/System
    external system registration/install

Script
    external Ability reflection/codegen + provider publication

Render
    external RenderFeature build/registration against public Render SDK

Toolchain
    installed importer/compiler consumer

Editor
    generated Lux UI Inspector/Tool/window contribution without direct ImGui

Product boundary
    PLAYER/server closure excludes Editor/Toolchain facets
```

Second builds after generated/CMake work should be no-op when inputs are unchanged.

---

## 25. Global Plugin/package STOP conditions

STOP for architecture review if implementation appears to require:

```text
IPlugin startup/shutdown god interface
PluginManager / PluginRegistry / ContributionRegistry spanning all owners
PluginContext as a generic service locator
PluginComponentId / PluginSystemId / PluginAbilityId wrappers
one monolithic plugin target linking every layer/product
runtime layer depending on plugin Editor/Toolchain implementation
RenderPlugin duplicating RenderFeature
RenderFeature depending on ECS/System/Editor because they share a package
EditorContext expanded with arbitrary plugin-specific Systems/services
private VFS/AssetManager per plugin
separate Plugin manifest before Product P
binary hot-unload design required merely for source sharing
resurrection of retired dynamic extension architecture without a new binary-plugin requirement
```

---

## 26. Frozen principles

The highest-level rules are:

> **Project Module is the normal gameplay extension unit. A Plugin Package is optional distribution/composition packaging around one or more modules.**

> **Plugin is orthogonal to architecture layers. A package may contain Domain, Render, Scene, Toolchain and Editor targets; each preserves its original owner/dependency/product classification.**

> **RenderFeature is already the Render subsystem extension contract. A Plugin Package may contain a RenderFeature facet, but Plugin packaging does not create a new RenderPlugin semantic layer.**
