# Render V2 · RD2 Data-First 统一架构：权威总纲

版本：**RD2-D0-C1 — 2026-10-11，已按用户歧义裁决修订的候选设计；仍待用户架构审查批准**。对象：`LUX-YU/lux-engine`, `codex/render-v2`，审查基线 `b9f9a909bc34ac87f00c594996201a20f0af00c8`。冻结 Legacy 来源 `a669409a289a6fa4092f21176397795b1cdb7f3e`。**本包是可评审的目标架构与阶段工作单，不是已实现的生产 API 或功能资格。**

> 一句话终态：一个 RenderRuntime，一个 VulkanBackend，一个 FrameExecutor，一个 RenderGraph；数据先以 C++20 强类型 Schema 声明，由 Scope 拥有的 Typed DataSet 管理逻辑状态，由 Backend Native Storage 管理 GPU 驻留；Feature 只声明需要/产生哪些数据及具体 GPU Pass，冷态 Feature Composition 先解析数据需求，再降低为 typed Graph 资源和 Pass；唯一 Graph 从实际访问推导依赖，统一编译、同步和执行。业务类型开放、GPU 机制受控、无 Universal Instance、无第二 ECS/Renderer。

## 0.1 权威顺序与冻结合同

1. **设计审查批准后**，本 RD2 `00–19`、`20_原设计迁移对照` 取代 2026-10-10 FINAL 原 00–19 的目标架构描述。批准之前旧 FINAL 仍是 Git 仓库已批准的权威；本包不能自动更改阶段授权。
2. 旧包 `appendix/LegacyCapabilityMatrix.csv`（86 条，36 F + 44 I + 6 H）、`GoldenWorkloads.csv`（25）、`RequirementsTrace.csv`（51）和 `TypeOwnership.csv` 被**原字节复制为历史来源**。这些 CSV 中的原阶段或 `Status/QualifiedStatus` 是历史快照，既不是 RD2 新状态，也不被改写。任何新增 Data-First 门禁使用本包 17、18、19 的增量记录；不删除旧行。
3. 719 个 `render_legacy/**` blob/mode 的冻结权威不变；现有 R0–R4、F0–F3 的 I/V 与 NOT_RUN/PARTIAL 历史不可回写；当前 F4 的原验收仍然 `PARTIAL`。资料不等于新的产品能力证明。
4. 若代码现状与本包未来终态矛盾，**不以当前代码倒逼降低设计**；写出兼容迁移及独立验收门禁。若本 RD2 与已验收下层的真实事实冲突，先报告具体源码和测试，不静默修改。

## 0.2 不能混为一谈的身份与类型

| 名称 | 真正含义 | 非含义 |
| --- | --- | --- |
| `RenderDataTypeId` / `DataSchema<T>` | 既有稳定语义类型 ID；Schema 描述字段、版本、存储约束及可选的 GPU 表示 | ECS Entity ID、VkBuffer ID、Shader 物理 Binding |
| `DataSetId` / `DataSet<T>` | 在 Runtime/Scene/View 某一个 Scope 内、可命名的**一份逻辑数据集合** | 每个 T 在 Runtime 只能出现一次；Feature 对象本身 |
| `RecordHandle<T>` | 特定 DataSet 中某记录的稳定生成式身份 | GPU 物理下标、GraphResourceId |
| `TypedStorage<T>` | CPU 索引/脏记录/活动集合的可复用算法或镜像 | 全局万能 ECS、所有资源必须 CPU 镜像 |
| `NativeStorageBacking` | 真实 Buffer/Image/Arena/Page 的 Vulkan RAII owner | CPU `RenderResourceDomain` 资产账本 |
| `SceneCapabilityId` | 具备某项行为或提供能力的契约（可无 Pass） | 数据类型身份；GPU RAW/WAW 关系 |
| `FeatureTypeId/FeatureInstance` | 选择性数据生产/消费与 Pass 组合算法 | CPU/GPU 数据存储必然拥有者 |
| `GraphResourceId` | 单份 RenderGraphDefinition 内的逻辑资源 | 跨 Frame 资产/Type ID/Scene DataSet ID |
| `PassSchema<T>` | 一份 Pass 的同源 GPU 读写/Shader 字段/Scalar 合同 | 数据长期驻留权威；独立于 DataSchema 的第二结构定义 |
| `BindingSlotKey` / `ShaderOwnerId` | 冷态结构性 Shader 绑定合同和完整 Shape（type/semantic/scope/ABI）；运行时匹配具体 `DataSetId` | 每 Scene/View 产生不同 PipelineLayout |
| `SubmissionTicket` | Vulkan 队列已提交及完成的 GPU 使用证据 | FrameSerial、CPU Revision、DataSet 生存期 |

**四组不同的依赖**：DataSet 的语义可用性依赖、Feature/Capability 依赖、Graph Pass 的资源版本/排序依赖、Native GPU 最后使用/Ready 依赖。它们通过确定的编译步骤关联，但绝不能统一成一个 `DependencyId` 或“安装顺序即读写顺序”。

## 0.3 唯一真相与谁做什么

- **Engine/Scene Adapter**：以一个 RenderSystem/Scene 观察变更，发布 typed `PROGRAM/CONTROL/UPLOAD`；不能通过 GPU 或 Render backend 私有头绕过 Transport。
- **Data Schema/Catalogue**：冷态、可由内置和外部插件声明真实类型；只描述数据合法性，不创建一个新业务 Runtime。
- **Scope-owned DataSet**：逻辑记录有效性、写入权威、Revision、DataSet 生命周期；Scene 内既可以有 `LightGpu` 的两个不同 DataSet，也允许没有任何 Instance。
- **Backend Native Storage**：唯一 GPU backing owner；先冻结逻辑 StorageInterfaceShape，再由实际访问与 Device Caps 在 C10 选择相容 backing、Ready、Retirement；不得晚改 Shader ABI。存储能力不是“Feature 提供者”私有缓存。
- **Feature**：具体 C++ 类 + C++20 Concepts/静态生成 Ops；声明输入 DataSet/Capability、可选输出/算法与 Graph Pass。不需要每个 Feature 都有 Pass 或 Store。
- **Graph**：Builder → 中立 Definition → 逻辑 version/hazard/scope → Shader/Layout/Native recipes → 有界 invocation/queue 执行；不得依赖 ECS、业务类型或插件名。

## 0.4 文档目录（从定义到执行，不存在双权威）

| 文档 | 主要合同 |
| --- | --- |
| `01_设计哲学与成熟引擎对照` | 择优理由，何谓开放，约束与非目标 |
| `02_模块分层与所有权` | DAG、实例所有权、生命周期和数据域 |
| `03_数据描述与类型身份` | 声明式 Schema，跨 DLL/Shader 的 ABI 同源 |
| `04_类型化存储与GPU布局` | EnTT 式 API，Sparse/Dense/Chunk/Buffer，Dirty 与 GPU-friendly Workset |
| `05_通信与发布事务` | 三 Lane、版本、Ready、上传、背压、Reader/Writer 安全 |
| `06_Feature组合与数据依赖` | 数据需求/写入权威/Capability，冷态组合与卸载 |
| `07_RenderGraph推导与编译` | 从 Feature/Data 到 Graph、C0–C13、版本/共享/条件 |
| `08_Shader与布局自动生成` | 单源 PassParams、DataSchema、多个真正 Owner、Descriptor |
| `09_VulkanNative与执行` | 物理资源、record、同步、队列、Fallback/错误 |
| `10_Runtime多Scene多ViewTarget` | 单循环，多 View Invocations、共享计划、Target |
| `11_插件SDK与扩展边界` | Host-owned 类型目录、版本化 ABI、Code Pin |
| `12_Legacy功能完整迁移` | 36 F/44 I/6 H 完整追踪，其中原 Legacy 业务与基础设施为 36 F/30 I |
| `13_先进渲染兼容性` | H1–H6 不预建过度框架的扩展路径 |
| `14_Cpp20类型和编码纪律` | 模板能做什么、不能做什么、类型责任与代码标准 |
| `15_端到端API示例` | 数据注册、写入、Feature、Graph、Shader、插件 |
| `16_性能模型与可比实验` | CPU/GPU、复杂度、预算、相同工作负载基准 |
| `17_完整验收与正确性矩阵` | 设计/功能/GPU/Sanitizer/SDK/负例 |
| `18_阶段计划与交付门禁` | D0→F4 Closeout→F5→F6→F7→F8–11→H1–6 |
| `19_冻结设计决策RD2` | 新旧优先级、必须/可选、禁止和必要变更审批 |
| `20_原设计迁移对照` | 原 D01–D32 和章节逐项保留、取代、重新解释 |
| `work_orders/**` | 仅文档门禁 / F4 收尾 / F5 待授权实施指令 |

## 0.5 交付判据

不能用“本地编译通过”代替：同源 Shader SPIR-V/ABI；真实 GPU 图像/CPU oracle；跨 Feature 数据共享；不依赖实例的 2D；GPU-owned 输出；脏传输背压；资源替换与退休；独立外部 SDK 插件；Legacy F01–F36 全量；稳态首方 0 heap/lazy PSO；有效性能门禁。

**所有代码示例都标明目标 API**，不允许把尚未实现的 `DataSchema`、`DataSet<T>`、`PassEncoder` 误写成已完成 F4 功能，也不允许让实施 LLM 自行通过大批假实现把文档变为“可编译”。
