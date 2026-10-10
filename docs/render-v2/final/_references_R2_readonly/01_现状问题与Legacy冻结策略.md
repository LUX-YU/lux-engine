# 01 — 现状问题与 Legacy 冻结策略

## 1. 基线与规模

设计审阅基线：`a669409a289a6fa4092f21176397795b1cdb7f3e`。2026-10-10 用户批准以此作为 frozen V1 reference base；未完成事项单独记录，不称全面 PASS。

当前 `modules/function/render/` 已经是大型子系统：

```text
约 663 个文件
约 538 个 C/C++ 文件
约 5.66 MB C/C++ 源码
```

Scene-side `engine/scene/builtin_systems/render/` 另有：

```text
27 个 C/C++ 文件
约 266 KB C/C++ 源码
```

Feature 生态已经包括约：

```text
31 个 *Feature.hpp
42 个 Feature/backend implementation .cpp
30 个 *Operation.hpp
25 个 OperationHandler .cpp
```

因此 V2 是 **major subsystem architecture migration**，不是局部重构。

## 2. 当前 V1 的主要结构问题

### 2.1 层次被历史类型覆盖

当前认知链大致：

```text
RenderRuntime
→ RendererThread
→ RuntimeServer
→ GeneralRenderServer
→ FrameOrchestrator
→ FrameDriver
→ Renderer
→ RenderScene
→ render::RenderContext
→ ResourceContext
→ DeviceContext
→ InstanceContext
```

问题不是每个类都无意义，而是这些名称都像“主层”，ownership 和依赖方向无法从名字直接推断。

### 2.2 `Context` 被滥用为 owner

典型：

```text
InstanceContext      实际拥有 VkInstance/debug
DeviceContext        实际拥有 device/queues/VMA
ResourceContext      实际拥有 descriptor/command pools
render::RenderContext 实际拥有 pipeline/descriptor/transfer/retirement infrastructure
PresentContext       实际拥有 presentation semantic state
```

V2 中 owner 必须用 owner/domain/infrastructure/device/target 等真实语义命名；`Context` 仅用于 operation/scope environment。

### 2.3 Scene input 是 Feature-centric

当前：

```text
RenderFeatureSceneBinding
    FeatureTypeId
    ComponentObservationSpec[]
    CreateRenderSyncStageFn

RenderSyncStage virtual base
    prepare
    commitPrepared
    discardPrepared
    fullSync
```

Mesh、Light、Camera、UI 等各自实现 stage。World/ECS 因此直接知道某个 Feature 的输入路径。

终态必须变成：

```text
World/ECS
→ RenderProjection
→ canonical typed RenderData
→ RenderRuntime
→ backend SceneData
→ RenderFeatures consume SceneData
```

### 2.4 Feature 是万能抽象基类

现有 `RenderFeature` 同时承担：

```text
identity
lifecycle
attach/detach
enable/disable
per-view state
frame hooks
scene rebase
frame-context injection
sampled target
RenderGraph contribution
target-slot request
live parameters
scene/context access
```

这导致：

```text
基类不断长虚函数
friend / lifecycle state 绑定 RenderScene
插件 ABI 与 inheritance 绑定
资源型 Feature 被迫继承 pass 型接口
```

V2 改为普通 concrete type + explicit registration + generated/templated erased ops。

### 2.5 Backend 直接 API 与 protocol API 重叠

同一语义可同时从 Server direct method 和 Control/Program/Upload protocol 进入，增加双实现和 validation 漂移风险。

### 2.6 `render_vulkan` 过度聚合

一个 component 同时包含：

```text
comm
GPU device
memory
descriptor
pipeline
transfer
graph
renderer
scene
targets
feature resources
```

功能层不明显，LLM 很难判断一个新功能应该落在哪一层。

### 2.7 Feature-owned resource 混入 backend core

例如 Mesh/Light/Shadow/Material/PointCloud/Terrain 等领域资源位于 Vulkan `resources/`。这让 backend foundation 与 built-in feature domain 相互污染。

### 2.8 热路径存在历史 allocation / indirect lookup 候选

包括：

```text
FrameRuntime transient vectors
SceneViewBatch temporary storage
submission merge scratch
external waits/signals
Feature/view bookkeeping unordered structures
per-call dynamic route lookup risk
```

V2 从第一天起要求 steady-state 基础设施 0 heap allocation。


## 2.9 R1 最终补充：禁止复制“未来兼容抽象”

Legacy 中以下内容即使当前能工作，也不能借“以后换 backend / 更通用”为理由复制到 V2：

```text
backend-neutral Feature execution facade
通用 capability owner registry
自动 device-lost restore state machine
generic Render allocator interface
万能 threaded transport
```

V2 只迁移已被真实 consumer 证明必要的机制。未来能力通过明确 seam 扩展，而不是提前建立无消费者框架。

## 3. 为什么不继续原地大改

原地迁移要求 LLM 同时理解 500+ C/C++ 文件中的：

```text
正确算法
历史结构
旧兼容 seam
测试专用 seam
过期注释
产品接线
插件导出
```

典型失败模式：

```text
旧类型不敢删
→ 新增 Adapter / Manager / Context
→ 两套 authority 共存
→ 加额外 bool/state 保护
→ 每个入口都加 if (!x) return
→ 最终“更安全”但语义更差、性能更差、架构更丑
```

这正是本项目必须避免的 Editor 式补偿性结构。

## 4. Legacy Freeze 决策

根据用户 2026-10-10 直接开工决定：

```text
1. 固定 Render V1 terminal implementation SHA
2. 固定 Render V1 terminal verification SHA
3. 建立 tag/记录：render-v1-terminal
4. 从该 terminal source 复制完整 Render closure 到 top-level render_legacy/
5. 新 Render 从空的 modules/function/render/ 重新建立
6. 新分支建议：codex/render-v2
```

## 5. `render_legacy/` 的地位

它不是 module，也不是可选 runtime。

必须：

```text
DO NOT BUILD
DO NOT INSTALL
DO NOT LINK
DO NOT ADD FEATURES
DO NOT FIX BUGS
DO NOT USE AS INCLUDE PATH
```

建议 `render_legacy/CMakeLists.txt`：

```cmake
message(FATAL_ERROR
    "Frozen Render V1 reference source. Build the terminal V1 SHA in a separate worktree.")
```

Root CMake 不 `add_subdirectory(render_legacy)`。

## 6. Legacy closure 范围

至少冻结：

```text
modules/function/render/**
engine/context/rendering/**
engine/scene/builtin_systems/render/**
modules/function/ui/rendering/**
engine/project/plugins/rendering/**
examples/render-plugin/**
```

产品层其它调用点留在原位置，后续改为新 API。

## 7. 可执行 oracle

不要让 `render_legacy/` 在 V2 build 中运行。另保留独立 worktree：

```text
worktree V1:
    checkout render-v1-terminal
    full Editor/Player/GPU tests

worktree V2:
    new architecture
```

同一场景、资产、camera、配置可做 V1/V2 行为与图像对比。

## 8. Legacy 代码三类

### A — 可直接迁移算法/leaf mechanism

```text
bounded queue algorithms
Program packet storage
Upload byte accounting
reply routing
BlobRef / attachment lifetime
Vulkan RAII leaf owners
DeferredDestroy / retirement
RenderGraph dependency/barrier/multi-queue algorithms
```

### B — 保留算法、重写外壳

```text
PipelineManager
DescriptorService
TransferScheduler
Presentation backing
Scene GPU storage
Mesh/PointCloud buffers
```

保留核心算法，重写 constructor dependency、owner、public surface 和目录。

### C — 禁止复制结构

```text
GeneralRenderServer
RuntimeServer
RenderFeature base
RenderFeatureSet old ownership model
RenderSyncStage
RenderFeatureSceneBinding
RendererThread
RenderContext / ResourceContext hierarchy
Feature-specific proxy hierarchy
giant render_vulkan CMake ownership
```

## 9. Algorithm transplant 规则

每次迁移 legacy 算法：

```text
1. 先定义 V2 新接口/owner
2. 先写 V2 contract test
3. 定位 V1 对应算法
4. 只迁 algorithm body / invariants
5. 不复制旧 owner/interface
6. 建立 V1/V2 behavior comparison
7. 记录 legacy source path + terminal SHA + qualification test
```

禁止：

```text
cp old_class.cpp new_class.cpp
然后边删边改成新架构
```

## 10. Legacy Freeze 阶段完成门禁

必须证明：

```text
V1 terminal worktree 可独立构建/测试
render_legacy 不进入任何 V2 CMake target
V2 install 中 0 legacy header/library
production source 中 0 include render_legacy
legacy SHA 和测试基线归档完成
```

完成后 STOP。不得同一提交开始写新 Render Core。

## 11. R2：冻结 V1 与 V2 构建资格必须分开

当前产品 CMake 直接依赖 `render_client` / `render_feature_client` / `render_standard_content` / `render_runtime` / `render_vulkan` / `scene_render` / `ui_rendering` 等 V1 targets。将完整旧 closure 移进 `render_legacy/` 时，**V2 分支原产品构建预期暂不可用**；不准以 `if(TARGET ...)` 大面积静默跳过、生成空替身库或重新 link legacy 伪装成 PASS。

独立的 frozen V1 worktree 保持完整运行/算法对照；V2 branch 只使用 `cmake/render-v2-bootstrap/` 作为新层专用构建与测试入口，逐层引入真正的 V2 target，不 link `render_legacy`。直到 R17 产品 cutover 才恢复产品全量构建资格。这样是有意的、受控的产品不可构建区间，不是“测试全绿”。

R0 必须记录两个独立状态：

```text
V1_ORACLE: frozen SHA; prior Windows/SDK/GPU evidence, unresolved issues explicit
V2_BOOTSTRAP: configure passes; 0 new production render classes at R0
V2_PRODUCT: EXPECTED_UNAVAILABLE_UNTIL_R17
```

如实施场景无法接受长期产品构建中断，**不得自行发明旧/新双 Renderer 开关**；暂停并请用户在迁移方案之间作明确决定。

## 12. R2：Legacy、文档与基线严格区分

冻结的是 V1 源码/已知行为，不代表旧问题都已验证或修复。R0 必须只形成一份 `V1_KNOWN_ISSUES.md`，保留至少 `render.transfer_idle`、skinned WAR、Clang UBSan PARTIAL、Linux NOT_RUN、minimize、原生输入/IME 延期的事实及来源 SHA；**不修 V1**。

历史中间收据可以从活动源码树删除，但必须先生成文件名清单、删除理由、源 commit、恢复指令，并确认当前 API/架构/README 没有被误判为中间收据。Git history 与树外 evidence 不得删除。
