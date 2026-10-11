# 07 — 唯一 RenderGraph：从 Data Dependency/Feature 到 Logical/Native Plan

## 7.1 Graph 只理解 GPU 值与工作，不理解业务 Data T

`RenderGraphDefinition` 仍保存 `GraphResource`（Buffer/Image 与 Import/Transient/Export）、`GraphPass`、`GraphResourceUse`、`Producer`、`Dependency`、`Scope` 与输出合同。Graph 作者看到的是 DataSet 的**结构性逻辑 DataView**（Schema、Scope、资源界面形状、BindingSlot），不要求 GPU VkBuffer 已经分配。`GraphResourceId` 是单份 Definition 局部 ID，GPU DataTypeId 不进入 Graph 通用变体。Typed Data Views 在 Graph Composition 期间被冷态降低成已验证 `GraphBuffer` / `GraphTexture` 及结构性 owner slot；真正 DataSet/backing/Ready 在 Invocation/Native Import 绑定。新增 Gaussian/Light/Sprite 不改 `RenderGraph` 核心算法；Native 新 GPU primitive 按真正需要扩展。

## 7.2 完整保留 C0–C8 编号和职责

DataSet/Feature Requirement/Capability 的解析在冷态 Feature Composition 完成；它输出已绑定 DataSlot/View 与中立 StorageInterfaceShape，再贡献 Graph 资源和 Pass。Graph 不依赖 DataStorage、Runtime registry 或业务类型。

| 阶段 | 输入/输出/证明 |
|---|---|
| C0 Canonicalize | Graph 作者声明 → owning IDs、typed resources/pass、normalized ranges |
| C1 Resolve | Graph 内 required/optional producer、semantic provider、import/export、fallback → 确切图资源；不查 DataSet Registry |
| C2 Validate Interfaces | PassSchema 与 Graph/Shader 资源访问接口契约一致；业务 authority 在 Composition 先验证 |
| C3 Build Versioned Hazard Index | exact buffer/image ranges、显式 producer/version/order → RAW/WAR/WAW 与初始化证明 |
| C4 Topological Schedule | 确定性拓扑；歧义/循环包含 pass/resource/path；不靠地址或注册顺序 |
| C5 Root/SideEffect Reachability | 从真实 present/readback/export/external effects 保留可达有用 Pass |
| C6 Lifetime/Usage | 子资源/版本逻辑 first/last use、有效 usage、alias 候选；不证明 GPU completion |
| C7 Scope & View Group Proof | Scene/View/Target 输入、revision/epoch/time/side effect 的合法共享证明 |
| C8 Emit LogicalPlan | 唯一中立、拥有型、不可变逻辑结果及诊断/完整缓存比较事实 |

RD2 不新增第二套调度器，不重排已验收 F2 阶段。DataSchema/GPU ABI 的冷态对账复用已有生成器与 LayoutPlan，Graph C2 接收中立接口事实，不访问业务 catalogue。

## 7.3 完整保留 C9–C13 编号和职责

| 阶段 | 输入/输出/证明 |
|---|---|
| C9 Layout/Shader | 完整结构性 owner shape、PassSchema、Shader 与 DeviceCaps → LayoutPlan、重定位且验证的 SPIR-V |
| C10 Physical resources | logical usage/lifetime 与预冻结 StorageInterfaceShape → 相容 native backing/placement、import/export/alias 物理约束；不晚改 Shader ABI |
| C11 Native pass execution | Shader variant 与 native pass contract → 完整 PSO、不可变录制/绑定配方；无 lazy creation |
| C12 Queue/sync | exact hazards、stage/access、queue/native overlap 与 external readiness → BarrierProgram、ownership/wait/signal 与提交调度 |
| C13 Executable assembly | 完整 C9–C12 候选 → ExecutableGraphPlan、native owners/code pins/bounded invocation 状态与退休合同 |

Compilation 各层 identity 保持其真实职责。Shader/Layout 结构性缓存键包含 BindingSlotKey、完整 Schema/GPU Layout/Storage compatibility、Shader/Device/Target 等必要事实；**不包含具体 DataSetId、普通 dirty revision、Camera 数值、FrameSerial 或当前 native handle**。完整字段比较最终确认相容性，hash 不能单独证明相等。

Invocation 才将结构槽绑定到实际 DataSet/NativeBackingGeneration，并验证 owner、generation、Ready、last-use。改变 GPU ABI、资源形状或设备能力时按既有 native key 产生新候选。跨 View 共用计划不等于跨 View 共用可变资源或无条件只执行一次 GPU 工作。

## 7.4 GPU authority 与 Graph transient 的边界

`DataSet<ParticleGpu>` 可有 GPU-owned resident Buffer，GPU Compute Pass 更新其版本；Graph 通过 import/use/write 权限和 Ticket 同步，而不拿走数据的 Native owner。中间 `CullScratch`、`VisibleSlots`、`DrawArgs` 可以由 Graph 在每个有界 invocation slot 分配 Transient Buffer；若连续帧需历史，应明确 View DataSet/History storage，不偷偷延长 transient 生存期。

Transient Alias 只有在实际 GPU 各 Queue/Cross-frame overlap 证明成立且 Native allocation requirements compatible 才允许；反之 noalias。Imported Buffer/Image 还需检查真实 overlapping VkDeviceMemory memory ranges；两不同 VkImage Handle 可能共享一块 VMA Allocation。

## 7.5 从 Feature 的依赖声明计算 Graph 的完整算法

**示例：** Light Feature 提供 `DataSet<LightGpu>`, GBuffer Feature 输出 `GBuffer.Normal`/`GBuffer.Albedo`, Shadow Feature GPU 写 `ShadowAtlas`，DeferredLighting Feature 要求三者。Compose 时验证 Type/Key/Scope/ABI 及 Shadow 的 optional fallback，再把每个 `GraphResourceUse` 绑定到实际缓冲/图像。GBuffer write 和 Lighting read 通过 `PassProducer + version` 建 RAW Edge，LightCPU Dirty→Upload 的 GPU readiness 由 Native Import Ticket 提供，并非需发一个伪 `LightFeaturePass`。Shadow 如果无 Pass/未安装，必须选择合法 fallback variant；不能绑定随机旧 image。

Graph 依赖是**按资源精确访问推导**，不是笼统 Feature DAG 的拓扑排序。两个不同 Feature 都只读 LightBuffer，不强制顺序；它们都写同 DataSet 同区间则必须有明确定义的顺序、分区或 cold-reject。

## 7.6 N Scene/N View 执行

多个相同 RenderPath、Shader、Target signature 相容的 View 共享 Definition/Logical/Native immutable Plan，但 View 的 Camera/History/Bindings/Input Events 不同。Scene-scope GPU Pass 按真实 source revision/view-independent input proof 去重；View-scope Cull/HZB/GBuffer/3DGS Projection 各自独立；Target Scope 只属于正确 Target/Composition。不能认为编译 Plan 共用就自动 GPU work 只执行一次。GPU Multiview 需要实际设备、shader、array-view、合并合法性，并保留 separate-pass fallback。

**关键 F5 衔接**：`frame_capacity=3` != 最大活跃 View 数量。Native Immutable Plan + N 有界 Invocation Slot / Slot lease（按真实完成回收）是后续合同；不能一个 Scene 四 View 在一个 `GraphSlot` 上相互覆盖 Descriptor/Attachment 状态，也不能为了 4 View 重建 4 份 Pipeline。

## 7.7 Native 录制边界

当前 F4 的 `variant<DispatchCommand, DrawCommand, CopyCommand>` 是有限测试指令，而非最终 Feature SDK。后续受限 `ComputePassContext/GraphicsPassContext/TransferPassContext` 要能表达 Vertex/Indexed/Indirect/IndirectCount/Copy/Dispatch 等已获授权的 native primitives；作者在 PassSchema 宣告 Resource/Access 后才用相应 ops 录制，避免重复式 `read()` + `.bindDS()` 和私有 Vulkan commands。新 Vulkan RT/mesh shader primitives 可局部扩展 Native，并在 Device Cap 选择合法 fallback；不是另建新 Renderer。
