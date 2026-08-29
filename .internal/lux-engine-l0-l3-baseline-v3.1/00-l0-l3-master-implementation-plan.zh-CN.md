# Lux Engine L0–L3 总实施计划（Baseline v3.1 / LLM-construction baseline）

> 基线：`LUX-YU/lux-engine@230374a5f0d53e52bbb5d3bdce33cac62da06660`
>
> 状态：**Normative implementation baseline**。
>
> 本版目的不是继续扩展架构，而是消除会让实施 LLM 自行补设计的空白。任何与本文冲突的旧 L0–L3 implementation spec 均视为 superseded。

---

## 1. Canonical architecture

```text
L6  Product / Host
L5  Toolchain / Editor
L4  Authoring
L3  Scene
L2  Process
L1  Simulation  ─────→  World
L0  Platform / Core / Resource / Function
```

本轮**禁止新增 architecture layer**。

Canonical definitions：

```text
World
    = durable/cooked facts + whole-world storage description

Simulation
    = concrete Systems + synchronous rules + compiled schedule

Process
    = domain-blind asynchronous execution substrate

Scene
    = one World + one authoritative Registry + one Simulation

Presentation
    = Product runtime concern that samples stable Simulation state;
      NOT an architecture layer
```

长期不变量：

```text
World != ECS Registry
WorldObjectId != ecs::Entity
ECS != architecture layer
Description != runtime object
Process != second TaskGraph
Editor != Authoring
Toolchain != Runtime
Simulation logical time != wall time
```

---

## 2. LLM 施工总规则

### 2.1 No Adapter Rule

当两个现有接口接不上时，默认动作是：

```text
检查是否缺少前置 Phase
```

不是：

```text
新建 Adapter / Bridge / Context / Manager / Services / Registry
```

典型例子：

```text
ComponentSchema 不能 materialize
    -> 回 Phase 4 增加 generated decode/emplace operation
    -> 禁止 WorldComponentAdapter

System install 拿不到 Registry
    -> 使用 SimulationBuilder::registry()
    -> 禁止 SimulationBuildContext

Scene async workflow 生命周期不好处理
    -> 修正 operation ownership
    -> 禁止 SceneAsyncScope / SceneExecutionContext
```

### 2.2 Public Type Budget

每个 Phase 只有本文明确列出的 production public types 可以新增。

如发现必须新增一个未列出的 public class/struct/interface：

```text
STOP THAT SUBTASK
record architecture gap
return to design review
```

不得由实施 LLM 自行扩大 public surface。

以下不计入 Public Type Budget：

```text
private Impl
private wire structs
private helper functions
unit-test-only fixtures/helpers
error enum/failure struct paired with an allowed public mechanism
```

### 2.3 No speculative framework

在真实 probe 之前禁止创建：

```text
SceneRuntime orchestrator
StreamingManager
WorldStreamingSelection
WorldStreamingBinding
WorldMaterializationPlan
WorldMaterializationRegistry
SystemFactoryRegistry
ServiceLocator
DependencyInjectionContainer
SceneServices
SceneContext
PresentationManager
LaneManager
ClockManager
TimeDomainRegistry
generic EventBus
```

### 2.4 Layer-first dependency order

施工必须严格按本文 Phase 顺序。

低层 Phase 未通过 tests/installed consumer 之前，不进入依赖它的高层 Phase。

禁止为了让高层先编译而：

```text
stub public API
compatibility alias
temporary manager
temporary adapter
legacy bridge
```

### 2.5 No compatibility shim by default

本轮允许 wire/schema version bump 与完整 recook。

除非本文明确要求，否则禁止：

```text
v1->v2 runtime compatibility layer
FrameInfo alias
legacy SceneDescription bridge
old SystemRegistry facade
old AssetManager naming shim
```

---

## 3. Frozen ownership summary

### World

```text
owns facts/storage metadata
not Registry
not runtime streaming state
```

### Scene

```text
owns:
    shared WorldDescription
    authoritative Registry
    Simulation
    Scene cancellation source

not owns:
    mandatory PartitionIndex
    loaded/pending partition sets
    WorldObjectId->Entity map
    AssetResidency
    Process scope
    Render backend
    main loop
```

### Simulation

```text
owns:
    concrete System instances
    TaskGraph/schedule
    synchronous rules

not owns:
    World IO
    World materialization
    partition lifecycle
    wall clock
```

### Streaming

```text
policy/ownership belongs to concrete developer System
mechanical World IO/materialization belongs to scene/runtime/world
```

### Runtime identity

```text
runtime gameplay identity = ecs::Entity
WorldObjectId = durable World/Authoring/cook identity
```

不建立 generic Scene-wide：

```text
WorldObjectId -> Entity
PartitionId -> Entity[]
```

---

## 4. Strict construction dependency graph

```text
PHASE 0   SSOT / supersede / soft gates
   |
   v
PHASE 1   L0 TaskGraph dynamic dependency-list prerequisite
          - TaskDependencies only
          - modify existing modules/core/task; no new target
   |
   v
PHASE 2   L1 World semantic metadata
   |
   v
PHASE 3   L1 World physical storage/wire
   |
   v
PHASE 4   L1 ECS prerequisites
          - double Transform
          - generated component decode/emplace seam
   |
   v
PHASE 5   L1 Simulation runtime
          - SystemRegistration
          - SystemRegistry type catalog
          - SimulationBuilder
          - Simulation
   |
   v
PHASE 6   L2 Process verification only unless a real primitive is missing
   |
   v
PHASE 7   L3 Scene core
          - SceneDescription
          - Scene
   |
   v
PHASE 8   L3 scene/runtime/world
          - WorldStorageSource
          - partition load
          - WorldMaterializer
   |
   +-----------> DESIGN BARRIER A: Asset residency/demand contract
   |
   v
PHASE 9   L3 latest-state cross-lane primitive
          - LatestSpscExchange<T>
   |
   v
PHASE 10   Product architecture probes
          - Spatial3D
          - Spatial2D
          - Pixel
          - Robot
   |
   v
DESIGN BARRIER B
          - phase/time/tick API only from evidence
          - common presentation/input mechanisms only from evidence
   |
   v
PHASE 11  hard architecture gates / final negative consumers
```

---

# PHASE 0 — SSOT / anti-confusion

## 0.1 Production code

**No new production type.**

## 0.2 Required work

1. 把本 baseline 放入仓库 canonical docs 位置。
2. 对冲突旧 implementation specs 在文件首部加：

```text
SUPERSEDED BY L0-L3 BASELINE v3.1
```

或移动到明确 archive。
3. 更新 `AGENTS.md` / `.internal/directory-target-product-architecture.md`：

```text
World = facts/storage metadata
Simulation = Systems/rules/schedule
Process = domain-blind async substrate
Scene = World + Registry + Simulation
Streaming policy = concrete developer System
canonical Transform = double
Presentation may run independently from Simulation
```

4. `ValidateSourceArchitecture.cmake` 开始扫描新 production `engine/scene` root。
5. 只增加不依赖未来 target 的 source vocabulary checks；不要提前创建空 target。

## 0.3 Forbidden

```text
创建 scene/runtime placeholder class
创建 TimeDomain/TickGroup types
创建 System adapter
创建 compatibility shim
```

## 0.4 Exit gate

- repo search 不存在两个同时自称 normative 且互相冲突的 L0–L3 spec；
- architecture wording 一致；
- build 未新增 production target/type。

---

# PHASE 1 — L0 TaskGraph dependency-list prerequisite

## 1.1 Allowed public type

唯一允许新增：

```text
lux::task::TaskDependencies
```

并允许一个与现有 `task::resources(...)` 对称的：

```text
task::dependencies(span<TaskHandle>)
```

**不得新增 target/package。** 直接修改现有 `modules/core/task`。

## 1.2 Why

`SimulationDescription` 的 System predecessor 数量由资产决定；现有 TaskGraph public property 只有单个 `TaskDependency`，缺少动态数量 owning property。

这个缺口属于 generic TaskGraph，而不是 L1 Simulation。

禁止 L1 为此新增：

```text
SystemTaskRegistry
TaskDependencyAdapter
TaskGraphService
```

## 1.3 Exact integration

`TaskDependencies` 只是：

```cpp
struct TaskDependencies final
{
    std::vector<TaskHandle> values;
};
```

加入现有 `TaskGraphBuilder` property collection，最终仍由 existing `addPending()` 完成 invalid/duplicate/forward-dependency validation。

不改变：

```text
single-pass TaskGraph build
backward-only explicit dependency
resource hazard semantics
TaskHandle lifetime
```

## 1.4 Exit gate

- empty / N dependency aggregate tests；
- duplicate across aggregate + single dependency仍由 existing error拒绝；
- foreign/forward handle behavior不变；
- no new scheduler/registry/adapter。

详见 `01`。

---

# PHASE 2 — L1 World semantic model

## 2.1 Allowed public types

新增仅允许：

```text
WorldBundleId
WorldBundleGeneration
WorldChunkReference
WorldStorageVolumeDescription
WorldPartitionTablePageDescription
WorldPartitionTable
WorldPartitionIndexDescription
```

以及它们必需的 error/failure 类型。

优先修改/复用现有：

```text
WorldDescription
WorldDescriptionBuilder
WorldPartitionLayout
WorldPartitionLayoutBuilder
WorldPartitionBuildProduct
WorldPartitionerDescriptor
WorldPartitionIndexTypeId
```

## 2.2 Required changes

- `WorldDescription` 改为 metadata-only。
- root/sidecar strong identity：BundleId + Generation + VolumeOrdinal。
- **不要新建 `WorldPartitionWorkspace`**。
- 直接把现有：

```cpp
WorldPartitionLayoutBuilder(const WorldDescription& world)
```

改为：

```cpp
explicit WorldPartitionLayoutBuilder(
    std::span<const WorldObjectId> objects
);
```

Authoring/Toolchain object universe 以后直接传给现有 builder。

## 2.3 Exit gate

- WorldDescription retained memory 不再与 object count 线性相关；
- existing partition build product 仍可由 object-id span 构建；
- no Workspace/Manager type。

详见 `02`。

---

# PHASE 3 — L1 World physical storage/wire

## 3.1 Allowed public types

```text
WorldPartitionData
WorldPartitionObjectView
```

如果现有 World storage package 已有等价 value/view，则修改现有类型，不重复创建。

Wire headers/descriptors 默认 private，不进入 public type budget。

## 3.2 Required work

- root Asset wire v2；
- non-Asset sidecar volumes；
- fixed-stride chunk descriptors；
- paged partition table；
- partition-index descriptors；
- independently compressed/digested chunks；
- partition payload decode/view。

冻结：

```text
minimum generic World IO unit = partition
```

Object-level materialization later 不允许额外 object disk IO。

## 3.3 Exit gate

- mixed-generation bundle 必须失败；
- exact range IO 可证明；
- 1M partition root retained-memory benchmark；
- malformed/overflow/digest tests。

详见 `02`。

---

# PHASE 4 — L1 ECS prerequisites

这是 L3 materialization 的**硬前置**，不能推迟到 L3 临时补 adapter。

## 4.1 Allowed public surface changes

优先**扩展现有**：

```text
Transform2D
WorldTransform2D
Transform3D
WorldTransform3D
ComponentSchema
ComponentOperations (only if implementation needs the existing container)
```

不新增 generic materialization class。

允许新增一个 paired failure type：

```text
ComponentDecodeFailure
```

## 4.2 Double migration

Canonical user-facing：

```text
Transform2D/3D       double
WorldTransform2D/3D  double
TransformSystem math double
```

Render/physics/navigational dense local data 保留 float，并在 consumer boundary 显式 relative conversion。

## 4.3 Generated decode/emplace seam

`ComponentSchema` 增加一个 generated operation，语义固定为：

```cpp
using DecodeEmplaceComponentFn =
    lux::cxx::expected<void, ComponentDecodeFailure> (*)(
        ecs::Registry&,
        ecs::Entity,
        std::uint32_t encoded_schema_version,
        std::span<const std::byte> encoded_payload
    ) noexcept;
```

`ComponentSchema` 中固定增加名为 `decode_emplace` 的 function pointer 字段：

```cpp
struct ComponentSchema final
{
    lux::cxx::TypeToken cpp_type;
    ComponentSchemaId id;
    std::uint32_t version{1};
    ComponentOperations operations;
    DecodeEmplaceComponentFn decode_emplace{};
    EComponentSnapshotPolicy snapshot{EComponentSnapshotPolicy::COPY};
    std::shared_ptr<const void> code_lifetime;
};
```

`decode_emplace == nullptr` 表示该 component schema 当前不能由 durable encoded payload direct materialize；`WorldMaterializer` 遇到 name-match 但 null thunk 时返回 structured failure，不得 fallback 到 RefClass。

该字段名与语义已冻结；**不得另建**：

```text
ComponentMaterializer
ComponentCodecRegistry
ComponentFactory
WorldComponentAdapter
```

Generated thunk 必须直接：

```text
decode stable schema payload
-> registry.emplace/replace<Component>()
```

不得热路径 `RefClass` field walk。

## 4.4 Direct-materializable restriction

v1 generated direct component materialization只支持：

```text
Registry-owned component state
self-contained payload
no external side effect
no Entity reference resolution requirement
```

包含 runtime `ecs::Entity` 引用、external physics/render/audio handle 的 component 不走 generic direct materializer。

## 4.5 Exit gate

- double Transform tests；
- hierarchy/transform behavior不回退；
- generated decode/emplace test；
- malformed payload不会留下半个 component；
- no RefClass hot-path materialization。

详见 `03`。

---

# PHASE 5 — L1 Simulation runtime

## 5.1 Allowed public types

仅：

```text
SystemRegistration
SystemRegistry
SimulationBuilder
Simulation
paired build/registration/execution failure types
```

`SystemRegistry` 语义重置为 **System TYPE catalog + install thunk**；不再持 concrete instances。

## 5.2 Exact registration

```cpp
class SimulationBuilder;

using InstallSystemFn =
    lux::cxx::expected<void, SystemBuildFailure> (*)(
        SimulationBuilder&,
        SimulationSystemView
    ) noexcept;

struct SystemRegistration final
{
    SystemTypeId type;
    std::uint32_t version{};
    InstallSystemFn install{};
};
```

本轮不为未来 plugin 增 `code_lifetime/user_context`。

每个 concrete package显式暴露 `std::span<const SystemRegistration>`；Product显式 `SystemRegistry::add()`。

## 5.3 Exact builder budget

第一版 `SimulationBuilder` 只允许：

```text
registry()
emplaceSystem<T>()
findSystem<T>()
addSystemTask<T>()
addSystemCommandTask<T>()
```

没有：

```text
Services
Context
DI
TaskGraphBuilder public access
Asset/Process/Render client
RuntimeResourceRegistry
```

System-private runtime作为 System/Impl 成员；多个 L1 Systems共享且 lifetime=Registry 的 concrete runtime mechanism使用 existing `Registry::ctx()`，不创建 Services bag。

## 5.4 Task dependency mapping

v1 每个 System最多一个 primary scheduled task。

Builder private implementation：

```text
SimulationDescription predecessors
    -> predecessor primary TaskHandles
    -> Phase 1 task::dependencies(...)

Type::Access
    -> existing simulation::ecs::systemTaskResources<Type>()

both
    -> existing lux::task::TaskGraphBuilder
```

不创建 `SystemTaskRegistry`。

0-task System可以存在，但出现在 execution dependency edge 中则 build failure。

## 5.5 EcsCommandWriter path

需要 structural commands 的 System 只能走：

```text
addSystemCommandTask<T>(instance, explicit EcsCommandProducerCapacity, callable)
```

Simulation private拥有至多一个 `EcsCommandBuffer`；builder收集 producer capacities，并在所有 primary tasks 后生成一个 existing `applyEcsCommands` flush task。

System task failure时 pending commands discard，不 apply。

禁止 generic command manager。

## 5.6 Deterministic construction

- validate all registrations/version first；
- `SimulationDescription` dependency graph deterministic topological order；
- independent-node tie-break使用 stable `SystemInstanceId` ordering；
- install thunk只可 `findSystem()` declared predecessor；
- build后 runtime topology immutable；
- TaskGraph destroyed before System objects；
- System objects destroyed before Scene-owned Registry。

## 5.7 Runtime execution baseline

```cpp
Simulation::create(
    Registry&,
    shared_ptr<const SimulationDescription>,
    const SystemRegistry&);

Simulation::execute(task::TaskExecutor&);
```

`execute()` 只表示“一次 compiled graph invocation”。

Phase 5 不创建：

```text
TimeDomainId
TickGroup
ScenePhase
ClockManager
FrameInfo compatibility
```

`SimulationExecutionFailure` 至少区分 TaskExecutor、System task、EcsCommand apply failure；generic layer不 type-erase arbitrary concrete System error payload。

## 5.8 Lane invariant

`Simulation::create()` 必须由 Product 在 intended Simulation Lane 调用，因为 LuxObject System affinity在 constructor thread冻结。

L1 不创建线程/scheduler/lane owner。

## 5.9 Exit gate

- SystemRegistry只存 types/thunks；
- L3 registration可加入而 L1不 include L3；
- dynamic predecessor list直接使用 Phase 1 TaskDependencies；
- one primary task / one final command flush invariant测试；
- no `SystemFactory/Context/Services/SystemTaskRegistry`。

详见 `04`。

---

# PHASE 6 — L2 Process verification

默认 **tests only**。

现有：

```text
OperationPort<T>
portSender
File/range IO
scheduler/execution resources
```

如果足够支持 Phase 8，不增加新 Process abstraction。

Process 永远不知道：

```text
Scene
WorldPartitionData workflow
StreamingSystem
AssetResidency
Render gameplay policy
```

只有实际缺少 byte-range IO primitive 时才在 L2 增最小 primitive。

---

# PHASE 7 — L3 Scene core

## 7.1 Allowed public types

仅：

```text
SceneDescription
Scene
SceneBuildFailure
```

## 7.2 SceneDescription

精确 durable content：

```cpp
struct SceneDescription final
{
    asset::AssetId world;
    asset::AssetId simulation;
};
```

canonical Asset type string：

```text
lux.scene.description
```

## 7.3 Scene creation signature

```cpp
class Scene final
{
public:
    [[nodiscard]] static
    lux::cxx::expected<std::unique_ptr<Scene>, SceneBuildFailure>
    create(
        std::shared_ptr<const world::WorldDescription> world,
        std::shared_ptr<const simulation::SimulationDescription> simulation,
        const simulation::SystemRegistry& systems
    ) noexcept;

    [[nodiscard]] const world::WorldDescription& world() const noexcept;
    [[nodiscard]] simulation::ecs::Registry& registry() noexcept;
    [[nodiscard]] const simulation::ecs::Registry& registry() const noexcept;
    [[nodiscard]] simulation::Simulation& simulation() noexcept;
    [[nodiscard]] const simulation::Simulation& simulation() const noexcept;

    [[nodiscard]] std::stop_token stopToken() const noexcept;
    void requestStop() noexcept;
};
```

## 7.4 Lane-affinity invariant

`Scene::create()` 必须在该 Scene 的 **Simulation Lane** 上执行最终 construction。

原因：concrete System 可能是 `LuxObject`，而 LuxObject construction-thread affine。

禁止：

```text
Main thread construct Scene/System
-> move execution to Simulation thread
```

单线程 Product 只需：

```text
Simulation Lane == Main Lane
```

## 7.5 Exit gate

- Scene stable address/non-movable；
- Simulation destroyed before Registry；
- stop requested before owned runtime destruction；
- no SceneFactory/SceneContext/SceneServices；
- no streaming/index state。

详见 `05`。

---

# PHASE 8 — L3 scene/runtime/world

## 8.1 Allowed public types

仅：

```text
ReadWorldStorageRange
WorldStorageSource
WorldMaterializer
WorldStorageRuntimeFailure
WorldMaterializeFailure
```

`loadWorldPartition` 是 free function / Sender-producing function，不创建 loader owner class。

## 8.2 WorldStorageSource

必须是 value/shared-state capability：

```text
owns/retains loaded WorldDescription
owns/retains bundle-bound physical read capability
copyable/movable by value
```

异步 operation capture `WorldStorageSource` by value。

不得借用：

```text
Scene&
Registry&
System&
WorldDescription& whose owner may disappear
```

## 8.3 Partition load

概念精确入口：

```cpp
loadWorldPartition(
    WorldStorageSource source,
    world::WorldPartitionOrdinal partition,
    std::stop_token stop
) -> Sender<world::WorldPartitionData>;
```

最小 generic IO unit = partition。

## 8.4 WorldMaterializer

唯一 generic materialization owner type：

```cpp
class WorldMaterializer final
{
public:
    [[nodiscard]] static
    lux::cxx::expected<WorldMaterializer, WorldMaterializeFailure>
    create(
        std::shared_ptr<const world::WorldDescription> world,
        simulation::ecs::ComponentSchemaSet components
    ) noexcept;

    [[nodiscard]]
    lux::cxx::expected<simulation::ecs::Entity, WorldMaterializeFailure>
    object(
        simulation::ecs::Registry& registry,
        world::WorldPartitionObjectView object
    ) const noexcept;

    [[nodiscard]]
    lux::cxx::expected<void, WorldMaterializeFailure>
    partition(
        simulation::ecs::Registry& registry,
        const world::WorldPartitionData& data,
        std::vector<simulation::ecs::Entity>* created = nullptr
    ) const noexcept;
};
```

内部只缓存：

```text
World schema ordinal -> const ComponentSchema* / generated decode thunk
```

禁止 Plan/Binding/Registry/Scratch/Context types。

## 8.5 v1 schema mapping

直接映射候选规则：

```text
WorldDataSchemaId canonical name == ComponentSchemaId canonical name
```

运行时：

```text
schema absent from ComponentSchemaSet
    -> ignore

schema present
    -> call generated decode/emplace thunk
```

unknown component默认 ignore 以支持 headless/product-specific schema subsets。

## 8.6 Atomicity

`object()` failure：destroy newly created entity。

`partition()` failure：destroy every entity created by this call，`created` 清空。

Generic materializer只允许 Registry-owned reversible state；禁止 external side effects。

## 8.7 Async lifetime

Scene teardown：

```text
disconnect new intents
request Scene stop
Scene may be destroyed
late workflow owns all required source data by value/shared state
weak completion target expired -> discard
```

Host structured scope只在 Product shutdown统一 join。

禁止 per-Scene async scope wrapper。

## 8.8 Exit gate

- exact partition IO tests；
- object materialization = zero extra IO；
- cancellation；
- requester destruction race；
- rollback；
- no Scene borrow in async operation state；
- Process remains domain-blind。

详见 `06`。

---

# DESIGN BARRIER A — Asset residency / resource demand

当前只冻结 ownership方向：

```text
CPU decoded asset lifetime/residency = engine runtime shared/Host-level concern
GPU residency/lifetime = Render-owned
World sidecars != Assets
```

在 Probe A 之前**禁止实施 generic demand wiring**。

禁止提前创建：

```text
DemandKey
DemandTracker
AssetRuntimeBridge
ResourceDemandRegistry
ResidencyConnection
```

只有真实 Mesh/Material/Texture workload出现后再冻结最小 API。

详见 `08`。

---

# PHASE 9 — L3 latest-state cross-lane primitive

## 9.1 Allowed public type

仅：

```text
LatestSpscExchange<T>
```

第一版位置固定：

```text
engine/scene/runtime/presentation
```

先不要新建 `modules/core/concurrency` target。

若 probes 后出现非-engine consumer，再决定下沉。

## 9.2 Exact semantic API

```cpp
template<class T>
class LatestSpscExchange final
{
public:
    static_assert(std::is_default_constructible_v<T>);

    [[nodiscard]] T& write() noexcept;       // producer only
    void publish() noexcept;                 // producer only, never waits

    [[nodiscard]] bool acquireLatest() noexcept; // consumer only, never waits
    [[nodiscard]] const T& read() const noexcept;
};
```

固定三 persistent slots。

语义：

```text
SPSC only
latest wins
intermediate states may disappear
producer never waits
consumer never waits
no per-publish allocation
```

实现采用 triple-buffer role exchange；详见 `09`。

## 9.3 Exit gate

- TSAN/stress；
- 100M publish/acquire monotonic-content test；
- producer/consumer no wait；
- fixed allocation after construction。

---

# PHASE 10 — Product architecture probes

四个**独立 Product binaries**：

```text
A Spatial3D large-world
B Spatial2D large-world
C Noita-like Pixel
D Robot fixed-step simulation
```

Probe 可创建 concrete domain types，但不能反向修改 generic L0–L3 public API，除非记录 architecture gap 并进入 Design Barrier B review。

Probe 目的：证明/否定 generic contract，不是恢复 legacy Player。

详见 `11`。

---

# DESIGN BARRIER B — only after probes

在此之前明确禁止 production：

```text
TimeDomainId
TickGroup
ScenePhase
ClockManager
TimeDomainRegistry
PresentationManager
universal fixed-step accumulator
```

四 probe evidence 完成后才决定：

```text
phase exact API
multi-rate/fixed-step exact API
presentation sampling policy
Presentation->Simulation ingress common primitive
whether any streaming source marker deserves promotion
```

重复出现至少两个 independent domain 且 ownership一致，才允许提升 generic abstraction。

---

# PHASE 11 — hard architecture gates

最后才针对已经存在的 target/type 开完整 hard gate：

```text
negative dependency consumers
source forbidden vocabulary
installed consumer matrix
no lower->higher dependency
no Process business type
no Scene mandatory streaming policy
no canonical float Transform
no old SystemRegistry instance-owner facade
```

---

## Cross-cutting A — Cross-lane mutation invariant

从 Phase 9 开始统一：

```text
Authoritative Registry mutation only on Simulation Lane
```

其他 lane：

```text
Platform input
Presentation/UI
Render completion
Process completion
network/sensor input
```

不得直接读写 concurrently-mutating Registry。

它们必须在 Simulation-owned ingress boundary 被 adopt。

在 probes 前不新建 generic `SimulationCommandQueue/InputBus`；可使用已有 LuxObject Event、probe-local SPSC packet 等最小手段。

---

## Cross-cutting B — Execution topology frozen semantics

```text
Time semantics
Ordering
CPU execution ownership
```

是三个正交问题。

本文只冻结术语概念：

```text
Time Domain   = logical time semantics
Tick Group    = ordering inside a schedule/domain
Execution Lane= CPU execution ownership
```

但 **TimeDomain/TickGroup production API 留到 Barrier B**。

通用 runtime 必须允许：

```text
Simulation Lane
Presentation Lane
Render Thread/Domain
```

Simulation 慢/快均不反压 Presentation。

---

## Cross-cutting C — Render transport split

```text
Simulation -> Presentation state
    LatestSpscExchange<T>
    latest-wins

Simulation -> Presentation transient event
    reliable domain-specific channel

Presentation -> Render frame
    existing BoundedSpscFrameRing
    bounded frame-ahead

Workers/Process -> Render operation
    reliable Sender / bounded Render ingress
```

禁止重新合并为 universal bidirectional game/render queue。

---

## Cross-cutting D — Large World precision frozen contract

```text
canonical CPU/world Transform = double
spatial index/query = double
World spatial wire = double
Render = subtract origin in double, then cast float
Jolt = PRIVATE backend; prefer double-position capability
Dense geometry/sensor/local execution data = float where appropriate
```

禁止 Scene-wide floating-origin mutation。

详见 `03`。

---

## Cross-cutting E — Handoff rule for implementation LLM

每个 Phase 开始前必须：

1. 只阅读本 baseline 中该 Phase 及其前置文档；
2. 列出该 Phase “Allowed public types”；
3. repo search 确认没有现有等价类型；
4. 优先修改现有类型；
5. 完成 exit gate 后才进入下一 Phase。

如果需求无法在允许类型内实现：

```text
DO NOT invent a wrapper.
Record the exact missing capability.
Stop that phase's dependent work.
```

这条优先级高于“让代码尽快编译”。
