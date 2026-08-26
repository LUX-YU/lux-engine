# Lux Engine L1 Simulation 最终重构实施规范

**文档性质：Normative Implementation Specification（规范性实施文档）**  
**日期：2026-08-26**  
**目标仓库：`LUX-YU/lux-engine`**  
**目标分支基线：`codex/object-ui-foundation`**  
**编写时分支 HEAD：`673b3d17b0324b9c0a57a9316dab0a461f5bfd91`**  
**该 HEAD 的 production/test parent：`d6754945c9dbb8d581f6b46fd46e93e69be58247`**  
**原始架构合同：`Lux_Engine_vNext_Canonical_Architecture_Contract_2026-08-26`**  
**原始合同 SHA-256：`892f74e8fe78869be467f3ea30cd047b34635135da7fefd2645f8ef7bdce04f5`**

---

## 0. 文档优先级与实施者须知

本文是经过后续架构讨论后形成的 **L1 Simulation 最终实施规范**。

对于下列主题，本文 **明确修订并取代** 原始 Canonical Architecture Contract 中较早的设计：

1. `SimulationDescription` 只保存少量 typed rule/config data 的最小模型；
2. `EcsState + EcsMutation + Lux Query` 作为默认 ECS 用户接口的设计；
3. 通用 `EcsChangeJournal / ChangeCursor / ComponentChanges / EntityChanges` 作为 L1 baseline observation 机制的设计；
4. `EcsChangeBatch / SimulationEcsMutation` 作为普通 ECS 写入路径的设计；
5. `HierarchySystem` 作为注册 System 的设计；
6. 把 `EcsState + EcsChangeJournal + EcsCommandBatch` 写死到通用 `executeSimulationStep()` 的设计；
7. 单一 `ScriptSystem` 在固定帧位置执行所有脚本的隐含模型；
8. 额外建立 `SimulationContract` 的方案。

原始合同中以下原则 **继续有效且不得削弱**：

```text
World      = Facts
Simulation = Rules + synchronous mechanisms
Process    = asynchronous/stateful orchestration
Scene      = World + Simulation + Process/run-root composition
```

以及：

```text
World never owns ECS.
World Object != ECS Entity.
TaskGraph remains the only generic CPU scheduler/composition graph.
Product/Host owns concrete capacities and budgets.
Foundation code does not embed product capacity defaults.
Asset payload remains immutable Description data.
No compatibility architecture is added merely to preserve pre-freeze call sites.
```

### 0.1 对实施 LLM 的硬性要求

本文中的 `MUST / MUST NOT / SHOULD / MAY` 为规范词：

- **MUST**：必须实现；
- **MUST NOT**：禁止实现；
- **SHOULD**：除非有明确且可记录的技术阻塞，否则应实现；
- **MAY**：可选，不应因为“可能以后有用”而自动实现。

实施者遇到本文没有覆盖的情况时：

> **不得通过新增 public interface、base class、registry、manager、service locator、event bus、history、compatibility alias 来自行补全架构。**

如果缺口确实无法在 private/internal 范围处理：

1. 保持现有 public surface 不扩张；
2. 在实施报告中写明 gap；
3. 等待架构决策；
4. 不自行“设计一个通用方案”。

---

# Part I — 最终 L1 Simulation 心智模型

## 1. Simulation 的定义

最终定义：

```text
Simulation
    = 一个可持久化的完整 SimulationDescription
      + 解释该 Description 的同步运行时代码
      + Systems
      + TaskGraph
      + concrete runtime resources
```

`Simulation` **不是**：

```text
固定 ECS runtime framework
固定 Physics loop
固定 ScriptSystem
固定 Hierarchy
固定 Transform
固定 ChangeJournal
固定 EventBus
固定 Frame lifecycle enum
```

同一个 `WorldDescription` 可以被不同 Simulation 解释：

```text
WorldDescription
      │
      ├── Default3DSimulation
      │      EnTT + Physics + Animation + Audio + Script
      │
      ├── DedicatedServerSimulation
      │      EnTT + Gameplay + Network
      │
      ├── EditorPreviewSimulation
      │      preview-only systems
      │
      └── RobotSimulation
             custom state / custom systems
             MAY have no EnTT at all
```

因此：

> ECS 是 Simulation 可以选择的一种实现机制，而不是 Simulation 的定义。

---

## 2. `SimulationDescription` 的最终定义

`SimulationDescription` 是 **一个具体 Simulation 的完整、canonical、immutable、可序列化描述**。

逻辑上：

```text
SimulationDescription
│
├── global typed/versioned Simulation data
│
├── System instances
│   ├── stable System type
│   ├── instance name
│   ├── System description version
│   ├── optional configuration schema + payload
│   ├── capabilities
│   ├── execution points
│   └── intrinsic events
│
└── cross-System execution dependencies
```

它必须足以让：

- Editor 在没有 runtime state 的情况下检查 Simulation 结构；
- Asset codec 完整保存/加载 Simulation；
- runtime composition code 验证当前代码是否能够解释该资产；
- Script/behavior tooling 发现可绑定 execution point 与 event；
- 后续 Process/Scene 引用这个 immutable Simulation asset。

### 2.1 `SimulationDescription` 是完整语义描述，不是 compiled TaskGraph

“完整”不等于把实现细节序列化。

禁止保存：

```text
TaskGraph node id
TaskResourceKey
worker index
lambda / function pointer
SystemId
SystemLease
SystemRegistry pointer
entt::registry pointer
LuxObject pointer
SignalView
TypeToken pointer
Physics backend pointer
runtime Entity
thread id
runtime event queue
runtime dirty set
runtime history
```

最终关系：

```text
SimulationDescription       durable semantic definition
        │
        │ interpreted by current code
        ▼
Runtime Simulation
        ├── SystemRegistry / concrete System objects
        ├── TaskGraph
        ├── optional EnTT Registry
        ├── optional PhysicsWorld
        ├── optional Animation runtime
        └── optional Script runtime
```

---

## 3. Simulation Asset 的定义

继续采用原始 Asset contract：

```text
Simulation asset payload = shared_ptr<const SimulationDescription>
```

不新增 resident C++ wrapper：

```text
class SimulationAsset          // FORBIDDEN
class RuntimeSimulationAsset   // FORBIDDEN
```

“SimulationAsset”仅表示逻辑资源类别。

Asset decode：

```text
bytes
  ↓ validate wire + caller budgets
immutable SimulationDescription
```

Asset decode **MUST NOT**：

```text
instantiate Systems
load script VM
create EnTT registry
create PhysicsWorld
compile TaskGraph
connect signals
run migration through arbitrary runtime code
```

---

# Part II — System 自描述模型

## 4. System 的最终职责

System 是 reusable synchronous behavior/runtime unit。

一个 System 类型应当同时具有两类完全不同的静态信息：

```text
Type::Access
    = TaskGraph scheduling/hazard metadata

Type::Description
    = durable semantic declaration source
```

不要合并二者。

`Access` 回答：

> 运行时这个 System 会读写哪些 component/resource？

`Description` 回答：

> 这个 System 是什么？提供什么 capability？有哪些可观察 execution point？会产生哪些 intrinsic event？配置 schema 是什么？

---

## 5. System 必须自描述 execution points

System 被多个 Simulation 复用时，它自身必须携带稳定的 execution-point 声明。

例如：

```cpp
struct PhysicsSystem
{
    static constexpr inline SystemExecutionPoint BeforeStep{
        "before_step"
    };

    static constexpr inline SystemExecutionPoint AfterStep{
        "after_step"
    };

    // ... Description / Access / runtime implementation
};
```

**Simulation builder 不得再次手写同样的 point 名称作为第二 source of truth。**

错误：

```cpp
builder.addSystem("physics", PhysicsSystem::Description, config);
builder.addExecutionPoint("physics", "before_step"); // FORBIDDEN
```

正确：

```cpp
builder.addSystem("physics", PhysicsSystem::Description, config);
```

Builder 自动 materialize `PhysicsSystem::Description` 中的 points。

---

## 6. `SystemExecutionPoint`

第一版 public 类型保持极简：

```cpp
struct SystemExecutionPoint final
{
    std::string_view name;
};
```

其唯一语义：

> System 对外承诺存在一个具有稳定名字的同步执行边界。

不得增加：

```text
priority
thread
worker
script language
TaskId
TaskGraph node
callback address
delivery mode
time scale
frame enum
physics enum
```

### 6.1 Execution Point 的 callback 基本签名

L1 对 phase-like execution point 固定使用：

```cpp
struct SimulationStepInfo final
{
    float delta_seconds{};
    std::uint64_t step_index{};
};
```

行为 callback 的逻辑签名为：

```cpp
void(const SimulationStepInfo&) noexcept;
```

这里不包含 `Entity self`。

如果某个 script/behavior 是绑定到某个 entity/object 的实例，`self` 由 behavior binding/runtime instance 自己持有，不重复放入 phase payload。

---

## 7. 删除 `FrameInfo`

现有：

```cpp
struct FrameInfo
{
    float delta_seconds;
    uint64_t tick_index;
};
```

必须删除并替换为：

```cpp
struct SimulationStepInfo
{
    float delta_seconds;
    uint64_t step_index;
};
```

理由：

```text
render frame != Simulation step
```

L2 Process 未来可能在一个 render frame 中执行：

```text
0 / 1 / 2 / N fixed Simulation steps
```

禁止 compatibility alias：

```cpp
using FrameInfo = SimulationStepInfo; // FORBIDDEN
```

---

## 8. System capability

为满足 Editor/用户判断某个 Simulation 是否拥有 Physics、Animation、Audio、Scripting 等能力，capability 由 System 自描述。

**不创建全局 enum。**

示例：

```cpp
static constexpr inline std::array Capabilities{
    std::string_view{"physics.simulation"},
    std::string_view{"physics.query"},
};
```

名称必须 namespaced/stable。

示例：

```text
physics.simulation
physics.query
animation.playback
animation.markers
audio.playback
scripting.lua
navigation.query
render.extraction
```

L1 MUST NOT 定义：

```cpp
enum class ESimulationCapability { Physics, Audio, ... }; // FORBIDDEN
```

因为新的 subsystem 不应要求修改 L1 中央 enum。

`SimulationDescription::hasCapability(name)` 通过 materialized System capabilities 回答。

同一 capability 可以有多个 System provider。

---

# Part III — System Event

## 9. Event 是一等 Simulation 描述概念

Event 与 Execution Point 不同：

```text
Execution Point
    = 确定存在的同步调度边界

Event
    = 运行过程中条件发生的 0..N occurrence
```

例如：

```text
PhysicsSystem::AfterStep               execution point
PhysicsSystem::CollisionBegin          event

AnimationSystem::AfterEvaluate         execution point
AnimationSystem::Marker                event
```

碰撞和动画 Marker 不应被伪装成“固定脚本阶段”。

---

## 10. Intrinsic Event 由 System 自声明

如果事件是 System 固有产生的语义，则由该 System 声明。

建议静态类型：

```cpp
struct SystemEventDescription final
{
    std::string_view name;
    std::string_view dispatch_point;

    // durable semantic schema
    std::string_view payload_schema_name;
    std::uint32_t payload_schema_version{};

    // code-side validation only; NEVER serialized as pointer/object
    lux::cxx::TypeToken payload_cpp_type;
};
```

通过 helper 创建：

```cpp
static constexpr inline auto CollisionBegin =
    makeSystemEvent<CollisionBeginEvent>(
        "collision_begin",
        AfterStep,
        "lux.physics.collision-begin",
        1
    );
```

`makeSystemEvent<Payload>` 必须自动填充 code-side payload type metadata。

调用者不得手工传 C++ type hash/type name 造成漂移。

### 10.1 无 payload event

允许：

```cpp
makeSystemEvent<void>(...)
```

规则：

```text
payload_schema_name = empty
payload_schema_version = 0
payload_cpp_type = void
```

---

## 11. Event 必须归属于一个 System instance

最终 materialized event 地址：

```text
System instance + Event name
```

例如：

```text
physics / collision_begin
animation / marker
```

不要求全 Simulation event name 全局唯一。

允许：

```text
physics_a / collision_begin
physics_b / collision_begin
```

只要求一个 System instance 内 event name 唯一。

---

## 12. Event 必须绑定到该 System 的 execution point

`dispatch_point` 必须引用同一个 System declaration 中的 execution point。

例如：

```text
CollisionBegin -> AfterStep
Marker         -> AfterEvaluate
```

第一版禁止：

```text
Physics event dispatch at AnimationSystem point
source-less global event
runtime-created event kind
```

跨 System 关系通过 Simulation dependency/composition 表达，不通过 event 偷渡调度语义。

---

## 13. Event 是瞬态 occurrence，不是 history

生命周期：

```text
step N
    produce
      ↓
    transient buffer
      ↓
    declared dispatch point
      ↓
    dispatch
      ↓
    clear/discard
```

禁止 L1 通用 Event：

```text
EventJournal
EventCursor
EventEpoch
EventHistory
readSince(sequence)
oldest retained event
```

Replay、Network replication、Audit、Telemetry 若需要历史，必须由各自 subsystem 实现自己的语义日志。

---

## 14. Event ordering

同一 event kind 的 occurrence 顺序必须 deterministic。

不得依赖：

```text
worker completion order
thread id
pointer order
unordered_map iteration order
```

多 producer 实现 SHOULD 使用类似：

```text
producer ordinal ascending
then producer-local append order
```

同一个 execution point 下多个 event kind 的 dispatch 顺序：

> 按 System static Description 中 event 声明顺序。

执行点语义顺序固定为：

```text
1. preceding System work quiescent
2. validate transient event batches
3. dispatch events attached to this point
4. invoke execution-point behavior callbacks
5. continue graph
```

因此 `AfterPhysics` 类型 callback 看到的是：

> 该边界公开的 physics events 已经全部 dispatch 完毕。

如果某个 concrete System 真正需要 “before-event-dispatch” hook，应显式声明另一个 execution point；不得增加 `priority/before_after` 通用字段。

---

# Part IV — `SystemDescription`

## 15. 推荐最终 public shape

`SystemDescription` 是 **code-side static declaration**，不是 persisted record 本身。

建议：

```cpp
struct SystemDescription final
{
    std::string_view canonical_name;
    std::uint32_t version{};

    std::string_view configuration_schema_name;
    std::uint32_t configuration_schema_version{};

    std::span<const std::string_view> capabilities;
    std::span<const SystemExecutionPoint> execution_points;
    std::span<const SystemEventDescription> events;
};
```

所有 span 指向 System 类型的 static storage。

Builder 在调用期间同步复制其 durable 内容；`SimulationDescription` 不持有这些 span。

### 15.1 `canonical_name`

规则：

```text
non-empty
stable
namespaced
globally intended unique
```

示例：

```text
lux.physics3d
lux.animation
lux.audio
mygame.gameplay
robot.navigation
```

不要用：

```text
C++ mangled name
compiler RTTI name
pointer address
SystemId
registry index
```

### 15.2 `version`

必须非 0。

以下不兼容变更必须 bump：

```text
configuration schema incompatible change
execution point removed/renamed/reinterpreted
event removed/renamed/reinterpreted
event payload schema incompatible change
capability semantic breaking change
```

仅内部 TaskGraph 实现变化，如果 public semantics 不变，不需要 bump。

---

## 16. System configuration

`SystemDescription` MAY 声明一个 optional configuration schema：

```text
configuration_schema_name
configuration_schema_version
```

为空表示该 System instance 没有专属配置 payload。

示例：

```text
System: lux.physics3d
config schema: lux.physics3d.config
version: 2
```

`SimulationDescriptionBuilder::addSystem()` 接收已经编码好的 bytes；L1 不定义 universal object serializer。

System-specific module 可以提供：

```text
PhysicsConfig -> bytes
AnimationConfig -> bytes
```

但这不是 `SimulationDescriptionBuilder` 的职责。

### 16.1 Global SimulationData 继续存在

原有：

```text
SimulationDataSchemaId + version + payload
```

继续保留，用于：

```text
跨 System 配置
game-specific rules
script binding tables
product/domain extensions
不自然属于单个 System instance 的 data
```

因此：

```text
per-System configuration != global SimulationData
```

二者都属于完整 `SimulationDescription`。

---

## 17. 一个完整 System 声明示例

```cpp
struct PhysicsSystem final
{
    static constexpr inline SystemExecutionPoint BeforeStep{
        "before_step"
    };

    static constexpr inline SystemExecutionPoint AfterStep{
        "after_step"
    };

    static constexpr inline std::array ExecutionPoints{
        BeforeStep,
        AfterStep,
    };

    static constexpr inline std::array Capabilities{
        std::string_view{"physics.simulation"},
        std::string_view{"physics.query"},
    };

    static constexpr inline std::array Events{
        makeSystemEvent<CollisionBeginEvent>(
            "collision_begin",
            AfterStep,
            "lux.physics.collision-begin",
            1
        ),
        makeSystemEvent<CollisionEndEvent>(
            "collision_end",
            AfterStep,
            "lux.physics.collision-end",
            1
        ),
    };

    static constexpr inline SystemDescription Description{
        .canonical_name = "lux.physics3d",
        .version = 1,
        .configuration_schema_name = "lux.physics3d.config",
        .configuration_schema_version = 1,
        .capabilities = Capabilities,
        .execution_points = ExecutionPoints,
        .events = Events,
    };

    static constexpr inline auto Access =
        makeSystemAccessSpec<
            ComponentRead<Transform>,
            ComponentWrite<PhysicsPose>,
            ExternalWrite<PhysicsWorld>
        >();
};
```

注意：

- `Description` 不保存 runtime object；
- `Access` 不负责 Query；
- event payload C++ 类型只存在于 code-side static declaration；
- Builder materialize 后保存 durable schema name/version，不保存 `TypeToken` object/pointer。

---

# Part V — `SimulationDescription` materialized 模型

## 18. 新增 `SystemTypeId`

持久化的 System type identity 与 runtime `SystemId` 必须分开。

建议：

```cpp
struct SystemTypeId final
{
    std::uint64_t hash{};
    std::string name;

    [[nodiscard]] bool valid() const noexcept;
};
```

语义与现有 `SimulationDataSchemaId` 的 stable-name 模式一致：

```text
hash = stable FNV-1a(name)
name non-empty
hash collision must compare name as well
```

提供：

```cpp
SystemTypeId systemTypeId(std::string_view canonical_name);
```

禁止持久化：

```text
SystemId
TypeToken only
C++ type_info pointer
mangled name
```

---

## 19. `SimulationSystemView`

建议 public API：

```cpp
class SimulationSystemView final
{
public:
    [[nodiscard]] explicit operator bool() const noexcept;

    [[nodiscard]] std::string_view instanceName() const noexcept;
    [[nodiscard]] const SystemTypeId& type() const noexcept;
    [[nodiscard]] std::uint32_t version() const noexcept;

    [[nodiscard]] std::string_view configurationSchemaName() const noexcept;
    [[nodiscard]] std::uint64_t configurationSchemaHash() const noexcept;
    [[nodiscard]] std::uint32_t configurationSchemaVersion() const noexcept;
    [[nodiscard]] std::span<const std::byte> configurationPayload() const noexcept;

    [[nodiscard]] std::size_t capabilityCount() const noexcept;
    [[nodiscard]] std::string_view capabilityAt(std::size_t) const noexcept;
    [[nodiscard]] bool hasCapability(std::string_view) const noexcept;

    [[nodiscard]] std::size_t executionPointCount() const noexcept;
    [[nodiscard]] SimulationExecutionPointView executionPointAt(std::size_t) const noexcept;
    [[nodiscard]] SimulationExecutionPointView findExecutionPoint(std::string_view) const noexcept;

    [[nodiscard]] std::size_t eventCount() const noexcept;
    [[nodiscard]] SimulationEventView eventAt(std::size_t) const noexcept;
    [[nodiscard]] SimulationEventView findEvent(std::string_view) const noexcept;
};
```

View 必须 non-owning、轻量，生命周期不得超过 owning `SimulationDescription`。

---

## 20. System instance identity

一个 Simulation 内 `instance_name` 必须：

```text
non-empty
unique
stable within the asset
```

允许多个同 type System：

```text
physics_main     : lux.physics3d
physics_preview  : lux.physics3d
```

不要假定：

```text
one C++ System type == one Simulation instance
```

`SystemId` 仍然只是 runtime registry handle；它不等于 persisted instance identity。

---

## 21. `SimulationExecutionPointView`

建议：

```cpp
class SimulationExecutionPointView final
{
public:
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] SimulationSystemView system() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
};
```

它是 materialized persisted point。

其 stable logical address：

```text
(system instance name, point name)
```

---

## 22. `SimulationEventView`

建议：

```cpp
class SimulationEventView final
{
public:
    [[nodiscard]] explicit operator bool() const noexcept;

    [[nodiscard]] SimulationSystemView system() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] SimulationExecutionPointView dispatchPoint() const noexcept;

    [[nodiscard]] std::string_view payloadSchemaName() const noexcept;
    [[nodiscard]] std::uint64_t payloadSchemaHash() const noexcept;
    [[nodiscard]] std::uint32_t payloadSchemaVersion() const noexcept;
};
```

不暴露 persisted `TypeToken`，因为它不是 durable asset identity。

---

## 23. `SimulationDependencyView`

跨 System composition 需要明确持久化。

建议新增一个简单 view：

```cpp
class SimulationDependencyView final
{
public:
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] SimulationExecutionPointView before() const noexcept;
    [[nodiscard]] SimulationExecutionPointView after() const noexcept;
};
```

语义：

```text
before point must complete before after point may execute
```

不存 TaskGraph edge id。

---

## 24. `SimulationDescription` 推荐最终 API

保留已有 data API，并扩展：

```cpp
class SimulationDescription final
{
public:
    // existing global data
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t dataCount() const noexcept;
    [[nodiscard]] SimulationDataView dataAt(std::size_t) const noexcept;
    [[nodiscard]] SimulationDataView findData(
        const SimulationDataSchemaId&
    ) const noexcept;

    // systems
    [[nodiscard]] std::size_t systemCount() const noexcept;
    [[nodiscard]] SimulationSystemView systemAt(std::size_t) const noexcept;
    [[nodiscard]] SimulationSystemView findSystem(std::string_view instance_name) const noexcept;

    // aggregate capability lookup
    [[nodiscard]] bool hasCapability(std::string_view name) const noexcept;

    // qualified point/event lookup
    [[nodiscard]] SimulationExecutionPointView findExecutionPoint(
        std::string_view system_instance,
        std::string_view point_name
    ) const noexcept;

    [[nodiscard]] SimulationEventView findEvent(
        std::string_view system_instance,
        std::string_view event_name
    ) const noexcept;

    // cross-system ordering
    [[nodiscard]] std::size_t dependencyCount() const noexcept;
    [[nodiscard]] SimulationDependencyView dependencyAt(std::size_t) const noexcept;
};
```

不要增加运行时 mutation method。

`SimulationDescription` build 后 immutable。

---

# Part VI — `SimulationDescriptionBuilder`

## 25. Builder 是唯一 materialization mutation path

Builder 负责：

```text
copy static System declaration
validate
canonicalize strings/ids
store per-system config
materialize capabilities
materialize execution points
materialize events
add cross-system ordering
check graph consistency
build immutable SimulationDescription
```

Builder **不是 runtime System registry**。

---

## 26. `addSystem()`

推荐 API：

```cpp
[[nodiscard]]
expected<void, SimulationDescriptionFailure>
addSystem(
    std::string_view instance_name,
    const SystemDescription& system,
    std::span<const std::byte> configuration = {}
) noexcept;
```

调用：

```cpp
builder.addSystem(
    "physics",
    PhysicsSystem::Description,
    physics_config_bytes
);
```

### 26.1 `addSystem()` 必须完成的工作

顺序建议：

```text
1. validate instance name
2. validate SystemDescription canonical name + version
3. validate config schema declaration
4. validate capability names and duplicates
5. validate execution point names and duplicates
6. validate event names and duplicates
7. validate every event dispatch point exists
8. validate event payload schema
9. compute/validate stable hashes
10. reject duplicate System instance name
11. copy all durable strings
12. copy configuration payload
13. materialize points/events/capabilities
```

不得要求调用方再次调用：

```text
addCapability
addExecutionPoint
addEvent
```

第一版这些 API **禁止存在**，防止双 source of truth。

---

## 27. Builder dependency API

推荐：

```cpp
[[nodiscard]]
expected<void, SimulationDescriptionFailure>
addDependency(
    std::string_view before_system,
    SystemExecutionPoint before_point,
    std::string_view after_system,
    SystemExecutionPoint after_point
) noexcept;
```

示例：

```cpp
builder.addDependency(
    "physics",
    PhysicsSystem::AfterStep,
    "animation",
    AnimationSystem::BeforeEvaluate
);
```

Builder 必须验证：

```text
system exists
point belongs to selected system description
no self-edge
no duplicate edge
no cycle
```

如果调用方只有资产中的字符串，也 MAY 提供等价 string overload，但不能引入新的 ID registry。

---

## 28. Builder edit operations

为 Editor/Authoring 支持，SHOULD 保留/增加：

```text
eraseSystem(instance)
setSystemConfiguration(instance, payload)
addDependency(...)
eraseDependency(...)
addData / setData / eraseData
clear
build
```

删除一个 System 时：

- 必须删除该 System materialized points/events；
- 必须删除所有引用它的 dependencies；
- 不得留下 dangling reference。

---

## 29. Builder failure semantics

扩展 `ESimulationDescriptionError`，至少能够区分：

```text
INVALID_SCHEMA_ID
SCHEMA_HASH_COLLISION
INVALID_SCHEMA_VERSION
DUPLICATE_DATA
DATA_NOT_FOUND

INVALID_SYSTEM_INSTANCE_NAME
INVALID_SYSTEM_TYPE
SYSTEM_TYPE_HASH_COLLISION
INVALID_SYSTEM_VERSION
DUPLICATE_SYSTEM_INSTANCE

INVALID_CONFIGURATION_SCHEMA

INVALID_CAPABILITY
DUPLICATE_CAPABILITY

INVALID_EXECUTION_POINT
DUPLICATE_EXECUTION_POINT

INVALID_EVENT
DUPLICATE_EVENT
INVALID_EVENT_DISPATCH_POINT
INVALID_EVENT_PAYLOAD_SCHEMA

SYSTEM_NOT_FOUND
EXECUTION_POINT_NOT_FOUND
INVALID_DEPENDENCY
DUPLICATE_DEPENDENCY
DEPENDENCY_CYCLE

SIZE_OVERFLOW
ALLOCATION_FAILURE
```

命名可以轻微调整，但错误类别不得被粗暴合并为：

```text
INVALID_ARGUMENT
UNKNOWN_ERROR
```

所有 `noexcept` public builder API：

- `std::bad_alloc` -> `ALLOCATION_FAILURE`；
- `length_error / size overflow` -> `SIZE_OVERFLOW`；
- 不得 `std::terminate()`；
- 构造 allocation failure 的错误对象本身不得再次依赖新的 heap allocation。

---

# Part VII — Asset codec vNext

## 30. Simulation asset wire format 必须升级

现行 Simulation asset 只覆盖 typed data records，已经不足以表达完整 Description。

本重构后需要新的 wire version。

继续使用既定 primary magic：

```text
LXSD / 0x4453584C
```

但 format version 必须增加。

L1 尚未正式 freeze，因此：

> **默认不为旧的 pre-freeze SimulationDescription wire format保留 compatibility decoder。**

测试 fixture / generated asset 直接重建。

只有用户明确声明存在必须保留的真实外部资产时，才设计 legacy decoder。

---

## 31. 推荐 wire section

概念结构：

```text
Header
    magic
    format_version
    counts
    offsets/sizes

String table

Global SimulationData schema table
Global SimulationData records

System type/name records
System instance records
System capabilities
System execution points
System events
System dependencies

Payload bytes
    global data payloads
    system configuration payloads
```

实际字段布局可以按现有 codec 风格优化，但必须满足下列语义。

---

## 32. 资产中允许持久化的数据

允许：

```text
stable names
stable hashes
explicit schema versions
system instance names
configuration bytes
capability names
execution point names
event names
event payload schema names/hashes/versions
cross-system point references
```

禁止：

```text
TypeToken object as ABI
C++ pointer
Signal descriptor address
RefClass pointer
RefMethod pointer
SystemId
TaskId
TaskResourceKey
Entity bits
thread/worker id
```

---

## 33. Wire safety 与 Product budget

继续遵守已修正的底层原则：

```text
BinaryReader/BinaryWriter -> wire safety only
Simulation codec          -> semantic validation
Product/Host              -> concrete limits
```

Codec context 必须显式提供：

```text
max input bytes
max decoded bytes
max encoded bytes
```

如果需要更细的 Simulation limits，例如：

```text
max systems
max execution points
max events
max dependencies
max string bytes
max configuration bytes
```

具体数值必须来自 caller/Product profile，不得在 L1 foundation 写默认值。

---

## 34. Decode validation

Decode 必须验证：

```text
bounds / integer overflow
valid stable hash+name pairs
unique System instance names
valid System versions
valid schema versions
unique points within system
unique events within system
valid event dispatch references
valid event payload schema
valid dependency references
unique dependencies
acyclic dependency graph
payload range exactness
no overlapping/out-of-bounds payload ranges
```

Decode 只创建 immutable Description。

---

# Part VIII — ECS：明确使用 EnTT

## 35. 总原则

Lux L1 不再重新实现一层“类似 EnTT 的 ECS API”。

如果一个 concrete Simulation 选择 EnTT，则 C++ System author 可以直接使用 EnTT。

目标：

```text
Lux owns:
    scheduling metadata
    stable Entity type choice
    optional structural command utility
    optional snapshot/hierarchy utilities
    Simulation composition

EnTT owns:
    registry
    storage
    view/group/query semantics
    patch/signals/reactive storage
```

---

## 36. 新增极薄 `ecs::Registry`

建议：

```cpp
using Registry = entt::basic_registry<Entity>;
```

保留现有 `ecs::Entity`，因为它提供：

- 明确的 runtime entity semantic type；
- 与 EnTT custom entity 支持兼容；
- 防止通用 serializer 把 transient Entity bits 当 durable identity。

`WorldObjectId != ecs::Entity` 继续严格成立。

---

## 37. 删除 `EcsState / EcsMutation`

最终 C++ System 不再写：

```cpp
EcsState state;
auto mutation = state.mutate();
mutation->query<...>();
```

而是：

```cpp
ecs::Registry registry;

auto view = registry.view<Position, const Velocity>();
for (auto [entity, position, velocity] : view.each())
{
    // direct EnTT API
}
```

需要 observable update：

```cpp
registry.patch<Position>(entity, [](Position& p)
{
    p.x += 1.0f;
});
```

因此删除：

```text
EcsState
EcsMutation
EcsMutation.hpp forwarding header
SimulationEcsMutation
```

**不提供 compatibility alias/wrapper。**

---

## 38. 删除 Lux Query wrapper

删除整个旧 Query abstraction：

```text
Query.hpp
Read<T>             // old query meaning
Write<T>            // old query meaning
QuerySpec
query(...)
BasicQuery
ChangeRecorder
BoundEcsChangeStream
ChangeStreamBinder
```

原因：

1. 它重复 EnTT view；
2. 当前 iterator 解引用即记录 MODIFIED，即使用户没有真正修改；
3. change tracking 污染普通 Query；
4. System scheduling metadata 与 actual query 被错误地绑在一起。

---

# Part IX — System scheduling metadata 与 EnTT Query 分离

## 39. 新 scheduling tags

为了 TaskGraph hazard，保留极薄的组件访问声明，但不再称为 Query。

建议：

```cpp
template<class Component>
struct ComponentRead final {};

template<class Component>
struct ComponentWrite final {};
```

继续保留：

```cpp
template<class Resource>
struct ExternalRead final {};

template<class Resource>
struct ExternalWrite final {};
```

System：

```cpp
static constexpr inline auto Access =
    makeSystemAccessSpec<
        ComponentRead<Velocity>,
        ComponentWrite<Position>,
        ExternalRead<GameClock>
    >();
```

实现：

```cpp
auto view = registry.view<Position, const Velocity>();
```

二者完全独立。

---

## 40. `SystemAccessSpec.hpp` 不再 include `Query.hpp`

必须直接实现 component access traits。

现有依赖：

```text
SystemAccessSpec -> ecs::Query.hpp -> change-tracking machinery
```

必须消失。

`SystemAccessSpec` 只需要：

```text
component TypeToken/hash
READ/WRITE mode
external TypeToken
```

---

## 41. 删除 `EcsTaskAccess` DSL

当前 `EcsTaskAccess` 同时封装：

```text
Task resource declaration
QuerySpec generation
taskQuery
taskWriter
EcsChangeBatch binding
```

这是重复抽象。

删除：

```text
EcsTaskAccess<T...>
access<T...>
taskQuery(...)
taskWriter(...)
```

对于 System task：

```text
SystemAccessSpec -> TaskGraph resources
```

对于少量非 System infrastructure task：

> 直接使用 canonical ECS TaskResource mapping 函数和 L0 `task::read/write`。

不再建立第二套 typed DSL。

---

## 42. ECS TaskResource 保留范围

删除 change-history resource 后，canonical ECS resources 至少保留：

```text
component storage resource(type hash)
external resource(type)
ECS structure resource
ECS command-buffer resource
```

删除：

```text
ECS changes resource
```

因为 `EcsChangeJournal` 被删除。

### 42.1 Structural safety

任何使用 ECS component storage 的普通 task，转换成 TaskGraph resource 时必须隐式包含：

```text
READ ecsStructureTaskResource
```

结构修改 safe-point task：

```text
WRITE ecsStructureTaskResource
```

这保证 structural flush 与所有 ECS iteration/read/write task 冲突。

不要要求每个 System author 手工声明 structure read。

---

# Part X — Reactive / dirty update 取代通用 ChangeJournal

## 43. 删除通用 observation history

L1 baseline 不再提供：

```text
EcsChangeJournal
EcsChangeHistoryBudget
ChangeCursor<T>
ChangeCursorAccess
ComponentChanges<T>
EntityChangeCursor
EntityChanges
EntityChange
EcsChangeBatch
```

删除相关 internal：

```text
EcsChangeLog
Journal stream/pin machinery
journal reader benchmark
ecsChangesTaskResource
```

原因：

当前主要真实需求是：

> “哪些 Entity/Component 自上次消费后变脏？”

而不是：

> “请保留跨多帧的严格 ordered generic change history。”

---

## 44. Dirty update 使用 EnTT reactive model

典型：

```text
Script / Gameplay
    registry.patch<Position>()
           ↓
    EnTT on_update<Position>
           ↓
    RenderPositionDirty reactive storage
           ↓
    Render extraction updates only dirty entities
           ↓
    clear reactive set
```

每个 consumer 可以拥有自己的 reactive storage：

```text
RenderDirty
NetworkDirty
TransformDirty
```

因此不需要共享 Cursor。

---

## 45. `patch` 是 observable component mutation contract

EnTT 的 `on_update` 不能感知：

```cpp
auto& p = registry.get<Position>(e);
p.x = 10; // no automatic on_update
```

所以：

> 任何希望被 reactive observer 感知的写入，必须通过 `patch/replace/emplace_or_replace` 或 subsystem 明确的 dirty API。

### 45.1 Script 层

Script 层必须利用反射生成 component setter，并默认落到：

```cpp
registry.patch<Component>(entity, ...);
```

例如：

```text
script: position.x = 10
        ↓ reflected setter
registry.patch<Position>(...)
```

如果一次脚本编辑修改多个 field，可在 script bridge 内合并为一次 patch；这是优化，不改变语义。

### 45.2 C++ System

C++ System author 直接使用 EnTT，因此由 API contract 负责正确写入。

若某 System 选择直接 mutable iteration 获得最高吞吐，同时又有 reactive consumer，则它必须显式处理 dirty emission；Lux 不为阻止 C++ author 违反 contract 而重新包一层 registry。

---

## 46. `EntityChanges` 不再存在

Generic entity lifecycle event 通常语义过弱。

Render 真正关心：

```text
Renderable construct/destroy
```

Physics 真正关心：

```text
RigidBody construct/destroy
```

Audio 真正关心：

```text
AudioEmitter construct/destroy
```

各 subsystem 使用 EnTT component construct/destroy signals/reactive sets。

如果未来某个真实 subsystem 确实需要“所有 Entity 创建/销毁”，由该 subsystem 证明需求后再设计；L1 不预先提供。

---

## 47. 什么时候允许重新出现 history

只有出现明确需求：

```text
ordered audit log
true event sourcing
replay
network protocol requiring exact ordered deltas
```

才能在对应 subsystem 中实现 history。

不得重新建立一个“也许以后所有人都能用”的 generic `EcsChangeJournal`。

---

# Part XI — Deferred ECS structural operations

## 48. 为什么仍然需要 deferred structural operations

TaskGraph 解决的是：

```text
不同 task 之间的读写 hazard / concurrency
```

它不能自动解决：

```text
同一个 task 正在迭代 EnTT storage 时修改结构导致 iterator/storage stability 问题
```

所以保留一个 **可选、极简、只负责 structural ECS mutation** 的 command buffer。

它不是所有 Simulation 的组成部分。

---

## 49. 删除旧 command public layering

删除/重命名：

```text
EcsCommandBatch             -> EcsCommandBuffer
EcsCommandRecordingScope    -> DELETE
EcsCommands                 -> EcsCommandWriter
arbitrary Command::apply()  -> DELETE public extension point
```

保留 private implementation 可以使用：

```text
arena
records
type-erased destructor/apply function
producer shards
```

private machinery 数量不是 public over-design。

---

## 50. `EcsCommandWriter` 只暴露 structural operations

目标 API：

```cpp
class EcsCommandWriter final
{
public:
    DeferredEntity create() noexcept;

    void destroy(Entity entity) noexcept;

    template<class Component, class... Args>
    bool emplace(Entity entity, Args&&... args) noexcept;

    template<class Component, class... Args>
    bool emplace(DeferredEntity entity, Args&&... args) noexcept;

    template<class Component>
    bool remove(Entity entity) noexcept;
};
```

禁止：

```cpp
writer.push(MyArbitraryGameplayCommand{}); // FORBIDDEN
```

CommandBuffer 的职责必须固定为：

> 在不安全的 ECS iteration/parallel recording 区间暂存 structural operations，并在 explicit safe point 应用。

普通 component value update 不经过 command buffer；使用 EnTT normal access/patch。

---

## 51. `DeferredEntity`

为支持：

```text
create entity
then add components to that new entity
```

允许新增一个极小 transient token：

```cpp
struct DeferredEntity final
{
    std::uint32_t producer{};
    std::uint32_t ordinal{};
    std::uint32_t generation{};
};
```

规则：

- 非 durable identity；
- 不可序列化；
- 只能属于创建它的 `EcsCommandBuffer` generation；
- 一个 producer 不得直接使用另一个 producer 的 token；
- apply 时 CREATE 首次产生真实 `Entity`，后续同 producer commands 可解析 token；
- buffer reset/prepare 后旧 token invalid。

SHOULD 提供：

```cpp
std::optional<Entity> resolve(DeferredEntity) const noexcept;
```

仅在成功 apply 后、下一次 reset/prepare 前有效。

---

## 52. Command capacity

继续保持最新代码已经修正的原则：

```text
capacity explicit
prepare before hot recording
no hidden foundation reserve
no growth during prepared hot path
```

建议保留类似：

```cpp
struct EcsCommandProducerCapacity
{
    std::size_t max_commands;
    std::size_t max_payload_bytes;
};
```

具体数值来自 concrete Simulation/Product。

---

## 53. Default EnTT-based Simulation 的 flush 语义

默认 built-in EnTT Simulation SHOULD 有一个 terminal structural safe point：

```text
all normal ECS tasks
        ↓
FlushEcsCommands
```

所有普通 component-access task 隐式持有：

```text
READ ecsStructure resource
```

Flush task：

```text
WRITE ecsStructure resource
WRITE ecsCommands resource
```

因此它只能在所有 ECS iteration 结束后运行。

### 53.1 不是所有 Simulation 强制存在

Custom Simulation：

```text
MAY omit command buffer entirely
MAY use direct safe-point structure mutation
MAY have multiple explicit flush points
```

L0 TaskGraph 不自动插入 ECS flush。

Concrete Simulation composition 决定。

---

## 54. Command failure semantics

规则：

```text
recording overflow/failure
    -> no partial apply
    -> discard pending
    -> step reports structured failure

TaskGraph fails before flush
    -> discard pending

apply operation fails in middle
    -> stop
    -> structured apply failure
    -> already-applied canonical mutations are NOT rolled back
```

不要伪造事务语义。

如果 L0 Task callback 不能返回 structured failure：

- flush task 把失败写入 concrete Simulation-owned result slot；
- executor 返回后 concrete Simulation 检查；
- **不得为了这个问题修改 L0 TaskGraph 为 script/ECS 专用 scheduler。**

---

# Part XII — 删除 generic `executeSimulationStep()` 的 ECS 假设

## 55. 当前问题

现有 `executeSimulationStep()` 直接要求：

```text
TaskExecutor
TaskGraph
EcsState
EcsChangeJournal
EcsCommandBatch
```

这相当于定义：

> 所有 Simulation 都必须是这个 ECS runtime pipeline。

与最终架构冲突。

---

## 56. 最终处理

如果 `engine/simulation/core` 仅用于这一个 helper：

> 删除该 ECS-specific generic Simulation step helper；必要时删除空 target。

不新增另一个万能：

```text
SimulationContext
SimulationRunner
SimulationManager
AnySimulation
ISimulation
```

Concrete Simulation 自己拥有：

```text
TaskGraph
TaskExecutor integration
optional Registry
optional CommandBuffer
failure collection
```

L0 `TaskExecutor` 已经是 generic execution mechanism。

---

# Part XIII — Hierarchy 重新定位

## 57. `HierarchySystem` 必须删除

Hierarchy topology/index maintenance 不应伪装成 gameplay/domain System。

删除：

```text
HierarchySystem
HierarchySystem registration requirement
HierarchySystem public scheduling identity
```

理由：

不同 Simulation 可以：

```text
完全不用 hierarchy
使用 Parent 但不需要 reverse index
使用 HierarchyIndex
使用自己的 relation component
使用完全不同的 graph/tree
```

普通 System 不应被迫知道 hierarchy。

---

## 58. `Parent` 的语义

若保留通用 `Parent`：

> 它只是 optional single-parent relation data。

它 **不得隐式意味着**：

```text
transform inheritance
visibility inheritance
lifetime cascading
scene ownership
activation inheritance
network ownership
```

这些都是具体 System 的语义。

如果以后只有 Transform 使用它，则可以重新评估是否应改为 `TransformParent`；当前第一版可以保留 neutral `Parent`，但不得扩张其语义。

---

## 59. `HierarchyIndex`

`HierarchyIndex` 是 optional derived acceleration structure，不是 canonical ECS state。

```text
Parent components = canonical relation data
HierarchyIndex     = derived lookup/traversal acceleration
```

它不得依赖：

```text
EcsChangeJournal
ChangeCursor
Simulation global history
```

维护方式由 concrete Simulation 选择。

---

## 60. Hierarchy maintenance 第一版策略

不要在 L1 再抽象一个通用 hierarchy event/history pipeline。

推荐 built-in Simulation 使用：

```text
explicit Parent mutation helper
and/or EnTT construct/update/destroy reactive set
        ↓
explicit infrastructure maintenance task
        ↓
HierarchyIndex exact before consumers
```

该 maintenance task：

- MAY 是 TaskGraph task；
- MUST NOT 注册成 `SystemRegistry` 中的 `HierarchySystem`；
- 是 concrete Simulation infrastructure。

---

## 61. Hierarchy delta 类型

现有 `HierarchyDeltaBatch` / mutation batches 只有在第二个真实 consumer 证明需要时才保留 public。

目标第一版：

> 尽量把 hierarchy incremental scratch/delta 下沉为 private/internal。

如果 Transform、Editor、其他 subsystem 目前确实共同消费同一个 transient topology delta，则可保留一个 neutral public value type；否则删除。

实施者不得因为现有 benchmark 已经存在就反向证明类型必须存在。

---

# Part XIV — Transform

## 62. Transform 仍然可以是 System

与 Hierarchy maintenance 不同：

> Transform propagation 是具体 domain behavior，作为 System 是合理的。

它可以选择：

```text
flat transform
Parent lookup
HierarchyIndex traversal
other custom relation
```

取决于 concrete Simulation/System implementation。

Transform System 不得使 hierarchy 成为所有 Simulation 的 mandatory service。

---

# Part XV — Script / behavior

## 63. 不再设计单一 `ScriptSystem`

删除/禁止架构假设：

```text
one ScriptSystem
one fixed position in frame
all script callbacks happen there
```

脚本本质上：

> 与一个具体 Simulation 的 execution points、events、capabilities 高度绑定的 behavior runtime。

因此 L1 foundation 不创建 universal `ScriptSystem`。

---

## 64. 脚本绑定到什么

脚本可以绑定：

```text
System execution point
System intrinsic event
```

例如：

```text
physics / before_step
physics / after_step
physics / collision_begin
animation / marker
animation / finished
```

Editor 读取 `SimulationDescription` 即可知道这个 Simulation 暴露了什么。

---

## 65. Script binding data 放在哪里

L1 **不新增**：

```text
ScriptHookDescriptor
ScriptBindingRegistry
UniversalBehaviorDescription
```

一个 concrete scripting-enabled Simulation 可以把：

```text
script asset references
function bindings
behavior class data
marker bindings
```

编码进现有 generic：

```text
SimulationDataSchemaId + version + payload
```

例如 domain schema：

```text
lux.scripting.lua.bindings
mygame.behavior-bindings
```

因此 `SimulationDescription` 仍然是完整资产，而 L1 不被某种脚本语言绑死。

---

## 66. Reflection 与 component patch

脚本 bridge 通过 Lux reflection 为 component 生成 setter。

默认 setter 必须使用：

```cpp
registry.patch<Component>(...)
```

这样 `on_update` / reactive dirty mechanism 一致生效。

不得把长期 mutable component pointer/reference 暴露给脚本作为默认写接口。

---

## 67. Dynamic script concurrency

脚本执行时用户可能动态访问任意 component/resource。

第一版默认：

> dynamic script callbacks 不与可能访问同一 Simulation mutable state 的普通 Systems 并行。

不要尝试静态推断任意脚本访问集。

Concrete Simulation 必须通过 execution point dependencies / safe boundary 保证。

未来只有真实 profiling 证明需要时，才增加 opt-in script access declaration。

---

# Part XVI — Physics 与 Script 时序

## 68. Physics query 与 Physics event 必须分开

Script：

```text
“现在 raycast/overlap 的结果是什么？”
```

属于 query。

```text
“这一个 Physics step 是否产生 CollisionBegin？”
```

属于 event。

不得用重新执行一次碰撞检测来模拟 physics event。

---

## 69. Physics query snapshot semantics

Physics query 查询：

> 当前 PhysicsWorld 已经同步完成的 snapshot。

如果 ECS Transform 刚修改但还没执行 ECS->Physics sync：

```text
query sees previous synced physics state
```

不得隐藏：

```text
automatic physics resimulation
automatic SyncTransforms before every query
```

需要新状态时，由 Simulation ordering 明确：

```text
ECS mutation
   ↓
PhysicsInputSync
   ↓
query point
```

---

## 70. Physics script points 示例

PhysicsSystem 可声明：

```text
before_step
after_step
```

事件：

```text
collision_begin -> after_step
collision_end   -> after_step
trigger_enter   -> after_step
```

于是：

```text
script bound to before_step
    may set forces / desired velocity / perform predictive raycast

PhysicsStep

collision events dispatch

script bound to after_step
    reacts to completed physics result
```

不需要一个全局 `ScriptSystem after Physics`。

---

# Part XVII — Animation Event

## 71. Animation Marker 是事件，不是脚本函数名

Animation asset 应描述 semantic marker：

```text
frame/time 37
marker = gameplay.footstep
```

不得直接耦合：

```text
Player.lua
function OnFootstep
```

AnimationSystem 运行时产生：

```text
AnimationMarkerEvent
    entity
    marker id
    optional normalized time / clip info
```

并在自身声明的 dispatch point 发送。

Script binding table再把：

```text
marker gameplay.footstep
    -> script function
```

关联起来。

这允许同一个 animation asset 在：

```text
GameSimulation
EditorPreviewSimulation
CinematicSimulation
```

有不同解释。

---

# Part XVIII — LuxObject Event/Signal 的使用边界

## 72. LuxObject `Signal` 可复用，但不是 Description identity

现有 LuxObject 已经提供：

```text
typed Signal<Owner, Payload>
Connection
DIRECT / QUEUED / AUTO
dynamic reflected observe
thread affinity
```

这些能力可以用于 runtime behavior/event delivery。

但是 `SimulationDescription` 不保存：

```text
SignalView
SignalDescriptor pointer
Signal owner object pointer
reflected field address
```

Description 的 canonical event identity 是：

```text
system instance + event name + payload schema
```

LuxObject Signal 是 runtime transport。

---

## 73. 不建立第二个 EventBus

禁止：

```text
SimulationEventBus
GlobalEventBus
ScriptEventBus
EventDispatcher with string->callback map
runtime registerEvent(name)
```

如果 concrete built-in System 使用 LuxObject Signal，则直接复用它。

---

## 74. Simulation synchronous behavior 默认 DIRECT

对于 deterministic synchronous Simulation callback：

```text
EDelivery::DIRECT
```

不得把：

```text
QUEUED / AUTO
```

作为 gameplay Simulation callback 的默认行为。

`QUEUED/AUTO` 可以用于：

```text
Simulation -> Editor UI observation
background tool -> UI
non-deterministic tooling
```

但这不属于 synchronous Simulation behavior semantics。

---

## 75. Worker 不直接调用用户脚本

错误：

```text
Physics worker detects contact
    ↓
Signal notify
    ↓
Lua callback immediately
```

正确：

```text
parallel producer
    ↓
transient event buffer
    ↓
quiescent declared dispatch point
    ↓
DIRECT Signal / script bridge
```

这样避免：

```text
solver reentrancy
cross-thread Lua
nondeterministic callback order
ECS structure mutation during worker iteration
```

---

## 76. Dynamic reflected callback 必须 noexcept-safe

在本两次重构中同步完成：

`RefMethod` MUST 增加：

```cpp
bool is_noexcept;
```

Dynamic LuxObject observe 在连接前必须拒绝 throwing callback：

```text
METHOD_MUST_BE_NOEXCEPT
```

不要在 callback 真正抛出后才 `terminate`。

脚本 VM 错误必须被 C++ noexcept bridge 捕获并转换为 ScriptRuntime 自己的 failure/diagnostic。

---

# Part XIX — SystemRegistry 清理

## 77. SystemRegistry 保留

`SystemRegistry` 仍然有价值：

```text
heterogeneous concrete System ownership
SystemLease lifetime
module/code lifetime retention
runtime SystemId
```

它不是 scheduler。

它不编译 TaskGraph。

---

## 78. `System` concept 增加 Description requirement

现有 concept 只检查 `Type::Access`。

最终 concept SHOULD 等价于：

```text
has trusted Type::Access
has valid static Type::Description
nothrow destructible
```

静态校验：

```text
SystemDescription canonical name valid
version != 0
capabilities valid/unique
points valid/unique
events valid/unique
event dispatch points valid
config schema internally valid
```

---

## 79. System construction failure 分类

`SystemRegistry::emplace` 不得：

```text
catch (...) -> ALLOCATION_FAILURE
```

必须区分至少：

```text
std::bad_alloc -> ALLOCATION_FAILURE
other constructor exception -> CONSTRUCTION_FAILURE
```

System destructor 必须 nothrow。

---

# Part XX — Runtime interpretation / code binding

## 80. Description 完整不等于建立 universal System factory registry

保持原合同的谨慎原则：

> 本 L1 closure 不因为资产出现 SystemTypeId 就自动创建 `SimulationProviderRegistry / AnySystemFactory / GlobalSystemFactoryRegistry`。

Product/domain composition 负责：

```text
SystemTypeId
    -> currently loaded System implementation
```

如果未来至少两个真实 module 需要稳定 plugin ABI，再单独设计 provider registry。

---

## 81. Load-time static-description compatibility validation

加载后的 `SimulationDescription` 在构造 runtime System 前必须验证当前代码与资产记录兼容。

对于一个具体 `System Type` 至少比较：

```text
canonical type name/hash
System description version
configuration schema name/version
capability set
execution point names/order
event names/order
event dispatch points
event payload schema names/versions
```

注意：

> 资产 metadata 是 authored durable truth；当前 C++ static description 是当前代码 truth。

不匹配时：

```text
fail structured
or explicit migration
```

不得 silently ignore removed event/point。

---

## 82. Missing code module

Editor/asset inspection：

```text
MUST still be able to read SimulationDescription
```

即使当前 executable 没有加载对应 System implementation。

Runtime Simulation construction：

```text
MUST fail clearly: implementation unavailable
```

不要让 Asset codec 强制 load plugin。

---

# Part XXI — Snapshot

## 83. `EcsSnapshot` 适配 direct Registry

`EcsSnapshot` 仍可作为 optional EnTT Simulation structural utility。

其 API 应从：

```text
EcsState
```

迁移到：

```text
ecs::Registry
```

仍然只负责：

```text
entity/component structural state
registered snapshot component types
```

不负责：

```text
Physics state
Script VM
RNG
Simulation clock
Event buffers
Dirty sets
Hierarchy policy
```

未来 `SimulationSnapshot` 是不同概念。

---

# Part XXII — 最终项目结构

## 84. 推荐目录

```text
engine/simulation/
│
├── description/
│   ├── include/lux/engine/simulation/
│   │   ├── SimulationDataSchemaId.hpp
│   │   ├── SystemTypeId.hpp                    [NEW]
│   │   ├── SystemExecutionPoint.hpp            [NEW]
│   │   ├── SystemEventDescription.hpp          [NEW]
│   │   ├── SystemDescription.hpp               [NEW]
│   │   ├── SimulationStepInfo.hpp              [NEW]
│   │   ├── SimulationDescription.hpp           [EXTEND]
│   │   └── SimulationDescriptionBuilder.hpp    [EXTEND]
│   └── ...
│
├── asset/
│   ├── SimulationAssetCodec.hpp
│   └── SimulationAssetCodec.cpp                [FORMAT UPGRADE]
│
├── system/
│   ├── SystemConcept.hpp                       [EXTEND]
│   ├── SystemAccessSpec.hpp                    [REMOVE Query dependency]
│   ├── SystemRegistry.hpp
│   ├── SystemId.hpp
│   └── SystemError.hpp
│
├── ecs/
│   ├── core/
│   │   ├── Entity.hpp                          [KEEP]
│   │   ├── Registry.hpp                        [NEW thin alias]
│   │   ├── EcsCommands.hpp                     [REWRITE/SIMPLIFY]
│   │   ├── EcsTaskResources.hpp                [SIMPLIFY]
│   │   └── ... private command storage
│   │
│   ├── task/
│   │   ├── SystemTaskResources.hpp             [SIMPLIFY]
│   │   └── no EcsTaskAccess DSL
│   │
│   ├── snapshot/
│   │   └── Registry-based snapshot
│   │
│   ├── hierarchy/
│   │   ├── Parent.hpp
│   │   └── HierarchyIndex.hpp
│   │       no HierarchySystem
│   │
│   └── transform/
│       └── concrete transform Systems/utilities
│
└── core/
    └── REMOVE if only ECS-specific executeSimulationStep remains
```

不要为了文件对称性保留空 target。

---

# Part XXIII — 必须删除的旧 public 类型/文件

## 85. 删除清单

以下属于最终目标明确删除项；不要留 deprecated alias/forwarder：

### ECS state/query/change

```text
EcsState
EcsMutation
EcsMutation.hpp
SimulationEcsMutation

Query.hpp
Read<T>              old query tag
Write<T>             old query tag
QuerySpec
query()
BasicQuery
ChangeRecorder
BoundEcsChangeStream
ChangeStreamBinder

ChangeCursor<T>
ChangeCursorAccess
ComponentChanges<T>
EntityChangeCursor
EntityChange
EntityChanges
EcsChangeJournal
EcsChangeHistoryBudget
EcsChangeBatch
```

### ECS task wrapper

```text
EcsTaskAccess<T...>
access<T...>
taskQuery
taskWriter
ECS changes TaskResource
```

### Commands old public layers

```text
EcsCommandBatch        old name/surface
EcsCommandRecordingScope
EcsCommands            old nested handle
public arbitrary Command::apply extension
```

### Hierarchy

```text
HierarchySystem
```

### Simulation execution

```text
FrameInfo
ECS-specific generic executeSimulationStep()
```

### 禁止创建的新重复抽象

```text
SimulationContract
SimulationContractBuilder
SimulationEventBus
ScriptHookDescriptor
ScriptHookRegistry
ScriptSystem as universal phase owner
ISimulation
SimulationBase
SystemBase
ISystem
SimulationContext service bag
SystemContext service bag
GlobalSystemFactoryRegistry
EventJournal
EventCursor
EventEpoch
```

---

# Part XXIV — 新增/保留 public 类型总表

## 86. 新增

```text
SimulationStepInfo
SystemTypeId
SystemExecutionPoint
SystemEventDescription
SystemDescription
SimulationSystemView
SimulationExecutionPointView
SimulationEventView
SimulationDependencyView

ecs::Registry              thin alias
ComponentRead<T>
ComponentWrite<T>

EcsCommandBuffer
EcsCommandWriter
DeferredEntity
```

根据实现命名可以非常轻微调整，但不得额外分裂为多层 descriptor/registry。

---

## 87. 保留

```text
SimulationDescription
SimulationDescriptionBuilder
SimulationDataSchemaId
SimulationDataView
Simulation asset codec descriptor

System
SystemRegistry
SystemLease
SystemId
SystemAccessSpec
ExternalRead<T>
ExternalWrite<T>

ecs::Entity
EcsSnapshot / snapshot schema utilities
HierarchyIndex / Parent if still used
TaskGraph / TaskExecutor
LuxObject Signal / Connection / reflection
```

---

# Part XXV — 两次重构的严格实施顺序

## 88. Refactor 1 — ECS Simplification + System Self-Description Foundation

**目标：先把 runtime/API 模型变简单；不在同一个 commit 中同时重写全部资产格式。**

### 88.1 Phase A — Contract update / compile boundary

1. 将本文加入仓库 `doc/` 或 architecture docs；
2. 修改原 canonical contract 中冲突段落；
3. architecture tests 更新：允许/要求 EnTT direct API；
4. 不开始 L2 Process。

### 88.2 Phase B — System metadata foundation

新增：

```text
SimulationStepInfo
SystemExecutionPoint
SystemEventDescription
SystemDescription
SystemTypeId
ComponentRead/ComponentWrite
```

更新：

```text
SystemConcept
SystemAccessSpec
SystemRegistry error semantics
RefMethod noexcept metadata / dynamic observe validation
```

先写 compile tests，再迁移 production callers。

### 88.3 Phase C — Direct EnTT

新增：

```text
ecs::Registry alias
```

迁移所有 production code：

```text
EcsState -> Registry
EcsMutation -> direct Registry safe-point operations
Lux Query -> EnTT view/group
observable updates -> patch
```

### 88.4 Phase D — Remove change-history stack

删除：

```text
Journal/Cursor/Changes/ChangeBatch/SimulationEcsMutation
```

对应 consumer 改为：

```text
EnTT reactive storage
component on_construct/on_update/on_destroy
or subsystem-local explicit dirty set
```

### 88.5 Phase E — Simplify command buffer

重构：

```text
EcsCommandBuffer
EcsCommandWriter
DeferredEntity
```

去 Journal coupling。

保留/重做 allocation-free prepared benchmark。

### 88.6 Phase F — Hierarchy cleanup

删除 `HierarchySystem`。

保留 `Parent/HierarchyIndex` only if current consumers need them。

把 maintenance 变为 concrete Simulation infrastructure。

### 88.7 Phase G — Remove generic ECS SimulationExecution

删除或下沉 `executeSimulationStep()` 的 ECS-specific assumptions。

如果 `simulation/core` 变空，删除 target。

### 88.8 Refactor 1 结束条件

必须：

```text
all production targets compile
all tests pass
installed consumer can use direct EnTT Registry
System self-description compile/static validation works
no old Query/Journal headers installed
command hot path still prepared/no-growth
no HierarchySystem
no generic ScriptSystem introduced
```

此时不要宣称 L1 freeze；继续 Refactor 2。

---

## 89. Refactor 2 — Complete SimulationDescription + Asset vNext

### 89.1 Extend in-memory Description

加入：

```text
systems
per-system config
capabilities
execution points
events
dependencies
```

保持已有 global data。

### 89.2 Builder materialization

实现：

```text
addSystem(instance, SystemDescription, config)
eraseSystem
set config
dependency operations
full validation
cycle detection
```

### 89.3 Asset codec format upgrade

升级 LXSD format version。

实现新的 tables / strings / payload layout。

删除 pre-freeze old-format compatibility unless explicit request。

### 89.4 Runtime compatibility validation

至少提供 internal/templated mechanism，使 concrete product 可以：

```text
loaded SimulationSystemView
    vs
CurrentSystem::Description
```

做 exact semantic validation。

不要顺手创建 global factory registry。

### 89.5 LuxObject integration proof

增加至少一个真实 test System：

```text
execution point
intrinsic event
LuxObject typed Signal runtime delivery
reflected noexcept receiver
DIRECT connection
```

证明：

```text
Description does not store Signal pointer
runtime can use LuxObject transport
payload reflection matches
```

### 89.6 Script/animation/physics example fixture

测试 fixture SHOULD 模拟：

```text
PhysicsSystem
    before_step
    after_step
    collision_begin event

AnimationSystem
    before_evaluate
    after_evaluate
    marker event

SimulationDescription dependency:
    physics.after_step -> animation.before_evaluate
```

并验证 Editor-style enumeration 输出足够描述：

```text
has physics.simulation
has animation.playback
points discoverable
events discoverable
payload schemas discoverable
```

### 89.7 Refactor 2 结束条件

完整 asset roundtrip：

```text
build description
encode
fresh decode
compare all semantic fields
validate against static System descriptions
build installed consumer
```

之后才能重新开始 L1 exact-SHA freeze qualification。

---

# Part XXVI — 测试矩阵

## 90. Static SystemDescription tests

必须覆盖：

```text
valid description compiles
empty canonical name rejected
version 0 rejected
invalid config schema rejected
duplicate capability rejected
empty capability rejected
duplicate point rejected
empty point rejected
duplicate event rejected
event references missing point rejected
invalid payload schema rejected
System with throwing destructor rejected
```

尽可能使用 compile-negative tests 固定 public contract。

---

## 91. SimulationDescriptionBuilder tests

```text
empty description valid
add one System
multiple System types
multiple instances same type
duplicate instance name rejected
per-system config retained exactly
capabilities materialized
points materialized
events materialized
event payload schema retained
add valid dependency
duplicate dependency rejected
unknown system rejected
unknown point rejected
cycle rejected
erase System cleans dependencies
move semantics
allocation failure
size overflow
```

---

## 92. Asset codec tests

```text
roundtrip empty
roundtrip global data only
roundtrip systems only
roundtrip system config
roundtrip capabilities
roundtrip execution points
roundtrip events
roundtrip dependencies
roundtrip mixed realistic description

bad magic
bad version
truncated table
offset overflow
payload overflow
string overflow
hash/name mismatch
duplicate system
bad event point reference
bad dependency reference
cycle encoded in file
budget exceeded
```

---

## 93. Direct EnTT tests

Installed/public consumer 必须证明：

```cpp
ecs::Registry registry;
const auto e = registry.create();
registry.emplace<Position>(e);

auto view = registry.view<Position>();
registry.patch<Position>(e, ...);
```

不得要求 include Lux Query/EcsMutation。

---

## 94. Reactive dirty tests

至少：

```text
patch Position -> RenderDirty receives entity
direct read -> no dirty
consumer clear -> empty
multiple patch same entity -> reactive semantics stable
multiple independent reactive consumers -> each gets notification
component destroy -> subsystem-specific destroy observer works
```

并明确测试：

> 直接 `T&` field mutation 不会神奇触发 update；这是 documented contract，不要写 wrapper 去猜。

---

## 95. Command tests

```text
prepare exact capacities
record no allocation
record overflow -> failed
no partial apply after recording failure
destroy existing entity
remove component
emplace existing entity
create DeferredEntity
emplace onto DeferredEntity
resolve after success
old token invalid after reset
cross-producer token misuse rejected
TaskGraph failure -> pending discarded
terminal flush ordered after ECS tasks
apply failure -> structured, no rollback claim
```

---

## 96. Hierarchy tests

```text
no HierarchySystem type/header
Simulation can build with no hierarchy
Parent/HierarchyIndex usable as optional utility
HierarchyIndex has no Journal include/dependency
hierarchy maintenance can be scheduled explicitly
flat System has zero hierarchy dependency
```

---

## 97. LuxObject binding tests

```text
correct noexcept handler connects
wrong payload type rejected
wrong return type rejected
throw-capable reflected method rejected before connection
DIRECT cross-affinity rejected
event generated on worker is not delivered until safe dispatch point
occurrence ordering deterministic
execution point callback occurs after events for that point
```

---

# Part XXVII — Benchmark / Qualification 更新

## 98. 删除已经失去架构意义的 qualification metrics

随着设计删除，对应 benchmark 不应为了“证据连续性”保留：

```text
EcsChangeBatch recording scaling
Journal concurrent readers
HierarchyDelta benchmark（若 public delta 删除）
```

旧 evidence 仍作为历史记录，但不能作为新 SHA freeze 证据。

---

## 99. 保留/新增结构性性能证据

至少保留：

```text
TaskGraph execution scaling
EcsCommandBuffer prepared recording scaling
World description build/lookup/partition
EcsSnapshot structural capture if retained
```

新增建议：

### 99.1 Reactive dirty

```text
100k / 1m patch notifications
prepared reactive storage
no per-update heap allocation
```

只测实际需要的真实 consumer pattern，不为了替代 Journal 而制造复杂 benchmark framework。

### 99.2 Direct event dispatch

setup：

```text
all Connections created
reflection resolved
payload/event buffers prepared
```

measurement：

```text
100k / 1m typed DIRECT dispatch
```

结构规则：

```text
0 reflection lookup per occurrence
0 string lookup per occurrence
0 allocation per occurrence
callback_count == event_count
```

---

# Part XXVIII — Installed consumer

## 100. 必须新增/更新 installed consumer

Fresh install prefix consumer 必须覆盖：

```text
SimulationDescription
SystemDescription
SystemExecutionPoint
SystemEventDescription
SystemTypeId
SimulationSystemView/EventView/PointView
asset codec descriptor
SystemRegistry
ComponentRead/ComponentWrite
EnTT Registry direct use
EcsCommandBuffer
LuxObject reflected event binding
```

第二次 build 必须 no-work。

---

# Part XXIX — LLM 实施禁止事项

## 101. 禁止“为了方便”新增的设计

实施者不得新增：

```text
SimulationContract
SimulationContractBuilder
SimulationManager
SimulationContext
SystemContext
WorldContext
ServiceLocator
AnySystem
AnySimulation
ISimulation
SystemBase
ISystem
ScriptSystem universal phase
SimulationEventBus
EventJournal
ChangeJournal replacement with another generic log
ObserverManager
DirtyManager
HierarchySystem replacement with HierarchyManager
global ESimulationPhase enum
global ESimulationCapability enum
runtime string event dispatch
runtime dynamic registerExecutionPoint
runtime dynamic registerEvent
```

---

## 102. 禁止 compatibility architecture

本次 L1 尚未 freeze，旧 API 删除后：

```text
不要 alias
不要 forwarding header
不要 deprecated wrapper
不要 hidden compatibility target
```

例如：

```cpp
using EcsState = Registry;                    // FORBIDDEN
using EcsCommandBatch = EcsCommandBuffer;     // FORBIDDEN
using FrameInfo = SimulationStepInfo;         // FORBIDDEN
```

旧调用方直接迁移。

---

## 103. 禁止把 runtime implementation 写进 asset

绝不能序列化：

```text
C++ pointer
function pointer
Signal descriptor address
TaskGraph node id
SystemId
Entity
registry address
module handle
```

如果实施者发现自己需要这些值，说明 Description/runtime 边界被破坏，应停止该方案。

---

## 104. 禁止 hidden Product policies

不得在新增：

```text
System count
Event count
Script binding count
Command count
Command bytes
Asset bytes
Reactive dirty entries
```

相关 foundation API 中写：

```text
65536
1 MiB
64 MiB
1024 systems
```

等“看起来合理”的默认上限。

容量必须由 concrete Simulation / Product / codec context 提供。

---

# Part XXX — 建议 commit 切分

## 105. Refactor 1 commits

建议最小可审计切分：

```text
1. docs(simulation): adopt final L1 simulation refactor contract

2. refactor(simulation): add System static semantic descriptions

3. refactor(system): decouple scheduling access from ECS Query

4. refactor(ecs): expose direct EnTT Registry and remove Lux Query

5. refactor(ecs): retire generic change journal and cursor stack

6. refactor(ecs): simplify deferred structural command buffer

7. refactor(hierarchy): remove HierarchySystem from System model

8. refactor(simulation): remove ECS-specific generic step helper

9. fix(object/system): close noexcept and construction failure contracts

10. test(l1): qualify refactor-1 public boundaries
```

不要把所有内容 squash 成一个无法审计的大 commit。

---

## 106. Refactor 2 commits

```text
1. feat(simulation): materialize Systems into SimulationDescription

2. feat(simulation): persist capabilities execution points and events

3. feat(simulation): persist cross-system execution dependencies

4. feat(simulation): add strict Description builder validation

5. refactor(simulation-asset): upgrade LXSD format to complete description

6. test(simulation): add full asset roundtrip and malformed-image matrix

7. test(simulation): prove LuxObject runtime event integration

8. test(install): add complete SimulationDescription consumer

9. perf(l1): refresh qualification metrics for new architecture

10. test(l1): publish final exact-sha freeze candidate evidence
```

---

# Part XXXI — Canonical Contract 需要同步修改的章节

## 107. 必须修订原合同的核心内容

实施完成前，原 canonical contract 至少应做以下语义替换。

### 107.1 原 §19 `SimulationDescription` minimal typed data model

替换为：

```text
SimulationDescription is the complete immutable serializable definition of one Simulation.
It contains global typed data, materialized System instances, per-System configuration,
capabilities, System-declared execution points, System-declared intrinsic events, and
cross-System execution dependencies.

It does not contain runtime objects or a compiled TaskGraph.
```

### 107.2 原 §21 `EcsState final contract`

替换为：

```text
L1 does not wrap EnTT with a parallel registry/query/mutation object model.
An EnTT-based Simulation owns an ecs::Registry alias and concrete Systems may use EnTT directly.
```

### 107.3 原 §23 ECS task bridge

改为：

```text
one scheduling metadata path:
System ComponentRead/ComponentWrite/ExternalRead/ExternalWrite
    -> canonical TaskGraph resources

No EcsTaskAccess Query/TaskWriter DSL.
```

### 107.4 原 §24 ChangeJournal

删除 baseline requirement。

替换为：

```text
Dirty/reactive update is subsystem-owned and normally uses EnTT patch/signals/reactive storage.
Ordered history is not an L1 generic ECS facility.
```

### 107.5 原 §25 Deferred commands

改为：

```text
optional structural command buffer for EnTT-based Simulations;
not coupled to Journal;
structural operations only;
concrete Simulation chooses safe points.
```

### 107.6 原 §27 Hierarchy

删除 `HierarchySystem`。

改为：

```text
Parent optional relation data
HierarchyIndex optional derived acceleration
maintenance is concrete Simulation infrastructure
Transform behavior remains separate
```

### 107.7 Summary

旧：

```text
Simulation = typed rule/config facts + Systems + explicit ChangeJournal/Commands/Snapshot
```

新：

```text
SimulationDescription = complete durable Simulation definition
System = reusable self-described synchronous behavior unit
ECS = optional EnTT-based implementation mechanism
Dirty observation = reactive/subsystem-owned
Deferred structure = optional explicit safe-point utility
Events/ExecutionPoints = System-declared, Description-materialized semantic anchors
```

---

# Part XXXII — 最终 L1 freeze gate

## 108. 任何 production change 都使旧 exact-SHA qualification 失效

当前 branch evidence：

```text
HEAD evidence commit:
673b3d17b0324b9c0a57a9316dab0a461f5bfd91

qualified production/test commit:
d6754945c9dbb8d581f6b46fd46e93e69be58247
```

本规范实施后它们只作为历史证据。

必须针对新的最终 production SHA 全量重跑。

---

## 109. Final freeze checklist

### Architecture

```text
[ ] World has zero ECS ownership
[ ] SimulationDescription is complete durable definition
[ ] no SimulationContract duplicate model
[ ] Systems self-describe points/events/capabilities
[ ] Description materializes System metadata
[ ] no runtime pointer/task id in asset
[ ] one generic scheduler only: TaskGraph
[ ] no universal ScriptSystem
[ ] no HierarchySystem
[ ] no generic ECS change history
```

### ECS

```text
[ ] direct EnTT public consumer works
[ ] no EcsState/EcsMutation/Query wrapper remains
[ ] observable script setters use patch
[ ] reactive dirty consumer works
[ ] structural command buffer is optional and Journal-free
[ ] terminal safe-point ordering proven
[ ] snapshot adapted to Registry
```

### Description / Asset

```text
[ ] systems roundtrip
[ ] config roundtrip
[ ] capability roundtrip
[ ] execution points roundtrip
[ ] events roundtrip
[ ] payload schemas roundtrip
[ ] dependencies roundtrip
[ ] malformed/cycle/bounds tests pass
[ ] Product codec budgets explicit
```

### System/Object safety

```text
[ ] constructor exception != bad_alloc correctly classified
[ ] System destructor nothrow contract
[ ] dynamic reflected Signal handler noexcept validated before connect
[ ] DIRECT cross-affinity contract tested
```

### Build matrix

```text
[ ] Windows Debug full build/test
[ ] Windows RelWithDebInfo full build/test
[ ] hardened contract configuration
[ ] Android arm64-v8a PLAYER
[ ] fresh install
[ ] all installed consumers
[ ] second builds no-work
```

### Performance / structural evidence

```text
[ ] TaskGraph scaling
[ ] EcsCommandBuffer prepared recording
[ ] reactive dirty representative workload
[ ] direct event dispatch representative workload
[ ] World qualification retained
[ ] Snapshot qualification retained if Snapshot remains public
[ ] zero hot-path allocations where contract requires
[ ] zero per-event reflection/string lookup in typed hot dispatch
```

### Freeze governance

```text
[ ] evidence identifies exact production SHA
[ ] architecture review uses same SHA
[ ] installed consumer uses fresh prefix from same SHA
[ ] independent public API acceptance
[ ] old pre-refactor evidence not mixed with new evidence
```

只有全部满足后：

```text
L1 = FROZEN
```

然后才允许：

```text
formal L2 Process implementation/public dependency
```

---

# Part XXXIII — 最终公共心智图

## 110. 最终架构

```text
                              Scene (L3)
                                 │
                     selects / composes assets
                                 │
             ┌───────────────────┴───────────────────┐
             │                                       │
      WorldDescription                         SimulationDescription
        durable facts                         durable rules definition
                                                     │
                                                     │ interpreted by
                                                     ▼
                                           concrete Runtime Simulation
                                                     │
                                   ┌─────────────────┼─────────────────┐
                                   │                 │                 │
                               Systems           TaskGraph          resources
                                   │                                   │
                    self-described semantics                         optional
                 ┌───────────────┼───────────────┐       ┌───────────┼───────────┐
                 │               │               │       │           │           │
            capabilities   execution points     events   EnTT      Physics    Animation
                                                        Registry    World       runtime
                                                           │
                                      ┌────────────────────┼────────────────────┐
                                      │                    │                    │
                                   patch              reactive sets      deferred structure
                                      │                    │                    │
                                      └──────── dirty consumers ────────────────┘
```

---

# Part XXXIV — 最终一句话规范

## 111. Freeze sentence

> **World 描述存在什么；SimulationDescription 完整描述这个 World 应如何被某种 Simulation 解释和运行；System 是可复用且自描述的同步行为单元，它自己声明 capability、execution point 与 intrinsic event；TaskGraph 只负责调度；选择 EnTT 的 Simulation 直接使用 EnTT，并以 patch/reactive storage 实现脏更新，以可选 deferred structural command buffer 解决结构修改 safe point；脚本绑定到具体 System 的 execution point/event，而不是由一个全局 ScriptSystem 决定整帧脚本时序。**

---

# Appendix A — 当前仓库基线事实（编写本规范时）

本规范编写时重新确认：

```text
branch: codex/object-ui-foundation
HEAD:   673b3d17b0324b9c0a57a9316dab0a461f5bfd91
parent: d6754945c9dbb8d581f6b46fd46e93e69be58247
```

当前代码已经在上一轮 closure 中修复/强化了多项问题，包括 explicit command capacity、hierarchy 与 journal 解耦方向、structured command apply failure、asset/serialization capacity policy 等。

本次重构不是因为这些修复无效，而是因为最终架构已经进一步明确：

```text
不要把为旧 ECS runtime framework 修好的机制继续误当成未来必须保留的公共抽象。
```

例如当前 `EcsChangeJournal` 即使并发与 capacity 已经实现正确，仍然可以因为**产品语义上不再需要 generic history**而被删除。

正确实现 != 必须存在。

---

# Appendix B — LLM 最终执行口令

可将以下内容直接放入实现任务最前方：

> Implement exactly the architecture in `Lux_Engine_L1_Simulation_Final_Refactor_Implementation_Spec_2026-08-26.md`.
>
> This is a pre-freeze breaking cleanup. Remove obsolete public types completely; do not preserve compatibility aliases or forwarding headers.
>
> Do not invent missing public abstractions. In particular, do not create SimulationContract, ScriptSystem, SimulationEventBus, generic event history, a service-locator context, a second scheduler, or a global System factory registry.
>
> `SimulationDescription` is the complete immutable serializable Simulation definition. `SystemDescription` is a static code-side declaration that is materialized into it. Systems own their capability/execution-point/intrinsic-event declarations. Cross-System ordering belongs to SimulationDescription.
>
> EnTT is intentionally exposed to C++ System authors. Remove Lux Query/EcsState/EcsMutation/ChangeJournal wrappers as specified. Scheduling metadata remains separate from EnTT queries.
>
> Use `registry.patch` for writes that must trigger reactive dirty observation. Do not recreate a generic change journal under another name.
>
> Deferred ECS commands are optional structural operations only and must not carry arbitrary gameplay callbacks or depend on change history.
>
> If an implementation detail is not specified and cannot be kept private, stop and report the gap instead of creating a new public architecture.
