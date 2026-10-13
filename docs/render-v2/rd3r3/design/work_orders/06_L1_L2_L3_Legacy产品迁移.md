# L1/L2/L3 — 恢复全部 Legacy 功能并完成产品消费

每个 L 小阶段单独授权、独立 I/V；不得以完成 R3 机制测试宣称 Legacy parity。

L1：F01–F03/F06/F07/F11–F14 MeshStack、Forward/GBuffer、Material、Depth、SpatialCull、GPU Skinning、RenderCluster/Picking、Camera；Root MeshStorage/Scene InstanceStorage 和 ECS MeshProjection 真数据流。保留 VertexPool、InstanceSlot、GPU cull/LOD、shader ABI、staging/retirement 算法。

L2：F04–F05/F08–F10/F15–F20 Light、Clustered/Deferred Lighting、PCF/EVSM/CSM Shadow、MeshShadow、HZB、Tonemap/SSAO/LinearDepth/Fog/Highlight/Skybox；Provider ScopeKey、Temporal history/Native Sync2 真输出。

L3：F21–F36 Editor Grid2D/3D、Gizmo line/tri、Canvas2D、Terrain/Water、Trajectory、StreamingFeedback、PointCloud Simple/GPUDriven/LOD/SoftSplat/Transient、GPU Point/Octree/Trajectory。五类 PointCloud legacy algorithm 原业务输出必须逐个证明；SoftSplat 不等于 Full 3DGS。

产品闭环：Editor/Player/Toolchain/SDK、Window/Offscreen、多 View/Scene/Target、Input/History/Resize/Device Lost/Stop、36 F/44 I 对应真实 GPU/Numeric/Pixel oracles 与性能/VRAM。任何已证明的功能差距按 PARTIAL 留痕，不删原 workload，用户另行裁决阶段合并/接纳。
