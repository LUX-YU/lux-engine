# 23 — Legacy 专业 GPU Storage：保留高价值算法，按新语义重写

> 来源 `render_legacy/**` 冻结 719 源文件、commit `a669409a289a6fa4092f21176397795b1cdb7f3e`。这些不是 P2-A 已实现的业务功能。执行方必须逐项读源文件、Shader ABI、真实消费者和旧基准；不允许只复制类名/每个 `.hpp`。

## 23.1 底层 GPU Buffer 与分页

| Legacy 源码 | 应继承的具体算法、数据结构 | 新落点及必须改的接口 |
|---|---|---|
| `.../gpu/memory/GPUBuffer.hpp` `TGpuBuffer` | CPU_TO_GPU/GPU_ONLY 条件 policy、可选 Host mirror、Dirty epochs、reused coalescing scratch、非 coherent Flush & Range barrier、按 FIF 增量 | 专业 Storage 内部的 `GpuBuffer` primitive；按 native Queue Ticket 生命周期而非全局 FIF serial，避免双 CPU authority |
| `.../gpu/memory/PagedGpuStream.hpp` | 稳定 page slots、Dirty page coalescing、`runs_scratch_` 跨帧复用 | Instance/Chunk Storage，Hot Dirty 同样 O(changed)；不要每个 Record 注册 Scope entry |
| `.../resources/mesh/StableRecordPages.hpp` | Mesh CPU metadata 的固定 4096-record pages、稳定引用/分块增长 | MeshStorage 的 data layout，不能改为普通 dense SlotMap value 导致裸引用随 erase 移动 |
| `.../gpu/memory/ArenaAllocator.hpp` | VMA virtual TLSF、free/coalescing、live range、defrag copy plan | MeshStorage 内子分配，业务 VRAM budget 与真实 submitted copy tickets 协调 |
| `.../gpu/memory/ChainedArenaAllocator.hpp` | segment 逐步扩展、优先最近/旧空洞回收、候选成功才发布、`uint16` index bounds | Mesh VBO/IBO segmented arena；Segment Native Buffer owner 由 Storage 持有，arena 本身不必 own VkBuffer |

路径 `...` 均以前缀 `render_legacy/modules/function/render/vulkan/sinclude/lux/engine/render/` 为准；精确 Path 见 `appendix/LegacyGpuAlgorithms.csv`。

## 23.2 Geometry/Instance/Vertex Pool

`MeshResources` 中 VBO/IBO segmented arenas、真实 Index format、LOD ranges、GPU ready、CPU bounding box/BVH、StagingOnly fallback 具有实际价值；迁移为 Session Global `MeshStorage`，一次创建供多个 Scene 引用。`InstanceResources` 中 stable `InstanceSlotRegistry`、Transform/PrevTransform/CullMeta/Property streams、dense alive slots、page-relative transform/world origin rebase、per-stream Dirty 保留；迁移为 Scene-scope `InstanceStorage`，多个 View/Effect 共享实例 backing 但独立 Cull/History/visibility results。

`VertexPoolRegistry` 的 bindless vertex source slot 与 registration lease semantics 需继续有效，但按真实 native ready/last-use 和 new ScopeResources 的 attach lease 分离。**GPU Shader 读取的 stable slot index 不必等于 lux-cxx SlotMap 的物理 dense index**；统一 Host ID 不能破坏 shader buffer layout。

## 23.3 Texture、Bindless、Material

`TextureResources` / `BindlessCombinedSet` 的 stable slot generation、GPU-in-flight recycle deferral、mip residency/feedback、persistent region updates、2D/Cube bindings、fallback texture、batch uploads 是一体的内存/正确性优化；Root `TextureStorage` 是唯一 backing，多个 Scene/Effect 可以拿 lease。`MaterialResources` 的 per-family GPU table、shader variant/texture bindless slot、FIF descriptor revision 也要保留，但应让 native BindingProvider 接入当前 generated Shader ABI/Owner Shape，不重新发明固定 set/binding 配置。DeviceCaps 不支持时选择合法的 bindless fallback 或明确不支持。

## 23.4 Light、Skinning、PointCloud、其他专业存储

- `LightResources`：Directional/Point/Spot/Area SSBO、GPU ABI、FIF update、位置页与 shadow data；迁移 Scene LightStorage，实际 data writer 由 Engine LightProjection 负责；别强制 LightDataProvider 成为含空 pass 的 Feature。
- `SkinningResources`：GPU bone palette/预 skin compute/Ready/LOD，禁用每 View CPU skinning 替换真正 GPU pipeline。
- `PointCloudResources` + `PointCloudGlobalBuffer`：Chunk slot capacity reuse、octree nodes、ordered `clear/upload/reset` queue、GPU-safe deferred reuse；专用 PointCloudStorage，不强制套 Mesh record；Legacy soft splat 不等于 H6 真实 3DGS。
- Terrain/Water/Trajectory/Canvas/UI/Gizmo：按实际 Feature/Shader Data 与专业 Streaming/Storage 机制迁移，保留原业务性能特性和 Target final composition 的输出。

## 23.5 Transfer/Retirement

Legacy `TransferScheduler` 的 staging ring/overflow FIF buffers、batch copies、reused barrier scratch、Host/GPU flush；`FifOwned` / DeferredDestroyQueue 保护 accepted GPU work 的本质都值得继承。但是 Native R4 已有 Queue Sync2、real SubmissionTicket 和 retirement，请在同一 Native owner 内融合优化算法，**不能再次引入平行 Transfer Worker 或第二套“FrameSerial 等于 completed”机制**。Buffer/Texture 切换旧 backing 时，CPU logical release 和 GPU last use 分离；Native resource 必须在可审计 ticket 状态中退休。

## 23.6 每项算法迁移证据卡

最少含：`LegacyPaths`、`OriginalDataLayout/ShaderABI`、`OwnerScope/ScopeKey`、`ResourceRequirement/Factory`、`AttachLeaseBinding`、`Transfer/Ready/LastUse`、`RetainedAlgorithmAndReason`、`NewAPIAndWhy`、`GPUPixelNumericOracle`、`Dirty/VRAM/CPU/GPU`、`Affected Legacy CapabilityId / GoldenId`、`ImplementationSHA/VerificationSHA`。只有技术证据说明更简单且效果/性能不退，才可改变成熟数据结构；不能以改名/换容器作为“性能优化”。


## 23.7 RenderGraph transient allocator 和 Material/UPLOAD 不是普通 Storage 的附注

旧 `RGVulkanResourceAllocator` 的 exact-key pool、transient image VMA pool、独立 LazyMemory pool 和跨 Graph 重编/Resize backing 重用必须在第 29/32 章独立验证；这类 Graph transient 不经 SceneScope Registry 的 `getOrCreate<MeshStorage>`。Material Graph 另有稳定 Node/Cook/ShaderIR 工具链、GraphFamily GPU params/texture 和 refcounted Shader Variant Bucket（第 30 章），不能只迁移一个 `MaterialResources` Buffer。Legacy `RenderUploadClient/Session`→bounded bytes UploadQueue→`GpuTransferPipeline` 的 dedicated/record-only 模式与 QFOT（第 31 章）是真实 Mesh/Texture 性能路线；只有吸收到 R4 唯一 Queue Ticket/Retirement 才能称迁移，不照抄第二套 Submission Owner。
