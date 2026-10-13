# 12 — Legacy 86 项合同及完整产品迁移

## 12.1 范围一项不减

冻结来源 `a669409a289a6fa4092f21176397795b1cdb7f3e` 的 719 个 Legacy source 保持原样。`appendix/CapabilityMatrix.csv` 精确保留 F01–F36 36 项业务、I01–I44 44 项设施/新增合同、H01–H06 6 项高级方向；`GoldenWorkloads.csv` W01–W25 与 `RequirementsTrace.csv` REQ01–REQ51 的历史 wording/source/oracle 作为来源字段不删除。新列给出 RD3-R3 的 owner/stage/test；并不表示未来功能已 PASS。

## 12.2 业务里程碑

| Stage | 交付闭环（真实 native 输出/数据） | 主要能力 |
|---|---|---|
| L1 | MeshStack、Forward GPU-driven、Deferred GBuffer、StandardMaterial、DepthPrepass、Spatial Cull、GPU Skinning、RenderCluster/Picking、Camera Projection | F01–F03, F06–F07, F11–F14 |
| L2 | Clustered Lighting、Scene Light provider、PCF/EVSM/CSM/Mesh Shadow、HZB、Tonemap/SSAO/LinearDepth/Fog/Highlight/Skybox | F04–F05, F08–F10, F15–F20 |
| L3 | Grid2D/3D、Gizmo lines/triangles、Canvas2D、Terrain/Water/Trajectory、StreamingFeedback、PointCloud Simple/GPU-driven/LOD/SoftSplat/Transient、GPU Point/Trajectory storage | F21–F36 |
| Product | Editor/Player/Toolchain/installed SDK、完整 WSI/Offscreen/multiple Views/Targets、所有 F/I 实际 consumer | F01–F36 + I01–I44 |
| H1–H6 | 虚拟几何、动态 GI、TSR、VSM、复杂 BSDF/体积、真正 3DGS；分别做质量、稳定性、可比性能资格 | H01–H06 |

Legacy PointCloud 的软 splat ≠ Full 3DGS；`Graph` 能表达一个 pass ≠ 已实现高级质量算法。不能把 Grid/Canvas/Trajectory 全部降为 MeshInstance，也不能把 GPU-only dynamic data 迁回 CPU 每帧全量重建。

## 12.3 每类迁移最小证据

检查 frozen Legacy 的 Shader ABI/GPU layout、内存分配和 Dirty 策略、增量上传、Scope/Producer、RenderGraph Pass/SideEffect、Ready/LastUse、Plugin Pin、性能负载。先写一个新的具体 Storage Consumer，沿用已证明的算法；再由 Source→Projection→Schema→ResourcePlan→Attach→Executor→Target 做真实 Pixel/Numeric Oracle。保留旧报告中的 regression/partial 样本，不用删除失败历史为新版本通过。

## 12.4 不强制复刻 Legacy 的所有类

旧 `RenderContext` 总服务对象、Scene Feature 安装、Feature `onFrameBegin` 虚生命周期、业务 Proxy/RenderSyncStage、`ResourceRegistry` 哈希身份表不自动继承。旧 `GPUBuffer`、PagedGpuStream、Arena、Bindless、PointCloud Chunk、Transfer/Staging、Deferred retirement 等算法保留并与新 Context/ScopedResources/Attachment Ticket/Output 声明对接。第 23 章和 LegacyGpuAlgorithms.csv 为逐类迁移最低要求。


## 12.5 不能遗漏的 RenderGraphCompiler / MaterialGraph / 专用 Upload 迁移

第 29 章的 Graph compiler 快路和同步正确性负例、第 30 章现有中立材质 Node Graph/Shader Cooker + Legacy GraphFamilyGPU、第 31 章大体积 UPLOAD/专用 Native Transfer Queue、第 32 章 Transient Physical Pool 现在都是正式业务迁移输入。把它们列入 R3-B/R3-C/R3-D/R3-E/L1–L3 的跨阶段验收；不是仅等到 H5 才考虑材质图，也不是仅靠旧 R2 `UPLOAD` Lane 存在就算完成 Mesh/Texture 大上传。
