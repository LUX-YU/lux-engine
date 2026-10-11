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
