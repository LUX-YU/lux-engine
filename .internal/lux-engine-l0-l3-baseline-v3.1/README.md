# Lux Engine L0–L3 Architecture Baseline v3.1（Repository Canonical + Errata）

这是一套**给实施 LLM / 人类工程师直接施工**的规范包。

v3.1 相比 v3 的目标不是继续扩展架构，而是减少实施自由度：

```text
精确施工顺序
Public Type Budget
No Adapter Rule
Design Barrier
明确禁止提前实现的 generic types
```

目标基线：`LUX-YU/lux-engine@230374a5f0d53e52bbb5d3bdce33cac62da06660`。

仓库 canonical copy 包含两项已批准的勘误：Phase 2 补入原文已引用但漏列的
`WorldChunkReference`/`WorldPartitionTable`；Phase 8 删除 `WorldStorageSource::Impl`，
改为持有 `WorldDescription shared_ptr + OperationPort` 的静态 `create()` value capability。

---

## 1. 施工顺序

**严格按编号阅读和施工。** 高层不得先行创建 stub/adapter 让自己编译。

| 顺序 | 文档 | 作用 |
|---:|---|---|
| 00 | `00-l0-l3-master-implementation-plan.zh-CN.md` | 总施工 DAG、type budget、barriers、跨层不变量 |
| 01 | `01-l0-taskgraph-dependency-list-prerequisite.zh-CN.md` | L0 唯一前置：`TaskDependencies` |
| 02 | `02-world-description-v2-storage-implementation-spec.zh-CN.md` | World metadata-only + bundle/sidecar storage |
| 03 | `03-ecs-double-precision-component-decode-prerequisite.zh-CN.md` | canonical double + generated component decode/emplace |
| 04 | `04-simulation-system-registry-runtime-implementation-spec.zh-CN.md` | System type catalog + SimulationBuilder + Simulation |
| 05 | `05-scene-core-description-runtime-composition-spec.zh-CN.md` | SceneDescription + minimal Scene |
| 06 | `06-scene-runtime-world-process-materialization-spec.zh-CN.md` | World partition IO + `WorldMaterializer` |
| 07 | `07-system-luxobject-streaming-resource-protocol.zh-CN.md` | concrete System/LuxObject/streaming protocol constraints |
| 08 | `08-engine-asset-residency-design-hold.zh-CN.md` | Asset residency ownership已冻结；generic demand wiring 暂停 |
| 09 | `09-runtime-execution-lanes-presentation-render-contract.zh-CN.md` | Simulation/Presentation/Render lanes + `LatestSpscExchange<T>` |
| 10 | `10-topology-cmake-architecture-gates.zh-CN.md` | target topology / CMake / negative gates |
| 11 | `11-architecture-probes-3d-2d-pixel-robot.zh-CN.md` | 3D/2D/Pixel/Robot vertical slices |
| 12 | `12-p1-backlog.zh-CN.md` | probes 后再处理的 P1 |

注意：文档编号是依赖顺序；实际 Phase 6 Process verification 是 tests-only，因此没有单独 Process 重构文档。

---

## 2. Canonical architecture

```text
L6  Product / Host
L5  Toolchain / Editor
L4  Authoring
L3  Scene
L2  Process
L1  Simulation  ─────→  World
L0  Platform / Core / Resource / Function
```

```text
World       = durable/cooked facts + whole-world storage description
Simulation  = concrete Systems + synchronous rules + compiled schedule
Process     = domain-blind async substrate
Scene       = one World + one authoritative Registry + one Simulation
Presentation= runtime concern, not a new architecture layer
```

---

## 3. 最重要的 anti-LLM rules

### 3.1 No Adapter Rule

两个接口接不上：

```text
先检查 prerequisite Phase 是否缺能力
```

不是：

```text
Adapter / Bridge / Context / Manager / Services / Registry
```

### 3.2 Public Type Budget

每份规范明确列出允许新增的 public production type。

未列出的 public type：

```text
STOP subtask
record architecture gap
return to design review
```

private `Impl`、private helper、test fixture、paired error/failure 不计入 type budget。

### 3.3 Probe-before-abstraction

Barrier B 前禁止 production：

```text
TimeDomainId
TickGroup
ScenePhase
ClockManager
TimeDomainRegistry
PresentationManager
WorldStreamingSourceComponent
StreamingSourceBase
```

3D / 2D / Pixel / Robot 至少两个独立 domain 出现真正相同语义，才允许提升 generic abstraction。

---

## 4. Frozen P0 decisions

### World

- `WorldDescription` metadata-only；不保存 `WorldObject[]`。
- root 是 Asset；sidecars 不是 Asset。
- root/volume 共享 `WorldBundleId + WorldBundleGeneration`；sidecar 另有 `VolumeOrdinal`。
- generic minimum IO unit = partition。
- object-level materialization不产生额外 IO。
- `WorldPartitionLayoutBuilder` 改为直接接 `span<const WorldObjectId>`；不创建 `WorldPartitionWorkspace`。

### ECS / precision

- user-facing `Transform2D/3D`、`WorldTransform2D/3D` 全 double。
- canonical spatial index/query/world placement double。
- Render/Jolt/Nav/sensor dense local data可用 float/native。
- double→float 必须先在 double 中 subtract local origin，再显式 narrow。
- 不做 Scene-wide floating-origin mutation。
- `ComponentSchema` 增 generated decode/emplace thunk；L3 不再发明 materialization binding registry。

### Simulation

- `SystemRegistry` = System **type** catalog + install thunk，不是 live instance owner。
- concrete System instances由 `Simulation` private ownership。
- `SimulationBuilder` 不成为 Services/Context/DI。
- v1 每个 System最多一个 primary TaskGraph task。
- dynamic System dependency直接使用 L0 `TaskDependencies`。
- command task复用现有 `EcsCommandBuffer/EcsCommandWriter`，一个 Simulation 最多一个共享 command buffer + 一个 final flush task。
- System最终构造必须发生在 intended Simulation Lane。

### Scene

```cpp
struct SceneDescription {
    AssetId world;
    AssetId simulation;
};
```

canonical asset type：`lux.scene.description`。

Scene只拥有：

```text
WorldDescription
Registry
Simulation
stop_source
```

不拥有 mandatory streaming/index/residency/process/render state。

### Streaming

- load/unload/index/query/entity-lifetime policy 属 concrete developer System。
- generic Scene/runtime只提供 World IO/materialization mechanism。
- 不建立 `WorldStreamingSelection`。
- probes 前不建立 generic `WorldStreamingSourceComponent`。

### Async lifetime

- Scene owns stop source；Host owns shared structured async scope。
- started async operation不得借用 `Scene&/Registry&/System&`。
- capture value/shared state + weak completion target。
- Scene requestStop 后可销毁；Host scope只在 Product shutdown join。
- 不创建 per-Scene async scope wrapper。

### Execution lanes

```text
Simulation Lane      authoritative state
        |
        | compact latest stable state
        v
Presentation Lane    wall-time UI/camera/presentation
        |
        | bounded frame handoff
        v
Render Thread/Domain
```

Simulation→Presentation：`LatestSpscExchange<T>`，SPSC triple-buffer latest-wins，producer/consumer never wait。

Presentation→Render：现有 bounded persistent SPSC frame lane 仍然适用。

reliable operations/events 不走 latest-wins state channel。

---

## 5. Design barriers

### Barrier A — Asset residency

ownership已冻结，但 generic demand identity/lease/retry/invalidation 未冻结。

Probe A 前禁止新增：

```text
DemandKey
DemandTracker
ResidencyBridge
ResourceDemandRegistry
```

### Barrier B — after four probes

只有完成 Spatial3D / Spatial2D / Pixel / Robot 后，才讨论：

```text
TimeDomain/TickGroup exact API
Scene structural phase exact API
common Presentation->Simulation ingress primitive
streaming-source generic promotion
asset-demand generic promotion
```

---

## 6. Implementation-LLM stopping rule

如果某个子任务只有通过新增未授权 public type才能继续：

> **不要补架构。停止这个子任务，报告 gap。**

尤其禁止“先写个临时 Context/Adapter，以后再删”。临时 public abstraction 通常会变成事实架构。

---

## 7. First document to hand to an implementation agent

先给：

```text
00-l0-l3-master-implementation-plan.zh-CN.md
```

然后只给它当前 Phase 所需的对应规范与直接 prerequisite，不要一次把 P1/未来 plugin 设计混进当前任务。
