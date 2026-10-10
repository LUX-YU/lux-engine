# 10 — Legacy 全功能等价：业务数据提供、真实算法、Feature 与通信闭环

> 本章是**不可裁剪的完整性合同**，不只是推荐功能。每条 `Fxx/Ixx` 对应 `appendix/LegacyCapabilityMatrix.csv`，以冻结 SHA `a669409a289a6fa4092f21176397795b1cdb7f3e` 下的真实源码和相应操作 Schema 为行为参照。矩阵列 `RequiredAcceptance` 是交付的独立最低行为；阶段报告不能只写“已支持 Mesh/Shadow”。

## 10.1 Mesh/Instance/Material 的真实数据链

**Source**：World/ECS Mesh component、Transform、Material assignments、visibility/instance flags、optional Editor selection/streaming data。**CPU ownership**：`RenderProjection<Mesh...>` + Runtime integration-scope `RenderResourceDomain` asset retain/receipt。**Wire**：PROGRAM upsert/remove/retire/batch state；UPLOAD decoded/cooked Mesh/Texture/Material chunks；CONTROL destroy/query/feature attach。**Backend**：persistent MeshSceneData/InstanceResources + runtime-shared GPU mesh/vertex/texture/material owners；native resources ready/last-use fence。

**Graph**：Frame/Scene-scope GPU skinning (pose-dependent), cull candidate/compact/indirect visible list（View-dependent as needed）, Forward/Deferred/MeshShadow graphics, correct depth/prepass/alpha mask/opaque/translucent ordering；same View mixed Mesh+PointCloud+Terrain/Gizmo；shared input Asset does not duplicate uploads across 2 Scenes。

**必须恢复细节**：Stable RenderObject/Entity generations、instance create/update/update/delete、per-View visible mask、instance draw flags/cast/receive shadow、fade/retire transition、MDC grouping、vertex-source pool and indirect args、node-graph materials、PBR/Stylized/Unlit、normal/clearcoat/sheen/emission/alpha cutout and material specialization variants、shader live preview candidate rollback。现有 `ForwardMeshFeature`、`DeferredGBufferFeature`、`StandardMeshStackFeature`、`StandardMaterialFeature` 等不是一个“Mesh Feature”能替代的算法集合。

## 10.2 光照、阴影、Clustered 与 GBuffer

`LightSceneData` provider 拥有 directional/point/spot/area light SoA、create/update/delete/intensity transition；Forward/DeferredLighting/Shadow/MeshShadow 作为消费者 attach resolve。Deferred Lighting 要真实 GBuffer MRT/depth producer，Clustered pipeline 包含 build/count/scan/fill/draw，资源上限/overflows/光簇和 depth 判定需 GPU 验证。

ShadowMap 保留 PCF/EVSM、方向光 CSM、atlas slice/page 分配与 light technique switch；MeshShadow 保留 GPU-driven caster cull/compact/alpha-material handling，Shadow resources 同时供 Forward 与 DeferredLighting 读取。EVSM atlas/filter 的 code path 必须真实存在。Camera 不同的 CSM 不能无条件只做一份；Point/Spot 可按 shared light projection/cache key 复用。

`DepthPrepass → GBuffer/Forward → Lighting → LinearDepth/SSAO/Fog/Water → Tonemap → Highlight/Grid/Gizmo/UI` 仅为示例拓扑；实际颜色/深度/attachment LOAD/clear/store、HDR→LDR/sRGB 和 cross-pass dependency 通过 Graph compiler 决定，不能依靠注册顺序凑出“看起来能画”的画面。

## 10.3 Geometry GPU compute：Skinning/HZB/Cull/RenderCluster

**Skinning**：Bone palette owned blob/batch 输入，correct layout/stride/vertex pool IDs、GPU compute output pool、material/mesh draw 共用 producer output；cross-frame WAR 旧限制必须新测试覆盖。相同 Scene 姿态/输入 epoch 的两个 View 不重复 pre-skin，但 View-specific visibility/camera cull 仍需独立。

**HZB**：从 depth 构建 mip chain，每 mip subresource RAW/WAW/barrier，history current/previous valid epoch 由 View/Feature owner 持有，resize/camera cut/rebase/target change invalidates；cull consumer GPU indirect / occlusion re-projection 不得读取 stale mip。

**SpatialCullGrid**：world/page origin、distance cells, optional coarse cull mask, scene large-world rebasing；它可以是只有 SceneData/cull mask provider 的 Feature，不强迫添加 Graph Pass。

**RenderCluster**：树形 cluster parent/children、GPU cull/LOD expansion/hierarchical fallback、GPU-visible count、Picking ID target/readback/request generation/stale View recreation rejection。picking 不能因为读回时 View 被销毁而把旧 ID 回给新 View。

## 10.4 PointCloud 不得缩成一个 Feature

| 原策略 | 输入与计算 | 必须保留的独立结果 |
| --- | --- | --- |
| Simple | chunk/PointCloudPoint16 → GPU resident points → direct draw | 颜色/点大小/增长/清理/稳定窗口 |
| GPUDriven | octree point chunks + compute frustum cull/visible compact → indirect draw | GPU cull accuracy/indirect counts/latency |
| LOD | HZB/Camera/point-world-size/Octree → screen/world size scheduling | depth-scale/LOD stable transitions |
| Splatting | Gaussian **soft-splat point cloud** with blending/point splat frag | alpha/depth correct、GPU-driven batching；**非完整 3D Gaussian Splatting** |
| Transient | owner-frame point data, per-frame ring/generation | 下一帧清理，无 stale memory, no unbounded realloc |

资源基础还包括 PointCloudGlobalBuffer、GpuOctreeNodeBuffer、PointCloud chunk create/replace/remove/clearAll/query/pointSize、跨 Scene 共享可证明的 asset、per-view visibility/LOD states。五条算法必须分别有 shader/GPU readback/image/performance fixture，不允许用一个 PointListDraw 替代。**未来 3D Gaussian Splatting / 3DGS 是另一套高阶表示算法**：需要带协方差/旋转/颜色与不透明度的 splat 参数、view projection、screen-space tile binning/sorting、alpha transmittance compositing、GPU 数值稳定性与深度混合；它应作为同一个 Scene/View/Target/Graph 内可组合的 Feature，而不是新增 `GaussianSplatRenderer`/第二 Backend。旧 F33 soft-splat 不能被当成完整 3DGS。见第 11 章 H06。

## 10.5 Terrain/Water/Large World

Terrain `UploadTerrainPagePayload` 包含 page ID、parent/children、quadtree/page min-max data；真实 GPU page selection/cull/indirect/draw、page fallback、HLOD/transition；页式缓存有 logical capacity、fallback capacity、eviction、resident/ready stats，不得因 UPLOAD 背压丢 parent fallback。Terrain 受 Camera/View/Scene origin high precision binding 影响，`rebaseSceneOrigin` 不得重传全静态 Mesh；GPU bounded expansion budget 实测。

Water `SurfaceCreate/Update/Destroy/Stats` 是 SurfaceSceneData provider，支持 scene transform、material/depth/fog/local reflection 依赖、实际 Graph composite、过期 handle 清理与 transition。水面如果只是屏幕空间覆盖层不能冒称高级流体/反射水体；这些属于第 11 章新增算法目标。

Large-world CPU 坐标可以使用 page+local 或双精度，投影到 GPU 必须有**唯一明确的精度表示**（origin page、high/low split/view-relative local 均可选并经数值误差 fixture 证明）；所有需要绝对位置的 Feature（Shadow/RenderCluster/Terrain/Water/Trajectory/PointCloud）都必须响应 rebase，不得静默继续用 stale position。

## 10.6 Canvas2D/UI/Editor

Canvas2D 含 GPU-resident Image2D、PixelField、TileMap、ordered priority/instanced batches、premultiplied alpha、offscreen subgroups 和 group composite；`TransformBatch` 只发 dirty，`Canvas2DEnabled` retained bit 控制合法绘制。2D Camera 和 UI 像素坐标不强制伪装为 3D Scene projection。混合 2D/3D/Editor 应通过同一 Target chain/FrameExecutor，不能单独 UIRenderServer 的 FrameLoop。

Grid2D/Grid3D（under/over painter order）、Gizmo transient Lines/Triangles、Highlight selection mask/cull/blur/composite、StreamingFeedback 面片与可选 shader pattern、Skybox cubemap/equirect、RenderCluster Picking、Scene Target UI overlay 等保留各自 Effect 参数和正确深度/颜色关系。`Transient` 意味受限跨帧资源，不能直接复用永久 Asset GPU pool。Editor camera 更新不依赖 Simulation tick。

## 10.7 Postprocess 完整接口与图输出

Tonemap (exposure/operators/ACES fitted、HDR→sRGB correct), LinearDepth (nonlinear→linear), SSAO (depth sampling/AO history/fallback), Fog (depth+scene color), Highlight Gaussian Blur/composite、Water composite、StreamingFeedback mask/composite 都必须通过 typed PassParams+Shader binding+Graph native execution 实际接通。`Fog` 和 `Water` 的 required/optional dependency 必须有显式 fallback；不能因为缺一层上游 pass 就不报错地采样旧资源。

Layered target/present、multi offscreen/readback、swapchain image import/final layout、MSAA/depth/resolve、multiple overlays、UI/Editor handoff 需要真实图像和 resize/minimize/fence 证明，不能只做 CPU Graph compilation。

## 10.8 插件、Toolchain、Core/Transport 的历史行为完整性

- `LUX_OP`, `LUX_COMM_CONFIG`, stable registration/proxy/typed op codegen；`PROGRAM/CONTROL/UPLOAD` 成熟 queue/packet/blob/borrow/retention 算法。
- `LUX_PASS_PARAMS`, `LUX_RESOURCE`, `.lglsl` emitter、`ShaderInfo`、Shader reflection、complete EngineShared/LayoutPlan、SPIR-V Descriptor relocation、vertex layout/variants/pipelines。
- `RenderRuntime` stop/wake、reply/diagnostic/resource status、Scene feature registration；一套 FrameDriver/fences/semaphores、surface/offscreen/presentation/teardown。
- Engine Scene RenderSystem 单入口、asset dedupe、SceneReceipt/ViewReceipt、View camera extraction、World-specific Projection、UI data sources 和 external feature plugin installed SDK example。

都要在新的完整测试链复现。R0 719 冻结文件、R1–R4原验收事实不得因设计文档“新增功能”被重新定义为已实现。

## 10.9 每个业务能力的规范交付记录

实施日志每 `Fxx` 必须维护：`SOURCE_SHA/source_path + historical operation contract + author-facing API + source input + typed wire + backend persistent owner + Graph/Shader/PSO chain + state/error/stop/retirement + positive/negative GPU oracle + same-workload performance + exact implementation SHA`。一行未绑定实际证据则仍是 `NOT_MIGRATED/PARTIAL`，不能标记 PASS。

示例：对 `F33 PointCloud Splatting` 的通过标准是**旧 soft-splat 语义等价 + 真 GPU raster/alpha output**；“给未来 3DGS 留 Pass 接口”不得替代该行。对 `F08 Shadow PCF/EVSM/CSM`，允许分子门禁并最终汇总，但只移植 PCF 不能把整行关闭。

## 10.10 跟踪矩阵与来源证明

主表 `appendix/LegacyCapabilityMatrix.csv` 保留原 F01–F36、I01–I30 编号，并添加 `EvidenceRequired`, `NewDesignChapter`, `AcceptanceTag`, `ActualImplementationSHA`, `QualificationSHA`, `QualifiedStatus` 字段；初始新 Graph/业务未实现状态均为 `NOT_MIGRATED`。接受功能时实施方必须追加准确测试/证据哈希，不可改原 SourceFile/SourceSHA 指向另一处代码。另设新增 UE 高级能力的 `Hxx` 行，不与 Legacy parity 结果混合。
