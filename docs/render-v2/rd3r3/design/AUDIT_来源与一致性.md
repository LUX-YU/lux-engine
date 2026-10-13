# AUDIT — RD3-R3 来源、正确性层级与校验范围

## 原始输入及其边界

- 本次用户附件：`RenderV2_RD3R2_DeviceSession_EffectGraph_Architecture_2026-10-12(3).zip`，SHA-256 `839ce60343d843e56317adbd72a12d7f388f0c22e75ed1b961d8c50a86e2d2d3`，实际含 43 个文件（24 章、7 个原工作单、原 86/25/51/16 等 appendices）。
- 对应较早 RD3-R2 目标已晚于 P1，但早于 P2-A I/V；本包重新核对 GitHub 最新 `codex/render-v2` HEAD `960e755395be2c9f78a86339a062c507826c4eb4`（P2-A V，P2-A I `483d20ed900b8c226a740704f3c888498b1dae11`）。所有状态仅来自公开仓库提交/报告，无本地 Windows GPU 重跑，不把设计文档当代码已经落地。
- Frozen Legacy 719 source tree 只作为算法与功能来源；`a669409a289a6fa4092f21176397795b1cdb7f3e` SHA 保留，不修改生产文件或 GitHub 仓库。

## 本版来源与新推导如何区分

- 原包功能名称、Legacy source/Operation、原始 RequiredAcceptance、Golden Oracle、Requirements 及各历史 Qualification 字段作为**来源字段**保存。原文件 `RD3R2...` 列重命名为 `PriorRD3R2...`，明确不是新设计 Owner/Stage/Pass。
- RD3-R3 新设计：Effect attach RAII ResourceLease、ScopedResources 而非公开 Domain 树强制生命期、编译 ResourcePlan、InstanceContext 外部 owner、Engine registered DataProjection、neutral Native Port、对 UE-like 高级 H 阶段的需求分解。这些是新架构决策/建议，**不能表述为 P2-A 已实现**。
- `appendix/PriorRD3R2TypeOwnership_ARCHIVE.csv` 仅防止丢失前序 66 个类型记录；现行唯一类型权限表为 `appendix/TypeOwnership.csv`，不将旧 ExecutionJob/ResourceDomain 写成新权威。

## 已审 Legacy 源码的事实

- `GPUBuffer.hpp`: CPU_TO_GPU/GPU_ONLY policy, dirty epochs, scratch reuse, host mirror/optional storage。
- `PagedGpuStream.hpp`, `StableRecordPages.hpp`: page stable data, coalesced dirty page runs, reused scratch。
- `ArenaAllocator.hpp`, `ChainedArenaAllocator.hpp`, `MeshResources.hpp`: VMA virtual TLSF and segmented mesh VBO/IBO, record pages/LOD/Ready。
- `InstanceResources.hpp`: stable slots, Transform/Cull/Property streams, dense alive, per-stream Dirty。
- `TextureResources.hpp`, `BindlessCombinedSet.hpp`: stable bindless slots, mip feedback/region update, deferred recycle。
- `LightResources.hpp`, `MaterialResources.hpp`, `PointCloudResources.hpp`, `TransferScheduler.hpp`: typed GPU records, per-FIF update, ordered chunk ops, staging ring/batch/barriers。
- Legacy `RenderFeatureSceneBinding.hpp`, `RenderSyncStage.hpp`, `Builtin3DRenderStages.cpp`: registered observations, reactive change sets, O(changed), prepare/commit/discard publication/backpressure；**不必继续原虚继承 Proxy 类结构**。

## 正确性与当前未证明内容

- 本轮没有生产代码修改或编译/实机 GPU 测试。只对 ZIP/Manifest/Markdown/CSV 主键/阶段映射/章节一致性进行本地机械校验。
- 历史 P2-A 原验收报告包含 Native OOM retry 的历史实验，而新目标明确不承诺 OOM recoverability。此差异不是篡改历史测试结果。
- 新 D3D12 Backend、Feature SDK、DataProjection、ResourcePlan、ScopedResources RAII、H1–H6 都未在当前 P2-A 源码中完成。
- 参考 Epic 官方文档见 `REFERENCES_外部机制参考.md`；不能据此声称 LuxEngine 有 UE 同级算法或 GPU 性能。

## 校验契约

脚本 `checks/verify_package.py` 验证 00–32 共 33 章、I/V stage 工作单、86=F36+I44+H6、W25、REQ51、LegacyGPU16、Compiler20、Material14、Upload16、PriorType66、现行 Type79、Scenarios69、决策 33 ID、Markdown fenced code closing、manifest SHA、镜像与合订内容来源及 ZIP CRC（作为打包工具另检）。每次修改正文必须重新生成合订版和 manifest；若更新 Owner/Scope/Stage，必须同步其 CSV/工作单及本章状态。


## 追加专项审计：RenderGraph、MaterialGraph、大资源传输（2026-10-13）

源码定位全部来自 GitHub `LUX-YU/lux-engine` **HEAD `960e755395be2c9f78a86339a062c507826c4eb4`**：

- 冻结 Legacy: `render_legacy/modules/function/render/vulkan/src/graph/RenderGraphCompiler.{cpp,ExecutionPlan.cpp,Barriers.cpp,LayoutAllocator.cpp}`、`RGBuilder.hpp`、`RGCompiledGraph.hpp`、`RGVulkanResourceAllocator.*`、`RGGraphResize.cpp`、`RGVulkanRecorder.cpp`；以及 `render_legacy/modules/function/render/graph/src/DependencyAnalyzer.cpp`。新 Compiler 的语义与优化证据见第 29 章及 `LegacyCompilerMechanisms.csv`。
- **现行独立材料库（不是 render_legacy 冻结源）**：`modules/function/material/graph/`、`MaterialNodeCatalog.hpp`、`src/MaterialLowering.cpp`；现行工具链 `engine/toolchain/material/MaterialCompiler.cpp/MaterialToGraph.cpp/ShaderEmitter.cpp/ShaderCompiler.cpp/MaterialCooker.cpp`；Legacy 业务 `GraphMaterialData.hpp/MaterialResources.cpp/VariantBucketManager.cpp`，分别分清作者/编译/产品 GPU 的完成边界，见第 30 章及 `MaterialGraphContracts.csv`。
- 传输路径：Legacy `RenderUploadClient/RenderUploadSession/UploadQueue`、`GpuTransferPipeline`、`TransferScheduler`，现行 R2 `transport/{Transport.hpp,Operation.hpp}`、R4 `vulkan/transfer/Submission.hpp`、P2-A `runtime/src/Backend.cpp`。P2-A `pollUpload` 仅 Stop Drain，未提供 active Mesh/Texture upload；**不能把 Lane 的存在作为专业 GPU Upload PASS**。见第 31 章及 `UploadTransportCases.csv`。
- Graph Transient pool 的 VMA image/lazy pool/cache/resize、专业 Shader/Material/Texture/Mesh/Instance 存储的性能结构见第 32 章。

新版 29–32 章是必须阅读的正式设计输入，并同步修订 00/07/08/09/12/13/19/23/25/27/28、相关阶段工作单、类型与决策 CSV 和机械校验。所有事项仍是**设计与源码审阅**，没有生产代码修改/本地 Windows GPU 重跑，也不宣称当前 P2-A 已实现本次补充功能。


**状态标签释义**：`LegacyCompilerMechanisms.csv` 的 `NOT_QUALIFIED_IN_P2A` 表示 P2-A 没有逐项证明与 Legacy 对应能力等价，**不代表已审定 R4 没有该算法**；`PARTLY_R4_NATIVE` 表示已找到部分 R4 Native 实现，仍需完整语义对照。严禁把这些状态直接翻译为重写 R4 Compiler 的施工要求。
