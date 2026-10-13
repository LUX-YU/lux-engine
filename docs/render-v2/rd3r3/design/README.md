# Render V2 — RD3-R3 完整架构与分阶段实施设计

**版本：2026-10-13 / RD3-R3 — Attachment RAII + compiled ResourcePlan + portable DataProjection**

此包是对上一版 `RenderV2_RD3R2_DeviceSession_EffectGraph_Architecture_2026-10-12(3).zip` 的**整套替代设计**，并以最新 `codex/render-v2` P2-A I/V 为代码实施事实源。它不是 Docs-only 合格后的生产实现，所有新设计工作单均须用户明确授权。旧交接文档和旧设计包仅为来源，**不能与此包并列作为施工权威**。

阅读顺序：`STATUS_实施状态与边界.md` → `00_架构总纲.md` → `19_RD3R3统一架构裁决.md` → `02_模块分层与所有权.md` → `04_作用域资源与RAII生命周期.md` → `05_ResourcePlan与Attach编译事务.md` → `06_RenderEffect与Feature组合.md` → `21_Vulkan三级Context与单DeviceSession.md` → `24_ECS数据投影与插件扩展.md` → `25_Protocol与可替换NativeBackend.md` → `23_Legacy专业GPU存储迁移规范.md` → `27_H1至H6高级渲染设计输入.md` → `28_目录与CMake模块边界.md` → `29_Legacy_RenderGraph_Compiler源码审计与迁移合同.md` → `30_MaterialGraph_Shader变体与运行时集成.md` → `31_大资源UPLOAD与GPUTransferPipeline.md` → `32_GraphTransient与专业GPUStorage性能账本.md` → Stage 工作单。

## 一句话对象模型

**Effect owns Features+Logical Graph；Compiler outputs ResourcePlan；Backend owns EffectId→(SceneId,ViewId,TargetId) attachments and RAII resource-use leases；DeviceSession owns native backing (InstanceContext is outer bootstrap-owned)；Executor consumes prepared indexed native bindings；Engine publishes typed data solely through neutral RenderRuntime and plugin DataProjection.**

## 完整索引

- `00–32`：**33 章完整架构正文（含 29–32 专项源码审计）**（分章为唯一编辑来源）。
- `FINAL_RD3R3_全章合订版.md`：机器合并所有章节，作为便于传递/审阅的快照。
- `work_orders/`：从已完成 P2-A 之后按 R3-A → R3-B → R3-C → R3-D → R3-E → L1/L2/L3/Product → H1–H6 的实施入口；每阶段独立 I/V/STOP。
- `appendix/CapabilityMatrix.csv`：**86 项** F01–F36、I01–I44、H01–H06 保留原能力/验收/源文件字面值，并新增 R3 stage/owner。
- `appendix/GoldenWorkloads.csv`：**25 项** W01–W25 原始 oracle/历史 qualification 继续保留。
- `appendix/RequirementsTrace.csv`：**51 项** REQ01–REQ51 原始需求与跟踪信息保留。
- `appendix/LegacyGpuAlgorithms.csv`：**16 项** 旧 GPU 存储核心算法的迁移/性能约束。
- `appendix/LegacyCompilerMechanisms.csv`：**20 项** 旧编译器特殊语义/优化、反例与迁移阶段。
- `appendix/MaterialGraphContracts.csv`：**14 项** 现有 MaterialGraph/Shader/Legacy GPU 材质链路。
- `appendix/UploadTransportCases.csv`：**16 项** Bulk UPLOAD/独立传输队列、字节预算与真实 GPU Ticket。
- `appendix/TypeOwnership.csv`：**79 项**本版唯一规范类型/函数所有权表。
- `appendix/PriorRD3R2TypeOwnership_ARCHIVE.csv`：旧版 66 项类型清单，仅作历史对照，明确非权威。
- `appendix/ExecutionScenarios.csv`：**69 项**新旧场景，旧来源与新增专项反例各保留自己的状态。
- `appendix/MigrationDecisions.csv`：旧类/错误设计删除/迁移方向。
- `appendix/DesignDecisions.csv`：**33 项**与第 19 章同步的裁决编号。
- `appendix/DataProjectionExamples.csv`、`appendix/ResourcePlanExamples.csv`：能落地的跨 Plugin/Scope 示例。
- `28_目录与CMake模块边界.md`：新目录权威；`LAYOUT_目录与CMake模块边界.md` 是其自动生成镜像而非另一个设计版本。
- `REFERENCES_外部机制参考.md`：Epic 技术机制与来源边界。
- `AUDIT_来源与一致性.md`、`MANIFEST.sha256`、`checks/verify_package.py`：来源和机械一致性工具。

请从 R3-A 的一条真正 CPU lease + Native Buffer + GPU submit 用例起步；先审核设计、批准后才授权施工，不一次性让 LLM 重写整个 Render 子树。Plugin/高级渲染的架构合同在本包写全，但具体算法/业务按真实阶段实现。


## 2026-10-13 Legacy 源码深度审核补充（本文档包内同等权威）

正式新增 **29–32 四章**，逐项核对 Legacy 编译器的真实优化与负例、现有 MaterialGraph/Shader Toolchain 的实际资产流程、P2-A 仍未接线的大资源 UPLOAD 与 dedicated/aliased Vulkan Transfer Queue、Transient GPU backing pool。新增三个有来源路径与 Stage 的专项 CSV。**这些是新设计与迁移义务，不是已通过生产 GPU 验收的实现。** 原第 07/09/23 章只作总览，具体合同以第 29–32 章为准；分章是唯一编辑源，合订版机械生成。新版阶段授权必须由用户另行作出。
