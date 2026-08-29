# L0–L3 Topology、CMake 与 Architecture Gates（Baseline v3.1）

> 状态：Normative。
>
> Hard gates 按 Phase 逐步启用；禁止在 target 尚不存在时通过 placeholder target“提前完成架构”。

---

## 1. Target topology

目标 topology：

```text
engine/
  domain/
    world/
      core/
      storage/
      asset/

    simulation/
      description/
      system/          # System type catalog/registration
      core/            # Simulation runtime + private instances
      ecs/
        core/
        schema/
        hierarchy/
        transform/
        snapshot/
        task/
        ...

  process/
    execution/
    io/

  scene/
    core/              # Scene + SceneDescription semantic
    asset/             # SceneDescription codec if separate leaf
    runtime/
      world/           # WorldStorageSource/load/materialize
      presentation/    # LatestSpscExchange<T> first implementation
    spatial3d/         # created by Probe A when real code exists
    spatial2d/         # created by Probe B when real code exists
```

明确不创建：

```text
engine/runtime top-level
simulation/ecs/materialization
scene/runtime/manager
scene/runtime/context
scene/runtime/services
```

Pixel/Robot package位置由 Probe真实 functionality决定；不要提前创建空目录/target。

---

## 2. Namespace

```text
World       -> lux::world
Simulation  -> lux::simulation / lux::simulation::ecs
Scene       -> lux::scene
Spatial3D   -> lux::scene::spatial3d
Spatial2D   -> lux::scene::spatial2d
```

禁止 architecture-number namespace：

```text
lux::l1
lux::l2
lux::l3
```

---

## 3. Classification

典型：

```cmake
world_core             LAYER WORLD       PRODUCT RUNTIME ROLE DOMAIN/FOUNDATION per existing taxonomy
world_storage          LAYER WORLD       PRODUCT RUNTIME ROLE FOUNDATION
simulation_system      LAYER SIMULATION  PRODUCT RUNTIME ROLE DOMAIN/FOUNDATION
simulation_core        LAYER SIMULATION  PRODUCT RUNTIME ROLE COMPOSITION
scene_core             LAYER SCENE       PRODUCT RUNTIME ROLE COMPOSITION
scene_runtime_world    LAYER SCENE       PRODUCT RUNTIME ROLE INTEGRATION
scene_runtime_present  LAYER SCENE       PRODUCT RUNTIME ROLE FOUNDATION/INTEGRATION (choose existing valid role)
scene_spatial3d        LAYER SCENE       PRODUCT RUNTIME ROLE DOMAIN
scene_spatial2d        LAYER SCENE       PRODUCT RUNTIME ROLE DOMAIN
```

不要为 execution lane新增 architecture layer。

---

## 4. scene/core dependency

允许：

```text
CORE
RESOURCE stable AssetId
WORLD
SIMULATION
```

禁止：

```text
PROCESS
stdexec
File IO
Render backend
Asset residency
Editor
Authoring
Toolchain
Host
ObjectMessageQueue ownership
```

`std::stop_source` 是标准库 value，不构成 Process dependency。

Scene core第一版不依赖 LuxObject。

---

## 5. scene/runtime/world dependency

允许：

```text
SCENE core
WORLD
SIMULATION ECS core/schema
PROCESS low-level OperationPort/Sender/IO
CORE/RESOURCE minimal byte/capability types
```

禁止：

```text
scene/spatial3d
scene/spatial2d
Render backend
Editor
Authoring
Toolchain
Host
```

因此 L3 mechanical runtime不会知道 concrete query/policy。

---

## 6. scene/runtime/presentation dependency

第一版 `LatestSpscExchange<T>` 放：

```text
engine/scene/runtime/presentation
```

原因：目前只有 engine runtime需要，避免未经证据新建共享 `modules/core/concurrency` target。

它只允许依赖：

```text
CORE / standard library atomics
```

不得 include：

```text
Scene
Simulation
Registry
Render
World
```

虽然物理位置在 L3，其模板本身保持 domain-blind。

Probe 后若 sibling/non-engine consumer也需要，再单独批准下沉到 shared modules。

---

## 7. SystemRegistry target rule

Canonical statement：

```text
SystemRegistry = known System TYPE registrations/install thunks
Simulation = concrete System INSTANCE owner
```

`engine/domain/simulation/system` 不允许重新扩张为：

```text
runtime global service locator
live System manager
plugin manager
```

旧 concrete instance owner implementation必须被替换/迁移，而不是保留 facade。

---

## 8. Explicit System registration contribution

Concrete packages暴露：

```text
xxxSystemRegistrations()
```

Product explicit `SystemRegistry::add()`。

禁止 CMake/link order 作为 runtime type registration语义。

禁止 Product依赖 `ReflectionRegistry` 自动发现所有 Systems。

---

## 9. World build API gate

Source check确保 v2 后不再出现：

```cpp
WorldPartitionLayoutBuilder(const WorldDescription&)
```

目标应为 object-id span构造。

禁止新 production type：

```text
WorldPartitionWorkspace
WorldCookContext
```

---

## 10. ECS precision gate

Canonical：

```text
Transform2D/3D double
WorldTransform2D/3D double
```

Source/installed tests防止：

```text
Eigen::Vector3f translation in canonical Transform3D
Eigen::Affine3f WorldTransform3D
```

Render/physics private float conversion允许，但必须显式 relative conversion。

---

## 11. Component materialization prerequisite gate

Phase 4 必须在 `ComponentSchema` 或现有 schema operation holder 中存在 generated decode/emplace seam。

Phase 8 `scene/runtime/world` 不得定义第二套：

```text
ComponentFactory
ComponentMaterializerRegistry
ReflectionComponentAdapter
```

Negative source test可针对这些名字/known patterns。

---

## 12. Process boundary

Process只拥有：

```text
Timer/File/byte IO
OperationPort -> Sender
execution resources/primitives
```

Process生产代码禁止 include：

```text
Scene
WorldPartitionData workflow types
WorldMaterializer
StreamingSystem
Asset residency policy
Render gameplay policy
```

注意：L3 `ReadWorldStorageRange` operation port type可以被 Process adapter template化处理，但 Process package本身不 include该 concrete operation header做业务 switch。

---

## 13. Product terminology

Architecture prose使用：

```text
Product executable
runtime product
Host
application binary
```

不假设 universal Player。

如果已有 CMake build profile 仍叫 PLAYER，可暂留 profile enum；它不是 architecture concept。

---

## 14. Phase-gated source vocabulary

### Phase 0 起禁止新 production roots中出现

```text
WorldStreamingBinding
StreamingManager
SceneServices
SceneContext
SystemFactoryRegistry
SimulationContext
WorldPartitionWorkspace
WorldMaterializationPlan
WorldMaterializationRegistry
```

### Barrier B 前额外禁止 production

```text
TimeDomainRegistry
ClockManager
PresentationManager
LaneManager
ScenePhaseManager
```

不要机械扫描 legacy/archive/doc examples造成误报；gate针对当前 production source roots。

---

## 15. AssetResidency hold gate

Barrier A 前禁止新增：

```text
AssetDemandKey
DemandTracker
ResidencyBridge
ResourceDemandRegistry
```

`08` 标记 DESIGN HOLD。

这不是缺实现，不允许 LLM为了 Probe编译而绕过。

---

## 16. Negative dependency tests

当相关 target存在后逐个加入：

```text
scene_core_depends_process_negative
scene_core_depends_render_negative
world_storage_depends_scene_negative
process_depends_scene_negative
simulation_depends_scene_negative
scene_runtime_world_depends_spatial3d_negative
scene_runtime_world_depends_render_negative
simulation_system_registry_depends_scene_negative
```

不要 Phase 0 先建空 target只为了跑 negative test。

---

## 17. Installed consumer sequence

按施工 Phase 增加：

```text
Phase 1:
  modify existing modules/core/task only
  TaskDependencies unit/installed consumer coverage
  no new target/package

Phase 2:
  world-description-v2
  world-partition-layout

Phase 3:
  world-storage
  world-partition-data

Phase 4:
  component-decode-emplace
  large-world-transform

Phase 5:
  simulation-system-registry
  simulation-runtime

Phase 6:
  Process existing installed consumers only / no new target by default

Phase 7:
  scene-description
  scene-core

Phase 8:
  scene-runtime-world

Phase 9:
  latest-spsc-exchange

Phase 10:
  product probe targets/tests
```

不再有：

```text
simulation-world-materialization
scene-streaming-manager
```

---

## 18. Superseded documentation gate

Phase 0 必做：仓库中冲突旧 implementation specs 文件顶部写：

```text
SUPERSEDED: see L0-L3 Architecture Baseline v3.1
```

尤其旧文档如果仍宣称：

```text
Simulation::materialize/dematerialize
SystemRegistry owns concrete instances
Scene owns mandatory PartitionIndex
FrameInfo / float SimulationStepInfo as final time design
```

不得与新 baseline 同时 normative。

---

## 19. Hard gates only after probes

Phase 11 才把所有新 production roots完整纳入 architecture gate。

原因：Probe期间允许 concrete domain package试验，但不允许 generic promotion。

最终 gate应确认：

```text
no L1->L3 dependency
no Process business semantics
no Scene mandatory streaming state
no generic WorldObjectId->Entity service
no canonical float Transform
no old SystemRegistry facade
no universal Player assumption
```

---

## 20. CMake “package purity” rule

继续遵守现有：production package root = Collection or Leaf，不建 Hybrid。

如果一个目录同时开始承担：

```text
Scene core semantic + World IO + Spatial3D policy
```

必须按现有 package taxonomy拆分，而不是引入 `Manager` 试图隐藏混合职责。

---

## 21. Implementation LLM rule

当编译依赖缺失时：

```text
1. inspect previous Phase
2. reuse existing target/type
3. if public capability truly absent -> record gap
4. do NOT introduce an adapter target
```

CMake成功不是架构正确性的替代品。
