# Architecture Probes：Spatial3D / Spatial2D / Pixel / Robot（v3.1）

> 状态：Normative validation plan。
>
> 施工 Phase：**10**。
>
> 目标：从真实 Product binaries 向下验证 L0–L3，而不是继续凭空扩展 generic API。

---

## 1. Entry condition

只有 Phase 0–9 全通过后开始 probes。

Probe 不得为了赶进度修改 generic lower-layer public surface。

如果遇到无法实现：

```text
record exact architecture gap
stop that probe path
bring gap to Design Barrier B
```

禁止在 Probe 里偷偷新增 generic：

```text
Manager
Context
Registry
Adapter
TimeDomain framework
Streaming framework
```

---

## 2. Promotion rule

一个机制只有满足：

```text
至少 2 个 independent probes
拥有相同 ownership
拥有相同 lifetime semantics
拥有相同 performance direction
```

才有资格在 Barrier B 升格 generic contract。

“名字看起来像”不算重复。

---

## 3. Shared baseline all probes must use

```text
WorldDescription v2 where durable world facts exist
Scene = World + Registry + Simulation
SystemRegistry explicit type registrations
Simulation owns Systems
Process domain-blind
canonical Transform double
Scene final construction on Simulation Lane
no generic WorldObjectId->Entity index
no Scene mandatory streaming state
```

Probe-specific runtime可以不使用 World partition，如果 domain不适合。

---

# PROBE A — Spatial3D Large World Product

## A.1 Goals

证明：

```text
double canonical large-world coordinates
World partition sidecar IO
concrete Spatial3D partition index
concrete StreamingSystem owns policy
Jolt PRIVATE backend
Render relative-float boundary
Asset/Render workflows can remain separate from World partition load
```

## A.2 Allowed concrete types

Probe A 可以按需创建 concrete L3 types，例如：

```text
Spatial3DStreamingSystem
Spatial3DPartitionIndex
Spatial3DStreamingSource     # exact name can match package convention
Spatial3DStreamingConfig
```

这些**不是 generic Scene types**。

如果只有 Probe A 使用，不提升到 `scene/core`。

## A.3 Required flow

```text
Product loads SceneDescription
-> WorldDescription + SimulationDescription
-> build SystemRegistry incl. Spatial3D registration
-> Scene::create on Simulation Lane

Spatial3D System reads runtime components/input
-> queries concrete Spatial3DPartitionIndex
-> decides partition ordinals
-> emits/calls concrete load intent
-> loadWorldPartition(WorldStorageSource,...)
-> completion adopted on Simulation Lane
-> WorldMaterializer selected/all objects
-> System keeps any ownership state it wants
```

Scene core不参与 query/load set计算。

## A.4 Large-world precision proof

测试至少一个远离 origin 的 coordinate：

```text
~Earth-scale meters or significantly large coordinate
```

验证：

```text
Transform3D/WorldTransform3D double
Spatial index query stable
Jolt position path preserves intended precision
Render subtracts view origin in double then casts float
camera motion has no visible world-coordinate jitter at test scale
```

## A.5 Teleport/full-load behavior

模拟 teleport 到新区域：

```text
System immediately issues all desired partition intents
scene/runtime has no arbitrary “20 partitions/frame” throttle
Process/backend controls actual IO concurrency
```

记录：

```text
IO throughput
pending count
memory high-water
decode CPU
materialization CPU
```

不在此 Probe创建 generic IO budget manager。

## A.6 Asset Barrier A evidence

World materializes Mesh/Material/Texture references 后，用现有 concrete asset/render path完成至少一个真实资源 lifecycle。

记录：

```text
who owns demand
when duplicate demand occurs
when release occurs
what CPU payload lifetime is
what Render upload completion looks like
```

Probe成功后进入 Design Barrier A，而不是现场造 generic AssetResidency。

---

# PROBE B — Spatial2D Large World Product

## B.1 Goals

证明 generic World/Scene/Process 不依赖 3D assumptions。

Concrete 2D 可以选择：

```text
grid
quadtree
tile chunks
other index
```

## B.2 Allowed concrete types

例如：

```text
Spatial2DStreamingSystem
Tile2DPartitionIndex
Spatial2DStreamingSource
```

不要复用 Spatial3D query struct 只为了“统一”。

如果 2D 与 3D 最终只共享 `enabled/priority`，记录 evidence，Barrier B 再决定是否有 generic marker。

## B.3 Required flow

与 Probe A共享：

```text
WorldStorageSource
loadWorldPartition
WorldMaterializer
SystemRegistration
Scene core
```

不同：

```text
index/query/policy完全 concrete 2D
```

如果为了 Probe B 被迫修改 `scene/core` 加 Vec2/tile concept，则 architecture failure。

## B.4 Precision

Canonical 2D Transform = double。

Dense tile/image data仍可 float/integer/local。

---

# PROBE C — Noita-like Pixel Product

## C.1 Existing workload reference

Legacy 已有 full-stack visual stress workload：

```text
ECS
fixed-step cellular automata
chunked PixelField runtime
cross-chunk simulation
camera/render presentation
```

新 Probe 应复刻**行为目标**，不复刻 legacy ownership/services architecture。

## C.2 Key architecture question

Pixel simulation chunks **不要求**成为 generic World partitions。

优先模型：

```text
World
    initial PixelField/material durable facts if needed

PixelField runtime/System
    owns live simulation chunks
    owns wake/sleep/update policy

Presentation
    publishes changed chunk/revision state
```

如果 Pixel domain自然有自己的 storage/chunk format，保留 domain-owned。

不要为了使用 generic `loadWorldPartition()` 而强行把 cell chunks包装为 WorldPartitionData。

## C.3 Allowed concrete types

复用/迁移 existing PixelField types优先。

只有真实缺失时新增 concrete Pixel types。

禁止 generic promotion：

```text
ChunkManager
StreamingChunkRegistry
```

## C.4 Timing experiment

直接使用 probe-local fixed-step loop：

```cpp
while (accumulator >= kPixelStep)
{
    pixelStep(kPixelStep);
    accumulator -= kPixelStep;
}
```

Presentation通过 `LatestSpscExchange<PixelPresentationState>` 或一个真实 compact payload独立采样。

不要创建 TimeDomain/TickGroup API。

## C.5 Pass criteria

```text
CA tick rate independent from Render rate
Pixel chunks domain-owned
Scene core unchanged
Latest state can skip intermediate CA states
no Registry triple buffering
no dense cell data promoted to double just because Transform is double
```

---

# PROBE D — Robot Fixed-step Simulation Product

## D.1 Goals

验证最极端时间解耦：

```text
fixed logical step unrelated to wall compute
slow Simulation does not make UI/Render 1fps
Jolt PRIVATE physics
canonical double pose
headless mode
```

## D.2 Mandatory stress case

至少一个 test mode：

```text
logical simulation step = 10 us
artificial/real compute per step ≈ 1 s wall
Presentation target = 60/144fps
```

期望：

```text
Simulation Lane computes N+1 for ~1s
Presentation Lane repeatedly reads state N
camera/UI remain responsive
Render continues independently
when N+1 publishes, Presentation switches
```

## D.3 Execution model

Simulation内部可以完全 serial：

```text
input/controller
-> physics
-> robot dynamics
-> sensor state update
```

只要求：

```text
Simulation Lane != Presentation Lane when slow-step test runs
```

不要求 gameplay/System parallelism。

## D.4 Local time implementation

Probe D 直接：

```cpp
constexpr auto kStep = 10us;
while (running)
{
    runRobotStep(kStep);
    publishPresentationIfNeeded();
}
```

如果需要 lidar 10Hz / camera 30Hz：

```text
先用 probe-local counters/accumulators
```

不要立即创建 multi-TimeDomain framework。

记录哪些调度语义重复。

## D.5 Jolt worker policy

确保 Jolt JobSystem worker配置不会占满所有 CPU导致 Presentation starvation。

记录：

```text
simulation worker count
presentation/render latency
wall compute per logical step
```

Jolt types不得进入 public ECS/Scene API。

## D.6 Headless

Robot Product 必须能：

```text
not create Presentation/Render at all
continue Simulation at same logical semantics
```

这证明 Simulation 不依赖 Presentation。

---

## 7. Cross-lane input experiment

至少 Probe A 或 D 测试：

```text
Presentation/UI input
-> Simulation authoritative effect
```

禁止 Presentation thread直接修改 Registry。

先使用 minimal existing transport。

记录：

```text
latency
ordering
whether reliable queue semantics are needed
whether SPSC is enough
```

Barrier B 再决定 generic ingress primitive。

---

## 8. Shared instrumentation

所有 probe至少记录：

```text
Simulation logical steps/sec
wall compute/step
Presentation fps
Render fps
presentation publish count
consumer acquire count
skipped/intermediate state count estimate
Simulation->Presentation blocking time (must be 0 by design)
heap allocations after warmup
World IO throughput where applicable
materialization CPU time where applicable
Registry entity/component counts
```

Probe-specific：

```text
3D precision error at large coordinate
Pixel changed chunks/update
Robot real-time factor
```

---

## 9. Architecture gap log

每个 Probe 维护表：

| Gap | 当前无法表达的需求 | 涉及层 | 临时 concrete workaround? | 是否需要 Barrier B 讨论 |
|---|---|---|---|---|

临时 workaround只能：

```text
probe-private
not exported
not used by other package
```

不得悄悄变 public adapter。

---

## 10. Evidence matrix required before Barrier B

最终输出：

| Mechanism | 3D | 2D | Pixel | Robot | Same semantics? | Decision |
|---|---:|---:|---:|---:|---|---|
| fixed logical step | | | | | | |
| multiple rates | | | | | | |
| streaming source marker | | | | | | |
| presentation sampling | | | | | | |
| input ingress | | | | | | |
| asset demand lifetime | | | | | | |
| partition entity bookkeeping | | | | | | |

Decision 只能：

```text
promote generic
keep domain-specific
delete
needs more evidence
```

---

## 11. Barrier B output

只有 evidence matrix完成后才允许设计：

```text
TimeDomainId
TickGroup
runtime phase public types
common presentation sampling API
common Simulation ingress API
possible generic streaming-source marker
```

如果四个 Probe 通过现有 primitives 已足够，不要求必须新增这些类型。

“没有新 abstraction”也是成功结果。
