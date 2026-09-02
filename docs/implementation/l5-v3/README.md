# Lux Engine Architecture / Implementation Docset — 2026-09-02 v3

Status: **Normative architecture + current implementation roadmap**

This revision supersedes `lux-engine-architecture-implementation-docset-2026-09-02 v2`.

Reviewed repository checkpoint for this revision: `main@2caaa6f7b35ad759d32e9f9763a67dcb559b8860` (`test(ui): match snapshot linkage contract`).

> v3 keeps the ownership, VFS, execution, graph, product and Lux UI decisions, and now freezes the runtime scripting capability/coroutine model plus Script Ability reflection/provider binding/codegen. Dear ImGui remains private to `modules/function/ui`. `modules/` remains Engine-independent/reusable; Engine Script Ability declarations stay with their real Engine/domain owners.

## Major convergence changes

1. **Current foundation is preserved, not redesigned.** B/V1/V2/A/F/G/R0 are treated as the implemented foundation direction.
2. **Lux UI remains an explicit implementation wave.** `modules/function/ui` is the public UI boundary; Editor, generated bindings and plugins consume Lux UI, not Dear ImGui.
3. **Dear ImGui is a private backend detail.** Public Lux headers, L5 Editor packages, generated UI code and plugin SDK surfaces MUST NOT expose ImGui/imgui-node-editor headers or types.
4. **UI remains immediate-mode internally.** Long-lived semantic objects such as `Pane`/`UISession` are object-oriented; frame/scope objects are ephemeral; leaf widgets remain immediate-mode functions.
5. **Legacy is a visual/interaction reference, not an architectural source of truth.** Lux preserves selected workflows/panel composition/visual language while replacing Legacy ownership and monolithic Editor structure.
6. **Generated Inspector UI targets Lux UI.** First-party and plugin UI codegen must not expose private ImGui types.
7. **Graph rendering remains backend-isolated.** `GraphEditingSession`/`GraphRenderProtocol` remain canonical; default graph rendering consumes Lux UI NodeCanvas.
8. **Product/runtime decisions remain unchanged.** Final game output is project-specific; `PLAYER` is only a runtime-clean qualification profile; VFS remains explicit/product-wide.
9. **Persistence and Product STOP gates remain.** Material/Flow durable source persistence and project target-generation inputs still require dedicated approved contracts.
10. **Script API is a callable contract; provider is a runtime capability implementation.** Scripts depend on stable contracts such as `PhysicsQuery3D`, not concrete providers such as Jolt.
11. **System is a common provider form, not the only provider form.** Entity/ECS or AssetLoading abilities may be provided by appropriate runtime/integration objects rather than fake Systems.
12. **`modules/` boundary is preserved.** Engine/Scene/Simulation/System capability ontology MUST NOT be moved into `modules/function/script` merely to centralize scripting. `modules/` only owns truly reusable Engine-independent script/ABI/codegen primitives.
13. **Ability declaration follows semantic owner.** A physics ability is declared with the physics Engine package; ECS abilities with Simulation ECS owner; external projects may declare their own abilities in their own Engine/project packages.
14. **Reflection is static; provider binding is runtime.** Codegen emits canonical Ability metadata plus typed binder/thunks. It does not construct/own provider objects.
15. **Provider objects retain their existing owner.** Simulation/composition continues to own installed Systems; generated ability bindings only borrow already-owned provider instances.
16. **Receiver v1 is intentionally small.** `NONE` or `PROVIDER_INSTANCE`. Receiver is binding metadata, not a script-visible parameter.
17. **Default provider is unique in v1.** Multiple providers for one default contract fail with `SCRIPT_CAPABILITY_AMBIGUOUS_PROVIDER`; no registration-order selection or string service lookup.
18. **Ability codegen is explicit CMake opt-in.** Selected sources/types are reflected; generated files live in the build generated tree; domain targets do not depend directly on Lua/FlowForge/Python implementations.
19. **Codegen is two-stage.** Owner-side reflection produces canonical language-neutral Ability metadata + binder; language/tool projections consume it for C++, Lua, FlowForge and future Python.
20. **Coroutine/await is the canonical cross-frame script control-flow model.** FlowForge uses explicit state machine; language-native coroutine mechanisms stay backend-private behind one continuation contract.
21. **Event + await coexist.** Hook/Event remain engine-to-script contracts; `Event.next(...)` may be awaited without turning EventPoint into a per-coroutine connection framework.
22. **Delay semantics are split.** `Delay.seconds()` means Simulation time; `Delay.realSeconds()` uses monotonic real time; all resume occurs only at explicit Simulation stable points.
23. **Borrowed data cannot cross suspension.** Ability/schema projection distinguishes `OWNED_VALUE`, `STABLE_ID`, `BORROWED_STEP`, `AWAITABLE`; `BORROWED_STEP` values are invalid across await.
24. **Script API contract does not mandate dispatch.** Dynamic boundaries may use prepared receiver + function table; project-specific C++/FlowForge shipping paths may statically specialize and inline known providers.

## Terminology

- **DAG** — Directed Acyclic Graph，有向无环依赖图。
- **ABI** — Application Binary Interface，应用二进制接口。
- **SDK** — Software Development Kit，软件开发工具包/公开插件开发接口集合。
- **UB** — Undefined Behavior，C++ 未定义行为。
- **RAII** — Resource Acquisition Is Initialization，资源获取即初始化。
- **VFS** — Virtual File System，虚拟文件系统。
- **Ability** — reflected callable Script API contract declared with its semantic owner.
- **Provider** — existing runtime instance that implements an Ability contract.
- **Receiver** — hidden binding target (`NONE` or `PROVIDER_INSTANCE` in v1), not a script-visible parameter.

## Current implementation checkpoint

```text
Closed/preserved foundation:
    R0  Foundation requalification/hotfix
    B   L2 Execution
    V1  AssetVfs read/control split
    V2  VFS-backed AssetRead / blocking IO isolation
    A   EditorApplication + non-owning EditorContext + Toolset
    F   Shared Graph Source
    G   Graph editing/render protocol split

Editor/UI implementation now present:
    U0/U1 direction  Lux UI backend isolation + real windowed UI presentation
    C direction      generated EntityInspector mounted as a real visible slice

Immediate Editor gate:
    qualify/close the current visible U/C checkpoint before broadening Editor feature scope

Runtime scripting design:
    S0 design        FROZEN by `11` + `12`

Runtime scripting implementation sub-DAG:
    S1   capability identity/requirements/provider publication + continuation/awaitable/resume foundation
      ↓
    S1.5 Ability reflection + receiver/provider binder + explicit CMake codegen + projection proof
      ↓
    S2   NextStep / Simulation Delay / Real Delay / AssetLoad
      ↓
    S3   FlowForge generated API nodes + coroutine lowering
      ↓
    S4   Lua generated binding + coroutine bridge
      ↓
    S5   Event.await + real Physics/Navigation/etc. abilities
      ↓
    S6   C++ coroutine ergonomics + shipping static specialization

Parallelism:
    after visible U/C qualification, D/E/U2 Editor work and S1 scripting may proceed in parallel
    S1.5 depends on S1 qualification; it does not wait for D/E/U2

Product:
    P project target generation remains STOP until manifest/target spec approval
```

## Reading order

1. `00-L5-architecture-overview.md`
2. `08-normative-execution-contract.md`
3. `07-implementation-roadmap-and-gates.md`
4. `10-lux-ui-foundation-and-legacy-visual-parity.md` for Editor UI work
5. Current-wave functional document(s)
6. `09-product-runtime-vfs-and-async-script.md` for product composition, VFS, asset IO and historical async integration context
7. `11-script-api-capabilities-coroutines-and-await.md` for any runtime Script API/capability/coroutine/await work
8. `12-script-ability-reflection-provider-binding-and-codegen.md` for Ability declaration, receiver/provider binding, CMake codegen or language projection work

File numbering is reading organization, **not implementation order**.

## Documents

- `00-L5-architecture-overview.md` — L0–L5, current foundation checkpoint, EditorApplication/Context, UI/codegen, VFS, graph and async overview.
- `01-editor-context-toolset-and-plugins.md` — exact EditorApplication ownership, Toolset/application lifecycle, non-owning EditorContext, plugin contribution lifetime.
- `02-entity-inspector-codegen-ui.md` — generated typed Inspector hot path targeting Lux UI.
- `03-asset-vfs-filewatch.md` — product-wide VFS, AssetVfsView, async asset-loading seam, AssetBrowser/FileWatch.
- `04-scene-editor-outliner-viewport.md` — SceneEditor/Outliner/Viewport and optional hierarchy projection.
- `05-shared-graph-source-and-graphkit.md` — shared GraphTopology/GraphLayout and graph editing/render protocol, with Lux UI NodeCanvas backend isolation.
- `06-toolchain-process-async-execution.md` — L2 execution, re-entrant-safe TaskScope contract, compiler Sender model, AssetRead endpoint and runtime async mechanisms.
- `07-implementation-roadmap-and-gates.md` — general implementation DAG and gates; its older scripting sub-DAG is superseded where `11/12` explicitly differ.
- `08-normative-execution-contract.md` — highest-priority general MUST/MUST NOT/STOP contract for coding agents except where later approved `11/12` explicitly supersede scripting clauses.
- `09-product-runtime-vfs-and-async-script.md` — generated product target model, product-wide capabilities, VFS ownership/concurrency and historical async-script integration context; conflicting Delay/Ability details are superseded by `11/12`.
- `10-lux-ui-foundation-and-legacy-visual-parity.md` — Lux UI public/private boundary, object/immediate-mode model, Theme, Legacy visual parity, plugin/codegen and graph UI rules.
- `11-script-api-capabilities-coroutines-and-await.md` — runtime Script Ability/capability contracts, provider lifetime/binding, mount requirements, backend-neutral continuation/awaitable model, Event.await, Delay semantics and performance policy.
- `12-script-ability-reflection-provider-binding-and-codegen.md` — S1.5 exact contract for owner-local Ability declarations, receiver semantics, CMake reflection/codegen, generated binder/thunks, provider ownership and C++/Lua/FlowForge projection.

## Normative priority

1. Current repository canonical topology facts that have not been explicitly superseded by an approved architecture decision.
2. For Script Ability/capability/provider/coroutine/await semantics: `11-script-api-capabilities-coroutines-and-await.md`.
3. For Ability reflection/receiver/provider binding/CMake codegen/language projection: `12-script-ability-reflection-provider-binding-and-codegen.md`.
4. `08-normative-execution-contract.md` for general ownership/execution MUST/MUST NOT rules not superseded by 2–3.
5. `07-implementation-roadmap-and-gates.md`, except its scripting sub-DAG where 2–3 are newer.
6. `10-lux-ui-foundation-and-legacy-visual-parity.md` for UI/backend questions.
7. Other functional design documents.

If implementation requires changing an owner, introducing a global singleton/service locator, exposing ImGui outside the private UI backend, creating a retained-mode widget framework, putting Engine capability ontology into `modules/`, making generated code own runtime providers, adding a new architecture layer, or resolving a STOP condition, the coding agent MUST stop for architecture review rather than improvise.
