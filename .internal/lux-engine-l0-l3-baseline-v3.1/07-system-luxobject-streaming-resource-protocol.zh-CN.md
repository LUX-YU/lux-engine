# System / LuxObject / Streaming Intent / Resource Protocol（v3.1）

> 状态：Normative protocol constraints + Asset demand DESIGN HOLD。
>
> 适用：Phase 7–10 的 concrete System/runtime wiring。本文不定义 generic StreamingSystem framework，只限制 concrete System 与 runtime mechanism 的交互方式。

---

## 1. Core rule

```text
Gameplay/runtime policy belongs to concrete System.
Mechanical IO/materialization belongs to scene/runtime/world.
```

Concrete System 可以决定：

```text
what should load
when should unload
which index/query to use
priority/hysteresis/prediction
whether to track partition/entity ownership
whether loaded data persists
```

Generic Scene/runtime 不决定这些。

---

## 2. No generic WorldStreamingSourceComponent in v3.1 construction waves

上一版的：

```cpp
struct WorldStreamingSourceComponent
{
    bool enabled;
    int32_t priority;
};
```

**不在 Phase 0–9 创建。**

原因：generic promotion 尚无两个 independent domain 的证据。

Probe A 可创建 concrete：

```text
Spatial3DStreamingSource
```

Probe B 可创建：

```text
Spatial2D/TileStreamingSource
```

只有 probe evidence 证明它们真的共享稳定 semantics，Barrier B 后再考虑提升。

禁止提前创建：

```text
WorldStreamingSourceComponent
StreamingSourceBase
StreamingSourceRegistry
```

---

## 3. Concrete StreamingSystem ownership

例如 future Probe A：

```text
scene/spatial3d/Spatial3DStreamingSystem
```

可以拥有：

```text
Spatial3D partition index instance
streaming source query state
hysteresis
priority
pending partition loads
created Entity handles if that product wants them
```

Probe B 可完全不同。

Scene core不 include concrete System。

Concrete L3 System通过 `SystemRegistration` 加入 Product `SystemRegistry`。

---

## 4. No mandatory load/keep contract

Generic runtime 不定义：

```text
WorldStreamingSelection
load set
keep set
retire set
```

这些可以是 Spatial3D System内部算法，但不是 cross-domain contract。

Room/portal、pixel、robot等 workload 可以完全不同。

---

## 5. Typed LuxObject Signal is composition option, not mandatory framework

如果 concrete System需要从 Simulation Lane 发出一个 typed intent：

```cpp
struct Spatial3DPartitionLoadIntent final
{
    world::WorldPartitionOrdinal partition;
};
```

可以使用该 concrete System自己的 typed Signal：

```text
System -- DIRECT typed Signal --> product/runtime handler
```

DIRECT 用于同 lane synchronous intent adoption时，handler 可以：

```text
copy small intent values
start asynchronous Sender workflow
record probe-local pending state
```

不得：

```text
在 System update stack 中立即做危险 structural Registry mutation
阻塞等 IO
进入 Render backend
```

不要创建 generic：

```text
WorldStreamingBinding
StreamingObserver
StreamingIntentBus
SceneRuntimeManager
```

直接用 `observeScoped`/free function/lambda wiring 即可。

---

## 6. Async completion target

如果 requester 是 LuxObject System：

```text
capture ObjectWeakRef
async completion
-> postEvent(weak requester, typed event)
```

Event payload 可以持：

```text
WorldPartitionData shared/owning value
failure information
Asset ready handle/value where applicable
```

Requester 已销毁：

```text
weak post safely discards
```

不要为 completion 创建：

```text
CompletionRegistry
CallbackRegistry
RequestOwnerManager
```

---

## 7. LuxObject construction affinity

LuxObject 在构造线程建立 affinity。

因此 concrete LuxObject Systems 必须在 intended Simulation Lane construction path 中建立。

这由：

```text
Product -> Scene::create on Simulation Lane -> Simulation::create -> System install thunk
```

保证。

禁止：

```text
Main 构造后把 System object 移给 Simulation thread
```

System 本身可以 non-movable，符合当前对象模型。

---

## 8. Registry lane rule

Authoritative Registry 只允许 Simulation Lane mutation。

Cross-lane completion/input：

```text
Process completion
Presentation input
Render feedback
sensor/network input
```

不得直接从其 producer lane 修改 Registry。

它们必须：

```text
post/adopt into Simulation Lane
then mutate at a safe point
```

Barrier B 前不创建 generic SimulationIngressManager。

Probe 可使用：

```text
LuxObject queued Event
existing main/simulation scheduler continuation
probe-local SPSC command packet
```

选择最小已存在机制。

---

## 9. DIRECT Signal threading contract

v1 DIRECT signal仅用于：

```text
same-lane synchronous delivery
```

Cross-affinity 使用现有 queued semantics/Event。

不要用 DIRECT 绕过 thread affinity。

对于 streaming intent：

```text
System and runtime handler should normally live/adopt on Simulation Lane
```

handler启动 async workflow后立即返回。

---

## 10. Object message queue policy

当前 Object queued Event 继续保持 logically unbounded。

本 baseline 不引入：

```text
QUEUE_FULL
completion drop
per-frame ObjectEvent adoption budget
```

如果未来 benchmark 证明 mutex/deque是 bottleneck，可以替换内部实现为 dynamic MPSC/lock-free queue；public Signal/Event API不变。

不要在 L3 为此创建第二个 event transport abstraction。

---

## 11. Streaming request state belongs to concrete System/product

如果 System需要 pending dedup：

```cpp
std::unordered_set<WorldPartitionOrdinal> pending;
```

或其他 concrete storage即可。

不要因为多个 concrete Systems可能都需要 pending state，提前创建：

```text
PartitionRequestRegistry
StreamingRequestManager
SceneStreamingState
```

只有 probes 证明 exact same semantics重复后才提升。

---

## 12. Entity ownership after materialization

Generic materializer不记录 ownership。

Concrete System可选择：

```cpp
struct MyLoadedPartition
{
    WorldPartitionOrdinal partition;
    std::vector<Entity> entities;
};
```

这是 concrete/private state，不升级为 Scene API。

另一个 System也可以完全不按 partition保存 entities。

Gameplay对象离开出生 partition后是否仍存在，由 gameplay System决定。

---

## 13. No implicit persistence

Streaming unload不意味着：

```text
automatic save
automatic overlay capture
automatic restore
```

Persistence属于开发者/未来 Save subsystem。

Concrete System若要“门打开后卸载再加载仍打开”，可以通过其自己的 gameplay state实现。

不要在 streaming protocol 中创建 generic overlay。

---

## 14. Asset/resource demand — DESIGN HOLD

本节只冻结：

```text
System should not directly own GPU residency policy
Render owns GPU lifetime
```

但在 Design Barrier A 前禁止实施 generic：

```text
ResourceDemand
AssetDemandKey
DemandTracker
ResidencyBridge
```

Probe A 先使用 concrete load flow。

如果 concrete System想加载 Mesh/Material/Texture，不要把 Asset load递归塞进 World partition load；正确方向仍是：

```text
World materializes reference component
-> concrete render/presentation/resource functionality observes it
-> asset workflow separately starts
```

但 exact demand lifetime API暂缓。

详见 `08`。

---

## 15. SystemRegistration and concrete package

Concrete System package需要：

```cpp
std::span<const simulation::SystemRegistration>
spatial3dSystemRegistrations() noexcept;
```

Product显式 add。

不要依赖 ReflectionRegistry 在 runtime 自动找到它。

Codegen可以生成静态表内容。

---

## 16. Forbidden vocabulary in new generic packages

```text
StreamingManager
StreamingContext
WorldStreamingBinding
WorldStreamingSelection
SceneResidency
PartitionOwnershipService
SystemServices
ResourceDemandRegistry
```

Concrete probe-private struct 可以有直白业务名字，不必为了绕 vocabulary 改成另一个 framework 名称。

---

## 17. Tests

### LuxObject/System

```text
System constructed on Simulation Lane
DIRECT same-affinity intent
queued wrong-affinity completion
weak requester destruction
ScopedConnection disconnect
```

### Streaming policy ownership

```text
Scene core has no streaming state
concrete System can choose partition and request load
concrete System can choose not to materialize
concrete System can retain/destroy created entities itself
no generic WorldStreamingSelection
no generic WorldStreamingSourceComponent before probes
```

### Threading

```text
worker completion cannot mutate Registry directly
completion adopted on Simulation Lane before Registry change
```

---

## 18. Completion condition

本 protocol实现完成后，代码结构应呈现：

```text
Concrete System
    policy
    small concrete state
    typed intent/event if needed
        |
        v
free-function/product wiring
        |
        v
scene/runtime/world mechanical Sender
```

而不是：

```text
System -> Binding -> Manager -> Context -> Registry -> Adapter -> runtime
```
