# L5 / Runtime Script / Editor 实施路线、Dependency DAG、Architecture Gates 与 Stop Conditions

Status: **Normative Implementation Roadmap — v3 reconciled 2026-09-03**

Reviewed public repository checkpoint: `main@bc15a84252c5740e6e47f3e1094810d6dd4ab711`.

> 本文定义当前唯一实施 DAG。旧版本中把 R0/S0/S1/S2 写成“下一步”的状态说明已过时；这些阶段的设计方向保留，但 implementation status 以本文为准。Coding agent 仍必须同时遵守 `08`；Script work 额外遵守 `11/12/13`；Plugin/package work 额外遵守 `14`；UI work 额外遵守 `10`。

---

## 1. 已关闭 / preserved foundation

以下方向已经进入可继续消费的 foundation，不允许因为后续 feature 需要而重新设计：

```text
R0   committed-source requalification / lifecycle hotfix
B    L2 Execution / TaskScope / Timer / bounded scheduling
V1   AssetVfs read/control split
V2   VFS-backed AssetRead + blocking IO isolation
A    EditorApplication + non-owning EditorContext + Toolset
F    Shared Graph Source
G    GraphEditingSession / GraphRenderProtocol separation
```

仍然持续作为 regression gate：

```text
TaskScope re-entrant admission/close
VFS immutable read snapshots
AssetRead no owner-thread blocking
Graph transaction atomicity / fail-closed rollback
Toolset composition-only mutation + freeze
PLAYER/EDITOR/TOOLCHAIN dependency cleanliness
installed/relocated consumers
```

不要重新恢复：

```text
JobSystem / JobManager
ServiceRegistry / EditorServices
AssetManager replacement / AssetIndex merely for UI
GenericGraphIR / universal graph property bag
old Authoring/session architecture
```

---

## 2. 已完成的 Script 子 DAG

当前 public main checkpoint 已记录到 S4/PB2；S4-P 是进入 S5 前的 portability gate。

已完成方向：

```text
S1
Capability identity + requirements
Provider publication/binding
Continuation / Awaitable / bounded resume foundation
        ↓
S1.5
Ability reflection
receiver/provider binder
explicit CMake codegen
C++ / Lua / FlowForge projection proof
        ↓
S2.0
production Script stable point + SimulationClock
        ↓
S2.1
Delay.nextStep
        ↓
S2.2
Delay.seconds / simulationSeconds
        ↓
S2.3
Delay.realSeconds
        ↓
S2.5
UE-style gameplay Script object lifecycle
        ↓
PB0
runtime baseline
        ↓
S3
FlowForge generated Ability nodes
explicit resumable native state machine
        ↓
S3-H
transitive suspension / BORROWED_STEP hardening
        ↓
PB1
FlowForge end-to-end baseline
        ↓
S4
production Lua authoring + per-instance prepared Ability
Lua coroutine bridge
        ↓
PB2
LuaJIT end-to-end baseline
```

`S2.4 AssetLoad` remains independent/conditional. Do not mark it complete until a script-visible stable/residency-backed Asset handle/value contract is approved.

---

## 3. 当前 Script DAG

```text
S4-P
Portable Lua VM Closure
        ↓
S5.0
Event.await runtime semantics
        ↓
S5.1
FlowForge + portable Lua Event.await projection
        ↓
S5.2
first production domain Ability: Physics
        ↓
S5.3
Navigation when its real domain contract is ready
+ conditional S2.4 AssetLoad closure
        ↓
S5.4 / PB3
realistic gameplay async/event/domain baseline
        ↓
S6
C++ coroutine ergonomics
+ shipping static specialization
        ↓
SCRIPT FRAMEWORK FREEZE
```

There is no pre-authorized S7.

---

## 4. S4-P — Portable Lua VM Closure

Goal:

```text
same packaged LUA_SOURCE artifact
same lifecycle / Ability / coroutine semantics
        ↓
LuaJIT JIT ON
LuaJIT JIT OFF
Lua 5.4
```

MUST:

```text
JIT is performance policy only
Lua VM C-API differences stay behind a narrow private Lua compatibility seam
ScriptSystem never knows VM kind
canonical artifact stores source, never VM bytecode
portable scalar semantics are explicit and lossless across supported VMs
Ability code/source name is separate from display name
same external/project Lua source works in every qualified VM configuration
```

MUST NOT:

```text
LuaJitScriptBackend + Lua54ScriptBackend duplicate architectures
LUAJIT_SOURCE / LUA54_SOURCE artifact kinds
LuaJIT ffi as canonical value representation
worker thread direct lua_resume
VM plugin manager / dynamic VM registry
```

Gate S4-P:

```text
same LXSA bytes execute on the qualified VM configurations
lifecycle and coroutine parity pass
installed consumers pass for LuaJIT and Lua54 prefixes
JIT-off correctness passes
portable numeric/value profile is explicit
```

---

## 5. S5 — Gameplay Async Integration Closure

S5 does not create a new coroutine runtime. It connects the existing one to real event/domain sources.

### S5.0 Event.await runtime

Canonical topology:

```text
EventPoint
    ↓
ScriptSystem EventBucket
    ├─ normal bound handlers
    └─ one-shot coroutine waiters
```

MUST:

```text
waiter is bounded + generational + one-shot
no per-waiter EventPoint.connect/disconnect
event payload crossing dispatch lifetime is copied/marshalled to owned resume storage
event dispatch only marks READY/enqueues; it never directly resumes script
idle waiters are event-driven; no per-frame full waiter scan
callback and await consumption may coexist for one EventPoint
nested dispatch and entity-retirement ordering deterministic/fail-closed
```

### S5.1 language/tool projection

FlowForge:

```text
Event-await node
    -> existing explicit state machine
    -> existing ScriptAwaitable
```

Lua:

```text
portable Lua event await surface
    -> existing Lua coroutine bridge
    -> existing ScriptAwaitable
```

Do not implement C++ `co_await Event` here; S6 owns C++ coroutine ergonomics.

### S5.2 Physics Ability

First real production domain family should be narrow, for example `PhysicsQuery3D`, based only on operations already owned/stable in the Physics package.

Rules:

```text
Ability declaration follows Physics semantic owner
Provider may be JoltPhysicsSystem but Script contract never names Jolt
synchronous domain operation remains QUERY
only genuinely time-spanning operation becomes ASYNC_OPERATION
result/request types are backend-neutral semantic values
no Jolt/Vulkan/third-party layout crosses the Script contract
```

### S5.3 Navigation + conditional AssetLoad

Navigation follows exactly the same owner/provider rules. Do not create a universal Script collection merely to expose a path if no approved value representation exists.

AssetLoad may close only if its return identity/residency contract is approved. Otherwise:

```text
S5 PASS
while
S2.4 AssetLoad remains BLOCKED
```

### S5.4 PB3

Measure real Event/domain behavior, including:

```text
waiter register/cancel/delivery
payload ownership/copy
10k/50k/100k idle waiters
fan-out storm + resume budget
sparse realistic event workload
Physics Ability boundary vs direct domain cost
mixed C++ Static / FlowForge / Lua gameplay scene
```

PB3 is a baseline, not an invented absolute performance gate. Complexity violations such as full waiter scans or unbounded queues are blockers.

Gate S5:

```text
Event.await semantics qualified
FlowForge + Lua reuse existing continuation runtime
first production Physics Ability qualified
Navigation included only when real contract ready
AssetLoad status explicit
PB3 records scaling/complexity
no Script/Event manager/service-locator architecture added
```

---

## 6. S6 — C++ Coroutine Ergonomics + Shipping Specialization

S6 is the final planned Script framework wave.

### S6.0 C++ coroutine frontend

Target user model may expose an ergonomic `co_await` surface, but:

```text
std::coroutine_handle remains private to C++ backend
ScriptBackendContinuation remains Engine contract
ScriptSystem never knows native coroutine representation
BeginPlay/EndPlay remain synchronous
BORROWED_STEP cannot cross co_await
```

### S6.1 C++ Ability ergonomics

The same canonical Ability metadata remains the source of truth. C++ conveniences must not fork semantic schemas from Lua/FlowForge.

### S6.2 Shipping static specialization

When product composition knows:

```text
Ability contract
provider type
selected composition
```

it may generate/static-specialize the call path for LTO/devirtualization/inlining.

Dynamic prepared binding remains the semantic/default contract. Performance strategy must not change `ContractId`, schema, provider ownership, or ScriptArtifact requirements.

Gate S6:

```text
C++ coroutine correctness/lifetime qualified
same ScriptSystem continuation semantics reused
shipping specialization demonstrates semantic equivalence
cross-backend performance comparison recorded
no new generic runtime manager/scheduler
```

After S6: **STOP Script framework expansion** and enter R1.

---

## 7. R1 — Whole-engine Requalification / Framework Gap Review

R1 is not a redesign wave and should not add product features.

Purpose:

```text
re-run current clean profile/install matrix
map all post-S6 architecture owners
confirm Script changes did not contaminate PLAYER/Toolchain/UI boundaries
review unresolved STOP gates
identify only concrete framework gaps exposed by current code
```

R1 MUST answer:

```text
Is Script framework frozen enough to be consumed by Editor/product work?
Are U/C foundations still compatible with current runtime/codegen?
Are there owner/lifetime regressions that must be fixed before UI expansion?
What remains blocked by persistence/Product specs rather than implementation?
```

Gate R1:

```text
clean exact-HEAD qualification
no unknown ownership/lifetime blockers
no need for another speculative runtime framework wave
```

---

## 8. Wave U — Lux UI Foundation

`10-lux-ui-foundation-and-legacy-visual-parity.md` remains authoritative.

### U0/U1

Preserve/complete/qualify only real Editor-required primitives:

```text
modules/function/ui public boundary
Dear ImGui private implementation
ui::Frame / scopes
Theme/design tokens
window/child/menu/popup/toolbar/table/tree
text/button/input/scalar/vector/enum
search/filter helpers
tooltip/focus/hover facts
stable drag/drop payload
edit gesture begin/change/commit
ViewportElement
opaque presentation/texture handle
```

Do not build a retained widget tree, WidgetManager, `IUiBackend`, or one-to-one ImGui wrapper universe.

### U2 NodeCanvas

```text
GraphRenderProtocol
    -> DefaultNodeGraphRenderer
    -> ui::NodeCanvas
    -> private imgui-node-editor / ImGui backend
```

Gate U:

```text
public Lux UI headers compile without ImGui includes
Editor/generated/plugin code has no direct ImGui/node-editor dependency
only private UI backend owns those dependencies
```

---

## 9. Wave C — Generated EntityInspector

C remains the first required Editor vertical slice because it validates UI + codegen + ECS + Selection + undo + AssetId interaction together.

MUST:

```text
generated typed Lux UI bindings
no runtime reflection fallback hot path
bool/integer/float/double/vector/quaternion/enum/AssetId supported as approved
registry.patch<T>() / canonical typed mutation
Parent relation never raw memory write
edit gesture live preview -> one undo commit
unknown editor binding fails visibly/read-only
```

No direct Dear ImGui in generated/editor binding code.

---

## 10. Wave D — AssetBrowser + FileMonitor

MUST remain VFS-first:

```text
AssetVfsView snapshot enumeration
virtual paths/breadcrumb/filter/grid/list
stable AssetId drag payload
thumbnail via Lux opaque presentation handle
FileWatcher -> normalize/debounce/stabilize + project/root generation filtering
Toolset importer/cooker + root TaskScope for async work
```

MUST NOT introduce `AssetIndex` merely to implement v1 UI, a UI-owned worker thread, or per-frame recursive filesystem scan.

---

## 11. Wave E — SceneEditor / Outliner / Viewport

Preserve:

```text
one EditorApplication-owned EditorSelection
EditorSceneHandle{slot,generation} + Entity live identity
flat Outliner as first-class projection
hierarchy only when real hierarchy semantics exist
canonical reparent/detach
Viewport consumes injected render presentation capability
picking updates shared Selection
gizmo uses direct typed Transform mutation/patch
```

No giant `EditorScene`, no RenderRuntime/device ownership in a window, no ImGui texture/ID leakage.

---

## 12. H0 / I0 — In-memory Tool Integration

After U2:

```text
H0 MaterialEditor
I0 FlowForgeEditor
```

Both should consume existing Graph/Toolset/TaskScope/compiler foundations rather than add new framework layers.

Common async pattern:

```text
clone/freeze source snapshot
compiler.compile(snapshot)
root TaskScope owns operation
main/stable completion
validate document/revision
apply diagnostics/preview
```

Window close does not cancel application-owned root work merely because the UI view vanished.

---

## 13. H1 / I1 — Persistence STOP gates

Durable source workflows remain blocked until approved contracts exist.

Material:

```text
MaterialGraph codec + document/source identity
```

FlowForge:

```text
FlowGraph codec + stable ScriptSymbol source identity/package contract
```

UI/compiler work MUST NOT invent persistence formats to bypass these gates.

---

## 14. FC — Engine Framework Closure

FC formalizes the earlier “L5 Foundation/UI Ready” definition and adds Script/tool integration closure.

FC requires:

```text
Runtime
    Execution / VFS / AssetRead stable
    Simulation / Scene / Render foundation stable
    Script S1–S6 frozen

Editor foundation
    EditorApplication ownership stable
    Toolset freeze/lifetime stable
    Lux UI private backend boundary stable
    generated EntityInspector working
    AssetBrowser/FileMonitor working
    SceneEditor/Selection/Viewport working
    NodeCanvas backend isolation working

Tool integration
    H0 Material in-memory edit/compile working
    I0 FlowForge in-memory edit/compile working

Architecture
    PLAYER runtime-clean
    EDITOR/TOOLCHAIN closure clean
    installed/relocated consumers pass
    no known architecture STOP condition outstanding
```

When FC passes:

> **Engine Framework v1 architecture is closed.**

Subsequent work should normally be feature production (animation/audio/world tools/more Abilities/debugger/etc.), not another root architecture rewrite.

---

## 15. Plugin/source-package track — cross-cutting, not a layer

Plugin v1 is source-distributed/build-time-composed packaging around ordinary modules.

It is not a serialized runtime activation framework and not a separate implementation wave that owns all extension points.

A package may export independently classified targets:

```text
domain/runtime
render
scene integration
toolchain
editor
```

Each target uses the owner-specific extension seam:

```text
Simulation -> SimulationSystemRegistration / component/codegen
Script     -> Ability/Event declarations + generated metadata
Render     -> RenderFeature/FeatureFactory/registration
Scene      -> integration/sync binding
Toolchain  -> concrete importer/cooker/compiler target
Editor     -> Tool/window/command/Inspector/graph contribution
```

CMake target graph is the v1 composition truth until Product P freezes a project/package manifest.

See `14-plugin-package-and-extension-composition.md`.

---

## 16. Product Track P

Final shipping product remains project-specific, not a fixed generic Player architecture.

P is STOP until an approved project target-generation specification freezes at least:

```text
selected runtime/domain modules
selected Systems / Scene integrations
selected Render backend/features
project native/generated sources
source Plugin Package dependencies/facets
plugin/static linkage policy
platform/backend selection
cooked/pak inputs
CMake/generator output contract
```

Do not independently invent a Plugin manifest and a Project manifest. Plugin package selection belongs in the same Product composition model.

---

## 17. Qualification matrix

Every cross-layer wave, as applicable:

```text
exact clean source SHA/status
Default Developer
PLAYER/runtime-clean
EDITOR
TOOLCHAIN
Full Render when touched
install + relocated consumers
second build no-work after CMake/codegen changes
architecture dependency/source validators
```

Script additionally:

```text
continuation/lifecycle race stress
late completion/generation invalidation
bounded queue/capacity tests
performance complexity baseline
```

UI additionally:

```text
public UI headers without ImGui include path
Editor/generated/plugin targets without direct ImGui/node-editor
Legacy visual/interaction checklist where relevant
```

Graph additionally:

```text
fault-injection rollback
inverse rollback failure -> poison/fail closed
compile/artifact equivalence
```

Platform support claims require actual target configure/build where practical; copying installed headers is not a platform qualification.

---

## 18. Global STOP conditions

STOP for architecture review if implementation appears to require:

```text
new upward dependency
new global Manager/Registry/Services root
change of EditorApplication/EditorContext ownership
shared graph owner move away from modules/function/graph
new Material/Flow persistence format from UI work
new project/plugin manifest before Product P
ScriptSystem learning backend-native coroutine types
worker/domain direct Script resume
borrowed data crossing await
LuaJIT-specific canonical Script semantics
Dear ImGui/node-editor leaking outside private UI backend
retained-mode widget framework
PluginManager/IPlugin used to bypass owner-specific registration
RenderPlugin duplicating RenderFeature
binary hot-unload architecture merely to support source plugins
```

Hard-cut migrations are preferred while the architecture is still pre-product; do not leave permanent compatibility shims between retired and current designs.
