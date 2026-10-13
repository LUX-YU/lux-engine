# 28 — RD3-R3 目标目录与真实依赖 DAG

> 本章重写 RD3-R2 `LAYOUT` 并与新的 Attachment/Compiler/Projection/Native Bootstrap 设计保持一致。目录建议按**稳定职责**划分，不预设每一个目录都要 CMake target；实际文件迁移按需要的消费者逐步进行。只有本章目录是 RD3-R3 目标，不再引用旧 RD3/R2 目录图作为当前事实。

```text
modules/function/render/
├── core/               # RenderResult/Strong IDs, neutral GPU/data descriptors
├── transport/          # 既有 R2 PROGRAM, CONTROL, UPLOAD, reply/admission/diagnostics
├── client/             # RenderRuntime & typed protocol client (NO Vk/D3D12)
├── data/               # neutral DataSchema, Version/Wire/Projection data contracts
├── scene/              # neutral Scene/View; Scene owns View SlotMap
├── target/             # neutral TargetDesc/Attachment placement/final output contracts
├── effect/             # RenderEffect, Feature author ops, logical Graph ownership
├── graph/              # existing RenderGraphBuilder/C0-C8, ResourcePlan (neutral)
├── backend/            # neutral RenderBackend; one editable Attachment relationship
│   ├── include/        # narrow native port/bootstrap-free neutral server API
│   └── src/            # owner-thread/attach transaction/cold target schedule
├── vulkan/             # selected concrete Native provider; Engine not depending on it
│   ├── context/
│   │   ├── InstanceContext.*    # Native bootstrap-OWNED, outlives Session
│   │   ├── DeviceContext.*      # Session-OWNED, selected VkDevice/VMA/Queue/Caps
│   │   └── ResourceContext.*    # Session-OWNED, native staging/pool services
│   ├── DeviceSession.*          # ONE selected device session, borrows Instance
│   ├── device/                 # existing R4 VulkanInstance/Device/Allocator RAII
│   ├── memory/                 # Buffer/Image/Arena primitive; no business Registry
│   ├── resources/              # PRIVATE ScopeResources/leases/native storage factories
│   ├── transfer/               # one queue/submission/staging/retirement mechanism
│   ├── shader/                 # SPIR-V/PSO/OwnerShape/Layout/NativeShaderProgram
│   ├── graph/
│   │   ├── Compiler.*          # C9-C13 fixed cold Native stages
│   │   ├── Graph.*             # immutable code, no per-View mutable business state
│   │   ├── Executor.*          # record/submit algorithm; single FrameLoop owner
│   │   └── ExecutionContext.*  # native FIFO/descriptors/tickets/transient/history
│   └── target/                 # Offscreen/Surface/Swapchain/Readback WSI backing
├── features/            # actual Mesh/Lighting/Shadow/Grid/Canvas/PointCloud/Robotics authors
│   └── <feature>/       # neutral algorithm & data schema; opt native providers separately
└── sdk/                 # only if installed plugin C ABI/authoring need distinct export

engine/scene/builtin_systems/render/
    # RenderSystem/registered ECS DataProjection, O(changed) + R2 Client only
engine/toolchain/shader/
    # existing one codegen/reflect toolchain; Vulkan SPIR-V or future D3D12 DXIL outputs
```

## 28.1 CMake 依赖有向图

`render_core` ← `render_transport`/`render_data`/`render_graph`; `render_client → render_core+transport+neutral_schema`；`render_effect → graph/data`；`render_backend(neutral) → core/scene/effect/target/graph/transport`；`render_vulkan → neutral backend/graph + R4 Device/Queue/ShaderToolchain`；Engine RenderSystem → client+neutral Schema。`vulkan` 不被 `RenderRuntime.hpp`、Engine/Editor ECS Project 以及中立 Feature 公开头链接或包含。需要时 Native Provider 自己构造 DeviceSession，并通过窄 native port 交给中立 Backend。

P2-A 真实代码当前仍在 `modules/function/render/runtime/**` 中，包括 `DeviceSession.hpp/.cpp`、`ResourceDomain.hpp`、`Objects.hpp`、`Backend.cpp`，Native DeviceSession/Instance 尚未完全分离。目标目录迁移必须根据 include/link/installed consumer 与实际额外修改影响判断，不能一次性造几十个空文件或为每种“概念”创建独立静态库。`RenderBackend::Impl` 当前 Vulkan 强耦合可有界迁移；中立 UI/client 必须保持可安装头的 backend independence。

## 28.2 文件组织约束

新 `Graph`/`ResourcePlan` 在中立 authoring 层；`ScopedResources` 仅 Native 侧存储，不新建 Engine 级 ResourceDomain Registry；`DataProjection` 在 Engine/插件数据源模块，不归 Vulkan Context/Effect 类；`Legacy MeshStorage` 落专业 GPU 资源实现并取得最窄 Native Context，不把每个 Mesh 登记进 Session 顶层 Scope 表。Compiler Native phases 不需要动态注册 OO class。拼写统一 camelCase `getOrCreate`，没有 `get_or_create` 或 `VulkanBackend` 大管家类型。


## 28.3 材质工具链与大资源传输的真实目录职责

**已有且必须复用**：`modules/function/material/`（MaterialGraph/NodeCatalog/Pin/Payload/ShaderIR 的中立作者核心），`engine/toolchain/material/`（MaterialCompiler/Cooker/GLSL+SPIR-V 编译，未来可增 DXIL backend），`engine/toolchain/shader/`（单份反射/ShaderABI）；不要在 `modules/function/render/features/material/` 新造同义 `MaterialGraph`/`MaterialNodeRegistry`。RenderFeature/Graph Author 引用的是 Cooked Shader/Material contract，实际 GPU 表放在 `vulkan/resources/material/` 等按业务组织的专业 Storage。

**Native 业务入口**：`modules/function/render/vulkan/transfer/` 保留 R4 SubmissionQueue/Ticket 与 staging；按真实用户增加有界 BulkUpload/Texture/Mesh Transfer 工具和必要 worker，不在 `client/transport` 放 Vulkan 对象，也不为 dedicated VkQueue 再新建一个不兼容的 Submission 同步库；逻辑 Graph 的外部 Ready/Use 合同仍在 `render/graph`。`render/vulkan/graph` 的 ResourcePlanner/LocalRead/TransientPool/ExecutionContext 等按实际消费者组织，不复制 `RenderGraphCompiler.cpp` 旧 1762 行总管。细节见第 29–32 章。
