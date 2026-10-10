# 07 — RenderSystem、Projection 与 RenderResourceDomain R2

## 1. Scene 渲染能力唯一入口

> **Scene 只有安装 RenderSystem 才拥有 Scene→Render publication capability。**

禁止：

```text
MeshRenderSystem
LightRenderSystem
PointCloudRenderSystem
```

每个 Feature 再建 SceneSystem。

## 2. RenderSystem 与 SimulationSystem

Simulation：

```text
advance World/ECS
```

RenderSystem：

```text
consume stable Scene/World facts
publish typed RenderData
```

Render 不成为 gameplay authority。

## 3. RenderSystem 内部概念

```text
SceneBinding
RenderProjectionSet
RenderResourceDomain&
View publication
retained complete RenderProgram candidate
RenderRuntime&
```

不要求每个框都成为类。

## 4. Projection 不是 System

Projection：

```text
World/ECS source
→ canonical RenderData
```

没有：

```text
独立 SceneSystem identity
独立 scheduler
独立 frame loop
独立 Runtime
```

## 5. Concrete Projection

作者只描述：

```text
supported World types
membership/source components
published private state
source→RenderData conversion
departure/remove conversion
```

`makeEcsRenderProjection<T>()` 产生显式 registration。

## 6. Projection runtime contract

包含：

```text
ProjectionTypeId
world types
ComponentObservationSpec[]
output RenderDataTypeId[]
erased create/destroy
prepare/commit/discard
code lifetime
```

traits 是 authoring；registration 是 runtime contract。

## 7. World type resolution

```text
exact world type > "*"
```

同一：

```text
(world type, canonical output role)
```

默认一个有效 Projection。

多个匹配：

```text
AMBIGUOUS_RENDER_PROJECTION
```

不按 registration order 选第一个。

## 8. 不同 World 可产生同一 RenderData

```text
Spatial3D  Mesh3D + Transform + ResolvedMesh
CAD        CadPart + CadPose + ResolvedMesh
Robotics   RobotVisual + RobotPose + ResolvedMesh
                 ↓
          MeshInstanceUpdate
```

Backend 不知道上层 World component 类型。

## 9. Generic ECS Projection mechanism

负责：

```text
construct/update observation
membership leave
full sync
dirty collection
private published state
prepared scratch
batching
commit/discard
statistics
```

作者只写 semantic conversion。

先覆盖真实 Camera/Light/Mesh 模式，不提前造 universal query DSL。

## 10. Projection 性能模型

正常增量更新必须接近：

```text
O(changed entities)
```

建议结构：

```text
dense published/membership state
dirty dense vector
sparse/bit membership marker for de-dup
departure queue
```

禁止稳定帧默认：

```text
scan all entities
scan all published state to detect changes
per-entity route lookup
```

只有：

```text
full sync
explicit rebuild
```

允许 O(N membership)。

capacity 稳定后 traversal 0 heap growth。

## 11. Projection private state

纯 Render extraction cache：

```text
Projection private dense/sparse storage
```

不要向 ECS 塞：

```text
owner pointer
published flag
private render mirror component
```

除非该状态被其它正式 Scene/System 消费。

## 12. RenderSystem publication transaction

```text
if previous complete candidate backpressured:
    retry same owning packet
    return PENDING

prepare/reuse new candidate
prepare Scene/View facts
for projection:
    prepare into typed writer

any failure:
    discard all projection candidates
    published state unchanged

candidate complete:
    commit projection private state noexcept
    retain complete packet

submit:
    accepted      → release retained packet
    backpressured → retain same packet
    failure       → Scene execution failure
```

`commitPrepared()`：

```text
noexcept
allocation-free
only publishes already complete private candidate
```

## 13. RenderResourceDomain scope

**默认不是每 RenderSystem / 每 Scene 一份。**

正常一个 RenderRuntime integration domain 拥有一个 `RenderResourceDomain`，多个 Scene/RenderSystem 借用它。

原因：

```text
Mesh/Texture/immutable material 等共享 asset
不应因 Scene A / Scene B 各上传一份
```

允许多个 ResourceDomain 只用于明确隔离的 runtime/product domain，不作为默认。

## 14. 三种资源 scope

### Runtime/domain-global shared resources

```text
Mesh
Texture
immutable/cooked material data
shared shader/content-backed GPU assets
```

由 `RenderResourceDomain` 解析/缓存/拥有其 Scene-side authority。

### Scene-local semantic resources

```text
RenderScene request/receipt
RenderView request/receipt
scene instance identity
```

属于具体 Scene integration lifetime。

### Feature/backend-local resources

```text
shadow cache
HZB
per-view history
feature-private GPU buffers
```

由 provider Feature/backend Scene state拥有，不进入 Scene-side ResourceDomain。

禁止把三类资源重新集中到万能 ResourceManager。

## 15. 大资源与实时状态分流

```text
Asset/source
   ↓
RenderResourceDomain
   ↓ UPLOAD
RMeshHandle/RTextureHandle/...
   ↓
ResolvedXXX
   ↓
Projection
   ↓ PROGRAM
instance/state update
```

Projection：

```text
不等 IO
不创建 task 等待资源
不阻塞 GPU upload
```

resource 未 ready → membership 不满足；ready 后 Resolved state 触发 observation。

## 16. View

Scene-side RenderViewRequest：

```text
create/resize/remove
camera/source association
published result
```

Projection 不直接 create backend View。

## 17. UI / procedural source

不是所有 RenderData 来自 ECS。

允许：

```text
Editor/UI/procedural typed producer
→ same typed transport
```

属于某 Scene publication 时优先纳入 RenderSystem composition，不恢复 Feature-specific Scene bridge。

## 18. 完成门禁

```text
0 RenderSyncStage
0 CreateRenderSyncStageFn
0 RenderFeatureSceneBinding
Camera/Light/Mesh 使用 generic projection mechanism
normal incremental projection ~O(changed)
full-sync semantics explicit
ResourceDomain scope 非 per-Scene duplication
shared asset across two Scenes does not duplicate semantic upload ownership
backpressure/full-sync/entity-leave 与 V1 等价
external Projection SDK sample
```

## 19. R2：背压期间的 Source Revision 不得丢失

保留一个 owning PROGRAM packet 与其 prepared source snapshot 并不意味着 World 停止变化。`RenderSystem` 在 packet 尚未被接纳时不得重新覆盖该 packet、不得重新发布同一 candidate，也不得丢弃后续 ECS dirty signals / departure records。

应区分：

```text
published revision（已纳入完整 retained candidate 的快照）
observed revision（World 继续推进的最新状态）
pending candidate（待接纳的不可变 owning packet）
```

这些可以是现有 Projection 状态字段，不要求创建新的 RevisionManager。最终具体 revision 存储粒度需在 R8 基于真实 ECS observer 语义确定。验收包含同一 entity 的 create→update→update→remove、backpressure、多 Scene source 和资源 ready 延迟场景，不能 drop/duplicate/reorder 违反逻辑状态。

## 20. R2：ResourceDomain 的共享不引入第二份 GPU owner

`RenderResourceDomain` 默认是 runtime-integration-scope 的 Scene-side asset resolution/retain 账本。Backend GPU resource 的真实拥有者仍是 Vulkan resource owner/provider；跨 Scene 共享只通过稳定 handle、receipt 和引用/退休合同表达，不复制 native backing ownership。

## 21. HARD GATE：Simulation / Publication / Rendering 边界

Simulation 推进 ECS、物理和业务逻辑等具有 Simulation 时间语义的 World 状态。
RenderSystem / Projection 仅将已经成立的状态变化转换为 typed RenderData 并发布。
RenderRuntime / VulkanBackend 按独立进度从已提交的持久 RenderScene 状态产生 GPU 工作、图像和 Present。
Publication 不是帧触发器；Simulation tick、publication revision 与 Render frame serial 不必一一对应。
Backend 不得借用 ECS live mutable state 完成每帧渲染。

Simulation revision/time 与 Render time 分离。无新的 World revision 时仍可产生多帧，
每帧可有独立 View/camera 参数、动态资源绑定与 Render time。只重复最后合法状态即可满足最初合同；
transform/camera 插值或预测由真实 consumer 的时间语义决定，不要求所有 RenderData 自动插值，
不新增 TimeManager 或 UniversalInterpolationManager。

PROGRAM 背压期间 Backend 可以继续使用最后合法状态绘制。§19 的 observed/published revision、
retained candidate 与后续 dirty/departure 责任继续成立，不得阻塞 Simulation 等待默认渲染进度，
不得丢失后续 World revisions 或必要生命周期变化，也不得缓存无界 Simulation frame 快照。

R8 使用受控低频 Simulation 与较高目标 Render 频率（例如 10 Hz 对 60 Hz）验证：
渲染进度不受 tick 数量驱动；Publication 仅传输状态变化；PROGRAM 背压不强迫 Backend 停帧；
retained candidate 期间后续 revisions 不丢失。实际帧率仍受合法 target/GPU/pacing 约束。
具体执行调度见 05 §18；本轮不实现 RenderSystem 或通用插值系统。
