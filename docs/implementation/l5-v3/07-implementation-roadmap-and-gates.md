# L5 / Runtime Script / Editor / Product Storage 实施路线、Dependency DAG、Architecture Gates 与 Stop Conditions

Status: **Normative Implementation Roadmap — Script framework frozen 2026-09-04**

Qualified implementation checkpoint: production `718425883a695c26008fa600ae196b60d8738644`.

> 本文定义当前唯一实施 DAG。旧版本中把 R0/S0/S1/S2 写成“下一步”的状态说明已过时；这些阶段的设计方向保留，但 implementation status 以本文为准。Coding agent 仍必须同时遵守 `08`；Script work 额外遵守 `11/12/13`；Plugin/package work 额外遵守 `14`；Pak/Product storage work 额外遵守 `15`；UI work 额外遵守 `10`。

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

当前 implementation 已完成 S1–S6：S5 CLOSED/PASS，S6 COMPLETE/PASS，Script framework FROZEN。
本节保留阶段定义和 gates 作为已执行 contract；下一步仅标记为 R1，不在本轮启动。

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

AssetLoad may close only if its return identity/residency contract is approved. LuxPak/Chunk identities do not substitute for this Script residency/value contract.

Otherwise:

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
What remains blocked by persistence/Product/Pak specs rather than implementation?
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

PakProvider's persisted AssetId hash/path radix index is an authoritative provider storage index defined by `15`; it is not the forbidden Editor AssetIndex/Catalog.

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

PAK-0/PAK-1 are Product storage closure gates and need not block FC if no Editor/runtime framework ownership gap remains; final shipping Product P still depends on PAK-1.

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

Plugin runtime assets use the same LuxPak/VFS storage path when selected into a product; no PluginPak/VFS is created.

CMake target graph is the v1 composition truth until Product P freezes a project/package manifest.

See `14-plugin-package-and-extension-composition.md`.

---

## 16. PAK-0 — LuxPak Format Prototype / Benchmark Gate

PAK-0 is a Product/VFS/Toolchain storage experiment, not a new runtime architecture layer.

Normative logical design is in `15`:

```text
AssetId -> persisted flat hash -> ContentEntry
canonical VFS path -> persisted compact prefix index -> ContentEntry
ContentEntry -> Segment -> Chunk
payload first + read-ready TOC + fixed footer logically
immutable package model
```

PAK-0 must prototype and measure rather than guess the remaining physical parameters.

MUST benchmark/freeze at least:

```text
AssetId hash slot/probe/hash/load-factor policy
compact radix vs double-array/static-trie VFS layout
canonical path exact/miss/prefix enumeration
ContentHash algorithm/width
embedded vs sidecar TOC
chunk-size policies
compression policy
TOC/mount memory and latency
random read / sequential locality
small Base+Patch overlay
content-hash update diff
```

MUST use large synthetic entry counts plus representative cooked assets.

MUST NOT implement encryption/DRM during this wave.

Gate PAK-0:

```text
no runtime payload scan
no runtime index rebuild
cross-platform deterministic/readable wire prototype
persisted AssetId and VFS prefix indexes qualified
Asset -> Segment -> Chunk read path works
patch/tombstone model validated
performance/complexity evidence recorded
exact physical parameters selected for PAK-1
```

---

## 17. PAK-1 — Shipping Pak / Incremental Update Closure

PAK-1 turns the qualified prototype into the shipping cooked-content provider/update contract.

MUST close:

```text
canonical VFS path byte contract
fixed v1 wire structs/validation
PakProvider + AssetRead async integration
immutable Base/Patch containers
ADD / REPLACE / TOMBSTONE
strong ContentHash diff
Release Manifest/update inventory relation
atomic patch install/publication/rollback policy
bounded patch chain + rebase/compaction policy
path rename without unchanged payload retransmission
```

Minimum v1 client may download a complete Patch Pak containing only changed/new content.

Later chunk-aware CDN distribution may download only missing strong-hash chunks and materialize a self-contained Patch Pak, but normal `loadAsset()` must remain an installed-storage operation rather than implicit network I/O.

Encryption remains NOT DONE unless a separate security requirement approves it. The format may reserve `SecurityMode::NONE`/future flags and must fail closed on unsupported modes.

Gate PAK-1:

```text
shipping PakProvider qualified
incremental update avoids full Base replacement
patch tombstones prevent fallback resurrection
bounded overlay lookup/rebase policy qualified
no runtime index reconstruction
release/update tooling reproducible from exact source/cooked inputs
```

---

## 18. Product Track P

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
cooked LuxPak/container inputs
Base/Patch/release-variant selection
host-tool requirements
CMake/generator output contract
```

Do not independently invent a Plugin manifest and a Project manifest. Plugin package selection belongs in the same Product composition model.

PAK-1 owns **how cooked content is stored and incrementally updated**. Product P owns **which selected project/plugin content belongs to which product/container/release variant**.

---

## 19. Qualification matrix

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

Pak additionally:

```text
large TOC/index stress
malformed/corrupt TOC bounds validation
exact/miss/prefix path workloads
random/sequential/chunk amplification benchmark
Base/Patch/tombstone overlay
incremental release diff
cross-platform wire reproducibility
no runtime index rebuild
```

Platform support claims require actual target configure/build where practical; copying installed headers is not a platform qualification.

---

## 20. Global STOP conditions

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
runtime Pak payload scan or per-mount Asset/path index reconstruction
full-path hash used as the only VFS storage index while prefix/enumeration semantics require a second ad-hoc directory SSOT
in-place mutation of installed Base Pak for normal update
network/CDN I/O hidden inside normal loadAsset()
unbounded patch chain without a compaction policy
encryption/DRM framework introduced in PAK-0 without separate approval
```

Hard-cut migrations are preferred while the architecture is still pre-product; do not leave permanent compatibility shims between retired and current designs.
