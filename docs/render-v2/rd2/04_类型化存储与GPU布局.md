# 04 — EnTT 风格 Typed Storage、StoragePlan、GPU 驻留与 Worksets

## 4.1 最小可复用算法，不是通用 ECS

CPU-only `TypedStorage<T>` 无需 GPU ABI，其合法性由 CPU 存储策略的生命周期约束决定；需要 GPU view 才应用可选 GPU Layout/Representation 合同。`TypedStorage<T>` 的 CPU 端复用数据结构算法，不拥有 FrameLoop、Feature 调度或 Vulkan Device。其状态针对 CPU-authoritative 记录：

```text
SlotTable: generation[slot], live[slot], sparse_to_dense[slot]
DenseAliveSlots: slot[]  (紧凑可遍历，仅用于 membership/candidate list)
Records: stable slot-indexed T[]  (可分页，不因 dense swap 移动)
Dirty: bitset + compact dirty list / merged ranges, revision numbers
FreeSlots/RetiredSlots: 可复用条件由 GPU completion/读者使用证明确定
```

图示是**一种 StoragePlan**，不是所有 T 机械采用的内存布局。EnTT 的 `sparse↔dense` 变换用于快速 membership/iteration；数据记录在 Shader 必须保持 Slot 稳定时可采用分离的 stable slot-indexed 页。DenseAliveSlots 删除时 swap-and-pop，只修其本身映射，记录并不迁移。部分高吞吐领域可以选择 record dense+indirection 或单独 GPU compact output，但必须在合同里说明物理位置变化、引用修正和事务。

## 4.2 稳定 Identity vs GPU 地址

- `RecordHandle<T>` 包含 DataSet owner、slot、generation；不是裸 `uint32` 跨 Frame/Scene 可复用身份。
- CPU 热查询可用 sparse→dense/valid；无业务副作用的成员移除平均 O(1)。generation wrap 必须明确最终退休该 slot 或使用更宽值，不重用导致旧句柄复活。
- Shader Normal Path **不检查字符串、类型 ID 或 CPU Generation**。预检的 `VisibleSlots` 等 Workset 存 `uint32_t` slot，shader 直接 `data[slot]`。如 GPU 长期持有跨帧悬空引用，则应专门生成 `generation` 校验或引用重新映射，且必须计入性能。
- 一个物理 backing 可能被多 Frame 读；Record erase 在 CPU 语义上立即生效，但 slot 字节的重新写入/复用需等到 GPU 最后使用后或采用不同地址的 Copy-on-Write。CPU dense indices 的写入与旧 GPU 使用也同样需要同步/版本保护。

## 4.3 `StoragePlan` 的冷态确定流程

下述 Graph/Native 路径只适用于需要 GPU 表示的集合；纯 CPU-only DataSet 只选择合法 CPU 策略，不制造 GPU shape/backing 或伪 Pass。

```text
DataSchema<T> + DataSet requested scope/count/budget + optional GPU profile/Caps
             ↓ derive **logical StorageInterfaceShape** (GPU ABI/format,
               addressability/capacity guarantees) WITHOUT allocating VkBuffer
Cold Feature Composition resolves DataSet/Capability requirements
             ↓ typed logical views contribute PassSchema/Graph declarations
RenderGraph logical C0–C8 (no DataSet registry lookup)
             ↓ compile NativeStoragePlan from real uses WITHOUT changing
               that already-frozen logical Shader/Graph interface
StoragePlan {logical interface shape, CPU strategy, GPU physical recipe,
             update, descriptor binding compatibility, retirement policy}
             ↓ native objects & opaque typed per-invocation views
NativeStorageBacking + TypedStorage<T> / GPU-owned state
```

**避免循环依赖**：Feature 需要先在 Graph 声明数据读写，才能决定 Native 物理 usage；而 Graph Authoring 又必须提前知道资源的合法逻辑 Byte Range/Texture Shape。因此先从 Schema/请求固定中立的 `StorageInterfaceShape` 并提供 typed **逻辑** DataView；Graph C0–C8 计算实际访问后，再在 C10 选择相容 Native 物理方案和分配 Backing；C9/C11/C12/C13 保持既有 Layout/执行配方/同步/装配职责。若请求的 SoA/Atlas/BDA 模式会改变 Shader 可见 ABI，必须在固定 Interface Shape 前通过明确已生成的变体选择，不允许物理阶段偷偷改写 C++/GLSL 结构。这是同一 `StoragePlan` 冷态流程的两个阶段，不是再建一个长期 StorageManager。

不能承诺最优策略而不提供成本数据。默认策略：

| 数据工作量 | CPU storage | Native GPU storage | Shader 工作 |
|---|---|---|---|
| Sparse CPU updates、stable slot | Sparse index + slot records + dirty | 稳定 Slot Buffer + active indices，可分页扩容 | 按 slot gather；必要 GPU compact Workset |
| 主要线性扫描、可移动 | 稠密 records + generation map | contiguous Buffer，可控 relocation | 连续数组 |
| 大 Mesh/Texture/PointCloud/Gaussian | Chunk/Page/Free range/asset ledger | Arena、分段 Buffer 或 Image Pool；按预算驻留 | chunk ranges, specialized cull/LOD |
| GPU-authored Compute/Skinning/Particle | CPU 仅 metadata/控制 | GPU Buffer + versioned/ping-pong/indirect | graph producer/consumer，不 CPU 每帧 mirror |
| View history, HZB/TAA | View 数据集的 history validity | View-scoped Images/Versioned aliases | 每 View 独立 Graph imports/read/writes |
| 本帧 scratch | 无 DataSet 必需 | Graph transient Buffer/Image + verified alias | Pass-local scratch |

不能将 `Image` 强行装入 `StorageBuffer<T>`，也不将大 Mesh Blob 拆成每 Vertex 的 Sparse Set。`DataSchema` 的结构定义与 `ImageDescription` 的图像格式/extent 语义是不同层；通过受限 Storage Profile 连接。

## 4.4 Dirty Upload / Readyness / Backpressure

`patch(handle, f)` 仅在合法 writer 队列上修改逻辑状态，将必要 slot/range 标 dirty，并增加 observed revision。发布分 `observed` / `accepted(published)` / `applied` / `GPU ready`；失败（Staging 满、Queue 容量、跨 Lane 乱序）**不清除脏状态**。同个 Record 多次连续更新允许在未 admission 时合并到最终值，但 create/remove/generation/对外副作用不得遗漏。

更新策略：低 D 时 merge contiguous byte ranges；高度离散更新可批量上传 packed `{slot,value}` 并 GPU scatter；D/N 高时 full range replace。所有阈值按设备与负载实测后确定，不能默认“Scatter 总比小 Copy 快”。`ActiveSlots` 列表修补也必须有前一帧 GPU read-overlap 的 Ready/Retire 保证。

**重要**：Persistent GPU 数据并不等于全局 Triple Buffer。对高更新数据按 fence/usage 采用 per-slot 版本、ring 或 Copy-on-Write；只读/未修改数据持久共享一个 backing。写入 GPU 的数据不能被 CPU dirty 镜像再次覆盖。

## 4.5 同类型多存储 / GPU Worksets

一 Scene 可以有 `Light.main`、`Light.preview` 两份同 T DataSet；多个 Scene 可以独立持有并复用共享 Material Asset backing。一个 `MeshDraw` T 不应成为 Universal Instance。Sprite2D、PointCloudChunk、GaussianChunk 不必绑定 Transform3D/MeshMaterial；各自声明真实字段和关联。

GPU 工作列表按**表示或 Pipeline/材质兼容性**聚合，避免在单 Shader workgroup 里对每记录动态 `switch(GeometryKind)`。`Cull hot data` 可以抽出紧凑 bounds/LOD/slot 列，而存储主表维持稳定 Handle。无必要不强制 SoA；每条额外 GPU 内存引用都要评估 Cache/Miss。透明 Canvas 按稳定 Layer/Blend 顺序处理，原子 Append 不能自动保序。

示例访问链：`gl_InstanceIndex → VisibleDrawSlots[instance] → MeshDrawRecords[slot] → geometry/material indices`。若 gather 占主要 GPU 时间，图内构造 CompactVisibleRecord 以减少多重间接；禁止借可扩展性引入 TypeId/ServiceLocator per-shader lookup。

## 4.6 用模板争取 EnTT 级作者体验

```cpp
// 目标语义 API：不是当前源码承诺，拼写需通过实际 C++20 编译/SDK 检查。
auto lights = scene.data().write<LightGpu>("main");
auto id = lights.emplace(LightGpu{...});
lights.patch(id, [](LightGpu& value) noexcept { value.color_intensity[3] = 3.5f; });
lights.erase(id);

auto r = scene.data().read<LightGpu>("main");
r.each([](RecordHandle<LightGpu> id, const LightGpu& value) noexcept { /* CPU only */ });

// 对 GPU-authority type，CPU get/patch 不提供，编译负例必须拒绝。
```

`patch` 是受限同步 callable，内部捕捉 dirty 与 revision；不返回长寿 `T&` 让作者绕过脏标记。`each` 是否暴露给一般 Feature 应以真实 CPU 消费者为前提；GPU 大规模 Cull 不允许每帧 CPU `each()` 构造 Visible Set。访问权限由 cold DataSet binding 固化，在热路径没有字符串/反射查询。写 API 不跨 Engine 与 Backend 线程，其他线程通过现有 typed transport 提交变化。

模板分层：public `DataSchema<T>`, `RecordHandle<T>`, `Reader<T>`, `Writer<T>` 和薄 `TypedStorage<T>`；底层通用稀疏 Slot/Dirty 合并/Page/retirement 作为**少数不依赖 T 的算法**，减少模板实例化和 ABI 膨胀。`std::pmr`/SOA/FreeList 优化按 profile 证据，不能先造 GenericAllocator/Manager。

## 4.7 真实内存生命周期

GPU data = {Dataset identity, active revision, native backing generation, bound resource/descriptor revision, last GPU use}. Delete/resize/detach 时：禁止新引用 → 准备完整候选 storage/descriptor → 原子 publish → 在途 Frame 继续 pin 旧 backing → 等 completion ticket → 回收 slot/arena/descriptor → 保留缓存容量或释放。Device lost 独立错误处理。不同 Owner 的 native allocation alias 必须比较实际 memory range，不能只比较 VkBuffer/VkImage handle。

**不得把 VMA 显存字节节省、CPU DenseAlive 容量和 GPU Pixel 算法吞吐误写成同一个“存储优化指标”。**
