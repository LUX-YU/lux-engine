# Script Ability Reflection、Provider Binding 与 Multi-language Codegen

Status: **Normative Implementation Contract (S1.5)**  
Date: **2026-09-02**  
Parent: `11-script-api-capabilities-coroutines-and-await.md`

> 本文件冻结 Script Ability reflection/codegen 的 physical ownership、receiver/provider instance binding、CMake opt-in、generated artifacts 与 multi-language projection。它 supersede 任何把 Engine Script SDK schema 集中放进 `modules/function/script/sdk` 的旧建议，也 supersede `07` 中与 S1.5 次序冲突的 scripting 子 DAG 表述。

---

## 1. Goal

S1.5 的目标不是实现 Physics、Navigation、Python 或完整 FlowForge coroutine。

目标是建立唯一通路：

```text
Engine/domain owner declares callable Ability
        ↓
CMake explicitly opts source/types into reflection/codegen
        ↓
canonical generated Ability descriptor/schema
        ↓
generated provider conformance + binder/thunk
        ↓
composition binds existing provider instance
        ↓
prepared Script capability
        ↓
C++ / Lua / FlowForge / future Python projection consume same schema
```

必须证明：

```text
static contract declaration
!= runtime provider instance ownership
!= language-specific wrapper
```

---

## 2. Physical ownership

### 2.1 `modules/`

`modules/` 保持 Engine-independent/reusable。

允许：

```text
portable reflection/parser/codegen primitives
Script ABI/value primitives
ScriptArtifact-neutral reusable data structures
language-generic template utilities
```

禁止：

```text
SimulationSystem
Scene capability
PhysicsQuery3D Engine declaration
Entity/Component Script API owned by Simulation
AssetLoading Engine integration ability
SystemInstanceId-based provider registry
```

不要创建：

```text
modules/function/script/sdk/
    EntityApi
    PhysicsApi
    AssetApi
    SceneCapability...
```

作为 Engine API source of truth。

### 2.2 Engine/domain package

Ability declaration 跟随真实 semantic owner。

示例，不强制 exact folder name：

```text
engine/domain/simulation/builtin/physics/
├─ abilities/
│  ├─ PhysicsQuery3D.hpp
│  └─ PhysicsBody3D.hpp
└─ ...

engine/domain/simulation/ecs/
└─ abilities/
   ├─ Entity.hpp
   ├─ Component.hpp
   └─ Query.hpp
```

Asset ability 应放在真实 Engine-facing asset-loading/integration owner，而不是为目录整齐硬塞到 Simulation 或 Script package。

原则：

> declaration follows semantic owner; generated projection follows build graph.

---

## 3. Ability declaration model

第一版推荐 contract-first declaration，而不是 concrete provider-first reflection。

概念：

```cpp
LUX_SCRIPT_ABILITY(PhysicsQuery3D)
struct PhysicsQuery3D
{
    LUX_SCRIPT_QUERY
    RaycastHit raycast(const RaycastRequest& request);

    LUX_SCRIPT_ASYNC
    RaycastHit raycastAsync(const RaycastRequest& request);
};
```

宏/attribute 的 exact spelling **尚由实施结合现有 meta parser 风格决定**；语义必须固定：

```text
Ability identity
method identity/name
method kind: QUERY / COMMAND / ASYNC_OPERATION
parameter/result type schema
receiver kind
lifetime category
optional documentation/display metadata
```

不得把 concrete provider type 写进 script-visible signature。

错误：

```cpp
raycast(JoltPhysicsSystem&, RaycastRequest);
```

正确 contract：

```text
PhysicsQuery3D.raycast(RaycastRequest) -> RaycastHit
receiver = PROVIDER_INSTANCE
```

---

## 4. Receiver model

v1 只允许：

```text
NONE
PROVIDER_INSTANCE
```

### 4.1 NONE

适用于 truly stateless/pure callable。

Generated dynamic thunk 可以：

```text
context = nullptr
```

### 4.2 PROVIDER_INSTANCE

适用于需要 runtime object/state 的能力。

例如：

```text
PhysicsQuery3D -> PhysicsSystem instance
EntityApi      -> ECS endpoint instance
AssetLoading   -> Asset loading endpoint instance
```

Receiver 不出现在语言 signature 中。

Lua 可以看到：

```text
Physics.raycast(request)
```

实际 dynamic binding：

```text
context = provider instance
invoke  = generated thunk
```

---

## 5. Generated provider conformance

Contract 不强迫 concrete provider 继承 virtual base。

推荐 codegen 产生 compile-time conformance check / concept / static assertion。

概念：

```cpp
template<class Provider>
concept PhysicsQuery3DProvider = requires(
    Provider& provider,
    const RaycastRequest& request)
{
    { provider.raycast(request) } -> std::same_as<RaycastHit>;
};
```

Exact generated form 可不同，但 MUST：

```text
provider mismatch detected at build/composition boundary where possible
no required virtual inheritance
no RTTI/dynamic_cast requirement
no concrete backend name embedded into contract identity
```

如果 provider 方法名/shape 需要 adapter，允许 package 自己提供显式 typed adapter；不得靠 string-based runtime mapping。

---

## 6. Generated binder/thunk

对于 `PROVIDER_INSTANCE`，codegen 产生 typed binder：

```cpp
bindPhysicsQuery3D(Provider& provider)
    -> BoundScriptCapability
```

Binder 的职责：

```text
validate/encode contract identity + schema
capture non-owning receiver pointer
populate method thunk/table
attach diagnostics metadata
optionally record owner identity supplied by composition
```

Generated thunk 概念：

```cpp
RaycastHit raycastThunk(
    void* context,
    const RaycastRequest& request) noexcept
{
    auto& provider = *static_cast<Provider*>(context);
    return provider.raycast(request);
}
```

Codegen MUST NOT：

```text
new Provider
make_shared<Provider>
static Provider instance
lookup provider from global registry
own or destroy provider
```

---

## 7. Runtime owner and composition binding

Provider object 由真实 composition owner 创建。

对于 SimulationSystem，当前模型已经是：

```text
SimulationBuilder / Simulation
    creates and owns System instance
```

Ability binding 发生在 provider 已成功安装以后：

```text
emplace/install provider
        ↓
bind generated Ability(provider)
        ↓
publish frozen capability
```

概念：

```cpp
auto physics = builder.emplaceSystem<JoltPhysicsSystem>(id, ...);
if (!physics)
    return unexpected(...);

// exact API TBD; semantics frozen
builder.publishScriptAbility(
    id,
    generated::bindPhysicsQuery3D(**physics)
);
```

这里：

```text
Simulation owns JoltPhysicsSystem
Bound Ability borrows JoltPhysicsSystem
ScriptSystem never owns JoltPhysicsSystem
```

---

## 8. Provider owner identity

Bound capability 可以记录 runtime owner identity 用于 diagnostics/lifetime validation。

对于 SimulationSystem provider：

```text
owner = SystemInstanceId
```

对于非-System provider，可使用其真实 composition-level stable identity/owner record；不要为了统一而强行给所有 provider 发 `SystemInstanceId`。

ScriptArtifact requirement 不保存 owner identity，只保存：

```text
ContractId
SchemaHash/ABI version
```

---

## 9. Default provider uniqueness

v1 一个 composition scope 内，每个 Script API contract 最多一个 default active provider。

若出现：

```text
PhysicsQuery3D <- Provider A
PhysicsQuery3D <- Provider B
```

必须：

```text
SCRIPT_CAPABILITY_AMBIGUOUS_PROVIDER
```

并 fail closed。

禁止：

```text
first registered wins
last registered wins
string name lookup to select provider
implicit priority number
```

未来若真实需求要求 multiple physics worlds/providers，再单独设计 explicit provider selection contract。

---

## 10. CMake codegen opt-in

Ability reflection 必须是 target/package 显式 opt-in，不扫描整个 repository。

概念 API：

```cmake
lux_script_abilities(
    TARGET lux_engine_simulation_builtin_physics
    SOURCES
        abilities/PhysicsQuery3D.hpp
        abilities/PhysicsBody3D.hpp
)
```

Exact CMake function name 可以按现有 Lux codegen convention 调整，但 MUST：

```text
input sources/types explicit
outputs declared to build system
generated files live under build/generated or equivalent
incremental dependency tracking works
second build with no source changes does no unnecessary regeneration
installed/public closure does not reference source-tree absolute paths
```

MUST NOT：

```text
glob all headers and reflect implicitly
write generated output into checked-in source directories
make domain package depend on Lua/FlowForge/Python implementation
```

---

## 11. Two-stage codegen

Codegen 分两阶段。

### Stage A — owner-side canonical reflection

Domain package input：

```text
abilities/*.hpp
component/meta declarations where applicable
```

输出 language-neutral generated data/code：

```text
Ability descriptor/schema
stable contract/method identity
method kind
parameter/result schema
receiver kind
lifetime metadata
generated typed provider binder/thunks
```

### Stage B — language/tool projection

消费者读取 canonical schema：

```text
C++ projection
Lua registration/stubs
FlowForge node/catalog contribution
future Python registration/.pyi
```

Domain package不直接知道这些 consumer implementation。

依赖方向应是：

```text
canonical reflected metadata
        ↑
owner package

language/tool projection target
        ↓ consumes
canonical metadata
```

而不是 Physics -> Lua / Physics -> FlowForge。

---

## 12. Component/ECS API projection

Entity/Component/Query 是重要公共 Script ability，但 declaration owner 仍在 Simulation/ECS engine package。

现有 Component metadata/codegen 可以作为输入：

```text
Component declaration/meta
        ↓
canonical component schema
        ↓
script projection
```

第一版 semantic operations 建议：

```text
Entity.valid
Entity.create
Entity.destroy

Component.has<T>
Component.get<T>
Component.patch<T>
Component.add<T>
Component.remove<T>

Query<T...>
```

不要实现 string query language。

动态语言 `get<T>` 不得长期暴露 raw ECS storage pointer。

---

## 13. Lifetime metadata

Canonical schema 必须能表达：

```text
OWNED_VALUE
STABLE_ID
BORROWED_STEP
AWAITABLE
```

示例：

```text
EntityId             -> STABLE_ID
AssetId              -> STABLE_ID
RaycastHit           -> OWNED_VALUE
Component reference  -> BORROWED_STEP
Query iterator/view  -> BORROWED_STEP
Async AssetLoad      -> AWAITABLE<OWNED_VALUE/...>
```

任何 `BORROWED_STEP` 值不得跨 coroutine suspension。

FlowForge projection必须能用该 metadata 做 static validation。

Lua/Python projection不得把它包装成可无限持有且看似安全的对象。

---

## 14. Method kind projection

Canonical schema中的：

```text
QUERY
COMMAND
ASYNC_OPERATION
```

决定语言/tool projection 行为。

例如：

```text
QUERY
    C++       -> immediate value
    Lua       -> immediate value
    FlowForge -> normal node

ASYNC_OPERATION
    C++       -> ScriptAwaitable<T> / later co_await surface
    Lua       -> awaitable/yield bridge surface
    FlowForge -> suspension-capable node
```

不得通过 method name suffix 猜 asyncness。

---

## 15. Lua projection

现有 `modules/function/script/lua` 已有 reusable Lua codegen/registration machinery；S1.5 应复用其真正 Engine-independent 部分，而不是把 Engine ability declaration 移入 modules。

S1.5 最小 Lua proof：

```text
one test Ability
canonical schema
        ↓
generated/derived Lua registration wrapper
        ↓
call reaches bound provider instance
```

本阶段不实现 Lua coroutine S4。

---

## 16. FlowForge projection

S1.5 只需要证明 canonical Ability 可以贡献/生成一个最小 FlowForge API node/catalog entry。

FlowForge source node identity必须基于：

```text
ScriptApiContractId
ScriptApiMethodId
```

禁止保存：

```text
Provider class name
Jolt method symbol
raw function pointer
```

本阶段不实现 coroutine lowering；ASYNC_OPERATION 可以生成/描述 suspension capability metadata，但真正 state-machine lowering 属于 S3。

---

## 17. C++ projection

S1.5 证明同一 canonical Ability 可得到 typed C++ facade/binding。

Dynamic/development path允许：

```text
prepared context + thunk/table
```

未来 shipping specialization必须仍有空间：

```text
known provider type
        ↓
generated typed direct adapter
        ↓
LTO/devirtualization/inlining
```

不要把 function-pointer layout 写进 semantic contract schema。

---

## 18. External project support

外部项目也可以声明自己的 Engine-owned abilities，例如：

```text
MyGame/
└─ simulation/
   └─ abilities/
      └─ InventoryAbility.hpp
```

只要该 project target能调用同一 Lux CMake reflection/codegen function。

因此 codegen tool本身应具有可复用安装/SDK surface，但 **project-specific Ability declaration仍属于项目/Engine domain，不属于 `modules/`**。

---

## 19. Generated artifact placement

推荐：

```text
<build>/generated/<target>/script_abilities/...
```

或项目现有等价 generated-tree convention。

生成内容可包括：

```text
*.ability.generated.hpp
*.ability.generated.cpp
canonical descriptor tables
language/tool contribution metadata
```

不要将 generated 文件提交为手写 source truth，除非项目已有经过批准的 generated-source policy。

---

## 20. ABI and install boundary

需要区分：

```text
canonical semantic schema
runtime dynamic binding ABI
language-specific binding ABI
```

S1.5 不应该因为 codegen 引入第二套平行 Script ABI。

现有 Script ABI/Artifact 方向继续保留；如果 canonical ability schema 无法映射到现有 ABI 而必须破坏持久/installed contract，STOP 做专门 architecture review。

安装后 codegen consumer不得依赖 repo source absolute path。

---

## 21. Teardown/lifetime tests

必须有真实 lifetime proof：

```text
provider constructed once by owner
ability binder does not construct provider
ability binding does not own provider
script/capability binding cleared before provider destruction
provider destroyed exactly once
late script continuation cannot invoke destroyed provider
```

禁止以 `shared_ptr` reference count 作为 correctness proof。

---

## 22. S1.5 minimum implementation scope

只实现：

```text
1. one canonical Ability declaration representation
2. receiver NONE / PROVIDER_INSTANCE metadata
3. explicit CMake opt-in
4. generated descriptor/schema
5. generated typed provider binder/thunk
6. composition publication of a test provider
7. mount requirement resolution against generated capability
8. one C++ projection proof
9. one Lua projection proof
10. one FlowForge catalog/node projection proof
11. lifetime metadata representation
```

推荐使用：

```text
TestAbility / TestProvider
```

不要以 Physics 作为第一 proof，否则会把 reflection/codegen 风险和 physics integration 风险混在一起。

---

## 23. Explicitly NOT in S1.5

```text
real Physics API
real Navigation API
AssetLoad await execution
Delay implementation
Event.await
FlowForge coroutine lowering
Lua coroutine bridge
C++ co_await ergonomics
Python runtime
provider hot swap
multiple provider selection
semantic version negotiation
universal reflection registry
```

---

## 24. Tests

至少覆盖：

```text
contract descriptor is deterministic
method identity/schema stable across rebuild
CMake only regenerates when relevant input changes
PROVIDER_INSTANCE binder calls exact existing provider object
NONE receiver works without object
binder does not allocate/own provider
provider compile-time mismatch fails at build where practical
missing provider -> SCRIPT_CAPABILITY_NOT_FOUND
schema mismatch -> SCRIPT_CAPABILITY_SCHEMA_MISMATCH
duplicate default provider -> SCRIPT_CAPABILITY_AMBIGUOUS_PROVIDER
prepared call has no runtime string lookup
Lua proof reaches same provider
FlowForge generated/contributed node uses contract+method ID
C++ proof uses same canonical contract
BORROWED_STEP metadata survives projection
```

---

## 25. Qualification

至少运行：

```text
Default Developer
PLAYER
TOOLCHAIN
```

如果 generated public headers/installed codegen SDK 进入 Editor closure，同时运行 EDITOR。

如果安装 surface/CMake package 被修改：

```text
install
relocated consumer
second build -> no work to do
```

检查 generated output 不包含 source/build absolute path leakage（除非明确允许的 debug-only metadata）。

---

## 26. STOP conditions

出现以下任一情况必须停止：

```text
必须把 Engine ability declaration 放进 modules 才能 codegen
必须建立全局 runtime reflection registry
必须让 codegen own/create provider
必须让 ScriptSystem shared-own provider
必须把 concrete provider type写进 ScriptArtifact requirement
必须让 Physics/domain target link Lua/FlowForge/Python implementation
CMake 必须扫描整个 repo 才能发现 Ability
需要 provider hot swap/multiple provider routing 才能完成最小 proof
需要修改 project target-generation manifest contract
需要并行维护 legacy/new 两套 Ability ABI compatibility shim
```

---

## 27. S1.5 completion gate

只有以下全部成立才允许进入 S2：

```text
[ ] Engine/module ownership boundary preserved
[ ] Ability declaration follows semantic owner
[ ] explicit CMake codegen opt-in exists
[ ] canonical schema generated deterministically
[ ] receiver NONE / PROVIDER_INSTANCE frozen
[ ] generated binder borrows existing provider
[ ] provider lifetime remains with original owner
[ ] missing/schema/ambiguous diagnostics work
[ ] one C++ projection proof
[ ] one Lua projection proof
[ ] one FlowForge projection proof
[ ] lifetime metadata represented
[ ] no per-call string/service lookup
[ ] clean qualification passes
```

Then and only then proceed to S2 `NextStep / Delay / AssetLoad`.
