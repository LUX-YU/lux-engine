# Script API Capability Contracts、Coroutine / Await 与 System Integration

Status: **Normative Runtime Scripting Design (v1, v3 docset)**  
Date: **2026-09-02**  
Parent documents: `00-L5-architecture-overview.md`, `07-implementation-roadmap-and-gates.md`, `08-normative-execution-contract.md`, `09-product-runtime-vfs-and-async-script.md`

> Normative scripting priority: 对 Script API capability、coroutine/await、Event.await 和 Delay 时间语义，本文件 supersede `06/07/08/09` 中任何冲突的旧 scripting 条款。`08` 在未被本文件明确 supersede 的 general execution/ownership 规则上仍保持最高优先级。

---

## 1. 统一 ontology

Lux runtime scripting 冻结为：

```text
Component
    = state/data contract

System / integration provider
    = behavior capability implementation

Script API
    = callable capability contract

HookPoint / EventPoint
    = engine-to-script execution/event contract

Coroutine / Awaitable
    = time-spanning script control-flow contract
```

Script API 描述语义，不规定底层一定使用 function pointer、virtual interface 或某一种 ABI dispatch。

当前已有 `ScriptSymbolId`、`ScriptArtifact` export index、`ScriptSystem` mount/binding、HookPoint/EventPoint、prepared `BoundScriptCall`、Lua、C++ static/native module、FlowForge -> ScriptArtifact、ExecutionRuntime/Timer/TaskScope、AssetReadPort/loadAsset<T>() 等基础；这些方向必须保留。

---

## 2. Script API contract 与 provider 分离

脚本依赖 contract，不依赖具体 backend/provider identity。

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

Provider 不要求一定是 SimulationSystem。`AssetLoading` 可以由 Scene/Application integration 提供；owner 必须遵循真实物理/语义生命周期。

---

## 3. Scene Script Capability Set

Scene/Simulation composition 完成后，可发布一个只用于 script composition/binding 的 immutable/frozen capability view。

```text
Scene composition
├─ Physics provider
├─ Navigation provider
├─ Audio provider
├─ Asset integration
└─ ...
        ↓
SceneScriptCapabilitySet
```

其语义：

```text
build/publish during composition
resolve during script mount/bind
frozen during normal runtime v1
not a general service locator
not queried by arbitrary domain code every frame
```

禁止演化为：

```cpp
scene.services().get<T>();
scene.capabilities().resolve("Physics");
```

hot path 在 mount/bind 后必须使用 prepared binding，不重复 lookup contract/provider。

---

## 4. Stable contract identity

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

Script requirement 与 provider publication 必须 exact-compatible；不一致返回 `SCRIPT_CAPABILITY_SCHEMA_MISMATCH`。第一版不建立 semantic-version negotiation framework。

---

## 5. Script API method kinds

第一版 method semantic kind：

```text
QUERY
    guaranteed synchronous result at the legal semantic point

COMMAND
    submit/apply action; caller does not await result

ASYNC_OPERATION
    returns ScriptAwaitable<T>; may complete later
```

Event 继续由 EventPoint 表达，不塞进 generic method table。

Asyncness belongs to the contract。不得依靠函数名 `Async` 猜测语义。

例如：

```text
PhysicsQuery3D.raycast
    kind = QUERY
    input = RaycastRequest
    output = Optional<RaycastHit>

PhysicsQuery3D.raycastAsync
    kind = ASYNC_OPERATION
    input = RaycastRequest
    output = Optional<RaycastHit>
```

---

## 6. ScriptArtifact requirements 与 fail-early mount

ScriptArtifact 除 exports 外必须能够声明/携带 Script API requirements。

```text
Player.lua
Exports:
    OnUpdate(float)
    OnHit(CollisionEvent)

Requires:
    PhysicsQuery3D / schema X
    AssetLoading / schema Y
```

来源：

```text
FlowForge
    API node -> compile-time requirement

C++
    generated/registration metadata

Lua
    package/binding metadata

future Python
    generated/package metadata
```

不得在 runtime 通过任意字符串调用推断 requirement。

Script mount/bind MUST 在执行前验证：

```text
asset/export exists
Hook/Event target exists
signature/scope compatible
all required Script API contracts exist
contract schema compatible
```

错误必须区分：

```text
SCRIPT_ENDPOINT_NOT_FOUND
    engine -> script Hook/Event target missing

SCRIPT_CAPABILITY_NOT_FOUND
    script -> engine API contract missing

SCRIPT_CAPABILITY_SCHEMA_MISMATCH
    contract exists but incompatible
```

Scene 没有 Physics capability 时，使用 PhysicsQuery3D 的 script 必须在 mount/bind 阶段 fail early，而不是执行 `raycast()` 时得到 null service。

---

## 7. Script API contract 不规定 dispatch 机制

动态边界允许 prepared function table，例如：

```cpp
struct BoundPhysicsQuery3D
{
    void* context{};
    RaycastResult (*raycast)(void*, const RaycastRequest&) noexcept {};
};
```

这是 dynamic binding strategy，不是 contract 本身。

MUST：

```text
resolve once at mount/bind
immutable prepared binding
no per-call string lookup
no dynamic_cast/service lookup hot path
```

MUST NOT 把 `std::function` 或 mandatory virtual inheritance 作为 canonical Script API binding。

函数指针可能阻止部分 inlining，但在 Lua/Python、physics query、asset IO、async boundary 等情况下通常不是主要成本。

对于 project-specific shipping target，允许同一 semantic contract 使用 generated/static specialization：

```text
Project selects PhysicsQuery3D -> JoltPhysicsSystem
        ↓
generated C++ / FlowForge lowering
        ↓
direct typed call / direct IR callee
        ↓
Link Time Optimization / whole-program optimization
```

因此：

> Contract 与 dispatch strategy 分离；动态边界可间接调用，已知 shipping hot path 可静态特化/内联。

真正高频 data-parallel API 应优先考虑 batch contract，而不是只优化一次 indirect-call 的纳秒级成本。

---

## 8. Coroutine 是用户模型；continuation 是 engine 模型

用户层跨帧控制流采用 coroutine/await：

```text
Lua        coroutine/yield-resume
C++        co_await
FlowForge  visible sequential graph + suspension node
Python     future await
```

引擎核心采用 backend-neutral continuation，不将任何语言 coroutine representation 作为通用 ABI。

必须支持生命周期：

```text
RUNNING
   ↓ await
SUSPENDED
   ↓ resume
RUNNING
   ↓
COMPLETED / FAILED
```

概念执行结果：

```cpp
enum class EScriptStepState
{
    COMPLETED,
    SUSPENDED,
    FAILED,
};

struct ScriptStepResult
{
    EScriptStepState state;
    ScriptAwaitableId waiting_on;
    ScriptError error;
};
```

不得把 `SUSPENDED` 偷塞进旧 success/error integer 的某个 magic value 而不更新明确 contract。

---

## 9. Stable generational identity

至少需要稳定 generational identity：

```text
ScriptInstanceId
ScriptContinuationId
ScriptAwaitableId
```

分别回答：

```text
哪个 script instance？
哪一次尚未完成的执行？
当前等待什么？
```

跨线程/跨帧 completion 不传：

```text
ScriptInstance*
coroutine_handle*
lua_State*
PhysicsQuery*
raw continuation pointer
```

completion 只携带稳定 ID + owned result/error；Simulation resume 时重新 resolve 并校验 generation。

---

## 10. Backend-neutral continuation

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
FlowForge
    compiler-generated explicit state machine + locals

Lua
    Lua coroutine/thread reference

C++
    std::coroutine_handle<> inside C++ backend only

future Python
    Python coroutine/task reference
```

`ScriptSystem` MUST NOT know `std::coroutine_handle`、`lua_State`、Python object 或 FlowForge program-counter layout。

---

## 11. Awaitable model

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

storage 必须 bounded + generational。

需要正确处理 eager completion：operation 可能在 coroutine 完成 suspension registration 之前 ready；READY state 必须被保存，不能丢 completion。

---

## 12. Stable resume point

任何 external completion/event 都只能：

```text
mark Awaitable READY
        ↓
enqueue ScriptResume
```

MUST NOT 立即 resume script。

明确禁止：

```text
worker -> coroutine.resume()
timer thread -> lua_resume()
physics callback -> FlowForge continue()
GPU callback -> C++ coroutine_handle.resume()
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

如果一个 resumed coroutine 立即等待已经 READY 的 awaitable，也不得递归 `resume -> resume -> resume`；使用 tail enqueue，避免 stack growth、reentrancy 和单 coroutine 独占 frame。

---

## 13. Event + await

Hook/Event callback 模式继续存在。

同时允许：

```text
await Event.next(...)
```

二者语义不同但共享现有 EventPoint source：

```text
EventPoint
    ↓
ScriptSystem EventBucket
    ├─ normal bound handlers
    └─ coroutine waiters
```

MUST NOT 为每一个 `Event.next()` waiter 临时 `EventPoint.connect()`/disconnect。

原因包括：topology churn、dispatch-active mutation 限制、额外 allocation 以及 reentrancy。

Event payload 若跨 dispatch 生命周期被 coroutine 消费，必须 marshal/copy 到 owned resume storage；不得保存当前 call-frame/payload pointer。

---

## 14. Delay 时间语义

第一版冻结：

```text
Delay.seconds(x)
    == Delay.simulationSeconds(x)

Delay.realSeconds(x)
    = monotonic real time

Delay.nextStep()
    = next eligible Simulation step
```

### 14.1 Simulation-time Delay

Gameplay 默认必须跟随 Simulation clock / time scale / pause。

推荐：

```text
SimulationClock.now + duration -> deadline
ordered deadline queue / suitable timer structure
first stable point with now >= deadline -> READY
```

MUST NOT：

```text
one Process TimerSender per simulation-time Delay
scan/decrement every suspended Delay every frame
resume before deadline
```

内部 clock/deadline 使用高精度 integer/fixed/chrono-like duration；不要用长期累计 float deadline。

实际脚本恢复精度由 stable-point 粒度约束：60 Hz 时正常量化延迟小于约 16.67 ms（不含 hitch 和 resume-budget latency）。原则是 **never early; first eligible stable point at/after deadline**。

### 14.2 Real-time Delay

`Delay.realSeconds()` 可以桥接 monotonic `TimerClient/TimerSender` 或等价真实时钟能力，但 Timer ready 仍只 enqueue resume；脚本继续执行仍发生在 Simulation stable point。

---

## 15. Actual async operation ownership

`ScriptSystem` owns：

```text
script instance state
continuation storage
awaitable storage
resume queue
Event await waiter semantics
Simulation-time delay semantics
```

实际 time-spanning work 由真实 domain/process owner 提供：

```text
real timer      -> Process execution integration
AssetLoad       -> AssetReadPort/loadAsset<T>()
Physics query   -> Physics/Scene integration
GPU query       -> Render/Scene integration
Navigation      -> corresponding domain/integration
```

因此 L1 Simulation 不应仅因为支持 await 就直接拥有 `stdexec` worker pool、Process TaskScope、VFS blocking IO 或 GPU fence。

Scene/Application integration bridge 把 domain completion 转成稳定 Script awaitable completion record。

---

## 16. Cancellation / shutdown

Scene stop、script unmount/reload、entity/script-instance invalidation 必须：

```text
invalidate ScriptInstance generation first
mark/cancel pending Awaitables
request stop on external operations where supported
destroy backend continuation at safe point
```

迟到 completion resolve 到旧 generation：

```text
discard
```

绝不能 resume stale backend state。

---

## 17. Fan-out policy

v1 recurring Hook invocation 为 **single-flight**：

```text
previous Hook invocation is SUSPENDED
        ↓
next recurring Hook trigger does not start another copy
```

避免 `OnUpdate() { await Delay(10s); }` 在 60 Hz 下不断创建 continuation。

Event invocation 可以 multi-flight，因为每个事件是独立事实，但必须有：

```text
per-script-instance continuation capacity
global/Scene continuation capacity
bounded resume queue
per-step resume budget
```

超限必须 fail closed/diagnostic，不能无限增长。

---

## 18. FlowForge projection

FlowForge 是第一验证 backend。

Source graph API node 保存：

```text
ScriptApiContractId
ScriptApiMethodId
```

不得保存 `JoltPhysicsSystem::raycast` 等 provider-specific identity。

ASYNC_OPERATION node 在 lowering 中形成显式 state-machine suspension：

```cpp
switch (pc)
{
case 0:
    pc = 1;
    return suspend(awaitable);
case 1:
    result = resumeValue<T>();
    ...
}
```

locals 存在 generated continuation frame；不保留 native stack across frames。

Project-specific shipping compile 可在已知 provider 时把同一 ContractId/MethodId 静态 lower 到 concrete typed callee/IR call，实现 direct call / inlining / Link Time Optimization。

---

## 19. Lua / C++ / future Python

### Lua

Lua backend 使用 Lua coroutine/yield-resume，但只作为 private representation：

```text
await -> yield ScriptAwaitableId
Simulation stable resume -> push owned result -> lua_resume
```

### C++

C++ script 可提供 ergonomic `ScriptTask<T>` / `co_await`，但 `std::coroutine_handle<>` 只存在于 C++ backend。

建议：

```text
engine-controlled initial/final suspend
script-aware bounded/slab allocator for coroutine frame
promise await_transform only accepts Lux ScriptAwaitable<T>
no unmanaged std::suspend_always inside script coroutine
```

static C++ project path 可使用 templated/generated capability adapter，允许 concrete provider direct call/inlining。

### Python

future only；不得提前让 S1-S6 依赖 Python runtime。

---

## 20. Script API codegen/projection

同一个 Script API contract 应投影到：

```text
FlowForge node/palette metadata
C++ typed facade
Lua binding/module
future Python binding
ScriptArtifact requirement metadata
Editor diagnostics
```

语言表现可以不同，但 identity/method semantics/asyncness/result schema 必须来自同一 contract source。

FlowForge node palette MAY 根据当前 Scene capabilities 把 unavailable API 显示为 disabled + reason；不要 silent hide 已经被 asset 使用的 dependency。

---

## 21. Script authoring UX

Editor 应区分：

```text
Requirements
    script wants what capabilities?

Bindings
    who invokes exported script symbols?
```

例如：

```text
Player.lua
Requirements
✓ PhysicsQuery3D
✓ AssetLoading
✗ NavigationQuery

Exports
OnUpdate(float)
    Bind -> Gameplay / AfterUpdate
OnCollision(CollisionEvent)
    Bind -> Physics / Collision
```

Export identity 使用稳定 `ScriptSymbolId`；name 用于显示/诊断。

MUST NOT 通过函数名 heuristic 自动绑定 execution endpoint。

---

## 22. Provider lifecycle

v1 capability topology 在正常 Scene runtime 中 frozen。

不得在 script prepared binding 仍存活时热替换 provider。

如果 composition 发生变化：

```text
stop/invalidate scripts
cancel continuations
recompose capability set
revalidate requirements
rebind prepared calls
restart
```

动态 provider hot swap 不是 v1 目标。

---

## 23. Performance policy

### Dynamic boundary

允许 `context + function pointer`：

```text
Lua/Python bindings
plugin/dynamic artifact
runtime capability binding
coroutine backend resume/destroy
```

### Static hot path

项目专用 shipping target 可使用：

```text
generated adapters
templates/direct typed calls
FlowForge direct IR callee
Link Time Optimization / whole-program optimization
```

Contract 不规定 dispatch，因此二者共享同一 semantic source。

同步 Script hot path 不应因为 coroutine support 而产生 continuation allocation；只有真正 suspend 才分配/占用 continuation/awaitable storage。

对高频大量 query，优先设计 batch API/数据局部性/并行执行，而不是为了几纳秒 indirect call 破坏 capability contract。

---

## 24. Implementation waves

### S0 — Contract freeze

**DONE by this document.** Coding agent 不得重新发明 continuation/capability/time semantics。

### S1 — Core continuation + capability foundation

实现：

```text
ScriptApiContractId / ScriptApiMethodId
minimal requirement/provider publication and mount validation
ScriptInstanceId / ScriptContinuationId / ScriptAwaitableId
bounded generational storage
ScriptStepResult COMPLETED/SUSPENDED/FAILED
backend-neutral continuation storage
bounded resume queue + stable resume point
cancel/generation invalidation
```

保持现有同步 `BoundScriptCall` hot path，不为同步调用强制 allocation。

### S2 — First awaitables

```text
Delay.nextStep()
Delay.seconds()/simulationSeconds()
Delay.realSeconds()
AssetLoad
```

### S3 — FlowForge explicit state-machine lowering

用 FlowForge 验证 S1/S2 engine contract。

### S4 — Lua coroutine bridge

### S5 — Event.await + first real domain async Script API

Physics only after a real Physics Script API contract/provider is frozen/implemented；不要为了测试 coroutine 发明假 universal Physics service。

### S6 — C++ coroutine ergonomics + static specialization

### Future — Python

S1 may begin after the current visible U/C Editor checkpoint is qualified. It **does not wait for D/E/U2**；runtime scripting 与 Editor D/E 可并行。

---

## 25. Required tests

### Capability contract

```text
required capability found -> bind
missing -> SCRIPT_CAPABILITY_NOT_FOUND
schema mismatch -> SCRIPT_CAPABILITY_SCHEMA_MISMATCH
endpoint missing remains SCRIPT_ENDPOINT_NOT_FOUND
prepared hot call performs no per-call string lookup
provider-specific backend identity absent from ScriptArtifact/FlowGraph generic source
```

### Continuation/Awaitable

```text
sync invocation completes without continuation allocation
suspend -> ready -> resume -> complete
suspend -> resume -> suspend again
ready-before-waiter-registration race
cancel vs completion race
instance generation invalidation discards late completion
bounded continuation/awaitable/resume queue exhaustion
resume budget
no recursive resume chain
resume only at Simulation stable point
```

### Delay

```text
nextStep never resumes same pass
simulation Delay follows Simulation clock/pause/time scale
simulation Delay never resumes early
many delays do not require one OS/Process timer each
real Delay can become ready during pause but script resumes only when eligible stable point is processed
```

### Event.await

```text
normal handlers unchanged
Event.next waiter resumes once
entity-target filtering
owned payload survives dispatch lifetime
no per-waiter EventPoint topology connection
bounded multi-flight behavior
```

### Language/backend later

```text
FlowForge state machine preserves locals/result/error
Lua yield/resume maps to same AwaitableId
C++ coroutine handle never leaks into ScriptSystem/public ABI
```

---

## 26. MUST NOT

```text
No CoroutineManager singleton.
No AsyncManager/EventAwaitManager parallel lifecycle universe.
No generic ScriptApiManager/service locator hot path.
No string-key capability lookup per call.
No std::function as canonical binding mechanism.
No mandatory virtual provider base class solely for Script API.
No provider-specific identity in generic FlowGraph/Script API source.
No one EventPoint connection per Event.await waiter.
No raw coroutine/backend pointer in cross-thread completion.
No worker/timer/physics/render direct script resume.
No native C++ stack retained across frames.
No Process TimerSender per simulation-time Delay.
No per-frame full scan/decrement of all suspended delays.
No unbounded recurring Hook coroutine fan-out.
No unbounded Event continuation fan-out.
No provider hot swap underneath prepared script bindings.
No Python dependency before an approved Python wave.
```

---

## 27. STOP conditions

STOP for architecture review if implementation appears to require：

```text
general Scene service registry / EngineContext
universal capability semantics that are not truly shared by providers
new language-specific async lifecycle outside the common continuation contract
changing Simulation affinity/order merely to resume scripts
changing Product target model for S1-S6
inventing project manifest/target-generation format
provider hot swap framework
compatibility shim keeping old and new script async architectures in parallel
```

---

## 28. Ready definition

Script coroutine/capability foundation is ready when：

```text
Script dependencies are stable capability contracts, not backend identities
mount fails early on missing/incompatible contracts
sync prepared-call hot path remains lean
continuation/awaitable identities are bounded + generational
all resume occurs only at explicit Simulation stable point
Event callback + Event.await coexist
simulation-time and real-time Delay semantics are distinct
FlowForge can lower suspension without native stack retention
Lua/C++ can project to the same engine continuation contract
shipping C++/FlowForge paths retain static specialization/inlining opportunity
no service locator/coroutine manager/second async framework was introduced
```
