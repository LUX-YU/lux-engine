# 09 — 实施阶段、Vertical Slice 与算法迁移 R2

## 1. 总原则

新 Render 从 Foundation 向 Execution、Extension、Integration 生长。

每阶段：

```text
implementation
→ independent qualification
→ verification commit
→ STOP
→ review
```

禁止一次跨多层“大迁移”。

## R0 — Render V1 Terminal / Legacy Freeze

入场：用户于 2026-10-10 明确批准立即开始；旧 MA PARTIAL/NOT_RUN 作为 known issues 留存。固定 V1 reference SHA=`a669409a289a6fa4092f21176397795b1cdb7f3e`，不称旧阶段全面 PASS。

输出：

```text
render-v1-terminal implementation SHA
verification SHA/tag
render_legacy frozen source
independent V1 oracle worktree
V1 functional/performance/GPU baseline
```

不写 V2 production code。

## R1 — Render Core

只允许：

```text
modules/function/render/core/
tests
minimal CMake
```

建立：

```text
stable IDs
RenderDataTypeId
FeatureTypeId
SceneCapabilityId
errors
handles
target semantics
static descriptors
```

禁止 Vulkan/thread/Scene/Feature execution。

## R2 — Transport Foundation

建立：

```text
PROGRAM owner-thread SPSC
CONTROL owner-thread SPSC
UPLOAD bounded MPMC
request/reply
blob/attachment ownership
stable ID → local RouteId registration/binding
```

迁 V1 validated queue/packet algorithms。

## R3 — Logical RenderGraph

backend-neutral：

```text
RenderGraphDefinition
logical resource/pass/dependency
```

无 Vulkan package/include。

迁 V1 logical test vectors。

Independent Render Progress 准入门禁：Graph compile 不依赖 Simulation tick；相同拓扑配合不同
FrameGraphBindings 必须复用编译计划；普通动态 Scene/View 值不自动 invalidation，真正拓扑变更才
触发重新编译。保持 Definition / CompiledPlan / FrameBindings 三段模型。纯逻辑 Graph 无
Vulkan、Runtime、Scene/ECS 依赖；R3 不实现真实 FrameLoop。

## R4 — Vulkan Foundation

按顺序：

```text
Instance/Device/VMA
memory owners
descriptor mechanisms
pipeline/shader owners
transfer
retirement
```

每个 owner RAII/fault tested。

不创建 RenderScene。

## R5 — Minimal Real RenderRuntime + VulkanBackend

**直接建立真实最小 `RenderRuntime`，不允许临时 Runtime-like host。**

Vertical slice：

```text
RenderRuntime
→ backend thread
→ VulkanBackend
→ one RenderScene
→ one View
→ one offscreen target
→ compiled clear-color graph
→ readback expected color
```

随后加 presentation target。

验证：

```text
Runtime/Backend/Scene/View/Graph/Frame/Target
Device Lost terminal failure
shutdown
```

R5 另须满足 05 §18 的 Independent Render Progress HARD GATE：真实 backend execution domain，
持久 RenderScene 跨多帧复用；多帧无 PROGRAM 时仍在合法 target/pacing 下通过真实 GPU
offscreen/presentation clear-color 验证，帧数不等于 publication 次数；空队列无无界忙等，
Stop、Device Lost 与 GPU in-flight 生命周期仍正确。可控帧触发/测试时间源可用，fake Runtime 不可用。

## R6 — Minimal Concrete Feature Execution API

只实现最小 Vulkan Feature API：

```text
FeatureInstance
RenderFeatureOps
RenderFeatureContext
RenderSceneContext
explicit registration
```

用一个 trivial `DebugFeature`：

```text
ordinary concrete type
addViewPasses()
```

不先实现完整 SceneCapability framework。

Gate：

```text
0 RenderFeature abstract base
feature install/remove/enable lifetime correct
code lifetime pin test
```

## R7 — Typed RenderData Vertical Slice

定义极小：

```text
DebugColorData
```

链路：

```text
typed producer
→ stable RenderDataTypeId
→ bound local RouteId
→ PROGRAM
→ typed backend sink
→ persistent SceneData
→ DebugFeature reads state
→ output changes
```

验证 writer hot path 无 dynamic lookup/switch。

独立进度验收：只发布一次 DebugColorData，无后续 PROGRAM 时连续渲染多帧并使用持久状态；
再发布新值后，后续帧使用新提交状态，不依赖重复发送相同数据来驱动帧。

## R8 — RenderSystem / Projection Vertical Slice

建立：

```text
RenderSystem
Projection registration
generic ECS projection
publication transaction
backpressure packet retention
```

先用 Debug/Camera 小案例贯穿真实 SceneDriver。

Projection normal update 接近 `O(changed)`。

独立进度验收见 07 §21：受控 Simulation 10 Hz 与 Render 目标 60 Hz 或更高；渲染进度不由
Simulation tick 数决定；Publication 只传输合法状态变化；PROGRAM 背压不强迫停帧，
retained candidate 期间后续 World revisions 不丢失。不要求通用插值系统。

## R9 — RenderResourceDomain / Upload Vertical Slice

建立一个 runtime-integration-scope `RenderResourceDomain`。

先迁简单 Buffer/Texture 资源：

```text
Asset/source
→ UPLOAD
→ resolved handle
→ Projection/PROGRAM
```

验证两个 Scene 共享同一 asset 时不默认重复上传/拥有两份 authority。

## R10 — Mesh Vertical Slice

必须覆盖：

```text
large mesh upload
resolved handle
entity create/update/remove
transform batch
material handle
many entities
backpressure
one View/Target draw
```

迁 V1 Mesh algorithm，禁止复制 old Stage/Feature hierarchy。

## R11 — PointCloud + Mixed Rendering

第二 geometry domain：

```text
Mesh + PointCloud
same RenderScene
same View
same depth/target
one compiled RenderGraph
```

如果需要第二 Renderer/frame loop → FAIL。

## R12 — SceneCapability + Provider/Effect

此时已有至少 Mesh + PointCloud 两个真实 domain，才冻结最小 capability resolver。

迁：

```text
provider owns SceneData
capability index non-owning
consumer attach binds typed pointer once
single-provider ambiguity/missing checks
```

不要实现 generic priority/multi-provider framework。

## R13 — Complex Built-in Features

按 dependency groups：

```text
Camera/View data
Light/Material provider
Forward/Deferred
Shadow
HZB/SpatialCull
Highlight/Gizmo
Terrain/Water
Skinning
Streaming feedback
其它 built-ins
```

每组单独 qualification。

算法迁移期间不顺便改 shading/render algorithm。

## R14 — Engine / UI / Plugin Integration

迁：

```text
EngineRendering
scene_render final wiring
UI rendering
Editor
external Feature plugin
external Projection plugin/sample
```

## R15 — Hot-path / Performance Hardening

只基于之前阶段 profile 做：

```text
allocation removal
wake coalescing
dirty traversal
graph compile invalidation
pipeline/descriptor churn
upload copy
```

任何优化 before/after benchmark。

## R16 — Directory / Include / CMake Finalization

语义已稳定后做：

```text
public include
target/package name
install components
source directory final layout
```

避免 mechanical diff 与架构 diff 混合。

## R17 — Product Cutover

EDITOR/PLAYER 默认 V2。

与 V1 oracle：

```text
representative scenes
image/readback semantic comparison
GPU validation
performance
resource/lifetime stress
```

不维持长期 runtime dual-renderer switch。

## R18 — Final Removal / Qualification

清零：

```text
0 production legacy include/link
0 old aliases
0 compatibility bridge
0 dual renderer configuration
```

运行 final static/dependency/performance/SDK gates。

## 2. 为什么这个顺序更严格

R5 只验证真实：

```text
Runtime + Backend + Graph + Frame + Target
```

R6 才加入 Feature execution。

R7 才加入 typed data。

R8 才加入 Scene/ECS。

R12 才在至少两个真实 domain 后抽 SceneCapability。

避免用未来假设倒逼基础 API。

## 3. Legacy algorithm transplant

每次：

```text
先 V2 contract
先 V2 test
记录 legacy source + terminal SHA
只迁 algorithm/invariant
不复制 old owner/interface
行为对比
```

禁止 `cp old_class.cpp new_class.cpp` 后边删边改。

## 4. 架构迁移期不顺便优化 Feature 算法

保持：

```text
Mesh algorithm
Shadow algorithm
PointCloud algorithm
Deferred lighting
shader math
barrier algorithm
```

除非迁移 correctness 必需。

## 5. V1/V2 equivalence

适合 bit/struct：

```text
wire payload
CPU generated table
logical graph descriptions
```

适合 semantic/image：

```text
GPU rendering
floating shader output
```

不强求无意义 framebuffer bit-exact。

## 6. Performance baseline

R0 固定 V1。

R5 起每层增加同类 V2 baseline：

```text
minimal frame
1 Scene/1 View
1 Scene/4 Views
multi-scene
many mesh instances
mixed Mesh+PointCloud
upload pressure
```

记录：

```text
p50/p95/max
alloc count/bytes
wake count
graph compile count
pipeline create count
GPU validation errors
```

## 7. R2：Clean-room 期间独立构建资格

R0 实施先 freeze，再移动完整 V1 closure。移走后 V2 开发分支正常产品/TOOLCHAIN 的全部旧 targets 暂时不存在，直到 R17 product cutover **EXPECTED_UNAVAILABLE**。这不是可以用 mock/fallback shim 填补的缺陷。

R0 创建唯一 `cmake/render-v2-bootstrap/CMakeLists.txt`（独立、non-install CMake 入口），当 V2 target 尚未出现时可空项目配置，不导入 `render_legacy`。R1–R16 只通过此入口建设 V2 层的构建与测试；逐阶段审阅其实际依赖图。V1 全功能 oracle 继续在冻结 SHA 的**另一个 worktree** 构建/运行，不与 V2 同时链接到一个产品。

R0 的最低验收：冻结 SHA/closure 文件数/哈希、独立 bootstrap 配置、不见 legacy build/include/link、known-issues 和历史文档整理、未污染用户现有差异。**R0 不声称新 Render 可显示图像，也不声称 EDITOR/PLAYER/TOOLCHAIN 在 V2 分支已恢复。**

R1 必须真正建立 `render_core` 后证明该 standalone 构建入口可以编译/运行 minimal test。R17 恢复产品全量构建和 installed SDK 验收资格。
