# Lux Engine L1 SystemHook / SystemEvent / Script Binding 最终实施规范

**文档性质：Normative Implementation Specification（规范性实施文档）**  
**日期：2026-08-27**  
**目标仓库：`LUX-YU/lux-engine`**  
**目标分支：`codex/object-ui-foundation`**  
**编写时分支 HEAD：`99f984cf0eb0ee75f45d21a5f1dd5d7c60c4b9be`**  
**当前 production parent：`8afeb80496a6ccd69494e9848714884b84f525cd`**  
**被本文修订的上一版实施规范：`doc/l1-simulation-final-refactor-implementation-spec.zh-CN.md` / `Lux_Engine_L1_Simulation_Final_Refactor_Implementation_Spec_2026-08-26.md`**

---

# 0. 文档优先级、适用范围与实施者须知

本文是在 2026-08-27 对 `SystemExecutionPoint`、`SystemEventDescription`、Script Asset、Script Description、Entity Script、Global Script、FlowForge/MLIR、Lua/C++/未来 Python 以及脚本热路径重新校准语义后形成的 **L1 freeze 前最终修订规范**。

对于本文覆盖的主题，本文优先级高于上一版 `L1 Simulation 最终重构实施规范`。上一版中未被本文明确修订的内容继续有效，尤其是：

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
Product/Host owns capacities and budgets.
Asset payload remains immutable Description data.
No compatibility architecture is added merely to preserve pre-freeze call sites.
No global service locator / manager / generic event bus is introduced for convenience.
```

## 0.1 本文明确废弃的上一版理解

以下理解从本文起视为 **错误或已废弃**，实施者不得继续实现：

1. `SystemExecutionPoint` 是 TaskGraph/scheduler 节点；
2. `(System instance, ExecutionPoint)` 是跨 System dependency DAG 的节点；
3. `SimulationDependency` 应以 `System + ExecutionPoint -> System + ExecutionPoint` 表达调度关系；
4. `SystemA.before -> Script.run -> SystemA.after` 是 ExecutionPoint 的核心语义；
5. Event 的 `BROADCAST` 可以表示扫描全部 Entity 后寻找 ScriptComponent；
6. 按函数名自动匹配/自动绑定 Hook/Event 是 runtime contract；
7. 一个统一 `ScriptSystem` 应在固定帧阶段执行全部脚本；
8. 一个进程全局 `ScriptEventRegistry` 应成为所有 System 的事件/脚本入口目录；
9. runtime 每次脚本调用允许做 reflection、name lookup、hash lookup、asset lookup 或 backend virtual dispatch；
10. ScriptDescription 的 backend-specific function table 可以继续各自拥有不同结构，而 Editor/Simulation 根据语言特殊处理。

## 0.2 本文的最终语义一句话

```text
Script describes callable code.
Simulation describes System-owned semantic hooks and events.
Composition describes bindings.
Reflection validates bindings.
Binding prepares final calls.
System hot paths only invoke prepared calls.
```

更短：

```text
Authoring produces Description.
Description produces Binding.
Binding produces BoundScriptCall.
Hot path only invokes.
```

## 0.3 MUST / MUST NOT / SHOULD / MAY

- **MUST**：必须实现；
- **MUST NOT**：禁止实现；
- **SHOULD**：除非存在明确且可记录的技术阻塞，否则应实现；
- **MAY**：可选；不得仅因为“未来可能有用”而实现。

如果本文没有覆盖某个边界：

> 不得自行新增 public manager、registry、bus、service locator、base class、compatibility wrapper 或持久化 runtime handle 来“补全”。

保持 public surface 最小，在实施报告中记录 gap。

---

# Part I — 当前代码事实与本轮修改边界

# 1. 唯一代码基线

本轮所有施工必须以：

```text
branch: codex/object-ui-foundation
HEAD:   99f984cf0eb0ee75f45d21a5f1dd5d7c60c4b9be
```

为唯一当前事实基线。

该 HEAD 只是 evidence commit；其 production parent 为：

```text
8afeb80496a6ccd69494e9848714884b84f525cd
```

当前 evidence 状态是：

```text
READY_FOR_INDEPENDENT_API_ACCEPTANCE
```

而不是：

```text
FROZEN
```

本文要求 public API / wire / benchmark / installed-consumer 再发生一次破坏性修订，因此：

> **`99f984...` 的 freeze evidence 从本轮施工开始只能作为历史证据，不得作为新 L1 freeze 的通过证据。**

完成本文后必须产生新的 production SHA 与新的 exact-SHA evidence。

---

# 2. 当前 L1 中需要被修订的代码事实

当前代码存在：

```text
engine/simulation/description/include/lux/engine/simulation/SystemExecutionPoint.hpp
```

其语义实际上只有：

```cpp
struct SystemExecutionPoint final
{
    std::string_view name;
};
```

当前 `SystemEventDescription` 包含：

```text
name
dispatch_point
payload_schema_name
payload_schema_version
payload_cpp_type
```

当前 `SystemDescription` 包含：

```text
capabilities
execution_points
events
```

当前 `SimulationDescription` 已 materialize：

```text
SimulationSystemView
SimulationExecutionPointView
SimulationEventView
SimulationDependencyView
```

当前 `SimulationDependency` 是：

```text
before System + before ExecutionPoint
    ->
after System + after ExecutionPoint
```

当前 LXSD wire version 为 v2，并含逻辑 sections：

```text
STRINGS
GLOBAL_DATA
SYSTEM_TYPES
INSTANCES
CAPABILITIES
EXECUTION_POINTS
EVENTS
DEPENDENCIES
PAYLOAD
```

这些 surface 是本轮主要破坏性修改对象。

---

# 3. 当前 Script / Meta 中可以复用的基础

## 3.1 `modules/function/script/core`

当前已经有：

```text
lux_script_abi.h
ScriptCallFrame.hpp
ScriptResult.hpp
ScriptSignature.hpp
ScriptValue.hpp
```

当前 ABI 已有：

```cpp
lux_script_type_desc
lux_script_value_slot
lux_script_call_frame
lux_script_invoke_fn
lux_script_function_desc
lux_script_module_desc
```

特别重要：

```cpp
lux_script_function_desc
{
    name;
    symbol_id;
    args;
    arg_count;
    returns;
    return_count;
    invoke;
};
```

这条 ABI **MUST 保留并演进，不得再造一套 Lua/MLIR/C++/Python 各自不同的 invoke abstraction。**

## 3.2 `modules/core/meta`

当前 Meta 已能表达：

```text
RefType
    QualType
    traits
    name
    hash
    size

RefParam
RefInvokable
RefFunction
RefMethod
```

`QualType` 已区分：

```text
T
T&
const T&
T&&
T*
const T*
...
```

`RefMethod` 已包含：

```text
is_noexcept
annotation_str / AnnotationView
```

因此 C++ signature 的 runtime truth source 已存在。本轮不应另造 C++ type registry。

## 3.3 当前 `rdesc::Script`

当前分支 **仍然存在**：

```text
modules/resource/description/include/lux/engine/description/Script.hpp
```

当前 schema v2 已有：

```text
ScriptValueType
ScriptFunction
ScriptDependency
ScriptProvenance
LuaSourceScript
NativeModuleScript
CppBehaviorScript
Script
```

这部分 **不是被删除了**；真正缺失的是旧 concrete `ScriptAsset : TAsset<Script>` 及其专用 SerDeser。

## 3.4 旧实现可复用机制，但不得恢复旧架构

历史实现中曾存在：

```text
ScriptAsset = ScriptDescription + backend payload
BoundScriptCall = { lux_script_invoke_fn, void* context }
per-event dense subscriber list
dispatchTo(entity)
FlowForge host-owned per-instance state block
```

这些机制经过历史实现验证，可以复用思想。

但是以下旧结构 **MUST NOT 恢复**：

```text
universal ScriptSystem
process-global ScriptEventRegistry
IScriptModule hot-path polymorphism
runtime per-call module lookup
old TAsset / AssetInfo hierarchy solely for ScriptAsset
```

---

# Part II — 最终分层与依赖方向

# 4. 四层职责

最终分层必须是：

```text
L0 / Script Foundation
    ↓
L1 / Simulation semantic contract
    ↓
Composition / Binding
    ↓
Language backend execution
```

更具体：

```text
core/meta
    runtime C++ type truth

modules/function/script/core
    language-neutral script ABI/signature/invocation primitive

resource/description + script asset codec
    serializable ScriptDescription + code payload

simulation/description
    Systems + SystemHookPoint + SystemEvent + System dependencies

binding/composition runtime
    ScriptAsset + ScriptMount + target Simulation
      -> BoundScriptCall / runtime slot

Lua / Native C++ / FlowForge-MLIR / future Python
    backend-specific prepare only
```

## 4.1 禁止的依赖方向

禁止：

```text
core/meta -> Simulation
script_core -> PhysicsSystem
script_core -> RenderSystem
script_core -> Entity/Registry
ScriptDescription -> TaskGraph
SimulationDescription -> Lua VM
System -> Python object
System -> Lua registry reference
```

允许：

```text
Simulation -> lower-level Script signature vocabulary
Script binding layer -> core/meta
Script backend -> script_core ABI
Simulation runtime composition -> ScriptAsset + SimulationDescription
```

## 4.2 关于“下降到 L0 的 core/script”

本轮语义上必须把下列能力视为 foundation：

```text
ScriptSymbolId
script value/signature vocabulary
script call frame
BoundScriptCall
signature comparison primitives
```

它们不得位于 ECS ScriptSystem 或具体 backend 中。

**本轮不强制为了目录名称而大规模搬迁 `modules/function/script/core`。**

但是实施后必须满足：

> 这些 API 不依赖 Simulation/ECS/Asset/具体语言，可被 Simulation Description、Asset importer 和 backend 共同消费。

若现有 architecture checker 要求其 classification 下降为 foundation，修改 target classification；不要为了层名引入第二套 `modules/core/script` 重复 API。

---

# Part III — L0 Script Foundation

# 5. L0 的 public responsibility

L0 Script Foundation 最终只回答：

```text
一个可调用函数的稳定 symbol 是什么？
参数/返回值的语义签名是什么？
如何构造一次 ABI call frame？
如何保存一个已经完成解析的 callable？
两个语义签名是否兼容？
```

它不知道：

```text
System
HookPoint
Event
Entity
Registry
TaskGraph
Asset UUID
Render
Physics
Scene
```

---

# 6. `ScriptSymbolId`

新增/正式化：

```cpp
using ScriptSymbolId = std::uint64_t;
inline constexpr ScriptSymbolId InvalidScriptSymbolId = 0;
```

规则：

1. 所有可进入 ScriptDescription `exports[]` 的函数 `symbol_id MUST != 0`；
2. 同一 ScriptDescription 中必须唯一；
3. runtime 绑定使用 `symbol_id`，不使用函数名；
4. `name` 仅用于 Editor / diagnostics / debugger；
5. symbol 如何由 authoring 工具生成是 importer/codegen 的责任；可以由 canonical name hash 生成，也可以由 sidecar metadata 保持 rename-stable；foundation 不建立全局 symbol registry。

禁止：

```text
runtime findFunction("on_render_start")
```

作为最终调用路径。

---

# 7. Durable Script Type / Signature Vocabulary

当前 `lux_script_type_desc` 是 **当前平台 ABI descriptor**，包含：

```text
type_id
size
align
kind
```

它不能直接作为跨平台、可序列化语义签名的唯一表示，因为 `size/align` 是平台 ABI 数据。

因此必须区分：

```text
Semantic signature
    durable / serializable / cross-platform

ABI signature
    current-process / current-platform
```

## 7.1 建议 public 类型

目标语义可采用等价命名，但字段语义必须一致：

```cpp
enum class EScriptPassMode : std::uint8_t
{
    VALUE,
    CONST_REF,
};

struct ScriptSemanticType final
{
    std::uint64_t type_id{};
    std::string_view canonical_name;
    EScriptPassMode pass{EScriptPassMode::VALUE};
};

struct ScriptFunctionSignatureView final
{
    std::span<const ScriptSemanticType> parameters;
    std::span<const ScriptSemanticType> returns;
};
```

持久化 Description 使用 owning strings/vectors；static System metadata 可使用 view/span。

## 7.2 第一版 Script boundary 允许的 pass semantics

第一版只允许：

```text
input primitive / ids / handles      -> VALUE
input reflected record               -> CONST_REF
return                                -> VALUE
```

MUST NOT 允许作为 script-bound public signature：

```text
T&
T&&
mutable pointer
arbitrary T*
Registry&
std::vector&
allocator-owned STL object reference
out parameter
variadic function
```

目的：

1. Lua/Python/MLIR/C++ 可以共享同一语义；
2. 不让脚本绕过 ECS/TaskGraph access contract；
3. 避免跨 FFI ownership 模糊；
4. 降低 marshal 与 hot path 成本。

## 7.3 `core/meta` 映射

C++ reflected function/method：

```text
RefInvokable
    ↓ normalize
ScriptFunctionSignature
```

映射必须检查：

```text
parameter count
return count
RefType.hash + canonical name
QualType
variadic
is_noexcept（C++ direct/native callback）
```

禁止仅比较 hash；canonical name 必须参与 collision defense。

禁止把 `RefType* / RefMethod*` 保存进 Description。

---

# 8. `BoundScriptCall`

把旧实现中的高价值 primitive 正式下沉到 Script Foundation：

```cpp
struct BoundScriptCall final
{
    lux_script_invoke_fn invoke{nullptr};
    void* context{nullptr};
};
```

必须保持：

```cpp
static_assert(sizeof(BoundScriptCall) == 2 * sizeof(void*));
static_assert(std::is_trivially_copyable_v<BoundScriptCall>);
```

`context` 为完全 opaque backend/instance context。

L0 **不得**解释其为：

```text
Entity*
Registry*
Lua State*
Python object*
FlowForge state*
```

具体 backend 决定。

## 8.1 `lux_script_call_frame`

继续使用当前 ABI：

```text
args
returns
world_context
user_context
```

约定：

```text
frame.user_context = BoundScriptCall.context
```

`world_context` 是 script host context 的 opaque pointer；不得把公开 contract 写成 `Registry*`。

---

# Part IV — SystemHookPoint

# 9. `SystemExecutionPoint` 正式退休

删除：

```text
SystemExecutionPoint
SystemExecutionPoint.hpp
SimulationExecutionPointView
executionPointCount()
executionPointAt()
findExecutionPoint()
```

不得保留：

```text
using SystemExecutionPoint = SystemHookPoint;
compat header
deprecated alias
shim package
```

本项目尚未 freeze，直接破坏性迁移。

---

# 10. `SystemHookPoint` 最终定义

正式定义：

> **SystemHookPoint 是一个由具体 System 拥有、由该 System 在明确代码位置主动触发的用户代码扩展点。**

它不是：

```text
TaskGraph node
barrier
scheduler phase
cross-System dependency endpoint
Event
```

## 10.1 Static System metadata

建议：

```cpp
enum class ESystemHookCardinality : std::uint8_t
{
    SINGLE,
    MULTI,
};

struct SystemHookPoint final
{
    std::string_view name;
    ESystemHookCardinality cardinality{ESystemHookCardinality::MULTI};
    lux::script::ScriptFunctionSignatureView signature;
};
```

提供 compile-time helper：

```cpp
makeSystemHookPoint<Signature>(name, cardinality)
```

例如：

```cpp
inline static constexpr auto BeforeStep =
    makeSystemHookPoint<void(const SimulationStepInfo&)>(
        "before_step",
        ESystemHookCardinality::MULTI
    );

inline static constexpr auto FilterContact =
    makeSystemHookPoint<bool(const ContactCandidate&)>(
        "filter_contact",
        ESystemHookCardinality::SINGLE
    );
```

## 10.2 Cardinality 规则

`MULTI`：

```text
0..N handlers
return count MUST == 0
```

`SINGLE`：

```text
0..1 handler after composition
return count MUST <= 1
```

第一版禁止多返回值 Hook，即使 generic script ABI 支持多个 returns。

### 10.2.1 SINGLE 的 mount 限制

第一版 `SINGLE` Hook 只允许 Global Script mount。

原因：System-wide Hook 不携带目标 Entity；允许任意 Entity Script 绑定 SINGLE 会产生“哪一个 Entity 是唯一 handler”的未定义语义。

如果未来需要 per-entity query/filter hook，应设计独立 targeted mechanism，不要偷偷把 SINGLE Hook 变成 entity scan。

---

# 11. Hook durable identity

本轮 **不要新增进程全局 `SystemHookRegistry` 或 public `SystemHookId` manager**。

Durable identity 使用：

```text
System canonical type
+ Hook canonical name
```

在具体 Simulation 中如需选择特定 System instance，再加：

```text
optional System instance name
```

Runtime 可以在 composition 时为每个 concrete Hook 分配 private dense ordinal；该 ordinal：

```text
MUST NOT serialize
MUST NOT become global
MUST NOT become public cross-session identity
```

---

# 12. Hook runtime slot

概念实现：

```cpp
struct HookRuntimeSlot
{
    std::vector<BoundScriptCall> calls;
};
```

不要使用：

```cpp
std::vector<std::function<...>>
```

作为 hot-path production storage。

实际实现 MAY 使用预分配 contiguous storage / small-vector / flat storage，但必须满足：

```text
iteration contiguous or equivalent cache-friendly
no runtime reflection
no name lookup
no asset lookup
no backend virtual dispatch
no allocation during invoke
```

## 12.1 同一函数绑定多个 Hook

明确允许：

```text
ScriptSymbol print_stage
    -> Physics.before_step
    -> Physics.after_step
    -> Render.before_render
```

它产生三条 binding，三个 slot 中各出现一次 `BoundScriptCall`。

如果三个 Hook 都触发，函数执行三次。

不得去重跨 target binding。

---

# Part V — SystemEvent

# 13. Event 与 Hook 的根本区别

```text
HookPoint
    = System 保证主动到达并调用扩展代码的位置

Event
    = System 运行过程中产生的 0..N occurrence
```

Event 不是 Hook alias。

Event 第一版：

```text
return count = 0
payload = void OR one typed payload record
0..N occurrences
```

不要让 Event 重新长成任意 N 参数函数模型；复杂数据放进 payload struct。

---

# 14. `ESystemEventTarget`

只允许：

```cpp
enum class ESystemEventTarget : std::uint8_t
{
    GLOBAL,
    ENTITY_TARGETED,
};
```

**禁止 `BROADCAST`。**

## 14.1 GLOBAL

语义：

```text
只交付给 Global Script instances 中绑定该 Event 的 handlers
```

MUST NOT：

```text
scan all Entity
scan ScriptComponent view
scan all script instances then test scope
```

Runtime 应拥有已经准备好的 global dense call list。

## 14.2 ENTITY_TARGETED

每个 occurrence 必须带一个明确 runtime Entity target。

语义：

```text
event occurrence + target Entity
    ↓
直接查目标 Entity 的 script runtime binding table
    ↓
只调用该 Entity 实际绑定的 handlers
```

MUST NOT 扫描其他 Entity。

---

# 15. `SystemEventDescription`

目标结构语义：

```cpp
struct SystemEventDescription final
{
    std::string_view name;
    std::string_view dispatch_hook;
    ESystemEventTarget target;

    std::string_view payload_schema_name;
    std::uint32_t payload_schema_version{};

    // code-side only validation metadata
    lux::cxx::TypeToken payload_cpp_type;
};
```

`makeSystemEvent<Payload>` 改为接受 `SystemHookPoint`：

```cpp
makeSystemEvent<Payload>(
    name,
    dispatch_hook,
    target,
    payload_schema_name,
    payload_schema_version
)
```

## 15.1 payload rule

`Payload = void`：

```text
payload_schema_name empty
payload_schema_version == 0
```

非 void：

```text
payload_schema_name non-empty
payload_schema_version > 0
payload_cpp_type valid on code-side static description
```

Serialized SimulationDescription **不得保存/恢复假的 C++ TypeToken**。

当前 decoder 中通过 durable schema hash/name 构造 synthetic TypeToken 的做法必须退休。

Decode 后只 materialize durable semantic payload identity；与当前 C++ System 兼容性比较由 internal compatibility helper 在加载/绑定阶段完成。

---

# 16. Event 的 `dispatch_hook`

`dispatch_hook` 只回答：

> worker/parallel runtime 产生的 Event occurrence 在该 System 的哪个安全 Hook 时刻被同步交付？

它不是 scheduler edge。

标准语义：

```text
worker tasks generate events
        ↓
producer-local/prepared event buffers
        ↓
System reaches dispatch_hook H
        ↓
deterministically merge + drain events assigned to H
        ↓
dispatch Event callbacks
        ↓
invoke H's Hook callbacks
```

本轮固定顺序：

> **同一个 Hook H：先交付绑定到 H 的 Event occurrences，再调用 H 自身的 Hook handlers。**

不得由不同 System/backend 自行反转，否则 Script 语义不可预测。

---

# 17. Worker Event buffering

Foundation 不新增 public generic EventBus。

允许：

```text
System-private typed event buffers
private/internal reusable prepared typed-event buffer helper
```

必须满足：

```text
producer count/capacity prepared outside hot loop
worker only appends to producer-local storage
no user script callback on worker unless target Hook contract explicitly declares worker-safe（第一版不提供）
quiescent hook merge deterministic
no allocation after prepare
```

第一版所有 Script-bound Event dispatch 均发生在安全 Hook，而不是生成事件的 worker 上。

---

# Part VI — SystemDescription 与 SimulationDescription

# 18. `SystemDescription`

目标字段：

```cpp
struct SystemDescription final
{
    std::string_view canonical_name;
    std::uint32_t version{};

    std::string_view configuration_schema_name;
    std::uint32_t configuration_schema_version{};

    std::span<const std::string_view> capabilities;
    std::span<const SystemHookPoint> hooks;
    std::span<const SystemEventDescription> events;
};
```

删除：

```text
execution_points
```

## 18.1 `validSystemDescription()` 必须验证

至少：

```text
canonical name non-empty
version > 0
configuration schema name/version pairing
capability names valid + unique
hook names non-empty + unique
hook signatures are script-bindable
MULTI hook has no return
SINGLE hook return count <= 1
event names non-empty + unique
event dispatch_hook exists in hooks
event target is GLOBAL or ENTITY_TARGETED
event payload schema rule valid
```

## 18.2 System Description version 必须随 contract 破坏性变化升级

本轮把：

```text
ExecutionPoint(name only)
    ->
HookPoint(name + cardinality + signature)

Event(dispatch_point)
    ->
Event(dispatch_hook + target + durable payload semantics)
```

因此任何当前内建 System 只要其 static Description 暴露了这些 metadata，都必须重新审查并升级 `Description.version`。

规则：

```text
Hook 增删/重命名                     -> version bump
Hook signature/cardinality 改变       -> version bump
Event 增删/重命名                    -> version bump
Event target/dispatch hook 改变       -> version bump
Event payload semantic schema 改变    -> version bump
仅 implementation 内部优化、不改 contract -> MAY keep version
```

不得因为“LXSD wire 已经 bump 到 v3”就省略 System contract version bump；两者解决不同层次的兼容性。

## 18.3 Current-System compatibility helper 必须升级

当前 private `matchesCurrentSystemDescription<CurrentSystem>()` 思路继续保留，但比较字段必须至少包含：

```text
SystemTypeId
System Description version
configuration schema
capabilities
Hook count/order/name
Hook cardinality
Hook complete semantic signature
Event count/order/name
Event dispatch hook
Event target
Event payload schema name/hash/version
```

不得只比较 Hook/Event 名称。

不得通过 serialized schema name/hash 构造 fake `TypeToken` 后再比较。

---

# 19. Simulation Description views 重命名

删除：

```text
SimulationExecutionPointView
```

新增：

```text
SimulationHookPointView
```

`SimulationSystemView`：

```text
hookCount()
hookAt()
findHook()
```

替换：

```text
executionPointCount()
executionPointAt()
findExecutionPoint()
```

`SimulationEventView`：

```text
system()
name()
dispatchHook()
target()
payloadSchemaName()
payloadSchemaHash()
payloadSchemaVersion()
```

Hook view 至少：

```text
system()
name()
cardinality()
parameterCount()/parameterAt()
returnCount()/returnAt()
```

或返回一个 immutable signature view。

---

# 20. `SimulationDependency` 改为 System-level

最终调度依赖只表达：

```text
System instance A must complete before System instance B may execute
```

Builder：

```cpp
addDependency(
    std::string_view before_system,
    std::string_view after_system
);
```

删除所有带 Hook/ExecutionPoint 参数的 overload。

`SimulationDependencyView`：

```text
beforeSystem()
afterSystem()
```

Cycle detection 直接在 System instance DAG 上执行。

这次改动不是“降低精度”，而是修正概念：

```text
HookPoint = user-code invocation semantic
Dependency = scheduling semantic
```

二者不再混用。

---

# 21. System local Hook order

Hook 声明顺序 **不是跨 System scheduling order**。

它仅用于：

```text
stable System self-description order
Editor display order
codec deterministic order
local runtime slot ordinal
```

System 自己在代码中何时调用哪个 Hook 由 System implementation 决定。

不得自动生成：

```text
hook[0] -> hook[1] -> hook[2]
```

TaskGraph edges。

---

# Part VII — ScriptDescription v3

# 22. `rdesc::Script` 从 schema v2 升级到 v3

当前 v2 的主要问题：

```text
NativeModuleScript 有 functions[]
LuaSourceScript 只有 entry
CppBehaviorScript 又是另一种结构
```

导致通用 Editor/Binding 层必须知道 backend kind。

v3 必须把 `exports[]` 提升为 Script common metadata。

---

# 23. Script common export model

目标：

```cpp
struct ScriptValueType
{
    std::string   canonical_name;
    std::uint64_t type_id{};
    EScriptPassMode pass{};
};

struct ScriptFunction
{
    std::string name;
    ScriptSymbolId symbol_id{};
    std::vector<ScriptValueType> args;
    std::vector<ScriptValueType> returns;
};
```

删除 durable common signature 中的：

```text
size
align
```

这些由 runtime current-platform ABI / loaded module descriptor 提供。

如果当前 codec/toolchain 暂时必须保留 ABI fields，则必须把它们移动到 backend-specific/native ABI manifest，不能把其定义成跨平台 semantic match 的 truth。

---

# 24. `ScriptBindingDescription`

Binding 是 Script function 与 Simulation semantic target 的关系。

它 **不属于函数身份本身**。

目标 serializable model：

```cpp
enum class EScriptBindingKind : std::uint8_t
{
    HOOK,
    EVENT,
};

struct ScriptBindingDescription final
{
    ScriptSymbolId function{};
    EScriptBindingKind kind{};

    // canonical System type name; cold-path resolves to SystemTypeId
    std::string system_type;

    // empty => require exactly one instance of system_type in composed Simulation
    std::string system_instance;

    // Hook/Event canonical member name
    std::string member;
};
```

这里故意不让 resource ScriptDescription 直接依赖 `Simulation::SystemTypeId`。

Runtime bind 时：

```text
system_type string
    -> systemTypeId()
    -> resolve instances
```

因为这是 cold path，字符串成本允许。

## 24.1 target resolve rule

若 `system_instance` 非空：

```text
该实例 MUST 存在
其 SystemType MUST == system_type
```

若 `system_instance` 为空：

```text
0 matching instance  -> TARGET_SYSTEM_NOT_FOUND
1 matching instance  -> success
>1 matching instance -> TARGET_SYSTEM_AMBIGUOUS
```

不得“取第一个”。

---

# 25. Script default bindings

ScriptDescription v3 common fields：

```text
schema_version
module_name
exports[]
default_bindings[]
dependencies
provenance
backend body
```

`default_bindings[]` 是 annotation / decorator / FlowForge / Editor 默认 authoring 的 canonical result。

按函数名自动绑定被删除。

没有 annotation 也完全合法：

```text
exports[] exists
default_bindings[] empty
```

Editor 可在 mount 上显式配置。

---

# 26. Backend body

`NativeModuleScript` v3：

```text
abi_version
state_layout_hash
state_size
state_defaults
```

`functions[]` 从 body 删除，移到 common `exports[]`。

`LuaSourceScript`：

```text
entry / module-specific load metadata
```

函数 export 仍在 common `exports[]`。

`CppBehaviorScript`：

```text
registered behavior name / static factory key
```

函数 export 仍在 common `exports[]`。

FlowForge AOT 继续作为 NativeModule backend；provenance 标识来源。

未来 Python 可新增 backend body，但 **不得改变 common export/binding model**。

---

# Part VIII — Script Asset

# 27. 恢复 ScriptAsset 语义，不恢复旧 Asset framework

最终 Script asset content：

```text
ScriptDescription v3
+
opaque backend payload bytes
```

例如：

```text
Lua      -> source/bytecode
Native   -> DLL/SO/dylib image
MLIR AOT -> native image
Python   -> source/bytecode（未来）
C++ static behavior -> payload MAY be empty
```

## 27.1 禁止恢复

禁止恢复旧：

```text
class ScriptAsset : TAsset<Script>
AssetInfo ownership hierarchy
old ScriptSerDeser factory hierarchy
```

仅因为历史代码存在而恢复这些结构。

## 27.2 使用当前 AssetCodecSet

通过当前：

```text
AssetTypeId
AssetCodecDescriptor
DecodedAsset
AssetCodecSet
```

注册 Script asset codec。

可新增一个 immutable decoded aggregate，例如：

```cpp
struct ScriptAssetPayload final
{
    rdesc::Script description;
    SharedBytes payload;
};
```

名称可按当前 Resource 命名约定调整，但语义必须是 immutable decoded content，不是 resident manager/object facade。

Script codec SHOULD 独立成 pay-for-use component，generic asset package 不必强制链接脚本 backend。

---

# 28. Script asset codec 验证

Decode MUST 验证：

```text
wire/schema version
input/decode budgets
backend kind
module_name
unique non-zero symbol ids
export names
semantic type identity
binding function references exist
binding target strings non-empty
no exact duplicate binding
native state layout bounds/defaults
payload bounds
```

Decode MUST NOT：

```text
load Lua VM
dlopen native library
import Python
compile MLIR
resolve Simulation systems
run reflection lookups
create ScriptInstance
```

这些属于 bind/runtime composition。

---

# Part IX — ScriptMount：Global 与 Entity

# 29. 一个统一的 `ScriptMountDescription`

不要创建：

```text
GlobalScriptAsset
EntityScriptAsset
FlowScriptAsset
```

使用同一 ScriptAsset，不同的是 mount ownership/lifetime。

建议：

```cpp
enum class EScriptBindingSetMode : std::uint8_t
{
    ASSET_DEFAULTS,
    EXPLICIT,
};

struct ScriptMountDescription final
{
    AssetId script;
    EScriptBindingSetMode binding_mode{EScriptBindingSetMode::ASSET_DEFAULTS};
    std::vector<ScriptBindingDescription> bindings;

    // authored instance parameters/state overrides MAY be added using
    // existing typed/versioned Description mechanism; do not store runtime state.
};
```

规则：

```text
ASSET_DEFAULTS:
    bindings MUST be empty
    effective bindings = ScriptDescription.default_bindings

EXPLICIT:
    bindings is the COMPLETE effective set
    asset defaults are ignored
```

第一版不设计：

```text
ADD
REMOVE
PATCH
MERGE
priority override language
```

Editor 如果用户想“默认 + 加一条”，应复制 defaults 后生成一份完整 EXPLICIT set。

---

# 30. Entity Script mount

World/ECS 的 ScriptComponent 只保存 authored facts：

```text
Script AssetId
ScriptMountDescription / equivalent
optional enabled/config data
```

MUST NOT 序列化：

```text
BoundScriptCall
Lua registry ref
PyObject*
Native function pointer
ScriptInstance*
System hook slot pointer
runtime Entity generation cache
```

Entity Script runtime：

```text
self = Entity
lifetime <= Entity + Script mount lifetime
```

Entity 生命周期结束后，该 instance 的全部 bindings 必须在安全同步点移除，并销毁 instance state。

## 30.1 一个 Entity 多脚本

本文不强制 public ECS 组件必须采用单 asset 或 vector 形状。

但是 runtime/binding design **MUST 支持同一 Entity 存在多个 Script mount 的语义**，不能把 targeted Event 实现写死为“每 Entity 永远只有一个 callback”。

若当前 World schema 只允许一个 `ScriptComponent`，其内部可包含多个 mount；若已有更合适的 authoring schema，复用它。

不要为这一点新建 global script manager。

---

# 31. Global Script mount

Global Script 属于 Simulation rules，因此 global mounts 是 `SimulationDescription` 的一部分。

逻辑：

```text
SimulationDescription
    global_script_mounts[]
```

Global runtime：

```text
self = none
lifetime = Simulation runtime
```

同一 ScriptAsset 可以同时被：

```text
Global mount
Entity mount
```

各自创建独立 ScriptInstance/state。

## 31.1 SimulationDescription public view / builder

新增等价 public surface：

```text
globalScriptMountCount()
globalScriptMountAt(index)
```

`SimulationGlobalScriptMountView` 至少暴露：

```text
script AssetId
binding mode
explicit binding count / bindingAt（仅 EXPLICIT 时）
```

Builder 至少提供：

```cpp
addGlobalScriptMount(const ScriptMountDescription&)
eraseGlobalScriptMount(std::size_t ordinal)
```

或使用当前 builder 风格的等价 API。

Builder 只验证 Description-local invariants：

```text
AssetId not nil
binding mode/list consistency
nonzero ScriptSymbolId for explicit bindings
target strings valid
no exact duplicate binding inside the mount
```

Builder **不得加载 ScriptAsset** 来验证 symbol 是否存在，因为 `SimulationDescription` build 是纯 Description 构建；asset existence/export/signature validation 发生在 runtime/editor composition bind 阶段。

因此需要区分：

```text
Description structural validity
    !=
composed runtime bind validity
```

---

# Part X — Binding Resolver

# 32. Binding 是 cold-path compiler

把 binding layer 看成一个小型“链接器”，不是 runtime manager。

流程：

```text
ScriptMount
    ↓
AssetId -> immutable ScriptAsset
    ↓
choose effective binding set
    ↓
resolve ScriptSymbolId
    ↓
resolve target System
    ↓
resolve Hook/Event member
    ↓
validate kind
    ↓
validate mount/event scope
    ↓
validate exact semantic signature
    ↓
backend prepare
    ↓
BoundScriptCall { invoke, context }
    ↓
install into runtime slot/table
```

所有昂贵工作都在这里。

---

# 33. Binding validation error model

至少需要区分：

```text
SCRIPT_ASSET_NOT_FOUND
SCRIPT_DESCRIPTION_INVALID
SCRIPT_SYMBOL_NOT_FOUND
SCRIPT_SYMBOL_DUPLICATE
TARGET_SYSTEM_NOT_FOUND
TARGET_SYSTEM_AMBIGUOUS
TARGET_INSTANCE_TYPE_MISMATCH
TARGET_HOOK_NOT_FOUND
TARGET_EVENT_NOT_FOUND
BINDING_KIND_MISMATCH
BINDING_SIGNATURE_MISMATCH
BINDING_SCOPE_MISMATCH
DUPLICATE_BINDING
SINGLE_HOOK_MULTIPLE_HANDLERS
BACKEND_NOT_AVAILABLE
BACKEND_PREPARE_FAILED
ALLOCATION_FAILURE
```

错误必须在 cold bind 时暴露。

不得第一次调用时才发现：

```text
function missing
signature mismatch
hook not found
```

---

# 34. Signature matching

## 34.1 Hook

```text
Script export semantic signature
    MUST EXACTLY MATCH
Hook semantic signature
```

第一版不做：

```text
prefix-compatible args
implicit numeric widening
T <-> optional<T>
ignore extra return
ignore extra args
```

## 34.2 Event

一个 non-void payload Event 的脚本 export canonical signature 为：

```text
void(const Payload&)
```

void payload Event：

```text
void()
```

Entity `self` 不进入参数列表。

## 34.3 Meta 使用

C++/Native reflected code：

```text
RefInvokable -> semantic signature
```

Meta 只参与：

```text
import/codegen/bind validation
```

MUST NOT 在 hot invoke 期间重新访问 ReflectionRegistry。

---

# 35. Scope validation

Global mount：

```text
MAY bind MULTI Hook
MAY bind SINGLE Hook
MAY bind GLOBAL Event
MUST NOT bind ENTITY_TARGETED Event
```

Entity mount：

```text
MAY bind MULTI Hook
MAY bind ENTITY_TARGETED Event
MUST NOT bind GLOBAL Event
MUST NOT bind SINGLE Hook（第一版）
```

如果未来需要 entity-targeted single query，另行设计 targeted Hook，不通过放宽这条规则偷渡。

---

# Part XI — Runtime Binding Plan 与 Hot Path

# 36. Private dense runtime ordinals

为了避免 Entity-targeted event hot-path hash lookup，composition runtime MAY/MUST 为当前 concrete Simulation 构造 private dense slot ids。

例如：

```cpp
using RuntimeScriptSlotId = std::uint32_t;
```

它可以表示：

```text
Physics.CollisionBegin targeted event
Render.BeforeRender hook subscription index
...
```

规则：

```text
private/internal only
session-local
not serialized
not global registry
not public API
```

它只是当前运行时 BindingPlan 的数组索引。

---

# 37. Hook hot path

正确模型：

```text
prepare frame ONCE
    ↓
for BoundScriptCall in contiguous slot
    frame.user_context = call.context
    result = call.invoke(&frame)
```

禁止：

```text
for each handler:
    rebuild signature
    lookup reflection
    lookup asset
    lookup function name
    allocate frame vectors
```

## 37.1 MULTI Hook

frame 参数只构造一次。

每 subscriber 成功路径只应做近似：

```text
read invoke ptr
read context ptr
write user_context
indirect call
check integer result
```

## 37.2 SINGLE Hook

只有 0..1 handler。

非 void return 使用预先准备的 return slot。

未绑定时：

```text
runtime slot returns “no handler”
System implementation decides fallback/default behavior
```

Foundation 不替 System 发明默认值。

---

# 38. GLOBAL Event hot path

每个 occurrence：

```text
construct payload frame once
    ↓
iterate already-bound GLOBAL handlers only
```

不会扫描 Entity。

如果同一 Event occurrence 有 N global subscribers：

```text
frame built once
N function pointer calls
```

---

# 39. ENTITY_TARGETED Event hot path

每个 occurrence 已有：

```text
target Entity
runtime event slot id
payload
```

正确路径：

```text
target Entity
    ↓
entity script runtime sidecar/table
    ↓
runtime event slot id
    ↓
0..N BoundScriptCall range
    ↓
invoke
```

必须是 O(targeted occurrences + actual bound callbacks)，不是 O(scene entities)。

如果目标 Entity：

```text
不存在
已销毁
没有 script runtime
没有绑定该 event
```

则快速 no-op。

---

# 40. `self` 语义

Entity mount：

```text
BoundScriptCall.context / ScriptInstance runtime state
    contains or can reach implicit self Entity
```

脚本函数：

```text
on_collision(payload)
```

不是：

```text
on_collision(selfEntity, payload)
```

Global mount：

```text
self absent
```

Host API 应显式区分 Global/Entity context；不得让 Global script 伪造 Entity self。

---

# 41. ECS access safety

Script callback **不得默认获得裸 `Registry&`**。

否则：

```text
SystemAccessSpec
TaskGraph hazard metadata
deferred structural command policy
reactive patch semantics
```

全部可被绕过。

Script host API 应通过受控能力提供：

```text
read component
patch component
queue structural mutation
query allowed service
```

具体 capability API 可分阶段实现，但 public Hook/Event signature 不得暴露 `Registry&`。

C++ ScriptBehavior 可复用历史 `getComponent` / `patchComponent` 的思想，但必须适配当前 direct EnTT / reactive semantics，不恢复旧 World wrapper。

---

# 42. Script failure during dispatch

Language errors必须在 backend thunk 内转换为 ABI integer error code：

```text
Lua error -> nonzero
Python exception -> nonzero（未来）
Native/C++ exception MUST NOT cross ABI boundary
C++ direct handler MUST be noexcept
```

MULTI/Event dispatch：

```text
record failure
continue or stop according to the fixed local dispatch policy
but MUST NOT structurally mutate active call list in-place
```

推荐本轮固定：

```text
record failed ScriptInstance/binding into preallocated failure list
continue remaining handlers
at post-dispatch quiescent point disable/unbind failed instance
```

这样 active contiguous list 不在遍历中 erase。

SINGLE Hook：

```text
return nonzero status to System caller
System chooses fallback/error policy
```

Hot path 不返回 heap/string/expected object；低层 status 使用整数/小 POD。

---

# Part XII — Binding mutation 与生命周期

# 43. Quiescent mutation rule

以下操作不能直接修改正在迭代的 Hook/Event call list：

```text
Entity destroy
ScriptComponent add/remove
Script mount enable/disable
hot reload
Global Script mount/unmount
script failure auto-disable
```

必须：

```text
record pending binding mutation
    ↓
reach designated quiescent synchronization point
    ↓
apply add/remove/rebuild
```

这与 ECS structural command 的纪律一致。

---

# 44. Entity lifecycle

推荐：

```text
Script mount becomes active
    ↓
acquire ScriptAsset / session code
create ScriptInstance
resolve + prepare bindings
queue/install bindings at safe point
optional create lifecycle hook/event

Entity or mount destroyed
    ↓
mark runtime instance closing
queue unbind
safe point removes all call entries
run destruction callback if contract provides one
release instance state
```

绝不能出现：

```text
Entity destroyed
but Hook slot still retains dangling context pointer
```

测试必须覆盖。

---

# 45. Code/session lifetime

复用旧 ADR 的正确思想：

```text
Asset identity != loaded code lifetime
```

Native module/Lua chunk/MLIR compiled artifact MAY 在一次 Simulation/Scene run session 内驻留到 stop，避免每个 Entity 的 ScriptInstance 销毁导致 module unload。

但是这属于 explicit runtime/session owner，不建立 process-global ScriptRuntime singleton。

`BoundScriptCall` 有效期必须被 module/code lifetime lease 覆盖。

---

# Part XIII — Authoring / Editor / FlowForge

# 46. Authoring 语法不是 runtime contract

以下都只是 authoring front-end：

```text
C++ annotation / macro
Python decorator
Lua comment annotation
Editor dropdown
FlowForge generated node
手工配置
```

最终都必须产生：

```text
ScriptDescription.exports
ScriptBindingDescription
ScriptMountDescription
```

Runtime 不识别 annotation/decorator。

---

# 47. 按函数名自动绑定正式删除

禁止 runtime/editor 自动规则：

```text
function name == Hook name
    => silently bind
```

Editor MAY 提示 suggestion，但确认后必须生成 explicit binding data。

因此函数可以叫：

```text
print_stage
foo
begin_frame_debug
```

并显式绑定到任意兼容 Hook。

---

# 48. C++ annotation（可选 authoring）

如果本轮实现 annotation，推荐 authoring surface 类似：

```cpp
LUX_BIND_POINT("lux.render", "on_start")
void print_stage() noexcept;

LUX_BIND_EVENT("lux.physics", "collision_begin")
void on_hit(const CollisionBegin&) noexcept;
```

不要把非标准 `[[bind_point="..."]]` 文本语法作为 public C++ contract。

现有 Meta `AnnotationView` 可用于 codegen/runtime description emission。

如果 free function annotation 需要 `RefFunction` 持有 annotation metadata，可扩展 Meta；但这是 authoring 支持，不得让 hot path 依赖 annotation lookup。

---

# 49. Python / Lua authoring（未来或 Editor tooling）

Python MAY：

```python
@lux.bind_point("lux.render", "on_start")
def print_stage() -> None: ...
```

Lua MAY：

```lua
---@lux.bind_point lux.render on_start
local function print_stage()
end
```

但 importer 的最终产物仍是同一 `ScriptDescription`。

Python backend 本身不是本轮 L1 freeze 的强制 runtime backend；foundation 必须为未来接入保留 common model，但不得为了未来 Python 引入当前不用的 manager/runtime framework。

---

# 50. FlowForge / MLIR

FlowForge 最终不需要依赖 annotation 或 magic function names。

Editor：

```text
SimulationDescription
    ↓ enumerate Systems
    ↓ enumerate Hooks / Events + signatures
    ↓ generate FlowForge palette nodes
```

Hook node/event node pins 直接由 Description signature/payload 生成。

FlowGraph -> MLIR：

```text
function/event entry
    -> common Script export
    -> explicit binding
    -> MLIR typed func
    -> JIT/AOT
    -> lux_script_abi
```

Legacy FlowForge 的以下机制应复用思想：

```text
typed FuncDef
OnEvent entry
stable graph ids
host-owned per-instance state block
MLIR typed lowering
```

但不得恢复 old ScriptEventRegistry 作为 palette truth source。

新的 palette truth source 是：

```text
SimulationDescription
```

---

# Part XIV — LXSD v3

# 51. Wire version 必须升级

当前 LXSD v2 的 wire semantics 含：

```text
EXECUTION_POINTS
point-level DEPENDENCIES
```

本轮它们发生了非加法语义变化，因此：

> **MUST bump LXSD wire version from 2 to 3.**

不得在 v2 number 下静默改变记录含义。

因为尚未 freeze：

```text
v1 reject
v2 reject
v3 accept
```

即可。

不写 compatibility migration runtime。

---

# 52. LXSD v3 逻辑 sections

建议至少：

```text
STRINGS
GLOBAL_DATA
SYSTEM_TYPES
INSTANCES
CAPABILITIES
HOOKS
HOOK_SIGNATURE_TYPES / SIGNATURE_DATA
EVENTS
SYSTEM_DEPENDENCIES
GLOBAL_SCRIPT_MOUNTS
SCRIPT_BINDINGS
PAYLOAD
```

具体 binary table 是否把 signature types 合并/拆分是 codec private implementation detail，但 wire 必须完整表示：

```text
Hook name
Hook cardinality
Hook semantic parameters
Hook semantic returns
Event name
Event dispatch hook
Event GLOBAL/ENTITY_TARGETED
Event payload durable schema identity
System-level dependency
Global Script AssetId
Global Script binding mode
Global explicit bindings（如有）
```

---

# 53. LXSD v3 canonicalization

继续要求 deterministic encoding：

```text
same semantic Description
    -> identical byte stream
```

String table 可排序 canonicalize；records 的 public declaration order 必须按 contract 保留时不得无意排序改变语义。

尤其：

```text
Hooks declaration order
Events declaration order
Global script mount order
Binding order
```

是确定性 dispatch 的输入之一，应保持。

---

# 54. Decode 安全

继续使用 caller-supplied decode budgets。

必须检查：

```text
section bounds
count * stride overflow
string bounds
ordinal bounds
hook signature type bounds
event dispatch hook ordinal
system dependency system ordinals
AssetId bytes
binding string/symbol validity
payload aggregate limits
```

Decode 不做：

```text
System instantiation
script asset load
script backend load
reflection
binding
TaskGraph build
```

---

# Part XV — Determinism

# 55. Hook handler deterministic order

建议冻结：

```text
1. Global Script mounts: SimulationDescription declaration order
2. Within one mount: effective binding declaration order
3. Entity mounts: deterministic Entity order
4. Within one Entity: mount order
5. Within one mount: binding declaration order
```

如果 ECS runtime entity order 不能稳定满足这一规则，则 binding apply 阶段必须显式建立 stable order，而不是依赖 unordered/hash insertion timing。

同一 function 绑定多个不同 Hook 不相互排序；各自按所在 Hook 的 handler list 排序。

---

# 56. Worker Event ordering

事件 merge key 至少必须包含：

```text
producer ordinal
producer-local sequence
```

producer ordinal 在 prepare/composition 阶段固定。

不得用：

```text
worker thread id
completion time
mutex acquisition order
```

决定脚本可见的 event 顺序。

---

# Part XVI — 性能合同

# 57. 冷路径允许，热路径禁止

Cold path 允许：

```text
Asset UUID lookup
string lookup
hash lookup
reflection
signature validation
Lua function resolution
Python inspect（未来）
MLIR manifest generation
allocation
module loading
```

Hot path 禁止：

```text
Asset lookup
UUID lookup
string lookup
unordered_map lookup
ReflectionRegistry lookup
shared_ptr atomic refcount churn
expected construction
virtual backend dispatch
heap allocation
function-name lookup
signature reconstruction
stale/revision check
```

---

# 58. Native/Prepared direct call contract

64-bit：

```text
BoundScriptCall = 16 bytes
```

成功 MULTI/Event subscriber hot loop 目标：

```text
load invoke pointer
load context pointer
write frame.user_context
indirect invoke
integer result branch
```

若 profiling 证明某个额外字段必须加入 BoundScriptCall，必须重新做架构 review；不得未经批准扩大 hot record。

---

# 59. Argument frame reuse

一个 Hook/Event occurrence 的参数 frame：

```text
MUST build once per occurrence
MUST reuse across all subscribers
```

错误：

```text
N subscriber -> N signature/frame builds
```

正确：

```text
1 occurrence -> 1 frame build -> N calls
```

`user_context` 每次 call 替换。

---

# 60. No-scene-scan performance invariant

必须通过 instrumentation/test 证明：

```text
GLOBAL event cost depends on global subscribers only
ENTITY_TARGETED event cost depends on targeted events + actual target handlers only
```

不能仅通过 benchmark 时间“看起来很快”间接证明。

测试应有 counter：

```text
entities_examined == targeted entities actually addressed
or
scene_scan_count == 0
```

---

# Part XVII — Benchmark 与 Qualification

# 61. benchmark schema 升级

当前仓库 benchmark 已到 schema v5，但 official evaluator/policy 仍存在 v4 残留；当前 `typed-event` benchmark 也没有真正覆盖 Script Signal/BoundScriptCall 路径。

本轮建议直接升为：

```text
benchmark schema v6
```

并同步：

```text
benchmark producer
official policy
evaluator
evidence writer
```

一次完成。

禁止 v5 benchmark + v4 policy 混用。

---

# 62. v6 必须包含的 Script/Hook/Event groups

至少：

```text
bound-call-native
hook-multi
global-event
entity-targeted-event
```

规模：

```text
100,000
1,000,000
```

其中：

## 62.1 `bound-call-native`

直接调用真实 `lux_script_invoke_fn` / `BoundScriptCall`。

要求：

```text
0 allocation
0 reflection lookup
0 string lookup
callback_count == call_count
```

## 62.2 `hook-multi`

真实 runtime slot + frame reuse。

要求：

```text
0 allocation after prepare
1 frame build per invocation
callback_count exact
```

## 62.3 `global-event`

真实 event payload -> prepared global subscriber list。

禁止仅遍历 `vector<TypedEvent>` 然后 `++callback_count`。

## 62.4 `entity-targeted-event`

测试大量 scene entities，其中只有一部分有 scripts；发生 N targeted events。

要求：

```text
no scene scan
no ScriptComponent view walk
callbacks == actual target subscriptions
```

---

# 63. 原 L1 benchmark groups 仍需保留

本文不取消上一版要求的：

```text
TaskGraph execution scaling
EcsCommandBuffer
World description build/lookup/partition
Snapshot（若 public）
reactive dirty path
```

新 evidence 必须是完整新 policy 的结果，不得只报告本轮 Script groups。

---

# Part XVIII — 测试矩阵

# 64. L0 Script Foundation tests

必须覆盖：

```text
ScriptSymbolId nonzero/unique validation
semantic signature equality
canonical-name collision defense
VALUE vs CONST_REF mismatch
unsupported mutable ref rejected
variadic rejected
BoundScriptCall sizeof/trivial-copy contract
call frame args/returns
native invoke error code propagation
```

Meta adapter：

```text
C++ void()
C++ primitive values
C++ const reflected struct&
return value
noexcept accepted
throwing C++ callback rejected where direct script-bound
mutable ref/pointer rejected
```

---

# 65. SystemDescription tests

覆盖：

```text
valid hook
empty hook name rejected
duplicate hook rejected
MULTI non-void rejected
SINGLE one-return accepted
unsupported signature rejected
valid GLOBAL event
valid ENTITY_TARGETED event
invalid target enum rejected
event missing dispatch hook rejected
duplicate event rejected
payload schema pairing
```

---

# 66. SimulationDescription builder tests

覆盖：

```text
System hooks materialized
Hook signatures materialized
Event dispatchHook materialized
Event target materialized
findHook
findEvent
System-level dependency add/erase
duplicate dependency
true System cycle rejected
no point-level dependency API remains
```

特别加入：

```text
SystemA has before/after hooks
SystemB exists
A -> B dependency is one System edge
binding same script to A.after and B.before is legal
```

并验证函数会被调用两次；不把它解释成一个 shared scheduling point。

---

# 67. LXSD v3 tests

至少：

```text
build rich Description
encode v3
decode fresh object
compare all global data
compare systems
compare hooks + signatures
compare events + target + dispatch hook
compare system dependencies
compare global script mounts/bindings
re-encode == original bytes
reject v1
reject v2
corrupt section offsets
corrupt hook type ordinal
corrupt event hook ordinal
corrupt dependency ordinal
corrupt binding symbol/strings
budget failures
```

---

# 68. ScriptDescription / ScriptAsset tests

ScriptDescription v3：

```text
common exports independent of backend
symbol uniqueness
common default bindings
Native body has no duplicate function table truth
Lua body uses common exports
Cpp behavior uses common exports
schema v2 rejected by new codec unless explicit offline migration tool exists
```

ScriptAsset codec：

```text
Description + payload roundtrip
asset type/magic uniqueness
payload budget
invalid binding references
invalid state defaults
immutable decoded payload
fresh installed consumer
```

---

# 69. Binding resolver tests

必须覆盖：

```text
one function -> one Hook
one function -> multiple Hooks
one function -> Hook + Event if signatures both compatible
duplicate identical binding rejected
missing function symbol
missing System type
ambiguous System type without instance
explicit instance success
instance/type mismatch
missing Hook
missing Event
HOOK binding to Event rejected
EVENT binding to Hook rejected
signature mismatch
Global mount -> GLOBAL Event success
Global mount -> ENTITY_TARGETED Event reject
Entity mount -> ENTITY_TARGETED Event success
Entity mount -> GLOBAL Event reject
Entity mount -> SINGLE Hook reject
MULTI Hook multiple handlers success
SINGLE Hook 0/1 success
SINGLE Hook >1 reject
```

---

# 70. Runtime lifecycle tests

必须有：

```text
Entity mount -> ScriptInstance created
binding installed only at safe point
Hook callback sees implicit self
Entity destroyed during frame -> no dangling callback
unbind deferred until quiescent point
next Hook no longer calls destroyed Entity
Global script lives for Simulation runtime
stop releases code after instances
hot list not mutated while dispatching
```

---

# 71. Worker Event tests

使用真实 worker count > 0。

必须证明：

```text
worker emits events
callback_count remains 0 before dispatch hook
at dispatch hook events delivered
producer/local deterministic order preserved
Hook callback runs AFTER events attached to same hook
no callback runs on worker thread
```

不要使用 `TaskExecutorConfig{0U,...}` 作为这条测试的唯一证据，因为 0 worker 是 caller-thread execution。

---

# 72. No-scan tests

构造：

```text
large Registry
many entities without ScriptComponent
small subset with script
small subset targeted by event
```

验证：

```text
no registry.view<ScriptComponent>() in event dispatch hot path
no full subscriber scan for ENTITY_TARGETED
exact handler count
```

可通过 test-only instrumentation 或 architecture scan + counter 双重证明。

---

# Part XIX — Installed Consumer

# 73. 新增/升级 consolidated installed consumer

建议一个 `simulation-script-binding` 或同等级 consumer，必须只使用 fresh installed packages。

它应：

1. 定义 System A：MULTI Hook + SINGLE Hook + GLOBAL Event；
2. 定义 System B：MULTI Hook + ENTITY_TARGETED Event；
3. 构造 `SystemDescription`；
4. 构造 `SimulationDescription`；
5. 添加 System-level dependency；
6. 枚举 `SimulationHookPointView` / `SimulationEventView`；
7. 构造 ScriptDescription v3 + common exports；
8. 构造 Script binding：同一 function 绑定两个 Hook；
9. Script asset codec encode/decode；
10. LXSD v3 encode/decode；
11. 使用 `BoundScriptCall` direct invoke fixture；
12. 使用 `core/meta` 验证一个 C++ reflected/noexcept function signature；
13. first build/run PASS；
14. second build `no work to do`。

已有浅 consumer 不能替代这条完整 public-surface 证明。

---

# Part XX — Architecture Scans / Negative Probes

# 74. 必须新增 retired API negative probes

至少：

```text
SystemExecutionPoint.hpp missing
SimulationExecutionPointView missing
executionPointCount missing
point-level addDependency overload missing
BROADCAST enum missing
universal ScriptSystem not reintroduced in new L1 path
process-global ScriptEventRegistry not reintroduced
```

旧历史目录如仍保留在 `legacy/`，scan 必须只禁止 production/public dependency，不要求删除历史参考源码。

---

# 75. Source architecture scan

扫描应禁止 production code 出现：

```text
runtime findFunction("...") inside Hook/Event hot dispatch
registry.view<ScriptComponent>() inside ENTITY_TARGETED dispatch
std::function storage for core Hook hot slots
ReflectionRegistry lookup inside invoke loop
shared_ptr ownership changes inside invoke loop
```

注意不要写过度脆弱的字符串 scan；优先检查明确路径/API dependency。

同时修正当前 reactive scan 对 `(hierarchy|transform)` 路径的过度硬编码：禁止 signal 的应该是 foundation/descriptive layer，合法 concrete reactive System 不应因为目录名不是 transform/hierarchy 被拒绝。

---

# Part XXI — 实施顺序

# 76. Phase 0 — 冻结当前基线与旧证据

1. 记录 branch HEAD = `99f984...`；
2. 标记当前 evidence 为 historical candidate；
3. 不删除旧 evidence 文档，但在新 evidence 中明确其已被 API/wire revision supersede；
4. 不在旧 production SHA `8afeb804...` 上追加 freeze 声明。

验收：

```text
no code changed yet
new implementation branch/worktree clean
```

---

# 77. Phase 1 — L0 Script primitives

实施：

```text
ScriptSymbolId
semantic signature/pass vocabulary
BoundScriptCall
signature validation helpers
Meta -> semantic signature adapter
```

保留 `lux_script_abi` C ABI。

先把这层 unit tests 完成。

禁止在此阶段引入 Simulation types。

---

# 78. Phase 2 — `SystemExecutionPoint -> SystemHookPoint`

1. 新增 HookPoint header/type/helper；
2. 迁移所有 built-in System Description；
3. `SystemDescription.execution_points -> hooks`；
4. `SystemEventDescription.dispatch_point -> dispatch_hook`；
5. 加 Event target；
6. 删除旧 header/API；
7. 更新 SystemRegistry validation tests。

这一步结束后 production tree 中不应再有新代码引用 `SystemExecutionPoint`。

---

# 79. Phase 3 — SimulationDescription / Dependency

1. `SimulationExecutionPointView -> SimulationHookPointView`；
2. materialize Hook semantic signatures；
3. materialize Event target/dispatch hook；
4. point-level dependency -> System-level dependency；
5. cycle detection 简化为 System DAG；
6. 更新 builder errors/API/tests；
7. 更新 private compatibility helper。

此阶段必须删除 synthetic decoded TypeToken 构造。

---

# 80. Phase 4 — ScriptDescription v3

1. common `exports[]`；
2. `default_bindings[]`；
3. Native functions 从 backend body 上移；
4. durable semantic signature 去除对 platform size/align 的 truth 依赖；
5. schema bump 3；
6. codec/description tests。

不要同时实现 Python backend。

---

# 81. Phase 5 — Script Asset codec on current AssetCodecSet

1. immutable decoded payload aggregate；
2. codec descriptor；
3. encode/decode；
4. budgets；
5. install/export package；
6. fresh consumer。

禁止恢复 old TAsset hierarchy。

---

# 82. Phase 6 — Global Script mounts in SimulationDescription

1. `ScriptMountDescription`；
2. `global_script_mounts[]`；
3. effective binding mode validation；
4. views；
5. builder；
6. LXSD v3 storage。

Entity ScriptComponent 的 authoring storage 可并行接入，但不要把 runtime instance state 放进 immutable World/Description。

---

# 83. Phase 7 — Binding resolver / runtime plan

优先实现 backend-neutral：

```text
resolve target
validate scope
validate signature
build private dense runtime slot ids
install BoundScriptCall
quiescent add/remove
```

使用 test fake/native function pointer 证明整条链，不先被 Lua 细节阻塞。

---

# 84. Phase 8 — Native/C++ backend integration

这是最容易证明 hot path 的 backend。

要求：

```text
load/resolve once
nonzero symbol id
signature exact match
noexcept C++ direct behavior
BoundScriptCall final 2 pointers
```

完成 real benchmark fixture。

---

# 85. Phase 9 — Lua backend integration

当前 Lua backend 只支持 parse/run chunk，不足以作为最终 bindable function backend。

需要：

```text
load module/chunk once
resolve exported functions once
cache Lua registry references
prepare per-instance state/context
prepare traceback/error handler once
BoundScriptCall thunk
```

Hot path 禁止：

```text
function name lookup
annotation parsing
signature reflection
traceback closure allocation per call
```

Lua annotation parser属于 importer/editor cold path，不属于 VM dispatch。

---

# 86. Phase 10 — FlowForge bridge

最低要求：

```text
FlowForge tooling can enumerate Simulation Hooks/Events
can generate typed entry nodes
can emit ScriptDescription exports/bindings
AOT continues through lux_script_abi
per-instance state block preserved
```

不要求本轮完成整个新 Editor UI，但 public data path 必须可行。

---

# 87. Phase 11 — LXSD v3 + Script wire finalization

在所有 Description field 稳定后再锁 wire layout，避免重复 bump。

完成：

```text
v3 codec
corruption tests
roundtrip
canonical reencode
reject old versions
```

---

# 88. Phase 12 — benchmark v6 / evaluator / policy

一次同步修改：

```text
benchmark output schema
qualification TOML/policy
evaluator
required groups
evidence reporting
```

删除旧 architecture-specific v4 metrics：

```text
EcsChangeBatch
Journal
old simulation_step_execute
old hierarchy delta metric
```

不能留在 current qualification gate。

---

# 89. Phase 13 — full build / install / exact-SHA evidence

形成 production commit：

```text
<NEW_PRODUCTION_SHA>
```

然后从该 SHA 创建 clean detached worktree，执行：

```text
RelWithDebInfo / Developer full build
second build no-work
CTest
fresh install
complete installed consumers

Debug / Developer
full build
second build no-work
CTest

RelWithDebInfo / Hardened
full build
second build no-work
CTest

Android arm64-v8a / PLAYER
full build
second build no-work
fresh install

benchmark v6 formal performance mode
architecture scans
```

所有 evidence 必须指向 `<NEW_PRODUCTION_SHA>`。

最后再提交：

```text
evidence-only commit
parent == <NEW_PRODUCTION_SHA>
```

---

# Part XXII — Freeze Gate

# 90. L1 FROZEN 的必要条件

必须同时满足：

```text
[ ] SystemExecutionPoint fully retired
[ ] SystemHookPoint semantics implemented
[ ] Hook signatures implemented
[ ] SystemEvent GLOBAL/ENTITY_TARGETED semantics implemented
[ ] no BROADCAST scene scan
[ ] Event dispatch_hook safe-point semantics proven
[ ] System-level dependencies implemented
[ ] point-level dependency API retired
[ ] ScriptDescription v3 common exports
[ ] Script Asset codec restored on current asset architecture
[ ] BoundScriptCall foundation primitive
[ ] binding cold-path signature validation
[ ] same function -> multiple Hook bindings proven
[ ] Entity implicit self proven
[ ] GLOBAL/ENTITY event scope validation
[ ] no reflection/string/hash lookup in hot path
[ ] no scene scan in targeted dispatch
[ ] real worker -> safe hook -> event callback test
[ ] real hook/event benchmark v6
[ ] current evaluator/policy v6
[ ] comprehensive fresh installed consumer
[ ] Windows build matrices pass
[ ] Android build/install pass
[ ] exact production SHA clean verification
[ ] evidence-only parent relationship correct
[ ] independent public API acceptance pass
```

只有此时：

```text
L1 = FROZEN
```

然后才允许：

```text
Formal L2 Process public implementation/dependency = GO
```

---

# Part XXIII — 明确禁止项

# 91. 实施者不得添加

以下全部禁止，除非未来另有新的 normative architecture decision：

```text
universal ScriptSystem
global ScriptRuntime singleton
process-global ScriptEventRegistry
generic EventBus for System->Script calls
SystemHookRegistry manager
runtime function-name auto binding
scene-wide BROADCAST
point-level scheduler dependency through Hook
std::function hot Hook storage
runtime reflection in invoke loop
runtime asset lookup in invoke loop
runtime unordered_map function lookup in invoke loop
shared_ptr churn in invoke loop
compat SystemExecutionPoint alias
compat old point-level dependency overload
compat LXSD v2 decoder solely for pre-freeze assets
old TAsset-based ScriptAsset hierarchy
fake TypeToken reconstructed from serialized schema identity
Registry& in default Script Hook/Event signature
Python-specific abstraction in Simulation
Lua-specific abstraction in Simulation
MLIR-specific abstraction in Simulation
```

---

# Part XXIV — 推荐目标文件布局

# 92. 仅作为施工导航，不是要求新增多余模块

建议最终 production tree 近似：

```text
modules/function/script/core/
    include/lux/engine/function/script/
        ScriptValue.hpp
        ScriptSignature.hpp
        ScriptSymbolId.hpp
        BoundScriptCall.hpp
        ScriptCallFrame.hpp
        abi/lux_script_abi.h
    ...

modules/core/meta/
    existing reflection runtime
    optional script signature adapter support

modules/resource/description/
    include/lux/engine/description/Script.hpp   // schema v3

modules/resource/asset/
    script/ or equivalent pay-for-use codec
        ScriptAssetPayload.hpp
        ScriptAssetCodec.hpp
        codec source/tests

engine/simulation/description/
    include/lux/engine/simulation/
        SystemHookPoint.hpp
        SystemEventDescription.hpp
        SystemDescription.hpp
        SimulationDescription.hpp
        SimulationDescriptionBuilder.hpp
        SimulationStepInfo.hpp

engine/simulation/asset/
    SimulationAssetCodec.hpp
    LXSD v3 source/tests

engine/simulation/<private runtime composition area>/
    private binding plan / hook slots / event buffers as needed
```

不要为了照抄此树而创建无实际职责的 `manager/registry/core/common` 目录。

---

# Part XXV — 实施示例（Normative Semantics）

# 93. System 声明

概念：

```cpp
struct PhysicsSystem
{
    inline static constexpr auto BeforeStep =
        makeSystemHookPoint<void(const SimulationStepInfo&)>(
            "before_step",
            ESystemHookCardinality::MULTI
        );

    inline static constexpr auto AfterStep =
        makeSystemHookPoint<void(const SimulationStepInfo&)>(
            "after_step",
            ESystemHookCardinality::MULTI
        );

    inline static constexpr auto CollisionBegin =
        makeSystemEvent<CollisionBeginPayload>(
            "collision_begin",
            AfterStep,
            ESystemEventTarget::ENTITY_TARGETED,
            "lux.physics.CollisionBegin",
            1
        );

    inline static constexpr std::array Hooks{
        BeforeStep,
        AfterStep,
    };

    inline static constexpr std::array Events{
        CollisionBegin,
    };

    inline static constexpr SystemDescription Description{
        .canonical_name = "lux.physics",
        .version = 1,
        .hooks = Hooks,
        .events = Events,
    };
};
```

实际 helper/template 细节可为 C++20 constexpr 限制微调，但 public semantic 不得改变。

---

# 94. ScriptDescription

概念：

```text
Script
    module_name = "PlayerController"

exports:
    symbol 1001
        name = "on_collision"
        signature = void(const CollisionBeginPayload&)

    symbol 1002
        name = "print_stage"
        signature = void(const SimulationStepInfo&)

default_bindings:
    1001 EVENT lux.physics / collision_begin
    1002 HOOK  lux.physics / before_step
    1002 HOOK  lux.physics / after_step
```

`print_stage` 被绑定两次是合法且预期行为。

---

# 95. Entity runtime

```text
Entity E
  Script mount PlayerController

bind:
  on_collision
      -> Physics.CollisionBegin targeted slot for E

  print_stage
      -> Physics.BeforeStep HookRuntimeSlot subscriber list with context self=E
      -> Physics.AfterStep HookRuntimeSlot subscriber list with context self=E
```

Physics step：

```text
BeforeStep dispatch hook-events(if any)
BeforeStep Hook handlers
    print_stage(self=E)

physics worker tasks
    emit CollisionBegin(target=E)

AfterStep
    merge CollisionBegin
    on_collision(self=E, payload)
    then AfterStep Hook handlers
    print_stage(self=E)
```

同一个 Entity script 在一个 step 内被调用三次完全合法。

---

# 96. 两个 System 的边界

Simulation dependency：

```text
PhysicsSystem -> RenderSystem
```

脚本：

```text
print_stage -> Physics.after_step
print_stage -> Render.before_render
```

运行：

```text
Physics body
Physics.after_step -> print_stage()

TaskGraph/System dependency boundary

Render.before_render -> print_stage()
Render body
```

两个 Hook 即使在 wall-clock 上相邻，也仍是两个独立 semantic invocation，函数执行两次。

不得把它们合并成：

```text
BetweenPhysicsAndRender
```

---

# Part XXVI — 代码审阅 Checklist

# 97. Reviewer 必须逐项检查

## Architecture

```text
[ ] Hook 不参与 TaskGraph dependency identity
[ ] Event 不成为 EventBus
[ ] Entity-targeted dispatch 不扫场景
[ ] Global dispatch 不扫 Entity
[ ] Script backend 不泄漏到 Simulation public headers
[ ] World/Description 不保存 runtime pointers
[ ] no generic manager/service locator
```

## API

```text
[ ] old ExecutionPoint headers removed
[ ] all public names use HookPoint
[ ] addDependency is System-level
[ ] Script export common across backend
[ ] binding explicit and serializable
[ ] no automatic name binding
```

## Safety

```text
[ ] Meta signature exact matching
[ ] no fake TypeToken on decode
[ ] C++ direct callbacks noexcept
[ ] no Registry& default script signature
[ ] binding mutation quiescent
[ ] module lifetime covers BoundScriptCall
```

## Performance

```text
[ ] BoundScriptCall 2 pointers
[ ] frame once per occurrence
[ ] no reflection in hot loop
[ ] no string/hash lookup in hot loop
[ ] no allocation after prepare
[ ] targeted event O(actual targets/callbacks)
```

## Qualification

```text
[ ] benchmark schema/policy/evaluator same version
[ ] real direct invoke benchmark
[ ] worker safe-point test
[ ] fresh installed consumer
[ ] exact-SHA evidence
```

---

# Part XXVII — 最终裁决

# 98. 本文的冻结意图

本文不是一次新的“大架构扩张”。

其目标恰恰是删除当前候选中混在一起的三个概念：

```text
System execution hook
System event occurrence
System scheduling dependency
```

并把旧 Script 实现中已经验证过的高性能直接调用机制，放回正确的分层：

```text
SystemHookPoint / SystemEvent
        ↓ semantic description
ScriptBindingDescription
        ↓ cold validation/prepare
BoundScriptCall
        ↓ hot direct invoke
```

最终工程应呈现：

```text
World = facts
    Entity may carry Script mount facts

Simulation = rules
    Systems
    SystemHookPoints
    SystemEvents
    System dependencies
    Global Script mounts

ScriptAsset = callable code + manifest

Binding = composition data

TaskGraph = scheduling

Process = future asynchronous/stateful orchestration
```

只要本文全部施工并通过新 exact-SHA qualification，就不应再对 L1 做另一轮泛化重构。

下一架构阶段应转入：

```text
L2 Process
```

而不是继续在 L1 引入新的 registry、manager、bus 或 compatibility layer。

---

# Appendix A — 本轮可直接复用的当前/历史代码锚点

当前分支 `99f984...`：

```text
engine/simulation/description/include/lux/engine/simulation/SystemExecutionPoint.hpp
engine/simulation/description/include/lux/engine/simulation/SystemEventDescription.hpp
engine/simulation/description/include/lux/engine/simulation/SystemDescription.hpp
engine/simulation/description/include/lux/engine/simulation/SimulationDescription.hpp
engine/simulation/description/include/lux/engine/simulation/SimulationDescriptionBuilder.hpp
engine/simulation/asset/
modules/function/script/core/
modules/core/meta/
modules/resource/description/include/lux/engine/description/Script.hpp
modules/resource/asset/include/lux/engine/resource/asset/AssetCodecSet.hpp
```

历史机制参考（只借机制，不恢复旧架构）：

```text
old ScriptAsset = description + payload
old BoundScriptCall = invoke + context
old zero-filter subscriber index
old dispatchTo(entity)
legacy FlowForge FuncDef/OnEvent typed nodes
legacy FlowForge StateLayout host-owned per-instance state
old direct-dispatch performance ADR
```

---

# Appendix B — 实施方遇到歧义时的默认裁决

如果实施者在两种实现中犹豫，按以下优先级：

```text
1. 语义分层正确
2. hot path 最短
3. public API 最少
4. no global ownership
5. no compatibility baggage
6. current real use case over speculative flexibility
```

典型例子：

```text
“要不要给 Hook 建全局 registry？”
    -> 不要。

“要不要 Event 加 BROADCAST？”
    -> 不要。

“要不要 runtime 按函数名自动绑定？”
    -> 不要。

“要不要为了多个 return handler 设计 reducer framework？”
    -> 不要；MULTI void，SINGLE <=1 handler。

“要不要让 Script 回调直接拿 Registry&？”
    -> 不要。

“要不要保留旧 SystemExecutionPoint alias？”
    -> 不要。

“要不要为了 Python 现在做统一 VM manager？”
    -> 不要。

“需要跨语言通用什么？”
    -> Description、signature、binding、ABI、BoundScriptCall。
```

---

**End of Normative Specification**
