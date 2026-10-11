# RD2-D0-C2 全章合订版（机械生成，只读）

来源为同目录 00–20；不独立编辑，不构成第二权威。候选仍待用户批准。


---

<!-- source: 00_README_权威总纲.md -->

# Render V2 · RD2 Data-First 统一架构：权威总纲

版本：**RD2-D0-C2 — 2026-10-11，已按用户歧义裁决修订的候选设计；仍待用户架构审查批准**。对象：`LUX-YU/lux-engine`, `codex/render-v2`，审查基线 `b9f9a909bc34ac87f00c594996201a20f0af00c8`。冻结 Legacy 来源 `a669409a289a6fa4092f21176397795b1cdb7f3e`。**本包是可评审的目标架构与阶段工作单，不是已实现的生产 API 或功能资格。**

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


---

<!-- source: 01_设计哲学与成熟引擎对照.md -->

# 01 — 架构哲学、成熟引擎对照和设计边界

## 1.1 我们实际上在优化什么

LuxEngine Render V2 不追求最大公共接口数量，也不追求将 GPU 资源变成 ECS Entity。目标是**让新增数据、Feature、Shader 的业务工作在其自身模块中封闭**；让同源 Schema、冷态组合和 Graph 编译负责跨模块关系；让 GPU 热路径面对专用连续数组/Buffer 与预绑定程序。开放是作者行为可新增，封闭是无需改动已稳定的原理与执行权威；Vulkan Native 新原语扩展需要改 Native 层，这是正常的、允许的边界。

不变的哲学：数据先于机制、算法优先于类型、值类型优先于对象、真实所有权才需要 RAII；C++20 `Concepts`/`if constexpr` 用于静态约束和一次性生成 Ops，运行时动态多态只发生于跨插件/动态类型装配边界。复杂类型必须以可检验的不可替代职责证明存在；不创建重复 Manager/Registry/Scheduler/Renderer。

## 1.2 已验证的成熟思想（不是一比一仿制）

| 参照 | 从官方内容可以确认的机制 | 借鉴 | 不照搬 |
|---|---|---|---|
| EnTT `entt::storage` / Sparse Set | 类型独立的存储池、paged sparse、可选 tombstones/pointer-stability、稠密成员索引 | `Storage<T>` 易用性、强类型读写、稀疏身份/稠密迭代 | 实体是资源必要条件；CPU packed array 直接等于 GPU ABI |
| Unreal GPU Scene + RDG | GPU 场景常驻 primitive 数据；RDG 依据 Shader parameters 推导 Pass 资源、调度、同步、transient | 持久数据与 Graph 工作分离，单一 Graph | UE RHI、Renderer层级、UObject/global component 结构 |
| Unity BatchRendererGroup | Shader 自定义 GPU 实例 Buffer Layout；Batch metadata 可寻址；替换 Buffer 后旧 Draw 仍可能引用旧空间 | 存储布局与 Draw Workset 可不同；旧 GPU backing 必须 pin 到最后使用 | 以 Mesh Instance 为唯一对象模型 |
| Bevy 0.19 | Change Lists、Sparse Mesh Uniform Upload、GPU 批次解包、GPU light clustering；RenderGraph 已按其 ECS Schedule 改造 | 增量更新、GPU 侧聚合批次、在 GPU 完成更多工作 | 双 ECS World + 第二种系统调度；无条件每帧全量同步 |

信息来源（按 2026-10-11 核对）：
- EnTT 官方 wiki：https://github.com/skypjack/entt/wiki/Entity-Component-System
- Epic RDG：https://dev.epicgames.com/documentation/unreal-engine/render-dependency-graph-in-unreal-engine
- Unity BRG GPU instance layout：https://docs.unity.com/en-us/engine/7000.0/manual/analysis/graphics-performance-profiling/in-urp/reduce-draw-calls-urp/batch-renderer-group/creating-a-renderer/batches
- Bevy 0.19：https://bevy.org/news/bevy-0-19/
- Bevy 0.18→0.19：https://bevy.org/learn/migration-guides/0-18-to-0-19/

这些资料启发具体机制，不证明 LuxEngine 性能；外部引擎的数字不得拿来替代 LuxEngine 本机实测。

## 1.3 五条功能结构定理

**定理 A（数据类型开放）**：`LightGpu`, `SpriteGpu`, `GaussianChunkGpu` 等新 `T`，只需新增同源 DataSchema、正确的生产源、必要 Shader/Feature；不允许向 `RenderGraph`、`VulkanDevice` 或 Runtime 主循环增添 `switch(TypeId)`。

**定理 B（DataSet 自治）**：DataSet 的所有者是 Runtime/Scene/View 的 Scope，不是 Feature。Feature 卸载不必删除 DataSet；Scene 结束后不允许仍有无主的逻辑 DataSet。与 Native GPU backing 的销毁和缓存复用是不同决策。

**定理 C（依赖分层）**：Feature 需求只证明可构成的 DataSet/Capability；Shader 的访问由 PassSchema 生成为实际 GraphAccess；Graph 版本规则才决定 RAW/WAR/WAW、顺序、条件、culling、共享；Native 最后使用由 Queue 提交完成证明。

**定理 D（GPU 热路径非类型擦除）**：动态类型发现、Schema 验证、Owner resolution、Descriptor location 和 Shader binary relocation 均冷态执行。GPU 在单个 Kernel 中以明确内存布局访问，必要时使用具名 `VisibleSlots` / `DrawArgs` / Chunk ranges，不需要按 ObjectKind 每像素或每线程类型分派。

**定理 E（可变实例不等于可变内存）**：RecordHandle 可作废、Generation 变更；GPU Slot 可以等待退休再复用；VkBuffer 可保留容量。这三种状态不同，不能因为 Entity 销毁立即覆盖 GPU 使用中的旧字节。

## 1.4 明确非目标

- 不在 F4/F5 建立一套与引擎 ECS 平行的第二 ECS World。
- 不让每一种新的 `T` 触发一个虚接口、全局 Storage Manager、Graph Compiler 类或 GPU Descriptor Registry。
- 不保证 `layout=auto` 能凭空推导最优 AoS/SoA、纹理压缩、Bindless 和 Shader 算法；只在**已声明约束、实际消费访问、设备能力**之内确定合法计划，未知时选择可验证的稳妥方案或拒绝。
- 不把 `Feature.requires<DataType>` 当成实际 GPU 同步；没有 PassAccess 的依赖不产生 Graph hazard。
- 不把 2D/PointCloud/3DGS 强制建模为 Mesh Instance；共享的只是有证明的子机制。
- 不借此设计重写 R2 Transport、F2 Graph、F3 Shader 或 F4 R4 RAII 的既有正确实现；先找复用位置，仅对缺失数据扩展接缝最小修改。

## 1.5 简洁的定量验收

实现新外部 `SpriteData` + Compute/Graphics Pass、两个独立 Data Owner 的单 Shader 消费、多 Scene 共享材质、GPU-authored Particle 数据时，量化：业务作者重复资源描述次数；核心源码必须修改的数量；注册/编译/稳态的冷、热操作；CPU Dirty 处理是否与 D 而非 N 接近；GPU work divergence/随机内存访问；Plugin unload 错误路径。只有这些指标成立，才可宣称“设计优雅”。


---

<!-- source: 02_模块分层与所有权.md -->

# 02 — 模块 DAG、所有权、Scope 和生命周期

## 2.1 模块依赖（箭头右边为直接或传递依赖）

```text
render_core ───────────────────────────────────────► lux-cxx
    │
    ├── render_transport ───────────────────────────► render_core
    ├── render_graph ───────────────────────────────► render_core + shader description
    └── render_data  (新增中立数据 Schema/typed CPU storage)
                └──────────────────────────────────► render_core + neutral description

engine/toolchain/shader  [existing meta generator, emitter, reflector]
        └── neutral data/pass/layout contracts

render_vulkan [R4 Foundation + F3 Shader + F4 Native Graph + 后续 Native Storage]
        └── render_graph + render_data (neutral) + Vulkan/VMA
render_runtime [仅一个 VulkanBackend/FrameDriver/Scene/View/Target]
        └── render_transport + render_data + render_graph + render_vulkan
render_vulkan/feature/public [唯一 Feature 作者级 API，概念上来自 F6]
        └── 受限 RenderRuntime/Graph/Data views，无私有 Backend owner 暴露
scene_render [一 Scene 一个 RenderSystem + Typed Projections]
        └── render_runtime + render_data + ECS/Scene 源
render_features + external plugins
        └── installed public feature/data/graph/shader contracts
```

`render_graph` 不知道 `RenderScene`, `DataStorage<T>`, Vulkan 或 ECS；Graph 只看到中立 `GraphResourceUse` / import/export / PassSchema。`render_data` 的 CPU typed storage 不拥有 `VkDevice`；真正 GPU backing 在 `render_vulkan` 内作为独立 move-only RAII owner。不要在 Core 建一个知道所有业务类型的 `RenderStorageRegistry` target。

## 2.2 所有权树（单一权威）

```text
RenderRuntime  [唯一线程/生命周期/CPU-GPU协调]
 └─ VulkanBackend  [唯一设备、实际 NativeStorage 分配、计划缓存、Retirement]
     ├─ VulkanDevice / VulkanAllocator / Queue / Retire
     ├─ RuntimeScopeDataSets             [逻辑 authority]
     ├─ RenderScene A
     │   ├─ SceneScopeDataSets           [逻辑 authority, Type+Key]
     │   ├─ FeatureInstances             [算法、许可、代码 pin；不默认拥有 data]
     │   └─ RenderViews A1..An
     │       └─ ViewScopeDataSets/Camera/History/Target references
     ├─ RenderScene B [...]              [逻辑数据独立]
     ├─ NativeStorageBackings             [具体 GPU 存储 owner/lease，与 DataSetId 绑定]
     └─ Shared Logical/Native Plan Cache + Frame Execution Slots
```

这是**概念所有权**，不规定必须新增上述每一个名称的 C++ 包装类。`RenderRuntime`/`RenderScene` 应拥有现有资源表成员而不是由四个 Manager 转发。CPU `DataSet<T>` 可以是 Scope 成员的 typed value/storage；Native backing 由 Backend GPU owner 明确所有。所谓“DataSet 拥有数据”，是逻辑 authority；所谓“NativeStorageBackings 拥有分配”，是物理 RAII authority；两者不是重复拥有同一个 Vulkan 对象。

## 2.3 身份空间

| ID | 身份键 | 生成/失效 | 是否跨 DLL/Wire |
|---|---|---|---|
| `RenderDataTypeId` | 既有 renderDataTypeId(canonical_name)；版本/兼容性单独校验 | 冷态装配 | 稳定语义 ID 可编码；Wire 合同另行验证，不把 Schema 版本拼入现有 ID |
| `DataSetId` | scope owner + RenderDataTypeId + key + generation（Runtime-local） | Scope 创建/删除 | 应转换成 owner-local route/opaque handle，不在 Wire 传裸指针 |
| `RecordHandle<T>` | DataSet 句柄 + slot + generation | insert/erase/reuse | 通过生成 typed record wire 或 opaque ID 校验 |
| `SceneCapabilityId` | 既有 sceneCapabilityId(canonical_name)；contract_version 独立验证 | Feature attach/detach | 可经版本化协议 |
| `FeatureTypeId` | Feature 注册语义 | 动态载入/卸载 | 可，经 stable ABI |
| `GraphResourceId` | Graph Definition 内位置 | 冷态编译时创建 | 不可当全局资产身份 |
| `ShaderOwnerId` / `BindingSlotKey` | 真实数据 Schema/Storage 合同的**结构性绑定槽**（scope/type/semantic/full shape），不是某个 Scene 的 DataSet 实例 | 冷态编译；相容 View/Scene 可共享 | 不与 `DataSetId` 或 Vulkan 数值 set 混用 |
| `Native backing` | 具体 VkBuffer/Image/Memory | 分配/替换/退役 | 不直接暴露给插件 |
| `SubmissionTicket` | Queue owner + submitted serial | GPU 真实提交 | 仅 native 内约定借用/Pin，不是 Wire ack |

强 ID 避免跨域误传，不因为所有底层都是 `uint64_t` 建通用 `ResourceId`。

## 2.4 Data Type != DataSet != Native backing

`LightGpu` 是类型，`{SceneA, LightGpu, "main"}` 是一份 DataSet；SceneB 的 `LightGpu` 不能被误绑定，SceneA 内的 `"preview"` 也可以有不同灯光集合。单个 `DataSetId` 的 Native buffer 可经过数次 resize/replacement；在途 Frame 仍引用旧 buffer 时保留其 native lease。DataSet 可以仍有效却暂时 GPU 未 Ready；GPU buffer 可以被 Cache 保留容量，但逻辑 DataSet 已经不存在；不允许“保留容量”让失效对象自动重生。

对真正跨 Scene 共享的 Mesh/Texture，CPU Asset 的 semantic retain 仍归原 `RenderResourceDomain`；Native backing 归 Backend 的物理资源 Owner，并由共享 Asset identity & retain 同步引用。两个 Scene 有各自 Instance/Visibility/History，不因为资产相同共享场景状态。`RenderResourceDomain` 不许同时成为 Vulkan VMA 的第二 owner。

## 2.5 生命周期与引用失效

1. **Record erase**：停止产生新引用；增加 slot generation；修复 CPU dense-active；产生 GPU-visible active-index/tombstone 更新；旧 slot/backing 直到已提交访问完成之前不得重新放入可被 GPU 写入的可复用位置。
2. **DataSet resize**：冷态构造完整新 backing，上传保留记录、更新描述符/物理 slot map；在安全发布点交换；旧 Backing + Descriptor + Cache entries 等 GPU last-use 才退休。不能因为 `Buffer::create()` 成功就立即销毁旧。
3. **Feature detach**：撤销它的读写许可/新 Pass，组合候选 Plan 成功后发布，等待 CPU callback/code pin/native last use；**不自动删除仍有 scope/其他 consumer 的 DataSet**。若是 Feature 自动提供的私有 DataSet，仍需显式 Scope ownership/retention policy。
4. **Scope destruction**：禁止新写者/消费者，撤销 typed views，退役旧 RenderPlans、NativeStorageBackings，等真实 GPU completion 与所有插件 CPU callbacks quiesce；无额外全局 `waitIdle` 代替正常资源退休。
5. **Device lost**：不伪造完成 ticket，不调用 GPU Native 后继续假装有效；当前 Runtime 进入终态；宿主可保留合法 CPU authority 并另建新的 Runtime，Native backing 按所属 device-loss 合同终止，不在原 Runtime 内自动恢复。

`DataSetReader<T>` 是带 Scope/Revision 的有期限 borrow；`DataSetWriter<T>` 仅通过已绑定权限和受控作用域提供，不能作为可长期逃逸的裸 `T*`。跨线程用 PROGRAM/UPLOAD，不把 span/引用存入将来执行的任务。


---

<!-- source: 03_数据描述与类型身份.md -->

# 03 — DataSchema、存储约束、同源 C++/Shader/Wire ABI

## 3.1 Data Type、CPU/Wire/GPU 表示各有合同

`RenderDataTypeId` 继续使用既有 `renderDataTypeId(canonical_name)`；canonical name 的精确字节及碰撞检查不变。Schema semantic version、Wire version、native value layout version 与可选 GPU layout fingerprint 分开验证。不得创建平行全局 `GpuTypeId`。

`DataSchema<T>` 描述语义身份、字段、允许 Scope、authority、变更模型与存储资格，不意味着分配 GPU Buffer，也不意味着创建 Entity。CPU-only T 不要求 standard-layout、trivially-copyable 或 std430；具体 CPU storage 策略只要求其真实构造、析构、移动/拷贝能力。

既有 `RenderDataDescriptor` 的 `wire_version/layout_version/size/alignment` 保持原语义，不能静默改为 GPU ABI fingerprint。后续可选 `GpuLayout<T>` 或显式 `GpuRepresentation<T>` 保存 Shader-visible 表示与布局身份；其正式 public shape 由 F6 授权与实测冻结。CPU struct、Wire bytes、GPU record 不必字节相同。

## 3.2 目标作者语法（不代表 F4 已实现）

```cpp
struct alignas(16) LUX_RENDER_DATA(
    name="lux.light.record.v1",
    default_scope=scene,
    authority=cpu,
    residency=gpu,
    indexing=stable,
    update=dirty,
    layout=auto
) LightGpu final {
    float position_radius[4];
    float color_intensity[4];
};

// CPU_only：可有 DataSet 而不为其分配 GPU。
// GPU_owned：可以只有 GPU Buffer/State，不提供虚假的 CPU get<T>()。
// chunk/image：按实际内容与访问声明绑定独立 StoragePolicy。
```

注解拼写最终以现有 `lux-cxx meta generator` 能实际解析的 C++20 grammar 为准，先用 Golden Schema Probe 锁定，**不得引入第二 Parser 或以注释中的伪注解冒充落地**。同一个作者字段列表生成：

- `DataSchema<T>` 编译期字段/版本/Scope/Authority 元数据；复用 `RenderDataTypeId` 与 canonical-name collision 检查。
- 可安装的 Type-specific C++ `Reader/Writer`, `RecordHandle<T>`, `GpuDataView<T>` 静态 Traits（只在真需要时实例化）。
- 仅 Shader-visible 表示生成可选 `GpuLayout<T>`/`GpuRepresentation<T>`：验证 offset/size/alignment/stride/matrix-major/std430/std140/scalar、Shader 声明与实际 SPIR-V reflection；必要时生成 CPU→GPU 打包转换。CPU-only 不生成虚假的 GPU ABI。
- 仅跨 Transport/ABI 时生成版本化 Wire Encoding/typed Operation，遵循既有 PacketValue 与 owned attachment/codec；不得把任意 C++ struct 直接 memcpy 为 Wire ABI。Wire-only 不要求 GPU Layout。
- 分别保存 Schema 兼容性、可选 Wire 编码身份、可选 GPU Layout Identity，区分语义兼容、编码兼容、Shader ABI 兼容和 physical backing compatibility。

## 3.3 两类描述：事实与请求

**编译期间不变的事实**：canonical data type, field kind, alignment/size/offset, declared update authority, CPU/GPU read/write legality, stable reference requirements, allowable scopes, numeric version, ownership/cross-plugin ABI. **需要冷态协商的请求**：初始容量、Budget、growth limit、GPU access roles、是否频繁遍历/按 slot 查找、是否数据压缩/紧凑、缓存策略、设备 caps。

`layout=auto` 仅允许在不违背已声明索引/内存访问/Shader ABI 下选择已知合法策略（例：small CPU authoritative → sparse stable slots；GPU authored → native GPU buffer; chunk → page-backed）。需要先由 DataSchema/容量/可选 GPU 数据视图固定**逻辑 StorageInterfaceShape**（供 Graph 作者与 Shader ABI），再在 Graph 知道确切 Access/usage 后确定相容的 Native StoragePlan；不能要求先分配 GPU Buffer 才能构建 Graph，也不能在晚期物理分配时更改已提交的逻辑 Shader ABI。不能在 GPU Shader 使用 `data[index]` 的情况下悄然选择改变 ABI 的 SoA；若有显式 `layout=columns`，则同源生成 Shader typed load/store accessor 并以新 layout identity 编译新的 PSO/Graph。

## 3.4 分层的目标 C++20 约束

下例是概念性目标约束，尚非当前可编译 API；最终拼写和完整 Concepts 正负例属于 F6/F7。

```cpp
template<class T>
concept DescribedData = requires { DataSchema<T>::descriptor(); };

template<class T>
concept GpuRecord = DescribedData<T> && requires {
    requires GpuLayout<T>::validated;
};

template<DescribedData T> struct RecordHandle;
template<DescribedData T> class TypedStorage;
// 具体 CPU storage 策略另约束 T 的生命周期能力。
// GPU-facing typed view/encoding 才要求 GpuRecord 或显式 GpuRepresentation。
// Wire packet 继续由既有 PacketValue、生成 wire contract 与 owned codec 验证。
```

`GpuRecord` 不是 DataSchema 或 CPU TypedStorage 的前提。CPU-only 非平凡类型可合法存储；不能因此直接进入 Wire/GPU。GPU-authority 数据不暴露伪 CPU get/patch。显式 GPU representation 可不同于 CPU T，必须由同一现有 meta-generator 生成并核对布局与转换，不引入第二反射、序列化或 Shader parser。

## 3.5 DataSet Identity 与版本兼容

一份集合标识为 `(OwnerScopeId, RenderDataTypeId, DataSetKey, generation)`。此 DataSet handle 为 Runtime-local，必须校验真实 owner 和 generation；不能跨 Runtime 直接按数值比较。`RecordHandle<T>` 必须携带 DataSet 身份或绑定到严格限定的 reader/writer，防止同 T 不同集合的 slot 碰撞。`DataSetKey` 是 Scope 内不同集合的冷态名字或稳定语义键（例如 `Light.main`/`Light.preview`），不是全局唯一静态 Store。Type 匹配校验 name collision、schema semantic major/version、field type 与 read/write contract；只有涉及 Shader-visible 表示时才另校验 stride/GPU ABI fingerprint；只有“两个 T 字节数相同”不代表类型可互换。

同名不同版本：若有**显式、可证明的**兼容/迁移事务，可以继续创建候选；否则明确拒绝，不能从注册顺序选择第一份。动态插件的 Host 复制 canonical/field 描述值，生命周期不借用插件 DLL 临时 `string_view` 或 `span`。

## 3.6 兼容三种权威来源

- `authority=cpu`：`patch/update/erase` 会产生日志/dirty；GPU 是其快照，受 Ready/Submitted/Committed Revision 限制。
- `authority=gpu`：CPU 可以通过 typed 配置命令改变结构/初值，但不能使用 `get<T>()` 假装可同步读取 GPU 当前状态；GPU Pass 输出和版本由 Graph 负责。
- `authority=external`：摄像头/视频/其他 GPU 插件等外部 owner 提供 Image/Buffer 与真实 Native 状态，需 explicit Import/Ready/Retirement；DataSet 的逻辑身份不能转移该 owner 的 VkDeviceMemory 责任。

Data authority 与写入权限不同：允许多个只读 Consumer；默认一个 active Writer authority；允许多 Writer 的情况下必须明确合并、仲裁或不同数据范围的正确 Graph 规则，禁止靠 Feature attach 顺序决定谁最后写。

## 3.7 与现有 F1/F3 的关系

现有 `StorageBuffer<T>`, `UniformBuffer<T>`, `SampledTexture` 和 `LUX_PASS_PARAMS` 完全保留其**Pass-level access** 职责。`DataSchema<T>` 提供 T 的长期语义合同，可选 `GpuLayout/GpuRepresentation` 提供 GPU 表示与 backing 约束；`PassSchema<P>` 描述本 Pass 如何读取/写入它。二者由共享生成器与冷态 LayoutPlan 合并，绝不要求作者再次手填物理 binding，也不允许仅靠反射名字把 Shader 接到错误 DataSet。


---

<!-- source: 04_类型化存储与GPU布局.md -->

# 04 — EnTT 风格 Typed Storage、StoragePlan、GPU 驻留与 Worksets

## 4.1 最小可复用算法，不是通用 ECS

CPU-only `TypedStorage<T>` 无需 GPU ABI，其合法性由 CPU 存储策略的生命周期约束决定；需要 GPU view 才应用可选 GPU Layout/Representation 合同。`TypedStorage<T>` 的 CPU 端复用数据结构算法，不拥有 FrameLoop、Feature 调度或 Vulkan Device。其状态针对 CPU-authoritative 记录：

```text
SlotTable: generation[slot], live[slot], sparse_to_dense[slot]
DenseAliveSlots: slot[]  (紧凑可遍历，仅用于 membership/candidate list)
Records: stable slot-indexed T[]  (可分页，不因 dense swap 移动)
Dirty: bitset + compact dirty list / merged ranges, revision numbers
FreeSlots/RetiredSlots: 可复用条件由 GPU completion/读者使用证明确定
```

图示是**一种 StoragePlan**，不是所有 T 机械采用的内存布局。EnTT 的 `sparse↔dense` 变换用于快速 membership/iteration；数据记录在 Shader 必须保持 Slot 稳定时可采用分离的 stable slot-indexed 页。DenseAliveSlots 删除时 swap-and-pop，只修其本身映射，记录并不迁移。部分高吞吐领域可以选择 record dense+indirection 或单独 GPU compact output，但必须在合同里说明物理位置变化、引用修正和事务。

## 4.2 稳定 Identity vs GPU 地址

- `RecordHandle<T>` 包含 DataSet owner、slot、generation；不是裸 `uint32` 跨 Frame/Scene 可复用身份。
- CPU 热查询可用 sparse→dense/valid；无业务副作用的成员移除平均 O(1)。generation wrap 必须明确最终退休该 slot 或使用更宽值，不重用导致旧句柄复活。
- Shader Normal Path **不检查字符串、类型 ID 或 CPU Generation**。预检的 `VisibleSlots` 等 Workset 存 `uint32_t` slot，shader 直接 `data[slot]`。如 GPU 长期持有跨帧悬空引用，则应专门生成 `generation` 校验或引用重新映射，且必须计入性能。
- 一个物理 backing 可能被多 Frame 读；Record erase 在 CPU 语义上立即生效，但 slot 字节的重新写入/复用需等到 GPU 最后使用后或采用不同地址的 Copy-on-Write。CPU dense indices 的写入与旧 GPU 使用也同样需要同步/版本保护。

## 4.3 `StoragePlan` 的冷态确定流程

下述 Graph/Native 路径只适用于需要 GPU 表示的集合；纯 CPU-only DataSet 只选择合法 CPU 策略，不制造 GPU shape/backing 或伪 Pass。

```text
DataSchema/type registration + Scope declarations + requests/budget + optional GPU profile/Caps
             ↓ cold Feature Composition resolves DataSet/Capability/Writer requirements
             ↓ freeze neutral StorageInterfaceShape + structural BindingSlotKey
               (GPU ABI/format/addressability/capacity guarantees; no VkBuffer allocation)
             ↓ resolved typed logical views contribute PassSchema/Graph declarations
RenderGraph logical C0–C8 (no DataSet registry lookup)
             ↓ compile NativeStoragePlan from real uses WITHOUT changing
               that already-frozen logical Shader/Graph interface
StoragePlan {logical interface shape, CPU strategy, GPU physical recipe,
             update, descriptor binding compatibility, retirement policy}
             ↓ native objects & opaque typed per-invocation views
NativeStorageBacking + TypedStorage<T> / GPU-owned state
```

**避免循环依赖**：Feature 需要先在 Graph 声明数据读写，才能决定 Native 物理 usage；而 Graph Authoring 又必须提前知道资源的合法逻辑 Byte Range/Texture Shape。因此先从 Schema/请求固定中立的 `StorageInterfaceShape` 并提供 typed **逻辑** DataView；Graph C0–C8 计算实际访问后，再在 C10 选择相容 Native 物理方案和分配 Backing；C9/C11/C12/C13 保持既有 Layout/执行配方/同步/装配职责。若请求的 SoA/Atlas/BDA 模式会改变 Shader 可见 ABI，必须在固定 Interface Shape 前通过明确已生成的变体选择，不允许物理阶段偷偷改写 C++/GLSL 结构。这是同一 `StoragePlan` 冷态流程的两个阶段，不是再建一个长期 StorageManager。

不能承诺最优策略而不提供成本数据。默认策略：

| 数据工作量 | CPU storage | Native GPU storage | Shader 工作 |
|---|---|---|---|
| Sparse CPU updates、stable slot | Sparse index + slot records + dirty | 稳定 Slot Buffer + active indices，可分页扩容 | 按 slot gather；必要 GPU compact Workset |
| 主要线性扫描、可移动 | 稠密 records + generation map | contiguous Buffer，可控 relocation | 连续数组 |
| 大 Mesh/Texture/PointCloud/Gaussian | Chunk/Page/Free range/asset ledger | Arena、分段 Buffer 或 Image Pool；按预算驻留 | chunk ranges, specialized cull/LOD |
| GPU-authored Compute/Skinning/Particle | CPU 仅 metadata/控制 | GPU Buffer + versioned/ping-pong/indirect | graph producer/consumer，不 CPU 每帧 mirror |
| View history, HZB/TAA | View 数据集的 history validity | View-scoped Images/Versioned aliases | 每 View 独立 Graph imports/read/writes |
| 本帧 scratch | 无 DataSet 必需 | Graph transient Buffer/Image + verified alias | Pass-local scratch |

不能将 `Image` 强行装入 `StorageBuffer<T>`，也不将大 Mesh Blob 拆成每 Vertex 的 Sparse Set。`DataSchema` 的结构定义与 `ImageDescription` 的图像格式/extent 语义是不同层；通过受限 Storage Profile 连接。

## 4.4 Dirty Upload / Readyness / Backpressure

`patch(handle, f)` 仅在合法 writer 队列上修改逻辑状态，将必要 slot/range 标 dirty，并增加 observed revision。发布分 `observed` / `accepted(published)` / `applied` / `GPU ready`；失败（Staging 满、Queue 容量、跨 Lane 乱序）**不清除脏状态**。同个 Record 多次连续更新允许在未 admission 时合并到最终值，但 create/remove/generation/对外副作用不得遗漏。

更新策略：低 D 时 merge contiguous byte ranges；高度离散更新可批量上传 packed `{slot,value}` 并 GPU scatter；D/N 高时 full range replace。所有阈值按设备与负载实测后确定，不能默认“Scatter 总比小 Copy 快”。`ActiveSlots` 列表修补也必须有前一帧 GPU read-overlap 的 Ready/Retire 保证。

**重要**：Persistent GPU 数据并不等于全局 Triple Buffer。对高更新数据按 fence/usage 采用 per-slot 版本、ring 或 Copy-on-Write；只读/未修改数据持久共享一个 backing。写入 GPU 的数据不能被 CPU dirty 镜像再次覆盖。

## 4.5 同类型多存储 / GPU Worksets

一 Scene 可以有 `Light.main`、`Light.preview` 两份同 T DataSet；多个 Scene 可以独立持有并复用共享 Material Asset backing。一个 `MeshDraw` T 不应成为 Universal Instance。Sprite2D、PointCloudChunk、GaussianChunk 不必绑定 Transform3D/MeshMaterial；各自声明真实字段和关联。

GPU 工作列表按**表示或 Pipeline/材质兼容性**聚合，避免在单 Shader workgroup 里对每记录动态 `switch(GeometryKind)`。`Cull hot data` 可以抽出紧凑 bounds/LOD/slot 列，而存储主表维持稳定 Handle。无必要不强制 SoA；每条额外 GPU 内存引用都要评估 Cache/Miss。透明 Canvas 按稳定 Layer/Blend 顺序处理，原子 Append 不能自动保序。

示例访问链：`gl_InstanceIndex → VisibleDrawSlots[instance] → MeshDrawRecords[slot] → geometry/material indices`。若 gather 占主要 GPU 时间，图内构造 CompactVisibleRecord 以减少多重间接；禁止借可扩展性引入 TypeId/ServiceLocator per-shader lookup。

## 4.6 用模板争取 EnTT 级作者体验

```cpp
// 目标语义 API：不是当前源码承诺，拼写需通过实际 C++20 编译/SDK 检查。
auto lights = scene.data().write<LightGpu>("main");
auto id = lights.emplace(LightGpu{...});
lights.patch(id, [](LightGpu& value) noexcept { value.color_intensity[3] = 3.5f; });
lights.erase(id);

auto r = scene.data().read<LightGpu>("main");
r.each([](RecordHandle<LightGpu> id, const LightGpu& value) noexcept { /* CPU only */ });

// 对 GPU-authority type，CPU get/patch 不提供，编译负例必须拒绝。
```

`patch` 是受限同步 callable，内部捕捉 dirty 与 revision；不返回长寿 `T&` 让作者绕过脏标记。`each` 是否暴露给一般 Feature 应以真实 CPU 消费者为前提；GPU 大规模 Cull 不允许每帧 CPU `each()` 构造 Visible Set。访问权限由 cold DataSet binding 固化，在热路径没有字符串/反射查询。写 API 不跨 Engine 与 Backend 线程，其他线程通过现有 typed transport 提交变化。

模板分层：public `DataSchema<T>`, `RecordHandle<T>`, `Reader<T>`, `Writer<T>` 和薄 `TypedStorage<T>`；底层通用稀疏 Slot/Dirty 合并/Page/retirement 作为**少数不依赖 T 的算法**，减少模板实例化和 ABI 膨胀。`std::pmr`/SOA/FreeList 优化按 profile 证据，不能先造 GenericAllocator/Manager。

## 4.7 真实内存生命周期

GPU data = {Dataset identity, active revision, native backing generation, bound resource/descriptor revision, last GPU use}. Delete/resize/detach 时：禁止新引用 → 准备完整候选 storage/descriptor → 原子 publish → 在途 Frame 继续 pin 旧 backing → 等 completion ticket → 回收 slot/arena/descriptor → 保留缓存容量或释放。Device lost 独立错误处理。不同 Owner 的 native allocation alias 必须比较实际 memory range，不能只比较 VkBuffer/VkImage handle。

**不得把 VMA 显存字节节省、CPU DenseAlive 容量和 GPU Pixel 算法吞吐误写成同一个“存储优化指标”。**


---

<!-- source: 05_通信与发布事务.md -->

# 05 — Typed 数据通信、增量发布、跨 Lane 因果与状态版本

## 5.1 完整保留原三 Lane

| Lane | 来源/消费 | 数据 | 背压语义 |
|---|---|---|---|
| `PROGRAM` | Scene/Engine owner-thread → Backend 单消费者 SPSC | 已成立的 Typed DataSet mutation、Source/Generation/Revision | 未 admission 的更新保持为待发布事实；不丢变化 |
| `CONTROL` | owner-thread SPSC + bounded replies | Scope/DataSet/Feature/Target create/destroy/reconfigure、Query、Stop | 可失败/重试的事务，reply consume 或 abandon |
| `UPLOAD` | 多生产者 → Backend bounded MPMC | Owned chunk/asset/texture/video bytes + ready requests | byte+cell budget、owned attachments/lease、对 PROGRAM 无假定先后 |

禁止第四个独立 EventBus、每 Feature 一个私有 Worker 或自己配置异步调度器。已有 `TransportValue`, `RenderOpTraits`, `PacketValue` 与 host `ReplyCell` 行为继续保留；新增 DataSchema 使用它们的类型元数据，而不创建消息类型解析框架。`std::span`/裸指针不能在异步包内逃逸，必须有 owned bytes/attachments 和适当 version。

## 5.2 Schema 与操作分开

`DataSchema<T>` 定义持久逻辑数据和可验证布局；`RenderOpTraits<UpsertLight>` 定义 PROGRAM 怎样携带此类型记录更新；`PassSchema<LightingParams>` 定义 GPU Pass 如何访问 Buffer。三个可选契约复用同一现有 generator 但职责不同：CPU-only 不要求 Wire/GPU；Wire-only 不要求 GPU；需要 Shader-visible 数据才生成并验证 GPU 表示。**不允许**仅靠相同 `sizeof(T)` 假定 POD Wire 安全；Cross DLL/Thread 使用显式版本化编码/descriptor 检查，必要时 field-wise serialize。

示例目标消息：

```cpp
struct LightDelta { DataSetRoute set; RecordHandleWire id; uint64_t revision; LightGpu value; };
struct RemoveLight { DataSetRoute set; RecordHandleWire id; uint64_t revision; };
// 都是生成的合法 PacketValue 或明确定义的 owned attachment / codec；
// 不把 CPU std::vector、引用、类型名、vtable 对象直接当作 Transport bytes。
```

## 5.3 来源与目标强身份

`RenderProjection<Mesh/Light/Camera/Sprite>` 将 ECS observer、Editor、procedural generator、Asset readiness 变成源变化事实。每 Scene 恰好一个 `RenderSystem` 是 Render 发布责任中心，其内部可组合多个 typed projections，不为每种 Feature 新增 Scene ECS System。一个 Record 的 Source key 必须保留 generation；destroy→create 即使 numeric slot 相同也不能重用旧写入。在未变化的 N 个 Source 上，steady state 不生成 N 个空包。

## 5.4 五个不同进度量

1. `observed_revision`：在生产者已观察到的 CPU 状态；
2. `admitted/published_revision`：Queue 确实接纳的最高合法数据；
3. `backend_applied_revision`：Backend 已验证并修改逻辑 DataSet；
4. `upload_ready_ticket` / `gpu_snapshot_revision`：完整 GPU backing、Index、Descriptor 已产生可用的数据版本；
5. `submission_completed`：实际 GPU 执行/读/写已完成，旧资源可退休。

`FrameSerial`、Simulation Tick、Published Revision 均不是 GPU completion 证明。`UPLOAD` 和 `PROGRAM` 间无全局 FIFO；DataSet 内按 ID、generation、revision、Ready 绑定跨 Lane causality；未 Ready 不能被 Shader 偷读。写入成功但 GPU 暂未完成时可以保留上一个合法快照或明确暂停目标，不能静默将未初始化 Buffer 伪装成成功。

## 5.5 脏增量不以 CPU Full Scan 达成

一般流程：`observe → prepare immutable delta → attempt Queue admission → accepted commit revision → backend apply→ collect dirty → GPU upload with ticket → read/execute`。失败之前不清除 dirty；多轮变更可合并最后值但不得吞掉身份/生命周期副作用。Dirty membership 通过 bitset/changed-list 插入，在构造上传区间时仅处理 D 个修改槽（排序可能为 O(D log D)）；不能每帧扫描 N 个 slot 才发现 D 个脏记录。

CPU write methods 在单 Backend owner 线程调度时允许直接 `emplace/patch/erase`，但上层 Engine/外部 Plugin 默认通过控制/数据 Lane 请求，不允许直接借用 Backend 的 `DataSet<T>&` 跨线程修改。

## 5.6 GPU-authority 的版本流

`ParticleState`、GPU Visible List、HZB/History、VSM Feedback 等可能有 GPU Pass 连续写入。其 `DataSet` 的语义有效性、布局、预算仍是 Scope-owned，但真实当前数据通过 `GraphResourceVersion + Last-use/Ready Ticket` 跟踪；不会每帧被 CPU 镜像回写。GPU readback 是独立 Query 路径：Frame/Feature 提交有界 Readback Pass，GPU 完成后把 typed result/owned output 交给 CPU；不能在 GPU Cull 完成后每帧读回 count 再逐对象发 Draw。

## 5.7 错误、回复、热卸载

未知 type/schema/route；过期 owner/generation；wrong lane；超容量；重复 writer；跨 DLL ABI mismatch；上传缺 ready；DataSet 已关闭：必须返回明确错误/transaction rejection 或保留待发布状态。取消 Reply 消费不代表撤销已经接纳的写入或 GPU 作用。诊断与请求回复分离、有界且丢失计数可观测；插件卸载前撤销路由、等待在途处理和 code pin，而不能关闭 Thread 即可毁掉 Resource。


---

<!-- source: 06_Feature组合与数据依赖.md -->

# 06 — Concrete Feature、Data Provider/Consumer、Capability 与组合事务

## 6.1 Feature 的唯一职责

一个 Feature 是具体算法/行为单元：可响应 typed events、准备 Frame Input、声明 Graph Pass、提供/需求 Scene Capability 或 DataSet。**Feature 本身不等于数据 Storage、Renderer、FrameLoop 或 Vulkan owner**。Passive 数据提供者可以没有 Pass；纯后处理 Consumer 可以没有 CPU 持久状态；新 Feature 不需要从 `IRenderFeature` 虚基类继承。

`DataType`（数据结构） ≠ `DataSet`（一份数据实例） ≠ `SceneCapability`（功能契约） ≠ `FeatureType`（行为类）。例如 `LightGpu` 是 Type；Scene A 的 `Light.main` 是 DataSet；`HasSceneLights` 是 Capability；`DeferredLightingFeature` 消费 Dataset 且声明 Compute Pass。一个 DataSet 可以独立于 Feature 保留，多 Consumer 不必拷贝一份。

## 6.2 先做数据依赖解析，再造唯一 Graph

**确定性冷态 Pipeline：**

```text
installed Builtin/Plugin Feature descriptors + Scene/View/Target configuration
       ↓ [capability requirements, data demands, writer authority]
Composition candidate: resolve {Scope, DataType, DataKey, Version, Access}
       ↓ [cold error on missing required, ambiguous writer/provider, ABI mismatch]
Type-safe **logical** DataSet slots + frozen neutral StorageInterfaceShape;
no VkBuffer required at authoring time
       ↓ [Feature declares actual per-scope GPU Pass authoring and required accesses]
RenderGraphBuilder + generated PassSchema accesses + typed producer outputs
       ↓ [unify data-derived edges with versioned pass reads/writes/after clauses]
RenderGraphDefinition → existing compileLogicalGraph [C0–C8]
       ↓ [C1 graph producers/semantics/import-export; C2 pass resource interfaces only]
       ↓ [solve NativeStoragePlan from real usage under frozen interface ABI]
C9 Layout/Shader; C10 physical backing; C11 native pass recipes;
C12 queue/sync; C13 executable assembly [existing responsibilities]
Complete candidate (data bindings, resident/temporary resources, executable plan)
       ↓ backend safe publish; old candidate continues until GPU/CPU/code safe
FrameExecutor [Scene-scoped once when proven; View-scoped per view]
```

**关键反例**：`Feature A requires<LightGpu>()` 并不代表它的 GPU Pass 必须 `after(LightFeature)`。如果 Light 被 CPU Dirty-upload，一次 `ready ticket + Graph import state` 是它需要的 GPU 依赖；如果 Light 是 GPU Compute 写入，则应通过真实 `GraphResourceVersion` 和 Pass Producer 建 RAW edge。没有实际 GPU 读写的 Feature dependency **不能**自动产生无意义的 GPU barrier。

## 6.3 数据需求合同

一项需求必须明确：

- 预期 `DataSchema<T>`、稳定 ID/ABI revision；
- DataSet Scope（Runtime/Scene/View）和 Key/Selector；
- 必需或可选、可选时明确 fallback/empty behavior；
- CPU Read/Write 或 GPU Read/Write 权限，单 writer authority/可并行区间策略；
- 使用时点（cold attach / per Frame GPU read / dataset change callback）；
- 对其他 Capability 的 required/optional；
- 是否允许复用同 Scope 的现有 DataSet，多个 Provider 冲突的确定规则。

不允许“按类型查找第一个 Storage”作为默认行为，尤其在一个 Scene 具有多个同 T Dataset 时。若作者省略 DataKey，仅当匹配的数据集恰好一份且 API 合同明确定义 Default 时才合法；否则应返回 Ambiguous，而非以注册顺序选第一份。

## 6.4 目标 C++20 Feature 模型

```cpp
class DeferredLighting final {
public:
    // 真实最终拼写须经 F6 设计/安装 SDK 编译验证。
    static constexpr auto needs = data_requirements(
        required<LightGpu>(scene_scope, "main", gpu_read),
        optional<ShadowAtlas>(scene_scope, "main", gpu_read));

    RenderResult<void> declareViewPasses(
        const ViewGraphContext& view, RenderGraphBuilder& graph) noexcept;
};

// has optional hook → if constexpr 编译期生成对应 fixed FeatureOps；
// 没有 hook 就是 nullptr，不生产 dummy manager/base method。
```

`needs` 只记录静态类型层面的需求，`declareViewPasses()` 消费 cold-bound DataView 而非每 Frame 再通过字符串查 Registry。对于可由 GPU Pass 产生的数据集，Feature 提供的 typed output slot 在 Graph 中成为一个明确可用的 `GraphValue`/Producer Version。

## 6.5 Feature 之间的 Graph 依赖如何确定

1. **数据源归属**：同 DataSet 的生产者在组合阶段唯一确定（或显式 merge policy），不能隐式选择最近安装的 Feature。
2. **访问映射**：Composition 用已解析 Typed DataView 与 Generated PassSchema 将字段降低成中立 Graph Buffer/Image range、read/write、stages 和 producers。Graph 不查 DataSet Registry；实际实例/backing 留到 Invocation 绑定。其它 Attachment/Indirect/Copy side effects 也由同源 typed params 描述。
3. **producer versions**：DataSet 如果在 Graph 内先写后读，读 Pass 要引用该写的版本或显式“latest if unambiguous”。多个可能 Writer 必须 cold-fail ambiguous，不因 `feature list` 顺序决定。
4. **ordering**：无资源依赖但有真正业务顺序（透明合成、特定工具覆盖层）由受限 typed order contract 表达，并经 C0–C8 cycle check；不得建 `Stage enum` 保证所有 Feature 进入固定模板。
5. **conditional**：Feature 可跳过时，消费者必须有合法上帧数据/可用 fallback 或由新组合候选替换 topology；绝不读未初始化资源。
6. **dedupe**：Scene-scope Pass 只有在真实 input scope/revisions/epochs/side effects 可证明相同才跨 View 只执行一次；Gpu Multiview 是另外的物理优化。

## 6.6 注册/装配/卸载事务

`registerType/feature → prepareAttach → validateComposition → compile candidate → publish atomic → old retain/retire`。同一 Feature 可声明多个 DataSet Output、且可以消费自己或他人的 DataSet。对于 DataSet 而非 Feature 的生命周期：解绑最后 Producer 不意味着删数据；按 Scope/Retention Policy 决定，卸载插件前 Host 必须复制 Schema 并对仍使用插件 ABI/code 的 Callbacks 保留 Code Pin。

错误：required 缺失、同 key 不兼容类型/布局、多个 Writer、循环依赖、跨 Scope 偷读、缓存错误复用、Feature 卸载后仍持有裸指针、unready backing、无授权写入；编译/attach 阶段拒绝且旧 Graph/Store 不受污染。

## 6.7 业务 Example：2D 甚至无需 Instance/Geometry

`Sprite2D` Dataset 可以直接由 Editor/Canvas Producer 维护；`SpriteCullFeature` 消费 Sprite 和 Camera2D，输出 View-scope `VisibleSpriteSlots`；`SpriteDrawFeature` 消费 GPU visible result 和 Atlas，生成 Indirect draw；`UICompositeFeature` 消费该 View 的 color 与当前 Target。没有 Mesh, 3D Transform, GeometryKind 分支，也没有独立 UI FrameLoop。Shader 物理资源来自 DataSet/PassSchema，而非 Feature 自建 Descriptor Set Authority。

## 6.8 Cold Contract Identity 与 Runtime 实例不可混同

Feature 依赖解析以 Scope+Type+Key 确定当次运行中实际 `DataSetId`；Shader/Layout 冷态建立**结构性** `BindingSlotKey` 与 DataSchema full shape，不能把具体 DataSetId 写进共享 Pipeline Layout key。若 Scene A/B 都使用 `Light.main`、相同 shader/target/layout，可以共享一份 immutable Plan，而在各自 FrameBindings 中绑定各自 Native backing；如果同一 Pass 使用 `Light.main` 与 `Light.preview`，Schema Type 虽相同，但 BindingSlotKey 不相同，必须正确分成两个资源槽。版本/ready/generation 只负责 actual invocation 及输出工作共享的合法性。


---

<!-- source: 07_RenderGraph推导与编译.md -->

# 07 — 唯一 RenderGraph：从 Data Dependency/Feature 到 Logical/Native Plan

## 7.1 Graph 只理解 GPU 值与工作，不理解业务 Data T

`RenderGraphDefinition` 仍保存 `GraphResource`（Buffer/Image 与 Import/Transient/Export）、`GraphPass`、`GraphResourceUse`、`Producer`、`Dependency`、`Scope` 与输出合同。Graph 作者看到的是 DataSet 的**结构性逻辑 DataView**（Schema、Scope、资源界面形状、BindingSlot），不要求 GPU VkBuffer 已经分配。`GraphResourceId` 是单份 Definition 局部 ID，业务 RenderDataTypeId 不进入 Graph 通用变体；不创建平行 GPU Type ID。Typed Data Views 在 Graph 编译之前的冷态 Feature Composition 期间被冷态降低成已验证 `GraphBuffer` / `GraphTexture` 及结构性 owner slot；真正 DataSet/backing/Ready 在 Invocation/Native Import 绑定。新增 Gaussian/Light/Sprite 不改 `RenderGraph` 核心算法；Native 新 GPU primitive 按真正需要扩展。

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


---

<!-- source: 08_Shader与布局自动生成.md -->

# 08 — DataSchema + PassSchema + Shader + LayoutPlan 的唯一绑定事实

## 8.1 一份类型声明，一份 Pass 行为声明

- `DataSchema<T>`：稳定数据类型/字段/CPU 存储约束、Schema 兼容性；不声明某个 Pass 的 access。GPU-facing 数据另由同源可选 GpuLayout/GpuRepresentation 描述 Shader ABI，CPU-only 不要求 GPU Layout。
- `PassSchema<P>`：该 Pass 使用哪些 typed resources、访问角色/范围/stage/fallback、Push/UBO 参数以及目标 attachment；与现有 `LUX_PASS_PARAMS/LUX_RESOURCE` 保持兼容。
- `.lglsl` / GLSL emitter：使用同源生成的可选 GPU 表示、布局声明、必要打包转换与函数，不手写第二套 `set/binding`, `struct stride`。
- `LayoutPlan`：按照真实 **DataSchema/Storage full-shape 结构性绑定合同 + pass-local** 决定 Vulkan set/binding/descriptor type/count/push/align，冷态 relocate SPIR-V 后再验证。具体 Scene/View 的 `DataSetId` 只在 Invocation 将该绑定槽实例化为 native backing，不能进入共享 PipelineLayout 的身份。

被锁定的 F1/F3 机制必须复用；禁止新 Parser、按 shader reflection subset 再造共享 Set、脚本热路径生成 shader。

## 8.2 修正原三槽 `OwnerAssignment`

F3 当前 `OwnerAssignment {scene, feature, pass_local}` 每类只有一个 `ShaderOwnerId`，不足以表达同一个 Shader 同时读取 Light / Material / Shadow 三个独立数据绑定合同。RD2 **明确分开两种身份**：

1. `BindingSlotKey/ShaderOwnerId`：冷态结构性合同 `{namespace, TypeSchemaLayout, resource semantic/key, scope class, full shape}`，供 Shader Layout/PSO 缓存，两个不同 Scene/View 只要结构兼容即可共享同一 Plan。
2. `DataSetId/NativeBackingGeneration`：运行时具体数据实例及 Vulkan allocation，在 FrameInvocation `Bindings` 中绑定到相应结构槽，**不纳入共享 Shader/PipelineLayout 的缓存身份**；需要尺寸/资源规格不同才分叉 Native ResourcePlan。

`scene/feature/pass_local` 只是类别/频率提示，不能代替两个真实不同的绑定槽。两个 Feature 读同一个 Dataset 可以共用真实 backing，而相同 Type 的 `main` 与 `preview` 可以占两个明确不同的逻辑绑定槽。

目标冷态 `BindingSlotKey`：`{BindingNamespace, TypeSchemaLayoutFingerprint, ResourceSemantic, ScopeClass, AccessRole, StageVisibility, ArrayShape}`；它**不是** `{DataSetId}`。Cold Composition 将 Pass Field 按 Type/Key 解析到这些独立槽，Frame Invocation 将槽映射到当前真实 `DataSetId` 与 backing。物理 Set 的数目必须由 LayoutPlan 按设备限制、Owner full shape 和更新频率合并，不准每个 DataSet 实例强制一个 Vulkan Set。完整 Owner Shape 来自稳定 DataSchema/Storage contract，不因某个 Shader 使用字段子集就裁剪成不兼容 set。

例子：一个 Shader 的 `LightGpu`、`MaterialGpu` 都可来自 Scene DataSet，不能因为二者落入旧 `OwnerAssignment.feature` 类别（不是 DataSet 的实际 Scope） 就重叠绑定；F6 内以两个独立的共享 Owner、相同 role、不同 canonical Data Schema 的真实 GPU oracle 验收。

## 8.3 不同数据访问形式

| 作者数据字段 | Graph role | Native descriptor / command | 访问条件 |
|---|---|---|---|
| `StorageBuffer<T>` | shader read/write + exact byte/stride | SSBO / Storage buffer；可用 StoragePlan | T GPU ABI、read/write stage, offset |
| `UniformBuffer<T>` | uniform read | UBO/dynamic offset 或受限 Push fallback | std140/ABI/alignment/limit |
| `SampledTexture` | sampled read | ImageView + Sampler | format/dimension/usage/layout/state |
| `StorageTexture` | storage read/write | storage Image | storage format/capability/actual layout |
| Vertex/Index/Indirect | explicit `VERTEX/INDEX/INDIRECT` accesses | prebound geometry/Draw Indirect ops | shader/graph roles + GPU sync |
| Attachment/Resolve/Depth | output/write/load/store/resolve | native rendering recipes | format/sample/depth/stencil/local-read |
| external GPU resource | import/ready/last-use | borrowed Buffer/Image/View with real Native backing receipt | must not outlive owner's actual allocated memory |

Native future `AccelerationStructure`/Sparse Image 是新的 Vulkan Primitive（必要时扩大 Graph primitive/PassSchema roles），不能单纯把它们冒充 `StorageBuffer<T>`。CPU 类型开放不意味着所有 Native resource classes 都不再变化。

## 8.4 编译/缓存/错误

`ShaderVariantIdentity` 包含 asset/schema bytes/stage/pipeline identity 与 device enablement；`LayoutIdentity` 包含完整 Owner full shape、实际 descriptor sets、shader fields 和 push ranges；`NativeProgramIdentity` 在此基础上加入 pass, device, driver, attachment/sample state。保持 full equality 或 hash+exact compare，缓存冷态 safe publish/last-good 回退。凡未知 GPU struct fields、std430/std140/offset/stride 不符、两个 schema 同名不兼容、Owner 冲突、Descriptor count 设备不支持、变量数量/Update-after-bind 未启用，都要在 cold build/compile 拒绝，不允许 silent dummy descriptor 或 black output。

支持更多 Shader Stage（Mesh/Task/Ray/Geometry/Tessellation）属于以后真正需求驱动的扩展：只扩展工具链 Stage Meta/Capability 和 Native Pipeline，不向每个普通 Feature 加一套 IShaderStage/RenderPath。现有 F3 只支持 VS/FS/CS 且 Descriptor Indexing 默认未启用，不能把**硬件支持**当作**当前设备已启用**。

## 8.5 支持插件的数据声明

外部插件若使用已有 `LightGpu`，由已安装的 DataSchema 直接 `require<LightGpu>(key)`；若是新 `GaussianChunkGpu`，只需插件自带源类型/Schema，Host 对 Schema value 做深拷贝和 ABI/code-pin 验证，然后通过同一 DataSet/Graph/Shader Pipeline 组合。跨 DLL 不能传 `std::vector<T>`、C++ 异常或原生 VMA pointers 作为不受控 ABI。


---

<!-- source: 09_VulkanNative与执行.md -->

# 09 — Vulkan Foundation、Native Graph、GPU 资源/同步/Queue/Recorder

## 9.1 保留已经正确的实现

R4 `VulkanInstance/Device/Allocator`、Buffer/Image/Sampler/ImageView、Staging/SubmissionQueue/RetirementQueue 等 move-only native RAII 保留；F3 已有的 LayoutPlan/PSO/Descriptor owner 保留；F4 `compileVulkanGraph` 是一个冷态自由算法、`ExecutableGraphPlan` 是复合 owner，属于正确方向。不要因 RD2 构建 `GpuResourceManager`、`VulkanFoundation2`、第二 RenderGraph 或第二 VMA Owner。

## 9.2 物理资源类别边界

- Buffer/Image/Sampler 当前是已实现的 Native primitive；Typed DataSet 使用它们作为 backing，不在 Renderer native enum 内注册每一种业务 `T`。
- DataSet 的 GPU backing 使用 `VulkanAllocator` 申请实际 Buffer/Image/Arena/Chunk；StoragePlan 指定 usage, access, alignment, capacity, shape 与 native descriptors。维持单个 Native owner + borrow/leases。
- Graph transient Buffer/Image 单独由 Native Plan/Frame Slot 负责；不能拿它当跨 Frame DataSet 的隐式 owner。
- 新的 Native primitive（如加速结构、Sparse Image、Video/RT）需要在 Vulkan-native 类型、同步/Shader 角色、DeviceCaps 上显式扩展；这是合法的局部更改，不意味着新的 Feature 基类。

## 9.3 CPU/Native Slot 与 GPU 真正完成

GPU 入场受实际 Queue Capacity 和 Submission Tickets 约束；native submit 可跨真实 graphics/compute/transfer queues + Sync2 family ownership。资源合法性检查包括 Imported backing shape/usage, VkDevice, memory overlap, current layout/stage/access, Queue owner/ready, valid descriptor, exact subrange 与身份版本。GPU 多 Queue Fallback 必须确实在单 Queue 上正确执行，不可以“只记录一个枚举”冒充。

**不同 VkImage/VkBuffer native handle 可能别名同一 VkDeviceMemory 及部分 range**，因此 Imported Alias 不可只按 handle 身份判定；根据内存绑定区间做 conservative overlapping check，若无显式别名关系则拒绝。句柄比较只是第一层。别名优化必须证明实际 queue happens-before、正确布局和访问掩码。

**Host readback**：Compute shader Storage Write / Transfer Write → Host Read 需要正确 stage/access dependency、真实 GPU completion 和必要 non-coherent invalidate。不能只认 `TRANSFER_WRITE`，也不能以 fence 完成替代 shader→host memory barrier。

## 9.4 Slot lease 和跨 View 调用

`FrameCapacity=3` 不意味着 1 个 Native Plan 只能同时服务 3 个 View；RD2/FINAL F5 目标是 immutable `NativePlan` + N bounded dynamic `InvocationSlots`（每次提交有独立 Command scratch, attachment views, descriptors/readonly snapshots, resource pins）。FrameSlot、ViewID、InvocationLeaseID 各自是不同身份，不能用 `ViewID % FIF` 猜测是否可复用。Slot 在所有参与 Queue 的 tickets 和外部 last use 完成后才可重新借用。静态 PSO/Descriptor layout/compile recipes 仍然共享，不能为每 View 克隆全套 Shader。

原 F4 当前 `ExecutableGraphPlan` 同时拥有 immutable recipes 与 per-FIF mutable slot，F5 才完成上述隔离；F4 仅需保证不堵死演进路径并关闭既有安全门禁。

## 9.5 Typed GPU operation 与 Hot Path

当前 F4 仅实际实现直接 procedural draw、dispatch、copy/readback，3D/cube/relative extent 和 vertex-stream graphics NOT_IMPLEMENTED。F8 的 GPU-driven Mesh/PointCloud 需要 Index/Vertex/Indirect/IndirectCount（受实际 device caps）并最终由一个 Native Recorder 处理。公开 `GraphicsPassContext`/`ComputePassContext`/`TransferPassContext` 是**受限的命令录制借用接口**，不能公开原生 VkDevice/Allocator 让插件绕过 Graph；也不能为自定义算法实现第二套 Native Scheduler。

编译后 hot path：`Frame bindings + precompiled recipe + bounded Slot → record+submit`，首方 C++ heap 0、无 lazy PSO/layout/Shader compile/name map、无 runtime TypeId 查找。跨 Scene/View 的 Scene shared GPU work 需要依赖证明，不自动重复运行完整 Scene graph。

## 9.6 Resize/替换/失败回滚

Native Storage 发生扩容、GPU ABI/Layout 修订、Image dimension change、Shader hot-reload：在 cold path 完整创建候选 Backing/Pipeline/Layout/Graph，校验所有 limit & authoring contracts，成功后安全 publish，旧对象按最后使用在后台由已有 Retire 队列**被动回收**。无需自建 worker；正常帧不要 `vkDeviceWaitIdle`。失败保留 old valid plan/visible snapshot，若目标已不兼容则显式错误/暂停目标而非绑定不正确旧数据。

## 9.7 F4 已知现实与不得误宣称

截至审查基线 `b9f9a909bc34`: Normal 146/146、Full ASan 146/146、真实 GPU Validation/SyncValidation 0 errors（测试路径），但 F4 **PARTIAL**：fixed binding comparable p50 相对 F3 +5.0420%，配对置信区间横跨 5%，仍未关闭；3D/cube/relative、vertex-stream 仍明确 NOT_IMPLEMENTED。早前源码审查发现应增加**负例/真实 GPU 测试以证实或排除**：直接 Compute→Host Readback barrier、imported alias physical overlap、导入后旧 ImageView/backing 的生命周期。不能把这些未经复现的静态风险描述为已经在实机触发。新 Native 设计不能修改已验收的 F2 逻辑语义来绕开它们。


---

<!-- source: 10_Runtime多Scene多ViewTarget.md -->

# 10 — 单一 Runtime、Scene/View/Target、存储作用域与帧执行

## 10.1 一个真正的执行域

终态恰好一个 `RenderRuntime`、一个 `VulkanBackend`、一个 `FrameExecutor/FrameDriver`。Engine 只有一个 Scene RenderSystem 作为发布源（每 Scene），Editor、Player、Tools 皆使用同一个 typed transport/render entry。Feature 没有自己的 FrameLoop，Canvas/Gizmo/UI 不创建独立 UI Renderer。Runtime 无 PROGRAM 变化时仍可按 Frame Pacing（10Hz simulation → 60–144Hz render）提交合法帧；FrameSerial 和 GPU completion/diagnostics 独立计量。

## 10.2 Scope 与数据使用

- **Runtime Scope**：跨多个 Scene 真正共享的资源集合（如 cooked Mesh/Texture/Material Asset 的 GPU backing cache）；但逻辑资产 retain 仍由 CPU `RenderResourceDomain` 决定。
- **Scene Scope**：Light/Transform/MeshDraw/Sprite/PointCloud/Scene settings 等不同 Scene 不能默认混同；同 Scene 多 View 可共享稳定 DataSet/Storage。
- **View Scope**：Camera/jitter/HZB/TAA/temporal feedback/per-view visible lists 等；每 View 的历史 epoch、resize/camera cut/rebase invalidation 独立。
- **Target Scope**：Swapchain/Offscreen/rendered layer/shader outputs，真实 present/format/sample/extent/resize 生命周期。
- **Graph Transient**：不等价上述永久 Scope；只管理有明确 GPU lifetime 的中间结果。

Type 具有默认/允许 Scope，DataSet 创建时决定某份实例实际所属 scope。不能把 `scope=scene` 写死为某个 C++ 单例，也不能让一个跨两 Scene 共享的 Mesh 资产错误共享 Scene visibility/cull。

## 10.3 FrameDriver 的唯一调度逻辑

1. 拿到最新已 admission 的 CONTROL、PROGRAM 与 UPLOAD，按因果 receipt 更新 Scope data；不在帧循环里扫描全部 ECS。
2. 根据 Target deadline、有效 Scene/View/RenderPath，选用已编译 plan；只有 schema/topology/native key 改变才编译候选。
3. 验证 DataSet GPU-ready snapshot 和写入冲突；制作一组有界 `FrameGraphBindings` / `NativeImportBinding`，pin NativeResourceOwner。
4. 对 Scene-scope pass 构造共享工作 key（data revisions, scope, time, side effects），相同并可证才执行一次。
5. 对每 View 构造 camera/history/target-specific invocations；可选合法 Vulkan Multiview 合并，不合法走 separate view fallback。
6. 按 Native Graph Queue admission 录制/提交；所有 Export/Present/Readback 有真实 layout, external use, completion。
7. 轮询/回收已完成的 slots/native backing/retired plugins/query，不阻塞正常帧；Device Lost 令当前 Runtime 终态失败；宿主只能按后续授权另建 Runtime，不在此引入自动恢复状态机。

`FrameClock` 的模拟/渲染分频不意味着每个 Simulation Tick 对应一帧 Render。无新业务包时仍可绘制 Camera/Editor UI; dirty-free Scene permanent buffers 不需要重新上传。

## 10.4 N View/Target 可伸缩性门槛

F5 必须实测：一 Scene 四个 View 使用相容的 Plan，**只编译一份** Shader/Native immutable plan，每 View 有独立绑定/历史/目标，四个可合法 in-flight；相同输入的 Scene Pass 确实只执行一次，View Pass 四次；一 View 切换 RenderPath 不影响其它 View 的 DataSet；不同 Target 格式/尺寸若不相容必须产生可证明的 Native key 分叉。Frame-capacity/Invocation-slot-capacity/number-of-views 三者分开，不按数组索引取巧。

## 10.5 Scene/File/Feature unload 与资源留驻

删除上游 ECS Entity 只取消对应 DataSet record/generation；DataSet 本身是否存在取决于所属 Scene/Key/retention，GPU physical allocation 可在最后 GPU 使用后保留容量以供复用。Feature detach 不自动删除 shared DataSet。删除一个 Scene 应禁止它的 Consumer、Retire 其 Scope data/backings，但不能影响另一个 Scene 共享的 Asset GPU backing。Target resize/minimize、Swapchain image acquired/present、历史 epoch、设备丢失要有真实原生证明；不能测试无窗口小例后宣称产品已可用。

## 10.6 Layout 共享与 Native Backing 的实例化

RenderPath 编译只含 `BindingSlotKey`、DataSchema ABI、GPU Device/Shader/PSO、Target compatible shape；不同 Scene/View 的实际 `DataSetId`、Record revision 与 native image/buffer handle 在同样结构时不产生新的 Pipeline Layout。GPU backing 每次由对应 FrameInvocation 的 `DataSetId→NativeResource` 绑定并提供 Ready/Last-use。若 StoragePlan 改变物理 shader layout、descriptor shape 或设备 capability 不相容，新的 Native Plan 才需 cold compile；只增长容量且相容的 buffer 可以只安全更新 Descriptor/Backing snapshot（需退休旧）。


---

<!-- source: 11_插件SDK与扩展边界.md -->

# 11 — Public Feature/Data SDK、插件、ABI 与权限

## 11.1 公开两层接口、唯一运行时目录

**C++20 作者接口**：强类型 `DataSchema<T>`, `Reader<T>/Writer<T>`, `DataSetRef<T>`, `GpuDataView<T>`, `PassSchema<P>`, `RenderGraphBuilder`, Feature Concepts/Generated Ops；只安装 `public` headers 和实际库，不让插件 include `pinclude/sinclude` 或任何 `VulkanBackend` 私有 owner。

**动态 C ABI**：`extern "C"`、`abi_version`, `struct_size`, canonical type/schema identity, opaque DataSet/Record/Feature Handles, function table, explicit allocator/copy/retain policy, callback threading/error contract。跨 DLL 不穿过 STL owning containers、异常、RTTI hash 或不稳定 `typeid(T).hash_code()`；插件 Authoring C++ wrapper 在自己 TU 静态编译，Host type registration 只在 cold boundary 擦除一次。

## 11.2 内置类型优先复用

当已有 `LightGpu`/`MaterialGpu` 或公用 Image contract满足需要时，插件 `require<T>(ScopeKey)`，不复制一份 `PluginLightGpu` 或在内部创建重复 Buffer。若缺少合法数据类型，插件声明新的 DataSchema，并由 Host 同源生成器产生可安装类型信息；Host 复制和验证 Schema，再通过相同 DataSet/Graph/Layout/Natives 的 pipeline 使用。重复 type canonical 名但 layout 或 version 不兼容必须 fail，不能悄悄覆盖。

Type registration **不等于立即创建数据集**；一个插件可以仅新增一组值的 Schema，只有实际 Scope 调用 `create<T>` 才分配逻辑 DataSet。一个 DataSet 可以在 Feature 卸载后继续存在（仅当 Host 拥有其数据与安全 layout/CPU destructor/code contract），否则由 Scope 的生命周期事务撤销。

## 11.3 真正的插件 GPU 权限

Reader/Writers 与 GPU Views 是带 Access 和 Scope 的受限借用，不暴露 `VkDevice*`。写入 Provider 默认唯一 authority，多个 Consumer 可读。对于 GPU Pass，Authoring 通过 PassSchema 声明 `READ/WRITE/READ_WRITE` 和用途，Composition 校验其权限/Owner/Scope；不能通过自由录制 `vkCmdPipelineBarrier()` 绕过 Graph 的访问证明。高级 Vulkan tier 只在得到特定 Device Capability 和审查批准时开放受限原生 primitive，仍属于唯一 Native executor。

## 11.4 动态卸载顺序不可省略

```text
revoke new routes/data accesses → validate candidate composition
→ publish replacement graph/data bindings → quiesce CPU callbacks
→ wait/retire GPU in-flight old native backing/plan/descriptor
→ retire Feature state/callback code → release plugin code pin → unload DLL
```

DataSet 的 Host-owned POD/Schema metadata 可以在插件卸载后存在，前提是其 allocator/destructor/readers/writers 都不依赖已卸载 DLL。若删除记录需要插件自定义析构或提供转换函数，则不能先卸载插件。Static `FeatureOps` 函数表绝不等于 Code Pin；任何 callback 放在组件指针上保存都要跟踪来源插件和其最后使用。

## 11.5 外部插件门禁

使用真正的 installed SDK 外部进程/编译目录（无源码 private include）实现两个 Case：

1. 插件仅消费内置 `LightGpu`，新增自己的 Shader/Pass，输出像素正确，删插件后主 Shader 继续运作；
2. 插件新增自己的 `GaussianChunkGpu` 和 DataSet，多个 Feature 共享，真实 GPU Upload/Compute/Readback，卸载/重载验证 Schema 冲突、GPU in-flight、安全退休。

完成之前不能把 F1 元数据、F3 Binding 或 F4 Native 小例称作“第三方 Feature SDK 已实现”。


---

<!-- source: 12_Legacy功能完整迁移.md -->

# 12 — Legacy 全量业务迁移清单与 Data-First 拆耦

## 12.1 绝对来源与非削减原则

V1 冻结 SHA `a669409a289a6fa4092f21176397795b1cdb7f3e` 的 **719 个源文件**不能修改。原附录 `LegacyCapabilityMatrix.csv` 精确保存 86 条：F01–F36 是全部 36 项业务功能，I01–I44 是基础设施/新增合同 44 项，H01–H06 是新高级方向 6 项。迁移可以改变实现结构和数据 owner，但**不得减少任何旧功能输出、Shader 变体、Operation 语义、性能/错误合同**。F01–F36/I01–I44 的完整端到端闭环到 F11 产品切换为止；H01–H06 的算法分别由 F11 之后的 H1–H6 验收，不能因为 mechanism fixture 通过就标记业务 PASS。

## 12.2 数据与 Feature 分开的转换表

| Legacy 实例/Owner | Data-First DataSchema/DataSet | Feature 只贡献什么 | 具体保留行为 |
|---|---|---|---|
| `InstanceResources` / `StandardMeshStack` | `TransformGpu`, `PrevTransformGpu`, `MeshDrawGpu`, `InstanceCullGpu`, CPU source→GPU stable handles，Scene Scope | Scene mesh binding/projection、相关 GPU Cull/Draw | stable generation、diff upload、visibility/LOD/fade/retire、资源关联 |
| `MeshResources` | `MeshGeometry`, `MeshSection`, `VertexPool`, `IndexPool`, Material 资产 Runtime/shared scope | Mesh Draw/Cull/Shadow 使用数据 | VBO/IBO segment reuse、LOD、mesh ready/asset retain |
| `LightResources` | `LightGpu` Scene DataSet，Shadow/Cluster metadata | Lighting/Shadow/Postprocess consumers | dir/point/spot/area、删除/插值、Cross Forward/Deferred |
| `MaterialResources` / Texture | `MaterialGpu`/TextureHandle/descriptor catalogue，global/shared Asset retain | PBR/Unlit/Stylized、MaterialGraph、Shader variants | texture flags、normal/alpha/clearcoat/sheen、preview rollback |
| GPU skinning | Pose/bone data, skin result GPU-owned Buffer | Scene-scope skin compute | 跨帧 WAR、3 FIF、View 可复用姿态结果 |
| HZB | View-scoped History DataSet Image | HZB build pass and consumers | Mip subresource version、resize/cut/rebase invalidation |
| Canvas2D Arena | `Sprite/Tile/PixelField/Image2D` 独立 Scene DataSets | Sprite/Tile/Group/Layer Draw/Composite | group、ordering、透明、Transform dirty |
| PointCloud | `PointChunk/Octree/VisibleList` + GPU-owned outputs | Simple/GPUDriven/LOD/Splatting/Transient 五套算法 | chunk create/remove、LOD、point-size、soft splat（不是完整 3DGS） |
| Terrain/Water | TerrainPage/PageHierarchy、WaterSurface DataSets | LOD/page fallback/draw、水面 composite | streaming/VRAM budget、Scene rebase |
| Robot/Editor | Trajectory / Grid / Gizmo / Pick / Highlight / UI data | 独立类型 Feature 贡献同 Graph Pass | picking generations、overlay order、Editor 独立于 Simulation 的渲染进度，仍使用唯一 FrameDriver |

**特别约束**：`MeshGeometry` 不因为 `MeshDraw` 删除就必然销毁；`Instance` 不再是所有 GPU record 的基类。`2D`、PointCloud、3DGS 可以完全没有 MeshInstance。现有 Legacy Mesh 的 geometry 资源其实部分独立，重构应保留其高效共享与可退休行为，重点拆掉 Instance 的过多职责。

## 12.3 F01–F36 的完整项目名

| 阶段 | Capability ID | 必须恢复（不得合并缩减） |
|---|---|---|
| F8 | F01–F03 | MeshStack instance/transform/retire；Forward GPU-driven；Deferred GBuffer |
| F8 | F06–F07 | StandardMaterial/Graph；Depth Prepass |
| F8 | F11–F14 | Spatial Cull；GPU pre-skinning；RenderCluster/GPU picking；View camera |
| F9 | F04–F05 | Deferred Clustered Light；Light provider |
| F9 | F08–F10 | PCF/EVSM/CSM shadow；MeshShadow GPU-driven；HZB/view history |
| F9 | F15–F20 | Tonemap/exposure；SSAO；LinearDepth；Fog；Highlight；Skybox |
| F10 | F21–F25 | Grid2D/3D；Gizmo line/triangle；Canvas2D |
| F10 | F26–F29 | Terrain；Water；Trajectory Lines；StreamingFeedback |
| F10 | F30–F36 | PointCloud Simple / GPUDriven / LOD / Soft Splatting / Transient；GPU point/octree；Scene trajectory buffer |

第三方 Plugin/UI/Engine 产品 Ixx 继续在 F5–F11 的阶段进行；冻结 CSV 包含其完整确切的 SourceFile/OperationSchema/RequiredAcceptance/EvidenceTag，**不得改写/删行/把阶段改成“已完成”**。旧 R0–R4 数据/Schema/target identity 追溯仍可用。

## 12.4 每条业务 Feature 的完成证据

记录：Legacy SourceSHA/source_path + 原 Operation schema + DataSchema/StoragePlan + CPU Projection/wire + persistent Native backing Owner/ready + Graph resource/Pass/Shader/PSO + 真实 GPU output/readback/pixel image reference + 性能/VRAM/capacity/retirement + GPU validation + actual I/V SHA。存在但只是 Test Shader，不是业务算法资格；“可扩展到某功能”不能取代原功能验收。

## 12.5 三项额外不可回归

- Legacy GPU-only storage 和 GPU-driven Cull/Compact/Indirect 路径不能退化为每帧 CPU 全量遍历/绘制。
- Legacy Canvas2D 的特有像素/Tile/Layer 数据和五 PointCloud 不能消失在“统一 GPU Object”类型里。
- Legacy 仍有 transfer_idle、skinning WAR、minimize、Clang/UBSan 等冻结已知问题，R0 的 NOT_RUN/PARTIAL 保留，不可用新架构纸面承诺改判历史测试。


---

<!-- source: 13_先进渲染兼容性.md -->

# 13 — 为 H1–H6 保留机制，不提前仿写高级画质算法

## 13.1 设计准绳

目标是现代 Raster Base 的质量与规模可达到先进引擎同等级的视觉问题覆盖，**不是**宣称复制 Unreal 私有实现。需先 F8–F11 恢复 Legacy 的 PBR、Shadow、MSAA/HDR、Postprocess、2D/Editor 等，作为高级算法可控 fallback；高级功能每项独立真实 GPU/reference oracle，不得以 Pass 名、数据 Schema 或“Shader 已能调度”宣称算法实现。

## 13.2 H1 Virtual Geometry / Nanite-like

DataSets：MeshClusterHierarchy/GeometryPageMeta/LOD Bounds、GPU-owned residency map、page request list；真实 Page backing 由 Backend 资源 owner 管理。Feature：view-dependent cluster select、HZB occlusion、indirect/mesh dispatch、page loading priority、parent coarse fallback，Camera movement 与 dynamic mesh fallback。Graph：GPU feedback/write → transfer/upload ready → next frame cluster cull → indirect draw; 不引入独立 VirtualGeometryRenderer。新 Native primitive：Mesh Shader/Task 可为 optional tier，不支持时仍有 Indexed/Indirect raster path。必须测错失 page、GPU bounds/LOD/popping、显存峰值与正确性。

## 13.3 H2 Dynamic GI/Reflection / Lumen-like

Data：SceneRepresentation/SDF/proxy/surface cache/ProbeRadiance(3D Image 或 Buffer)、View-depth/normal/motion/history。Feature：SS/Software tracing、可选 Ray Query/RT、irradiance cache invalidation、temporal/spatial denoise；这些真正算法而非 `GI bool`。DataSchema 可以声明 3D 数据结构，但 `VkAccelerationStructureKHR` 是新的 Native primitive，必须扩展 DeviceCaps/Shader/ResourceUse/retirement。兼容基础 SSAO/SSR/IBL。多 View 可按数据事实共享 Scene cache、各自持有 temporal history。

## 13.4 H3 TSR-like

Data：View jitter/current/previous Transform、Motion Vector/Reactive/Depth/Color、history validity/epoch、dynamic resolution. Graph 支持 Target-relative extent、history current/previous import、write/readback 与 resize/cut invalidation，GPU 做 reprojection/clamp/temporal solve/upscale；不能把简单 blend 叫做 TSR。需 ghosting/flicker/disocclusion/LPIPS 或指定参考测度。无高级功能时 TAA/native render-scale/FXAA 明确 fallback。

## 13.5 H4 VSM-like

Data：ShadowVirtualAddress/PageTable/AtlasResidency/DirtyRegions/Feedback/LightProjection，动态光源/对象变化影响版本与缓存。Feature 生成 page request、priority、physical atlas alloc、draw、filter、missing-page coarse fallback。GPU resident storage Page 是特定 DataSet profile，而不是被所有灯光类型要求的通用 VMA manager。保留旧 PCF/EVSM/CSM。

## 13.6 H5 Advanced Materials, Volumes, multi-view

Data：Layered BSDF closure params、volumetric grid、texture descriptors、Material/Scene-light owner dependencies。单源 Shader Schema + 多真实 DataSet Owner + LayoutPlan，物理 set 合并/descriptor indexing/device budget，不因每多一 Material/Feature 加一个固定 set 分类。Shader 材质变体 cold compile/last-good、Texture bindless 能力必须协商；3D textures 使用 Native Image capability。Volume/Fog/Atmosphere/DOF/Bloom 有各自真实 pass/quality oracle，不把“引擎支持 Shader 变体”当成功能。

## 13.7 H6 3D Gaussian Splatting / 4D 扩展

Data：GPU-owned large Gaussian payload (position, covariance/scale-rotation, color/SH, opacity)、ChunkBounds/LOD/Page、View projection、TileRanges、Sort scratch。一个 Chunk 不等于一个 Mesh Instance；Gaussian 本身不创建百万个 C++ Handle。Feature：view projection, frustum/tiling, binning/sort, front-to-back transmittance/composition、Mesh depth merge、streaming。Graph 仍只见 Buffer/Image/resource versions/queues；CPU/Shader 不按 GeometryKind 进入大 Switch。H6 完成也不代表任意 4D deformation/temporal tracking 已完成；需独立后续设计。

## 13.8 高级能力的装载隔离

三个质量档位 Compat/Standard/High 是 DeviceCaps + Feature/RenderPath + Budget 的合法组合，不代表三个独立 Renderer 或 FrameLoop。Device-supported ≠ Device-enabled：高阶 Bindless/RT/Mesh Shader/Multiview 的启用必须在 Device create 期协商，插件如果要求未启用能力只能选明确定义的 fallback 或走受控的重创建设备事务。正常用户不能因硬件不支持 High 而丢失 Legacy 的基础功能。

对每项高级特性保存 fixed scenes/seed/resolution/reference images, native GPU timings, memory budget, history invalidation, cross-view, queue topology, device fallback 的真实证据。


---

<!-- source: 14_Cpp20类型和编码纪律.md -->

# 14 — 现代 C++20 实现哲学、模板、强类型、错误与代码审查

## 14.1 真正的职责分类

| 类型种类 | 典型形态 | 责任/允许成员 | 误用 |
|---|---|---|---|
| Value / Schema | `struct` + aggregate/constexpr Generated Trait | 只存可比较 facts、强类型字段 | 简单数据转 `IRecord/Context` |
| Strong identity | StrongId/`RecordHandle<T>` | 生成/Owner/Generation 验证 | 裸 `uint32` 混合 Scene/Record/Vk IDs |
| Typed reader/writer | 轻量受限 borrow（move/短生命周期） | 已绑定 DataSet/Scope 权限，限定作用域 | Runtime 全局查找、逃逸跨线程引用 |
| Cold builder | `RenderGraphBuilder`/Schema emission | short-lived、`finish()&&` | 运行时缓存 Native owner/Graph 权威 |
| Stateless algorithm | free functions | compile/schedule/liveness/stride/dirty merge | 创造持久 `*CompilerManager` |
| GPU native owner | `class final` move-only RAII | fallible create、device/native, retire/lastUse | 两个 owner 同时释放相同 VkAllocation |
| Runtime authority | `RenderRuntime`/`RenderScene` | frame/device/scene/data scope owner | 第四 EventBus、parallel FrameLoop |
| Plugin boundary | generated fixed Ops/Versioned C ABI | pinned callbacks、opaque handles | vtable across DLL、STL owning ABI |

所有新增 public 类在提交时需说明：单一职责、成员中实际保有的状态、谁可创建/销毁、是否 thread-affine、失败模式、为什么不能用值类型/自由函数代替。对 C++ 类型命名保持语义准确：`DataSet` 是实例，`DataSchema` 是类型，`StoragePlan` 是冷态计划，`NativeBacking` 是所有权。不要将“Context”用于毫无真实作用域的数值配置。

## 14.2 模板应用标准

应该广泛使用 `template<DescribedData T>` 提供 `DataSetReader/Writer/RecordHandle/TypedStorage`、`if constexpr` 选择稳定/移动型索引算法；`consteval` 验证 Generated Schema（字段角色、GPU ABI/对齐/字段计数）；C++20 `requires` 检测可选 Feature hooks；`std::span` 表达同步 borrowed GPU upload data；`std::variant` 表达真正封闭的 Buffer/Image/Native op 家族。

**不应该**将每一条 RenderGraph Pass/Shader 执行逻辑模板化，或用 type-erased class/vtable 包装每个读写操作。大量 Template Bloat 的通用 Page/Dirty/FreeRange 算法应提炼为少量不依赖 `T` 的实现（`record_size/align`, pointer, range），在 `TypedStorage<T>` 只保留类型检查和对象生命周期。禁止为没有真实消费者的优化先引入万能 allocator/page cache/registry。`std::pmr` 用 profile 证明才加。

## 14.3 头文件和 ABI/编译标准

**只支持 C++20**；使用现有 `cxx::expected` / `RenderResult`，不能偷换成 C++23 `std::expected`, `std::mdspan`, `std::move_only_function`, `std::ranges::to`。仅 Shader-visible representation 需满足经生成器/反射核对的 GPU 布局。CPU-only T 不要求平凡复制或 standard-layout；Wire-only 遵守既有 PacketValue/codec。不凭 trivially_copyable 断言可以直接跨 DLL/Shader memcpy。复杂 Source data 可由明确 codec 转为简单 GPU records；DataSchema/TypeName/ABI 均通过同一代码生成/校验工具链。

Author API 仅 public installable headers；`DataSchema<T>` 的模板值定义必须可被真实 SDK Consumer 包含并链接，不要求包含 private generated source path。跨 DLL 所有注册描述必须由 Host 深拷贝或被明确 Code Pin，不存临时 borrowed spans。

## 14.4 代码外观正例

```cpp
// 逻辑数据插入：类型和权限在编译期确定，实质写入由合法 Owner 线程执行。
auto lights = scope.data().write<LightGpu>("main");
auto id = lights.emplace(LightGpu{...});
lights.patch(id, [](LightGpu& x) noexcept { x.color_intensity[3] = 4; });

// Feature 有需要才实现该 Hook；无钩子不生成空回调。
if constexpr (HasViewPasses<T>) {
    ops.declare_view = +[](void* p, ViewGraphContext& v, RenderGraphBuilder& g) noexcept {
        return static_cast<T*>(p)->declareViewPasses(v, g);
    };
}
```

对可以返回错误的回调，Concept 的签名应为 `RenderResult<void> noexcept`，不能将 `void noexcept` 用在会发生资源容量/组合失败的操作上而让实现方默默丢错。目标 API 的最终签名必须和生成 ops/table/SDK ABI 及错误门禁一致。

## 14.5 代码外观反例

```cpp
// 禁止：业务 Data Type 的新增要求修改中心运行时，热路径按类型名分派。
switch (dataTypeId) { case LIGHT: ...; case MESH: ...; case GAUSSIAN: ...; }

// 禁止：对每个 Feature 创建一套 GPU Manager/FrameLoop。
new LightResourceManager; new MeshResourceManager; new SpriteScheduler;

// 禁止：用 raw GPU index 作为可以跨 Scene、跨 erase 后继续工作的身份。
using RecordId = uint32_t;

// 禁止：Shader 每像素或每实例使用泛化类型跳转代替专用 Worksets。
if (object.type == MESH) ... else if (object.type == GAUSSIAN) ...
```

## 14.6 热路径、错误、构造与线程

- 已固定 capacity/topology 的 GPU Submit、Pass Binding、Typed hot update 首方 Heap Allocation = 0；允许冷态 Storage growth 并单独计量。
- 普通 new/STL OOM 继续为 fatal，不新增可恢复 ALLOCATION_FAILURE 或热路径 catch。`RenderResult` 用于可预期的 invalid type/range/owner/DeviceCaps/Capacity/Busy/Ready error；除普通堆 OOM 的既有 fatal 合同外，不可恢复的内部不变量破坏使用已有 fatal 出口（不依赖 release assert），并有独立反证测试。不能吞掉 native Vulkan result 并返回成功。
- 借用 `std::span/string_view`、`DataSetReader`, `CommandBatch` 的存活期必须明确；异步操作使用 copy+pin 和 GPU Completion，禁止逃逸。
- 构造完整候选后 publish；旧计划/Storage 继续被真正的 in-flight refs pin，不能在 failure path 先替换旧 owner。
- 单 Backend owner-thread 处理 GPU data mutation；多 Source Producer 通过 bounded Upload/Program lane 协商，不隐式加内部背景线程/任务队列。

## 14.7 针对这次新存储的专属代码审核

必须验证 1）插入/删除/generation 无误，2）CPU dense membership 与 stable Slot 语义分离，3）Shader ABI Generated 与字段校验，4）Scope/Owner/Writer 绑定不在稳态查 Map，5）GPU snapshot/resize/retire 无交叉 frame 覆写，6）首次无资源时无假创建，7）模板代码量和 public include closure 可控。新增抽象至少节省一项重复事实、删除一种非法状态、简化调用者，或提升可测 CPU/GPU 性能；仅减少 LOC 或“看起来泛型”不够。


---

<!-- source: 15_端到端API示例.md -->

# 15 — Data-First C++20 API / Shader / Graph / Plugin 贯通示例

> **全部代码是目标 API 语义草案，不是目前 F4 可运行程序。** 现有 `LUX_PASS_PARAMS`, `LUX_RESOURCE`, `RenderGraphBuilder` 名称应复用；新 `LUX_RENDER_DATA`, `DataSchema<T>`, `DataSetWriter`, `graph.read/graphicsIndirect` 等接口最终拼写需经现有 Generator、MSVC C++20、分离 CPU/Wire/GPU 合同、安装 SDK 和真实 GPU fixture 固化，不准引入第二种 Meta Parser。按下面场景最小实现，不需增加没有消费者的类。

## 15.1 E01：零 Feature 的数据类型

```cpp
struct alignas(16) LUX_RENDER_DATA(
    name="example.sprite.v1", scope=scene, authority=cpu,
    residency=gpu, indexing=stable, update=dirty, layout=auto
) SpriteGpu {
    float xywh[4];
    float uv[4];
    uint32_t atlas_index, layer, flags, reserved;
};
// 目标 Shader-visible representation 为 48 字节；实际 GPU ABI 需生成器/反射核对。
// 这不要求所有 CPU-only 类型具有此布局，也不将 CPU bytes 默认视为 Wire bytes。
```

目标 Scope Writer：

```cpp
auto sprites = scene.data().write<SpriteGpu>("canvas.main");
auto first = sprites.emplace(SpriteGpu{.xywh={0,0,40,40}, .uv={0,0,1,1},
                                      .atlas_index=0, .layer=2});
sprites.patch(first, [](SpriteGpu& s) noexcept { s.xywh[0] += 10.0f; });
sprites.erase(first); // 撤销逻辑可见性；原 GPU slot 未退休前不得覆写
```

C++ 引用/Reader/Writer 不是跨线程事件；Engine 主线程应发 `PROGRAM SpriteDelta`，Backend owner thread 接纳后执行对应操作。DataSet 不因 SpriteFeature 卸载自动删除。

## 15.2 E02：GPU Compute Cull + Indirect Draw

目标 PassParams 中资源只声明一次：

```cpp
struct LUX_PASS_PARAMS() SpriteCullParams {
    LUX_RESOURCE(role=push_constant, stages=4) uint32_t active_count;
    LUX_RESOURCE(role=read_only_storage, stages=4)
       StorageBuffer<SpriteGpu> sprites;
    LUX_RESOURCE(role=read_only_storage, stages=4)
       StorageBuffer<uint32_t> active_slots;
    LUX_RESOURCE(role=read_write_storage, stages=4)
       StorageBuffer<uint32_t> visible_slots;
    LUX_RESOURCE(role=read_write_storage, stages=4)
       StorageBuffer<uint32_t> draw_args;
};
```

对应 Compute（生成器先初始化/绑定 `SpriteGpu` 的 GPU ABI）：

```glsl
#version 450
//! lux-shader stage=compute entry=main
layout(local_size_x=128) in;
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= lux_push.p_active_count) return;
    uint slot = p_active_slots.data[i];
    SpriteGpu s = p_sprites.data[slot];
    if (!spriteVisible(s)) return;
    // args[1] 是 draw VkDrawIndirectCommand::instanceCount，
    // 且在另一个经过 Graph 声明的 Init Pass 被置为 0。
    uint dst = atomicAdd(p_draw_args.data[1], 1u);
    p_visible_slots.data[dst] = slot;
}
```

Compute→Graphics Graph 必须见 `visible_slots` STORAGE_WRITE→SHADER_READ，`draw_args` STORAGE_WRITE→INDIRECT_READ；Shader 资源来自 generated schema，`graphicsIndirect` 为受限 Native Op，不能偷偷接受未声明为 INDIRECT 的 raw Buffer。

```cpp
// 目标 composition API，临时 Graph buffers 是本帧中间结果：
auto s = graph.import(view.data().require<SpriteGpu>("canvas.main"));
auto visible = graph.buffer(visible_desc, "sprite.visible");
auto args    = graph.buffer(indirect_desc, "sprite.draw_args");
graph.clearIndirect(args, /*vertexCount=*/6); // 实际初始化是合法 Pass
// cull params 用 `s.records`, `s.activeSlots` 及 `visible`, `args`
graph.compute("sprite.cull", {...}, EExecutionScope::VIEW, cull_params);
graph.graphicsIndirect("sprite.draw", {...}, EExecutionScope::VIEW, draw_params);
```

这里只创建一个 `SpriteGpu` 类型，没有 GeometryKind、Mesh Instance 或新 RenderGraph 调度器。`std::vector<SpriteGpu>` 的 CPU 整体 memcpy 并非强制 GPU 记录布局，draw Workset 可以独立 compact。

## 15.3 E03：跨 Feature 多个独立 Owner

数据预先存在：Scene 数据集 `Light.main`、Runtime 数据集 `Material.catalog`、View 数据集 `Camera.current`，Shadow Optional 数据集由相关 provider 使用 GPU Pass 更新。

```cpp
struct LUX_PASS_PARAMS() DeferredLightingParams {
    // 结构性 Owner 依据 Schema/semantic/scope/full shape；实际 DataSetRef 留到 Invocation。
    LUX_RESOURCE(role=read_only_storage, stages=4, semantic="Light.main")
       StorageBuffer<LightGpu> lights;
    LUX_RESOURCE(role=read_only_storage, stages=4, semantic="Material.catalog")
       StorageBuffer<MaterialGpu> materials;
    LUX_RESOURCE(role=sampled_read, stages=4, semantic="GBuffer.Normal")
       SampledTexture normals;
    LUX_RESOURCE(role=storage_write, stages=4, format=rgba16f)
       StorageTexture hdr;
};
```

Feature Composition 用 `require<LightGpu>(Scene,"main")`, `require<MaterialGpu>(Runtime,"catalog")`, `require<GBufferNormal>(View,...)`, optional Shadow + explicit fallback 做冷态 resolve，然后将 Reader 的 data views 转成 PassSchema resources。Graph 自动建立 GBuffer Write→Lighting Read 的 RAW；CPU Light Dirty Upload 的数据 ready 使用 Native ticket，不伪造 Light Pass。相同字段尺寸但不同 TypeID 的材料与灯光不允许误接。当前 F3 单一 `OwnerAssignment.feature` **不支持上述多真实 Owner 合同**，未来实施必须最小更改 LayoutPlan 绑定键而非扩建固定 owner 枚举。

## 15.4 E04：GPU-authored Particle / HZB

```cpp
struct LUX_RENDER_DATA(name="example.particle.v1", authority=gpu,
                       scope=scene, residency=gpu, layout=auto)
ParticleGpu { float position[4]; float velocity[4]; };
```

CPU DataSet 记录 Shader Buffer 的 Shape/Capacity 与 Write Authority，不暴露假的实时 `get<ParticleGpu>()`；GPU `Integrate` 写 `current`, Raster 或再次 Compute 读版本，下一帧通过 Native Ticket 和合法 ping-pong/slot reuse 建因果关系。CPU 发射命令来自 PROGRAM/UPLOAD，不能每帧读回粒子数。对于 View HZB，History 合法性来自 View DataSet Epoch，不来自 UniversalHistoryManager。

## 15.5 E05：插件只需声明数据，不一定声明 Feature

插件 A 单独注册 `example.gaussian.chunk.v1` 的 `DataSchema<GaussianChunkGpu>`，Host 在 Scope 创建 DataSet。插件 B 可定义 Gaussian feature 读取它，插件 C 可另写 TileSort Feature。Type Catalogue 只注册一次 Schema，二者共享 DataSet，功能 Code Pins 独立；Type/Backing 可以在特定合法 retention 下比 Feature 存活更久。跨 DLL 传 versioned POD descriptor/opaque routes，插件 C++ wrapper 内静态 `require<GaussianChunkGpu>()`，不持有私有 Vulkan Device。

## 15.6 E06：两个 Scene、四个 View

Scene A/B 共享 `MeshGeometry` 资产 Native backing（按 Asset retain），各自 `TransformGpu/MeshDrawGpu` DataSets。Scene A 四个 View 使用同 RenderPath，共享一个 Native Plan；四份 Camera/History/Bindings/Invocation Slots，若 Skinning 姿态和数据版本相同只做一次 Scene Compute，Cull/HZB/GBuffer 各 View 运行。实时 Sim 10Hz、Render 144Hz，即使 PROGRAM 暂无数据仍可按 Target deadline 绘制。

## 15.7 E07：纯工具/2D，无 ECS Entity

一个离线 Viewer/Editor 模块直接发布 `Sprite2D`, `TileMap2D`, `GizmoLine`, `Trajectory` DataSets，不创建 MeshInstance，也不强制创建 ECS World；所有颜色/深度/UI/Target pass 进入同一个 RenderGraph 和 Native Executor。Layer/Painter Order 作为 typed constraints 而非 GPU AtomicList 的偶然输出顺序。

## 15.8 每个示例的负例

类型同 size 不同 schema；同 type 两 dataset 没给 Key；多 writer 冲突；Writer 跨 Scope/线程；GPU-only 数据 CPU `.get` 编译错误；未 Ready Shader 读取；空 History/fallback；Draw Args 未初始化/未声明 INDIRECT role；插件 unload 时仍有在途 shader/code callback；旧 record handle 在 slot reuse 后复活；两个 imported VkImages 不同 handles 但 memory overlap；CPU Buffer 写 GPU in-flight 同 Range。


---

<!-- source: 16_性能模型与可比实验.md -->

# 16 — CPU/Upload/GPU 性能模型与质量不退步门禁

## 16.1 拒绝没有证据的性能宣称

`Sparse Set` 是 CPU 身份/活跃成员的设计，不等于 GPU 内存连续。EnTT 的 `dense` 可以是分页数据，shader 的 `records[slot]` 仍可能随机访问。GPU 时间必须按 Shader/Workset 实测；数据规模与更新比例只是建立分析模型。新系统不能因为 CPU 上传变少而假定 GPU 更快，也不能因 0 heap 证明没有 O(N) 查询。

| 符号 | 意义 |
|---|---|
| N | DataSet record allocation/capacity or alive size（记录清楚采用哪个） |
| D | 在当前发布周期实际 Dirty records |
| A | 有效 active candidates |
| V | GPU Cull 后 visible work items |
| K | 需要的 GPU Draw/Dispatch/Material-compatible groups |
| S | 由于 Dirty fragmentation 而实际产生的上传 ranges/submits |

目标 CPU 增量构造 O(D)（合并排序可 O(D log D)）而非 O(N)；Sparse slot 操作平均 O(1)；真正 GPU Cull O(A) 或以 coarse window/page candidates 为界，Raster 与 V/K/材料复杂度相关；GPU Compact 排序可能 O(V log V)，需限定使用范围/算法；GPU 输运成本包含 S 个 copy 或 scatter 和同步。

## 16.2 理论量级和真实测法

100 万条、48 B 的 `SpriteGpu` 全 Record 48 MB（十进制），1% Dirty 约 0.48 MB payload；ActiveSlots 若每条 4B 则 4 MB。**这些只是理论 Bytes**，不含 metadata/Alignment/Heap/Page/Double Buffer、不代表 VkBus 带宽或 frame time。10,000 离散小更新可能比直接一段 48MB copy 更慢；只有本机测上传次数/合并范围/queue/GPU scatter/PCIe 才能选自动策略阈值。

建议测试集：10k/100k/1M records × {0,0.1,1,10,100}% dirty × {clustered,random} × {1,4 Views}；create/erase churn、slot fragmentation、partial residency、GPU-owned update、Resource Resize while 3+ FIF、DataSet A/B 两个消费者等。分别比较 Legacy current Fxx 真实 GPU oracle；只对同质量/Resolution/Shader/scene/driver 等价工作负载做前后百分比比较。

## 16.3 GPU layout：三条真实路径

| Profile | 访问 | 优势 | 风险/优化 |
|---|---|---|---|
| StableSlot + ActiveIds | `slots[i]→records[slot]` | 插入/删除/引用稳定，脏上传便宜 | Gather/cache random，必要时 GPU compact packed data |
| DenseRecords | `records[i]` | 连续 load、GPU lane 友好 | 删除搬移/handle relocate，old in-flight pins |
| ChunkArena + Worksets | `chunk→range→typed payload` | 大资源流式、LOD/VRAM budget | page residency/address indirection，需 coarse GPU culling |

Shader hot loops 禁止多次通用 hash/type lookup。2D 不强制用 GPU Cull（小数量可合法 Direct Draw）；大规模 GPU-driven 采用专用 Compute/Indirect。材质/透明 batch order 由正确结果/质量决定，不能用随便改变排序偷取性能。`StoragePlan` 的优化变化导致 Shader ABI 变化，必须编译新 Shader/PSO and run numeric oracle。

## 16.4 F4 性能门禁及阶段禁止事项

F3 锁定 `fixed_native_binding_recipe` 与 F4 的真实可比证据：F4 最终 p50 +5.0420168067%，95% CI [+3.3058%, +7.6271%]，未能证明在 5% 工程带内；p95 +4.7619%。**该 CI 跨阈值，不代表严格证明 >5% 回退，但仍未过原门禁**。不能重测直到碰巧成功或豁免。下一步应追踪真实 binary/caller/record layout/counters、保持 F3 可比 baseline，修正或提交合规设计变更。F4 已记录 hot first-party 0 allocation、无 lazy Native creation；这些成绩是保留门禁而非忽略 Binding 比较的理由。

## 16.5 测量指标与验收输出

Cold: Data schema generation、Type catalogue bind、StoragePlan compile、LayoutPlan/PSO/Graph compile、Native allocations/bytes，独立报告。Warm: CPU Projection/Queue/Dirty, Upload bytes/S/copy, `Record+Submit` p50/p95/max/cycles, shader GPU timestamps, VRAM peak, descriptors, barriers, queue waits, FPS frame pacing, image correctness, GPU Cache/indirection counters (if available)。报告 Raw samples、配对控制、构建 SHA、设备/driver、固定 Shader identity、CI；同一质量比较默认 **不可解释的 >5% 回退 STOP**，小纳秒测量有噪声时用锁定统计纪律进行判断。

可选优化目标：把纯静态 CPU 未变化成本接近 0，GPU storage allocation/recycle 可用缓存，View Shared Scene Work 按 InputKey 证明去重；并非要求未来每种高级算法都零 GPU 开销。


---

<!-- source: 17_完整验收与正确性矩阵.md -->

# 17 — 设计正确性、存储/Graph/GPU/SDK/Legacy 完整验收矩阵

## 17.1 验收分层：证明什么就写什么

| 层 | 必须给出的事实 | 不可用来代替 |
|---|---|---|
| DataSchema/Codegen | 一份源类型按实际用途生成 CPU Schema、可选 Wire/GLSL/GPU Layout；三类独立正负例与 Type Collision/ABI 检查 | 只编过普通 POD 与 `sizeof(T)` |
| TypedStorage CPU | sparse/dense/dirty/generation/erase/slot reuse、N/D 负载、强类型 invalid handle | Shader GPU 正确性 |
| StoragePlan GPU | 真 Buffer/Image allocation、usage、ABI、GPU literal readback、growth/retire、upload bytes | Vulkan 报 0 error 即 ABI 语义正确 |
| Feature Composition | two owners/one shader、required/optional、ambiguous provider、writer authority、fallback/cold errors | 仅用孤立单 Feature Pass 测试 |
| Logical Graph | exact versions/ranges/hazards/scope/share/cull/condition/cache | 没有 Runtime 的 Scene 功能 |
| Native Graph | actual Sync2 barriers/queues/alias/Import memory overlap/Direct readback/retirement | F2 逻辑 CPU test |
| Runtime | one FrameLoop/N Scene/N View/N Targets，Slot leases/camera/history, independent pacing | F4 offscreen graph fixture |
| SDK | external installed plugin、独立 codepin/unload/generation/ABI failure | same repo internal target |
| Product & Legacy | F01–F36 与所有原 Ixx/golden actual GPU/Editor/Player/Toolchain | Demo 画出来一个三角形 |
| Advanced | H1–H6 每项算法/场景/参考画像/性能/Device fallback | Pass 接口能调度新 Feature |

## 17.2 Data-first 最小强制 Golden 新门禁（新增、不改原 25 W）

| 新门禁 | 测试 | 成功判据 |
|---|---|---|
| DF01 Data ABI | CPU-only 非平凡类型、Wire-only 显式编码、LightGpu/SpriteGpu GPU 表示分别验证；GPU align/array/struct 与 GLSL/SPIRV 对账；错 schema/version/stride | 正例 GPU 数值、负例在 cold 拒绝，只有一份作者 Schema |
| DF02 Typed Storage | stable Handle/Generation, dense membership swap, 10k churn, after erase then reuse, strong compile negatives | 无旧句柄复活、O(1) 平均路径，内存无 UB |
| DF03 Dirty | N=10k–1M，D=0/0.1/1/10/100%，clustered/random/backpressure | 无修改不遍历/上传全量；失败前不清 dirty，最终 GPU 正确 |
| DF04 Same T multiple sets | 同 Scene `Light.main` 与 `Light.preview`，SceneB `main` | 显式选择、ambiguous fail、跨 Scene 无串话 |
| DF05 Data owner != Feature | 先建 DataSet，A/B Feature 读，卸载 A，更新 B，再移除 Dataset | B 不受错误销毁影响，合法 lifetime/codepin |
| DF06 Two real owner shader | `LightGpu` + `MaterialGpu` 相同角色不同 Dataset，1 shader；再以另 Scene 同结构两份 Dataset 验证重用 | LayoutPlan 只编译一份结构性 BindingSlotKey、两个真实 backing 对应正确 GPU 像素、Scene A/B 共享 PSO，无重复 Shader 字段 |
| DF07 GPU-only | Particle compute 更新/下帧消费、CPU get/patch 非法 | GPU 不经 CPU readback/ping 不失效，GPU snapshot 正确 |
| DF08 2D no Mesh | Sprite/Tile 不创建 Mesh/Instance，Cull→Indirect Draw→Color | 真实正确像素、material/layer/alpha 排序正确 |
| DF09 Memory reuse | GPU in-flight erase/reuse、resize、shared Asset、alias overlapping imports | 不覆写旧 GPU readers，无双重释放/错误别名，内存受预算限制 |
| DF10 Feature-Graph | 提供者没有 Pass、CPU upload、GPU writer、conditional fallback、producer cycles | Graph hazard 与 Data dependency 一致而非安装顺序 |
| DF11 Multi View | Scene A 4 Views + Scene B + 1 Asset | 相容 Plan 共享、View output 分离、Scene Compute 证明只运行一次 |
| DF12 External plugin | 两个 DLL，其中一个新增 GaussianChunk Schema、另一个消费 | Installed SDK 无 private includes，卸载/重载/codepin/ABI fail |
| DF13 GPU work efficiency | stable gather vs compact vs dense；不同 CPU/Graphics/Compute payload | 指标含 CPU/GPU/Upload/VRAM；防 hidden runtime TypeId switch |
| DF14 Advanced extension proof | 新 3DGS Chunk metadata/3D image/optional primitive proposal | 插件新类型不改 Graph 核心；真正新 Vulkan primitive 只改授权 native 层 |
| DF15 Editor+Video | target resize/minimize/IME as applicable、DataSet release/replacement | 不悄然改变旧缺测记录；真正产品或明确 NOT_RUN |

DF01–DF15 是**设计目标**，不声称 F4 已实现；每项按阶段可拆分子证据，但 Whole-row PASS 要所有 required categories。DF08 可能在 F8/F10 完成，但 F6/F7 应使用更小的独立消费者证明不依赖 MeshInstance。

## 17.3 正确性边界和必须拒绝的错误

- CPU Header / Schema：同名不同 Layout、跨 DLL typeid hash、数据字段嵌套裸 pointer、unknown shader stage、潜在未定义 scalar stride、`DataType<T>` 未注册、Scope/Key 不唯一；
- Storage：已删 generation、capacity/generation wrap、wrong Thread、Writer 不授权、CPU get GPU-only、GPU resize 后旧 Descriptor 悬空、free slot 过早重用、跨 Frame Read/Write overwrite；
- Graph：producer missing/ambiguous、版本选择不明确、conditional 缺 fallback、local-read unsupported、跨 Scope 偷看 View/History、incompatible native Shader layout、Target signature mismatch；
- Native：Buffer/Image memory alias range overlap、wrong queue owner, foreign/no-ready ticket、Shader write→Host read barrier 缺失、partial submit fault/Device Lost；
- Plugin：DLL 卸载后回调运行、Host 持有插件字符串 borrowed pointer、C ABI 版本/struct_size 不匹配、重复 provider。

所有 Native 负例应证明**在错误 GPU 访问以前**安全拒绝，而不是期待 validation 输出警告后算 PASS。正例真实 GPU reference（CPU numeric oracle/像素/跨 View/质量）优先于仅检测 API 返回值。

## 17.4 原 Golden 和追踪表不丢

原 `GoldenWorkloads.csv` W01–W25；原 `LegacyCapabilityMatrix.csv` F01–F36, I01–I44, H01–H06；原 `RequirementsTrace.csv` REQ01–REQ51 **原字节保留**。RD2 不通过重写原追踪 CSV 来“自动迁移成功”。另建 RD2 设计验证映射供实施使用，新 DF 行只追加新的门禁。每个 DF/F/I/H/REQ 的实现证据需包含 `SourceSHA`, `ImplementationI`, `VerificationV`, `Class(compile/cpu/gpu/perf/sdk/product)`, actual `PASS/PARTIAL/NOT_RUN`, reviewer-approved owner stage。

## 17.5 I/V 与真实依赖影响

I commit 是单个阶段生产改动；V 是**独立 clean clone**、确定的 I SHA 上执行构建/测试/验收记录，且只能新增验收报告，不允许夹带生产源修复。Report 必须记录原始错误、失败样本、CI/设备、source/binary/link closure、first-party ASan、GPU SyncValidation、全部污染保护和 Frozen Legacy 719 blob 对比。变更一个 lower layer 不能无条件复用上阶段已有资格；只允许以 CMake FileAPI/编译 deps/link/codegen hash 证明未受影响的历史证据复用。不可擅自删除旧 test 或改 benchmark workload 取得 PASS。


---

<!-- source: 18_阶段计划与交付门禁.md -->

# 18 — 当前 F4 PARTIAL 之后的有限实施顺序（不得跳阶段）

## 18.1 阶段依赖图

```text
现有：R0/R1/R2/R4; F0/F1/F2/F3 [保留历史资格]
                  │
        RD2-D0 文档设计门禁（doc only; 候选文档资格不等于用户批准架构）
                  │ explicit user review
        F4-C Correctness / Perf Closeout
                  │ F4 PASS + explicit user authorization
        F5 One Runtime / Plan sharing / Invocation slots
                  │
        F6 DataSchema + Multi-Owner Shader + Feature SDK + Plugin
                  │
        F7 Scope-owned DataSets + Typed Storage/Dirty/Resident GPU + Scene Data
                  │
        F8 Mesh/Material/GPU-driven/Camera
        F9 Lighting/Shadow/Postprocess
        F10 2D/Editor/Terrain/Water/PointCloud/Robotics
        F11 Legacy parity + real installed SDK/product cutover
                  │
        H1..H6 advanced algorithms (each independent)
```

**设计门禁 D0 与 F4-C 不可同时交付**，F4 尚 PARTIAL，不得先写 F5/F6 生产代码。D0 并不改变 F4 历史 I/V，不让此前没实现的 3D/cube/relative/vertex-stream 被标记为 hardware fallback。

## 18.2 D0：架构变更审批（现在最合适的第一张工作单）

只新增 `docs/render-v2/rd2/**` 经过检查的完整 RD2 设计文档/Schema snippets、旧 D01–D32 决策迁移表、appendix 可追溯快照 hash 与各阶段入场合同。根仓库 `final/**` 原权威暂时只读，直到用户独立批准新设计再加明确 supersession pointer/独立设计授权记录；不可因为“新设计很先进”直接修改 R1/F2/F3/F4 C++/Test/History。完成 I 后 docs-only 独立 clean-clone 验证 V，STOP。

## 18.3 F4-C：消除阻断而非大重构

先逐项用最小负例/真实 GPU oracle 判断三个审查风险：Compute→Host direct readback memory visibility、Imported Native alias memory overlap、Import A→B 旧 ImageView/backing 生命周期；再修复已证实的问题。解决固定 Binding +5.042% 原性能门禁，通过测量/代码生成/二进制归因而不是反复运行取幸运样本；明确 3D/Cube/Relative/Vertex-stream 的阶段义务，不许悄悄降为不支持设备。Scoped changes 只限现有 Native Vulkan F4 所属文件与直接测试/报告。不得趁机实现完整 TypedStorage、重写 F2 图或 F5 Runtime。完整 I→CleanV→STOP。

## 18.4 F5：Runtime 的不可变 DataSet 边界与多 View

交付真正的单 Runtime/Backend/FrameExecutor、多 Scene/View/Target/offscreen/readback/present、Frame pacing/stop/resize/lost、可证 Scene pass 共享。F5 只确定 DataSet Scope owner slots 与 Borrow/Native backing interface **边界**，用明确定义的临时固定测试输入打通 Plan/Invocation，不提前实现 F7 万能 Registry。F5 关键门禁：1 Scene 4 View 共享 1 compiled immutable plan、四份 independent GPU snapshot/slot，Scene work =1、View work=4、2 Scenes×1 shared Asset、空 PROGRAM 仍渲染；Graph/Shader 逻辑 key 真正复用且旧对象 fence-retired。必须按 FINAL 第 15 章完成其各项既定功能，不因 RD2 范围减小。

## 18.5 F6：首先冻结 Data/SDK Schema，再装配真正 Feature

由用户另行授权的 F6 可按内部子阶段交付：

- F6.a 现有 `lux-cxx` 生成链新增 `LUX_RENDER_DATA`/DataSchema/FieldABI/Trait/Type Registration，public C++20 positive/negative tests，按用途可选的 wire/GPU layout 合同；CPU-only 不强制 GPU ABI；不另造 parser。
- F6.b 多真正 Owner（DataSet + pass-local）LayoutPlan/Shader Bindings，现有 F3 code/PSO tests 以及两 Source Owner 一 shader 的 real GPU oracle；只能修改所需下层，先提供 impact/理由。
- F6.c **最小真实可用的 DataSet/Native typed binding**：Host 为插件/内置 Type 提供一个有限容量的实际 R4 Buffer/Image GPU backing与 ready/retire，使 `DataSetRef<T>`、Feature 和 Shader 有真实 GPU 出入；这是 F7 通用 TypedStorage 的最小垂直验证基底，不是假接口、无全量优化/多存储策略。
- F6.d Concrete Feature + C++20 Concepts optional ops、Scope/Capability/Data dependency composition、Versioned C ABI & Codepin、installed independent plugin compile GPU case，无 Feature base/Manager。

F6 需要最小能真实 Bind/Execute 的 Host-owned DataSet 与 GPU Buffer 以证明 SDK 并非空壳；F7 才交付 CPU typed Sparse/Dense、真正的 Dirty Upload/Resize/Paging/GPU authority/多 profile 等完整 Storage。F6 不得先用一个临时 `LegacyFeatureStorage` 之后再用 F7 的第二套结构替代；基础 Host owner 和 Type/Binding identity 必须可以直接扩展。

## 18.6 F7：真正的 Typed Storage 和数据流

最小真实消费者顺序：`LightGpu`（CPU-authority）、`SpriteGpu`（2D 非 Mesh），`ParticleGpu`（GPU-authority）、`GaussianChunkGpu`（插件新的 Chunk Metadata）。先实现少数必要 CPU algorithms (sparse slot/generation/dense active/dirty ranges) 和 NativeStorage Adapter (stable buffer, chunk, GPU-only, image profiles on demand)；按真实 Hardware/ABI/Graph access 冷态合法选择 StoragePlan。首批真实数据通过 `PROGRAM/UPLOAD` 对接，一个 Scene RenderSystem Projection，GPU ready/backpressure、resize/reuse、两个 Feature 共享、跨 Scene 资产共享、GPU-only 输出和独立 SDK correctness。其后 F8/F9 才逐步填真实 Mesh/Light 算法。

禁止在 F7 预写一个 AllTypes ECS、Universal Instance，或将所有 GPU Data 强制通过 CPU mirror；不得把 TypedStorage 的 O(changed) 测试替代 GPU Cull/Indirect 功能验收。

## 18.7 F8–H6 责任不削减

F8 Mesh/Material/GPU-driven/Animation/Camera（至少 Indexed/Indirect 正确）；F9 Light/Shadow/HZB/PP；F10 Canvas/2D/Gizmo/Terrain/Water/Five PointCloud/Robotics；F11 完成 F01–F36/I01–I44 及 Editor/Player/Toolchain/Installed SDK product cutover，保留全部 86 ID 追踪；H01–H06 不能在 F11 提前关闭，H1–H6 新图形/质量算法分别真实 GPU/quality/reference。不得从完成 F7 Typed Storage 推导 F8–F11 业务已通过。

## 18.8 实施与测试权限纪律

每阶段必须独立列 `ALLOWED/READ-ONLY/FORBIDDEN` 文件，声明实现 commit I、独立仅验证提交 V、真实 I parent、原用户脏工作区字节保护；现有 719 Legacy blob 全程冻结。发现需改变已通过 F1/F2/F3 下层真实行为先 STOP 呈报 `old design decision + source + failing minimal test + migration/perf alternative`，不能同一 LLM 自动追入下一阶段。每次用户审查 Gate PASS 后才授权下一阶段。


---

<!-- source: 19_冻结设计决策RD2.md -->

# 19 — RD2 数据与渲染架构的冻结决策、禁令与优先级

**本文件在 RD2 设计门禁获得用户批准之后生效；在此之前是提案。** 旧 FINAL D01–D32 的原始文档和 I/V 证据保留。下列标注 `KEEP/CLARIFY/REPLACE` 用于说明修订范围，不等于要求实施方立刻修改已通过的生产代码。

## 19.1 原冻结 D01–D32 迁移结果

| 原 ID | RD2 裁决 | 解释/新含义 |
|---|---|---|
| D01 | KEEP | 唯一 Runtime、VulkanBackend、FrameExecutor、多 Scene/View/Target |
| D02 | CLARIFY | RenderView 拥有 ViewScope DataSets/Camera/History，但不默认拥有完整 GraphInstance |
| D03 | KEEP | 共享 immutable plan 的真实 Shader/Device/Target/Storage ABI 身份必须等价 |
| D04 | CLARIFY | Scene pass 可共享须证明 DataSet snapshot/Revision/Scope/Side Effect 相同 |
| D05 | KEEP | Multiview 只是可选物理优化、separate fallback |
| D06 | KEEP+EXTEND | RenderDataType/SceneCapability/GraphResource 三独立域，另外 DataSetId 为 Scope/Runtime-local 实例身份、RecordHandle 受 DataSet 约束；既有 renderDataTypeId(canonical_name) 与版本字段不改 |
| D07 | KEEP | 每 Scene 单 RenderSystem、typed Projections、O(changed) 发布 |
| D08 | KEEP | PROGRAM/CONTROL SPSC + UPLOAD MPMC、有界因果、无第四事件系统 |
| D09 | KEEP | R2 回复 consume/abandon/admission 语义保持 |
| D10 | KEEP | RenderResourceDomain 只持 CPU asset semantic ledger，不成为 native VMA owner；真正 GPU 数据由 Scope-owned Dataset+Native backing 管理 |
| D11 | KEEP | 具体 Feature + Concepts/Generated Ops，无 Feature 虚基类或 FeatureManager |
| D12 | CLARIFY | capability provider 默认唯一；**DataSet 可多个相同 T，但不同 key/scope**，不从类型随便选第一份 |
| D13 | KEEP | GraphBuilder/immutable Definition/free compiler/native RAII，DataSet/Feature 解析在 Composition；不改原 C0–C8 职责或 Graph 算法 |
| D14 | EXTEND | 继续仅用既有 generator/`LUX_PASS_PARAMS`；同一 meta 增加 `DataSchema<T>`（非第二 parser） |
| D15 | KEEP+EXTEND | 自动完整 Shader/Layout/ABI，增加可选 GPU Layout/Representation↔PassSchema 对账，CPU-only/Wire-only 分别验证 |
| D16 | **REPLACE** | Shared Descriptor Shape 权威从“Feature 身份”修正为 **DataSchema/Storage full shape + 结构性 BindingSlotKey**；具体 DataSetId 仅在 invocation 映射；Feature 只借数据；物理 set 由唯一 LayoutPlan 规划 |
| D17 | KEEP | SPIR-V relocation、cold last-good candidate、真实验证与缓存 |
| D18 | KEEP | 子资源/Buffer byte range 版本化 RAW/WAR/WAW |
| D19 | KEEP+EXTEND | conditional/cull/side effect/scope 有合法 fallback；新增 DataSet Scope/Ready 契约 |
| D20 | KEEP+EXTEND | stable topology native key 还包含合法 Storage ABI/owner keys，不因 Camera/current data revision 无故重编译 |
| D21 | KEEP | 唯一 Native Compiler C9–C13；C11=Native pass execution、C12=Queue/sync，保持 FINAL 分工，不冒称产品已可用 |
| D22 | KEEP | 只有真实 native queue/memory overlap proof 才 alias，真实 noalias |
| D23 | **CLARIFY** | History 有效期归真实 ViewScope DataSet；Feature 负责产生/消费与 invalidation，**Feature 本体不默认拥有 History GPU backing**；禁 UniversalHistoryManager |
| D24 | KEEP | 复用 R4 RAII/VMA/Queue/Retirement，不建立第二层 Foundation |
| D25 | KEEP | 真实 Sync2、不同 Queue wait/ownership，single queue fallback |
| D26 | EXTEND | Plugin SDK 额外公开 DataSchema/Typed DataSet views 及按用途可选的 Wire/GPU 合同（host-owned Type descriptors），继续 versioned C ABI/code pin |
| D27 | KEEP+EXTEND | C++20 Concepts/RAII/value/free algorithms；存储算法允许充分使用模板、但不发明第二 ECS |
| D28 | KEEP | RenderResult、fail-fast 内部 invariants、GPU lifetime |
| D29 | KEEP | 固定 hot 0 first-party heap/lazy native create、cold growth 单列 |
| D30 | KEEP | frozen 719、66 旧能力、86 综合 ID、H1–H6；不可删任何业务路径 |
| D31 | KEEP+EXTEND | I/V/STOP 保留；新增 RD2 D0 docs-only 与 F4-C closeout 门禁，不可跨阶段自动推进 |
| D32 | KEEP | 无 PROGRAM 也独立 paced frame，GPU/Frame serial 不混用 |

**解释权**：若本表的某项 REPLACE/CLARIFY 与旧文档语句矛盾，在 RD2 批准后以**本表及 02–08 相应章节**为准；原文件作为历史来源/验收依据只读保留。其余保留的冻结约束仍然适用。不得把 D16 改为“一个 Feature 全部资源共用一个 owner”，那是本轮明确否决的旧模型。

## 19.2 新增 RD2 冻结决策 N01–N17

| ID | RD2 最终选择 | 必须拒绝 |
|---|---|---|
| N01 | Data First：Data Type Schema / Scope DataSet / Native GPU backing / Feature 四者分离 | Feature 默认 private owns GPU data；每 Feature 一个 Registry |
| N02 | CPU 数据索引用 C++20 `TypedStorage<T>`（CPU-only 仅按真实生命周期策略约束，不要求 GPU ABI），强类型 Handle+Generation/Sparse membership/Dense active+dirty | 引入 Universal ECS Entity、全局 `void*` store、多余虚继承 |
| N03 | Composition 先冻结中立 StorageInterfaceShape；原 C10 根据实际 Pass Access+Caps 决定相容 backing，不晚改 ABI，非法形状明确拒绝 | 无事实依据凭 `sizeof(T)` 算最优 GPU layout；每帧重规划 |
| N04 | 同 T 可以多个 Dataset；身份为 Scope+Type+Key+Generation | 一类 Type 唯一 Runtime 单例或按注册次序 first-match |
| N05 | CPU authoritative、GPU authoritative、external authoritative 各有合法不同 API | GPU-only Buffer 冒充可 CPU `.get`；CPU 每帧覆盖 GPU 结果 |
| N06 | Stable Record Handle 不是 native slot/address/dense index；物理 reuse 必须 GPU completion | CPU 删除就释放在途 GPU 字节；Slot reuse 不更 generation |
| N07 | DataSet Reader/Writer/Graph Views 类型强约束、CPU Write 只在 backend owner 合法线程 | 全局跨线程裸 `T&`、每帧 runtime type registry find |
| N08 | Feature Composition 先解析 Type/Key/Scope/Access/Capability，降低成 typed Graph 声明；原 C1 只解析图内关系，原 C2 只验证 Pass 接口，版本算法从真实访问建立依赖 | Feature requires 自动生成假的 GPU pass/barrier 或当作安装顺序 |
| N09 | DataSchema/Storage 的 Full Shape 是 Shader 的结构性 BindingSlot Owner；Runtime DataSetId 在 Invocation 映射，LayoutPlan 唯一最终 set/binding | `OwnerAssignment{scene,feature,pass}` 冒充多个真实 Owner，或把 DataSetId 纳入 PipelineLayout key 破坏多 View 共享 |
| N10 | CPU Schema、可选 Wire 编码和可选 Shader GPU representation 由同一现有 generator 生成；GPU 布局另验证 fingerprint，不改 Core descriptor 含义 | 重复手写 GLSL data struct/Descriptor set offset |
| N11 | GPU 计算按 representation/pipeline 专用 Workset 与 Kernel；无 Universal GeometryKind 热分支 | 每线程查询 DataTypeId/GeometryKind/map 才能读记录 |
| N12 | 可选择 stable slot/packed/page/chunk/SoA GPU 布局，真实 GPU ABI 和性能可验证 | 所有 Data 强制 GPU mirror EnTT packed pages、不可控 Shader divergence |
| N13 | Feature 删除只撤销算法/访问，不默认删除共享 DataSet；Scope 才决定逻辑数据 lifetime | 装卸一个 Feature 即移动/释放所有 Consumer 共享 Buffer |
| N14 | 外部插件先复用已有数据 Schema，否则注册新 T；Host 深拷贝 ABI 描述/校验版本 | 重复 PluginLight/Mesh schema、跨 DLL 不安全 C++ RTTI/STL |
| N15 | Native Primitive 扩展只改已授权 Vulkan/Shader/Graph access 能力层 | 修改 Runtime/Scene 或建立第二 Renderer 来支持 RT/Mesh Shader |
| N16 | 每个新类型须证明作者 API 简化与 Storage/GPU 性能、ABI/错误/GPU retirement | 新模板抽象仅凭“泛型/少代码”就合入 |
| N17 | F4 PARTIAL 时首先 docs-only RD2 门禁，按 Gate 授权 F4 closeout；禁止先修改 F5/F6/F7 代码 | 以架构修订为由大规模同时改多阶段生产源码 |

## 19.3 严格类属边界

- `DataTypeId` 是数据语义类型；`DataSetId` 是一份数据实例；`SceneCapabilityId` 是行为能力；`GraphResourceId` 是单图资源；`ShaderOwnerId/BindingSlotKey` 是结构性的 Shader binding contract，而 `DataSetId` 是具体 Runtime/Scene/View 实例；它们不允许合并为一个 UniversalID。
- `TypedStorage<T>` CPU Index 与 Native GPU backing 不允许双重 ownership；`RenderResourceDomain` CPU asset retain 与 Native Backing cache 不允许互相冒充。
- `Feature data needs` 的冷态解析 ≠ Shader Pass 的 actual reads/writes ≠ Sync2 Barrier ≠ GPU completion；每一步拥有唯一权威。
- `PassSchema` 的资源访问声明 ≠ `DataSchema` 的长期数据布局声明；两者由同一 generator/ABI 验证合并，作者不重复布局真相。
- `SceneScope` ≠ `RenderViewScope`；相容 4 View 共享计划 ≠ 四份不可缺少的 GPU View 工作也只做一次。

## 19.4 变更必须证明

若要变更 N01–N17：提交具体 Design Decision ID、为什么真实消费者无法被当前方案表达、Git SOURCE 路径和最小失败 tests、性能/复杂度/类型增减、GPU/GPU error & ABI、迁移方案和已完成阶段 impacted evidence；由用户批准新独立设计提交后方可实施。不能因实现方便悄悄改变“先数据、后 Feature”。


---

<!-- source: 20_原设计迁移对照.md -->

# 20 — 2026-10-10 原 FINAL 与 RD2 的明确对照（防前后自相矛盾）

## 20.1 来源与作用

来源：用户上传 `LuxEngine_RenderV2_FINAL_Architecture_2026-10-10(4).zip`，原包 00–19、合订只读版、`appendix` CSV 与 R2 historical references。其核心工作目标（719 frozen legacy、86 件能力、Single Runtime/Graph、C++20、三 Lane、I/V/STOP）保持。RD2 **真正变化的唯一主轴** 是“原 Feature/Scene 数据提供者默认掌握持久 GPU 资源”修正为“数据先定义，由 Scope-owned DataSet + Backend NativeStorage 承担状态/分配，Feature 通过契约生产/消费”。除此之外的变化必须明确归类成必要演进接缝，不属于任意推翻过去批准的 F1–F4。

## 20.2 原 00–19 逐章对应

| 原章节 | RD2 对应 | 状态 / 确切变化 |
|---|---|---|
| 00 权威总纲 | 00 | **重写**：Data-First 权威和设计审查状态；保留一个执行域/完整 Legacy |
| 01 哲学 | 01,14 | **扩展**：EnTT 式 typed storage，但热路径无动态类型系统 |
| 02 分层/所有权 | 02,03,04 | **必要替换**：逻辑 DataSet Scope owner 和 Native owner 分开；Feature 不默认拥有长期 Storage |
| 03 通信/数据事务 | 05 | **保留**三 Lane/Revision/Backpressure；增加 DataSet identity/dirty GPU snapshot |
| 04 Graph 作者模型 | 06,07 | **保留**Graph Builder、版本化算法；增加 DataDependency→Dataset resolve→GraphAccess lowering |
| 05 Shader 自动绑定 | 03,08 | **必要替换**：`OwnerAssignment.feature` 单槽不能代表多个真实 DataSet；增加可选 GPU Layout/Representation，不要求 CPU-only 满足 GPU ABI |
| 06 逻辑图编译 | 07 | **保留 F2 C0–C8**；Composition 先解析 DataSet/Feature，降低中立声明；原 C1/C2 职责与 C0–C13 编号保持，不重写依赖算法 |
| 07 Vulkan 编译同步 | 09 | **保留 R4/F3/F4**；增加 Native Storage/DataSet borrow、GPU alias/readback 验证 |
| 08 Runtime Scene/View/Target | 10 | **扩展** DataSet Scope 与 Native Invocation slot，保留单线程/plan share |
| 09 Feature/Capability/Plugin | 06,11 | **必要替换**：Feature 属于算法/权限/组合，不默认拥有数据；Capability 与 DataSet 是不同域 |
| 10 Legacy | 12 | **完整保留全部项目**；将 Instance-centered 归为 Legacy-specific，不再系统性强制 |
| 11 UE advanced | 13 | **保留 H1–H6**，为新 GPU data types 保留与 Graph 结合机制 |
| 12 C++20 哲学 | 14 | **扩展** TypedStorage templates/Concepts/Generator，禁止 Universal Registry |
| 13 错误/性能/生命周期 | 02,04,09,16 | **扩展** logical lifetime vs GPU storage reuse/revisions |
| 14 API 示例 | 15 | **重写**：从 Type Declaration→DataSet→Feature→PassShader→Graph GPU |
| 15 实施阶段 | 18 | **保持次序**，新增 D0 doc-only，F4 CLOSEOUT，明示 F6 Data schema / F7 Storage |
| 16 验收矩阵 | 17 | **保留旧 86/25/51**，另增 DF01–DF15 门禁不覆盖原证据 |
| 17 LLM 合同 | work_orders | **继承 I/V/STOP 与目录权限**，按新阶段明确范围 |
| 18 历史规范 | 00,20 | **保留原来源和已有实现资格**，RD2 不抹去老文件和报告 |
| 19 冻结决策 | 19 | **D16 替换、D23 澄清、D14/D15/D26 扩展**，其余逐项见 19.1 |

## 20.3 旧术语转新术语的系统替换准则

| 原说法 | 改成（优先） | 不允许的歧义 |
|---|---|---|
| `Feature-owned GPU state` | `Scope-owned DataSet + Backend NativeStorage; Feature authorized reader/writer` | 不允许 Feature 始终保管所有 Consumer 共享 backing |
| `MeshSceneData/InstanceResources provider` | 对应的 Scene DataSet `TransformGpu/MeshDraw/GeometryRefs` 与 Producer/Feature | 不要求所有 Scene/2D/Gaussian 是 MeshInstance |
| `Feature shared descriptor full shape` | `DataSchema/Storage full shape + stable BindingSlotKey; invocation maps actual DataSetId; Feature gets typed views` | 不用 Feature Type/enum 当共享 Set 唯一身份 |
| `Feature requires[]` | `DataRequirements` 与 `SceneCapabilities` 两套类型明确的依赖 | 不把它们作为同一个 ID 集合或自动 GPU barriers |
| `Graph external asset import` | `DataSet typed GPU view → Graph imported logical resource + backing receipt` | Graph 不因 import 持有长期数据 |
| `Type registry` | Runtime/Backend 已有冷态 descriptor catalogue 的扩展 | 不增一个每 Frame 查 name→data 的 Global Service |

## 20.4 原锁定决策仍有执行力的例子

- 三 Lane 的完整 SPSC/MPMC + Reply cell；F4 的真实 Queue/Sync2/alias；F2 的 exact hazard/cull/scoped sharing；F3 single `LUX_PASS_PARAMS`/SPIR-V remap；719 Legacy freeze；F5 N Scene/N View/N Target；F6 installed external plugin；F7 CPU asset domain；F8–F11 旧业务所有能力；H1–H6 新质量任务；C++20-only，反对虚基类/Manager；每阶段 I/V/STOP。

## 20.5 Status 保留纪律

原 CSV 中 `NOT_MIGRATED`, `DESIGN_ONLY`, `HISTORICAL_BASELINE_ONLY` 等状态属于创建之时的历史设计快照。当前 Git `F3_CAPABILITY_STATUS.md`、`F4_VERIFICATION.md` 才是该时点进度的后续证据索引；RD2 文档未改变任何既有 Status，也不声称所有 86 行 `PASS`。RD2 设计批准只是文档设计结果，不自动让 F4 `PASS`。
