# 12 — Legacy 36 业务功能、44 基础合同的完整迁移

## 12.1 核对权威

冻结 `render_legacy/**` 719 个源文件与原 tree `a669409a289a6fa4092f21176397795b1cdb7f3e`，逐 Blob/Mode/Size 验证，不可编辑。原 `F01–F36` 36 项业务、`I01–I44` 44 项基础设施/新增合同，加 H01–H06 共 86 项，本包 `appendix/CapabilityMatrix.csv` 一一对应。原 25 个 Golden W 与 51 条 Requirements 继续按原 ID 追踪；新列指向新的 `RenderEffect/Graph/Domain` 对象模型。历史源码名与 OperationSchema 仅作为**来源证据**，不是新类型命名要求。

## 12.2 旧 Feature 的职责重新拆分，而非删除需求

| Legacy 业务形态 | 新模型里的数据与 Owner | 新 RenderEffect/Feature 贡献 |
|---|---|---|
| MeshStack/Forward/Deferred/GPU Cull | Root MeshStorage/Material/Texture + Scene instance/transform GPU resource | Forward/Deferred Effect 添加 Cull/Indirect/Depth/GBuffer/PBR Pass |
| LightProvider/Cluster | Scene LightBuffer；GPU Cluster/visible list 按 Scene/View 使用 | Lighting Feature 定义 Cluster Compute 与 Shader 消费 |
| Shadow PCF/EVSM/CSM | Scene/Root Atlas/Shadow residency，View cascades/metadata | Effect 中的 Shadow Pass/算法变体，必要 GPU feedback/history |
| Camera/HZB/TAA | View ResourceDomain Camera/History/Depth mips | Graph 中 camera consume/HZB chain/temporal pass |
| Material/ShaderVariant | Root catalog + shader/toolchain cache | Effect Shader ABI、完整 Layout/PSO variants、Hot params |
| Canvas2D/Grid/Gizmo/UI | Scene或View 的 Sprite/Tile/Line/PixelField数据 | 单张输出 Graph 内 2D/UI/Composite Pass；不创建 Mesh Instance |
| Terrain/Water/Trajectory | Scene/Root page/vertex/trajectory/feedback backing | 流式地形/水面/Overlay Pass 与显式 Dependency |
| PointCloud Simple/GPUDriven/LOD/Splat/Transient | Chunk/octree/point data，View-specific visible/sort buffers | 五种独立正确渲染算法，绝不把 soft splat 冒充 Full 3DGS |
| Robotics/Picking/Selection | Scene/Editor controlled data，View readback/Target semantic | Picking/Highlight/Overlay 的真实 GPU/读回路径 |

`LightFeature`、`StandardMeshStackFeature`、`StandardMaterialFeature` 在 Legacy 中确有“只创建资源、不产生 Pass”的实际行为；新实现把真实资源提供给 ResourceDomain 的协议或资源构造操作，**不需要**按这些旧类名复制一个空 `RenderFeature`。`TonemapFeature` 的 Hot Params 搬到 RenderEffect/执行参数，Shadow View-slice 产生的 per-view 数据搬到可验证的 View Context/domain，GPU Pass 仍由 Feature build。

## 12.3 F01–F36 逐组进场标准

| 分组 | IDs | 最低不可删业务行为 |
|---|---|---|
| Legacy Geometry A | F01–F03 | Mesh instance/transform/visibility/retire、Forward GPU-driven、Deferred GBuffer，实际 GPU draw output |
| Legacy Light & Material | F04–F07 | Clustered Light/Provider、Material Graph/Shader variants、Depth Prepass |
| Legacy Shadow & Visibility | F08–F14 | PCF/EVSM/CSM、MeshShadow、HZB、Spatial Cull、GPU Skinning、RenderCluster/GPU picking、Camera |
| Legacy Postprocessing | F15–F20 | Tonemap/exposure、SSAO、LinearDepth、Fog、Highlight、Skybox |
| Legacy Editor/2D | F21–F25 | Grid2D/3D、Gizmo line/tri、Canvas2D image/pixel-field/tile/grouping |
| Legacy Geo/Robotics | F26–F29 | Terrain streaming/LOD、Water、Trajectory、StreamingFeedback |
| Legacy PointCloud/Robotics | F30–F36 | Simple、GPUDriven、LOD、SoftSplat、Transient 五算法、GPU octree/point storage、Scene trajectory buffer |

原 `I01–I44` 在新文档中明确重分配：Graph Builder/Compiler/Sync/Transient→`graph/`、`backend/vulkan/graph/`；Shader/Toolchain→`data/graph`+现有工具链；Frame/Target/Runtime→`RenderBackend`、`Target`、`vulkan::Executor`；Asset/ECS projection→Engine RenderSystem+Domain；Plugin→`effect/`+ABI；先进的新需求 I31–44 继续分别进行 GPU oracle，不能只靠类名存在判 PASS。

## 12.4 每项迁移的证据合同

每条 F/I/H 须交付：原 Legacy SourceFile/operation/Shader 特性 → 新 Feature 构建行为/资源来源及所有权 → 中立 Graph Pass/IR access → Native Shader/Layout/PSO/Executor/Target 正确结果 → 真实 GPU 图像/数值参考、性能/VRAM、容量、resource ready/retirement、负例/GPU SyncValidation/ASan → Implementation I / Verification V/代码 SHA。没有业务消费者的 `DataSchema` Test 不代表该 Feature 已迁移。

## 12.5 不允许退化

- GPU-driven 的 Cull/Indirect/Bone/PointCloud 不得退回每帧 CPU 全量实例遍历；Shader 的属性和材料语义与旧画质指标不能偷降。
- Canvas2D/PointCloud/Terrain/Water/Trajectory 无需强制 Mesh Instance/Material，而它们的特殊操作、shader 变体与输出必须保留。
- Legacy 多窗口/Offscreen/Custom UI Composite 功能必须通过**声明在完整 Graph 内的 Target 输出和受限插件合成**恢复，不能另建 UI Renderer/隐形 Graph。
- 当前产品停留在 V2 核心测试阶段：即使 F6.b 200/200 Normal/ASan，不能声明业务 F01–F36 或先进 H 通过。
