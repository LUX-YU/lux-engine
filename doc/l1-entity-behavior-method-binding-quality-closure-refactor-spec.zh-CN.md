# Lux Engine L1 EntityBehavior / Generic Meta Projection / Composition Binding
# 独立代码质量闭环与最终重构实施规范（Revision 2）

**文档性质：Normative Refactor & Implementation Specification（规范性重构/实施文档）**  
**日期：2026-08-27**  
**目标仓库：`LUX-YU/lux-engine`**  
**当前实施分支：`codex/l1-system-hook-script-binding`**  
**审查时分支 HEAD：`2bf05234c920b1a86a195e97ec68fc50be1cb5a6`**  
**当前被 qualification 覆盖的 production commit：`393180e9a67e01c9e6863b4d0bde4a8849b05b0c`**  
**上一轮基线：`99f984cf0eb0ee75f45d21a5f1dd5d7c60c4b9be`**  
**被本文修订的规范：`doc/l1-system-hook-script-binding-final-implementation-spec.zh-CN.md`**  
**Revision 2 核心修正：C++ `LUX_METHOD` 保持 generic Meta 语义；`RefMethod -> ScriptExportDescription` 由 Engine/Simulation bridge 完成。**

---

# 0. 结论先行

对 `codex/l1-system-hook-script-binding` 当前 exact-SHA 实现进行独立架构、语义和代码质量审查后：

```text
Current exact-SHA build/performance qualification:     STRONG
Current semantic/code-quality independent acceptance:  FAIL
Current L1 freeze status:                              NO-GO
Formal L2 Process:                                     BLOCKED
```

当前 HEAD `2bf052...` 是 evidence-only commit；其 parent `393180...` 是实际 production code。本文的代码质量判断全部以 `393180...` 为准。`2bf052...` 中的构建/测试/性能结果保留为历史 evidence，但不能继续作为 freeze 通过证据。

当前实现中以下成果应保留：

```text
SystemExecutionPoint -> SystemHookPoint                  正确
Hook 与 scheduler dependency 分离                       正确
System-level dependency                                  正确
SystemEvent GLOBAL / ENTITY_TARGETED                     正确方向
ScriptDescription common exports                         正确方向
ScriptAsset 使用当前 AssetCodecSet                       正确
BoundScriptCall = { invoke, context }                    正确
entity-targeted flattened event-major ranges             正确优化方向
benchmark schema v6 + formal 30 samples                   明显改善
exact-SHA clean build/install qualification               方法正确
FlowForge 从 SimulationDescription 获取 typed targets    Editor/catalog 方向正确
```

但是独立审查发现以下 freeze blocker：

```text
P0-1  延迟 Event occurrence 借用 call_frame / payload 指针，存在悬空/UAF 风险
P0-2  Entity mount -> MULTI Hook 被实现直接拒绝，与上一规范冲突
P0-3  Event signature matcher 未比较 CONST_REF pass mode
P0-4  primitive semantic canonical name 分裂：lux.i32 vs i32
P0-5  entity-targeted sidecar 只按 entity index，不校验 generation / exact Entity
P0-6  Lua backend 把 Lua 业务返回值当 ABI status，未写 frame.returns
P0-7  Lua unsupported STRUCT_REF 等没有在 bind 阶段拒绝，首次调用才失败
P0-8  真实 worker -> safe Hook 事件语义未被真实测试证明
P0-9  installed consumer 只证明“创建失败”，没有证明 public runtime 成功绑定/调用
```

此外，本轮实施之后我们又进一步收敛了 gameplay programming model。本文正式采用：

```text
EntityBehavior = opt-in per-Entity long-lived behavior object
没有 ScriptComponent = 不创建 Behavior、不进入 Behavior lifecycle
ECS System 与 EntityBehavior 共存：
    System         = 大规模、同构、data-oriented 规则
    EntityBehavior = 少量/局部/富状态 gameplay object context

各语言 frontend 只声明/产出可供 Engine 组合的 callable candidate：
    C++ member  LUX_METHOD() -> generic Meta RefMethod；当 C++ ScriptAsset 选择该 RefClass 时由 Engine projection 为 export
    C++ free    LUX_FUNC()   -> generic Meta RefFunction；由 C++ Script descriptor 显式选择后 Engine projection 为 export
    Python   @lux.method -> static AST importer
    Lua      ---@lux.method -> source importer
    FlowForge explicit exported entry

C++ 特别规则：
    LUX_METHOD() 不是 Script-specific annotation。
    它继续只表示 generic Meta method reflection opt-in。
    Script/Meta foundation 彼此无直接依赖。
    Engine/Simulation C++ Script bridge 同时消费 RefMethod 与 ScriptDescription vocabulary，
    将选定 RefMethod 单向投影为 ScriptExportDescription。

不再存在：
    bind_point annotation
    bind_event annotation
    lifecycle annotation
    ScriptAsset.default_bindings
    function-name automatic binding

所有实际绑定都属于 composition：
    ScriptComponent / Entity mount
    SimulationDescription / Global mount

Editor 只按 receiver/scope + parameters + returns + cardinality 过滤 compatible targets。
函数名只用于 diagnostics。
同一个 method MAY 绑定多个 compatible targets。
```

一句话：

```text
Export once.
Instantiate once.
Bind explicitly.
Prepare once.
Invoke many.
Retire once.
```

---

# 1. 文档优先级与实施纪律

对于以下主题，本文优先级高于上一份 `l1-system-hook-script-binding-final-implementation-spec`：

```text
ScriptDescription v3
ScriptAsset script wire v1
ScriptMountDescription / ScriptMountFacts
EScriptBindingSetMode / default_bindings
ScriptBindingSession event buffering
backend prepare-per-binding API
Meta/Script dependency direction、engine-side RefMethod projection 与 prepared C++ method invoke
CppBehaviorScript
Lua binding backend
FlowForge default binding generation
authoring annotations
Entity script lifecycle
```

继续有效的根原则：

```text
World      = Facts
Simulation = Rules + synchronous mechanisms
Process    = asynchronous/stateful orchestration
Scene      = World + Simulation + Process/run-root composition

World never owns ECS.
World Object != ECS Entity.
TaskGraph remains the only generic CPU scheduler/composition graph.
Product/Host owns capacities and budgets.
Asset payload remains immutable Description data.
No compatibility architecture solely for pre-freeze call sites.
No global service locator / generic event bus / global script manager.
```

MUST / MUST NOT / SHOULD / MAY 按规范性含义使用。实施者遇到本文未定义需求时：

```text
DO NOT invent a manager.
DO NOT invent a global registry.
DO NOT add compatibility aliases.
DO NOT add another annotation family.
DO NOT silently weaken signature validation.
DO NOT silently widen runtime component access.
```

## 1.1 Revision 2 三项冻结决策

本轮实施方 **没有选择权**，以下三项按本文唯一口径执行：

```text
A. generic Meta / LUX_METHOD
   LUX_METHOD 保持现有 generic method-reflection marker 语义；LUX_FUNC 保持 generic free-function reflection语义。
   不改名，不变成 Script-only marker，不增加第二个 C++ Script export annotation。
   C++ member export路径 = RefMethod -> Engine/Simulation bridge -> ScriptExportDescription。
   C++ free/global export路径 = RefFunction -> Engine/Simulation bridge -> ScriptExportDescription。
   core/meta MUST NOT depend on Script；function/script MUST NOT depend on Meta。

B. Editor
   本轮不创建临时 GUI/Editor application。
   MUST 完成 UI-agnostic authoring/binding catalog：
       exported callable enumeration
       bindable target enumeration
       exact compatibility filtering
       explicit binding construction/mutation
       dangling diagnostics
   FlowForge 与未来 canonical Editor MUST 复用同一 compatibility truth。

C. Python
   本轮固定为 Tier 1：静态 AST importer + ScriptDescription/ScriptAsset authoring support。
   MUST NOT 实现 CPython runtime backend、GIL/PyObject runtime、Python hot dispatch。
   Runtime mount PYTHON_SOURCE 在没有 backend 时 cold-fail BACKEND_NOT_AVAILABLE。
```

---

# Part I — Exact-SHA 独立代码质量审查

# 2. 审查基线

```text
branch HEAD / evidence:
2bf05234c920b1a86a195e97ec68fc50be1cb5a6

qualified production parent:
393180e9a67e01c9e6863b4d0bde4a8849b05b0c

previous implementation baseline:
99f984cf0eb0ee75f45d21a5f1dd5d7c60c4b9be
```

`393180...` 已通过较强的 build/performance qualification，但 independent semantic acceptance 现为 **FAILED**。完成本文后必须产生全新的 production SHA 与 evidence；不得在当前 evidence 上追加“已修复”说明后继续 freeze。

# 3. 通过审查、应保留的现有设计

## 3.1 SystemHookPoint

保留：

```text
name
SINGLE / MULTI cardinality
typed Script semantic signature
unsupported mutable ref / rvalue ref / pointer rejection
MULTI non-void rejection
```

不得重新退回 name-only ExecutionPoint。

## 3.2 SystemEvent

保留：

```text
dispatch_hook
GLOBAL
ENTITY_TARGETED
durable payload schema identity
```

永久禁止把 `BROADCAST` 定义成 scene/entity scan。

## 3.3 System dependency

保留：

```text
System A -> System B
```

Hook 不进入 scheduler DAG。

## 3.4 BoundScriptCall

保留且冻结 hot record：

```cpp
struct BoundScriptCall final
{
    lux_script_invoke_fn invoke;
    void* context;
};
```

64-bit 目标：16 bytes，trivially copyable。本文不扩充它。

## 3.5 ScriptAsset current AssetCodecSet 路径

保留：

```cpp
struct ScriptAssetContent final
{
    rdesc::Script description;
    std::vector<std::byte> payload;
};
```

不恢复旧 `TAsset<Script>` / AssetInfo / ScriptSerDeser hierarchy。

## 3.6 flattened targeted ranges

保留 hot-path 思想：

```text
target exact Entity
 -> sidecar
 -> event-major HandlerRange
 -> flat prepared-call ordinals
```

但必须修 exact generation identity 和 instance lifecycle。

## 3.7 qualification methodology

继续采用：

```text
clean detached exact-SHA worktree
full build
second build no-work
CTest
fresh install
installed architecture scan
Android PLAYER closure
TOOLCHAIN closure
formal benchmark warmup + 30 samples
benchmark exact commit embedding
evidence-only commit parent == production SHA
```

当前问题是测试覆盖/语义缺口，不是该 methodology 本身。

# 4. P0-1 — 延迟 Event occurrence 借用 frame 指针

当前 `ScriptBindingSession::State::Occurrence` 保存：

```cpp
const lux_script_value_slot* args;
lux_script_value_slot* returns;
uint32_t arg_count;
uint32_t return_count;
```

`emit()` 仅记录调用者 frame 中的指针，稍后 `dispatchHook()` 才读取。典型 UAF：

```cpp
void worker()
{
    CollisionEvent payload{...};
    lux_script_value_slot slot{..., &payload};
    lux_script_call_frame frame{&slot, ...};
    writer.emit(event, entity, frame);
} // payload + slot 死亡

// later
session.dispatchHook(...); // dangling
```

当前 test 让 payload/slot 一直活到 dispatch，因而漏测。

**裁决：MUST 删除 Session 内 future call-frame pointer retention。** 未来 Event payload 由 System-owned typed buffer 真正拥有；Session 只做同步 dispatch，详见 Part IX。

不得把“调用者保证 frame 活到 Hook”写成修复方案。

# 5. P0-2 — Entity -> MULTI Hook 被错误拒绝

上一规范允许：

```text
Entity mount MAY bind MULTI Hook
Entity mount MAY bind ENTITY_TARGETED Event
Entity mount MUST NOT bind SINGLE Hook (v1)
Entity mount MUST NOT bind GLOBAL Event
```

当前 `appendBinding()` 却对任何 Entity Hook 直接 `SCOPE_MISMATCH`。

新 scope contract：

```text
GLOBAL_MODULE:
    MULTI Hook       YES
    SINGLE Hook      YES
    GLOBAL Event     YES
    Lifecycle        NO
    ENTITY Event     NO

ENTITY_BEHAVIOR:
    MULTI Hook       YES
    SINGLE Hook      NO (first version)
    ENTITY Event     YES
    Lifecycle        YES
    GLOBAL Event     NO
```

# 6. P0-3 — Event pass mode 未检查

non-void Event 的唯一 canonical Script signature：

```text
void(const Payload&)
```

因此参数 pass 必须 `CONST_REF`。当前 matcher 只比 `canonical_name + type_id`；当前 FlowForge catalog/test 还使用 `VALUE`。

**裁决：**新增唯一 `eventScriptSignature()` / equivalent helper，Editor、FlowForge、runtime matcher、tests 全部复用。不得各写一份推导逻辑。

# 7. P0-4 — primitive semantic namespace 分裂

当前 Script core 使用：

```text
lux.bool
lux.i32
lux.u32
lux.i64
lux.u64
lux.f32
lux.f64
```

当前 Meta adapter/Native fixtures 使用：

```text
bool
i32
u32
i64
u64
f32
f64
```

所以 `makeSystemHookPoint<void(float)>` 与 reflected `void Foo(float)` 产生不同 semantic identity。

**裁决：**建立 Script semantic builtin canonical name 的唯一 source of truth；所有 traits、adapter、fixture、C++ codegen、Lua/Python importer、FlowForge compiler 只引用它。源码不得散落裸 `"i32"/"f32"` 作为 Script identity。

# 8. P0-5 — entity-targeted sidecar 忽略 generation

当前 mapping 使用 `entt::to_entity(entity)` index，生产优化还移除了 sidecar 内 exact Entity。若 slot 被重用：

```text
old Entity(index=42,generation=3) destroyed
new Entity(index=42,generation=4) created
```

旧 sidecar 在 quiescent rebuild 前仍可能命中新 Entity。

**裁决：**sidecar slot 必须保存 exact Entity（含 generation）：

```cpp
struct EntityDispatchSlot
{
    ecs::Entity owner{ecs::NullEntity};
    uint32_t sidecar{Invalid};
};
```

hot targeted lookup必须检查 `slot.owner == target`。Entity/ScriptComponent 进入 destruction 时立即 mark runtime `RETIRING`；可延迟 erase，但 normal invoke 必须 skip。

# 9. P0-6 — Lua business return 与 ABI status 混淆

`lux_script_invoke_fn` 的 `int` 是 backend invocation status；Script callable 的业务返回值属于 `frame.returns[]`。当前 Lua固定 `lua_pcall(..., 1)` 并把 numeric return 当 status，导致 non-void SINGLE Hook 语义错误。

MUST：

```text
Lua call success -> ABI status 0
business return -> marshal to frame.returns
Lua error -> ABI nonzero
```

不得再混用。

# 10. P0-7 — Lua unsupported marshal 首次调用才失败

当前 Lua invoke只处理 primitive；`STRUCT_REF` 在 invoke 返回错误，但 prepare 不拒绝。

MUST：backend prepare 完整验证其 marshal capability。不能支持的类型在 cold bind 返回 `UNSUPPORTED_MARSHAL_TYPE`，不得第一次真实 callback 才发现。

本轮 SHOULD 支持 canonical reflected record `STRUCT_REF` input；若存在明确阻塞，必须 cold reject 并记录 acceptance gap。

# 11. P0-8 — worker -> safe Hook 未真实证明

上一规范要求 worker_count > 0 的真实测试。当前 typed-event ordering 是 vector 人工 merge，不是 worker execution。

新 acceptance 必须证明：

```text
worker emits typed owned payload
before safe Hook callback_count == 0
producer stack can end safely
safe Hook drains on safe thread
producer ordinal/local sequence deterministic
Event callbacks before same Hook callbacks
```

# 12. P0-9 — installed consumer 没有成功 runtime path

当前 consolidated consumer 传空 resolver/backends，然后断言 Session create 失败。它没有证明 fresh-installed public API 可实际：

```text
export -> asset -> binding -> backend instance -> prepare -> lifecycle -> Hook/Event invoke
```

新 consumer 必须成功执行完整 path。

# 13. P1 — mount mutation 全 Session rebuild

当前任何 ScriptMountFacts construct/update/destroy 都只设置 `mounts_dirty=true`；quiescent apply 重新扫描所有 mounts、release/rebuild 所有 prepared calls。

引入 long-lived Behavior 后这是不可接受的：修改 Entity A 的 binding 不能重建 Entity B 的 object/state。

允许 quiescent 时重建 entire **dense DispatchIndex**；但 **MUST preserve unchanged backend instances and private state**。

# 14. P1 — mount ordinal 不能做 instance identity

当前 backend key：

```text
AssetId + Entity + mount_ordinal
```

vector insert/reorder会改变 ordinal。新增 durable `ScriptMountId`，nonzero、serialized、owner scope 内 unique、跨 reorder 稳定。实例 identity 使用 exact Entity + MountId（global 则 Simulation scope + MountId）。

# 15. P1 — backend prepare 是 per-binding

同一 method绑定三个 Hook 当前会 prepare 三次并生成三份 Call context。新模型必须：

```text
one mount -> one backend instance
one unique ScriptSymbolId -> prepareMethod once
N bindings -> reuse same BoundScriptCall
```

# 16. P1 — Lua source 当前 per Entity load/execute

同一 Lua asset挂 10,000 Entity 当前会 load/execute chunk 10,000 次。

新模型：

```text
LoadedLuaModule/prototype: once per asset/session
Lua instance: once per mount
PreparedLuaMethod: once per unique exported symbol used by mount
```

# 17. P1 — Native ABI validation 不完整

Native cold prepare 必须比较：

```text
arg/return count and order
canonical name
type_id
value kind
size
align
```

semantic `VALUE/CONST_REF` 由 ScriptDescription vs target exact matcher保证。C ABI layout不因本轮对象模型自动修改。

# 18. P1 — state alignment 缺失

Native/FlowForge state manifest必须恢复显式 `state_align`：>=1、power-of-two、allocation真正满足。legacy FlowForge已有 size/align/hash/defaults 的正确思想。

# 19. P1 — qualification counter 是 literal zero

当前 public `hotPathNameLookupCount/AssetLookupCount/SceneScanCount` 直接返回 0，不能作为 evidence。

删除这些 public diagnostic APIs。用 private test instrumentation + source scan + sparse-registry behavior test 证明结构。

# 20. P1 — targeted benchmark不够 sparse

当前 measured entities 全部带 ScriptMountFacts。新 benchmark必须有大量无脚本 Entity + 少量 scripted subset，证明 targeted cost不随 total scene size线性增长。

# 21. P1 — Session 责任过重

Public facade仍可只有 `ScriptBindingSession`，private implementation至少拆成逻辑层：

```text
BindingResolver   cold asset/target/signature resolve
InstanceTable     long-lived backend instances/lifecycle/diff
DispatchIndex     dense Hook/Event hot ranges
```

不得因此增加 public manager。Event producer buffer不再属于 Session。

# 22. P1 — backend context lifetime 与 duplicate kind

第一版 public contract必须写明 backend objects outlive Session，Session先销毁。`create()` 冷验证 backend kind unique；duplicate kind必须 `DUPLICATE_BACKEND_KIND`，不允许 silent first-wins。

若仓库已有成熟 code-lifetime lease 可复用，MAY 使用；不要建立 global backend registry。

# 23. P1 — duplicate export diagnostic name不应禁止

v4 只要求 `ScriptSymbolId` unique；`name` non-empty 但 MAY duplicate，以支持 C++ overload。Editor显示完整 signature 区分。

# 24. P1 — enum raw validation

所有 wire-decoded enum显式 range check：EventTarget、ScriptModel、binding target kind/lifecycle、pass mode等。

# 25. Meta / Script / Engine bridge 的最终边界

现有 `modules/core/meta_script` 把 `core/meta` 与 `function/script_core` 直接耦合，并把 Script-specific adapter 放在 CORE/FOUNDATION；该模块身份 **MUST 退休**。

但其中两类机制要区分处理：

```text
RefMethod/RefFunction -> Script semantic signature / ScriptExportDescription projection
    -> 保留思想，迁移到 Engine/Simulation C++ Script bridge

ReflectedScriptCall runtime wrapper
    -> 不作为 Foundation truth；若仍有 tooling consumer 可留在非-Core bridge/tooling，
       EntityBehavior normal path不依赖它的 dynamic lookup/heap setup
```

冻结依赖方向：

```text
core/meta
    MUST NOT depend on function/script or ScriptDescription

function/script/core
    MUST NOT depend on core/meta or Simulation

resource/description/Script
    MUST NOT depend on core/meta or Simulation targets

engine/simulation C++ Script bridge
    MAY depend on:
        core/meta
        function/script/core
        resource/description/Script
        SimulationDescription / Behavior runtime vocabulary
```

C++ 的 `RefMethod/RefFunction` 是 language/runtime reflection truth；`ScriptExportDescription` 是 durable、语言无关的序列化投影。关系是单向：

```text
RefMethod / RefFunction
   -> validate Script-callable subset
   -> canonicalize semantic types
   -> ScriptExportDescription
```

绝不能把 `RefMethod* / RefType* / MethodInvoker` 序列化进 ScriptAsset/LXSD。

---
# Part II — 最终游戏逻辑模型

# 26. ECS-first，Behavior opt-in

最终：

```text
ECS Entity       = data identity
Components       = canonical world state
System           = bulk/data-oriented rules
EntityBehavior   = optional per-Entity object-oriented gameplay context
ScriptComponent  = authored fact mounting one or more ScriptAssets onto Entity
```

**MUST NOT：**

```text
every Entity automatically owns EntityBehavior
every Entity allocates script/object sidecar
Entity becomes UObject/Actor wrapper
```

没有 `ScriptComponent`：

```text
0 Script instance allocation
0 Behavior lifecycle call
0 Hook subscription
0 Event subscription
0 Behavior-specific runtime state
```

# 27. ECS System 与 EntityBehavior 共存

推荐：

```text
EnemyMovementSystem
    query EnemyTag + Transform + Velocity
    batch update thousands of enemies

BossBehavior
    only exists on selected Boss entity
    owns phase/timer/dialogue local state
    reacts to selected typed targets
```

权威状态规则：

```text
Components = authoritative shared gameplay/world state
Behavior fields = private transient behavior state
```

错误：

```cpp
class EnemyBehavior {
    Transform transform_; // duplicate canonical ECS state
    Health health_;
};
```

正确：

```cpp
class EnemyBehavior {
    float attack_cooldown_{};
    Entity current_target_{};

    LUX_METHOD()
    void tick(const SimulationStepInfo&) noexcept {
        const auto& t = getComponent<Transform>();
        ...
    }
};
```

开发指导：

```text
同类 Entity 的大规模规则 -> System
少量/独特 Entity 的对象化局部逻辑 -> EntityBehavior
大规模 visual/MLIR compute -> compiled batch/System path
```

不要用 100,000 Behavior 替代本应由一个 System 批处理的逻辑。

# 28. EntityBehavior 是语义，不是统一内存布局

backend memory：

```text
C++       stable C++ object / aligned allocation / optional pool
Lua       VM instance table/userdata + registry refs
Python    PyObject* (future)
FlowForge state block
Native    backend-specific state block
```

Lux 统一：

```text
instance identity
lifecycle
hidden self
signature
binding
call ABI
```

Lux 不统一：

```text
sizeof object
allocator
GC
VM representation
```

禁止：

```cpp
std::byte universal_behavior_storage[256];
```

放进 ECS component。

---

# Part III — ScriptDescription v4：只描述 callable code

# 29. 删除 ScriptAsset-owned binding

当前 v3：

```text
exports[]
default_bindings[]
```

v4：

```text
exports[]
NO default_bindings
```

ScriptAsset只回答：

> 我提供哪些 Engine-visible callable？

Composition回答：

> 当前 Entity/Simulation 将这些 callable 绑定到哪里？

这两个 source of truth 不再混合。

# 30. `EScriptModel`

新增：

```cpp
enum class EScriptModel : std::uint8_t
{
    GLOBAL_MODULE,
    ENTITY_BEHAVIOR,
};
```

目标 `rdesc::Script`：

```cpp
class Script final
{
public:
    static constexpr std::uint32_t kSchemaVersion = 4U;

    std::uint32_t schema_version{kSchemaVersion};
    std::string module_name;
    EScriptModel model{EScriptModel::GLOBAL_MODULE};

    std::vector<ScriptFunction> exports;
    std::vector<ScriptDependency> dependencies;
    ScriptProvenance provenance;
    Body body;
};
```

第一版明确：

```text
one ENTITY_BEHAVIOR ScriptAsset = one behavior/prototype receiver root
```

不支持一个 ScriptAsset 内多个不同 Behavior classes。需要多个行为时建立多个 ScriptAssets。不要本轮引入 BehaviorId/sub-behavior registry。

# 31. `ScriptFunction`

保持：

```cpp
struct ScriptFunction final
{
    std::string name; // diagnostic only
    ScriptSymbolId symbol_id;
    std::vector<ScriptValueType> args;
    std::vector<ScriptValueType> returns;
};
```

规则：

```text
symbol_id != 0
symbol_id unique
name non-empty
name need NOT be unique
hidden EntityBehavior self NOT in args
```

对于 `ENTITY_BEHAVIOR`，全部 exports 都是该 behavior receiver 的 methods。对于 `GLOBAL_MODULE`，全部 exports 都是无 Entity receiver 的 module/free/static callables。

# 32. `ScriptSymbolId` API 与 type id 分离

底层哈希算法 MAY 继续相同，但 API 必须分开：

```cpp
scriptSemanticTypeId(canonical_type_name)
scriptSymbolId(canonical_symbol_identity)
```

禁止再用 `scriptSemanticTypeId()` 为 method/function mint symbol。

C++ codegen推荐 symbol canonical input：

```text
fully-qualified declaring type/module
+ source method/function name
+ semantic signature
```

这样 overload 可区分。函数 rename MAY 改变 SymbolId；本轮不增加 persistent method UUID annotation。Editor显示 old binding unresolved并让用户重选，绝不按名字猜。

# 33. backend body

## 33.1 Lua

```cpp
struct LuaSourceScript final
{
    std::string entry;
};
```

exports 只在 common `Script.exports`。

## 33.2 Native/FlowForge AOT

保留并修：

```text
abi_version
state_layout_hash
state_size
state_align
state_defaults
```

`state_align` >= 1 且 power-of-two。

## 33.3 C++ statically compiled script

当前 `CppBehaviorScript` 语义过窄。推荐重构为：

```cpp
struct CppStaticScript final
{
    std::string descriptor;
};
```

`Script.model` 区分 global/behavior。若为了 wire enum value 最小 churn 保留旧名字，只能作为内部历史命名；public semantic不能因此限制 C++ global module。

# 34. ScriptDescription v4 validation

MUST：

```text
schema == 4
module_name non-empty
ScriptModel raw enum valid
Kind raw enum valid
export name non-empty
export symbol nonzero + unique
semantic type name/id/pass valid
returns count supported
Native state_align valid
state_defaults.size <= state_size
```

删除：

```text
default binding reference validation
duplicate function name rejection
```

---

# Part IV — Language frontend -> Script export

# 35. C++：复用 generic Meta 的 `LUX_METHOD()` / `LUX_FUNC()`

`LUX_METHOD(...)` 的唯一语义继续是：

> 将该 C++ member method 标记为 Lux generic Meta reflection method。

`LUX_FUNC(...)` 的唯一语义继续是：

> 将该 C++ free function 标记为 Lux generic Meta reflection function。

二者 **都不是** Script-specific annotation，也不携带 Hook/Event/Lifecycle target。

EntityBehavior：

```cpp
LUX_CLASS()
class EnemyBehavior : public EntityBehavior
{
public:
    LUX_METHOD()
    void think(const SimulationStepInfo& step) noexcept;

    LUX_METHOD()
    void printStage(const StageInfo& stage) noexcept;

private:
    void internalHelper() noexcept;
};
```

Global/free C++ callable 使用已有 generic Meta marker：

```cpp
LUX_FUNC()
void dumpStats(const StageInfo& stage) noexcept;
```

删除/禁止新增：

```text
LUX_SCRIPT_METHOD
LUX_BIND_POINT
LUX_BIND_EVENT
LUX_BEHAVIOR_LIFECYCLE
[[bind_point(...)]]
function-name automatic binding
```

# 36. Meta generator 必须真正 honour method-level opt-in

当前/历史 annotation contract 已经定义 `LUX_METHOD` 为 method reflection marker，因此 generator MUST 只为显式标记 method 生成 `RefMethod`。

```text
LUX_METHOD() present
    -> RefClass.methods contains RefMethod

public helper without LUX_METHOD()
    -> MUST NOT enter RefClass.methods merely because it is public
```

50 public/private helpers + 3 `LUX_METHOD`：

```text
ONLY 3 RefMethod records attributable to method reflection opt-in
```

这条是 generic Meta 自身的 pay-for-use 规则，不是 Script 特例。

# 37. C++ Script export = generic reflection object -> `ScriptExportDescription`

C++ Script/Behavior cooking/importing 发生在 Engine/Simulation bridge。

EntityBehavior asset：

```text
CppStaticScript descriptor selects one reflected RefClass
    ↓
enumerate that RefClass.methods
    ↓
for each RefMethod:
    validate Script-callable subset
    canonicalize semantic args/returns
    hidden receiver = EntityBehavior instance
    mint ScriptSymbolId
    ↓
ScriptExportDescription / ScriptFunction
```

Global C++ module：

```text
CppStaticScript descriptor provides an explicit reflected function set
    ↓
resolve only those RefFunction entries (authored with LUX_FUNC)
    ↓
validate / project each RefFunction
    ↓
ScriptExportDescription / ScriptFunction
```

**MUST NOT：**扫描整个 process `ReflectionRegistry` 并把所有 `RefMethod/RefFunction` 自动变成一个 ScriptAsset 的 exports。

`Reflectable` 是 `Script-bindable` 的超集。以下 RefMethod 可以存在于 Meta，但投影到 Script 时 MUST reject：

```text
non-public
non-noexcept C++ callable
variadic
mutable lref
rvalue ref
unsupported pointer/raw ABI type
unsupported return
unknown semantic record identity
```

hidden `EntityBehavior self` 不进入 Script args。

# 38. C++ runtime preparation：允许 prepared RefMethod invoker，禁止 hot reflection lookup

本轮不要求再造一份 C++ Script-specific direct-thunk truth。

推荐第一版：Engine C++ Behavior bridge cold-prepare 一个稳定 context：

```cpp
struct PreparedRefMethodCall
{
    const RefMethod* method; // process-local, never serialized
    void* object;            // stable EntityBehavior instance
    // preallocated/fixed argument pointer storage as needed
};
```

`BoundScriptCall` 仍保持两指针：

```cpp
{ engine_ref_method_thunk, PreparedRefMethodCall* }
```

hot path MAY 最终调用已经解析好的 `RefMethod::invokable.invoker`，前提是：

```text
no ReflectionRegistry lookup
no method-name/hash lookup
no signature adaptation
no heap allocation
no vector resize
no stale-check manager traversal
```

因此本文删除旧的 blanket ban：`RefMethod generic invoke forbidden in hot path`。
真正禁止的是 **hot reflection discovery / compatibility work**。

若 benchmark 以后证明这一额外 indirect invoke 成为实际瓶颈，MAY 生成 specialized direct thunk；但 specialized thunk 只是 invocation optimization，`RefMethod` 仍是 C++ metadata truth，不产生第二套 signature truth。

# 39. Python：Tier 1，只保留 `@lux.method` authoring marker

```python
class EnemyBehavior(EntityBehavior):

    @lux.method
    def think(self, step: SimulationStepInfo) -> None:
        ...

    def helper(self):
        ... # not exported
```

Global：

```python
@lux.method
def print_stage(stage: StageInfo) -> None:
    ...
```

不再有 `@lux.bind_point/@lux.bind_event`。

本轮 Python **MUST 停在 authoring/import tier**，不实现 CPython runtime backend。

# 40. Python importer：AST-first，禁止 import/execute user module

解析：

```text
ClassDef
FunctionDef
Decorator list
parameter annotations
return annotation
imports/aliases required for static type resolution
```

`@lux.method` callable所有 explicit args必须静态解析为 canonical Script types；return必须明确或 `None`。EntityBehavior member 的 `self` 是 hidden receiver，不进入 ScriptFunction args。

无法静态解析 -> importer error，不 runtime 猜。

# 41. Python canonical types 与产物

不要任意假定 Python `int/float` 对应 i32/f32。第一版优先使用 Lux typed aliases / generated stubs：

```text
lux.i32/u32/i64/u64
lux.f32/f64
lux.bool
engine reflected record types exposed to authoring stubs
```

Importer MUST 产出与 C++/Lua/FlowForge 相同的 `ScriptDescription v4` common exports，并可生成 `PYTHON_SOURCE` ScriptAsset payload。

Product 没有 Python runtime backend 时：

```text
asset import/cook: success
Editor binding authoring: success
runtime prepare: BACKEND_NOT_AVAILABLE
```

# 42. Python 本轮明确不实现

```text
CPython embedding
PyObject* instance runtime
GIL policy
Py_INCREF/Py_DECREF integration
Python BoundScriptCall thunk
Python EntityBehavior runtime allocation
Python hot reload runtime
Python Android PLAYER runtime
per-call getattr/vectorcall path
```

这些属于后续 pay-for-use backend，不是本轮 L1 object/binding contract 的 freeze 条件。

# 43. Lua：只保留 `---@lux.method`

EntityBehavior：

```lua
local EnemyBehavior = {}

---@lux.method
---@param step lux.simulation.SimulationStepInfo
function EnemyBehavior:think(step)
    self.timer = (self.timer or 0) + step.delta_seconds
end

function EnemyBehavior:helper()
end

return EnemyBehavior
```

Global：

```lua
local Debug = {}

---@lux.method
---@param stage lux.debug.StageInfo
function Debug.print_stage(stage)
end

return Debug
```

Lux-specific annotation只有 `---@lux.method`。参数/返回复用 `---@param/@return` 风格。不要再造 bind annotations。

# 44. Lua importer rules

`ENTITY_BEHAVIOR` exported member第一版 SHOULD/MUST 采用 colon syntax，以明确 hidden self。`GLOBAL_MODULE` 无 hidden self。

```text
zero explicit args + no @return -> () -> void MAY infer
any explicit arg -> matching @param canonical type required
no @return -> void
one @return -> one return
>1 business returns -> unsupported v4
```

Lua debug reflection不是 type truth。

# 45. Lua loaded code vs instances

必须重构当前 per-instance chunk load：

```text
ScriptAsset A
  -> load/execute source once per session
  -> LoadedLuaModule / prototype

Entity mount 1 -> instance table 1
Entity mount 2 -> instance table 2
```

instance `__index -> prototype`，每 Entity私有 fields；多个 methods共享一个 instance。

# 46. Lua prepared method

cold：resolve exported function once并 cache registry ref/stable VM handle。hot：

```text
push cached function
if EntityBehavior push cached instance as self
push args
pcall
marshal declared business return -> frame.returns
return ABI status
```

禁止 hot name lookup、annotation parse、chunk load、traceback closure construction。

# 47. FlowForge

保留 `SimulationDescription -> typed target catalog`，但 catalog只用于 Editor palette/signature template。

当前 `compileFlowForgeScript(simulation,...)` 遍历全部 Hook/Event并自动创建 exports/default bindings 的语义必须删除。

Compiler只从**实际 graph-declared exported entries**生成 `ScriptDescription.exports`。Editor可以从 target catalog创建一个同 signature entry node作为模板，但 export本身不携带 target。随后 composition显式绑定。

ENTITY_BEHAVIOR FlowGraph：per-mount state block + hidden self host context；GLOBAL_MODULE FlowGraph无 Entity receiver。同一 graph export MAY bind multiple targets。

---

# Part V — Binding 只属于 Composition

# 48. 移动 binding type

当前 `lux::rdesc::ScriptBindingDescription` 从 Resource domain 删除。新 binding type属于 `engine/simulation/description`，因为它表达 Script symbol 与 Simulation semantic target 的关系。

# 49. Strongly typed target

推荐：

```cpp
struct SystemHookBindingTarget final
{
    SystemTypeId system_type;
    std::string system_instance; // empty => require unique instance by type
    std::string hook;
};

struct SystemEventBindingTarget final
{
    SystemTypeId system_type;
    std::string system_instance;
    std::string event;
};

enum class EBehaviorLifecyclePoint : std::uint8_t
{
    CONSTRUCT,
    START,
    STOP,
};

struct BehaviorLifecycleBindingTarget final
{
    EBehaviorLifecyclePoint point;
};

using ScriptBindingTarget = std::variant<
    SystemHookBindingTarget,
    SystemEventBindingTarget,
    BehaviorLifecycleBindingTarget>;

struct ScriptBindingDescription final
{
    ScriptSymbolId function;
    ScriptBindingTarget target;
};
```

若 current codec 不适合 `std::variant`，可用 tagged POD，但 validation必须等价；不要重新允许“kind=HOOK 但 member 指 Event”这种非法状态。

# 50. System target resolve

`system_instance` 非空：instance MUST exist且 type一致。为空：

```text
0 -> TARGET_SYSTEM_NOT_FOUND
1 -> success
>1 -> TARGET_SYSTEM_AMBIGUOUS
```

绝不取第一个。

# 51. ScriptMountId

新增：

```cpp
struct ScriptMountId final
{
    std::uint64_t value{};
    bool valid() const noexcept { return value != 0U; }
};
```

稳定、serialized、owner scope 内 unique。Editor/tooling生成；vector position不是 identity。

# 52. ScriptMountDescription explicit-only

删除 `EScriptBindingSetMode`：

```cpp
struct ScriptMountDescription final
{
    ScriptMountId id;
    AssetId script;
    std::vector<ScriptBindingDescription> bindings;
};
```

所有 binding都是完整 explicit composition。没有 asset defaults / merge / patch language。

# 53. Entity authored component

将 `ScriptMountFacts` 收敛/重命名为：

```cpp
struct ScriptComponent final
{
    std::vector<ScriptMountDescription> mounts;
};
```

若命名冲突可用 `EntityScriptComponent`，但不得保留两个同义 public components。

只序列化 authored facts；绝不存 backend ptr/Lua ref/PyObject/BoundScriptCall/runtime sidecar。

# 54. Global mounts

`SimulationDescription.global_script_mounts[]` 继续使用同一 `ScriptMountDescription`。

runtime cold validation：

```text
Global owner requires Script.model == GLOBAL_MODULE
Entity ScriptComponent requires Script.model == ENTITY_BEHAVIOR
```

# 55. Editor 唯一 binding workflow

1. load ScriptAsset Description；
2. list exports；
3. select callable；
4. enumerate current Simulation + lifecycle target catalog；
5. filter exact-compatible targets；
6. user selects zero or many；
7. write `ScriptMountDescription.bindings[]`。

函数名不参与匹配。Runtime仍重新 cold validate，不能仅相信 Editor。

# 56. 同 method 多 target

必须支持：

```text
DebugBehavior.print_stage(StageInfo)
 -> Physics.BeforeStage
 -> Physics.AfterStage
 -> Render.BeforeStage
```

backend `prepareMethod` 一次，三处 DispatchIndex重用同一 16-byte `BoundScriptCall`。

---

# Part VI — Behavior lifecycle

# 57. 仅三个 lifecycle points

第一版：

```text
CONSTRUCT
START
STOP
```

不复制 UE 全生命周期；不新增 enable/disable，除非以后 `ScriptComponent.enabled` 成为真实 contract。

# 58. lifecycle signatures

```text
CONSTRUCT -> void()
START     -> void()
STOP      -> void(BehaviorStopReason)
```

```cpp
enum class EBehaviorStopReason : std::uint8_t
{
    MOUNT_REMOVED,
    ENTITY_DESTROYED,
    SIMULATION_STOPPED,
};
```

canonical semantic name：`lux.simulation.BehaviorStopReason`。

Lifecycle 是 target，不是 magic method name：任意 `LUX_METHOD() void initialize() noexcept` 都可由 Editor绑定 CONSTRUCT。

# 59. constructor vs CONSTRUCT

```text
C++ constructor / Python __init__ / Lua instance table creation / FlowForge state allocation
    = backend local object construction
```

此阶段不得假设 Entity self/components有效。

随后：

```text
attach stable host/self context
apply authored properties
run CONSTRUCT target handlers
```

CONSTRUCT 是第一个 gameplay-level self/component access point。

C++ EntityBehavior 未 attach 时调用 self/getComponent：Debug/Hardened contract fail；Release不得返回伪造 Entity。

# 60. START 是 per-instance activation，不是 Scene-only

定义：每个 Behavior instance 第一次进入 ACTIVE 前恰好调用一次。

动态中途创建也必须：

```text
create -> attach -> CONSTRUCT -> prepare/publish shadow -> START -> ACTIVE
```

# 61. Initial batch ordering

```text
1 Entity + authored Components exist
2 discover all EntityBehavior mounts
3 create all backend instances
4 attach stable self/host contexts
5 apply authored state
6 prepare all unique methods referenced by bindings
7 run all CONSTRUCT handlers
8 build shadow DispatchIndex
9 run all START handlers
10 publish DispatchIndex
11 normal Hook/Event dispatch begins
```

Deterministic order：Entity exact bits ascending；within Entity mount declaration order；within mount binding declaration order。

# 62. Dynamic add

At designated quiescent point：

```text
resolve new mount
create instance
attach host/self
prepare unique methods
CONSTRUCT
build updated shadow index
START
publish
mark ACTIVE
```

Normal callbacks不得在 START 前看到新 instance。

# 63. STOP / retirement

收到 remove/destroy：

```text
mark RETIRING immediately
normal invoke skips RETIRING
quiescent point:
  build index excluding instance
  STOP(reason)
  release prepared methods
  destroy backend instance
  commit/publish updated index
```

STOP exactly once。Backend destructor在 STOP 后。

# 64. STOP 时 component availability

不虚构 EnTT teardown order：

```text
MOUNT_REMOVED: Entity仍 valid，可按 host contract访问 components
SIMULATION_STOPPED: Session stop应在 Registry teardown前执行
ENTITY_DESTROYED: universally only local state + Entity identity guaranteed
```

若未来 staged EcsCommandBuffer pre-destroy path可证明 components仍在，另行强化 contract。不要假设 raw `registry.destroy()` 的 component destruction order。

---
# Part VII — EntityBehavior host context / ECS access

# 65. stable per-instance host context

当前实现每次 invoke 构造 stack-local `ScriptHostContext`。对象模型改为 per-instance stable context：

```cpp
struct ScriptInstanceHostContext final
{
    const ScriptHostApi* api{};
    ecs::Entity self{ecs::NullEntity};
};
```

它由 MountRuntime/Session拥有，生命周期覆盖 backend instance。

每次 call：

```text
frame.world_context = stable ScriptInstanceHostContext*
frame.user_context  = prepared backend callable context
```

EntityBehavior C++ base可在 attach 时保存 host context pointer。Global instance self = NullEntity。

# 66. hidden self

EntityBehavior script method：

```text
source receiver self exists
ScriptFunction.args excludes self
```

例如：

```python
def tick(self, step: SimulationStepInfo) -> None
```

semantic signature是：

```text
(SimulationStepInfo const/ref semantics) -> void
```

不是 `(Entity, Step) -> void`。

# 67. Component access facade

继续禁止 public Script Hook/Event signature直接传 `Registry&`。

C++ base SHOULD：

```cpp
template<class C> const C& getComponent() const;
template<class C> bool hasComponent() const;
template<class C, class Fn> void patchComponent(Fn&&);
```

structural mutation：

```text
EcsCommandBuffer / host command API
```

不是直接 `registry.emplace/remove/destroy`。

返回 component reference只在当前同步 callback / ECS validity contract范围内借用；Behavior不得长期缓存裸 component address。

# 68. ECS concurrency safety

本轮只有 `LUX_METHOD()`，不增加 method-level Read/Write annotations，因此 runtime不知道某个 Behavior 会访问哪些 components。

第一版硬规则：

> EntityBehavior callback只能在 concrete System 明确证明的 ECS-safe / quiescent script Hook window 执行。

MUST NOT 在未知并发 ECS writer 存在时允许 Behavior read/patch。

不要为了“自动并行”偷偷新增第二套 annotation：

```text
LUX_READ_COMPONENT
LUX_WRITE_COMPONENT
@reads/@writes
```

未来如果 profile证明必要，再单独设计 declared script access metadata。

若当前 L1 尚无完整 runner可证明某 concrete Hook safe：该 Hook的 component-access-capable Behavior integration不得作为完成项宣称。

---

# Part VIII — Backend API：instance-first

# 69. 退休 per-binding prepare

当前 `prepare(context, mount/binding ordinal, asset, function, outCall)` 改成 instance-first语义。

建议：

```cpp
struct ScriptBackendInstance final
{
    void* value{};
};

struct ScriptInstanceCreateContext final
{
    AssetId script;
    ScriptMountId mount;
    ecs::Entity self{ecs::NullEntity};
    ScriptInstanceHostContext* host{};
};

enum class EScriptBackendResult : std::uint8_t
{
    SUCCESS,
    UNSUPPORTED_MODEL,
    UNSUPPORTED_SIGNATURE,
    UNSUPPORTED_MARSHAL_TYPE,
    CAPACITY_EXCEEDED,
    ALLOCATION_FAILURE,
    CONSTRUCTION_FAILURE,
};

struct ScriptBackendDescriptor final
{
    rdesc::Script::Kind kind;
    void* context;

    EScriptBackendResult (*createInstance)(
        void*, const ScriptInstanceCreateContext&,
        const ScriptAssetContent&, ScriptBackendInstance&) noexcept;

    EScriptBackendResult (*prepareMethod)(
        void*, ScriptBackendInstance,
        const ScriptFunction&, BoundScriptCall&) noexcept;

    void (*releaseMethod)(
        void*, ScriptBackendInstance, BoundScriptCall) noexcept;

    void (*destroyInstance)(
        void*, ScriptBackendInstance) noexcept;
};
```

命名可调整；职责不可偷换回 per-binding。

# 70. unique method preparation

Mount bindings：

```text
A -> target1
A -> target2
A -> target3
B -> target4
```

必须：

```text
createInstance = 1
prepareMethod(A) = 1
prepareMethod(B) = 1
```

DispatchIndex复制 prepared `BoundScriptCall`，不重复 backend prepare。

# 71. backend memory ownership

backend owns：

```text
C++ object
Lua refs/object
Native/FlowForge state
Python object
```

Session只保存 opaque handle + prepared calls。统一 handle，不统一 object layout。

# 72. backend lifetime

第一版 contract：

```text
backend objects MUST outlive ScriptBindingSession
Session MUST be destroyed before backend objects
```

`create()` 检查 backend kinds unique。若使用已有 code-lifetime lease，可强化，但不得建立 process-global backend registry。

---

# Part IX — C++ EntityBehavior backend

# 73. EntityBehavior C++ base

应放 opt-in C++/native script integration，不让 ECS core依赖：

```cpp
class EntityBehavior
{
protected:
    ecs::Entity self() const noexcept;
    template<class C> const C& getComponent() const;
    template<class C> bool hasComponent() const;
    template<class C, class Fn> void patchComponent(Fn&&);

private:
    ScriptInstanceHostContext* host_{};
    friend struct GeneratedBehaviorAccess;
};
```

constructor时 `host_ == nullptr`，backend attach后 valid。

# 74. generated C++ descriptor：explicit set，不建 global registry

概念：

```cpp
struct CppScriptMethodDescriptor final
{
    ScriptFunction function;
    lux_script_invoke_fn invoke;
};

struct CppStaticScriptDescriptor final
{
    std::string_view canonical_name;
    EScriptModel model;

    std::size_t object_size;
    std::size_t object_align;

    void (*construct)(void*) noexcept;
    void (*attachHost)(void*, ScriptInstanceHostContext*) noexcept;
    void (*destroy)(void*) noexcept;

    std::span<const CppScriptMethodDescriptor> methods;
    // optional existing code-lifetime lease if required by plugin/module system
};
```

Composition显式把 descriptor set交给 C++ backend。禁止 process-global `CppBehaviorRegistry`。

# 75. C++ object allocation

第一版允许 one stable aligned allocation per Behavior instance（cold path）。

```text
construct once
attach once
CONSTRUCT once
START once
invoke many
STOP once
destroy once
free once
```

禁止 per-callback allocation。以后 profile证明创建成本值得优化时 MAY slab/pool。

# 76. C++ method preparation：instance-first + method projection cache

一个 C++ Behavior instance创建一次。每个 unique exported `ScriptSymbolId` 在该 instance 上最多 prepare一次；同 method绑定多个 targets只复制 `BoundScriptCall` two-pointer record。

第一版允许：

```text
RefMethod resolved once
PreparedRefMethodCall allocated/prepared once
argument pointer storage fixed once
```

禁止 per-binding 重复构造相同 method adapter，禁止 per-callback allocation。

# 77. `modules/core/meta_script` cleanup

删除 `CORE/FOUNDATION -> function/script_core` 这条反向依赖。

将需要保留的逻辑拆入 Engine/Simulation C++ Script bridge：

```text
RefType/RefMethod -> Script semantic projection
C++ Behavior RefClass resolve/construct/destruct
Prepared RefMethod invocation adapter
```

不要把它迁移成 `function/script -> meta`；Meta 与 Script Foundation仍保持互不依赖。

若旧 `ReflectedScriptCall` 没有独立 tooling consumer则删除；若有，必须位于正确的 Engine/tooling bridge，并不得成为 serialized truth 或执行 lookup 的 hot manager。

---

# Part X — Lua backend object model

# 78. LoadedLuaModule cache

State至少区分：

```text
LoadedLuaModule keyed AssetId/session snapshot
LuaScriptInstance keyed stable mount identity
PreparedLuaMethod keyed instance + ScriptSymbolId
```

首次 Asset使用时 load/execute chunk一次，capture returned prototype/module table并 cache exported method refs。后续 Entity只创建 instance table，不重新 load source。

# 79. Lua EntityBehavior instance

推荐：

```text
instance = new table
metatable.__index = prototype
private stable host context attached through userdata/upvalue/internal field
```

`self.timer/self.target` 等 per-Entity state在 instance table。多个 exported methods共享该 instance。

# 80. Lua Global instance

同一 loaded module可被多个 global mounts使用，但每 mount若需要 mutable state必须有独立 mount instance，不能无意共享游戏状态。

# 81. Lua marshal

本轮最低支持或 cold reject：

```text
lux.bool
lux.i32/u32/i64/u64
lux.f32/f64
canonical reflected STRUCT_REF input
```

Event payload通常为 `const Payload&`；Lua可接 read-only reflected userdata/view 或当前已有等价安全表示。若无法实现，prepare必须 fail，不能 invoke 才 fail。

# 82. Lua return/status再次固定

```text
lua_pcall success -> ABI status 0
business return -> frame.returns
Lua exception -> nonzero ABI status
```

必须有真实 one-return test。

---

# Part XI — Native / FlowForge backend

# 83. NativeModule instance-first

保留 current module/state sharing思想。createInstance：

```text
retain loaded module
allocate state_size with state_align
copy defaults / initialize state
```

prepareMethod：

```text
find symbol once
validate full ABI descriptor
return BoundScriptCall
```

若 `function.invoke` 可直接使用 `frame.user_context = state`，优先：

```cpp
BoundScriptCall{function.invoke, state_ptr}
```

不要 per-method `Call` allocation。

# 84. C ABI version

本文 **不要求** 仅为对象模型 bump `LUX_SCRIPT_ABI_VERSION`。对象 lifetime通过 backend instance API实现；native function ABI仍是 `call_frame -> int status`。

只有实际修改 C struct layout才 bump ABI。不要为了版本“看起来新”无意义 bump。

# 85. FlowForge state alignment

恢复：

```text
size
align
hash
defaults
```

每 Entity behavior独立 state block；compiled code共享。

---

# Part XII — Event ownership / safe dispatch

# 86. 退休 future-buffer ScriptEventWriter API

删除/重构：

```text
ScriptBindingSession::writer()
ScriptEventWriter::emit(frame)
Session pending Occurrence storing frame pointers
beginUpdate() only for occurrence clear
```

Session不再拥有 future event payload memory。

# 87. System-owned typed buffer

System或 concrete module拥有：

```cpp
template<class Payload>
class SystemEventBuffer
{
    // producer-local typed values, capacity prepared
};
```

它是 System-private/reusable helper，不是：

```text
global EventBus
process event registry
gameplay message manager
```

producer append actual Payload + exact target Entity（targeted event）。

# 88. worker rules

prepare：producer count/capacity固定/预留。worker只 append own producer-local storage；normal append不锁全局 mutex、不调用 user callback、不在 capacity内分配。

safe Hook按：

```text
producer ordinal ascending
then producer-local sequence
```

merge/drain。

# 89. synchronous Session event dispatch

Session只提供同步 API，例如：

```cpp
dispatchEvent(
    ScriptEventSlot event,
    ecs::Entity target,
    const lux_script_call_frame& live_frame
) noexcept;
```

**MUST NOT retain任何 live_frame pointer after return。**

System在 typed payload存活期间 build frame once并 dispatch所有 actual handlers；返回后 payload可销毁/复用。

# 90. same-Hook ordering

固定：

```text
1 drain Event occurrences assigned to Hook H
2 invoke H's Hook handlers
```

职责：

```text
Event occurrence memory/order -> System
Binding/handler lookup        -> ScriptBindingSession
```

---

# Part XIII — ScriptBindingSession v2 private model

# 91. MountRuntime

概念：

```cpp
struct MountRuntime
{
    ScriptMountId id;
    AssetId asset;
    EScriptModel model;
    ecs::Entity self;

    ScriptInstanceHostContext host;
    ScriptBackendInstance instance;
    std::vector<PreparedMethod> methods;

    enum State { CONSTRUCTING, ACTIVE, RETIRING, DEAD };
};
```

实际 container只需保证 backend instance/prepared contexts地址在有效期内稳定。

# 92. PreparedMethod

```cpp
struct PreparedMethod
{
    ScriptSymbolId symbol;
    BoundScriptCall call;
    std::size_t backend_index;
};
```

每 mount+symbol最多一份。

# 93. DispatchIndex

保持 data-oriented：

```text
Hook slot -> contiguous prepared ordinals
Global Event -> contiguous prepared ordinals
Entity target -> exact EntityDispatchSlot -> event-major ranges -> flat ordinals
```

# 94. exact Entity slot

```cpp
struct EntityDispatchSlot
{
    ecs::Entity owner{ecs::NullEntity};
    std::uint32_t sidecar{Invalid};
};
```

targeted hot path必须 exact compare。generation mismatch快速 no-op。

# 95. dirty Entity diff，不用 bool full rebuild instance

监听 ScriptComponent construct/update/destroy，至少记录 dirty exact Entity。

quiescent：

```text
for each dirty Entity:
    diff authored mounts by ScriptMountId
    unchanged -> preserve instance
    added -> create
    removed -> retire
    same id but asset/model changed -> replace instance
    binding-only change -> preserve instance
```

然后 MAY 从所有 ACTIVE runtimes重建 dense DispatchIndex。

# 96. binding-only edit preserves state

same MountId + same AssetId，只 bindings变化：

```text
same backend object
same private state
constructor/CONSTRUCT/START NOT rerun
```

只 prepare newly referenced symbols、release no-longer-used symbols、rebuild index。

# 97. mount reorder

`[A(id10),B(id20)] -> [B(id20),A(id10)]`：instance identity/state保持；authored dispatch order MAY 随声明顺序变化。

# 98. failure policy

一个 method invoke失败：本轮固定 retire whole mount instance，而不是只 disable一个 method。避免 half-dead object。

```text
record compact failure
mark RETIRING
continue current safe iteration according to local policy
quiescent remove
STOP if applicable
destroy
```

不得 while iterating erase hot list。

---

# Part XIV — Binding validation / target compatibility

# 99. exact signature only

继续禁止 numeric widening、prefix args、extra return discard、optional coercion等。

```text
Hook      -> exact Hook signature
Event     -> exact canonical Event Script signature
Lifecycle -> exact lifecycle signature
```

# 100. Event canonical signature helper

non-void payload：

```text
parameter.canonical_name = payload schema name
parameter.type_id        = payload schema hash
parameter.pass           = CONST_REF
returns                  = empty
```

void payload：`() -> void`。

Editor/FlowForge/runtime matcher必须复用。

# 101. Global scope

GLOBAL_MODULE：

```text
MULTI Hook   allowed
SINGLE Hook  allowed
GLOBAL Event allowed
Lifecycle    forbidden
ENTITY Event forbidden
```

# 102. Entity scope

ENTITY_BEHAVIOR：

```text
MULTI Hook          allowed
ENTITY_TARGETED     allowed
Lifecycle           allowed
SINGLE Hook         forbidden v4
GLOBAL Event        forbidden
```

不设计 Entity SINGLE Hook reducer。

# 103. lifecycle cardinality

同一 lifecycle point MAY 绑定多个 compatible void methods，执行顺序 = binding declaration order。不要要求只有一个 magic `on_start`。

---

# Part XV — Semantic type single source of truth

# 104. primitive names冻结

```text
lux.bool
lux.i32
lux.u32
lux.i64
lux.u64
lux.f32
lux.f64
```

若加入 i8/u8/i16/u16/string_view，也必须在一个 canonical table定义。Traits、Meta bridge、fixtures、importers、FlowForge全引用同一处。

# 105. Meta / Script semantic bridge

冻结原则：

```text
core/meta has no Script dependency
function/script has no Meta dependency
Engine/Simulation bridge owns the conversion
```

Meta type hash/name **不自动等于** Script semantic identity。Engine bridge将 `RefType/QualType` 投影到唯一 Script semantic canonical table，并负责 collision/name validation。

C++ member projection source必须是已经由 `LUX_METHOD` 进入 `RefClass.methods` 的 `RefMethod`；C++ free/global projection source必须是由 `LUX_FUNC` 进入 Meta 的 `RefFunction`。不得重新用第二套 Script AST parser作为 C++ callable truth。

Generic Meta compatibility（例如 numeric initialization conversion）与 Script binding compatibility是两回事：

```text
Meta may support broader C++ runtime conversions.
Script binding v1 remains EXACT semantic signature match.
```

禁止直接用 `meta::canInitialize/canAssign` 放宽 Script target matching。

# 106. integrated regression

真实：

```cpp
LUX_METHOD() void f(float) noexcept;
makeSystemHookPoint<void(float)>("...");
```

经真实 generator/manifest后 exact match。再覆盖 i32、const reflected record&。

---

# Part XVI — Wire/schema revisions

# 107. ScriptDescription v4

`v3 -> v4`，原因：remove default_bindings、add ScriptModel、backend body revisions、state_align、C++ static semantics。runtime reject v3；若要迁移用 offline tool，不加 runtime shim。

# 108. ScriptAsset wire v2

当前 wire v1 serialized default_bindings，因此 bump：

```text
ScriptAsset wire v1 -> v2
```

v2只含 ScriptDescription v4 + payload；NO bindings。reject v1 runtime。

# 109. LXSD v4

当前 LXSD v3 global mount含 binding mode/list。新 mount含 stable mount id、explicit binding、strong target。因此 bump：

```text
LXSD v3 -> v4
```

完整编码 global mount id、AssetId、binding order、target variant、SystemType identity/optional instance/member、lifecycle enum。reject v1/v2/v3。

# 110. World/Snapshot schema

若 ScriptComponent进入 snapshot/world serialization：component schema version bump，并编码 stable mount id + explicit bindings；旧 ordinal绝不作为 identity。

---
# Part XVII — Editor / target catalog

# 111. Bindable target catalog

Editor统一展示：

```text
System Hook
System Event
Behavior Lifecycle
```

每个 target提供：

```text
display label
durable target identity
scope requirement
semantic signature
cardinality where relevant
```

这是 authoring catalog，不是 runtime global registry。

# 112. filtering algorithm

对于 selected export F：

```text
1 resolve mount owner/model
2 reject incompatible scope
3 derive target signature
4 exact compare parameters/returns/pass
5 apply cardinality rule
6 show compatible target
```

名字不参与。

# 113. 本轮 Editor 交付边界：authoring API，不是 GUI

仓库当前没有 canonical Editor target，本轮 **MUST NOT** 为满足规范临时创建 ImGui panel、Editor executable或新的应用框架。

Definition of Done 是 UI-agnostic authoring/binding layer：

```text
enumerate exported callables
enumerate bindable targets
return exact compatibleTargets(export, mount scope)
construct/remove/reorder explicit ScriptBindingDescription
report missing symbol / missing target / signature mismatch
support one export -> 0..N compatible targets
```

推荐 public/tooling surface可以表现为 `ScriptBindingCatalog` / `ScriptBindingAuthoring` 等等，名称按仓库约定；compatibility算法必须与 FlowForge/tooling共享，runtime再 defensive cold validate。

未来 Editor UI 只需把该 API呈现为：

```text
Exports:
  print_stage(StageInfo const&) -> void

Bindings:
  [export] -> [compatible target dropdown / multi-select]
```

但 GUI 本身 **不属于本轮完成条件**。

# 114. dangling diagnostics

System removed、Hook/Event renamed、signature changed、Script symbol removed/renamed时：Editor显示 missing target / signature mismatch / missing method。不得 silently fallback或按 name重新 bind。

---

# Part XVIII — FlowForge refactor

# 115. Target catalog保留但 Event pass修正

`TypedEntryCatalog`/renamed target catalog的 Event parameter必须 `CONST_REF`，不能 VALUE。

# 116. compiler不再从整个 Simulation自动生成 exports

当前 `compileFlowForgeScript(simulation,...)` 遍历所有 targets并产生 export + default binding 的行为必须删除。

新 compiler从 actual graph-declared exported entries生成 ScriptDescription。Editor MAY 从 target catalog复制 signature创建 entry node，但 export创建后独立于 target。

# 117. FlowForge model

Graph asset显式 model：GLOBAL_MODULE 或 ENTITY_BEHAVIOR。EntityBehavior graph methods共享 per-instance state block + hidden self host context；global graph没有 Entity receiver。同一 export可绑定多个 compatible targets。

---

# Part XIX — P0 correctness tests

# 118. Event payload lifetime regression

必须写一个会使旧实现失效的 test：

```cpp
emitFromScope()
{
    Payload p{123};
    typedBuffer.emit(p);
} // p destroyed

// later safe Hook
drainAndDispatch();
assert(callback saw 123);
```

不能把 payload/slot放在覆盖整个 dispatch 的外层作用域。

# 119. exact Entity generation regression

```text
create E0 generation G
mount behavior / build dispatch
request destroy E0
create/reuse slot -> E1 generation G+1
emit targeted event around quiescent boundary
```

验证旧 Behavior绝不被 E1触发；retiring instance不再接 normal callbacks。

# 120. Entity -> MULTI Hook

EntityBehavior method `void(const SimulationStepInfo&)` bind Physics.Before；prepare成功、self exact、callback exact。

# 121. Entity -> SINGLE Hook

即使 signature exact，也 cold reject，error明确。

# 122. Event pass mode

同 payload创建 VALUE export和 CONST_REF export；只有 CONST_REF可 bind Event。

# 123. semantic primitive integration

真实 Meta generator -> RefMethod -> Engine projection -> SystemHook跨层测试 `float/i32/const reflected record&`；不能只分别 unit-test helper。

# 124. prepareMethod dedup

一个 method绑定三 Hook：

```text
createInstance == 1
prepareMethod == 1
installed target references == 3
```

# 125. backend duplicate kind

Session create传两个相同 Kind backends必须 cold fail。

# 126. enum corrupt decode

ScriptAsset/LXSD raw invalid enum values必须 decode reject。

---

# Part XX — EntityBehavior lifecycle tests

# 127. Dynamic creation

Simulation已运行，加入 Entity + ScriptComponent，quiescent apply日志：

```text
backend constructor
attach self
CONSTRUCT
START
normal Hook
```

START exactly once。

# 128. Initial batch

多个 Entity必须：所有 create/attach -> all CONSTRUCT -> all START -> first normal Hook，且 deterministic order。

# 129. shared object state

method A写 private field，method B从另一个 Hook读取；证明同 mount多个 methods共享一个 instance。C++、Lua至少各一条。

# 130. no ScriptComponent

大量 plain Entities：backend createInstance、lifecycle、prepared methods全为 0。

# 131. binding-only edit preserves state

Behavior private counter=42；same mount id+asset只改 bindings。apply后 same instance identity，counter仍42，constructor/CONSTRUCT/START不重跑。

# 132. mount reorder preserves state

A/B mounts换顺序：instance A/B identity/state不变；dispatch declaration order可随 authoring order变化。

# 133. removal

mount remove：RETIRING后立即无 normal callback；STOP(MOUNT_REMOVED) exactly once；destroy once。

# 134. entity destroy

验证 generation safety、无 dangling call、STOP reason、instance release once。不要依赖未承诺的 component destroy顺序。

# 135. simulation stop

normal dispatch停止；Behavior STOP(SIMULATION_STOPPED)；instances先销毁，backend/module/code lifetime后释放。

---

# Part XXI — Lua tests

# 136. code load once

同一 Lua asset挂 100 Entity：chunk load/execute count=1，instance count=100。

# 137. hidden self / independent instances

两个 Entity的 `self.counter` 独立；同一 Entity methods A/B共享。

# 138. business return slot

Lua one-return method：ABI status=0，`frame.returns[0]`得到业务值。

# 139. Lua error

throw -> ABI status nonzero；业务 return不被当 status。

# 140. STRUCT_REF

若实现支持：真实 reflected payload读取字段成功；若不支持：prepare阶段明确 reject。首次 invoke 才 fail = test failure。

---

# Part XXII — worker/Event tests

# 141. real worker safe-point

使用真实 `TaskExecutor worker_count > 0`：

```text
worker writes typed payload
callback_count == 0 before safe Hook
producer stack ends
safe Hook drains
callback thread is designated safe thread
producer/local ordering deterministic
Event callbacks occur before Hook callback
```

# 142. occurrence capacity

producer-local capacity exceeded返回明确 error/status，不 heap grow，不 silently drop unless System contract explicitly chooses drop policy。

---

# Part XXIII — ECS System + Behavior coexistence tests

# 143. canonical state

EnemySystem按 declared phase更新组件；EnemyBehavior在 safe Hook读/patch同一 ECS component，结果 deterministic。Behavior fields只存 transient state，不复制 authority。

# 144. no unsafe concurrency

至少一个 integration test/trace证明 component-access-capable Behavior callback不与未知 ECS writer overlap。如果当前 L1 runner尚不能证明，则明确标记 concrete integration未启用；不得以单线程 unit fixture替代并发安全证据。

---

# Part XXIV — Installed consumer

# 145. fresh-installed consumer必须成功执行

旧 consumer“session create失败则 return 0”废弃。

新 consumer至少：

1. 只从 fresh installed packages include/link；
2. 定义 System：MULTI Hook + Event；
3. 构造 SimulationDescription；
4. 定义 C++ EntityBehavior；
5. 两个 `LUX_METHOD()`；
6. 通过 installed generic Meta 生成 `RefClass/RefMethod`，再经 public Engine/Simulation C++ bridge投影为 ScriptDescription v4；
7. ScriptAsset v2 encode/decode；
8. Entity + ScriptComponent + stable MountId；
9. method A -> MULTI Hook；
10. method A -> 第二 compatible Hook；
11. method B -> START 或 Event；
12. real C++ backend create；
13. Session prepare成功；
14. lifecycle/Hook实际 invoke；
15. self exact；
16. executable exits 0；
17. second build no-work。

若 external installed codegen API暂时无法提供，可使用 public generated-descriptor builder fixture，但必须 real success bind/invoke。

---

# Part XXV — Benchmark v7

# 146. schema同步升级

runtime architecture和 measurement变更，因此：

```text
benchmark schema v6 -> v7
```

producer、policy、evaluator、evidence writer同一提交同步修改。

# 147. 保留旧 L1 groups

继续 TaskGraph scaling、World description、EcsCommandBuffer、reactive dirty、Snapshot（若 public）。新 Script benchmark不能替代这些。

# 148. 新 Script groups

至少：

```text
cpp-method-prepared
hook-global-multi
hook-entity-multi
global-event
entity-targeted-event-sparse
owned-worker-event-buffer
```

# 149. `cpp-method-prepared`

真实 `LUX_METHOD -> RefMethod -> Engine projection -> PreparedRefMethodCall -> BoundScriptCall` 路径。要求：

```text
0 hot allocation
0 ReflectionRegistry/name lookup
0 hot signature adaptation
callback exact
```

同时记录是否经过一个额外 prepared MethodInvoker indirect call；这是可接受的第一版成本，不得为了 benchmark数字重新引入第二套 C++ signature/codegen truth。

# 150. multi-target structural metric

bindings=4、unique method=1：createInstance=1、prepareMethod=1。可以作为 structural gate，不必只看 timing。

# 151. sparse targeted benchmark

例如：

```text
1,000,000 total Entity
10,000 scripted Entity
100,000 targeted occurrences
```

target来自 scripted subset。再用更大 total scene但相同 target workload比较；时间不应按 total entity数量线性增长。

# 152. real instrumentation

删除 literal-zero public getters。private benchmark/test instrumentation真实记录：

```text
asset resolves
target/name resolves
reflection accesses
entities examined
instance creates
method prepares
frame builds
allocations
```

结合 source scan证明 hot path bans。

# 153. Lua structural performance

Lua绝对 interpreter ns不作为 L1 C++ scaling硬门槛，但必须证明：chunk load once、method lookup cold-only、无 Lux-side per-call allocation after prepare。

---

# Part XXVI — Architecture scans / negative probes

# 154. Retired surface negative probes

至少：

```text
ScriptDescription.default_bindings absent
EScriptBindingSetMode absent
LUX_BIND_POINT absent
LUX_BIND_EVENT absent
LUX_BEHAVIOR_LIFECYCLE absent
runtime automatic name binding absent
borrowed-frame ScriptEventWriter API absent
ScriptMountFacts absent if renamed to ScriptComponent
```

# 155. Hot-path bans

Production invoke path禁止：

```text
Asset/UUID lookup
string target lookup
unordered_map symbol lookup
ReflectionRegistry lookup / RefMethod discovery / signature adaptation
Lua getfield by method name
Python getattr by method name
module load
heap allocation
scene view scan
shared_ptr refcount churn per callback
signature reconstruction
```

# 156. No global architecture

禁止重新出现：

```text
universal ScriptSystem
process-global ScriptEventRegistry
global ScriptRuntime singleton
global CppBehavior registry
generic EventBus for System->Script
SystemHook manager/service locator
```

# 157. FlowForge scan

Toolchain compiler不得再“for every Simulation target -> auto export + binding”。Target catalog可枚举，但 compiler只处理 graph-declared exports。

---

# Part XXVII — 实施顺序（必须按依赖执行）

# 158. Phase 0 — 锁定当前 evidence并记录 independent review failed

在新工作 branch/worktree记录：

```text
source branch: codex/l1-system-hook-script-binding
evidence HEAD: 2bf052...
reviewed production: 393180...
```

旧 evidence不删除，但新增 review note明确：build/perf qualification通过 ≠ semantic acceptance；当前 freeze candidate rejected。

# 159. Phase 1 — 先建立 P0 regression tests

在大重构前先写会打爆旧实现的 tests：borrowed event lifetime、entity generation reuse、Event pass、primitive canonical mismatch、Entity MULTI Hook、Lua return/status、Lua unsupported marshal cold failure。

这保证后续不是“新代码看起来更漂亮但旧 bug 无证据消失”。

# 160. Phase 2 — ScriptDescription v4

实施：

```text
EScriptModel
remove default_bindings
remove binding types from resource Script.hpp
allow duplicate diagnostic names
Native state_align
schema v4
```

不要同时做 Editor UI。

# 161. Phase 3 — Composition binding + stable mount ids

实施 strong target、ScriptMountId、explicit-only ScriptMountDescription、ScriptComponent、global mount update、snapshot schema update。

# 162. Phase 4 — ScriptAsset wire v2 / LXSD v4

等 v4 data fields稳定再锁 wire。完成 roundtrip、canonical reencode、corruption、reject old versions。

# 163. Phase 5 — L0 semantic canonical cleanup

统一 primitive canonical table、`scriptSymbolId()`、Event signature helper、enum validation。保持 `core/meta` 与 `function/script` 无直接依赖。

# 164. Phase 6 — Generic Meta method opt-in 修正

1. 保持现有 `LUX_METHOD` public spelling与 generic Meta语义；
2. 修正 generator：只有显式 `LUX_METHOD` method进入 `RefClass.methods`；
3. public unmarked helper不得自动反射；
4. 覆盖 overload/noexcept/qualifier/record tests；
5. 不生成 Script binding/target metadata。

# 165. Phase 7 — Engine/Simulation C++ Script bridge

实施：

```text
RefClass selection for ENTITY_BEHAVIOR CppStaticScript
explicit RefFunction selection for GLOBAL_MODULE CppStaticScript
RefMethod/RefFunction -> ScriptExportDescription projection
Script semantic exact type conversion
ScriptSymbolId generation
C++ Behavior RefClass construct/destruct integration
PreparedRefMethodCall
```

删除 `modules/core/meta_script` 反向依赖。不得把 adapter迁到 Script Foundation。

# 166. Phase 8 — C++ EntityBehavior + instance-first runtime

实现 base/context、stable host attach、aligned stable object allocation、one instance per mount、one prepared method per unique symbol、多 target复用同 prepared call。

# 167. Phase 9 — ScriptBindingSession incremental composition

实现 MountRuntime、stable mount diff、exact Entity generation、lifecycle、Entity MULTI Hook、dense shadow DispatchIndex、failure retirement。one binding edit不得重建 unrelated objects。

# 168. Phase 10 — Event ownership refactor

删除 Session borrowed occurrence buffering；实现 typed System event buffer、worker producer-local storage、synchronous `dispatchEvent`、safe Hook order和真实 worker tests。

# 169. Phase 11 — Native backend cleanup

instance-first、state_align、complete ABI type validation、尽量直接 `{function.invoke,state}`，避免 per-method heap wrapper。

# 170. Phase 12 — Lua importer + object runtime

先 importer (`---@lux.method` + params/return -> v4)，再 runtime（load once、prototype、instance table、hidden self、method cache、return slots、STRUCT_REF support/cold reject）。

# 171. Phase 13 — FlowForge + binding authoring catalog

保留 typed target enumeration但 compiler停止 auto-export all targets；graph-declared exports only；no default bindings。实现 UI-agnostic binding catalog/compatibility API，FlowForge复用该 truth。

**不要创建临时 Editor GUI/application。**

# 172. Phase 14 — Python Tier 1 static authoring

实现：

```text
@lux.method AST discovery
static type annotation resolution
GLOBAL_MODULE / ENTITY_BEHAVIOR export description
PYTHON_SOURCE ScriptAsset authoring/cook
binding catalog visibility
```

不实现 CPython runtime backend。runtime没有 Python backend时 cold-fail `BACKEND_NOT_AVAILABLE`。

# 173. Phase 15 — acceptance tests + benchmark v7 + installed consumer

正确性先于性能。不要为了恢复旧 benchmark数字削弱 object/lifetime contract。

# 174. Phase 16 — exact-SHA qualification

形成 `<NEW_PRODUCTION_SHA>`，clean detached worktree执行：

```text
Windows RelWithDebInfo / Developer
  full all
  second no-work
  CTest
  fresh install
  source+installed scan
  all installed consumers

Windows Debug / Developer
  full
  second no-work
  CTest

Windows RelWithDebInfo / Hardened
  contract checks
  full
  no-work
  CTest

Android arm64-v8a / PLAYER
  full
  no-work
  fresh install
  installed scan
  verify no Lua/MLIR/LLVM leakage unless selected

TOOLCHAIN + FlowForge/MLIR
  full
  no-work
  CTest

benchmark v7 formal performance
architecture scans
```

最后 evidence-only commit，其 parent必须就是 `<NEW_PRODUCTION_SHA>`。

---

# Part XXVIII — Freeze Gate

# 175. Code quality gate

```text
[ ] no borrowed future call_frame/payload pointers
[ ] exact Entity generation checked
[ ] Entity -> MULTI Hook works
[ ] Entity -> SINGLE Hook rejected
[ ] Event CONST_REF exact match
[ ] one primitive semantic namespace
[ ] Lua return/status separated
[ ] unsupported Lua marshal cold-rejected
[ ] backend kind duplicate rejected
[ ] backend lifetime contract proven
```

# 176. Object model gate

```text
[ ] EntityBehavior opt-in only
[ ] no ScriptComponent -> zero instances/lifecycle
[ ] constructor cannot assume self
[ ] stable self attached before CONSTRUCT
[ ] CONSTRUCT once
[ ] START once for initial and dynamic instances
[ ] normal callbacks only after START
[ ] methods share one object state
[ ] one method binds multiple targets
[ ] prepareMethod once per unique method
[ ] STOP once
[ ] RETIRING blocks normal callbacks
[ ] mount reorder preserves state
[ ] binding-only edit preserves state
```

# 177. Binding gate

```text
[ ] C++ LUX_METHOD/LUX_FUNC remain generic Meta markers; Python/Lua keep only @lux.method / ---@lux.method authoring markers
[ ] no bind_point/bind_event/lifecycle source annotations
[ ] no automatic function-name binding
[ ] ScriptAsset has no target/default bindings
[ ] all bindings explicit in composition
[ ] Editor filters by exact signature/scope/cardinality
[ ] duplicate diagnostic names allowed
[ ] strong target validation
```

# 178. ECS coexistence gate

```text
[ ] Components remain canonical state
[ ] bulk Systems bypass Behavior layer
[ ] plain Entity no universal object wrapper
[ ] Behavior component access only at proven safe windows
[ ] no raw Registry in Script public signature
```

# 179. Event gate

```text
[ ] System owns typed occurrence memory
[ ] producer capacities prepared
[ ] worker never runs user callback
[ ] payload survives producer scope
[ ] deterministic merge
[ ] Event before same Hook callback
[ ] targeted cost depends on actual target handlers, not scene size
```

# 180. Backend gate

C++：only marked methods enter RefMethod；Engine bridge projects exports；stable object；no hot ReflectionRegistry lookup/signature adaptation。Lua：code load once、instance per mount、hidden self、return slots、cold lookup。Native/FlowForge：state alignment、full ABI validation、instance-first。Python若不 shipping，不要求 fake runtime。

# 181. Qualification gate

```text
[ ] real worker test
[ ] sparse targeted benchmark
[ ] real instrumentation
[ ] successful installed runtime consumer
[ ] benchmark v7 producer/policy/evaluator same schema
[ ] 30-sample formal performance
[ ] previous core L1 groups retained
[ ] all fresh exact-SHA matrices
[ ] independent API acceptance after this refactor
```

只有全部通过：

```text
L1 = FROZEN
Formal L2 Process = GO
```

---

# Part XXIX — 明确禁止项

# 182. MUST NOT reintroduce

```text
SystemExecutionPoint
point-level dependency
scene-wide BROADCAST
universal ScriptSystem
process-global ScriptEventRegistry
global ScriptRuntime singleton
global CppBehavior registry
generic EventBus for System->Script
runtime function-name auto binding
ScriptAsset default_bindings
EScriptBindingSetMode
LUX_BIND_POINT
LUX_BIND_EVENT
LUX_BEHAVIOR_LIFECYCLE
std::function hot Hook storage
per-call Behavior allocation
per-call Lua function name lookup
per-call Python getattr
per-call runtime reflection
per-call Asset lookup
per-call signature rebuild
raw Registry& in Script API
fixed-size universal Behavior byte storage
mount ordinal as instance identity
borrowed future call_frame pointers
runtime compatibility shim for ScriptDescription v3
runtime compatibility shim for ScriptAsset wire v1
runtime compatibility shim for LXSD v3
```

# 183. 本轮不要过度设计

```text
multi-Behavior-per-ScriptAsset manager
Behavior inheritance serialization framework
enable/disable lifecycle
method-level ECS Read/Write annotations
script priority/reducer language
Entity SINGLE Hook reduction
hot reload state migration
save-game serialization of Python __dict__/Lua table
cross-language object inheritance
global plugin/script service locator
```

真实需求出现后再单独评估。

---

# Part XXX — 推荐模块边界

# 184. L0 Script Foundation

继续：

```text
modules/function/script/core/
    ScriptSemantic.hpp
    BoundScriptCall.hpp
    ScriptCallFrame.hpp
    ScriptValue.hpp
    ScriptSignature.hpp
    abi/lux_script_abi.h
```

当前 component 已分类 L0/CORE Foundation；不要仅为目录名字做无收益搬迁。

# 185. Meta

`modules/core/meta` 保持 generic C++ reflection/type truth：

```text
LUX_CLASS / LUX_METHOD / LUX_FUNC
RefClass / RefMethod / RefType
construct/destruct / MethodInvoker
```

MUST：

```text
LUX_METHOD 继续是 generic method reflection marker
LUX_FUNC 继续是 generic free-function reflection marker
只有显式 LUX_METHOD method进入 RefClass.methods
core/meta不依赖 Script
```

MUST NOT：

```text
Meta拥有 ScriptExportDescription
Meta拥有 Hook/Event/Behavior binding policy
Meta include ScriptSemantic/BoundScriptCall
```

当前 `modules/core/meta_script` 退休；必要 projection/invoke adapter进入 Engine/Simulation C++ Script bridge。

# 186. Resource

```text
modules/resource/description/Script.hpp v4
    callable exports only

modules/resource/asset/script/
    ScriptAsset codec v2
```

Resource不引用 System Hook/Event binding target。

# 187. Simulation description

```text
SystemHookPoint.hpp
SystemEventDescription.hpp
BehaviorLifecycle.hpp
ScriptBindingDescription.hpp
ScriptMountDescription.hpp
SimulationDescription.*
```

# 188. Engine/Simulation C++ Script bridge

建议作为 Simulation domain 的独立 pay-for-use integration component（准确目录名按现有 target命名约定）：

```text
inputs:
    core/meta RefClass/RefMethod/RefFunction
    resource ScriptDescription
    script_core ABI/BoundScriptCall
    EntityBehavior runtime context

owns:
    RefMethod/RefFunction -> ScriptExportDescription projection
    CppBehavior RefClass resolution
    prepared C++ method adapter
```

它不是 global registry，也不把 process-local Meta pointers写入资产。

# 189. Simulation script runtime

Public尽量只有：

```text
ScriptBindingSession
ScriptComponent
EntityBehavior/host facade where appropriate
```

Private：BindingResolver、InstanceTable、DispatchIndex、mount diff。Event buffer属于 System/private helper，不变成 bus。

# 190. Backend separation

如果 current `native` 已表示 loaded shared-library module，建议命名清晰区分：

```text
cpp_static/
native_module/
lua/
```

避免 C++ EntityBehavior 与 generic NativeModule 混在同一个 backend implementation。

# 191. Toolchain

```text
engine/toolchain/flowforge/
engine/toolchain/script/ (Lua/Python importer if implemented)
```

Source annotations只在 toolchain/import cold path解析；runtime不读源码注解。

---

# Part XXXI — 实施 LLM 每阶段自检

每个 phase提交前必须回答：

```text
1. 我是否新增了第二个 binding source of truth？
2. ScriptAsset 是否偷偷又知道 target？
3. 一个 method 绑定三个 target 是否只 prepare 一次？
4. 一个 Entity binding edit 是否会重建其他 Entity object？
5. Entity slot reuse 是否可能调用旧 Behavior？
6. Event payload 是否由 producer buffer真正拥有？
7. Lua/Python 是否在每次调用做 name lookup？
8. C++ generic Meta 是否把未标记 public helper 错误放进 RefClass.methods，或 Engine bridge把未选 callable错误导出？
9. 没有 ScriptComponent 的 Entity 是否真的零 Behavior runtime？
10. ECS bulk System 是否仍完全绕开 Behavior layer？
11. Script component access 是否有明确 safe window？
12. 业务 return 与 ABI status 是否完全分离？
13. installed consumer 是否真实成功调用，而不是只验证失败？
14. benchmark counters 是否真实 instrumentation，而不是 literal zero？
15. evidence 是否来自新的 production SHA，而非沿用 393180？
```

任一答案“不确定”：

```text
DO NOT mark freeze candidate.
```

---

# 192. 最终架构定义

```text
ECS stores the world.
Systems process the world at scale.
EntityBehavior gives selected Entities long-lived object-oriented gameplay context.
ScriptAsset exports callable symbols only.
Composition binds those symbols to typed Hook/Event/Lifecycle targets.
Backends create one stable instance per mount and prepare each used method once.
System hot paths only invoke BoundScriptCall.
```

最短版本：

```text
Export once.
Instantiate once.
Bind explicitly.
Prepare once.
Invoke many.
Retire once.
```

这就是下一轮 production implementation 必须达到的最终形态。
