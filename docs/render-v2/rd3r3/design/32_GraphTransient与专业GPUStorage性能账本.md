# 32 — Legacy GPU Storage/Graph 物理规划性能账本与实施约束

## 32.1 两种池与四种生命周期必须区别

**不要将** `ResourcePlan` 中的 Scoped/Global/Scene/View/Attachment **等同于** Native Graph transient 的 GPU backing pool。前者表达专业资源被多个 Effect 共享的逻辑身份；后者表达 GPU 短生命周期 Buffer/Image 的别名与跨编译候选缓存。两者都是优化，解决的问题不同。

| 类别 | 优化源头 | 保留的特性 | 重要负例 |
|---|---|---|---|
| Resource-scope 专业存储 | `MeshResources` `InstanceResources` `TextureResources` `LightResources` `MaterialResources` | 稳定业务槽、结构化 GPU 数据、增量上传、跨 Effect 复用 | 别把每条 Mesh/Light 当 transient 重新分配 |
| Graph transient alias | `RenderGraphCompiler` / `PhysicalResourceAllocator` | 生命周期不重叠+内存兼容复用物理内存；Load/Store & precise sync | 输出/History 被 alias 写覆盖或 alias hazard 不同步 |
| Graph backing pool | `RGVulkanResourceAllocator` | exact TransientResourceKey、VMA image pools、LazyMemory capability、recompile/resize 资源池+GC | 别只按图片尺寸判断可复用，也别复用 GPU-inflight backing |
| Temporal / persistent | `RGBuilder` ping-pong/persistent，`RGGraphResize` | per-View/Attachment Epoch、FIF-independent current/previous、Resize 原子 swap | `frameIndex % FIF` 不等于所有 History 窗口都兼容 |

## 32.2 已审的代表性成熟优化

- `TGpuBuffer` 的 CPU_TO_GPU/GPU_ONLY 策略、HostMirror optional、epoch Dirty、reused `hm_upload_scratch_`、coalesced runs、non-coherent flush、FIF slice 与 GPU-only reserve migration。
- `TPagedGpuStream` Dirty-page runs 的 reused scratch，`TStableRecordPages` 4096-record 数据地址稳定；Instance 三种主流数据流及 GPU live/dynamic worklists，避免 CPU 每帧全量构建重映射。
- `ArenaAllocator` 的 VMA Virtual TLSF/free/coalesce/defrag plan，`ChainedArenaAllocator` 的 segment growth/returned freed blocks；`MeshResources` VBO+IBO segment 与 LOD/mesh GPU ready。
- `BindlessCombinedSet` 的 Generation/Slot deferred recycle、per-FIF safe replacement、texture mip feedback/requested residency、region/cube upload 与 staging chunk；`MaterialResources` 的 per-family SlicedSSBO+GraphFamily、bucket PSO 重用、parameter dirty revision。
- `PointCloudGlobalBuffer` 的 chunk slot growth/reset/clear/use-after-free protection，`TransferScheduler` 的 ring/overflow staging 和 batching；GPU stream read/write 的 Native owner 与 CPU source authority 明确分离。
- `RGVulkanResourceAllocator` 的 transient VMA pool + LAZILY_ALLOCATED 专用池，没 lazy memory 时仍用 ordinary pool；Pool cache 的成本/VRAM 上限和 LastUse 必须可观察。

## 32.3 为什么不能把这些压成 UniversalGpuStorage<T>

多个存储的真实 Shader ABI 不同：Mesh 多段 VBO/IBO/LOD、Instance stable BDA 页表/indirection、Material graph params/texture slots/PSO buckets、Texture bindless image/descriptor/mips、PointCloud chunk & octree。统一对外身份/创建/获取/绑定合同是正确的；统一所有内部 `upload(bytes)`、slot recycler、allocator 和 ready process 会丢掉优化甚至改变 GPU 数据布局。

建议的共通接口只在 Asset/Scope 层：`getOrCreate<T>()`（冷态、配置兼容）、tryGet、`ResourceRequirement/Ready/LastUse/BindingProvider`（结构与引用）以及纯 Native `Buffer/Image` 原语。具体业务保持 `updateTransform()`、`replaceMipRange()`、`uploadChunk()`、`updateMaterialParameters()` 等 typed operations；C++20 concept 约束 Factory/Binding outputs 而不要求热态 Storage 继承虚基类。

## 32.4 保留/优化/淘汰的客观准则

每类 Legacy 机制做 `Source → current R4/modern equivalent → retained algorithm → explicit adaptation → validation → benchmark evidence` 六列。没有真正差异和收益则复用现有实现或最小整合；改算法必须重跑数值、VRAM、dirty bytes、CPU/gpu timing 和 allocation，注明平台/设备和路径。对未验证的设备性能不得声称“更快”。Hash-based authoring/toolchain temporary lookup 不等于运行时对象 ID；新所有权身份仍必须通过 lux-cxx `SlotKey`，冷态 cache 的全结构相等才是正确性依据。

本章具体原始接口参看第 23/29/30/31 章；逐项测试索引在 `LegacyCompilerMechanisms.csv`、`MaterialGraphContracts.csv`、`UploadTransportCases.csv` 与旧 `LegacyGpuAlgorithms.csv`。
