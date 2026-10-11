# LuxEngine Render V2 — RD2 Data-First Architecture

**RD2-D0-C2 · 2026-10-11 · 已按用户裁决修订的设计候选，未替代仓库当前已批准的 2026-10-10 FINAL。** 以用户原 ZIP 和 `codex/render-v2` 当前 F4 PARTIAL 的实际源码/验证为基准，重写完整架构并给出独立阶段工作单。**请从 [00 权威总纲](00_README_权威总纲.md) 开始读，不能只看本 README。**

## 本轮裁决与文档资格入口

- [逐条正文勘误与哈希](RD2_D0_ERRATA.md)
- [决策迁移与 C0–C13 对照](RD2_DECISION_MIGRATION.md)
- [一致性审阅与目标 API](RD2_CONSISTENCY_AUDIT.md)
- [逐项要求与类型追踪](RD2_TRACEABILITY.md)
- [阶段准入与未完成项](RD2_IMPLEMENTATION_GATE.md)
- [来源与保护清单](RD2_PROVENANCE.md)
- [用户歧义裁决原文](work_orders/00_D0_用户歧义裁决_只读.md)

D0 独立报告在 Verification 提交中新增；文档 PASS 不激活候选架构。

## 读者入口

- [完整版合订文档](FINAL_RD2_全章合订版.md) —— 00–20 所有章节完整汇总。
- [原方案继承/替换表](20_原设计迁移对照.md) —— 关键 D16/D23/Shader owner 变更及原 00–19 逐章对账。
- [当前真实状态](18_阶段计划与交付门禁.md) —— `F4=PARTIAL`，不授权 F5。
- [文档入库工作单：立即可交实施方](work_orders/01_D0_文档架构冻结工作单.md) —— **只授权设计文档，不改生产代码**。
- [F4 收尾工作单：下一张需另行授权](work_orders/02_F4_CLOSEOUT_实施工作单_待授权.md) —— 先证伪/证实同步、Alias、Borrow 风险，并关性能门禁。
- [F5 Runtime 入场规划（未授权）](work_orders/03_F5_RUNTIME_入场工作单_未授权.md) —— 后续设计预告。
- [F6 DataSchema/Plugin 预备范围（未授权）](work_orders/04_F6_DataSchema_FeatureSDK_预备范围.md)
- [F7 Typed Storage/GPU Resident 预备范围（未授权）](work_orders/05_F7_TypedStorage_GPUResident_预备范围.md)

## 21 个完整章节

- [Render V2 · RD2 Data-First 统一架构：权威总纲](00_README_权威总纲.md)
- [01 — 架构哲学、成熟引擎对照和设计边界](01_设计哲学与成熟引擎对照.md)
- [02 — 模块 DAG、所有权、Scope 和生命周期](02_模块分层与所有权.md)
- [03 — DataSchema、存储约束、同源 C++/Shader/Wire ABI](03_数据描述与类型身份.md)
- [04 — EnTT 风格 Typed Storage、StoragePlan、GPU 驻留与 Worksets](04_类型化存储与GPU布局.md)
- [05 — Typed 数据通信、增量发布、跨 Lane 因果与状态版本](05_通信与发布事务.md)
- [06 — Concrete Feature、Data Provider/Consumer、Capability 与组合事务](06_Feature组合与数据依赖.md)
- [07 — 唯一 RenderGraph：从 Data Dependency/Feature 到 Logical/Native Plan](07_RenderGraph推导与编译.md)
- [08 — DataSchema + PassSchema + Shader + LayoutPlan 的唯一绑定事实](08_Shader与布局自动生成.md)
- [09 — Vulkan Foundation、Native Graph、GPU 资源/同步/Queue/Recorder](09_VulkanNative与执行.md)
- [10 — 单一 Runtime、Scene/View/Target、存储作用域与帧执行](10_Runtime多Scene多ViewTarget.md)
- [11 — Public Feature/Data SDK、插件、ABI 与权限](11_插件SDK与扩展边界.md)
- [12 — Legacy 全量业务迁移清单与 Data-First 拆耦](12_Legacy功能完整迁移.md)
- [13 — 为 H1–H6 保留机制，不提前仿写高级画质算法](13_先进渲染兼容性.md)
- [14 — 现代 C++20 实现哲学、模板、强类型、错误与代码审查](14_Cpp20类型和编码纪律.md)
- [15 — Data-First C++20 API / Shader / Graph / Plugin 贯通示例](15_端到端API示例.md)
- [16 — CPU/Upload/GPU 性能模型与质量不退步门禁](16_性能模型与可比实验.md)
- [17 — 设计正确性、存储/Graph/GPU/SDK/Legacy 完整验收矩阵](17_完整验收与正确性矩阵.md)
- [18 — 当前 F4 PARTIAL 之后的有限实施顺序（不得跳阶段）](18_阶段计划与交付门禁.md)
- [19 — RD2 数据与渲染架构的冻结决策、禁令与优先级](19_冻结设计决策RD2.md)
- [20 — 2026-10-10 原 FINAL 与 RD2 的明确对照（防前后自相矛盾）](20_原设计迁移对照.md)

## 历史来源、审查限制

- 冻结 Legacy 来源：`a669409a289a6fa4092f21176397795b1cdb7f3e`，719 files byte-unchanged。RD2 ZIP 没有重新打包 719 源码，只提供完整设计和需求矩阵。
- 远端审查基线：`b9f9a909bc34ac87f00c594996201a20f0af00c8`；F4 old I `bd5399063907087e147ee159f24e1da6a87ab30f`。实际使用时必须重新读取远端 HEAD。
- 原四份 CSV 在 `appendix/` **原字节复制**（86 capability、25 workloads、51 requirements、旧 TypeOwnership），是原设计时刻的快照，不是当前已完成状态。不在旧 CSV 填写新的 PASS 或修改旧验证。
- 完成 D0 并经用户审查批准后，RD2 才可成为 Git 层的新目标权威；真实 source/API/Shader 的改变必须按独立阶段 I/V 重新验证。
- 本包 API 源码是 C++20 **目标语义示例**，其中 `LUX_RENDER_DATA`, `DataSet<T>`, `graphicsIndirect` 等不代表 F4 已实现；它们需要以后真实 generator/SDK/GPU 资格。
