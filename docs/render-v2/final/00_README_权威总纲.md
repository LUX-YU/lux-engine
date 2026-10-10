# 00 — LuxEngine Render V2：最终架构与实施入口（F1）

> **状态：FINAL DESIGN / IMPLEMENTATION CONTRACT。** 日期：2026-10-10。用户要求本文件集作为直接交付实施方的目标规范。**本压缩包没有自动修改 GitHub 仓库，也不表示新 API 已实现。** 文档入库/实施按 §00.7 与第 17 章执行。
>
> 基线仓库：`LUX-YU/lux-engine`，远端 `codex/render-v2` 设计审阅 HEAD `7de3ddaa3f7614995745b41d73dfc98302310a4c`（R4 verification）。实施时必须首先读取实时远端 HEAD，若已前进则记录新提交并比较，不得盲目 reset 或覆盖。
>
> 冻结 Legacy 原始 SHA：`a669409a289a6fa4092f21176397795b1cdb7f3e`；719 个旧 Render 文件（加 archive guard）不得改写。R1 Core、R2/R2-FIX Transport、R3 Logical Graph、R4 Vulkan Foundation 的历史资格判定保持不变。**R3 API/实现准许按本设计重写；不能为 API 兼容保留永久并行 Graph 系统。** 已验收 Core/Transport/R4 不得擅自修改。

## 00.1 一句话终态

**一个 Runtime、一个 VulkanBackend、一个完整可编译/执行的 RenderGraph 机制、多个 Scene、每 Scene 多 View；SceneData 持久化，View 是观察者而非 Graph 的默认 owner；同渲染路径的 View 共享编译计划，共享可证明的 Scene 工作，View-local 工作分别或经 Multiview 合并执行。** Engine/Editor/ECS/外部插件经 typed RenderData + CONTROL/PROGRAM/UPLOAD 发布，Shader/Graph 由同一 PassParams 作者契约生成，稳定帧无隐式 Graph rebuild、Shader 编译、Pipeline/Layout 创建或基础设施堆分配。

```text
World / Editor / Asset / Procedural / Plugin Sources
      │                    │
      │ O(changed)         │ shared upload/asset demands
      ▼                    ▼
Scene RenderSystem       RenderResourceDomain (per Runtime integration domain)
(one per Scene)          semantic asset/receipt/retain ledger, NO Vulkan backing owner
      ├── typed RenderProjection / data source
      ├── PROGRAM  owner-thread SPSC     (state deltas)
      ├── CONTROL  owner-thread SPSC     (lifecycle/query/reconfiguration)
      └── UPLOAD   bounded MPMC          (large immutable payload/asset)
                     │
                     ▼
RenderRuntime — one independent backend execution domain, stop/error/reply/pacing
                     │
                     ▼
VulkanBackend — R4 Vulkan native owners + shared compile/exec mechanisms
      ├── SceneRenderer  → N RenderScenes (persistent SceneData/Capabilities/Features)
      │                         └→ N RenderViews (camera/output/history/path-choice)
      ├── Graph Authoring (cold) → Definition → LogicalGraphPlan
      ├── Shader Interface/Descriptor LayoutPlan → Vulkan ExecutableGraphPlan
      ├── Backend-scope compiled-plan cache (not per-View graph by default)
      └── FrameExecutor + VulkanFrameDriver
           ├── shared scene work once per proven dependency/invocation
           ├── view-local work: separate or compatible Vulkan Multiview
           ├── cross-target composition/readback/present
           └── fence-proven lifetime retirement
```

## 00.2 共同哲学与权威术语

- **事实与机制分离**：plain value/强类型 ID 表示数据；普通函数/无状态算法做转换；Builder 只负责暂态声明；RAII class 才拥有资源与生命周期。`Manager/Context/Controller` 名称本身不能证明职责。
- **一份事实只有一个 authority**：Source ECS、Scene-side asset ledger、Backend SceneData、native GPU backing、Graph transient plan 分别仅有一个合法 owner。非拥有索引不产生第二份真相。
- **三种依赖不能混同**：`RenderDataTypeId` = 传输数据型别；`SceneCapabilityId` = 持久 provider 的依赖；`GraphResourceId` = 图内部资源访问/版本。强 ID 之间不得隐式转换。
- **三条时间轴不同**：Simulation tick/revision、Publication revision、Render frame serial/time。任何一步都不强制与另一步一一对应。Render 无 PROGRAM 仍能合法持续绘制最后已提交状态，禁止 busy-spin。
- **Graph 是完整的声明→编译→执行系统**：不能只实现排序；需要 typed PassParams、shader reflection/LayoutPlan、子资源 hazards、schedule/culling、物理资源、barriers、queue sync、native execution、调试诊断。
- **View 是观察者**：不因两个相机不同就建两份图。允许不同 RenderPath；Graph cache 默认 Backend scope，View history/target 仍私有。结果共享必须有数据依赖证明，不能单凭 Pass 名称。
- **Feature 是 concrete composition**：Provider 可以没有 Pass；Effect 可以读多个 Capability；Compute 负责 GPU 工作；类型可组合但不创建一套继承树。内置与外部使用同一公开作者 API。
- **现代 C++20 是设计约束**：Concepts、`if constexpr`、值语义、`std::span` 仅作短 borrow、生成 metadata、零 hot dynamic-dispatch churn。不得暗中使用 C++23 的 `std::expected/std::move_only_function`；继续使用 `cxx::expected`。
- **Legacy 功能等价是不可裁剪项**：Forward/Deferred/PBR、Shadow PCF/EVSM/CSM、HZB/Cluster、Skinning、RenderCluster、五种 PointCloud 策略、Terrain/Water、Canvas2D、Picking、UI/Editor/外部 Feature 等详见第 10 章及 CSV，不是仅支持某个基类。
- **UE 级画质目标需真实算法**：Nanite-like、Lumen-like、TSR-like、VSM-like、3D Gaussian Splatting 与多表示混合 等须独立实现对应可验证 GPU 算法；图“能表达”不等于实现这些算法。见第 11 章。

## 00.3 规范的层级与冲突优先级

1. 本最终文档集 `00–19` 与附录矩阵为**同一个版本的权威目标**；相同概念首先采用第 19 章的冻结决策，再读对应负责章节。
2. `17_LLM实施合同` 的交付、变更与 STOP 门禁，和 `12_Cpp20`、`13_性能生命周期` 的硬性约束同时强制。
3. **冻结旧源码、当前 R0–R4 已发生提交/验收事实不被设计文档追溯修改。** 现有 `docs/render-v2/00–10` 中相冲突的“旧目标描述”需要由首先的文档集成提交逐章替换/标注 superseded，而不是改历史验证报告。
4. 压缩包 `_references_R2_readonly/` 是用户原 R2 历史设计的逐字参考，不参与运行时规范；`appendix/CPP20_讨论稿只读.md` 是早期思想来源，正式规范以第 12 章为准。
5. 真正的第三方/标准（Vulkan、SPIR-V、C++20）要求优先于本设计中任何伪代码。例如 device capability 不支持的优化必须退回保守正确实现，不能伪造 PASS。

## 00.4 文档清单及阅读顺序

| 文件 | 唯一责任 | 关键产物 |
| --- | --- | --- |
| `01_哲学_原则_完整性.md` | 最终设计哲学、事实范围、反偷工减料 | 不变量、保留/重写/新增分类 |
| `02_分层_类型_所有权.md` | 模块 DAG、最终类型、authority/borrow/lifetime | Type Registry / owner 图 |
| `03_通信_数据_事务.md` | PROGRAM/CONTROL/UPLOAD、typed RenderData、订阅/背压、资源事务 | wire + revision/receipt contracts |
| `04_RenderGraph_作者模型.md` | Builder、PassParams typed hooks、资源/Pass/Scope 等语义 | 完整 Authoring API |
| `05_Shader_自动绑定.md` | 同源 codegen、ShaderInfo、LayoutPlan、反射/重定位、材料接口 | 单一 binding authority |
| `06_逻辑图编译_优化.md` | hazards/subresource/culling/conditional/cache/lifetime | `LogicalGraphPlan` |
| `07_Vulkan编译_执行_同步.md` | native plan、queue/barrier/alias、descriptor/pipeline/record | `ExecutableGraphPlan` |
| `08_Runtime_Scene_View_Target.md` | 真实 Runtime 与多 Scene/View/Target、独立进度、Multiview | 真 GPU FrameExecutor |
| `09_Feature_Capability_插件.md` | Provider/effect/compute、配置、代码 pin 和 SDK | concrete Feature + public Ops |
| `10_Legacy功能_业务数据.md` | 各旧特性的输入、输出、资源、Graph 行为、异常 | 完整 parity 路线 |
| `11_UE级画质_先进能力.md` | Nanite/Lumen/TSR/VSM 对齐算法与预算 | 高级画质验收 |
| `12_Cpp20_实现哲学.md` | 现代 C++20、value/free algorithm/Concepts/RAII | 明确语法与禁止样式 |
| `13_错误_性能_生命周期.md` | 失败、异步停止、GPU lifetime、零分配 | 测量与故障注入 |
| `14_API范例_全链路.md` | 7 个可实现的端到端场景 | 编译/生成/GPU oracle |
| `15_实施阶段_工作范围.md` | 真实独立阶段、目录准入、迁移计划 | 阶段 I/V 和 STOP |
| `16_验收与功能矩阵.md` | 每个契约的验收、证明及否决条件 | CSV + qualification |
| `17_LLM实施合同.md` | 给实施 Agent 的唯一执行合同与首阶段授权 | 可复制 work order |
| `18_历史规范迁移.md` | R2→最终设计映射，R0–R4 preservation | 文档迁移规则 |
| `19_冻结决策与歧义消解.md` | 已关闭设计决定和技术降级边界 | D-01～D-32 |

附录：`appendix/LegacyCapabilityMatrix.csv`、`TypeOwnership.csv`、`RequirementsTrace.csv`、`GoldenWorkloads.csv`、`EVIDENCE_INPUTS.md`、`MANIFEST.sha256`。CSV 是执行级追踪文件而不是旁注。

## 00.5 非目标与不允许的替代方案

- 不新增第二 Renderer、FrameLoop、通用 RHI 抽象、Universal History/Resource/Manager、全局 EventBus、默认后台异步 Writer、隐藏锁或无界队列。
- 不用 RenderData `TypeId` 代替 Capability 与 Graph Resource；不允许 Feature 通过私有 VkDevice/RenderScene 头实现 SDK。
- 不让 CPU 不可信 borrow 越过异步边界；不把 GPU fence completion 等同于 CPU frame index；不在录制时临时创建 pipeline/descriptor layouts。
- 不用 mock Vulkan、CTest 数字增长、编译成功、预留 TODO、空 Feature、假兼容层、丢弃历史失败或空文件覆盖来满足验收。
- 不把 `RenderGraph` 简化为一个资源访问数组，也不复制 V1 的巨大 Compiler/Server/Context 层级。

## 00.6 可验证的完整性定义

功能被标为 `IMPLEMENTED + QUALIFIED` 的最低条件同时是：① 独立编译了正式生产目标；② 使用 public author API/真实 Shader/Graph/Backend；③ 真实 GPU 或合适的 CPU oracle 验证具体功能；④ 资源失败/stop/resize/rebase/卸载/背压覆盖适用项；⑤ 真实依赖闭包未越界；⑥ 性能数据同口径；⑦ 验收证据有确切 SHA，且历史 `NOT_RUN` 未伪报 `PASS`。

**完整 Legacy parity 与高画质新算法分别计分。** GPU-Driven PointCloud 的软 splat 并不是 3DGS；SSR 并不是 Lumen；可绘制 Shadow Atlas 并不是 VSM；图存在 motion-vector pass 也不是 TSR。

## 00.7 直接交付实施方的启动规则

开始时必须在独立工作树核实远端 `codex/render-v2` HEAD、R4 I/V 及本包 `MANIFEST.sha256`。首先**只做文档集成阶段 F0**：把 `00–19` 转入当前仓库 `docs/render-v2/final/` 或同等明确且唯一的权威目录，保留原验收报告，标记旧目标规范 superseded，验证目录、依赖和历史提交 unchanged，单独 doc-only commit、独立审核后 STOP。第 15 章为后续阶段目录；**本包不自动授权所有未来开发阶段无审查连跑**。用户在审阅 F0 后按对应 work order 放行 F1、F2 等；用户本次明确要求“可直接执行”的范围是 F0 文档整合及完整后续工作单准备，而不是先改 Vulkan/Graph 再补文档。

实施方不允许以本包某段话未能从当前已安装依赖构建为理由自行删掉要求；要提供可复现的阻断报告和最小设计修订候选，STOP 等用户判决。
