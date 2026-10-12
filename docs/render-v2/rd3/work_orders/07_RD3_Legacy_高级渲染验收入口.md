# RD3-L1/L2/L3 与 H1–H6 — 全量旧能力与高级渲染验收入口（DRAFT，NOT_AUTHORIZED）

## 首要原则

F8/F9/F10/F11 原业务和 H1–H6 的**要求不削减**，但对象/Owner/入口统一改成 RenderEffect/Graph/ResourceDomain/Target/Backend。先在新的 Client/Effect/Native 全链和公共 Plugin SDK 真实可用后逐组迁移；不能通过“一个 Feature 已声明 Pass”自动给业务 PASS。

## L1：Geometry/Camera/Materials

F01–F03、F06–F07、F11–F14：Mesh/Instance Root/Scene GPU resources、Forward/Deferred GPU-driven、GpuCull/Skinned/Depth/Camera/RenderCluster/Picking/Material Graph Shader variants。实际 GPU correct indirect/multi Queue/readback/history；CPU 不全量枚举 Mesh 代替 GPU Cull；不同 Scene 使用相同 Effect/PSO。

## L2：Lighting/Shadow/Postprocess

F04–F05、F08–F10、F15–F20：clustered Light、Light source、PCF/EVSM/CSM、MeshShadow、HZB、Tonemap/exposure、SSAO、LinearDepth/Fog/Highlight/Skybox；Frame 一张 Graph 完整输出、History/Shadow resources owner 清晰、Texture/Cubemap Native 实现按真实能力扩展。Shader 反射、readback/oracle、Perf/VRAM/fallback。

## L3：Editor/2D/Terrain/Water/PointCloud/Robotics

F21–F36：Grid2D/3D、GizmoLine/Tri、Canvas2D Tile/Pixel/Layer/Blending，Terrain page LOD fallback、Water、Trajectory/Streaming feedback、PointCloud Simple/GPUDriven/LOD/SoftSplat/Transient、Point/octree/scene trajectory GPU buffers。无需依赖 Universal Mesh Instance、UI 不开独立 FrameLoop，Output Target 的多 Layer/Offscreen/Surface 最终结果真实正确。

## F11：产品 Cutover

Legacy 36 F + I01–I44 每条实际功能/基础架构 item 必须带原 SourceSHA、操作合同、Shader/GPU reference/性能、Installed SDK/Engine/Editor/Player 端到端、Full ASan/GPU Validation/SyncValidation、I/V SHA。只在全部业务/兼容矩阵实际 PASS 后考虑产品切换；新 RenderEffect 架构与现有 F6.b core tests 不是 F11 PASS。

## H1–H6：高级算法各自独立

H1 Virtual Geometry、H2 GI/Reflection、H3 TSR、H4 VSM、H5 Advanced Material/Volume、H6 3DGS；分别交付固定参考场景/seed、GPU quality/temporal oracle、DeviceCaps/fallback、显存/带宽/perf、跨 View/Scene/Target/Editor；按需求增加 Native op，禁再造独立 Renderer/GraphCache/ResourceRegistry。4D 完整 deformation 是后续研究，Legacy soft splat 不是 Full 3DGS。

## 资格纪律

每组单独锁定 ALLOWED 文件/GPU workload/原始图像/CI95/旧 F/I/H ID、源 Git/代码实际 lineage，完成 I→独立 V→STOP。不通过删除历史 FAIL/NOT_RUN、改 baseline 画质或绕过原功能获得 PASS；新的效果/Shader 再漂亮也不能替代旧用户可见功能。
