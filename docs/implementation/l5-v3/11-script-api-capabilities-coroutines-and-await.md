# Script API Capability Contracts、Coroutine / Await 与 Provider Binding

Status: **Normative Runtime Scripting Design (v2, v3 docset)**  
Date: **2026-09-02**  
Parent documents: `00-L5-architecture-overview.md`, `07-implementation-roadmap-and-gates.md`, `08-normative-execution-contract.md`, `09-product-runtime-vfs-and-async-script.md`  
Companion implementation document: `12-script-ability-reflection-provider-binding-and-codegen.md`

> Normative scripting priority: 对 Script API capability、provider binding、coroutine/await、Event.await 和 Delay 时间语义，本文件 supersede `06/07/08/09` 中任何冲突的旧 scripting 条款。Ability reflection、CMake codegen、receiver/provider instance 绑定和多语言 projection 的精确实施规则以 `12` 为准。`08` 在未被 `11/12` 明确 supersede 的 general execution/ownership 规则上仍保持最高优先级。

---

## 1. 统一 ontology

Lux runtime scripting 冻结为：

```text
Component
    = state/data contract

System / integration object
    = behavior/runtime owner

Script Ability / Script API
    = callable contract

Capability Provider
    = runtime object that implements a callable contract

HookPoint / EventPoint
    = engine-to-script execution/event contract

Coroutine / Awaitable
    = time-spanning script control-flow contract
```

关键修正：

```text
System is often a Provider
but Provider is not necessarily a System.
```

例如：

```text
PhysicsQuery3D       <- JoltPhysicsSystem
NavigationQuery      <- NavigationSystem
Entity/Component API <- Simulation ECS host endpoint
AssetLoading         <- Scene/Application/Process integration endpoint
```

Script API 描述语义，不规定底层一定使用 function pointer、virtual interface 或某一种 ABI dispatch。

---

## 2. `modules/` boundary

`modules/` 是可被 Lux Engine 之外项目复用的通用 function/resource 基础设施。

因此 MUST NOT 因为 scripting capability/codegen 把以下 Engine ontology 下沉到 `modules/`：

```text
Scene
Simulation
SimulationSystem
SystemInstanceId semantics
Scene capability registry
PhysicsSystem ownership
Engine-specific AssetLoading ownership
```

`modules/function/script` 可以继续拥有真正通用的：

```text
ScriptSymbol / ScriptArtifact vocabulary
portable Script ABI/value representation
language-generic callable/result vocabulary
reusable parser/codegen primitives that do not know Engine ownership
Lua/native reusable support where already valid
```

但 Engine Script Ability 的声明源必须跟随真实 semantic owner 位于 `engine/...` 或外部项目自己的 package 中。

禁止新增一个 `modules/function/script/sdk` 并把 Engine 的 Entity/Physics/Scene/Asset API schema 集中塞进去。

---

## 3. Ability contract 与 concrete provider 分离

脚本依赖 callable contract，不依赖 concrete provider identity。

```text
PhysicsQuery3D contract
├─ raycast
├─ raycastAsync
├─ sweep
└─ overlap

JoltPhysicsSystem
    provides PhysicsQuery3D

AlternativePhysicsSystem
    provides PhysicsQuery3D
```

只要两个 provider 真正满足同一语义 contract，Script/FlowForge source 不应知道 Jolt、PhysX 或其他 backend 名称。

MUST NOT 为了“可替换”强行统一语义并不相同的接口。例如 2D/3D physics 如果语义不同，应分别形成真实 contract，而不是伪造 universal Physics API。

Ability declaration 跟随 domain owner。例如 future physics package 可采用：

```text
engine/domain/simulation/builtin/physics/
├─ abilities/
│  ├─ PhysicsQuery3D.hpp
│  └─ PhysicsBody3D.hpp
├─ ... provider implementation ...
└─ CMakeLists.txt
```

这不是强制所有 package 使用完全相同目录名；规范要求的是 **declaration follows semantic owner**，而不是 central Script API registry。

---

## 4. Static contract declaration vs runtime provider instance

必须严格区分：

```text
Ability declaration
    = static/type-level contract

Provider instance
    = runtime object with state/lifetime

Generated binder
    = binds one contract to one existing provider instance
```

Codegen MUST NOT：

```text
construct provider objects
own provider objects
introduce static singleton provider
extend provider lifetime with shared ownership
perform runtime service lookup per method call
```

Provider object 继续由其真实 owner 创建和销毁。

对于 SimulationSystem，当前 Simulation composition 已经负责创建/持有 System instance；generated ability binding 只能 non-owning borrow 该 instance。

---

## 5. Receiver model

v1 receiver kind 只冻结两个：

```text
NONE
PROVIDER_INSTANCE
```

### 5.1 NONE

真正无 runtime state 的 pure/static callable 可以没有 receiver。

### 5.2 PROVIDER_INSTANCE

绝大多数 Engine ability 绑定到一个 runtime provider object：

```text
PhysicsQuery3D -> PhysicsSystem instance
EntityApi      -> ECS host endpoint instance
AssetLoading   -> asset-loading integration instance
```

Receiver 是 binding metadata，不属于 script-visible parameter list。

禁止把 concrete provider 写成 script signature，例如：

```cpp
raycast(JoltPhysicsSystem&, RaycastRequest); // wrong public contract shape
```

contract 应只表达：

```text
PhysicsQuery3D.raycast(RaycastRequest) -> RaycastHit
receiver = PROVIDER_INSTANCE
```

具体 provider conformance/binding 由 generated code 负责。

---

## 6. Provider ownership and lifetime

推荐 runtime relation：

```text
Simulation / real owner
    OWNS provider object

Generated Bound Ability
    BORROWS provider object

ScriptSystem prepared capability binding
    BORROWS/copies immutable call table + owner identity
```

不得使用 `shared_ptr<System>` 让 Script capability 延长 System 生命周期。

必须满足 teardown invariant：

```text
stop new script invocations
invalidate script instances/generations
cancel/destroy continuations
clear prepared ability bindings
then destroy provider objects
```

如果当前 Simulation teardown 无法保证上述关系，实施必须 STOP 做 architecture review，而不是用 shared ownership 掩盖问题。

---

## 7. Capability publication and ambiguity

Composition 阶段把 generated binder 应用于已经存在的 provider instance：

```text
create/install provider
        ↓
generated bind Ability(provider)
        ↓
publish frozen capability
        ↓
Script mount resolves requirement once
```

v1 一个 composition scope 中，一个 required `ScriptApiContractId` 只能有一个默认 active provider。

如果两个 provider 同时发布相同 contract 且没有更高层明确 selection：

```text
SCRIPT_CAPABILITY_AMBIGUOUS_PROVIDER
```

必须 fail composition/mount；不得选择“第一个”、按注册顺序随机绑定，也不得因此发明 named service locator。

Provider publication 可记录 owner identity（例如 `SystemInstanceId`）用于 diagnostics/lifetime validation，但 ScriptArtifact requirement 仍只引用 contract identity/schema，不引用 concrete owner/provider。

---

## 8. Stable contract identity

运行时 identity 必须稳定、typed/structured，不使用字符串作为 hot-path identity。

概念类型：

```cpp
struct ScriptApiContractId;
struct ScriptApiMethodId;
```

name 只用于 Editor、diagnostic、debugger、codegen。

v1 compatibility 使用简单的：

```text
ContractId + SchemaHash / ABI version
```

Script requirement 与 provider publication 必须 exact-compatible；不一致返回：

```text
SCRIPT_CAPABILITY_SCHEMA_MISMATCH
```

第一版不建立 semantic-version negotiation framework。

---

## 9. Script API method kinds

第一版 semantic kind：

```text
QUERY
    guaranteed synchronous result at a legal semantic point

COMMAND
    submit/apply action; caller does not await result

ASYNC_OPERATION
    returns ScriptAwaitable<T>; may complete later
```

Event 继续由 EventPoint 表达，不塞进 generic method table。

Asyncness belongs to contract metadata；不得仅依赖方法名中的 `Async` 推断。

---

## 10. ScriptArtifact requirements 与 fail-early mount

ScriptArtifact 除 exports 外必须能够声明/携带 Script API requirements：

```text
Requires:
    PhysicsQuery3D / schema X
    AssetLoading / schema Y
```

requirement 指向 contract，不指向：

```text
JoltPhysicsSystem
specific SystemInstanceId
provider class name
```

Script mount/bind MUST 在执行前验证：

```text
asset/export exists
Hook/Event target exists
signature/scope compatible
all required Script API contracts exist
contract schema compatible
provider is unambiguous
```

错误必须区分：

```text
SCRIPT_ENDPOINT_NOT_FOUND
    engine -> script Hook/Event target missing

SCRIPT_CAPABILITY_NOT_FOUND
    script -> engine callable contract missing

SCRIPT_CAPABILITY_SCHEMA_MISMATCH
    contract exists but incompatible

SCRIPT_CAPABILITY_AMBIGUOUS_PROVIDER
    multiple default providers for one contract
```

Scene/Simulation 没有 Physics capability 时，使用 PhysicsQuery3D 的 script 必须在 mount/bind 阶段 fail early，而不是执行 `raycast()` 时得到 null service。

---

## 11. Dispatch strategy 与 contract 分离

动态边界允许 prepared binding：

```text
context/receiver pointer
+
generated function pointer/table
```

这是 dynamic binding strategy，不是 contract 本身。

MUST：

```text
resolve once at composition/mount
immutable prepared binding
no per-call string lookup
no dynamic_cast/service lookup hot path
```

MUST NOT 把 `std::function` 或 mandatory virtual inheritance 作为 canonical Script Ability binding。

对于 project-specific shipping target，允许同一 semantic contract 使用 generated/static specialization：

```text
Project selects PhysicsQuery3D -> JoltPhysicsSystem
        ↓
generated C++ / FlowForge lowering
        ↓
direct typed call / direct IR callee
        ↓
LTO / whole-program optimization
```

Contract 与 dispatch strategy 分离；dynamic language/plugin boundary 可间接调用，已知 shipping hot path 可静态特化/内联。

---

## 12. Ability reflection / codegen source of truth

Ability reflection/codegen 的精确规则见 `12-script-ability-reflection-provider-binding-and-codegen.md`。

本文件只冻结这些边界：

```text
reflection declaration lives with semantic owner
CMake explicitly opts selected source/types into codegen
codegen produces canonical contract metadata + binder/thunks
language projection consumes canonical metadata
provider package does not depend on Lua/FlowForge/Python
```

不要让 Physics CMake target 自己直接手写/拥有 Lua、FlowForge、Python binding implementation。

---

## 13. Core/common APIs

通用脚本能力仍然遵循真实 owner，而不是集中塞进 Script module：

```text
Entity / Component / Query
    declared near Simulation ECS-facing owner

Simulation Time / Delay
    declared near Simulation scripting/time owner

Asset Loading
    declared at the Engine-facing asset-loading/integration owner

Diagnostics
    declared at its real low-level/application-facing owner if exposed
```

Component-specific projection可以利用现有 component/meta codegen，但 Script API 的 Engine ownership 不得因此下沉到 `modules/`。

动态语言的 Component `get` 第一版应优先返回 owned value 或 validated step-local proxy；不得让 VM 长期持有裸 ECS component pointer。

---

## 14. Lifetime category across await

Coroutine 引入后，generated API/schema 必须能够表达至少这些结果 lifetime：

```text
OWNED_VALUE
STABLE_ID
BORROWED_STEP
AWAITABLE
```

硬规则：

```text
BORROWED_STEP value / component reference / query iterator
MUST NOT cross a suspension point.
```

FlowForge compiler 应在可静态证明时拒绝 borrowed value crossing `await`。

C++/Lua/Python projection 也不得把 step-local ECS pointer 包装成可无限保存的安全对象。

---

## 15. Coroutine 是用户模型；continuation 是 Engine 模型

用户层跨帧控制流采用 coroutine/await：

```text
Lua        coroutine/yield-resume
C++        co_await
FlowForge  visible sequential graph + suspension node
Python     future await
```

Engine 核心采用 backend-neutral continuation，不将任何语言 coroutine representation 作为通用 ABI。

必须支持：

```text
RUNNING
   ↓ await
SUSPENDED
   ↓ resume
RUNNING
   ↓
COMPLETED / FAILED
```

明确执行结果：

```text
COMPLETED
SUSPENDED
FAILED
```

不得把 `SUSPENDED` 偷塞进旧 success/error integer 的 magic value。

---

## 16. Stable generational identity

至少需要：

```text
ScriptInstanceId
ScriptContinuationId
ScriptAwaitableId
```

跨线程/跨帧 completion 不传：

```text
ScriptInstance*
coroutine_handle*
lua_State*
provider raw lifetime token
raw continuation pointer
```

completion 只携带稳定 ID + owned result/error；Simulation resume 时重新 resolve 并校验 generation。

---

## 17. Backend-neutral continuation

概念边界：

```cpp
struct ScriptBackendContinuation
{
    void* state{};
    ScriptStepResult (*resume)(void*, const ScriptResumePacket&) noexcept {};
    void (*destroy)(void*) noexcept {};
};
```

实际 representation 私有：

```text
FlowForge -> compiler-generated state machine + locals
Lua       -> Lua coroutine/thread reference
C++       -> std::coroutine_handle<> inside C++ backend only
Python    -> future coroutine/task reference
```

`ScriptSystem` MUST NOT know backend-native coroutine representation。

---

## 18. Awaitable model

所有 time-spanning wait 统一投影为 `ScriptAwaitableId`：

```text
Delay.nextStep()
Delay.seconds()
Delay.realSeconds()
Asset.load()
Physics async query
GPU query
Navigation async operation
Event.next()
```

Awaitable 至少有：

```text
PENDING
READY
CANCELLED
FAILED
```

storage 必须 bounded + generational，并正确处理 operation 在 waiter registration 之前已经 READY 的 eager-completion race。

---

## 19. Stable resume point

任何 external completion/event 都只能：

```text
mark Awaitable READY
        ↓
enqueue ScriptResume
```

禁止：

```text
worker -> coroutine.resume()
timer thread -> lua_resume()
physics callback -> FlowForge continue()
GPU callback -> coroutine_handle.resume()
```

Simulation 在明确 stable resume point：

```text
drain bounded resume queue
validate ScriptInstance generation
resolve Continuation
provide owned result/error
resume backend
```

resume queue 必须 bounded，并有 per-step resume budget。

已 READY awaitable 的后续 resume 使用 tail enqueue，禁止递归 resume chain。

---

## 20. Event + await

Hook/Event callback 模式继续存在，同时允许：

```text
await Event.next(...)
```

共享现有 EventPoint source：

```text
EventPoint
    ↓
ScriptSystem EventBucket
    ├─ normal bound handlers
    └─ coroutine waiters
```

MUST NOT 为每一个 waiter 临时 `EventPoint.connect()`/disconnect。

跨 dispatch 生命周期消费的 Event payload 必须 marshal/copy 到 owned resume storage；不得保存当前 call-frame/payload pointer。

---

## 21. Delay 时间语义

第一版冻结：

```text
Delay.seconds(x)
    == Delay.simulationSeconds(x)

Delay.realSeconds(x)
    = monotonic real time

Delay.nextStep()
    = next eligible Simulation step
```

Simulation-time Delay 使用 Simulation clock/deadline queue；不得一条 gameplay Delay 对应一个 Process TimerSender，也不得每帧扫描所有 suspended Delay。

原则：

```text
never early
resume at first eligible Simulation stable point at/after deadline
```

`Delay.realSeconds()` 可桥接 monotonic TimerClient/TimerSender，但 ready 只 enqueue；真正脚本执行仍在 Simulation stable point。

---

## 22. Actual async operation ownership

`ScriptSystem` owns：

```text
script instance state
continuation storage
awaitable storage
resume queue
Event waiter semantics
Simulation-time delay scheduling semantics
```

真正 time-spanning work 由真实 owner 提供：

```text
real timer    -> Process execution integration
AssetLoad     -> AssetReadPort/loadAsset<T>()
Physics query -> Physics/Scene integration
GPU query     -> Render/Scene integration
Navigation    -> corresponding domain/integration
```

L1 Simulation 不应仅因为支持 await 就直接拥有 worker pool、Process TaskScope、VFS blocking IO 或 GPU fence。

---

## 23. Cancellation / shutdown

Scene stop、script unmount/reload、entity/script-instance invalidation 必须：

```text
invalidate ScriptInstance generation first
mark/cancel pending Awaitables
request stop on external operations where supported
destroy backend continuation at safe point
clear prepared capability bindings before provider destruction
```

迟到 completion resolve 到旧 generation：discard。

绝不能 resume stale backend state 或访问已销毁 provider。

---

## 24. Fan-out policy

v1 recurring Hook invocation 为 single-flight：

```text
previous Hook invocation is SUSPENDED
        ↓
next recurring Hook trigger does not start another copy
```

Event invocation 可以 multi-flight，但必须有 per-instance/global continuation capacity、bounded resume queue 和 per-step resume budget。

---

## 25. FlowForge / Lua / C++ / future Python projection

所有语言/FlowForge consuming 的 source of truth 是 canonical reflected Ability metadata + Component/meta information，不是各 backend 手写第二份语义定义。

```text
Ability declaration / Component metadata
        ↓
canonical generated schema/descriptor
        ↓
C++ projection
Lua projection
FlowForge node/catalog projection
future Python projection
```

FlowForge API node 保存 stable contract/method identity，不保存 concrete provider method name。

ASYNC_OPERATION lowering 形成 explicit state-machine suspension。

Lua coroutine、C++ coroutine、future Python coroutine 都适配同一 Engine continuation contract。

---

## 26. Performance contract

MUST 保持：

```text
ordinary synchronous BoundScriptCall path does not allocate continuation
Ability/provider resolution happens once, not per call
prepared dynamic call uses narrow immutable binding
continuation/awaitable/resume storage is bounded
```

高频已知 shipping path允许 generated/static specialization + LTO。

真正高频 data-parallel API 应优先考虑 batch contract，而不是只优化一次 indirect-call 的纳秒级成本。

---

## 27. Implementation sequence

Runtime scripting 子 DAG：

```text
S1
Capability identity + requirements
Provider publication/binding foundation
Continuation/Awaitable/Resume foundation
        ↓
S1.5
Ability reflection declaration
Provider receiver/binder generation
CMake codegen opt-in
Canonical schema + minimal multi-language projection proof
        ↓
S2
NextStep / Simulation Delay / Real Delay / AssetLoad
        ↓
S3
FlowForge generated API nodes + coroutine lowering
        ↓
S4
Lua generated bindings + coroutine bridge
        ↓
S5
Event.await + Physics/Navigation/etc. real domain abilities
        ↓
S6
C++ coroutine ergonomics + shipping static specialization
        ↓
future Python
```

S1.5 不等待 D/E/U2 Editor feature 完成；只依赖 S1 qualification。

---

## 28. STOP conditions

实现若需要以下任一项，必须 STOP：

```text
put Engine/Scene/System capability ontology into modules/
generic ServiceRegistry / SceneServices / ScriptApiManager
global provider singleton
shared_ptr ownership to keep System alive for scripts
provider-specific identity persisted in ScriptArtifact
per-call string lookup
mandatory virtual provider hierarchy
codegen constructs runtime provider
Physics package directly depends on Lua/FlowForge/Python runtime
multiple default providers silently resolved by registration order
borrowed ECS value allowed across await
worker/domain callback directly resumes script
```

---

## 29. Qualification focus

至少证明：

```text
required contract present -> mount success
missing/schema mismatch/ambiguous provider -> distinct fail-early diagnostics
provider object remains owned by its original owner
binding does not extend provider lifetime
sync call path has no continuation allocation
suspend/resume/re-suspend/cancel/generation races are covered
late completion cannot touch stale continuation/provider
```

Ability codegen/receiver/provider-specific qualification继续见 `12`。
