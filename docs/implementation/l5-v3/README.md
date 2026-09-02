# Lux Engine Architecture / Implementation Docset — 2026-09-02 v3

Status: **Normative architecture + current implementation roadmap**

This revision supersedes `lux-engine-architecture-implementation-docset-2026-09-02 v2`.

Reviewed repository checkpoint for this revision: `main@2caaa6f7b35ad759d32e9f9763a67dcb559b8860` (`test(ui): match snapshot linkage contract`).

> v3 keeps the ownership, VFS, execution, graph, product and Lux UI decisions, and now also freezes the runtime scripting capability/coroutine model. Dear ImGui remains private to `modules/function/ui`; Script API contracts remain independent of concrete providers such as Jolt/other physics backends.

## Major convergence changes

1. **Current foundation is preserved, not redesigned.** B/V1/V2/A/F/G are treated as the implemented foundation direction. The next work starts with a Foundation Requalification gate rather than re-running architecture design from zero.
2. **Clean-checkout reproducibility is now a gate.** The reviewed HEAD references `engine/editor/application/test/editor_application_test.cpp` from CMake while that file is not tracked in the reviewed Git tree. The exact committed HEAD therefore cannot be treated as qualified until the missing test source is recovered/committed and qualification is rerun from a clean checkout.
3. **`TaskScope` gains a re-entrancy requirement.** Eager Sender start/spawn and stop callbacks MUST NOT execute while the scope holds its own admission/state mutex. Close MUST account for admitted-but-not-yet-registered starts.
4. **`EditorApplication::installTool` gains an application-lifecycle contract.** COMPOSING may install; RUNNING is frozen; STOPPING/JOINED must fail explicitly and must never dereference a disengaged Toolset owner.
5. **Lux UI becomes an explicit implementation wave.** `modules/function/ui` is the public UI boundary; Editor, generated bindings and plugins consume Lux UI, not Dear ImGui.
6. **Dear ImGui is a private backend detail.** Public Lux headers, L5 Editor packages, generated UI code and plugin SDK surfaces MUST NOT expose ImGui/imgui-node-editor headers or types.
7. **UI remains immediate-mode internally.** Long-lived semantic objects such as `Pane`/`UISession` are object-oriented; frame/scope objects are ephemeral; leaf widgets remain immediate-mode functions. v3 explicitly rejects rebuilding a retained-mode widget tree on top of ImGui.
8. **Legacy is a visual/interaction reference, not an architectural source of truth.** Lux should preserve selected Legacy workflows, panel composition and visual language while replacing Legacy ownership, manager webs, raw ImGui call structure and monolithic Editor objects.
9. **Generated Inspector UI now targets Lux UI.** The previous phrase “typed ImGui binding” is replaced by “typed Lux UI binding”; the private backend may map those calls to ImGui scalar/widget APIs.
10. **Graph rendering is backend-isolated.** `GraphEditingSession` and `GraphRenderProtocol` remain as implemented; the default renderer consumes `ui::NodeCanvas`, while imgui-node-editor is hidden inside the UI backend.
11. **Product/runtime decisions remain unchanged.** Final game output is project-specific; `PLAYER` is only a runtime-clean qualification profile; VFS remains explicit/product-wide; async script work still requires explicit continuation/suspension.
12. **Persistence and Product STOP gates remain.** Material/Flow durable source persistence, Flow stable ScriptSymbol identity and project target-generation input formats must be approved before those features are completed.
13. **Script API is now a capability contract.** Scripts depend on stable callable contracts such as `PhysicsQuery3D`; Scene/System/integration providers implement those contracts. Missing/incompatible capabilities fail script mount early.
14. **Coroutine/await is the canonical cross-frame script control-flow model.** FlowForge uses an explicit state machine; Lua/C++/future Python may use native language coroutine mechanisms behind one backend-neutral continuation/awaitable contract.
15. **Event + await coexist.** Hook/Event remain engine-to-script contracts; `Event.next(...)` may be awaited without turning EventPoint into a per-coroutine connection framework.
16. **Delay semantics are split.** `Delay.seconds()` means Simulation time; `Delay.realSeconds()` uses monotonic real time; all script resume still occurs only at an explicit Simulation stable point.
17. **Script API contract does not mandate dispatch.** Dynamic `context + function pointer` binding is allowed at dynamic boundaries; project-specific C++/FlowForge shipping paths may use generated/static specialization and Link Time Optimization for hot calls.

## Terminology

- **DAG** — Directed Acyclic Graph，有向无环依赖图；本文档集用它表示唯一 implementation dependency graph。
- **ABI** — Application Binary Interface，应用二进制接口。
- **SDK** — Software Development Kit，软件开发工具包/公开插件开发接口集合。
- **UB** — Undefined Behavior，C++ 未定义行为。
- **RAII** — Resource Acquisition Is Initialization，资源获取即初始化；UI 中主要用于短生命周期 scope guard，而不是持久 widget tree。
- **VFS** — Virtual File System，虚拟文件系统。

## Current implementation checkpoint

The reviewed repository has moved beyond the original foundation-only checkpoint：

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

Runtime scripting:
    S0 design        FROZEN by `11-script-api-capabilities-coroutines-and-await.md`
    S1+              may begin after the current U/C checkpoint qualification; it does not wait for D/E/U2 completion

Later Editor/product work:
    D/E              AssetBrowser + SceneEditor can proceed in parallel with scripting after their gates
    H/I              MaterialEditor / FlowForgeEditor after UI + persistence prerequisites
    P                project target generation remains STOP until manifest/target spec approval
```

## Reading order

1. `00-L5-architecture-overview.md`
2. `08-normative-execution-contract.md`
3. `07-implementation-roadmap-and-gates.md`
4. `10-lux-ui-foundation-and-legacy-visual-parity.md` for any Editor UI work
5. Current-wave functional document(s)
6. `09-product-runtime-vfs-and-async-script.md` whenever work touches product composition, VFS ownership, asset IO or script/cross-frame async behavior
7. `11-script-api-capabilities-coroutines-and-await.md` for any Script API, capability binding, coroutine/await, Event.await or async script implementation

File numbering is reading organization, **not implementation order**.

## Documents

- `00-L5-architecture-overview.md` — L0–L5, current foundation checkpoint, EditorApplication/Context, UI/codegen, VFS, graph and async overview.
- `01-editor-context-toolset-and-plugins.md` — exact EditorApplication ownership, Toolset/application lifecycle, non-owning EditorContext, plugin contribution lifetime.
- `02-entity-inspector-codegen-ui.md` — generated typed Inspector hot path targeting Lux UI.
- `03-asset-vfs-filewatch.md` — product-wide VFS, AssetVfsView, async asset-loading seam, AssetBrowser/FileWatch.
- `04-scene-editor-outliner-viewport.md` — SceneEditor/Outliner/Viewport and optional hierarchy projection.
- `05-shared-graph-source-and-graphkit.md` — shared GraphTopology/GraphLayout and graph editing/render protocol, with Lux UI NodeCanvas backend isolation.
- `06-toolchain-process-async-execution.md` — L2 execution, re-entrant-safe TaskScope contract, compiler Sender model, AssetRead endpoint and runtime async mechanisms.
- `07-implementation-roadmap-and-gates.md` — sole implementation DAG, current repository checkpoint and gates.
- `08-normative-execution-contract.md` — highest-priority MUST/MUST NOT/STOP contract for coding agents.
- `09-product-runtime-vfs-and-async-script.md` — generated product target model, product-wide capabilities, VFS ownership/concurrency and async script/cross-frame contract.
- `10-lux-ui-foundation-and-legacy-visual-parity.md` — Lux UI public/private boundary, object/immediate-mode model, Theme, Legacy visual parity, plugin/codegen and graph UI rules.
- `11-script-api-capabilities-coroutines-and-await.md` — Script API capability contracts, mount requirements, backend-neutral continuation/awaitable model, Event.await, Simulation/real-time Delay semantics, language projections and static/dynamic binding performance policy.

## Normative priority

1. Current repository canonical L0–L4/L5 topology facts that have not been explicitly superseded by an approved architecture decision.
2. For Script API capability / coroutine / await / Event.await / Delay-time semantics: `11-script-api-capabilities-coroutines-and-await.md`. It explicitly supersedes conflicting older scripting clauses in `06`/`07`/`08`/`09`.
3. `08-normative-execution-contract.md` for general ownership/execution MUST/MUST NOT rules not superseded by item 2.
4. `07-implementation-roadmap-and-gates.md`.
5. `10-lux-ui-foundation-and-legacy-visual-parity.md` for UI/backend questions.
6. Functional design documents.

If implementation requires changing an owner, introducing a global singleton/service locator, exposing ImGui outside the private UI backend, creating a retained-mode widget framework, adding a new architecture layer, or resolving a STOP condition, the coding agent MUST stop for architecture review rather than improvise.
