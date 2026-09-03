# Lux Engine Architecture / Implementation Docset — 2026-09-03 v3 reconciliation

Status: **Normative architecture + current implementation roadmap**

This revision reconciles the original 2026-09-02 v3 design with the runtime scripting work qualified on `main` through the Lua S4/PB2 checkpoint and with the currently approved post-S4 plan.

Reviewed public repository checkpoint for this reconciliation: `main@bc15a84252c5740e6e47f3e1094810d6dd4ab711` (`docs(perf): record final Lua PB2 baseline`). Later implementation may move ahead of this checkpoint; when that happens, coding agents MUST remap physical paths/targets to current repository facts without silently changing the semantic contracts in this docset.

> The v3 direction is preserved: explicit ownership, reusable `modules/`, domain-owned contracts, product-clean dependency boundaries, Lux UI with private Dear ImGui backend, shared graph source/edit protocol, project-specific final products, backend-neutral script continuations, and no global service-locator architecture.

---

## 1. Current convergence

The following foundation directions are treated as **closed/preserved**, not as work to redesign:

```text
R0  foundation requalification / lifecycle hotfix
B   L2 Execution / TaskScope / Timer / bounded async mechanisms
V1  AssetVfs control/read split
V2  VFS-backed AssetRead + blocking IO isolation
A   EditorApplication + non-owning EditorContext + Toolset
F   Shared Graph Source
G   Graph editing/render protocol separation
```

The Script runtime has advanced substantially beyond the original v3 checkpoint:

```text
S1 / S1.5  capability + continuation foundation, Ability reflection/codegen
S2.0–S2.3 stable point / NextStep / Simulation Delay / Real Delay
S2.5       gameplay Script object lifecycle
PB0        Script runtime baseline
S3         FlowForge Ability nodes + explicit state-machine coroutine lowering
S3-H       transitive suspension / borrowed-lifetime hardening
PB1        FlowForge end-to-end baseline
S4         production Lua object + prepared Ability + coroutine bridge
PB2        LuaJIT end-to-end baseline
```

`S2.4 AssetLoad` is **not** considered complete merely because the coroutine bridge exists. It remains conditional on an approved script-visible stable/residency-backed Asset handle/value contract.

The portability closure immediately before S5 is:

```text
S4-P  portable Lua VM closure
      same LUA_SOURCE artifact semantics
      LuaJIT JIT-on / LuaJIT interpreter / Lua 5.4 parity
      JIT is an optimization, never a script semantic
```

The approved remaining Script roadmap is:

```text
S4-P
  ↓
S5   Event.await + first real domain Abilities (Physics, then Navigation when ready)
     + conditional S2.4 AssetLoad closure
  ↓
PB3  realistic gameplay async/event/domain baseline
  ↓
S6   C++ coroutine ergonomics + shipping static specialization
  ↓
Script framework FREEZE
```

There is intentionally **no planned S7**. New Script/runtime work after S6 must be justified by a real product/editor/gameplay consumer.

Product storage has also gained a frozen logical direction before Product Track P:

```text
PAK-0
    LuxPak cooker/reader prototype + index/chunk/update benchmark
        ↓
PAK-1
    immutable Base/Patch LuxPak shipping storage closure
        ↓
P
    project-specific target/product manifest + package selection closure
```

`PAK-0/PAK-1` do not create a new architecture layer. They define the shipping storage/provider contract consumed by the existing Product/VFS/AssetRead model.

---

## 2. Post-S6 mainline

After S6 the main architecture track returns to Editor/UI and framework closure rather than continuing to expand Script infrastructure:

```text
S6
 ↓
R1  whole-engine requalification / framework-gap review
 ↓
U0/U1 Lux UI core + required Editor primitives qualification/completion
 ↓
C   generated EntityInspector vertical slice
 ↓
D + E
AssetBrowser/FileMonitor + SceneEditor/Outliner/Viewport
 ↓
U2  NodeCanvas / imgui-node-editor private backend isolation
 ↓
H0 + I0
MaterialEditor + FlowForgeEditor in-memory edit/compile integration
 ↓
FC  Engine Framework Closure
 ↓
H1 + I1
approved durable source/document/package workflows only
 ↓
P   project-specific target/product closure after project manifest spec
```

`D`, `E`, and the later part of `U2` may proceed in parallel where their dependencies allow. The roadmap is a dependency DAG, not a requirement that every commit be serialized.

`PAK-0` may proceed as an independent Product/VFS storage experiment once current runtime priorities permit; `PAK-1` must be closed before final shipping Product P can claim incremental cooked-content packaging. It must not block unrelated Editor/UI framework closure.

`FC` is the point at which Lux may call the v1 engine framework architecture closed. Feature production after FC should normally consume the existing architecture rather than reopen root ownership/runtime boundaries.

---

## 3. Project modules, Plugin Packages, and extension points

Normal game development does **not** require a Plugin abstraction.

A project may directly define:

```text
Components / data contracts
Simulation Systems / Providers
Script Abilities / Events
Scene integration
render-facing data
project code
```

A **Plugin Package** is an optional source-distribution/build-composition package around one or more such modules. Plugin is not a new architecture layer and does not replace Component/System/Ability/RenderFeature semantics.

A package may contain multiple independently classified facets/targets:

```text
Domain/runtime facet      Components, System/Provider, Ability/Event
Render facet              RenderFeature + render resources
Scene/integration facet   bridges Domain facts to Render/Scene composition
Toolchain facet           importer/cooker/compiler/codegen integration
Editor facet              tools/windows/commands/Inspector/graph/UI integration
```

Every facet keeps its original owner and dependency rules.

Important distinctions:

```text
Project Module   = normal game/engine extension unit
Plugin Package   = optional source-distribution/composition packaging
Editor Extension = Editor-only facet of a package
RenderFeature    = Render subsystem's own low-level extension contract
```

`RenderFeature` is **not** a new `RenderPlugin` category. A Plugin Package may carry a RenderFeature target, but packaging must not move RenderFeature upward or allow it to depend on Simulation/System/Editor semantics.

Source-shared/build-time-composed plugins are the v1 target. Binary ABI/hot-load/hot-unload plugins are a separate future architecture problem and MUST NOT distort the source-plugin model.

Until Product Track P freezes a project/package manifest, the CMake target graph is the source-plugin composition truth. Do not invent `plugin.toml`, `luxplugin.json`, a universal PluginManager, or a second package manifest.

See `14-plugin-package-and-extension-composition.md`.

---

## 4. Normative priority

When documents conflict, use this order:

1. **Current repository canonical topology facts**, unless an approved architecture document explicitly changes the semantic owner/contract.
2. `13-script-gameplay-object-lifecycle.md` for ScriptInstance incarnation, BeginPlay/EndPlay, retirement/materialization semantics.
3. `11-script-api-capabilities-coroutines-and-await.md` for Script Ability/capability/provider/coroutine/await/Event/Delay/backend portability semantics.
4. `12-script-ability-reflection-provider-binding-and-codegen.md` for Ability declaration, identity/naming, receiver/provider binding, CMake codegen and language/tool projections.
5. `14-plugin-package-and-extension-composition.md` for Project Module vs Plugin Package, multi-facet source-plugin composition and RenderFeature/Editor extension rules.
6. `15-luxpak-indexed-content-container-and-incremental-update.md` for shipping Pak storage, persisted AssetId/VFS indexes, ContentEntry→Segment→Chunk, immutable Patch Pak and incremental-update semantics.
7. `08-normative-execution-contract.md` for general ownership/execution/lifetime MUST/MUST NOT rules not superseded above.
8. `07-implementation-roadmap-and-gates.md` for the **current implementation order/gates**.
9. `10-lux-ui-foundation-and-legacy-visual-parity.md` for Lux UI/private backend/Legacy visual-parity rules.
10. The remaining functional design documents.

`09-product-runtime-vfs-and-async-script.md` remains normative for Product/VFS/AssetRead ownership but is **historical context** for old Script sequencing; detailed Script semantics and sequencing are superseded by `11/12/13` + `07`. LuxPak/Patch storage details are superseded by `15`.

`01-editor-context-toolset-and-plugins.md` remains normative for EditorApplication/EditorContext/Toolset and Editor contribution lifetime. Its older general “plugin” wording is interpreted through `14`; it does not define an all-engine plugin framework.

---

## 5. Reading order

For general architecture work:

1. `README.md`
2. `00-L5-architecture-overview.md`
3. `08-normative-execution-contract.md`
4. `07-implementation-roadmap-and-gates.md`
5. current-wave functional documents

For Script work additionally read:

6. `11-script-api-capabilities-coroutines-and-await.md`
7. `12-script-ability-reflection-provider-binding-and-codegen.md`
8. `13-script-gameplay-object-lifecycle.md`

For Plugin/package/extension work read:

9. `14-plugin-package-and-extension-composition.md`
10. the owner-specific contract (Simulation, Render, Toolchain, Editor, etc.)

For Product/VFS/Pak/update work read:

11. `03-asset-vfs-filewatch.md`
12. `09-product-runtime-vfs-and-async-script.md`
13. `15-luxpak-indexed-content-container-and-incremental-update.md`

For UI/Editor work read:

14. `10-lux-ui-foundation-and-legacy-visual-parity.md`
15. `01`–`05` as relevant

File numbering is reading organization, **not implementation order**.

---

## 6. Documents

- `00-L5-architecture-overview.md` — current L0–L5/Product overview, framework state and closure direction.
- `01-editor-context-toolset-and-plugins.md` — EditorApplication ownership, non-owning EditorContext, Toolset, Editor contribution lifetime.
- `02-entity-inspector-codegen-ui.md` — generated typed Inspector hot path targeting Lux UI.
- `03-asset-vfs-filewatch.md` — product-wide VFS, AssetVfsView, AssetBrowser/FileWatch and asset-facing Editor integration.
- `04-scene-editor-outliner-viewport.md` — SceneEditor/Outliner/Viewport and optional hierarchy projection.
- `05-shared-graph-source-and-graphkit.md` — shared GraphTopology/GraphLayout and graph editing/render protocol.
- `06-toolchain-process-async-execution.md` — L2 execution, TaskScope, compiler Sender model and AssetRead mechanisms.
- `07-implementation-roadmap-and-gates.md` — **current unique execution DAG**, Script S4-P→S6, post-S6 Editor return, R1/FC and Product storage/P gates.
- `08-normative-execution-contract.md` — general ownership/execution MUST/MUST NOT/STOP contract.
- `09-product-runtime-vfs-and-async-script.md` — Product/VFS/AssetRead normative design; old Script sequencing retained only as historical context.
- `10-lux-ui-foundation-and-legacy-visual-parity.md` — Lux UI public/private boundary, object/immediate model, Theme and backend isolation.
- `11-script-api-capabilities-coroutines-and-await.md` — current Script capability/coroutine/Event/Delay/portable-backend runtime contract.
- `12-script-ability-reflection-provider-binding-and-codegen.md` — current Ability reflection/binding/naming/codegen/projection contract.
- `13-script-gameplay-object-lifecycle.md` — Script object incarnation, BeginPlay/EndPlay and materialization/retirement contract.
- `14-plugin-package-and-extension-composition.md` — source-plugin packaging, multi-facet target composition, RenderFeature and Editor extension boundaries.
- `15-luxpak-indexed-content-container-and-incremental-update.md` — immutable LuxPak logical layout, persisted indexes, chunk storage, patch/update and PAK-0/PAK-1 gates.

---

## 7. Global architecture invariants

The following remain non-negotiable:

```text
no global EngineContext / ServiceRegistry / generic Manager root
no hidden ownership through shared_ptr merely to survive bad teardown
no lower layer depending on Editor/Toolchain
no modules/ pollution with Engine-specific Scene/System ontology
no per-call Script contract/provider string lookup
no worker/domain callback directly resuming Script
no native stack retained across Script suspension
no borrowed step-local value across await
no Dear ImGui/node-editor types outside the private Lux UI backend
no retained WidgetManager/framework invented over immediate-mode leaves
no Material/Flow persistence format invented from UI work
no project/plugin manifest invented before Product Track P
no PluginManager/IPlugin used to bypass normal owner-specific registration
no RenderPlugin layer duplicating RenderFeature
no runtime Pak payload scan or runtime rebuild of Asset/VFS package indexes
no in-place Base Pak mutation for normal shipping updates
no encryption/DRM framework introduced before a separately approved security requirement
```

Legacy remains a visual/interaction/behavior reference only, never an ownership or architecture source.

---

## 8. Evidence vs normative docs

Exact benchmark numbers, machine-specific timings, CTest totals and qualification SHAs belong in repository evidence such as:

```text
.internal/*qualification-evidence*.md
.internal/*performance-baseline*.md
```

Normative documents define semantic contracts, complexity invariants and qualification requirements. They MUST NOT turn one machine's PB0/PB1/PB2/PAK-0 timing into a universal architecture threshold.
