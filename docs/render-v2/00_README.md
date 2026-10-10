# LuxEngine Render V2：最终设计与实施文档集 R2

> 状态：**RECORDED / NOT STARTED**  
> 当前设计审阅基线：`a669409a289a6fa4092f21176397795b1cdb7f3e`  
> 当前分支：`codex/editor-framework-v2`  
> Render V1 frozen reference SHA：**`a669409a289a6fa4092f21176397795b1cdb7f3e`（用户批准带已知问题冻结，不宣称最终资格 PASS）**  
> 启动决定：**用户于 2026-10-10 明确批准立即启动；旧阶段 PARTIAL/NOT_RUN 仅记录为已知问题，不再是 Render V2 的入场门禁。**

本目录是 Render V2 的最终实施规范 R2。它保留上一版的核心方向：冻结 V1、clean rewrite、typed RenderData、三条通信 Lane、单 RenderSystem、单 VulkanBackend、单 RenderScene/RenderGraph 合成；本修订只做最后一轮**减法、依赖澄清与可测性能约束**。

## 1. R1 已收敛的最终设计（R2 继续保持）

R1 不新增新的大型 framework，主要收敛十点：

```text
1. 删除独立 render_feature execution component。
2. Feature 稳定元数据归 render_core；执行扩展面归 render_vulkan/feature。
3. RenderData / SceneCapability / RenderGraph 三类 dependency 完全分离。
4. stable semantic ID 与 runtime-local route ID 完全分离。
5. 固定线程语义：PROGRAM/CONTROL owner-thread；UPLOAD MPMC。
6. 明确 RenderResourceDomain scope，避免每 Scene 重复上传共享资产。
7. RenderGraph 明确 Definition / CompiledPlan / FrameBindings 三段。
8. SceneCapability table 只能做 non-owning composition index，不拥有第二份真相。
9. VK_ERROR_DEVICE_LOST 是 terminal runtime failure；不实现自动恢复状态机。
10. 0-allocation gate 改成严格、可重复测试的 benchmark contract。
```

此外，Projection 稳态更新必须接近 `O(changed entities)`；typed writer 不得在每条消息中 runtime switch `kind/lane`。

## 2. 最终 functional strata

不再用容易误解成线性继承关系的 L0–L7 表述。Render V2 分为四个功能层：

```text
FOUNDATION
    render_core
    render_data (schema-only typed domain contracts)
    render_transport
    render_graph
    Vulkan Foundation

EXECUTION
    VulkanBackend
    RenderRuntime

EXTENSIONS
    Vulkan Feature API
    built-in render_features

INTEGRATION
    RenderSystem / Projection / RenderResourceDomain
    Product / Editor / UI / Plugin composition
```

真实依赖仍是单向 DAG，不是四层彼此全连接。

## 3. 最终 mental model

```text
Scene
└── RenderSystem
      ├── RenderProjection
      ├── RenderResourceDomain&
      └── typed RenderData
               ↓
          RenderRuntime
               ↓
          VulkanBackend
               ↓
           RenderScene
      ┌────────┼──────────┐
      │        │          │
  SceneData  Features    Views
      └────────┼──────────┘
               ↓
          RenderGraph
               ↓
          RenderTarget
```

关键约束：

```text
World/ECS 不知道 concrete RenderFeature。
RenderFeature 不知道 World/ECS。
RenderRuntime 不依赖 built-in render_features。
VulkanBackend 不依赖 built-in render_features。
Scene integration 不 include Vulkan internals。
```

## 4. 三种 dependency，不得混用

### A. RenderData dependency

跨 Scene→Backend 边界的数据：

```text
MeshInstanceUpdate
LightUpdate
ViewCameraUpdate
```

### B. SceneCapability dependency

Backend Scene 中持久存在的共享能力：

```text
MeshSceneData
LightSceneData
RenderableGeometry
```

### C. RenderGraph dependency

当前 compiled graph 中 pass 对逻辑资源的 read/write 关系：

```text
SceneDepth
SceneColor
GBuffer
ShadowMap
```

禁止用一个万能 `requires[]` 同时表达三者。

## 5. 文档索引

### `01_现状问题与Legacy冻结策略.md`
V1 规模、结构问题、Legacy freeze、oracle、算法迁移纪律。

### `02_分层架构与依赖规则.md`
四个 functional strata、真实 target DAG、三类 dependency、Feature API 归属和 negative tests。

### `03_Core与Transport_数据契约和通信.md`
stable ID / local route ID、PROGRAM/CONTROL/UPLOAD、线程模型、typed→erased→typed、backpressure、packet ownership。

### `04_RenderGraph与VulkanFoundation.md`
Graph Definition/Compile/Execute 分界、Vulkan leaf owners、pipeline/descriptor hot-path 禁令、迁移算法边界。

### `05_VulkanBackend_Runtime_Frame_Target.md`
VulkanBackend、RenderRuntime、Scene/View/Frame/Target、Device Lost terminal policy、per-FIF scratch。

### `06_Feature系统与SceneCapability.md`
Concrete Feature、Vulkan Feature API、SceneCapability 非拥有 projection、provider/effect、mixed rendering。

### `07_RenderSystem_Projection_ResourceDomain.md`
RenderSystem、Projection、ResourceDomain scope、共享资产与 Scene-local state、`O(changed)` projection。

### `08_Cpp_RAII_错误语义与热路径规范.md`
RAII、complete object、expected、禁止 silent guard、线程/热路径/分配硬规则。

### `09_实施阶段_VerticalSlice_算法迁移.md`
最终实施顺序：Core → Transport → Graph → Vulkan → minimal Runtime/Backend → minimal Feature → typed data → Scene integration → Mesh/PointCloud → complex features。

### `10_验收门禁与LLM实施合同.md`
LLM hard contract、allowed/forbidden path、dependency/ID/thread/Device Lost/zero-allocation/static gates。

## 6. 共同优先级

冲突时：

```text
1. 10_验收门禁与LLM实施合同.md
2. 当前阶段对应功能层文档
3. 02_分层架构与依赖规则.md
4. 本总览
5. render_legacy 注释仅是行为/算法参考
```

## 7. 不提前实现的框架

R1 明确**不实现**：

```text
IRenderBackend
Universal TemporalHistoryManager
Universal VirtualResourceManager
generic multi-provider capability resolver
generic Render allocator framework
Feature SBO
async pipeline compiler
multi-backend Feature ABI
```

只有真实 consumer 出现后再抽象。

## 8. 一句话原则

> **Foundation 只提供稳定机制；Execution 负责执行；Extension 提供领域能力；Integration 负责把 Scene/产品接进来。复杂功能增长应增加 typed data、persistent capability、资源和 pass，而不是继续增长顶层框架。**

## 9. R2 开工前修订（强制）

R2 的修改不改变终态架构，仅补齐实施中容易产生双重真相或错误构建的边界：

1. **R0 移走旧源码后，V2 开发分支在 R17 产品切换前不要求原 EDITOR/PLAYER/TOOLCHAIN 全量构建通过。**旧基线在独立 V1 worktree 运行。V2 新层使用唯一、明确的 `cmake/render-v2-bootstrap/` standalone build入口；禁止通过兼容桥伪装原产品可构建。R0 必须实际验证这个空骨架可配置，R1 开始逐层验证。
2. **领域 typed RenderData 合同放 `render_data`（无 Vulkan、schema-only，可由 TOOLCHAIN 消费）**：`render_core` 只放通用机制和强类型 ID，`render_features` 只放执行实现；`render_content` 留给熟化/打包内容和 codec，不混入每帧状态协议。`scene_render` 依赖 `render_data`，不依赖具体 Vulkan Feature。
3. **跨 PROGRAM/CONTROL/UPLOAD 的先后性、准备态与资源使用寿命必须单独被验证。**单 lane FIFO 不是跨 lane 全序。
4. **RenderSystem 在 retained packet backpressure 期间仍必须追踪后续 World revisions**，不能因为当前 packet 被保留而丢变化。
5. **Graph topology 与 per-frame values 是不同变化**，只有结构改变才触发重编译；动态帧数据进入 `FrameGraphBindings`。
6. **删除历史中间文档必须先归档引用清单，再执行明确的删除集合**，不可按宽泛 glob 擅删当前契约。

R0 执行权威入口：[`11_R0_实施工作单.md`](11_R0_实施工作单.md)。任何旧文档仍要求“等待 MA 全 PASS”的文字均被本用户启动决定取代。
