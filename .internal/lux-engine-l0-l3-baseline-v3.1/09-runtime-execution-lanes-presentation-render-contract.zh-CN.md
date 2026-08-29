# Runtime Execution Lanes、Latest State Exchange 与 Render Handoff Contract（v3.1）

> 状态：P0 semantic + one concrete transport primitive。
>
> 施工 Phase：**9**。
>
> 前置：Scene Phase 7 已完成；该 primitive 本身不得依赖 Scene/Simulation concrete type。
>
> 重要：本版**只允许生产实现 `LatestSpscExchange<T>`**。`TimeDomainId`、`TickGroup`、`ScenePhase` 等 exact production types 在 Barrier B 前禁止创建。

---

## 1. Three orthogonal concepts

概念上必须区分：

```text
Time Domain
    = logical time semantics/rate

Tick Group
    = ordering/dependency inside a schedule/domain

Execution Lane
    = CPU execution ownership/thread/resource lane
```

但在 Probe 前，这三个词中只有 `Execution Lane` 是 runtime topology约束；不要把前两个变成 public code type。

---

## 2. Production Type Budget before Barrier B

只允许新增：

```text
LatestSpscExchange<T>
```

禁止新增：

```text
TimeDomainId
TimeDomain
TickGroup
TickGroupId
ScenePhase
RuntimePhase
PresentationManager
PresentationContext
LaneManager
SimulationLane
PresentationLane class wrappers
ClockManager
TimeDomainRegistry
```

“Simulation Lane / Presentation Lane”在本阶段是 Product execution ownership概念，不是必须有同名 class。

---

## 3. Canonical runtime topology

必须允许：

```text
Simulation Lane
    authoritative Registry mutation + Systems

Presentation Lane
    wall-time responsive UI/camera/debug/presentation sampling

Render Thread/Domain
    RenderFrame execution + GPU ownership
```

单线程 Product 可以：

```text
Simulation Lane == Presentation/Main Lane
```

但语义边界仍保留。

Robot/slow simulation Product 必须能真正分 lane。

---

## 4. Logical simulation time != wall time

合法：

```text
logical dt = 10 us
wall compute for one step = 1 s
```

也合法：

```text
logical dt = 1 ms
wall compute = 10 us
```

Simulation不根据 wall compute自动改变 logical dt。

具体 fixed/variable step API由 Probe实现局部逻辑，Barrier B 后再抽象。

---

## 5. Why TickGroup alone cannot solve slow simulation

如果同一线程：

```cpp
simulateOneStep(); // blocks 1s
render();
```

则 1s 内 Render无 CPU执行机会。

TickGroup只描述 ordering，不创造 CPU time。

因此要实现：

```text
slow Simulation + 60/144Hz responsive Presentation
```

必须允许 Simulation 与 Presentation在独立 execution lanes。

这不要求 Simulation Systems彼此并行。

---

## 6. Simulation -> Presentation semantics

这条 seam 交换：

```text
latest stable presentation state
```

不是：

```text
all simulation frames
all simulation ticks
reliable event history
```

Hard semantics：

```text
SPSC
latest wins
intermediate state may be skipped
producer never waits for consumer
consumer never waits for producer
fixed number of persistent slots
no per-publish allocation
```

---

## 7. First implementation placement

Phase 9 固定放：

```text
engine/scene/runtime/presentation
```

第一版不要创建 shared `modules/core/concurrency` target。

理由：先证明 engine runtime真实复用，再决定是否下沉到共享 modules。

`LatestSpscExchange.hpp` 本身不得 include Scene/Simulation/Render/World。

---

## 8. Exact LatestSpscExchange API

冻结：

```cpp
template<class T>
class LatestSpscExchange final
{
public:
    static_assert(std::is_default_constructible_v<T>);

    LatestSpscExchange() = default;
    LatestSpscExchange(const LatestSpscExchange&) = delete;
    LatestSpscExchange& operator=(const LatestSpscExchange&) = delete;
    LatestSpscExchange(LatestSpscExchange&&) = delete;
    LatestSpscExchange& operator=(LatestSpscExchange&&) = delete;

    // Producer thread only.
    [[nodiscard]] T& write() noexcept;
    void publish() noexcept;

    // Consumer thread only.
    [[nodiscard]] bool acquireLatest() noexcept;
    [[nodiscard]] const T& read() const noexcept;
};
```

不加：

```text
wait()
blockingPublish()
queue size
push/pop
callback
scheduler
Scene pointer
```

---

## 9. Triple-buffer ownership model

固定三 slots：

```text
front  = consumer-owned current read
middle = latest published / exchange slot
back   = producer-owned write
```

Producer：

```text
write() modifies only back
publish() atomically swaps back <-> middle and marks new data
```

Consumer：

```text
acquireLatest():
    if no new publication -> false, keep front
    if new publication -> atomically swap front <-> middle, return true
read() returns front
```

中间状态被覆盖是 intentional。

---

## 10. Reference atomic algorithm requirement

实现必须使用单个 packed atomic exchange state 或等价已证明 SPSC triple-buffer algorithm。

固定 reference representation：

```text
std::atomic<std::uint32_t> middle_state
    bits[1:0] = middle slot index
    bit[2]    = NEW_DATA

initial roles:
    front_index = 0
    middle_state = pack(1, CLEAN)
    back_index = 2
```

Producer private：

```text
back_index
```

Consumer private：

```text
front_index
```

Pseudo semantics：

```cpp
// producer publish
old_middle = middle_state.exchange(
    pack(back_index, NEW_DATA),
    std::memory_order_acq_rel);
back_index = index(old_middle);

// consumer acquire
state = middle_state.load(std::memory_order_acquire);
if (!has_new(state))
    return false;
old_middle = middle_state.exchange(
    pack(front_index, CLEAN),
    std::memory_order_acq_rel);
front_index = index(old_middle);
return true;
```

Memory-order contract：

- producer 对 back slot 的普通 writes happens-before producer `exchange(acq_rel)` publication；
- consumer `load(acquire)` / `exchange(acq_rel)` 后读取新 front，必须看见 publication 前全部 writes；
- consumer 在调用下一次 `acquireLatest()` 前必须结束对当前 `read()` payload 的使用，不得跨该调用长期保留引用/pointer；
- consumer 把旧 front 交换回 middle 后，producer 只有通过后续 atomic exchange取得该 slot后才能重写；
- producer 同样不得在 `publish()` 后继续持有旧 `write()` 返回引用。

这样三 slot 始终严格分属 producer/front/middle 三个 role，不需要 mutex。

如果使用不同算法，必须用 TSAN/stress证明相同 ownership，不允许加 mutex/condition_variable作为“先跑起来”的 fallback。

---

## 11. Payload memory rule

禁止：

```text
Registry x3
whole Scene x3
whole World x3
```

`T` = domain-specific compact Presentation representation。

例如 Spatial3D：

```text
renderable transforms
mesh/material logical handles
lights
visual animation state
debug primitives
```

Robot：

```text
robot visual poses
sensor visualization handles/data subset
debug geometry
```

Pixel：

```text
changed chunk/revision presentation state
```

不包含：

```text
AI
script VM
physics broadphase
navigation search state
WorldPartitionData
quest/gameplay internals
```

---

## 12. Simulation faster than Presentation

Example：

```text
Simulation publishes: 100 101 102 103 104 105
Presentation acquires:         103         105
```

101/102/104没有被展示，但 Simulation逻辑已经执行。

Exchange memory仍固定三 slots。

Simulation不得因 Presentation没 acquire而等待。

---

## 13. Simulation slower than Presentation

Example robot：

```text
state 100 published
Simulation computes 101 for 1s wall time
Presentation renders many frames from 100
state 101 publishes
Presentation switches to 101
```

Presentation-local：

```text
camera
UI
mouse
window
debug controls
```

仍可 60/144/unlocked wall-time运行。

---

## 14. Publish frequency is independent

不要求每个 simulation tick调用 `publish()`。

Probe可以：

```text
publish every N simulation steps
publish after each slow robot step
publish when Presentation requests refresh hint
publish when relevant state changed
```

但在 Barrier B 前不要抽象成：

```text
PresentationRateController
SamplingManager
```

---

## 15. State vs reliable Event

Latest exchange只能承载可覆盖 state。

可覆盖：

```text
latest transform
latest light state
latest sensor image reference
```

不能只靠 snapshot 表达：

```text
play sound once
explosion trigger
UI notification
resource destroy/create operation
```

这些必须走 reliable ordered/semantic channel。

Barrier B 前优先复用现有：

```text
LuxObject Event/Signal
Sender completion
probe-local SPSC event packet
```

禁止 generic EventBus。

---

## 16. Presentation -> Simulation ingress invariant

Presentation/Platform得到 input：

```text
keyboard/mouse/UI
```

如果要影响 authoritative ECS：

```text
Presentation thread MUST NOT registry.patch()
```

必须在 Simulation-owned ingress boundary adopt。

Probe可以用最小已有 transport。

在多个 probe重复之前禁止：

```text
SimulationCommandQueue generic API
InputBus
SimulationIngressManager
```

Barrier B 再决定是否有共同 primitive。

---

## 17. Presentation -> Render frame transport

现有 `BoundedSpscFrameRing<T, SlotCount>` 继续用于：

```text
one Presentation producer
one Render consumer
persistent heavyweight RenderFrame slots
small bounded frame-ahead
```

这里 bounded/backpressure 有意义：防止 Presentation比 Render/GPU无限超前。

它**不得反压 Simulation**。

如果 ring满：Product/Presentation可以跳过构造/提交新的 presentation frame并继续 pump UI/event；不得让 Simulation等待 Render frame slot。

---

## 18. Resource/control Render operations

仍然与 frame lane 分离：

```text
resource upload/control
    -> reliable Render Sender / bounded MPSC ingress
```

不能 latest-wins。

不要重新建立 universal bidirectional waiting Game/Render queue。

---

## 19. Render does not read mutable Registry concurrently

当 Simulation/Presentation 真正分 lane：

```text
Render/Presentation never traverses live concurrently-mutated Registry
```

Presentation representation必须在 Simulation stable point extract/copy/publish。

Exact phase naming留 Barrier B；此 invariant现在已经冻结。

---

## 20. CPU resource starvation

Non-blocking exchange解决 synchronization，不解决 CPU starvation。

Product execution policy必须允许：

```text
Simulation worker/resource bounded
Presentation latency-sensitive lane gets CPU
Render thread/domain gets CPU
```

Jolt作为 PRIVATE backend时，其 JobSystem worker count由 Product/runtime configuration控制，不得默认吞掉所有 hardware concurrency 导致 Presentation starvation。

本 Phase不创建 `ExecutionLaneManager`。

---

## 21. Tests for LatestSpscExchange

必须：

```text
single producer / single consumer contract diagnostics where practical
no allocation after construction
producer never blocks
consumer never blocks
consumer values always from fully-published slot
latest eventually observed
intermediate values allowed to skip
TSAN clean
100M+ exchange stress
large reusable vector payload allocation reuse test
producer much faster than consumer
consumer much faster than producer
```

不要把“每一个 publish 都必须被 consumer观察”写成 test；这与 latest-wins 语义冲突。

---

## 22. Probe-only timing code

Barrier B 前时间直接在 concrete Product/probe 表达。

Robot：

```cpp
constexpr auto kRobotStep = 10us;
runRobotStep(kRobotStep);
```

Pixel：

```cpp
while (accumulator >= kPixelStep)
{
    runPixelStep(kPixelStep);
    accumulator -= kPixelStep;
}
```

不要提前将这些包装成 `TimeDomain/TickGroup`。

---

## 23. Barrier B questions

只有四个 probe有数据后再回答：

```text
是否真的需要 TimeDomainId？
TickGroup 是 System description durable data 还是 Product schedule data？
fixed-step accumulator归 Simulation还是 Product？
Presentation sampling是否有两个以上 domain共享？
Presentation->Simulation ingress是否值得一个 generic SPSC primitive？
```

如果答案不一致，保留 domain-specific，不强行统一。
